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
#include <future>
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

namespace LTSM
{
    void stream_free(wStream* st)
    {
        Stream_Free(st, TRUE);
    }

    struct ClientContext
    {
        int                     test;
    };

    struct ServerContext : rdpContext
    {
        BITMAP_PLANAR_CONTEXT* 	planar;
        BITMAP_INTERLEAVED_CONTEXT* interleaved;
        HANDLE			vcm;
        CliprdrServerContext*   cliprdr;

        bool			activated;
        bool                    clipboard;
        size_t			frameId;

        const JsonObject*	config;
        Connector::RDP*		rdp;
        JsonObject*             keymap;
    };

    int ServerContextNew(rdp_freerdp_peer* peer, ServerContext* context)
    {
        context->planar = nullptr;
        context->interleaved = nullptr;
        context->vcm = WTSOpenServerA((LPSTR) peer->context);

        if(! context->vcm || context->vcm == INVALID_HANDLE_VALUE)
        {
            Application::error("%s: failed", "WTSOpenServer");
            return FALSE;
        }

        context->cliprdr = nullptr;
        context->activated = false;
        context->clipboard = true;
        context->frameId = 0;
        context->config = nullptr;
        context->rdp = nullptr;
        context->keymap = nullptr;

        Application::info("%s: success", __FUNCTION__);
        return TRUE;
    }

    void ServerContextFree(rdp_freerdp_peer* peer, ServerContext* context)
    {
        if(context->planar)
        {
            freerdp_bitmap_planar_context_free(context->planar);
            context->planar = nullptr;
        }

        if(context->interleaved)
        {
            bitmap_interleaved_context_free(context->interleaved);
            context->interleaved = nullptr;
        }

        if(context->vcm)
        {
        }

        if(context->vcm)
        {
            WTSCloseServer(context->vcm);
            context->vcm = nullptr;
        }

        if(context->keymap)
        {
            delete context->keymap;
            context->keymap = nullptr;
        }
    }

    // FreeRdp
    struct FreeRdpCallback
    {
        freerdp_peer*		peer;
        ServerContext*		context;
        std::atomic<HANDLE>	stopEvent;

