/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

#include <list>
#include <cmath>
#include <tuple>
#include <cstdio>
#include <string>
#include <thread>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <filesystem>

#include <unistd.h>

#include <winpr/crt.h>
#include <winpr/ssl.h>
#include <winpr/input.h>
#include <winpr/wtsapi.h>
#include <winpr/version.h>

#include <freerdp/freerdp.h>
#include <freerdp/version.h>
#include <freerdp/constants.h>
#include <freerdp/codec/planar.h>
#include <freerdp/codec/interleaved.h>
#include <freerdp/codec/region.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/channels.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/gdi/gdi.h>

#include "ltsm_tools.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_connector_rdp.h"

using namespace std::chrono_literals;

#define FREERDP_VERSION_NUMBER ((FREERDP_VERSION_MAJOR << 16) | (FREERDP_VERSION_MINOR << 8) | FREERDP_VERSION_REVISION)

namespace LTSM::Connector {
    void stream_free(wStream* st) {
        Stream_Free(st, TRUE);
    }

    struct ClientContext {
        int test = 0;
    };

    struct ServerContext : rdpContext {
        BITMAP_PLANAR_CONTEXT* planar = nullptr;
        BITMAP_INTERLEAVED_CONTEXT* interleaved = nullptr;
        HANDLE vcm = nullptr;
        CliprdrServerContext* cliprdr = nullptr;

        bool activated = false;
        bool clipboard = false;
        size_t frameId = 0;

        const JsonObject* config = nullptr;
        ConnectorRdp* conrdp = nullptr;
        std::unique_ptr<JsonObject> keymap;
    };

    int ServerContextNew(rdp_freerdp_peer* peer, ServerContext* context) {
        context->planar = nullptr;
        context->interleaved = nullptr;
        context->vcm = WTSOpenServerA((LPSTR) peer->context);

        if(! context->vcm || context->vcm == INVALID_HANDLE_VALUE) {
            Application::error("%s: failed", "WTSOpenServer");
            return FALSE;
        }

        context->cliprdr = nullptr;
        context->activated = false;
        context->clipboard = true;
        context->frameId = 0;
        context->config = nullptr;
        context->conrdp = nullptr;
        context->keymap.reset();
        Application::info("%s: success", __FUNCTION__);
        return TRUE;
    }

    void ServerContextFree(rdp_freerdp_peer* peer, ServerContext* context) {
        if(context->planar) {
            freerdp_bitmap_planar_context_free(context->planar);
            context->planar = nullptr;
        }

        if(context->interleaved) {
            bitmap_interleaved_context_free(context->interleaved);
            context->interleaved = nullptr;
        }

        if(context->vcm) {
        }

        if(context->vcm) {
            WTSCloseServer(context->vcm);
            context->vcm = nullptr;
        }

        if(context->keymap) {
            context->keymap.reset();
        }
    }

    // FreeRdp
    struct FreeRdpCallback {
        freerdp_peer* peer;
        ServerContext* context;
        std::atomic<HANDLE> stopEvent;

