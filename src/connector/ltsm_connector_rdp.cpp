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
using namespace boost;

#define FREERDP_VERSION_NUMBER ((FREERDP_VERSION_MAJOR << 16) | (FREERDP_VERSION_MINOR << 8) | FREERDP_VERSION_REVISION)

namespace LTSM::Connector {
    struct ServerContext : rdpContext {
        BITMAP_PLANAR_CONTEXT* planar = nullptr;
        BITMAP_INTERLEAVED_CONTEXT* interleaved = nullptr;
        HANDLE vcm = nullptr;

        bool activated = false;
        bool clipboard = false;
        size_t frameId = 0;

        ConnectorRdp* connector = nullptr;
    };

    int ServerContextNew(rdp_freerdp_peer* peer, ServerContext* context) {
        context->planar = nullptr;
        context->interleaved = nullptr;
        context->vcm = WTSOpenServerA((LPSTR) peer->context);

        if(! context->vcm || context->vcm == INVALID_HANDLE_VALUE) {
            Application::error("{}: {} failed", NS_FuncNameV, "WTSOpenServer");
            return FALSE;
        }

        context->activated = false;
        context->clipboard = true;
        context->frameId = 0;
        context->connector = nullptr;

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
    }

    // freerdp callback func
    BOOL rdpServerCapabilitiesCb(freerdp_peer* peer) {
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->connector;

#if defined(FREERDP3_API)
        return connector && connector->serverCapabilitiesEvent(peer->context->settings);
#else
        return connector && connector->serverCapabilitiesEvent(peer->settings);
#endif
    }

    BOOL rdpServerActivateCb(freerdp_peer* peer) {
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->connector;

#if defined(FREERDP3_API)
        return connector && connector->serverActivateEvent(peer->context->settings);
#else
        return connector && connector->serverActivateEvent(peer->settings);
#endif
    }

    BOOL rdpServerAdjustMonitorsLayoutCb(freerdp_peer* peer) {
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->connector;

#if defined(FREERDP3_API)
        return connector && connector->serverAdjustMonitorsEvent(peer->context->settings);
#else
        return connector && connector->serverAdjustMonitorsEvent(peer->settings);
#endif
    }

    BOOL rdpServerClientCapabilitiesCb(freerdp_peer* peer) {
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->connector;

#if defined(FREERDP3_API)
        return connector && connector->clientCapabilitiesEvent(peer->context->settings);
#else
        return connector && connector->clientCapabilitiesEvent(peer->settings);
#endif
    }

    BOOL rdpServerPostConnectCb(freerdp_peer* peer) {
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->connector;

#if defined(FREERDP3_API)
        return connector && connector->serverPostConnectEvent(peer->context->settings);
#else
        return connector && connector->serverPostConnectEvent(peer->settings);
#endif
    }

    BOOL rdpServerCloseCb(freerdp_peer* peer) {
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->connector;

        return connector && connector->serverCloseEvent();
    }

    void rdpServerDisconnectCb(freerdp_peer* peer) {
        auto context = static_cast<ServerContext*>(peer->context);
        auto connector = context->connector;

        if(connector) {
            connector->serverDisconnectEvent();
        }
    }