        FreeRdpCallback(int fd, const std::string & remoteaddr, const JsonObject & config, Connector::RDP* connector) : peer(nullptr), context(nullptr)
        {
            Application::info("freerdp version usage: %s, winpr: %s", FREERDP_VERSION_FULL, WINPR_VERSION_FULL);
            winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
            WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
            // init freerdp log system
            auto log = WLog_GetRoot();

            if(log)
            {
                WLog_SetLogAppenderType(log, WLOG_APPENDER_SYSLOG);
                auto str = Tools::lower(config.getString("rdp:wlog:level"));
                int type = WLOG_ERROR;

                if(str == "trace")
                    type = WLOG_TRACE;
                else if(str == "debug")
                    type = WLOG_DEBUG;
                else if(str == "info")
                    type = WLOG_INFO;
                else if(str == "warn")
                    type = WLOG_WARN;
                else if(str == "error")
                    type = WLOG_ERROR;
                else if(str == "fatal")
                    type = WLOG_FATAL;
                else if(str == "off")
                    type = WLOG_OFF;

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

            if(! freerdp_peer_context_new(peer))
            {
                Application::error("%s: failed", "freerdp_peer_context_new");
                throw EXIT_FAILURE;
            }

            Application::debug("peer context: %p", peer);
            Application::debug("rdp context: %p", peer->context);
            context = static_cast<ServerContext*>(peer->context);
            context->config = & config;
            context->rdp = connector;
            context->clipboard = config.getBoolean("rdp:clipboard", true);
            const std::string keymapFile = config.getString("rdp:keymap:file");

            if(! keymapFile.empty())
            {
                JsonContentFile jc(keymapFile);

                if(jc.isValid() && jc.isObject())
                {
                    context->keymap = new JsonObject(jc.toObject());
                    Application::info("keymap loaded: %s, items: %d", keymapFile.c_str(), context->keymap->size());
                }
            }

            auto certfile = connector->checkFileOption("rdp:server:certfile");

            if(certfile.size())
            {
                peer->settings->CertificateFile = strdup(certfile.c_str());
                Application::info("server cert: %s", peer->settings->CertificateFile);
            }

            auto keyfile = connector->checkFileOption("rdp:server:keyfile");

            if(keyfile.size())
            {
                peer->settings->PrivateKeyFile = strdup(keyfile.c_str());
                peer->settings->RdpKeyFile = strdup(keyfile.c_str());
                Application::info("server key: %s", peer->settings->RdpKeyFile);
            }

            int encryptionLevel = ENCRYPTION_LEVEL_NONE;
            auto paramencryptionLevel = Tools::lower(config.getString("rdp:encription:level", "compatible"));

            if(paramencryptionLevel == "high")
                encryptionLevel = ENCRYPTION_LEVEL_HIGH;
            else if(paramencryptionLevel == "low")
                encryptionLevel = ENCRYPTION_LEVEL_LOW;
            else if(paramencryptionLevel == "fips")
                encryptionLevel = ENCRYPTION_LEVEL_FIPS;
            else
                encryptionLevel = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;

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

            peer->PostConnect = Connector::RDP::cbServerPostConnect;
            peer->Activate = Connector::RDP::cbServerActivate;
            peer->Close = Connector::RDP::cbServerClose;
            peer->Disconnect = Connector::RDP::cbServerDisconnect;
            peer->Capabilities = Connector::RDP::cbServerCapabilities;
            peer->AdjustMonitorsLayout = Connector::RDP::cbServerAdjustMonitorsLayout;
            peer->ClientCapabilities = Connector::RDP::cbServerClientCapabilities;

            peer->input->KeyboardEvent = Connector::RDP::cbServerKeyboardEvent;
            peer->input->MouseEvent = Connector::RDP::cbServerMouseEvent;
            peer->update->RefreshRect = Connector::RDP::cbServerRefreshRect;
            peer->update->SuppressOutput = Connector::RDP::cbServerSuppressOutput;

            if(1 != peer->Initialize(peer))
            {
                Application::error("%s: failed", "peer->Initialize");
                throw EXIT_FAILURE;
            }
        }

        ~FreeRdpCallback()
        {
            if(stopEvent)
                CloseHandle(stopEvent);

            if(peer)
            {
                freerdp_peer_context_free(peer);
                freerdp_peer_free(peer);
            }
        }

        void stopEventLoop(void)
        {
            if(stopEvent)
            {
                Application::info("%s: stop event", "FreeRdpCallback");
                SetEvent(stopEvent);
            }
        }

        bool isShutdown(void) const
        {
            return ! stopEvent;
        }

        static bool enterEventLoop(FreeRdpCallback* rdp)
        {
            Application::info("%s: enter event loop", "FreeRdpCallback");
            auto peer = rdp->peer;

            // freerdp client events
            while(true)
            {
                if(rdp->isShutdown())
                    break;

                if(peer->CheckFileDescriptor(peer) != TRUE)
                    break;

                if(WTSVirtualChannelManagerCheckFileDescriptor(rdp->context->vcm) != TRUE)
                    break;

                if(WaitForSingleObject(rdp->stopEvent, 1) == WAIT_OBJECT_0)
                    break;

                // wait
                std::this_thread::sleep_for(1ms);
            }

            if(rdp->stopEvent)
            {
                // for shutdown flag
                CloseHandle(rdp->stopEvent);
                rdp->stopEvent = nullptr;
            }

            peer->Disconnect(peer);
            Application::info("%s: loop shutdown", "FreeRdpCallback");
            return true;
        }
    };

    /* Connector::RDP */
    Connector::RDP::RDP(const JsonObject & jo) : SignalProxy(jo, "rdp")
    {
    }

    Connector::RDP::~RDP()
    {
        if(0 < displayNum())
        {
            busConnectorTerminated(displayNum());
            disconnectedEvent();
        }
    }


    int Connector::RDP::communication(void)
    {
        if(0 >= busGetServiceVersion())
        {
            Application::error("%s: failed", "bus service");
            return EXIT_FAILURE;
        }

        auto home = Connector::homeRuntime();
        const auto socketFile = std::filesystem::path(home) / std::string("rdp_pid").append(std::to_string(getpid()));

        if(! proxyInitUnixSockets(socketFile))
            return EXIT_FAILURE;

        Application::info("%s: remote addr: %s", __FUNCTION__, _remoteaddr.c_str());
        proxyStartEventLoop();
        // create FreeRdpCallback
        Application::info("%s: %s", __FUNCTION__, "create freerdp context");
        freeRdp.reset(new FreeRdpCallback(proxyClientSocket(), _remoteaddr, *_config, this));
        auto freeRdpThread = std::thread([ptr = freeRdp.get()] { FreeRdpCallback::enterEventLoop(ptr); });
        damageRegion.assign(0, 0, 0, 0);
        // rdp session not activated trigger
        auto timerNotActivated = Tools::BaseTimer::create<std::chrono::seconds>(30, false, [this]()
        {
            if(this->freeRdp && this->freeRdp->context && ! this->freeRdp->context->activated)
            {
                Application::error("session timeout trigger: %s", "not activated");
                this->loopShutdownFlag = true;
            }
        });
        bool nodamage = _config->getBoolean("xcb:nodamage", false);

        // all ok
        while(! loopShutdownFlag)
        {
            if(freeRdp->isShutdown() || ! proxyRunning())
                loopShutdownFlag = true;

            if(isAllowXcbMessages())
            {
                if(auto err = _xcbDisplay->hasError())
                {
                    setEnableXcbMessages(false);
                    Application::error("xcb display error connection: %d", err);
                    break;
                }

                // xcb processing
                if(! xcbEventLoopAsync(nodamage))
                    loopShutdownFlag = true;
            }

            // wait
            std::this_thread::sleep_for(1ms);
        }

        proxyShutdown();
        freeRdp->stopEventLoop();

        channelsFree();
        timerNotActivated->stop();

        if(freeRdpThread.joinable()) freeRdpThread.join();

        return EXIT_SUCCESS;
    }