        FreeRdpCallback(int fd, const std::string & remoteaddr, const JsonObject & config,
                        ConnectorRdp* connector) : peer(nullptr), context(nullptr) {
            Application::info("freerdp version usage: %s, winpr: %s", FREERDP_VERSION_FULL, WINPR_VERSION_FULL);
            winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
            WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
            // init freerdp log system
            auto log = WLog_GetRoot();

            if(log) {
                WLog_SetLogAppenderType(log, WLOG_APPENDER_SYSLOG);
                auto str = Tools::lower(config.getString("rdp:wlog:level"));
                int type = WLOG_ERROR;

                if(str == "trace") {
                    type = WLOG_TRACE;
                } else if(str == "debug") {
                    type = WLOG_DEBUG;
                } else if(str == "info") {
                    type = WLOG_INFO;
                } else if(str == "warn") {
                    type = WLOG_WARN;
                } else if(str == "error") {
                    type = WLOG_ERROR;
                } else if(str == "fatal") {
                    type = WLOG_FATAL;
                } else if(str == "off") {
                    type = WLOG_OFF;
                }

                WLog_SetLogLevel(log, type);
            }

            peer = freerdp_peer_new(fd);
            peer->local = TRUE;
            std::copy_n(remoteaddr.begin(), std::min(sizeof(peer->hostname), remoteaddr.size()), peer->hostname);
            stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            // init context
            peer->ContextSize = sizeof(ServerContext);
            peer->ContextNew = (psPeerContextNew) ServerContextNew;
            peer->ContextFree = (psPeerContextFree) ServerContextFree;

            if(! freerdp_peer_context_new(peer)) {
                Application::error("%s: failed", "freerdp_peer_context_new");
                throw rdp_error(NS_FuncName);
            }

            Application::debug(DebugType::App, "peer context: %p", peer);
            Application::debug(DebugType::App, "rdp context: %p", peer->context);
            context = static_cast<ServerContext*>(peer->context);
            context->config = & config;
            context->conrdp = connector;
            context->clipboard = false;

            if(auto keymapFile = config.getString("rdp:keymap:file"); ! keymapFile.empty()) {
                JsonContentFile jc(keymapFile);

                if(jc.isValid() && jc.isObject()) {
                    context->keymap = std::make_unique<JsonObject>(jc.toObject());
                    Application::info("keymap loaded: %s, items: %lu", keymapFile.c_str(), context->keymap->size());
                }
            }

            auto certfile = connector->checkFileOption("rdp:server:certfile");

            if(certfile.size()) {
                peer->settings->CertificateFile = strdup(certfile.c_str());
                Application::info("server cert: %s", peer->settings->CertificateFile);
            }

            auto keyfile = connector->checkFileOption("rdp:server:keyfile");

            if(keyfile.size()) {
                peer->settings->PrivateKeyFile = strdup(keyfile.c_str());
                peer->settings->RdpKeyFile = strdup(keyfile.c_str());
                Application::info("server key: %s", peer->settings->RdpKeyFile);
            }

            int encryptionLevel = ENCRYPTION_LEVEL_NONE;
            auto paramencryptionLevel = Tools::lower(config.getString("rdp:encription:level", "compatible"));

            if(paramencryptionLevel == "high") {
                encryptionLevel = ENCRYPTION_LEVEL_HIGH;
            } else if(paramencryptionLevel == "low") {
                encryptionLevel = ENCRYPTION_LEVEL_LOW;
            } else if(paramencryptionLevel == "fips") {
                encryptionLevel = ENCRYPTION_LEVEL_FIPS;
            } else {
                encryptionLevel = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;
            }

            peer->settings->RdpSecurity = config.getBoolean("rdp:security:rdp", true) ? TRUE : FALSE;
            peer->settings->TlsSecurity = config.getBoolean("rdp:security:tls", true) ? TRUE : FALSE;
            peer->settings->NlaSecurity = config.getBoolean("rdp:security:nla", false) ? TRUE : FALSE;
            peer->settings->TlsSecLevel = config.getInteger("rdp:tls:level", 1);
            peer->settings->ExtSecurity = FALSE;
            peer->settings->UseRdpSecurityLayer = FALSE;
            peer->settings->EncryptionLevel = encryptionLevel;
            peer->settings->NSCodec = FALSE;
            peer->settings->RemoteFxCodec = FALSE;
            peer->settings->RefreshRect = TRUE;
            peer->settings->SuppressOutput = TRUE;
            peer->settings->FrameMarkerCommandEnabled = TRUE;
            peer->settings->SurfaceFrameMarkerEnabled = TRUE;
            peer->PostConnect = ConnectorRdp::cbServerPostConnect;
            peer->Activate = ConnectorRdp::cbServerActivate;
            peer->Close = ConnectorRdp::cbServerClose;
            peer->Disconnect = ConnectorRdp::cbServerDisconnect;
            peer->Capabilities = ConnectorRdp::cbServerCapabilities;
            peer->AdjustMonitorsLayout = ConnectorRdp::cbServerAdjustMonitorsLayout;
            peer->ClientCapabilities = ConnectorRdp::cbServerClientCapabilities;
            peer->input->KeyboardEvent = ConnectorRdp::cbServerKeyboardEvent;
            peer->input->MouseEvent = ConnectorRdp::cbServerMouseEvent;
            peer->update->RefreshRect = ConnectorRdp::cbServerRefreshRect;
            peer->update->SuppressOutput = ConnectorRdp::cbServerSuppressOutput;

            if(1 != peer->Initialize(peer)) {
                Application::error("%s: failed", "peer->Initialize");
                throw rdp_error(NS_FuncName);
            }
        }

        ~FreeRdpCallback() {
            if(stopEvent) {
                CloseHandle(stopEvent);
            }

            if(peer) {
                freerdp_peer_context_free(peer);
                freerdp_peer_free(peer);
            }
        }

        void stopEventLoop(void) {
            if(stopEvent) {
                Application::info("%s: stop event", "FreeRdpCallback");
                SetEvent(stopEvent);
            }
        }

        bool isShutdown(void) const {
            return ! stopEvent;
        }

        static bool enterEventLoop(FreeRdpCallback* rdp) {
            Application::info("%s: enter event loop", "FreeRdpCallback");
            freerdp_peer* peer = rdp->peer;
            ServerContext* context = rdp->context;
            ConnectorRdp* conrdp = context->conrdp;

            // freerdp client events
            while(true) {
                if(rdp->isShutdown()) {
                    break;
                }

                if(peer->CheckFileDescriptor(peer) != TRUE) {
                    break;
                }

                if(WTSVirtualChannelManagerCheckFileDescriptor(context->vcm) != TRUE) {
                    break;
                }

                if(WaitForSingleObject(rdp->stopEvent, 1) == WAIT_OBJECT_0) {
                    break;
                }

                conrdp->checkIdleTimeout();

                // wait
                std::this_thread::sleep_for(1ms);
            }

            if(rdp->stopEvent) {
                // for shutdown flag
                CloseHandle(rdp->stopEvent);
                rdp->stopEvent = nullptr;
            }

            peer->Disconnect(peer);
            Application::info("%s: loop shutdown", "FreeRdpCallback");
            return true;
        }
    };

    ConnectorRdp::ConnectorRdp(const std::filesystem::path & confile, bool debug) : DBusProxy(ConnectorType::RDP, confile, debug) {
    }

    ConnectorRdp::~ConnectorRdp() {
        if(0 < displayNum()) {
            busConnectorTerminated(displayNum(), getpid());
            disconnectedEvent();
        }
    }

