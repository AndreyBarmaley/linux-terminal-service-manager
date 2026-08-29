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
#include <freerdp/channels/channels.h>
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
            Application::error("{}: failed", "WTSOpenServer");
            return FALSE;
        }

        context->activated = false;
        context->clipboard = true;
        context->frameId = 0;
        context->config = nullptr;
        context->conrdp = nullptr;
        context->keymap.reset();
        Application::info("{}: success", NS_FuncNameV);
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
    struct FreeRdpEvents {
        freerdp_peer* peer = nullptr;
        ServerContext* context = nullptr;
        HANDLE stopEvent = nullptr;
        HANDLE xcbEvent = nullptr;
        std::mutex xcbLock;

        FreeRdpEvents(int clientFd, const std::string & remoteaddr, const JsonObject & config,
                        ConnectorRdp* connector) : peer(nullptr), context(nullptr) {
            Application::info("freerdp version usage: {}, winpr: {}", FREERDP_VERSION_FULL, WINPR_VERSION_FULL);
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

            peer = freerdp_peer_new(clientFd);
            peer->local = TRUE;
            std::copy_n(remoteaddr.begin(), std::min(sizeof(peer->hostname), remoteaddr.size()), peer->hostname);
            stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            // init context
            peer->ContextSize = sizeof(ServerContext);
            peer->ContextNew = (psPeerContextNew) ServerContextNew;
            peer->ContextFree = (psPeerContextFree) ServerContextFree;

            if(! freerdp_peer_context_new(peer)) {
                Application::error("{}: failed", "freerdp_peer_context_new");
                throw rdp_error(NS_FuncNameS);
            }

            Application::debug(DebugType::App, "peer context: {}", fmt::ptr(peer));
            Application::debug(DebugType::App, "rdp context: {}", fmt::ptr(peer->context));
            context = static_cast<ServerContext*>(peer->context);
            context->config = & config;
            context->conrdp = connector;
            context->clipboard = false;

            if(auto keymapFile = config.getString("rdp:keymap:file"); ! keymapFile.empty()) {
                JsonContentFile jc(keymapFile);

                if(jc.isValid() && jc.isObject()) {
                    context->keymap = std::make_unique<JsonObject>(jc.toObject());
                    Application::info("keymap loaded: {}, items: {}", keymapFile, context->keymap->size());
                }
            }

            auto certfile = connector->checkFileOption("rdp:server:certfile");

            if(certfile.size()) {
                peer->settings->CertificateFile = strdup(certfile.c_str());
                Application::info("server cert: {}", peer->settings->CertificateFile);
            }

            auto keyfile = connector->checkFileOption("rdp:server:keyfile");

            if(keyfile.size()) {
                peer->settings->PrivateKeyFile = strdup(keyfile.c_str());
                peer->settings->RdpKeyFile = strdup(keyfile.c_str());
                Application::info("server key: {}", peer->settings->RdpKeyFile);
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
            peer->PostConnect = ConnectorRdp::rdpServerPostConnect;
            peer->Activate = ConnectorRdp::rdpServerActivate;
            peer->Close = ConnectorRdp::rdpServerClose;
            peer->Disconnect = ConnectorRdp::rdpServerDisconnect;
            peer->Capabilities = ConnectorRdp::rdpServerCapabilities;
            peer->AdjustMonitorsLayout = ConnectorRdp::rdpServerAdjustMonitorsLayout;
            peer->ClientCapabilities = ConnectorRdp::rdpServerClientCapabilities;
            peer->input->KeyboardEvent = ConnectorRdp::rdpServerKeyboardEvent;
            peer->input->MouseEvent = ConnectorRdp::rdpServerMouseEvent;
            peer->update->RefreshRect = ConnectorRdp::rdpServerRefreshRect;
            peer->update->SuppressOutput = ConnectorRdp::rdpServerSuppressOutput;

            if(1 != peer->Initialize(peer)) {
                Application::error("{}: {} failed", NS_FuncNameV, "Peer::Initialize");
                throw rdp_error(NS_FuncNameS);
            }
        }

        ~FreeRdpEvents() {
            if(xcbEvent) {
                CloseHandle(xcbEvent);
            }

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
                Application::info("{}: stop event", NS_FuncNameV);
                SetEvent(stopEvent);
            }
        }

        void xcbDisconnected(void) {
            const std::scoped_lock guard{xcbLock};

            if(xcbEvent) {
                CloseHandle(xcbEvent);
                xcbEvent = nullptr;
            }
        }

        bool isActivated(void) const {
            return context ? context->activated : false;
        }

        void xcbConnected(void) {
            const std::scoped_lock guard{xcbLock};
            const ConnectorRdp* connector = context->conrdp;

            if(xcbEvent) {
                CloseHandle(xcbEvent);
            }

            xcbEvent = CreateFileDescriptorEventA(NULL, TRUE, FALSE, connector->getFd(), WINPR_FD_READ);
        }

        bool enterEventLoop(bool nodamage) {
            Application::info("{}: enter event loop", NS_FuncNameV);
            ConnectorRdp* connector = context->conrdp;

            using TimePointSeconds = Tools::TimePoint<std::chrono::seconds>;
            auto timerNotActivated = std::make_unique<TimePointSeconds>(30s);
            auto updateTp = std::chrono::steady_clock::now();
            const auto updateTimeoutMs = static_cast<uint32_t>(1000 / connector->frameRateOption());

            // freerdp client events
            while(true) {
                if(peer->CheckFileDescriptor(peer) != TRUE) {
                    break;
                }

                if(WTSVirtualChannelManagerCheckFileDescriptor(context->vcm) != TRUE) {
                    break;
                }

                if(! stopEvent ||
                   WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                    break;
                }

                if(connector->xcbAllowMessages() && xcbEvent &&
                   WaitForSingleObject(xcbEvent, 0) == WAIT_OBJECT_0) {
                    const std::scoped_lock guard{xcbLock};

                    if(auto err = connector->hasError()) {
                        connector->xcbDisableMessages(true);
                        Application::error("{}: xcb error: {}", NS_FuncNameV, err);
                        break;
                    }

                    // processing xcb events
                    while(auto ev = connector->pollEvent()) {
                    }

                    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - updateTp);
                    if(dt.count() >= updateTimeoutMs) {
                        bool success = connector->updateDisplayEvent(nodamage);
                        updateTp = std::chrono::steady_clock::now();

                        if(!success) {
                            Application::error("{}: update failed", NS_FuncNameV);
                            break;
                        }
                    }
                }

                if(timerNotActivated && timerNotActivated->check()) {
                    timerNotActivated.reset();

                    if(! context->activated) {
                        Application::error("{}: timeout trigger: {}", NS_FuncNameV, "not activated");
                        break;
                    }
                }

                // wait
                std::this_thread::sleep_for(1ms);
            }

            if(stopEvent) {
                // for shutdown flag
                CloseHandle(stopEvent);
                stopEvent = nullptr;
            }

            if(xcbEvent) {
                CloseHandle(xcbEvent);
                xcbEvent = nullptr;
            }

            peer->Disconnect(peer);
            connector->stop();

            Application::info("{}: event loop shutdown", NS_FuncNameV);
            return true;
        }
    };

    ConnectorRdp::ConnectorRdp(const std::filesystem::path & confile, bool debug)
        : DBusProxy(ConnectorType::RDP, confile, debug) {
    }

    ConnectorRdp::~ConnectorRdp() {
        try {
            if(0 < displayNum()) {
                busConnectorTerminated(displayNum(), getpid());
                disconnectedEvent();
                Application::info("{}: connector shutdown, display: {}", NS_FuncNameV, displayNum());
            }
        } catch(const std::exception & err) {
            Application::warning("{}: connector error: {}", NS_FuncNameV, err.what());
        }
    }

    void ConnectorRdp::stop(void) noexcept {
        std::call_once(stopFlag_, [this]() {
            try {
                DBusProxy::asioStop();
            } catch(const std::exception &) {
            }
        });
    }

    int ConnectorRdp::start(void) {
        if(0 >= busGetServiceVersion()) {
            Application::error("{}: failed", "bus service");
            return EXIT_FAILURE;
        }

        auto home = LTSM::Connector::homeRuntime();
        Application::info("{}: remote addr: {}", NS_FuncNameV, remoteAddress());

        // create FreeRdpEvents
        Application::info("{}: {}", NS_FuncNameV, "create freerdp context");
        rdpEvents_ = std::make_unique<FreeRdpEvents>(InetStream::fd(), remoteAddress(), config(), this);
        damageRegion_.assign(0, 0, 0, 0);

        bool nodamage = config().getBoolean("rdp:xcb:nodamage", false);
        frameRate_ = config().getInteger("frame:rate", frameRate_);
        rdpEvents_->enterEventLoop(nodamage);
        // BoostContext::run();

        channelsFree();
        return EXIT_SUCCESS;
    }

    uint32_t ConnectorRdp::frameRateOption(void) const {
        constexpr uint32_t minFps = 5;
        constexpr uint32_t maxFps = 20;
        return std::clamp(frameRate_, minFps, maxFps);
    }

    bool ConnectorRdp::updateDisplayEvent(bool nodamage) {
        if(nodamage) {
            damageRegion_ = RootDisplay::region();
        } else if(! damageRegion_.isEmpty()) {
            // fix out of screen
            damageRegion_ = RootDisplay::region().intersected(damageRegion_.align(4));
        }

        if(! damageRegion_.isEmpty() && rdpEvents_->isActivated()) {
            if(! updateRegionEvent(damageRegion_)) {
                Application::error("{}: update failed", NS_FuncNameV);
                return false;
            }

            rootDamageSubtrack(damageRegion_);
            damageRegion_.reset();
        }

        return true;
    }

    void ConnectorRdp::xcbDamageNotifyEvent(const xcb_rectangle_t & rt, uint8_t level) {
        damageRegion_.join(rt.x, rt.y, rt.width, rt.height);
    }

    void ConnectorRdp::xcbRandrScreenChangedEvent(const XCB::Size & dsz, const xcb_randr_notify_event_t & ne) {
        damageRegion_.reset();
        busDisplayResized(displayNum(), dsz.width, dsz.height);
        desktopResizeEvent(*rdpEvents_->peer, dsz.width, dsz.height);
    }

    void ConnectorRdp::xcbXkbGroupChangedEvent(int) {
    }

    void ConnectorRdp::setEncryptionInfo(const std::string & info) {
        busSetEncryptionInfo(displayNum(), info);
    }

    void ConnectorRdp::setAutoLogin(const std::string & login, const std::string & pass) {
        helperSetSessionLoginPassword(displayNum(), login, pass, false);
    }

    bool ConnectorRdp::createX11Session(uint8_t depth) {
        int screen = busStartLoginSession(getpid(), depth, remoteAddress(), "rdp");

        if(screen <= 0) {
            Application::error("{}", "login session request failure");
            return false;
        }

        Application::debug(DebugType::App, "login session request success, display: {}", screen);
        rdpEvents_->xcbDisconnected();

        if(! xcbConnect(screen, *this)) {
            Application::error("{}", "xcb connect failed");
            return false;
        }

        const xcb_visualtype_t* visual = XCB::RootDisplay::visual();

        if(! visual) {
            Application::error("{}", "xcb visual empty");
            return false;
        }

        Application::info("{}: xcb max request: {}", NS_FuncNameV, XCB::RootDisplay::getMaxRequest());
        // init server format
        serverFormat_ = PixelFormat(XCB::RootDisplay::bitsPerPixel(),
                                   visual->red_mask, visual->green_mask, visual->blue_mask, 0);
        rdpEvents_->xcbConnected();
        // FIXME sleep
        std::this_thread::sleep_for(50ms);
        return true;
    }

    void ConnectorRdp::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t & userUid) {
        if(display != displayNum()) {
            return;
        }

        // disable xcb messages processing
        xcbDisableMessages(true);
        rdpEvents_->xcbDisconnected();

        Application::notice("{}: display: {}, username: {}", NS_FuncNameV, display, userName);
        int oldDisplay = displayNum();
        int newDisplay = busStartUserSession(oldDisplay, getpid(), userName, remoteAddress(), connectorType());

        if(newDisplay < 0) {
            Application::error("{}: {} failed", NS_FuncNameV, "user session request");
            throw rdp_error(NS_FuncNameS);
        }

        if(newDisplay != oldDisplay) {
            // wait xcb old operations ended
            std::this_thread::sleep_for(100ms);

            if(! xcbConnect(newDisplay, *this)) {
                Application::error("{}: {} failed", NS_FuncNameV, "xcb connect");
                throw rdp_error(NS_FuncNameS);
            }

            busShutdownDisplay(oldDisplay);
        }

        // update context
        rdpEvents_->xcbConnected();
        xcbDisableMessages(false);

        // fix new session size
        auto wsz = XCB::RootDisplay::size();

        if(wsz.width != rdpEvents_->peer->settings->DesktopWidth || wsz.height != rdpEvents_->peer->settings->DesktopHeight) {
            Application::warning("{}: remote request desktop size: [{}, {}], display: {}", NS_FuncNameV,
                                 rdpEvents_->peer->settings->DesktopWidth, rdpEvents_->peer->settings->DesktopHeight, displayNum());

            if(XCB::RootDisplay::setRandrScreenSize(XCB::Size(rdpEvents_->peer->settings->DesktopWidth,
                    rdpEvents_->peer->settings->DesktopHeight))) {
                wsz = XCB::RootDisplay::size();
                Application::info("{}: change session size: {}, display: {}", NS_FuncNameV, wsz, displayNum());
            }
        } else {
            // full update
            serverScreenUpdateRequest(XCB::RootDisplay::region());
        }

        busConnectorConnected(newDisplay, getpid());
    }

    void ConnectorRdp::onShutdownConnector(const int32_t & display) {
        if(display == displayNum()) {
            Application::info("{}: dbus signal shutdown, display: {}", NS_FuncNameV, display);
            xcbDisableMessages(true);
            rdpEvents_->stopEventLoop();
        }
    }

    void ConnectorRdp::onSendBellSignal(const int32_t & display) {
        if(display == displayNum() &&
           rdpEvents_ && rdpEvents_->peer && rdpEvents_->peer->settings && rdpEvents_->peer->settings->SoundBeepsEnabled) {
            // FIXME beep
        }
    }

    void ConnectorRdp::serverScreenUpdateRequest(const XCB::Region & reg) {
        if(xcbAllowMessages()) {
            XCB::RootDisplay::rootDamageAddRegion(reg);
        }
    }

    // client events
    void ConnectorRdp::disconnectedEvent(void) {
        Application::warning("{}: display: {}", NS_FuncNameV, displayNum());
    }

    void ConnectorRdp::desktopResizeEvent(freerdp_peer & peer, uint16_t width, uint16_t height) {
        Application::info("{}: size: [{}, {}]", NS_FuncNameV, width, height);
        auto context = static_cast<ServerContext*>(peer.context);
        context->activated = false;
        peer.settings->DesktopWidth = width;
        peer.settings->DesktopHeight = height;

        if(peer.update->DesktopResize(peer.update->context)) {
            Application::error("{}: [{}, {}] failed", NS_FuncNameV, width, height);
        }
    }

    bool ConnectorRdp::updateRegionEvent(const XCB::Region & reg) {
        try {
            //auto context = static_cast<ServerContext*>(rdpEvents_->peer->context);
            auto reply = XCB::RootDisplay::copyRootImageRegion(reg);
            // reply info dump
            Application::debug(DebugType::App, "{}: request size: {}, reply length: {}, bits per pixel: {}, red: {:#010x}, green: {:#010x}, blue: {:#010x}",
                           NS_FuncNameV, reg.toSize(), reply->size(), reply->bitsPerPixel(), reply->rmask, reply->gmask, reply->bmask);
            FrameBuffer frameBuffer(reply->data(), reg, serverFormat_);
            // apply render primitives
            renderPrimitivesToFB(frameBuffer);
            return 24 == reply->bitsPerPixel() || 32 == reply->bitsPerPixel() ?
               updateBitmapPlanar(reg, reply) : updateBitmapInterleaved(reg, reply);
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        }
        return false;
    }

    bool ConnectorRdp::updateBitmapPlanar(const XCB::Region & reg, const XCB::PixmapInfoReply & reply) {
        auto context = static_cast<ServerContext*>(rdpEvents_->peer->context);
        const size_t scanLineBytes = reg.width * reply->bytePerPixel();
        const size_t tileSize = 64;
        const size_t pixelFormat = rdpEvents_->peer->settings->OsMajorType == 6 ? PIXEL_FORMAT_RGBX32 : PIXEL_FORMAT_BGRX32;

        if(reply->size() != reg.height * reg.width * reply->bytePerPixel()) {
            Application::error("{}: {} failed, length: {}, size: {}, bpp: {}", NS_FuncNameV,
                               "align region", reply->size(), reg.toSize(), reply->bytePerPixel());
            throw rdp_error(NS_FuncNameS);
        }

        // planar activate
        if(! context->planar) {
            DWORD planarFlags = PLANAR_FORMAT_HEADER_RLE;

            if(rdpEvents_->peer->settings->DrawAllowSkipAlpha) {
                planarFlags |= PLANAR_FORMAT_HEADER_NA;
            }

            context->planar = freerdp_bitmap_planar_context_new(planarFlags, tileSize, tileSize);

            if(! context->planar) {
                Application::error("{}: {} failed", NS_FuncNameV, "bitmap_planar_context_new");
                throw rdp_error(NS_FuncNameS);
            }
        }

        if(! freerdp_bitmap_planar_context_reset(context->planar, tileSize, tileSize)) {
            Application::error("{}: {} failed", NS_FuncNameV, "bitmap_planar_context_reset");
            throw rdp_error(NS_FuncNameS);
        }

        Application::debug(DebugType::App, "{}: area: {}, bits per pixel: {}, scanline: {}",
                           NS_FuncNameV, reg, reply->bitsPerPixel(), scanLineBytes);
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

            if(rdpEvents_->peer->settings->MultifragMaxRequestSize < st.cbCompMainBodySize + hdrsz) {
                Application::error("{}: {} failed", NS_FuncNameV, "MultifragMaxRequestSize");
                throw rdp_error(NS_FuncNameS);
            }

            vec.emplace_back(st);
        }

        auto it1 = vec.begin();

        while(it1 != vec.end()) {
            // calc blocks
            size_t totalSize = 0;
            auto it2 = std::ranges::find_if(it1, vec.end(), [&](auto & st) {
                if(totalSize + (st.cbCompMainBodySize + hdrsz) > rdpEvents_->peer->settings->MultifragMaxRequestSize) {
                    return true;
                }

                totalSize += (st.cbCompMainBodySize + hdrsz);
                return false;
            });

            BITMAP_UPDATE bitmapUpdate = {};
            bitmapUpdate.count = bitmapUpdate.number = std::distance(it1, it2);
            bitmapUpdate.rectangles = & (*it1);

            if(! rdpEvents_->peer->update->BitmapUpdate(context, & bitmapUpdate)) {
                Application::error("{}: {} failed, length: {}", NS_FuncNameV, "BitmapUpdate", totalSize);
                throw rdp_error(NS_FuncNameS);
            }

            it1 = it2;
        }

        for(const auto & st : vec) {
            std::free(st.bitmapDataStream);
        }

        return true;
    }

    bool ConnectorRdp::updateBitmapInterleaved(const XCB::Region & reg, const XCB::PixmapInfoReply & reply) {
        auto context = static_cast<ServerContext*>(rdpEvents_->peer->context);
        const size_t scanLineBytes = reg.width * reply->bytePerPixel();
        // size fixed: libfreerdp/codec/interleaved.c
        const size_t tileSize = 64;

        if(reply->size() != reg.height * reg.width * reply->bytePerPixel()) {
            Application::error("{}: {} failed, length: {}, size: {}, bpp: {}", NS_FuncNameV,
                               "align region", reply->size(), reg.toSize(), reply->bytePerPixel());
            throw rdp_error(NS_FuncNameS);
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
                Application::error("{}: {} failed", NS_FuncNameV, "pixel format");
                throw rdp_error(NS_FuncNameS);
        }

        // planar activate
        if(! context->interleaved) {
            BOOL compressor = TRUE;
            context->interleaved = bitmap_interleaved_context_new(compressor);

            if(! context->interleaved) {
                Application::error("{}: {} failed", NS_FuncNameV, "bitmap_interleaved_context_new");
                throw rdp_error(NS_FuncNameS);
            }
        }

        if(! bitmap_interleaved_context_reset(context->interleaved)) {
            Application::error("{}: {} failed", NS_FuncNameV, "bitmap_interleaved_context_reset");
            throw rdp_error(NS_FuncNameS);
        }

        Application::debug(DebugType::App, "{}: area: {}, bits per pixel: {}, scanline: {}",
                           NS_FuncNameV, reg, reply->bitsPerPixel(), scanLineBytes);
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
                Application::error("{}: {} failed", NS_FuncNameV, "interleaved_compress");
                throw rdp_error(NS_FuncNameS);
            }

            st.bitmapDataStream = data.get();
            st.cbCompMainBodySize = st.bitmapLength;

            if(rdpEvents_->peer->settings->MultifragMaxRequestSize < st.bitmapLength + 22) {
                Application::error("{}: {} failed", NS_FuncNameV, "MultifragMaxRequestSize");
                throw rdp_error(NS_FuncNameS);
            }

            BITMAP_UPDATE bitmapUpdate = {};
            bitmapUpdate.count = bitmapUpdate.number = 1;
            bitmapUpdate.rectangles = & st;
            auto ret = rdpEvents_->peer->update->BitmapUpdate(context, & bitmapUpdate);

            if(! ret) {
                Application::error("{}: {} failed", NS_FuncNameV, "BitmapUpdate");
                throw rdp_error(NS_FuncNameS);
            }
        }

        return true;
    }

    bool ConnectorRdp::channelsInit(void) {
/*
        if(rdpEvents_->context->clipboard &&
           WTSVirtualChannelManagerIsChannelJoined(rdpEvents_->context->vcm, CLIPRDR_SVC_CHANNEL_NAME)) {
        }
*/
        return true;
    }

    void ConnectorRdp::channelsFree(void) {
    }

    // freerdp callback func
    BOOL ConnectorRdp::rdpServerAuthenticate(freerdp_peer* peer, const char** user, const char** domain,
                                            const char** password) {
        Application::info("{}: peer: {}", NS_FuncNameV, fmt::ptr(peer));
        return TRUE;
    }

    BOOL ConnectorRdp::rdpServerCapabilities(freerdp_peer* peer) {
        Application::info("{}: peer: {}, desktop: [{}, {}], peer depth: {}", NS_FuncNameV, fmt::ptr(peer), peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->conrdp;

        if(! connector->createX11Session(24)) {
            Application::error("{}: X11 failed", NS_FuncNameV);
            return FALSE;
        }

        peer->settings->ColorDepth = static_cast<XCB::RootDisplay*>(connector)->bitsPerPixel();
        return TRUE;
    }

    BOOL ConnectorRdp::rdpServerAdjustMonitorsLayout(freerdp_peer* peer) {
        Application::info("{}: peer: {}, desktop: [{}, {}], peer depth: {}", NS_FuncNameV, fmt::ptr(peer), peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        return TRUE;
    }

    BOOL ConnectorRdp::rdpServerClientCapabilities(freerdp_peer* peer) {
        Application::info("{}: peer: {}, desktop: [{}, {}], peer depth: {}", NS_FuncNameV, fmt::ptr(peer), peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        [[maybe_unused]] auto context = static_cast<ServerContext*>(peer->context);
        //auto connector = context->conrdp;
        //peer->settings->ColorDepth = static_cast<XCB::RootDisplay*>(connector)->bitsPerPixel();
        //peer->settings->ColorDepth = 32;
        // if(peer->settings->ColorDepth == 15 || peer->settings->ColorDepth == 16)
        // context->lowcolor = true;
        return TRUE;
    }

    BOOL ConnectorRdp::rdpServerPostConnect(freerdp_peer* peer) {
        Application::info("{}: peer: {}, desktop: [{}, {}], peer depth: {}", NS_FuncNameV, fmt::ptr(peer), peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);
        auto wsz = xcbDisplay->size();

        if(wsz.width != peer->settings->DesktopWidth || wsz.height != peer->settings->DesktopHeight) {
            Application::info("{}: request desktop resize [{}, {}], display: {}", NS_FuncNameV, peer->settings->DesktopWidth,
                              peer->settings->DesktopHeight, connector->displayNum());
            xcbDisplay->setRandrScreenSize(XCB::Size(peer->settings->DesktopWidth, peer->settings->DesktopHeight));
        }

        if(! connector->channelsInit()) {
            return FALSE;
        }

        return TRUE;
    }

    BOOL ConnectorRdp::rdpServerClose(freerdp_peer* peer) {
        Application::info("{}: peer: {}, desktop: [{}, {}], peer depth: {}", NS_FuncNameV, fmt::ptr(peer), peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
        return TRUE;
    }

    void ConnectorRdp::rdpServerDisconnect(freerdp_peer* peer) {
        Application::info("{}: peer: {}, desktop: [{}, {}], peer depth: {}", NS_FuncNameV, fmt::ptr(peer), peer->settings->DesktopWidth,
                          peer->settings->DesktopHeight, peer->settings->ColorDepth);
    }

    inline const char* fmt_cstr(const char* str) {
        return str ? str : "(null)";
    }

    BOOL ConnectorRdp::rdpServerActivate(freerdp_peer* peer) {
        Application::info("{}: peer:{}", NS_FuncNameV, fmt::ptr(peer));
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);

        if(1) {
            Application::info("peer settings: {}: {:#010x}", "RdpVersion", peer->settings->RdpVersion);
            Application::info("peer settings: {}: {:#06x}", "OsMajorType", peer->settings->OsMajorType);
            Application::info("peer settings: {}: {:#06x}", "OsMinorType", peer->settings->OsMinorType);
            Application::info("peer settings: {}: {}", "Username", fmt_cstr(peer->settings->Username));
            Application::info("peer settings: {}: {}", "Domain", fmt_cstr(peer->settings->Domain));
            Application::info("peer settings: {}: {}", "DesktopWidth", peer->settings->DesktopWidth);
            Application::info("peer settings: {}: {}", "DesktopHeight", peer->settings->DesktopHeight);
            Application::info("peer settings: {}: {}", "DesktopColorDepth", peer->settings->ColorDepth);
            Application::info("peer settings: {}: {}", "peerProductId", peer->settings->ClientProductId);
            Application::info("peer settings: {}: {}", "AutoLogonEnabled", (peer->settings->AutoLogonEnabled ? "true" : "false"));
            Application::info("peer settings: {}: {}", "CompressionEnabled",
                              (peer->settings->CompressionEnabled ? "true" : "false"));
            Application::info("peer settings: {}: {}", "RemoteFxCodec", (peer->settings->RemoteFxCodec ? "true" : "false"));
            Application::info("peer settings: {}: {}", "NSCodec", (peer->settings->NSCodec ? "true" : "false"));
            Application::info("peer settings: {}: {}", "JpegCodec", (peer->settings->JpegCodec ? "true" : "false"));
            Application::info("peer settings: {}: {}", "FrameMarkerCommandEnabled",
                              (peer->settings->FrameMarkerCommandEnabled ? "true" : "false"));
            Application::info("peer settings: {}: {}", "SurfaceFrameMarkerEnabled",
                              (peer->settings->SurfaceFrameMarkerEnabled ? "true" : "false"));
            Application::info("peer settings: {}: {}", "SurfaceCommandsEnabled",
                              (peer->settings->SurfaceCommandsEnabled ? "true" : "false"));
            Application::info("peer settings: {}: {}", "FastPathInput", (peer->settings->FastPathInput ? "true" : "false"));
            Application::info("peer settings: {}: {}", "FastPathOutput", (peer->settings->FastPathOutput ? "true" : "false"));
            Application::info("peer settings: {}: {}", "UnicodeInput", (peer->settings->UnicodeInput ? "true" : "false"));
            Application::info("peer settings: {}: {}", "BitmapCacheEnabled",
                              (peer->settings->BitmapCacheEnabled ? "true" : "false"));
            Application::info("peer settings: {}: {}", "DesktopResize", (peer->settings->DesktopResize ? "true" : "false"));
            Application::info("peer settings: {}: {}", "RefreshRect", (peer->settings->RefreshRect ? "true" : "false"));
            Application::info("peer settings: {}: {}", "SuppressOutput", (peer->settings->SuppressOutput ? "true" : "false"));
            Application::info("peer settings: {}: {}", "TlsSecurity", (peer->settings->TlsSecurity ? "true" : "false"));
            Application::info("peer settings: {}: {}", "NlaSecurity", (peer->settings->NlaSecurity ? "true" : "false"));
            Application::info("peer settings: {}: {}", "RdpSecurity", (peer->settings->RdpSecurity ? "true" : "false"));
            Application::info("peer settings: {}: {}", "SoundBeepsEnabled", (peer->settings->SoundBeepsEnabled ? "true" : "false"));
            Application::info("peer settings: {}: {}", "AuthenticationLevel", peer->settings->AuthenticationLevel);
            Application::info("peer settings: {}: {}", "AllowedTlsCiphers", fmt_cstr(peer->settings->AllowedTlsCiphers));
            Application::info("peer settings: {}: {}", "TlsSecLevel", peer->settings->TlsSecLevel);
            Application::info("peer settings: {}: {}", "EncryptionMethods", peer->settings->EncryptionMethods);
            Application::info("peer settings: {}: {}", "EncryptionLevel", peer->settings->EncryptionLevel);
            Application::info("peer settings: {}: {}", "CompressionLevel", peer->settings->CompressionLevel);
            Application::info("peer settings: {}: {}", "MultifragMaxRequestSize", peer->settings->MultifragMaxRequestSize);
        }

        std::string encryptionInfo;

        if(0 < peer->settings->TlsSecLevel) {
            encryptionInfo = fmt::format("TLS security level: {}", peer->settings->TlsSecLevel);
        }

        switch(peer->settings->EncryptionMethods) {
            case ENCRYPTION_METHOD_40BIT:
                encryptionInfo = fmt::format("{}, RDP method: {}", encryptionInfo, "40bit");
                break;

            case ENCRYPTION_METHOD_56BIT:
                encryptionInfo = fmt::format("{}, RDP method: {}", encryptionInfo, "56bit");
                break;

            case ENCRYPTION_METHOD_128BIT:
                encryptionInfo = fmt::format("{}, RDP method: {}", encryptionInfo, "128bit");
                break;

            case ENCRYPTION_METHOD_FIPS:
                encryptionInfo = fmt::format("{}, RDP method: {}", encryptionInfo, "fips");
                break;

            default:
                break;
        }

        if(encryptionInfo.size()) {
            connector->setEncryptionInfo(encryptionInfo);
        }

        context->activated = true;
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
    BOOL ConnectorRdp::rdpServerKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code) {
        Application::debug(DebugType::App, "{}: flags: {:#06x}, code: {:#06x}, input: {}, context: {}", NS_FuncNameV, flags, code,
                           fmt::ptr(input), fmt::ptr(input->context));
        auto context = static_cast<ServerContext*>(input->context);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);

        connector->idleSessionReset();

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
    BOOL ConnectorRdp::rdpServerMouseEvent(rdpInput* input, UINT16 flags, UINT16 posx, UINT16 posy) {
        Application::debug(DebugType::App, "{}: flags: {:#06x}, pos: {}, input: {}, context: {}", NS_FuncNameV,
                           flags, XCB::Point(posx, posy), fmt::ptr(input), fmt::ptr(input->context));
        auto context = static_cast<ServerContext*>(input->context);
        auto connector = context->conrdp;
        auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);

        connector->idleSessionReset();

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

    BOOL ConnectorRdp::rdpServerRefreshRect(rdpContext* rdpctx, BYTE count, const RECTANGLE_16* areas) {
        Application::debug(DebugType::App, "{}: count rects: {}, context: {}", NS_FuncNameV, (int) count, fmt::ptr(rdpctx));
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

    BOOL ConnectorRdp::rdpServerSuppressOutput(rdpContext* rdpctx, BYTE allow, const RECTANGLE_16* area) {
        auto context = static_cast<ServerContext*>(rdpctx);
        auto connector = context->conrdp;

        if(area && 0 < allow) {
            Application::debug(DebugType::App, "{}: peer restore output(left:{},top:{},right:{},bottom:{})", NS_FuncNameV, area->left, area->top,
                               area->right, area->bottom);
            connector->xcbDisableMessages(false);
            auto xcbDisplay = static_cast<XCB::RootDisplay*>(connector);
            xcbDisplay->rootDamageAddRegion(xcbDisplay->region());
        } else {
            Application::debug(DebugType::App, "{}: peer minimized and suppress output", NS_FuncNameV);
            connector->xcbDisableMessages(true);
        }

        return TRUE;
    }
}