    bool Connector::RDP::xcbEventLoopAsync(bool nodamage)
    {
        // get all damages and join it
        while(auto ev = _xcbDisplay->poolEvent())
        {
            int shmOpcode = _xcbDisplay->eventErrorOpcode(ev, XCB::Module::SHM);

            if(0 <= shmOpcode)
            {
                _xcbDisplay->extendedError(ev.toerror(), __FUNCTION__, "");
                return false;
            }

            if(_xcbDisplay->isDamageNotify(ev))
            {
                auto notify = reinterpret_cast<xcb_damage_notify_event_t*>(ev.get());
                damageRegion.join(notify->area);
            }
            else if(_xcbDisplay->isRandrCRTCNotify(ev))
            {
                auto notify = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                xcb_randr_crtc_change_t cc = notify->u.cc;

                if(0 < cc.width && 0 < cc.height)
                {
                    busDisplayResized(displayNum(), cc.width, cc.height);
                    damageRegion.reset();
                    desktopResizeEvent(*freeRdp->peer, cc.width, cc.height);
                }
            }
            else if(_xcbDisplay->isSelectionNotify(ev))
            {
                // FIXME: rdp inform
                //auto notify = reinterpret_cast<xcb_selection_notify_event_t*>(ev.get());
                //if(_xcbDisplay->selectionNotifyAction(notify))
                //    selbuf = _xcbDisplay->getSelectionData();
            }
        }

        if(nodamage)
            damageRegion = _xcbDisplay->region();
        else if(! damageRegion.empty())
            // fix out of screen
            damageRegion = _xcbDisplay->region().intersected(damageRegion.align(4));

        if(! damageRegion.empty() && freeRdp->context && freeRdp->context->activated)
        {
            updatePartFlag = true;

            try
            {
                if(updateEvent(damageRegion))
                {
                    _xcbDisplay->damageSubtrack(damageRegion);
                    damageRegion.reset();
                }
            }
            catch(const std::exception & err)
            {
                Application::error("xcb exception: %s", err.what());
                return false;
            }
            catch(...)
            {
                Application::error("xcb exception: %s", "unknown");
                return false;
            }

            updatePartFlag = false;
        }

        return true;
    }

    void Connector::RDP::setEncryptionInfo(const std::string & info)
    {
        busSetEncryptionInfo(displayNum(), info);
    }

    void Connector::RDP::setAutoLogin(const std::string & login, const std::string & pass)
    {
        helperSetSessionLoginPassword(displayNum(), login, pass, false);
    }

    bool Connector::RDP::createX11Session(uint8_t depth)
    {
        int screen = busStartLoginSession(depth, _remoteaddr, "rdp");
        if(screen <= 0)
        {
            Application::error("%s", "login session request failure");
            return false;
        }

        Application::debug("login session request success, display: %d", screen);

        if(! xcbConnect(screen))
        {
            Application::error("%s", "xcb connect failed");
            return false;
        }

        const xcb_visualtype_t* visual = _xcbDisplay->visual();

        if(! visual)
        {
            Application::error("%s", "xcb visual empty");
            return false;
        }

        Application::info("xcb max request: %d", _xcbDisplay->getMaxRequest());
        // init server format
        serverFormat = PixelFormat(_xcbDisplay->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);

        // wait widget started signal(onHelperWidgetStarted), 3000ms, 10 ms pause
        if(! Tools::waitCallable<std::chrono::milliseconds>(3000, 10,
                [=]() { return ! this->helperStartedFlag; }))
        {
            Application::error("connector starting: %s", "something went wrong...");
            return false;
        }
        std::this_thread::sleep_for(50ms);
        return true;
    }