    int ConnectorRdp::communication(void) {
        if(0 >= busGetServiceVersion()) {
            Application::error("%s: failed", "bus service");
            return EXIT_FAILURE;
        }

        auto home = LTSM::Connector::homeRuntime();
        const auto socketFile = std::filesystem::path(home) / std::string("rdp_pid").append(std::to_string(getpid()));

        if(! proxyInitUnixSockets(socketFile)) {
            return EXIT_FAILURE;
        }

        Application::info("%s: remote addr: %s", __FUNCTION__, _remoteaddr.c_str());
        proxyStartEventLoop();
        // create FreeRdpCallback
        Application::info("%s: %s", __FUNCTION__, "create freerdp context");
        freeRdp = std::make_unique<FreeRdpCallback>(proxyClientSocket(), _remoteaddr, config(), this);
        auto freeRdpThread = std::thread([ptr = freeRdp.get()] { FreeRdpCallback::enterEventLoop(ptr); });
        damageRegion.assign(0, 0, 0, 0);
        // rdp session not activated trigger
        auto timerNotActivated = Tools::BaseTimer::create<std::chrono::seconds>(30, false, [this]() {
            if(this->freeRdp && this->freeRdp->context && ! this->freeRdp->context->activated) {
                Application::error("session timeout trigger: %s", "not activated");
                this->loopShutdownFlag = true;
            }
        });

        bool nodamage = config().getBoolean("rdp:xcb:nodamage", false);

        // all ok
        while(! loopShutdownFlag) {
            if(freeRdp->isShutdown() || ! proxyRunning()) {
                loopShutdownFlag = true;
            }

            if(xcbAllowMessages()) {
                if(auto err = XCB::RootDisplay::hasError()) {
                    xcbDisableMessages(true);
                    Application::error("xcb display error connection: %d", err);
                    break;
                }

                // xcb processing
                if(! xcbEventLoopAsync(nodamage)) {
                    loopShutdownFlag = true;
                }
            }

            // wait
            std::this_thread::sleep_for(1ms);
        }

        proxyShutdown();
        freeRdp->stopEventLoop();
        channelsFree();
        timerNotActivated->stop();

        if(freeRdpThread.joinable()) {
            freeRdpThread.join();
        }

        return EXIT_SUCCESS;
    }

    void ConnectorRdp::xcbDamageNotifyEvent(const xcb_rectangle_t & rt) {
        damageRegion.join(rt.x, rt.y, rt.width, rt.height);
    }

    void ConnectorRdp::xcbRandrScreenChangedEvent(const XCB::Size & dsz, const xcb_randr_notify_event_t & ne) {
        damageRegion.reset();
        busDisplayResized(displayNum(), dsz.width, dsz.height);
        desktopResizeEvent(*freeRdp->peer, dsz.width, dsz.height);
    }

    void ConnectorRdp::xcbXkbGroupChangedEvent(int) {
    }

    bool ConnectorRdp::xcbEventLoopAsync(bool nodamage) {
        // processing xcb events
        while(auto ev = XCB::RootDisplay::pollEvent()) {
            if(auto err = XCB::RootDisplay::hasError()) {
                Application::error("%s: xcb error, code: %d", __FUNCTION__, err);
                return false;
            }
        }

        if(nodamage) {
            damageRegion = XCB::RootDisplay::region();
        } else if(! damageRegion.empty()) {
            // fix out of screen
            damageRegion = XCB::RootDisplay::region().intersected(damageRegion.align(4));
        }

        if(! damageRegion.empty() && freeRdp->context && freeRdp->context->activated) {
            updatePartFlag = true;

            try {
                if(updateEvent(damageRegion)) {
                    XCB::RootDisplay::rootDamageSubtrack(damageRegion);
                    damageRegion.reset();
                }
            } catch(const std::exception & err) {
                Application::error("xcb exception: %s", err.what());
                return false;
            }

            updatePartFlag = false;
        }

        return true;
    }

    void ConnectorRdp::setEncryptionInfo(const std::string & info) {
        busSetEncryptionInfo(displayNum(), info);
    }

    void ConnectorRdp::setAutoLogin(const std::string & login, const std::string & pass) {
        helperSetSessionLoginPassword(displayNum(), login, pass, false);
    }

    bool ConnectorRdp::createX11Session(uint8_t depth) {
        int screen = busStartLoginSession(getpid(), depth, _remoteaddr, "rdp");

        if(screen <= 0) {
            Application::error("%s", "login session request failure");
            return false;
        }

        Application::debug(DebugType::App, "login session request success, display: %d", screen);

        if(! xcbConnect(screen, *this)) {
            Application::error("%s", "xcb connect failed");
            return false;
        }

        const xcb_visualtype_t* visual = XCB::RootDisplay::visual();

        if(! visual) {
            Application::error("%s", "xcb visual empty");
            return false;
        }

        Application::info("%s: xcb max request: %lu", __FUNCTION__, XCB::RootDisplay::getMaxRequest());
        // init server format
        serverFormat = PixelFormat(XCB::RootDisplay::bitsPerPixel(),
                                   visual->red_mask, visual->green_mask, visual->blue_mask, 0);

        // wait widget started signal(onHelperWidgetStarted), 3000ms, 10 ms pause
        bool waitHelperStarted = Tools::waitCallable<std::chrono::milliseconds>(3000, 100, [this]() {
            return ! ! this->helperStartedFlag;
        });

        if(! waitHelperStarted) {
            Application::error("connector starting: %s", "something went wrong...");
            return false;
        }

        std::this_thread::sleep_for(50ms);
        return true;
    }