    /// @param flags: KBD_FLAGS_EXTENDED(0x0100), KBD_FLAGS_EXTENDED1(0x0200), KBD_FLAGS_DOWN(0x4000), KBD_FLAGS_RELEASE(0x8000)
    /// @see:  freerdp/input.h
#if defined(FREERDP3_API)
    BOOL rdpServerKeyboardEventCb(rdpInput* input, UINT16 flags, BYTE code) {
#else
    BOOL rdpServerKeyboardEventCb(rdpInput* input, UINT16 flags, UINT16 code) {
#endif
        auto context = static_cast<ServerContext*>(input->context);
        auto connector = context->connector;

        return connector && connector->serverKeyboardEvent(flags, code);
    }

    /// @param flags: PTR_FLAGS_BUTTON1(0x1000), PTR_FLAGS_BUTTON2(0x2000), PTR_FLAGS_BUTTON3(0x4000), PTR_FLAGS_HWHEEL(0x0400),
    ///               PTR_FLAGS_WHEEL(0x0200), PTR_FLAGS_WHEEL_NEGATIVE(0x0100), PTR_FLAGS_MOVE(0x0800), PTR_FLAGS_DOWN(0x8000)
    /// @see:  freerdp/input.h
    BOOL rdpServerMouseEventCb(rdpInput* input, UINT16 flags, UINT16 posx, UINT16 posy) {
        auto context = static_cast<ServerContext*>(input->context);
        auto connector = context->connector;

        return connector && connector->serverMouseEvent(flags, posx, posy);
    }

    BOOL rdpServerRefreshRectCb(rdpContext* rdpctx, BYTE count, const RECTANGLE_16* areas) {
        auto context = static_cast<ServerContext*>(rdpctx);
        auto connector = context->connector;

        return connector && connector->serverRefreshEvent(count, areas);
    }

    BOOL rdpServerSuppressOutputCb(rdpContext* rdpctx, BYTE allow, const RECTANGLE_16* area) {
        auto context = static_cast<ServerContext*>(rdpctx);
        auto connector = context->connector;

        return connector && connector->serverSuppressEvent(allow);
    }

    // FreeRdp
    struct FreeRdpEvents {
        freerdp_peer* peer = nullptr;
        ServerContext* context = nullptr;

        FreeRdpEvents(int clientFd, const std::string & remoteaddr, const JsonObject & config,
                        ConnectorRdp* connector)
            : peer(nullptr), context(nullptr) {

            winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
            WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());

            // init freerdp log system
            if(auto log = WLog_GetRoot()) {
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

            Application::info("{}: FreeRDP API version usage: {}, winpr: {}", NS_FuncNameV, FREERDP_VERSION_FULL, WINPR_VERSION_FULL);

            peer = freerdp_peer_new(clientFd);
            if(! peer) {
                Application::error("{}: {} failed", NS_FuncNameV, "freerdp_peer_new");
                throw rdp_error(NS_FuncNameS);
            }

            peer->local = TRUE;
            std::copy_n(remoteaddr.begin(), std::min(sizeof(peer->hostname), remoteaddr.size()), peer->hostname);
            // init context
            peer->ContextSize = sizeof(ServerContext);
            peer->ContextNew = (psPeerContextNew) ServerContextNew;
            peer->ContextFree = (psPeerContextFree) ServerContextFree;

            if(! freerdp_peer_context_new(peer)) {
                Application::error("{}: {} failed", NS_FuncNameV, "freerdp_peer_context_new");
                throw rdp_error(NS_FuncNameS);
            }

            Application::debug(DebugType::App, "{}: peer context: {}, server context: {}",
                    NS_FuncNameV, fmt::ptr(peer), fmt::ptr(peer->context));

            context = static_cast<ServerContext*>(peer->context);
            context->connector = connector;
            context->clipboard = false;

            auto certfile = connector->checkFileOption("rdp:server:certfile");
#if defined(FREERDP3_API)
            auto settings = peer->context->settings;
#else
            auto settings = peer->settings;
#endif

            if(certfile.size()) {
#if defined(FREERDP3_API)
                if(auto cert = freerdp_certificate_new_from_file(certfile.c_str())) {
                    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1);
                    Application::info("{}: server cert: {}", NS_FuncNameV, certfile);
                }
#else
                settings->CertificateFile = strdup(certfile.c_str());
                Application::info("{}: server cert: {}", NS_FuncNameV, certfile);
#endif
            }

            auto keyfile = connector->checkFileOption("rdp:server:keyfile");

            if(keyfile.size()) {
#if defined(FREERDP3_API)
                if(auto key = freerdp_key_new_from_file(keyfile.c_str())) {
                    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1);
                    Application::info("{}: server key: {}", NS_FuncNameV, keyfile);
                }
#else
                settings->PrivateKeyFile = strdup(keyfile.c_str());
                //settings->RdpKeyFile = strdup(keyfile.c_str());
                Application::info("{}: server key: {}", NS_FuncNameV, keyfile);
#endif
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

            peer->PostConnect = rdpServerPostConnectCb;
            peer->Activate = rdpServerActivateCb;
            peer->Close = rdpServerCloseCb;
            peer->Disconnect = rdpServerDisconnectCb;
            peer->Capabilities = rdpServerCapabilitiesCb;
            peer->AdjustMonitorsLayout = rdpServerAdjustMonitorsLayoutCb;
            peer->ClientCapabilities = rdpServerClientCapabilitiesCb;
#if defined(FREERDP3_API)
            peer->context->input->KeyboardEvent = rdpServerKeyboardEventCb;
            peer->context->input->MouseEvent = rdpServerMouseEventCb;
            peer->context->update->RefreshRect = rdpServerRefreshRectCb;
            peer->context->update->SuppressOutput = rdpServerSuppressOutputCb;
#else
            peer->input->KeyboardEvent = rdpServerKeyboardEventCb;
            peer->input->MouseEvent = rdpServerMouseEventCb;
            peer->update->RefreshRect = rdpServerRefreshRectCb;
            peer->update->SuppressOutput = rdpServerSuppressOutputCb;
#endif

            settings->RdpSecurity = config.getBoolean("rdp:security:rdp", true) ? TRUE : FALSE;
            settings->TlsSecurity = config.getBoolean("rdp:security:tls", true) ? TRUE : FALSE;
            settings->NlaSecurity = config.getBoolean("rdp:security:nla", false) ? TRUE : FALSE;
            settings->TlsSecLevel = config.getInteger("rdp:tls:level", 1);
            settings->ExtSecurity = FALSE;
            settings->UseRdpSecurityLayer = FALSE;
            settings->EncryptionLevel = encryptionLevel;
            settings->NSCodec = FALSE;
            settings->RemoteFxCodec = FALSE;
            settings->RefreshRect = TRUE;
            settings->SuppressOutput = TRUE;
            settings->FrameMarkerCommandEnabled = TRUE;
            settings->SurfaceFrameMarkerEnabled = TRUE;

            if(! peer->Initialize(peer)) {
                Application::error("{}: {} failed", NS_FuncNameV, "Peer::Initialize");
                throw rdp_error(NS_FuncNameS);
            }
        }

        ~FreeRdpEvents() {
            if(peer) {
                if(peer->context) {
                    freerdp_peer_context_free(peer);
                }
                freerdp_peer_free(peer);
            }
        }

        void xcbConnected(void) {
        }

        void xcbDisconnected(void) {
        }

        bool isActivated(void) const {
            return context ? context->activated : false;
        }

        bool checkValidEvents(void) const {
            if(! peer->CheckFileDescriptor(peer)) {
                Application::error("{}: {} failed", NS_FuncNameV, "Peer::CheckFileDescriptor");
                return false;
            }

            if(! WTSVirtualChannelManagerCheckFileDescriptor(context->vcm)) {
                Application::error("{}: {} failed", NS_FuncNameV, "WTSVirtualChannelManagerCheckFileDescriptor");
                return false;
            }

            return true;
        }

        asio::awaitable<void> rdpEventsAwait(void) {
            auto ex = co_await asio::this_coro::executor;
            //ConnectorRdp* connector = context->connector;

            try {
                while(checkValidEvents()) {
                    //
                    asio::steady_timer tm_delay{ex, 10ms};
                    co_await tm_delay.async_wait(asio::use_awaitable);
                }
            } catch(const system::system_error& err) {
                if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                    Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                }
            } catch(const std::exception& err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            }

            peer->Disconnect(peer);
            co_return;
        }
    };

    ConnectorRdp::ConnectorRdp(const std::filesystem::path & confile, bool debug)
        : DBusProxy(ConnectorType::RDP, confile, debug)
        , xcb_strand_{asio::make_strand(ioc())}
        , rdp_strand_{asio::make_strand(ioc())}
        , tm_not_activated_{ioc()} {

        if(auto keymapFile = config().getString("rdp:keymap:file"); ! keymapFile.empty()) {
            JsonContentFile jc(keymapFile);

            if(jc.isValid() && jc.isObject()) {
                keymap_ = std::make_unique<JsonObject>(jc.toObject());
                Application::info("{}: keymap loaded: {}, items: {}", NS_FuncNameV, keymapFile, keymap_->size());
            }
        }
    }

    ConnectorRdp::~ConnectorRdp() {
        stop();
    }

    asio::awaitable<void> ConnectorRdp::xcbEventsAwait(void) {
        auto fps_delay = std::chrono::milliseconds(static_cast<uint32_t>(1000 / frameRateOption()));
        auto update_tp = std::chrono::steady_clock::now() + fps_delay;
        try {
            auto ex = co_await asio::this_coro::executor;
            asio::posix::stream_descriptor sd{ex};

            for(;;) {
                if(! xcbAllowMessages()) {
                    asio::steady_timer tm_delay{ex, 50ms};
                    co_await tm_delay.async_wait(asio::use_awaitable);
                    continue;
                }

                if(auto err = RootDisplay::hasError()) {
                    Application::error("{}: xcb error, code: {}", NS_FuncNameV, err);
                    stop();
                    co_return;
                }

                sd.assign(RootDisplay::getFd());
                co_await sd.async_wait(asio::posix::stream_descriptor::wait_read, asio::use_awaitable);

                while(auto ev = RootDisplay::pollEvent()) {
                }

                sd.release();
                auto now = std::chrono::steady_clock::now();

                if(update_tp <= now) {
                    if(update_jobs_.load() <  5) {
                        xcbUpdateDisplay();
                    }
                    update_tp = std::chrono::steady_clock::now() + fps_delay;
                }
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                stop();
            }
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            stop();
        }

        co_return;
    }

    void ConnectorRdp::stop(void) noexcept {
        std::call_once(stopFlag_, [this]() {
            try {
                xcbDisableMessages(true);
                if(0 < displayNum()) {
                    busConnectorTerminated(displayNum(), getpid());
                }
                rdp_events_cancel_.emit(asio::cancellation_type::terminal);
                xcb_events_cancel_.emit(asio::cancellation_type::terminal);
                tm_not_activated_.cancel();
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
        rdpEvents_ = std::make_unique<FreeRdpEvents>(InetStream::fd(), remoteAddress(), config(), this);
        damageRegion_.assign(0, 0, 0, 0);

        x11NoDamage_ = config().getBoolean("rdp:xcb:nodamage", false);
        frameRate_ = config().getInteger("frame:rate", frameRate_);

        xcbDisableMessages(true);

        tm_not_activated_.expires_after(30s);
        tm_not_activated_.async_wait(std::bind(&ConnectorRdp::notActivatedCb, this, std::placeholders::_1));

        asio::co_spawn(rdp_strand_, [ptr=rdpEvents_.get()]() -> asio::awaitable<void> {
            co_await ptr->rdpEventsAwait();
            co_return;
        }, asio::bind_cancellation_slot(rdp_events_cancel_.slot(), asio::detached));

        asio::co_spawn(xcb_strand_, [this]() -> asio::awaitable<void> {
            co_await xcbEventsAwait();
            co_return;
        }, asio::bind_cancellation_slot(xcb_events_cancel_.slot(), asio::detached));

        BoostContext::run();

        rdpChannelsFree();
        return EXIT_SUCCESS;
    }

    void ConnectorRdp::notActivatedCb(const boost::system::error_code & err) {
        if(err) {
            return;
        }

        if(rdpEvents_ && ! rdpEvents_->context->activated) {
            Application::error("{}: timeout trigger", NS_FuncNameV);
            stop();
        }
    }

    bool ConnectorRdp::xcbNoDamageOption(void) const {
        return x11NoDamage_;
    }

    uint32_t ConnectorRdp::frameRateOption(void) const {
        constexpr uint32_t minFps = 5;
        constexpr uint32_t maxFps = 20;
        return std::clamp(frameRate_, minFps, maxFps);
    }

    void ConnectorRdp::xcbUpdateDisplay(void) {
        if(xcbNoDamageOption()) {
            damageRegion_ = RootDisplay::region();
        } else if(damageRegion_.isEmpty()) {
            return;
        } else {
            // fix out of screen
            damageRegion_ = RootDisplay::region().intersected(damageRegion_.align(4));
        }

        auto reply = RootDisplay::copyRootImageRegion(damageRegion_);
        // apply render primitives
        FrameBuffer frameBuffer(reply->data(), damageRegion_, serverPf_);
        renderPrimitivesToFB(frameBuffer);

        Application::debug(DebugType::App, "{}: size: {}, reply length: {}, bpp: {}, red: {:#010x}, green: {:#010x}, blue: {:#010x}",
                       NS_FuncNameV, damageRegion_.toSize(), reply->size(), reply->bitsPerPixel(), reply->rmask, reply->gmask, reply->bmask);

        // send update
        asio::post(rdp_strand_, [this,reg=damageRegion_,reply=std::move(reply)]() {
            update_jobs_.fetch_add(1);
            try {
                // rdpUpdateRegionEvent
                switch(reply->bitsPerPixel()) {
                    case 24:
                    case 32:
                        rdpUpdateBitmapPlanar(reg, reply);
                        break;
                    default:
                        rdpUpdateBitmapInterleaved(reg, reply);
                        break;
                }
            } catch(const std::exception& err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                asio::post(ioc(), std::bind(&ConnectorRdp::stop, this));
            }
            update_jobs_.fetch_sub(1);
        });

        RootDisplay::rootDamageSubtrack(damageRegion_);
        damageRegion_.reset();
    }

    void ConnectorRdp::xcbDamageNotifyEvent(const xcb_rectangle_t & rt, uint8_t level) {
        damageRegion_.join(rt.x, rt.y, rt.width, rt.height);
    }

    void ConnectorRdp::xcbRandrScreenChangedEvent(const XCB::Size & dsz, const xcb_randr_notify_event_t & ne) {
        Application::info("{}: size: {}", NS_FuncNameV, dsz);
        damageRegion_.reset();
        asio::post(rdp_strand_, [this, dsz]() {
            busDisplayResized(displayNum(), dsz.width, dsz.height);
            rdpDesktopResizeEvent(dsz);
        });
    }

    void ConnectorRdp::xcbXkbGroupChangedEvent(int) {
    }

    void ConnectorRdp::xcbKeyboardEvent(uint16_t flags, uint16_t code) {
        if(auto test = static_cast<const XCB::ModuleTest*>(RootDisplay::getExtension(XCB::Module::TEST))) {
            const auto keysym = static_cast<uint32_t>(flags) << 16 | code;

            // local keymap priority "rdp:keymap:file"
            if(auto value = (keymap_ ? keymap_->getValue(Tools::hex(keysym, 8)) : nullptr)) {
                // no wait xcb replies
                if(value->isArray()) {
                    const auto ja = static_cast<const JsonArray*>(value);

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

#if defined(FREERDP3_API)
                constexpr auto type2 = WINPR_KEYCODE_TYPE_EVDEV;
#else
                constexpr auto type2 = KEYCODE_TYPE_EVDEV;
#endif
                // winpr: input
                auto vkcode = GetVirtualKeyCodeFromVirtualScanCode(code, 4);
                auto keycode = GetKeycodeFromVirtualKeyCode((flags & KBD_FLAGS_EXTENDED ? vkcode | KBDEXT : vkcode),
                    type2);
                test->screenInputKeycode(keycode, flags & KBD_FLAGS_DOWN);
            }
        }
    }

    void ConnectorRdp::xcbMouseEvent(uint16_t flags, uint16_t posx, uint16_t posy) {
        if(auto test = static_cast<const XCB::ModuleTest*>(RootDisplay::getExtension(XCB::Module::TEST))) {
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
    }

    void ConnectorRdp::setAutoLogin(const std::string & login, const std::string & pass) {
        helperSetSessionLoginPassword(displayNum(), login, pass, false);
    }

    asio::awaitable<void> ConnectorRdp::createX11SessionAwait(const XCB::Size& csz, uint8_t depth) {
        // session request
        int screen = busStartLoginSession(getpid(), csz.width, csz.height, depth, remoteAddress(), "rdp");

        if(screen <= 0) {
            Application::error("{}: {} failed", NS_FuncNameV, "login session request");
            throw rdp_error(NS_FuncNameS);
        }

        Application::debug(DebugType::App, "{}: login session request success, display: {}", NS_FuncNameV, screen);
        auto xauthFile = busDisplayAuthFile(screen);

        co_await xcbConnectAwait(screen, xauthFile, *this);
        const xcb_visualtype_t* visual = RootDisplay::visual();

        if(! visual) {
            Application::error("{}: xcb visual empty", NS_FuncNameV);
            throw rdp_error(NS_FuncNameS);
        }

        Application::debug(DebugType::Xcb, "{}: xcb max request: {}", NS_FuncNameV, RootDisplay::getMaxRequest());
        // init server format
        serverPf_ = PixelFormat(RootDisplay::bitsPerPixel(),
                                   visual->red_mask, visual->green_mask, visual->blue_mask, 0);

        co_return;
    }

    asio::awaitable<void> ConnectorRdp::waitUpdateProcessAwait(void) {
        auto ex = co_await asio::this_coro::executor;

        while(0 < update_jobs_.load()) {
            asio::steady_timer tm_delay{ex, 1ms};
            co_await tm_delay.async_wait(asio::use_awaitable);
        }

        co_return;
    }

    asio::awaitable<void> ConnectorRdp::onLoginSuccessAwait(std::string userName, uint32_t userUid, XCB::Size csz) {
        xcbDisableMessages(true);
        co_await waitUpdateProcessAwait();

        int oldDisplay = displayNum();
        int newDisplay = busStartUserSession(oldDisplay, getpid(), userName, remoteAddress(), connectorType());

        if(newDisplay < 0) {
            Application::error("{}: {} failed", NS_FuncNameV, "busStartUserSession");
            throw rdp_error(NS_FuncNameS);
        }

        if(newDisplay != oldDisplay) {
            auto xauthFile = busDisplayAuthFile(newDisplay);
            co_await xcbConnectAwait(newDisplay, xauthFile, *this);
            // send later
            asio::post(ioc(), std::bind(&ConnectorRdp::busShutdownDisplay, this, oldDisplay));
        }

        if(auto wsz = RootDisplay::size(); wsz != csz) {
            Application::warning("{}: remote request desktop size: {}, display: {}", NS_FuncNameV,
                                 csz, displayNum());

            if(RootDisplay::setRandrScreenSize(csz)) {
                wsz = RootDisplay::size();
                Application::info("{}: change session size: {}, display: {}", NS_FuncNameV, wsz, displayNum());
            }
        }

        xcbDisableMessages(false);
        serverScreenUpdateRequest(XCB::Region{0,0,csz.width,csz.height});

        auto json = JsonContentString(busGetSessionJson(newDisplay)).toObject();
        setIdleTimeoutSec(json.getInteger("session:idle:timeout", 0));

        busConnectorConnected(newDisplay, getpid());
        co_return;
    }

    void ConnectorRdp::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t & userUid) {
        if(display != displayNum()) {
            return;
        }

        Application::notice("{}: dbus signal, display: {}, username: {}, uid: {}", NS_FuncNameV, display,
                            userName, userUid);

#if defined(FREERDP3_API)
        auto settings = rdpEvents_->peer->context->settings;
#else
        auto settings = rdpEvents_->peer->settings;
#endif

        asio::co_spawn(xcb_strand_, onLoginSuccessAwait(userName, userUid, XCB::Size(settings->DesktopWidth, settings->DesktopHeight)),
            // exit callback
            [this](std::exception_ptr ptr) {
            if(ptr) {
                try {
                    std::rethrow_exception(ptr);
                } catch (const std::exception& err) {
                    Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                    asio::post(ioc(), std::bind(&ConnectorRdp::stop, this));
                }
            }
        });
    }

    void ConnectorRdp::onShutdownConnector(const int32_t & display) {
        if(display == displayNum()) {
            Application::info("{}: dbus signal shutdown, display: {}", NS_FuncNameV, display);
            stop();
        }
    }

    void ConnectorRdp::onSendBellSignal(const int32_t & display) {
#if defined(FREERDP3_API)
        auto settings = rdpEvents_->peer->context->settings;
#else
        auto settings = rdpEvents_->peer->settings;
#endif
        if(display == displayNum() &&
           settings && settings->SoundBeepsEnabled) {
            // FIXME beep
        }
    }

    void ConnectorRdp::serverScreenUpdateRequest(const XCB::Region & reg) {
        if(xcbAllowMessages()) {
            RootDisplay::rootDamageAddRegion(reg);
        }
    }

    bool ConnectorRdp::serverCapabilitiesEvent(rdpSettings* settings) {
        if(xcb_strand_.running_in_this_thread()) {
            std::promise<bool> promise;
            auto future = promise.get_future();
            std::thread([this, settings, prom=std::move(promise)]() mutable {
                prom.set_value(serverCapabilitiesEvent(settings));
            }).detach();
            return future.get();
        }

        Application::info("{}: desktop size: {}, depth: {}",
                         NS_FuncNameV, XCB::Size(settings->DesktopWidth, settings->DesktopHeight), settings->ColorDepth);

        auto csz = XCB::Size(settings->DesktopWidth, settings->DesktopHeight);
        auto res = asio::co_spawn(xcb_strand_, [this, csz, depth=settings->ColorDepth]() -> asio::awaitable<bool> {
            try {
                co_await createX11SessionAwait(csz, depth);
                co_return true;
            } catch(const system::system_error& err) {
                if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                    Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                    asio::post(ioc(), std::bind(&ConnectorRdp::stop, this));
                }
            } catch(const std::exception& err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                asio::post(ioc(), std::bind(&ConnectorRdp::stop, this));
            }
            co_return false;
        }, asio::use_future);

        bool success = res.get();

        if(success) {
            settings->DesktopWidth = RootDisplay::width();
            settings->DesktopHeight = RootDisplay::height();
            settings->ColorDepth = RootDisplay::bitsPerPixel();
        }

        return success;
    }

    inline const char* fmt_cstr(const char* str) {
        return str ? str : "(null)";
    }

    bool ConnectorRdp::serverActivateEvent(const rdpSettings* settings) {
        Application::info("{}: desktop size: {}, depth: {}",
                         NS_FuncNameV, XCB::Size(settings->DesktopWidth, settings->DesktopHeight), settings->ColorDepth);

        if(1) {
            Application::info("{}: settings - {}: {:#010x}", NS_FuncNameV, "RdpVersion", settings->RdpVersion);
            Application::info("{}: settings - {}: {:#06x}", NS_FuncNameV, "OsMajorType", settings->OsMajorType);
            Application::info("{}: settings - {}: {:#06x}", NS_FuncNameV, "OsMinorType", settings->OsMinorType);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "Username", fmt_cstr(settings->Username));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "Domain", fmt_cstr(settings->Domain));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "DesktopWidth", settings->DesktopWidth);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "DesktopHeight", settings->DesktopHeight);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "DesktopColorDepth", settings->ColorDepth);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "peerProductId", settings->ClientProductId);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "AutoLogonEnabled", static_cast<bool>(settings->AutoLogonEnabled));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "CompressionEnabled", static_cast<bool>(settings->CompressionEnabled));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "RemoteFxCodec", static_cast<bool>(settings->RemoteFxCodec));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "NSCodec", static_cast<bool>(settings->NSCodec));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "JpegCodec", static_cast<bool>(settings->JpegCodec));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "FrameMarkerCommandEnabled", static_cast<bool>(settings->FrameMarkerCommandEnabled));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "SurfaceFrameMarkerEnabled", static_cast<bool>(settings->SurfaceFrameMarkerEnabled));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "SurfaceCommandsEnabled", static_cast<bool>(settings->SurfaceCommandsEnabled));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "FastPathInput", static_cast<bool>(settings->FastPathInput));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "FastPathOutput", static_cast<bool>(settings->FastPathOutput));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "UnicodeInput", static_cast<bool>(settings->UnicodeInput));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "BitmapCacheEnabled", static_cast<bool>(settings->BitmapCacheEnabled));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "DesktopResize", static_cast<bool>(settings->DesktopResize));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "RefreshRect", static_cast<bool>(settings->RefreshRect));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "SuppressOutput", static_cast<bool>(settings->SuppressOutput));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "TlsSecurity", static_cast<bool>(settings->TlsSecurity));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "NlaSecurity", static_cast<bool>(settings->NlaSecurity));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "RdpSecurity", static_cast<bool>(settings->RdpSecurity));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "SoundBeepsEnabled", static_cast<bool>(settings->SoundBeepsEnabled));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "AuthenticationLevel", settings->AuthenticationLevel);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "AllowedTlsCiphers", fmt_cstr(settings->AllowedTlsCiphers));
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "TlsSecLevel", settings->TlsSecLevel);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "EncryptionMethods", settings->EncryptionMethods);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "EncryptionLevel", settings->EncryptionLevel);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "CompressionLevel", settings->CompressionLevel);
            Application::info("{}: settings - {}: {}", NS_FuncNameV, "MultifragMaxRequestSize", settings->MultifragMaxRequestSize);
        }

        std::string encryptionInfo;

        if(0 < settings->TlsSecLevel) {
            encryptionInfo = fmt::format("TLS security level: {}", settings->TlsSecLevel);
        }

        switch(settings->EncryptionMethods) {
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
            busSetEncryptionInfo(displayNum(), encryptionInfo);
        }

        rdpEvents_->context->activated = true;
        xcbDisableMessages(false);

        if(settings->Username) {
            std::string user, pass;
            user.assign(settings->Username);

            if(settings->Password) {
                pass.assign(settings->Password);
            }

            if(user == pass) {
                pass.clear();
            }

            setAutoLogin(user, pass);
        }

        RootDisplay::rootDamageAddRegion(XCB::Region(0, 0, settings->DesktopWidth, settings->DesktopHeight));
        return true;
    }

    bool ConnectorRdp::clientCapabilitiesEvent(const rdpSettings* settings) const {
        Application::info("{}: desktop size: {}, depth: {}",
                         NS_FuncNameV, XCB::Size(settings->DesktopWidth, settings->DesktopHeight), settings->ColorDepth);

        Application::info("{}: settings - {}: {:#010x}", NS_FuncNameV, "RdpVersion", settings->RdpVersion);
        Application::info("{}: settings - {}: {:#06x}", NS_FuncNameV, "OsMajorType", settings->OsMajorType);
        Application::info("{}: settings - {}: {:#06x}", NS_FuncNameV, "OsMinorType", settings->OsMinorType);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "Username", fmt_cstr(settings->Username));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "Domain", fmt_cstr(settings->Domain));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "DesktopWidth", settings->DesktopWidth);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "DesktopHeight", settings->DesktopHeight);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "DesktopColorDepth", settings->ColorDepth);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "peerProductId", settings->ClientProductId);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "AutoLogonEnabled", static_cast<bool>(settings->AutoLogonEnabled));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "CompressionEnabled", static_cast<bool>(settings->CompressionEnabled));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "RemoteFxCodec", static_cast<bool>(settings->RemoteFxCodec));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "NSCodec", static_cast<bool>(settings->NSCodec));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "JpegCodec", static_cast<bool>(settings->JpegCodec));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "FrameMarkerCommandEnabled", static_cast<bool>(settings->FrameMarkerCommandEnabled));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "SurfaceFrameMarkerEnabled", static_cast<bool>(settings->SurfaceFrameMarkerEnabled));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "SurfaceCommandsEnabled", static_cast<bool>(settings->SurfaceCommandsEnabled));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "FastPathInput", static_cast<bool>(settings->FastPathInput));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "FastPathOutput", static_cast<bool>(settings->FastPathOutput));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "UnicodeInput", static_cast<bool>(settings->UnicodeInput));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "BitmapCacheEnabled", static_cast<bool>(settings->BitmapCacheEnabled));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "DesktopResize", static_cast<bool>(settings->DesktopResize));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "RefreshRect", static_cast<bool>(settings->RefreshRect));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "SuppressOutput", static_cast<bool>(settings->SuppressOutput));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "TlsSecurity", static_cast<bool>(settings->TlsSecurity));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "NlaSecurity", static_cast<bool>(settings->NlaSecurity));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "RdpSecurity", static_cast<bool>(settings->RdpSecurity));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "SoundBeepsEnabled", static_cast<bool>(settings->SoundBeepsEnabled));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "AuthenticationLevel", settings->AuthenticationLevel);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "AllowedTlsCiphers", fmt_cstr(settings->AllowedTlsCiphers));
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "TlsSecLevel", settings->TlsSecLevel);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "EncryptionMethods", settings->EncryptionMethods);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "EncryptionLevel", settings->EncryptionLevel);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "CompressionLevel", settings->CompressionLevel);
        Application::info("{}: settings - {}: {}", NS_FuncNameV, "MultifragMaxRequestSize", settings->MultifragMaxRequestSize);
        return true;
    }

    bool ConnectorRdp::serverAdjustMonitorsEvent(const rdpSettings* settings) const {
        UINT32 monitorCount = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
        Application::info("{}: monitors: {}", NS_FuncNameV, monitorCount);
        return true;
    }

    bool ConnectorRdp::serverPostConnectEvent(const rdpSettings* settings) {

        return rdpChannelsInit();
    }

    void ConnectorRdp::serverDisconnectEvent(void) {
        Application::info("{}: event", NS_FuncNameV);
        asio::post(ioc(), std::bind(&ConnectorRdp::stop, this));
    }

    bool ConnectorRdp::serverCloseEvent(void) const {
        Application::info("{}: event", NS_FuncNameV);
        return true;
    }

    bool ConnectorRdp::serverKeyboardEvent(uint16_t flags, uint16_t code) {
        Application::debug(DebugType::App, "{}: flags: {:#06x}, code: {:#06x}", NS_FuncNameV, flags, code);

        idleSessionReset();

        if(xcbAllowMessages()) {
            asio::post(xcb_strand_, [this, flags, code]() {
                xcbKeyboardEvent(flags, code);
            });
        }

        return true;
    }

    bool ConnectorRdp::serverMouseEvent(uint16_t flags, uint16_t posx, uint16_t posy) {
        Application::debug(DebugType::App, "{}: flags: {:#06x}, posx: {}, posy: {}", NS_FuncNameV, flags, posx, posy);

        idleSessionReset();

        if(xcbAllowMessages()) {
            asio::post(xcb_strand_, [this, flags, posx, posy]() {
                xcbMouseEvent(flags, posx, posy);
            });
        }

        return true;
    }

    bool ConnectorRdp::serverRefreshEvent(uint8_t counts, const RECTANGLE_16* rects) {
        Application::debug(DebugType::App, "{}: count rects: {}", NS_FuncNameV, counts);

        std::vector<xcb_rectangle_t> rectangles(0 < counts ? counts : 1);

        if(counts && rects) {
            for(int it = 0; it < counts; ++it) {
                rectangles[it].x = rects[it].left;
                rectangles[it].y = rects[it].top;
                rectangles[it].width = rects[it].right - rects[it].left + 1;
                rectangles[it].height = rects[it].bottom - rects[it].top + 1;
            }
        } else {
            auto wsz = RootDisplay::size();
            rectangles[0].x = 0;
            rectangles[0].y = 0;
            rectangles[0].width = wsz.width;
            rectangles[0].height = wsz.height;
        }

        asio::post(xcb_strand_, [this, rects=std::move(rectangles)]() {
            RootDisplay::rootDamageAddRegions(rects.data(), rects.size());
        });

        return true;
    }

    bool ConnectorRdp::serverSuppressEvent(bool allow) {
        Application::debug(DebugType::App, "{}: allow: {}", NS_FuncNameV, allow);

        asio::post(xcb_strand_, [this, allow]() {
            if(allow) {
                xcbDisableMessages(false);
                auto region = RootDisplay::region();
                RootDisplay::rootDamageAddRegion(region);
            } else {
                xcbDisableMessages(true);
            }
        });

        return true;
    }

    // client events
    void ConnectorRdp::rdpDesktopResizeEvent(const XCB::Size & dsz) {
        auto peer = rdpEvents_->peer;

#if defined(FREERDP3_API)
        auto settings = peer->context->settings;
        auto update = peer->context->update;
#else
        auto settings = peer->settings;
        auto update = peer->update;
#endif

        settings->DesktopWidth = dsz.width;
        settings->DesktopHeight = dsz.height;

        if(! update->DesktopResize(update->context)) {
            Application::error("{}: {} failed", NS_FuncNameV, "DesktopResize");
        }
    }

    bool ConnectorRdp::rdpUpdateBitmapPlanar(const XCB::Region & reg, const XCB::PixmapInfoReply & reply) {
        auto peer = rdpEvents_->peer;
        auto context = static_cast<ServerContext*>(peer->context);

#if defined(FREERDP3_API)
        auto settings = peer->context->settings;
        auto update = peer->context->update;
#else
        auto settings = peer->settings;
        auto update = peer->update;
#endif

        const size_t scanLineBytes = reg.width * reply->bytePerPixel();
        const size_t tileSize = 64;
        const size_t pixelFormat = settings->OsMajorType == 6 ? PIXEL_FORMAT_RGBX32 : PIXEL_FORMAT_BGRX32;

        if(reply->size() != reg.height * reg.width * reply->bytePerPixel()) {
            Application::error("{}: {} failed, length: {}, size: {}, bpp: {}", NS_FuncNameV,
                               "align region", reply->size(), reg.toSize(), reply->bytePerPixel());
            throw rdp_error(NS_FuncNameS);
        }

        // planar activate
        if(! context->planar) {
            DWORD planarFlags = PLANAR_FORMAT_HEADER_RLE;

            if(settings->DrawAllowSkipAlpha) {
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
                pixelFormat, subreg.width, subreg.height, scanLineBytes, nullptr, & st.bitmapLength);
            st.cbCompMainBodySize = st.bitmapLength;

            if(settings->MultifragMaxRequestSize < st.cbCompMainBodySize + hdrsz) {
                Application::error("{}: {} failed", NS_FuncNameV, "MultifragMaxRequestSize");
                throw rdp_error(NS_FuncNameS);
            }

            vec.emplace_back(st);
        }

        auto it1 = vec.begin();
        update->BeginPaint(context);

        while(it1 != vec.end()) {
            // calc blocks
            size_t totalSize = 0;
            auto it2 = std::ranges::find_if(it1, vec.end(), [&](auto & st) {
                if(totalSize + (st.cbCompMainBodySize + hdrsz) > settings->MultifragMaxRequestSize) {
                    return true;
                }

                totalSize += (st.cbCompMainBodySize + hdrsz);
                return false;
            });

            BITMAP_UPDATE bitmapUpdate = {};
#if defined(FREERDP3_API)
            bitmapUpdate.number = std::distance(it1, it2);
#else
            bitmapUpdate.count = bitmapUpdate.number = std::distance(it1, it2);
#endif
            bitmapUpdate.rectangles = & (*it1);

            if(! update->BitmapUpdate(context, & bitmapUpdate)) {
                Application::error("{}: {} failed, length: {}", NS_FuncNameV, "BitmapUpdate", totalSize);
                throw rdp_error(NS_FuncNameS);
            }

            it1 = it2;
        }
        update->EndPaint(context);

        for(const auto & st : vec) {
            std::free(st.bitmapDataStream);
        }

        return true;
    }

    bool ConnectorRdp::rdpUpdateBitmapInterleaved(const XCB::Region & reg, const XCB::PixmapInfoReply & reply) {
        auto peer = rdpEvents_->peer;
        auto context = static_cast<ServerContext*>(peer->context);

#if defined(FREERDP3_API)
        auto settings = peer->context->settings;
        auto update = peer->context->update;
#else
        auto settings = peer->settings;
        auto update = peer->update;
#endif

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
        update->BeginPaint(context);

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
                                      reply->data() + offset, pixelFormat, scanLineBytes, 0, 0, nullptr, reply->bitsPerPixel())) {
                Application::error("{}: {} failed", NS_FuncNameV, "interleaved_compress");
                throw rdp_error(NS_FuncNameS);
            }

            st.bitmapDataStream = data.get();
            st.cbCompMainBodySize = st.bitmapLength;

            if(settings->MultifragMaxRequestSize < st.bitmapLength + 22) {
                Application::error("{}: {} failed", NS_FuncNameV, "MultifragMaxRequestSize");
                throw rdp_error(NS_FuncNameS);
            }

            BITMAP_UPDATE bitmapUpdate = {};
#if defined(FREERDP3_API)
            bitmapUpdate.number = 1;
#else
            bitmapUpdate.count = bitmapUpdate.number = 1;
#endif
            bitmapUpdate.rectangles = & st;
            auto ret = update->BitmapUpdate(context, & bitmapUpdate);

            if(! ret) {
                Application::error("{}: {} failed", NS_FuncNameV, "BitmapUpdate");
                throw rdp_error(NS_FuncNameS);
            }
        }
        update->EndPaint(context);

        return true;
    }

    bool ConnectorRdp::rdpChannelsInit(void) {
/*
        if(rdpEvents_->context->clipboard &&
           WTSVirtualChannelManagerIsChannelJoined(rdpEvents_->context->vcm, CLIPRDR_SVC_CHANNEL_NAME)) {
        }
*/
        return true;
    }

    void ConnectorRdp::rdpChannelsFree(void) {
    }

}