    void Connector::RDP::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t& userUid)
    {
        if(0 < displayNum() && display == displayNum())
        {
            // disable xcb messages processing
            setEnableXcbMessages(false);

            // wait client update canceled, 1000ms, 10 ms pause
            if(updatePartFlag)
                Tools::waitCallable<std::chrono::milliseconds>(1000, 10, [=]()
            {
                return !!this->updatePartFlag;
            });
            // switch display
            SignalProxy::onLoginSuccess(display, userName, userUid);
            // update context
            setEnableXcbMessages(true);
            // fix new session size
            auto wsz = _xcbDisplay->size();

            if(wsz.width != freeRdp->peer->settings->DesktopWidth || wsz.height != freeRdp->peer->settings->DesktopHeight)
            {
                Application::warning("%s: remote request desktop size [%dx%d], display: %d", __FUNCTION__, freeRdp->peer->settings->DesktopWidth, freeRdp->peer->settings->DesktopHeight, displayNum());

                if(_xcbDisplay->setRandrScreenSize(freeRdp->peer->settings->DesktopWidth, freeRdp->peer->settings->DesktopHeight))
                {
                    wsz = _xcbDisplay->size();
                    Application::info("change session size [%d,%d], display: %d", wsz.width, wsz.height, displayNum());
                }
            }

            // full update
            _xcbDisplay->damageAdd(_xcbDisplay->region());
            Application::info("dbus signal: login success, display: %d, username: %s", displayNum(), userName.c_str());
        }
    }

    void Connector::RDP::onShutdownConnector(const int32_t & display)
    {
        if(0 < displayNum() && display == displayNum())
        {
            freeRdp->stopEventLoop();
            setEnableXcbMessages(false);
            loopShutdownFlag = true;
            Application::info("dbus signal: shutdown connector, display: %d", display);
        }
    }

    void Connector::RDP::onSendBellSignal(const int32_t & display)
    {
        if(0 < displayNum() && display == displayNum() &&
           freeRdp && freeRdp->peer && freeRdp->peer->settings && freeRdp->peer->settings->SoundBeepsEnabled)
        {
            // FIXME beep
        }
    }

    void Connector::RDP::onHelperWidgetStarted(const int32_t & display)
    {
        if(0 < displayNum() && display == displayNum())
        {
            helperStartedFlag = true;
            Application::info("dbus signal: helper started, display: %d", display);
        }
    }

    // client events
    void Connector::RDP::disconnectedEvent(void)
    {
        Application::warning("RDP disconnected, display: %d", displayNum());
    }

    void Connector::RDP::desktopResizeEvent(freerdp_peer & peer, uint16_t width, uint16_t height)
    {
        Application::debug("%s: [%d,%d]", __FUNCTION__, width, height);
        auto context = static_cast<ServerContext*>(peer.context);
        context->activated = false;
        peer.settings->DesktopWidth = width;
        peer.settings->DesktopHeight = height;

        if(peer.update->DesktopResize(peer.update->context))
            Application::error("%s: [%d,%d] failed", __FUNCTION__, width, height);
    }

    bool Connector::RDP::updateEvent(const XCB::Region & reg)
    {
        auto context = static_cast<ServerContext*>(freeRdp->peer->context);
        auto reply = _xcbDisplay->copyRootImageRegion(reg);

        // reply info dump
        if(Application::isDebugLevel(DebugLevel::SyslogDebug))
        {
            Application::info("get_image: request size: [%d,%d], reply length: %d, bits per pixel: %d, red: %08x, green: %08x, blue: %08x",
                                  reg.width, reg.height, reply->size(), reply->bitsPerPixel(), reply->rmask, reply->gmask, reply->bmask);
        }

        FrameBuffer frameBuffer(reply->data(), reg, serverFormat);
        // apply render primitives
        renderPrimitivesToFB(frameBuffer);

        return 24 == reply->bitsPerPixel() ?
               updateBitmapPlanar(reg, reply) : updateBitmapInterleaved(reg, reply);
    }