    void ConnectorRdp::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t & userUid) {
        if(display != displayNum()) {
            return;
        }

        // disable xcb messages processing
        xcbDisableMessages(true);

        // wait client update canceled, 1000ms, 10 ms pause
        if(updatePartFlag) {
            Tools::waitCallable<std::chrono::milliseconds>(1000, 100, [this]() {
                return ! this->updatePartFlag;
            });
        }

        Application::notice("%s: dbus signal, display: %" PRId32 ", username: %s", __FUNCTION__, display, userName.c_str());
        int oldDisplay = displayNum();
        int newDisplay = busStartUserSession(oldDisplay, getpid(), userName, _remoteaddr, connectorType());

        if(newDisplay < 0) {
            Application::error("%s: %s failed", __FUNCTION__, "user session request");
            throw rdp_error(NS_FuncName);
        }

        if(newDisplay != oldDisplay) {
            // wait xcb old operations ended
            std::this_thread::sleep_for(100ms);

            if(! xcbConnect(newDisplay, *this)) {
                Application::error("%s: %s failed", __FUNCTION__, "xcb connect");
                throw rdp_error(NS_FuncName);
            }

            busShutdownDisplay(oldDisplay);
        }

        // update context
        xcbDisableMessages(false);
        // fix new session size
        auto wsz = XCB::RootDisplay::size();

        if(wsz.width != freeRdp->peer->settings->DesktopWidth || wsz.height != freeRdp->peer->settings->DesktopHeight) {
            Application::warning("%s: remote request desktop size [%dx%d], display: %d", __FUNCTION__,
                                 freeRdp->peer->settings->DesktopWidth, freeRdp->peer->settings->DesktopHeight, displayNum());

            if(XCB::RootDisplay::setRandrScreenSize(XCB::Size(freeRdp->peer->settings->DesktopWidth,
                                                    freeRdp->peer->settings->DesktopHeight))) {
                wsz = XCB::RootDisplay::size();
                Application::info("change session size [%" PRIu16 ", %" PRIu16 "], display: %d", wsz.width, wsz.height, displayNum());
            }
        } else {
            // full update
            serverScreenUpdateRequest(XCB::RootDisplay::region());
        }

        busConnectorConnected(newDisplay, getpid());

        Application::info("dbus signal: login success, display: %d, username: %s", displayNum(), userName.c_str());
    }

    void ConnectorRdp::onShutdownConnector(const int32_t & display) {
        if(display == displayNum()) {
            freeRdp->stopEventLoop();
            xcbDisableMessages(true);
            loopShutdownFlag = true;
            Application::info("dbus signal: shutdown connector, display: %" PRId32, display);
        }
    }

    void ConnectorRdp::onSendBellSignal(const int32_t & display) {
        if(display == displayNum() &&
           freeRdp && freeRdp->peer && freeRdp->peer->settings && freeRdp->peer->settings->SoundBeepsEnabled) {
            // FIXME beep
        }
    }

    void ConnectorRdp::onHelperWidgetStarted(const int32_t & display) {
        if(display == displayNum()) {
            helperStartedFlag = true;
            Application::info("dbus signal: helper started, display: %" PRId32, display);
        }
    }

    void ConnectorRdp::serverScreenUpdateRequest(const XCB::Region & reg) {
        if(xcbAllowMessages()) {
            XCB::RootDisplay::rootDamageAddRegion(reg);
        }
    }

    // client events
    void ConnectorRdp::disconnectedEvent(void) {
        Application::warning("RDP disconnected, display: %d", displayNum());
    }

    void ConnectorRdp::desktopResizeEvent(freerdp_peer & peer, uint16_t width, uint16_t height) {
        Application::info("%s: size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, width, height);
        auto context = static_cast<ServerContext*>(peer.context);
        context->activated = false;
        peer.settings->DesktopWidth = width;
        peer.settings->DesktopHeight = height;

        if(peer.update->DesktopResize(peer.update->context)) {
            Application::error("%s: [%" PRIu16 ", %" PRIu16 "] failed", __FUNCTION__, width, height);
        }
    }

    bool ConnectorRdp::updateEvent(const XCB::Region & reg) {
        //auto context = static_cast<ServerContext*>(freeRdp->peer->context);
        auto reply = XCB::RootDisplay::copyRootImageRegion(reg);
        // reply info dump
        Application::debug(DebugType::App, "%s: request size: [%" PRIu16 ", %" PRIu16 "], reply length: %lu, bits per pixel: %" PRIu8
                           ", red: %08" PRIx32 ", green: %08" PRIx32 ", blue: %08" PRIx32,
                           __FUNCTION__, reg.width, reg.height, reply->size(), reply->bitsPerPixel(), reply->rmask, reply->gmask, reply->bmask);
        FrameBuffer frameBuffer(reply->data(), reg, serverFormat);
        // apply render primitives
        renderPrimitivesToFB(frameBuffer);
        return 24 == reply->bitsPerPixel() || 32 == reply->bitsPerPixel() ?
               updateBitmapPlanar(reg, reply) : updateBitmapInterleaved(reg, reply);
    }

    bool ConnectorRdp::updateBitmapPlanar(const XCB::Region & reg, const XCB::PixmapInfoReply & reply) {
        auto context = static_cast<ServerContext*>(freeRdp->peer->context);
        const size_t scanLineBytes = reg.width * reply->bytePerPixel();
        const size_t tileSize = 64;
        const size_t pixelFormat = freeRdp->peer->settings->OsMajorType == 6 ? PIXEL_FORMAT_RGBX32 : PIXEL_FORMAT_BGRX32;

        if(reply->size() != reg.height * reg.width * reply->bytePerPixel()) {
            Application::error("%s: %s failed, length: %lu, size: [%" PRIu16 ", %" PRIu16 "], bpp: %" PRIu8, __FUNCTION__,
                               "align region", reply->size(), reg.height, reg.width, reply->bytePerPixel());
            throw rdp_error(NS_FuncName);
        }

        // planar activate
        if(! context->planar) {
            DWORD planarFlags = PLANAR_FORMAT_HEADER_RLE;

            if(freeRdp->peer->settings->DrawAllowSkipAlpha) {
                planarFlags |= PLANAR_FORMAT_HEADER_NA;
            }

            context->planar = freerdp_bitmap_planar_context_new(planarFlags, tileSize, tileSize);

            if(! context->planar) {
                Application::error("%s: %s failed", __FUNCTION__, "bitmap_planar_context_new");
                throw rdp_error(NS_FuncName);
            }
        }

        if(! freerdp_bitmap_planar_context_reset(context->planar, tileSize, tileSize)) {
            Application::error("%s: %s failed", __FUNCTION__, "bitmap_planar_context_reset");
            throw rdp_error(NS_FuncName);
        }

        Application::debug(DebugType::App, "%s: area [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], bits per pixel: %" PRIu8
                           ", scanline: %lu", __FUNCTION__, reg.x, reg.y, reg.width, reg.height, reply->bitsPerPixel(), scanLineBytes);
        auto blocks = reg.divideBlocks(XCB::Size(tileSize, tileSize));
        // Compressed header of bitmap
        // http://msdn.microsoft.com/en-us/library/cc240644.aspx
        const size_t hdrsz = 34;
        std::vector<BITMAP_DATA> vec;
        vec.reserve(blocks.size());

        for(const auto & subreg : blocks) {
            const int16_t localX = subreg.x - reg.x;
            const int16_t localY = subreg.y - reg.y;
            const size_t offset = localY * scanLineBytes + localX * reply->bytePerPixel();
            BITMAP_DATA st = {};
            // Bitmap data here the screen capture
            // https://msdn.microsoft.com/en-us/library/cc240612.aspx
            st.destLeft = subreg.x;
            st.destRight = subreg.x + subreg.width - 1;
            st.width = subreg.width;
            st.bitsPerPixel = reply->bitsPerPixel();
            st.compressed = TRUE;
            st.height = subreg.height;
            st.destTop = subreg.y;
            st.destBottom = subreg.y + subreg.height - 1;
            st.cbScanWidth = subreg.width * reply->bytePerPixel();
            st.cbUncompressedSize = subreg.height * subreg.width * reply->bytePerPixel();
            st.bitmapDataStream = freerdp_bitmap_compress_planar(context->planar, reply->data() + offset,
                                  pixelFormat, subreg.width, subreg.height, scanLineBytes, NULL, & st.bitmapLength);
            st.cbCompMainBodySize = st.bitmapLength;

            if(freeRdp->peer->settings->MultifragMaxRequestSize < st.cbCompMainBodySize + hdrsz) {
                Application::error("%s: %s failed", __FUNCTION__, "MultifragMaxRequestSize");
                throw rdp_error(NS_FuncName);
            }

            vec.emplace_back(st);
        }

        auto it1 = vec.begin();

        while(it1 != vec.end()) {
            // calc blocks
            size_t totalSize = 0;
            auto it2 = std::find_if(it1, vec.end(), [ &](auto & st) {
                if(totalSize + (st.cbCompMainBodySize + hdrsz) > freeRdp->peer->settings->MultifragMaxRequestSize) {
                    return true;
                }

                totalSize += (st.cbCompMainBodySize + hdrsz);
                return false;
            });

            BITMAP_UPDATE bitmapUpdate = {};
            bitmapUpdate.count = bitmapUpdate.number = std::distance(it1, it2);
            bitmapUpdate.rectangles = & (*it1);

            if(! freeRdp->peer->update->BitmapUpdate(context, & bitmapUpdate)) {
                Application::error("%s: %s failed, length: %lu", __FUNCTION__, "BitmapUpdate", totalSize);
                throw rdp_error(NS_FuncName);
            }

            it1 = it2;
        }

        for(const auto & st : vec) {
            std::free(st.bitmapDataStream);
        }

        return true;
    }

    bool ConnectorRdp::updateBitmapInterleaved(const XCB::Region & reg, const XCB::PixmapInfoReply & reply) {
        auto context = static_cast<ServerContext*>(freeRdp->peer->context);
        const size_t scanLineBytes = reg.width * reply->bytePerPixel();
        // size fixed: libfreerdp/codec/interleaved.c
        const size_t tileSize = 64;

        if(reply->size() != reg.height * reg.width * reply->bytePerPixel()) {
            Application::error("%s: %s failed, length: %lu, size: [%" PRIu16 ", %" PRIu16 "], bpp: %" PRIu8, __FUNCTION__,
                               "align region", reply->size(), reg.height, reg.width, reply->bytePerPixel());
            throw rdp_error(NS_FuncName);
        }

        size_t pixelFormat = 0;

        switch(reply->bitsPerPixel()) {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)

            case 16:
                pixelFormat = PIXEL_FORMAT_RGB16;
                break;

            case 24:
                pixelFormat = PIXEL_FORMAT_RGBX32;
                break;
#else

            case 16:
                pixelFormat = PIXEL_FORMAT_BGR16;
                break;

            case 24:
                pixelFormat = PIXEL_FORMAT_BGRX32;
                break;
#endif

            default:
                Application::error("%s: %s failed", __FUNCTION__, "pixel format");
                throw rdp_error(NS_FuncName);
        }

        // planar activate
        if(! context->interleaved) {
            BOOL compressor = TRUE;
            context->interleaved = bitmap_interleaved_context_new(compressor);

            if(! context->interleaved) {
                Application::error("%s: %s failed", __FUNCTION__, "bitmap_interleaved_context_new");
                throw rdp_error(NS_FuncName);
            }
        }

        if(! bitmap_interleaved_context_reset(context->interleaved)) {
            Application::error("%s: %s failed", __FUNCTION__, "bitmap_interleaved_context_reset");
            throw rdp_error(NS_FuncName);
        }

        Application::debug(DebugType::App, "%s: area [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], bits per pixel: %" PRIu8
                           ", scanline: %lu", __FUNCTION__, reg.x, reg.y, reg.width, reg.height, reply->bitsPerPixel(), scanLineBytes);
        auto blocks = reg.divideBlocks(XCB::Size(tileSize, tileSize));
        // Compressed header of bitmap
        // http://msdn.microsoft.com/en-us/library/cc240644.aspx
        BITMAP_DATA st = {};
        // full size reserved
        auto data = std::make_unique<uint8_t[]>(tileSize * tileSize * 4);

        for(const auto & subreg : blocks) {
            const int16_t localX = subreg.x - reg.x;
            const int16_t localY = subreg.y - reg.y;
            const size_t offset = localY * scanLineBytes + localX * reply->bytePerPixel();
            // Bitmap data here the screen capture
            // https://msdn.microsoft.com/en-us/library/cc240612.aspx
            st.destLeft = subreg.x;
            st.destTop = subreg.y;
            st.destRight = subreg.x + subreg.width - 1;
            st.destBottom = subreg.y + subreg.height - 1;
            st.width = subreg.width;
            st.height = subreg.height;
            st.bitsPerPixel = reply->bitsPerPixel();
            st.compressed = TRUE;
            st.cbScanWidth = subreg.width * reply->bytePerPixel();
            st.cbUncompressedSize = subreg.height * subreg.width * reply->bytePerPixel();

            if(! interleaved_compress(context->interleaved, data.get(), & st.bitmapLength, st.width, st.height,
                                      reply->data() + offset, pixelFormat, scanLineBytes, 0, 0, NULL, reply->bitsPerPixel())) {
                Application::error("%s: %s failed", __FUNCTION__, "interleaved_compress");
                throw rdp_error(NS_FuncName);
            }

            st.bitmapDataStream = data.get();
            st.cbCompMainBodySize = st.bitmapLength;

            if(freeRdp->peer->settings->MultifragMaxRequestSize < st.bitmapLength + 22) {
                Application::error("%s: %s failed", __FUNCTION__, "MultifragMaxRequestSize");
                throw rdp_error(NS_FuncName);
            }

            BITMAP_UPDATE bitmapUpdate = {};
            bitmapUpdate.count = bitmapUpdate.number = 1;
            bitmapUpdate.rectangles = & st;
            auto ret = freeRdp->peer->update->BitmapUpdate(context, & bitmapUpdate);

            if(! ret) {
                Application::error("%s: %s failed", __FUNCTION__, "BitmapUpdate");
                throw rdp_error(NS_FuncName);
            }
        }

        return true;
    }

    bool ConnectorRdp::channelsInit(void) {
        if(freeRdp->context->clipboard &&
           WTSVirtualChannelManagerIsChannelJoined(freeRdp->context->vcm, CLIPRDR_SVC_CHANNEL_NAME)) {
            // freeRdp->context->cliprdr = cliprdr_server_context_new(freeRdp->context->vcm);
        }

        return true;
    }

    void ConnectorRdp::channelsFree(void) {
        if(freeRdp->context->cliprdr) {
            // cliprdr_server_context_free(freeRdp->context->cliprdr);
            freeRdp->context->cliprdr = nullptr;
        }
    }

    // freerdp callback func
    BOOL ConnectorRdp::cbServerAuthenticate(freerdp_peer* peer, const char** user, const char** domain,
                                            const char** password) {
        Application::info("%s: peer: %p", __FUNCTION__, peer);
        return TRUE;
    }

    BOOL ConnectorRdp::cbServerCapabilities(freerdp_peer* peer) {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->conrdp;

        if(! connector->createX11Session(24)) {
            Application::error("%s: X11 failed", __FUNCTION__);
            return FALSE;
        }

        peer->settings->ColorDepth = static_cast<XCB::RootDisplay*>(connector)->bitsPerPixel();
        return TRUE;
    }

    BOOL ConnectorRdp::cbServerAdjustMonitorsLayout(freerdp_peer* peer) {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        return TRUE;
    }

    BOOL ConnectorRdp::cbServerClientCapabilities(freerdp_peer* peer) {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        [[maybe_unused]] auto context = static_cast<ServerContext*>(peer->context);
        //auto connector = context->conrdp;
        //peer->settings->ColorDepth = static_cast<XCB::RootDisplay*>(connector)->bitsPerPixel();
        //peer->settings->ColorDepth = 32;
        // if(peer->settings->ColorDepth == 15 || peer->settings->ColorDepth == 16)
        // context->lowcolor = true;
        return TRUE;
    }

    BOOL ConnectorRdp::cbServerPostConnect(freerdp_peer* peer) {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);
        auto wsz = xcbDisplay->size();

        if(wsz.width != peer->settings->DesktopWidth || wsz.height != peer->settings->DesktopHeight) {
            Application::info("%s: request desktop resize [%d,%d], display: %d", __FUNCTION__, peer->settings->DesktopWidth,
                              peer->settings->DesktopHeight, connector->displayNum());
            xcbDisplay->setRandrScreenSize(XCB::Size(peer->settings->DesktopWidth, peer->settings->DesktopHeight));
        }

        if(! connector->channelsInit()) {
            return FALSE;
        }

        return TRUE;
    }

    BOOL ConnectorRdp::cbServerClose(freerdp_peer* peer) {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        return TRUE;
    }

    void ConnectorRdp::cbServerDisconnect(freerdp_peer* peer) {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
    }

    BOOL ConnectorRdp::cbServerActivate(freerdp_peer* peer) {
        Application::info("%s: peer:%p", __FUNCTION__, peer);
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);

        if(1) {
            Application::info("peer settings: %s: 0x%08x", "RdpVersion", peer->settings->RdpVersion);
            Application::info("peer settings: %s: 0x%04x", "OsMajorType", peer->settings->OsMajorType);
            Application::info("peer settings: %s: 0x%04x", "OsMinorType", peer->settings->OsMinorType);
            Application::info("peer settings: %s: %s", "Username", peer->settings->Username);
            Application::info("peer settings: %s: %s", "Domain", peer->settings->Domain);
            Application::info("peer settings: %s: %d", "DesktopWidth", peer->settings->DesktopWidth);
            Application::info("peer settings: %s: %d", "DesktopHeight", peer->settings->DesktopHeight);
            Application::info("peer settings: %s: %d", "DesktopColorDepth", peer->settings->ColorDepth);
            Application::info("peer settings: %s: %s", "peerProductId", peer->settings->ClientProductId);
            Application::info("peer settings: %s: %s", "AutoLogonEnabled", (peer->settings->AutoLogonEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "CompressionEnabled",
                              (peer->settings->CompressionEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "RemoteFxCodec", (peer->settings->RemoteFxCodec ? "true" : "false"));
            Application::info("peer settings: %s: %s", "NSCodec", (peer->settings->NSCodec ? "true" : "false"));
            Application::info("peer settings: %s: %s", "JpegCodec", (peer->settings->JpegCodec ? "true" : "false"));
            Application::info("peer settings: %s: %s", "FrameMarkerCommandEnabled",
                              (peer->settings->FrameMarkerCommandEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "SurfaceFrameMarkerEnabled",
                              (peer->settings->SurfaceFrameMarkerEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "SurfaceCommandsEnabled",
                              (peer->settings->SurfaceCommandsEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "FastPathInput", (peer->settings->FastPathInput ? "true" : "false"));
            Application::info("peer settings: %s: %s", "FastPathOutput", (peer->settings->FastPathOutput ? "true" : "false"));
            Application::info("peer settings: %s: %s", "UnicodeInput", (peer->settings->UnicodeInput ? "true" : "false"));
            Application::info("peer settings: %s: %s", "BitmapCacheEnabled",
                              (peer->settings->BitmapCacheEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "DesktopResize", (peer->settings->DesktopResize ? "true" : "false"));
            Application::info("peer settings: %s: %s", "RefreshRect", (peer->settings->RefreshRect ? "true" : "false"));
            Application::info("peer settings: %s: %s", "SuppressOutput", (peer->settings->SuppressOutput ? "true" : "false"));
            Application::info("peer settings: %s: %s", "TlsSecurity", (peer->settings->TlsSecurity ? "true" : "false"));
            Application::info("peer settings: %s: %s", "NlaSecurity", (peer->settings->NlaSecurity ? "true" : "false"));
            Application::info("peer settings: %s: %s", "RdpSecurity", (peer->settings->RdpSecurity ? "true" : "false"));
            Application::info("peer settings: %s: %s", "SoundBeepsEnabled", (peer->settings->SoundBeepsEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %d", "AuthenticationLevel", peer->settings->AuthenticationLevel);
            Application::info("peer settings: %s: %s", "AllowedTlsCiphers", peer->settings->AllowedTlsCiphers);
            Application::info("peer settings: %s: %d", "TlsSecLevel", peer->settings->TlsSecLevel);
            Application::info("peer settings: %s: %d", "EncryptionMethods", peer->settings->EncryptionMethods);
            Application::info("peer settings: %s: %d", "EncryptionLevel", peer->settings->EncryptionLevel);
            Application::info("peer settings: %s: %d", "CompressionLevel", peer->settings->CompressionLevel);
            Application::info("peer settings: %s: %d", "MultifragMaxRequestSize", peer->settings->MultifragMaxRequestSize);
        }

        std::string encryptionInfo;

        if(0 < peer->settings->TlsSecLevel) {
            encryptionInfo = Tools::joinToString("TLS security level: ", peer->settings->TlsSecLevel);
        }

        switch(peer->settings->EncryptionMethods) {
            case ENCRYPTION_METHOD_40BIT:
                encryptionInfo = Tools::joinToString(encryptionInfo, ", ", "RDP method: ", "40bit");
                break;

            case ENCRYPTION_METHOD_56BIT:
                encryptionInfo = Tools::joinToString(encryptionInfo, ", ", "RDP method: ", "56bit");
                break;

            case ENCRYPTION_METHOD_128BIT:
                encryptionInfo = Tools::joinToString(encryptionInfo, ", ", "RDP method: ", "128bit");
                break;

            case ENCRYPTION_METHOD_FIPS:
                encryptionInfo = Tools::joinToString(encryptionInfo, ", ", "RDP method: ", "fips");
                break;

            default:
                break;
        }

        if(encryptionInfo.size()) {
            connector->setEncryptionInfo(encryptionInfo);
        }

        context->activated = TRUE;
        connector->xcbDisableMessages(false);

        if(peer->settings->Username) {
            std::string user, pass;
            user.assign(peer->settings->Username);

            if(peer->settings->Password) {
                pass.assign(peer->settings->Password);
            }

            if(user == pass) {
                pass.clear();
            }

            connector->setAutoLogin(user, pass);
        }

        xcbDisplay->rootDamageAddRegion(XCB::Region(0, 0, peer->settings->DesktopWidth, peer->settings->DesktopHeight));
        return TRUE;
    }

    /// @param flags: KBD_FLAGS_EXTENDED(0x0100), KBD_FLAGS_EXTENDED1(0x0200), KBD_FLAGS_DOWN(0x4000), KBD_FLAGS_RELEASE(0x8000)
    /// @see:  freerdp/input.h
    BOOL ConnectorRdp::cbServerKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code) {
        Application::debug(DebugType::App, "%s: flags:0x%04" PRIx16 ", code:0x%04" PRIx16 ", input: %p, context: %p", __FUNCTION__, flags, code,
                           input, input->context);
        auto context = static_cast<ServerContext*>(input->context);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);

        connector->_idleSessionTp = std::chrono::steady_clock::now();

        if(connector->xcbAllowMessages()) {
            auto test = static_cast<const XCB::ModuleTest*>(xcbDisplay->getExtension(XCB::Module::TEST));

            if(! test) {
                return FALSE;
            }

            [[maybe_unused]] auto rootWin = xcbDisplay->root();
            uint32_t keysym = static_cast<uint32_t>(flags) << 16 | code;

            // local keymap priority "rdp:keymap:file"
            if(auto value = (context->keymap ? context->keymap->getValue(Tools::hex(keysym, 8)) : nullptr)) {
                // no wait xcb replies
                if(value->isArray()) {
                    auto ja = static_cast<const JsonArray*>(value);

                    for(const auto & val : ja->toStdVector<int>()) {
                        test->screenInputKeycode(val, flags & KBD_FLAGS_DOWN);
                    }
                } else {
                    test->screenInputKeycode(value->getInteger(), flags & KBD_FLAGS_DOWN);
                }
            } else {
                // see winpr/input.h
                // KBDEXT(0x0100), KBDMULTIVK(0x0200), KBDSPECIAL(0x0400), KBDNUMPAD(0x0800),
                // KBDUNICODE(0x1000), KBDINJECTEDVK(0x2000), KBDMAPPEDVK(0x4000), KBDBREAK(0x8000)
                if(flags & KBD_FLAGS_EXTENDED) {
                    code |= KBDEXT;
                }

                // winpr: input
                auto vkcode = GetVirtualKeyCodeFromVirtualScanCode(code, 4);
                auto keycode = GetKeycodeFromVirtualKeyCode((flags & KBD_FLAGS_EXTENDED ? vkcode | KBDEXT : vkcode),
                               KEYCODE_TYPE_EVDEV);
                test->screenInputKeycode(keycode, flags & KBD_FLAGS_DOWN);
            }
        }

        return TRUE;
    }

    /// @param flags: PTR_FLAGS_BUTTON1(0x1000), PTR_FLAGS_BUTTON2(0x2000), PTR_FLAGS_BUTTON3(0x4000), PTR_FLAGS_HWHEEL(0x0400),
    ///               PTR_FLAGS_WHEEL(0x0200), PTR_FLAGS_WHEEL_NEGATIVE(0x0100), PTR_FLAGS_MOVE(0x0800), PTR_FLAGS_DOWN(0x8000)
    /// @see:  freerdp/input.h
    BOOL ConnectorRdp::cbServerMouseEvent(rdpInput* input, UINT16 flags, UINT16 posx, UINT16 posy) {
        Application::debug(DebugType::App, "%s: flags:0x%04" PRIx16 ", pos: [%" PRIu16 ", %" PRIu16 "], input: %p, context: %p", __FUNCTION__,
                           flags, posx, posy, input, input->context);
        auto context = static_cast<ServerContext*>(input->context);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);

        connector->_idleSessionTp = std::chrono::steady_clock::now();

        if(connector->xcbAllowMessages()) {
            auto test = static_cast<const XCB::ModuleTest*>(xcbDisplay->getExtension(XCB::Module::TEST));

            if(! test) {
                return FALSE;
            }

            [[maybe_unused]] auto rootWin = xcbDisplay->root();

            // left button
            if(flags & PTR_FLAGS_BUTTON1) {
                test->screenInputButton(XCB_BUTTON_INDEX_1, XCB::Point(posx, posy), flags & PTR_FLAGS_DOWN);
            } else if(flags & PTR_FLAGS_BUTTON2) {
                // right button
                test->screenInputButton(XCB_BUTTON_INDEX_3, XCB::Point(posx, posy), flags & PTR_FLAGS_DOWN);
            } else if(flags & PTR_FLAGS_BUTTON3) {
                // middle button
                test->screenInputButton(XCB_BUTTON_INDEX_2, XCB::Point(posx, posy), flags & PTR_FLAGS_DOWN);
            } else if(flags & PTR_FLAGS_WHEEL) {
                test->screenInputButton(flags & PTR_FLAGS_WHEEL_NEGATIVE ? XCB_BUTTON_INDEX_5 : XCB_BUTTON_INDEX_4,
                                        XCB::Point(posx, posy), flags & PTR_FLAGS_DOWN);
            }

            if(flags & PTR_FLAGS_MOVE) {
                test->screenInputMove(XCB::Point(posx, posy));
            }
        }

        return TRUE;
    }

    BOOL ConnectorRdp::cbServerRefreshRect(rdpContext* rdpctx, BYTE count, const RECTANGLE_16* areas) {
        Application::debug(DebugType::App, "%s: count rects: %d, context: %p", __FUNCTION__, (int) count, rdpctx);
        auto context = static_cast<ServerContext*>(rdpctx);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);
        std::vector<xcb_rectangle_t> rectangles(0 < count ? count : 1);

        if(count && areas) {
            for(int it = 0; it < count; ++it) {
                rectangles[it].x = areas[it].left;
                rectangles[it].y = areas[it].top;
                rectangles[it].width = areas[it].right - areas[it].left + 1;
                rectangles[it].height = areas[it].bottom - areas[it].top + 1;
            }
        } else {
            auto wsz = xcbDisplay->size();
            rectangles[0].x = 0;
            rectangles[0].y = 0;
            rectangles[0].width = wsz.width;
            rectangles[0].height = wsz.height;
        }

        return xcbDisplay->rootDamageAddRegions(rectangles.data(), rectangles.size());
    }

    BOOL ConnectorRdp::cbServerSuppressOutput(rdpContext* rdpctx, BYTE allow, const RECTANGLE_16* area) {
        auto context = static_cast<ServerContext*>(rdpctx);
        auto connector = context->conrdp;

        if(area && 0 < allow) {
            Application::debug(DebugType::App, "%s: peer restore output(left:%d,top:%d,right:%d,bottom:%d)", __FUNCTION__, area->left, area->top,
                               area->right, area->bottom);
            connector->xcbDisableMessages(false);
            auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);
            xcbDisplay->rootDamageAddRegion(xcbDisplay->region());
        } else {
            Application::debug(DebugType::App, "%s: peer minimized and suppress output", __FUNCTION__);
            connector->xcbDisableMessages(true);
        }

        return TRUE;
    }
}