    bool Connector::RDP::updateBitmapPlanar(const XCB::Region & reg, const XCB::PixmapInfoReply & reply)
    {
        auto context = static_cast<ServerContext*>(freeRdp->peer->context);
        const size_t scanLineBytes = reg.width * reply->bytePerPixel();
        const size_t tileSize = 64;
        const size_t pixelFormat = freeRdp->peer->settings->OsMajorType == 6 ? PIXEL_FORMAT_RGBX32 : PIXEL_FORMAT_BGRX32;

        if(reply->size() != reg.height * reg.width * reply->bytePerPixel())
        {
            Application::error("%s: %s failed, length: %d, size: [%d,%d], bpp: %d", __FUNCTION__, "align region", reply->size(), reg.height, reg.width, reply->bytePerPixel());
            throw rdp_error(NS_FuncName);
        }

        // planar activate
        if(! context->planar)
        {
            DWORD planarFlags = PLANAR_FORMAT_HEADER_RLE;

            if(freeRdp->peer->settings->DrawAllowSkipAlpha)
                planarFlags |= PLANAR_FORMAT_HEADER_NA;

            context->planar = freerdp_bitmap_planar_context_new(planarFlags, tileSize, tileSize);

            if(! context->planar)
            {
                Application::error("%s: %s failed", __FUNCTION__, "bitmap_planar_context_new");
                throw rdp_error(NS_FuncName);
            }
        }

        if(! freerdp_bitmap_planar_context_reset(context->planar, tileSize, tileSize))
        {
            Application::error("%s: %s failed", __FUNCTION__, "bitmap_planar_context_reset");
            throw rdp_error(NS_FuncName);
        }

        Application::debug("%s: area [%d,%d,%d,%d], bits per pixel: %d, scanline: %d", __FUNCTION__, reg.x, reg.y, reg.width, reg.height, reply->bitsPerPixel(), scanLineBytes);
        auto blocks = reg.divideBlocks(XCB::Size(tileSize, tileSize));
        // Compressed header of bitmap
        // http://msdn.microsoft.com/en-us/library/cc240644.aspx
        const size_t hdrsz = 34;
        std::vector<BITMAP_DATA> vec;
        vec.reserve(blocks.size());

        for(auto & subreg : blocks)
        {
            const int16_t localX = subreg.x - reg.x;
            const int16_t localY = subreg.y - reg.y;
            const size_t offset = localY * scanLineBytes + localX * reply->bytePerPixel();
            BITMAP_DATA st = {0};
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

            if(freeRdp->peer->settings->MultifragMaxRequestSize < st.cbCompMainBodySize + hdrsz)
            {
                Application::error("%s: %s failed", __FUNCTION__, "MultifragMaxRequestSize");
                throw rdp_error(NS_FuncName);
            }

            vec.emplace_back(st);
        }

        auto it1 = vec.begin();

        while(it1 != vec.end())
        {
            // calc blocks
            size_t totalSize = 0;
            auto it2 = std::find_if(it1, vec.end(), [&](auto & st)
            {
                if(totalSize + (st.cbCompMainBodySize + hdrsz) > freeRdp->peer->settings->MultifragMaxRequestSize)
                    return true;

                totalSize += (st.cbCompMainBodySize + hdrsz);
                return false;
            });
            BITMAP_UPDATE bitmapUpdate = {0};
            bitmapUpdate.count = bitmapUpdate.number = std::distance(it1, it2);
            bitmapUpdate.rectangles = & (*it1);

            if(! freeRdp->peer->update->BitmapUpdate(context, &bitmapUpdate))
            {
                Application::error("%s: %s failed, length: %d", __FUNCTION__, "BitmapUpdate", totalSize);
                throw rdp_error(NS_FuncName);
            }

            it1 = it2;
        }

        for(auto & st : vec)
            std::free(st.bitmapDataStream);

        return true;
    }

    bool Connector::RDP::updateBitmapInterleaved(const XCB::Region & reg, const XCB::PixmapInfoReply & reply)
    {
        auto context = static_cast<ServerContext*>(freeRdp->peer->context);
        const size_t scanLineBytes = reg.width * reply->bytePerPixel();
        // size fixed: libfreerdp/codec/interleaved.c
        const size_t tileSize = 64;

        if(reply->size() != reg.height * reg.width * reply->bytePerPixel())
        {
            Application::error("%s: %s failed, length: %d, size: [%d,%d], bpp: %d", __FUNCTION__, "align region", reply->size(), reg.height, reg.width, reply->bytePerPixel());
            throw rdp_error(NS_FuncName);
        }

        size_t pixelFormat = 0;

        switch(reply->bitsPerPixel())
        {
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
        if(! context->interleaved)
        {
            BOOL compressor = TRUE;
            context->interleaved = bitmap_interleaved_context_new(compressor);

            if(! context->interleaved)
            {
                Application::error("%s: %s failed", __FUNCTION__, "bitmap_interleaved_context_new");
                throw rdp_error(NS_FuncName);
            }
        }

        if(! bitmap_interleaved_context_reset(context->interleaved))
        {
            Application::error("%s: %s failed", __FUNCTION__, "bitmap_interleaved_context_reset");
            throw rdp_error(NS_FuncName);
        }

        Application::debug("%s: area [%d,%d,%d,%d], bits per pixel: %d, scanline: %d", __FUNCTION__, reg.x, reg.y, reg.width, reg.height, reply->bitsPerPixel(), scanLineBytes);
        auto blocks = reg.divideBlocks(XCB::Size(tileSize, tileSize));
        // Compressed header of bitmap
        // http://msdn.microsoft.com/en-us/library/cc240644.aspx
        BITMAP_DATA st = {0};
        // full size reserved
        auto data = std::make_unique<uint8_t[]>(tileSize * tileSize * 4);

        for(auto & subreg : blocks)
        {
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
                                      reply->data() + offset, pixelFormat, scanLineBytes, 0, 0, NULL, reply->bitsPerPixel()))
            {
                Application::error("%s: %s failed", __FUNCTION__, "interleaved_compress");
                throw rdp_error(NS_FuncName);
            }

            st.bitmapDataStream = data.get();
            st.cbCompMainBodySize = st.bitmapLength;

            if(freeRdp->peer->settings->MultifragMaxRequestSize < st.bitmapLength + 22)
            {
                Application::error("%s: %s failed", __FUNCTION__, "MultifragMaxRequestSize");
                throw rdp_error(NS_FuncName);
            }

            BITMAP_UPDATE bitmapUpdate = {0};
            bitmapUpdate.count = bitmapUpdate.number = 1;
            bitmapUpdate.rectangles = & st;
            auto ret = freeRdp->peer->update->BitmapUpdate(context, &bitmapUpdate);

            if(! ret)
            {
                Application::error("%s: %s failed", __FUNCTION__, "BitmapUpdate");
                throw rdp_error(NS_FuncName);
            }
        }

        return true;
    }

    bool Connector::RDP::channelsInit(void)
    {
        if(freeRdp->context->clipboard &&
            WTSVirtualChannelManagerIsChannelJoined(freeRdp->context->vcm, CLIPRDR_SVC_CHANNEL_NAME))
        {
            // freeRdp->context->cliprdr = cliprdr_server_context_new(freeRdp->context->vcm);
        }

        return true;
    }

    void Connector::RDP::channelsFree(void)
    {
        if(freeRdp->context->cliprdr)
        {
            // cliprdr_server_context_free(freeRdp->context->cliprdr);
            freeRdp->context->cliprdr = nullptr;
        }
    }

    // freerdp callback func
    BOOL Connector::RDP::cbServerAuthenticate(freerdp_peer* peer, const char** user, const char** domain, const char** password)
    {
        Application::info("%s: peer:%p", __FUNCTION__, peer);
        return TRUE;
    }

    BOOL Connector::RDP::cbServerCapabilities(freerdp_peer* peer)
    {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth, peer->settings->DesktopHeight, peer->settings->ColorDepth);

        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->rdp;

        if(! connector->createX11Session(24))
        {
            Application::error("%s: X11 failed", __FUNCTION__);
            return FALSE;
        }

        if(! connector->_xcbDisplay)
            return FALSE;

        peer->settings->ColorDepth = connector->_xcbDisplay->bitsPerPixel();

        return TRUE;
    }

    BOOL Connector::RDP::cbServerAdjustMonitorsLayout(freerdp_peer* peer)
    {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth, peer->settings->DesktopHeight, peer->settings->ColorDepth);
        return TRUE;
    }

    BOOL Connector::RDP::cbServerClientCapabilities(freerdp_peer* peer)
    {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth, peer->settings->DesktopHeight, peer->settings->ColorDepth);

        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->rdp;

        //peer->settings->ColorDepth = connector->_xcbDisplay->bitsPerPixel();
        //peer->settings->ColorDepth = 32;

//        if(peer->settings->ColorDepth == 15 || peer->settings->ColorDepth == 16)
//            context->lowcolor = true;

        return TRUE;
    }

    BOOL Connector::RDP::cbServerPostConnect(freerdp_peer* peer)
    {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth, peer->settings->DesktopHeight, peer->settings->ColorDepth);
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->rdp;

        auto wsz = connector->_xcbDisplay->size();
        if(wsz.width != peer->settings->DesktopWidth || wsz.height != peer->settings->DesktopHeight)
        {
            Application::warning("%s: remote request desktop size [%dx%d], display: %d", __FUNCTION__, peer->settings->DesktopWidth, peer->settings->DesktopHeight, connector->displayNum());

            if(! connector->_xcbDisplay->setRandrScreenSize(peer->settings->DesktopWidth, peer->settings->DesktopHeight))
                Application::error("%s: x11display set size: failed", __FUNCTION__);

            auto wsz = connector->_xcbDisplay->size();

            if(wsz.width != peer->settings->DesktopWidth || wsz.height != peer->settings->DesktopHeight)
                Application::warning("%s: x11display size: [%d,%d]", __FUNCTION__, wsz.width, wsz.height);

            peer->settings->DesktopWidth = wsz.width;
            peer->settings->DesktopHeight = wsz.height;
            peer->update->DesktopResize(peer->update->context);
        }

        if(! connector->channelsInit())
            return FALSE;

        return TRUE;
    }

    BOOL Connector::RDP::cbServerClose(freerdp_peer* peer)
    {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth, peer->settings->DesktopHeight, peer->settings->ColorDepth);
        return TRUE;
    }

    void Connector::RDP::cbServerDisconnect(freerdp_peer* peer)
    {
        Application::info("%s: peer: %p, desktop: [%d,%d], peer depth: %d", __FUNCTION__, peer, peer->settings->DesktopWidth, peer->settings->DesktopHeight, peer->settings->ColorDepth);
    }

    BOOL Connector::RDP::cbServerActivate(freerdp_peer* peer)
    {
        Application::info("%s: peer:%p", __FUNCTION__, peer);
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->rdp;

        if(1)
        {
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
            Application::info("peer settings: %s: %s", "CompressionEnabled", (peer->settings->CompressionEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "RemoteFxCodec", (peer->settings->RemoteFxCodec ? "true" : "false"));
            Application::info("peer settings: %s: %s", "NSCodec", (peer->settings->NSCodec ? "true" : "false"));
            Application::info("peer settings: %s: %s", "JpegCodec", (peer->settings->JpegCodec ? "true" : "false"));
            Application::info("peer settings: %s: %s", "FrameMarkerCommandEnabled", (peer->settings->FrameMarkerCommandEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "SurfaceFrameMarkerEnabled", (peer->settings->SurfaceFrameMarkerEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "SurfaceCommandsEnabled", (peer->settings->SurfaceCommandsEnabled ? "true" : "false"));
            Application::info("peer settings: %s: %s", "FastPathInput", (peer->settings->FastPathInput ? "true" : "false"));
            Application::info("peer settings: %s: %s", "FastPathOutput", (peer->settings->FastPathOutput ? "true" : "false"));
            Application::info("peer settings: %s: %s", "UnicodeInput", (peer->settings->UnicodeInput ? "true" : "false"));
            Application::info("peer settings: %s: %s", "BitmapCacheEnabled", (peer->settings->BitmapCacheEnabled ? "true" : "false"));
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

        if(0 < peer->settings->TlsSecLevel)
            encryptionInfo.append("TLS security level: ").append(std::to_string(peer->settings->TlsSecLevel));

        const char* method = nullptr;

        switch(peer->settings->EncryptionMethods)
        {
            case ENCRYPTION_METHOD_40BIT:
                method = "40bit";
                break;

            case ENCRYPTION_METHOD_56BIT:
                method = "56bit";
                break;

            case ENCRYPTION_METHOD_128BIT:
                method = "128bit";
                break;

            case ENCRYPTION_METHOD_FIPS:
                method = "fips";
                break;

            default:
                break;
        }

        if(method)
        {
            if(encryptionInfo.size()) encryptionInfo.append(", ");

            encryptionInfo.append("RDP method: ").append(method);
        }

        if(encryptionInfo.size())
            connector->setEncryptionInfo(encryptionInfo);

        context->activated = TRUE;
        connector->setEnableXcbMessages(true);

        if(peer->settings->Username)
        {
            std::string user, pass;
            user.assign(peer->settings->Username);

            if(peer->settings->Password)
                pass.assign(peer->settings->Password);

            if(user == pass)
                pass.clear();

            connector->setAutoLogin(user, pass);
        }

        const XCB::Region damage(0, 0, peer->settings->DesktopWidth, peer->settings->DesktopHeight);
        connector->_xcbDisplay->damageAdd(damage);
        return TRUE;
    }

    /// @param flags: KBD_FLAGS_EXTENDED(0x0100), KBD_FLAGS_EXTENDED1(0x0200), KBD_FLAGS_DOWN(0x4000), KBD_FLAGS_RELEASE(0x8000)
    /// @see:  freerdp/input.h
    BOOL Connector::RDP::cbServerKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code)
    {
        Application::debug("%s: flags:0x%04X, code:0x%04X, input:%p, context:%p", __FUNCTION__, flags, code, input, input->context);
        auto context = static_cast<ServerContext*>(input->context);
        auto connector = context->rdp;

        if(connector->isAllowXcbMessages())
        {
            uint32_t keysym = static_cast<uint32_t>(flags) << 16 | code;

            // local keymap priority "rdp:keymap:file"
            if(auto value = (context->keymap ? context->keymap->getValue(Tools::hex(keysym, 8)) : nullptr))
            {
                // no wait xcb replies
                if(value->isArray())
                {
                    auto ja = static_cast<const JsonArray*>(value);

                    for(auto & val : ja->toStdVector<int>())
                        connector->_xcbDisplay->fakeInputTest(flags & KBD_FLAGS_DOWN ? XCB_KEY_PRESS : XCB_KEY_RELEASE, val, 0, 0);
                }
                else
                    connector->_xcbDisplay->fakeInputTest(flags & KBD_FLAGS_DOWN ? XCB_KEY_PRESS : XCB_KEY_RELEASE, value->getInteger(), 0, 0);
            }
            else
            {
                // see winpr/input.h
                // KBDEXT(0x0100), KBDMULTIVK(0x0200), KBDSPECIAL(0x0400), KBDNUMPAD(0x0800),
                // KBDUNICODE(0x1000), KBDINJECTEDVK(0x2000), KBDMAPPEDVK(0x4000), KBDBREAK(0x8000)
                if(flags & KBD_FLAGS_EXTENDED)
                    code |= KBDEXT;

                // winpr: input
                auto vkcode = GetVirtualKeyCodeFromVirtualScanCode(code, 4);
                auto keycode = GetKeycodeFromVirtualKeyCode((flags & KBD_FLAGS_EXTENDED ? vkcode | KBDEXT : vkcode), KEYCODE_TYPE_EVDEV);
                connector->_xcbDisplay->fakeInputTest(flags & KBD_FLAGS_DOWN ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, 0, 0);
            }
        }

        return TRUE;
    }

    /// @param flags: PTR_FLAGS_BUTTON1(0x1000), PTR_FLAGS_BUTTON2(0x2000), PTR_FLAGS_BUTTON3(0x4000), PTR_FLAGS_HWHEEL(0x0400),
    ///               PTR_FLAGS_WHEEL(0x0200), PTR_FLAGS_WHEEL_NEGATIVE(0x0100), PTR_FLAGS_MOVE(0x0800), PTR_FLAGS_DOWN(0x8000)
    /// @see:  freerdp/input.h
    BOOL Connector::RDP::cbServerMouseEvent(rdpInput* input, UINT16 flags, UINT16 posx, UINT16 posy)
    {
        Application::debug("%s: flags:0x%04X, pos:%d,%d, input:%p, context:%p", __FUNCTION__, flags, posx, posy, input, input->context);
        auto context = static_cast<ServerContext*>(input->context);
        auto connector = context->rdp;

        if(connector->isAllowXcbMessages())
        {
            // left button
            if(flags & PTR_FLAGS_BUTTON1)
                connector->_xcbDisplay->fakeInputTest(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, XCB_BUTTON_INDEX_1, posx, posy);
            else

                // right button
                if(flags & PTR_FLAGS_BUTTON2)
                    connector->_xcbDisplay->fakeInputTest(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, XCB_BUTTON_INDEX_3, posx, posy);
                else

                    // middle button
                    if(flags & PTR_FLAGS_BUTTON3)
                        connector->_xcbDisplay->fakeInputTest(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, XCB_BUTTON_INDEX_2, posx, posy);
                    else if(flags & PTR_FLAGS_WHEEL)
                        connector->_xcbDisplay->fakeInputTest(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, flags & PTR_FLAGS_WHEEL_NEGATIVE ? XCB_BUTTON_INDEX_5 : XCB_BUTTON_INDEX_4, posx, posy);

            if(flags & PTR_FLAGS_MOVE)
                connector->_xcbDisplay->fakeInputTest(XCB_MOTION_NOTIFY, 0, posx, posy);
        }

        return TRUE;
    }

    BOOL Connector::RDP::cbServerRefreshRect(rdpContext* rdpctx, BYTE count, const RECTANGLE_16* areas)
    {
        Application::debug("%s: count rects:%d, context:%p", __FUNCTION__, (int) count, rdpctx);
        auto context = static_cast<ServerContext*>(rdpctx);
        auto connector = context->rdp;
        std::vector<xcb_rectangle_t> rectangles(0 < count ? count : 1);

        if(count && areas)
        {
            for(int it = 0; it < count; ++it)
            {
                rectangles[it].x = areas[it].left;
                rectangles[it].y = areas[it].top;
                rectangles[it].width = areas[it].right - areas[it].left + 1;
                rectangles[it].height = areas[it].bottom - areas[it].top + 1;
            }
        }
        else
        {
            auto wsz = connector->_xcbDisplay->size();
            rectangles[0].x = 0;
            rectangles[0].y = 0;
            rectangles[0].width = wsz.width;
            rectangles[0].height = wsz.height;
        }

        return connector->_xcbDisplay->damageAdd(rectangles.data(), rectangles.size());
    }

    BOOL Connector::RDP::cbServerSuppressOutput(rdpContext* rdpctx, BYTE allow, const RECTANGLE_16* area)
    {
        auto context = static_cast<ServerContext*>(rdpctx);
        auto connector = context->rdp;

        if(area && 0 < allow)
        {
            Application::debug("%s: peer restore output(left:%d,top:%d,right:%d,bottom:%d)", __FUNCTION__, area->left, area->top, area->right, area->bottom);
            connector->setEnableXcbMessages(true);
            connector->_xcbDisplay->damageAdd(connector->_xcbDisplay->region());
        }
        else
        {
            Application::debug("%s: peer minimized and suppress output", __FUNCTION__);
            connector->setEnableXcbMessages(false);
        }

        return TRUE;
    }
}
