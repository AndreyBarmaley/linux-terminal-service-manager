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

#include <chrono>
#include <thread>
#include <exception>

#include "ltsm_tools.h"
#include "ltsm_connector_proto.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_channels.h"

using namespace boost;
using namespace std::chrono_literals;

namespace LTSM::Connector {
    /* ConnectorLtsm */
    ConnectorLtsm::ConnectorLtsm(const std::filesystem::path & confile, bool debug)
        : DBusProxy(ConnectorType::LTSM, confile, debug)
        , RFB::X11Server(ioc())
        , transfer_strand_{get_executor()} {}

    ConnectorLtsm::~ConnectorLtsm() {
        stop();
    }

    int ConnectorLtsm::start(void) {
        if(0 >= busGetServiceVersion()) {
            Application::error("{}: bus service failure", NS_FuncNameV);
            return EXIT_FAILURE;
        }

        Application::info("{}: remote addr: {}", NS_FuncNameV, remoteAddress());

        x11NoDamage_ = config().getBoolean("vnc:xcb:nodamage", false);
        asio::co_spawn(DBusProxy::ioc(), rfbCommunicationAwait(), asio::detached);

        BoostContext::run();
        return 0;
    }

    void ConnectorLtsm::stop(void) noexcept {
        std::call_once(stop_flag_, [this](){
            asioStop();
        });
    }

    void ConnectorLtsm::asioStop(void) noexcept {
        try {
            if(0 < displayNum()) {
                busConnectorTerminated(displayNum(), getpid());
                clientDisconnectedEvent(displayNum());
                Application::info("{}: connector shutdown, display: {}", NS_FuncNameV, displayNum());
            }

            X11Server::rfbStop();
            DBusProxy::asioStop();
        } catch(const std::exception & err) {
            Application::warning("{}: connector error: {}", NS_FuncNameV, err.what());
        }
    }

    uint16_t ConnectorLtsm::encodingThreads(void) const {
        return concurency();
    }

    std::future<BinaryBuf> ConnectorLtsm::postEncoderJob(RFB::PostEncoderJobCb && func, XCB::Region reg) const {
        std::promise<BinaryBuf> prom;
        auto ret = prom.get_future();
        if(1 < concurency()) {
            // job background
            boost::asio::post(const_cast<ConnectorLtsm&>(*this).ioc(), [promise=std::move(prom), job=std::move(func), reg]() mutable {
                try {
                    promise.set_value(job(reg));
                } catch(const std::exception&) {
                    promise.set_exception_at_thread_exit(std::current_exception());
                }
            });
        } else {
            try {
                prom.set_value(func(reg));
            } catch(const std::exception&) {
                prom.set_exception_at_thread_exit(std::current_exception());
            }
        }
        return ret;
    }

    asio::awaitable<void> ConnectorLtsm::onLoginSuccessAwait(std::string userName, uint32_t userUid) {
        xcbDisableMessages(true);
        co_await waitUpdateProcessAwait();

        int oldDisplay = displayNum();
        int newDisplay = busStartUserSession(oldDisplay, getpid(), userName, remoteAddress(), connectorType());

        if(newDisplay < 0) {
            Application::error("{}: {} failed", NS_FuncNameV, "busStartUserSession");
            throw proto_error(NS_FuncNameS);
        }

        if(newDisplay != oldDisplay) {
            busShutdownDisplay(oldDisplay);
            auto xauthFile = busDisplayAuthFile(newDisplay);

            co_await xcbConnectAwait(newDisplay, xauthFile, *this);
        }

        co_await xcbShmInit(userUid);

        xcbDisableMessages(false);
        const auto clientRegion = getClientRegion();

        // fix new session size
        if(RootDisplay::size() != clientRegion.toSize()) {
            Application::warning("{}: remote request desktop size: {}, display: {}", NS_FuncNameV,
                                 clientRegion.toSize(), displayNum());

            if(RootDisplay::setRandrScreenSize(clientRegion)) {
                Application::info("{}: change session size: {}, display: {}",
                        NS_FuncNameV, clientRegion.toSize(), displayNum());
            }
        } else {
            // full update
            X11Server::serverScreenUpdateRequest();
        }

        auto json = JsonContentString(busGetSessionJson(newDisplay)).toObject();

        setIdleTimeoutSec(json.getInteger("session:idle:timeout", 0));
        userSession_ = true;

        busConnectorConnected(newDisplay, getpid());
        busSetSessionEncodings(newDisplay, getClientEncodings().toVector());

        co_await asio::dispatch(rfb_strand(), asio::use_awaitable);
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::LoginSuccess);
        jos.push("action", true);
        static_cast<ChannelClient*>(this)->sendLtsmChannelData(static_cast<uint8_t>(ChannelType::System), jos.flush());

        co_return;
    }

    void ConnectorLtsm::serverScreenUpdateRequest(const XCB::Region& reg) {
        X11Server::serverScreenUpdateRequest(reg);
    }

    void ConnectorLtsm::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t & userUid) {
        if(display != displayNum()) {
            return;
        }

        Application::notice("{}: dbus signal, display: {}, username: {}, uid: {}", NS_FuncNameV, display,
                            userName, userUid);

        asio::co_spawn(xcb_strand(), onLoginSuccessAwait(userName, userUid), [this](std::exception_ptr ptr){
            if(ptr) {
                try {
                    std::rethrow_exception(ptr); 
                } catch (const std::exception& err) {
                    Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                    asio::post(ioc(), std::bind(&ConnectorLtsm::stop, this));
                }
            }
        });
    }

    void ConnectorLtsm::onShutdownConnector(const int32_t & display) {
        if(display == displayNum()) {
            Application::notice("{}: dbus signal, display: {}", NS_FuncNameV, display);
            asio::post(ioc(), std::bind(&ConnectorLtsm::stop, this));
        }
    }

    void ConnectorLtsm::onSendBellSignal(const int32_t & display) {
        if(display == displayNum()) {
            Application::info("{}: dbus signal, display: {}", NS_FuncNameV, display);
            asio::co_spawn(ioc(), sendBellEventAwait(), asio::detached);
        }
    }

    const PixelFormat & ConnectorLtsm::serverFormat(void) const {
        return serverPf_;
    }

    void ConnectorLtsm::serverFrameBufferModifyEvent(FrameBuffer & fb) const {
        renderPrimitivesToFB(fb);
    }

    void ConnectorLtsm::loadKeymap(const std::string & file) {
        if(JsonContentFile jc(file); jc.isObject()) {
            auto jo = jc.toObject();

            for(const auto & skey : jo.keys()) {
                if(skey == "keyname_hex8") {
                    continue;
                }

                try {
                    keymap_.emplace(std::stoi(skey, nullptr, 0), jo.getInteger(skey));
                } catch(const std::exception &) { }
            }
        }
    }

    asio::awaitable<void> ConnectorLtsm::connectorHandshakeVersionAwait(void) {
        // session request
        const int depth = 24;
        int screen = busStartLoginSession(getpid(), depth, remoteAddress(), "ltsm");

        if(screen <= 0) {
            Application::error("{}: {} failed", NS_FuncNameV, "login session request");
            throw proto_error(NS_FuncNameS);
        }

        Application::info("{}: login session request success, display: {}", NS_FuncNameV, screen);
        auto xauthFile = busDisplayAuthFile(screen);

        co_await xcbConnectAwait(screen, xauthFile, *this);
        const xcb_visualtype_t* visual = RootDisplay::visual();

        if(! visual) {
            Application::error("{}: xcb visual empty", NS_FuncNameV);
            throw proto_error(NS_FuncNameS);
        }

        Application::debug(DebugType::Xcb, "{}: xcb max request: {}", NS_FuncNameV, RootDisplay::getMaxRequest());
        // init server format
        serverPf_ = PixelFormat(RootDisplay::bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);

        // load keymap
        if(config().hasKey("vnc:keymap:file")) {
            loadKeymap(config().getString("vnc:keymap:file"));
        }

        co_return;
    }

    std::forward_list<std::string> ConnectorLtsm::serverDisabledEncodings(void) const {
        return config().getStdListForward<std::string>("encoding:blacklist");
    }

    void ConnectorLtsm::serverDisplayResizedEvent(const XCB::Size & sz) {
        busDisplayResized(displayNum(), sz.width, sz.height);
    }

    void ConnectorLtsm::serverEncodingsEvent(void) {
        if(isClientLtsmSupported()) {
            asio::co_spawn(rfb_strand(), sendEncodingLtsmSupportedAwait(), asio::detached);
        }
    }

    void ConnectorLtsm::serverConnectedEvent(void) {
#ifdef LTSM_WITH_GSSAPI

        if(auto info = ServerEncoder::authInfo(); ! info.first.empty()) {
            const auto & login = info.first;
            helperSetSessionLoginPassword(displayNum(), login, "", false);
            // not so fast
            std::this_thread::sleep_for(50ms);
            busSetAuthenticateToken(displayNum(), login);
        }

#endif
    }

    void ConnectorLtsm::serverSecurityInitEvent(void) {
        busSetEncryptionInfo(displayNum(), serverEncryptionInfo());
    }

    RFB::SecurityInfo ConnectorLtsm::rfbSecurityInfo(void) const {
        RFB::SecurityInfo secInfo;
        secInfo.authNone = true;
        secInfo.authVnc = false;
        secInfo.authVenCrypt = ! config().getBoolean("vnc:gnutls:disable", false);
        secInfo.tlsPriority = config().getString("vnc:gnutls:priority", "NORMAL:+ANON-ECDH:+ANON-DH");
        secInfo.tlsAnonMode = config().getBoolean("vnc:gnutls:anonmode", true);
        secInfo.caFile = config().getString("vnc:gnutls:cafile");
        secInfo.certFile = config().getString("vnc:gnutls:certfile");
        secInfo.keyFile = config().getString("vnc:gnutls:keyfile");
        secInfo.crlFile = config().getString("vnc:gnutls:crlfile");
        secInfo.tlsDebug = config().getInteger("vnc:gnutls:debug", 0);
#ifdef LTSM_WITH_GSSAPI
        secInfo.authKrb5 = ! config().getBoolean("vnc:kerberos:disable", false);
        secInfo.krb5Service = config().getString("vnc:kerberos:service", "TERMSRV");
#endif

        if(secInfo.authKrb5) {
            auto keytab = config().getString("vnc:kerberos:keytab", "/etc/ltsm/termsrv.keytab");

            if(keytab.empty()) {
                secInfo.authKrb5 = false;
                return secInfo;
            }

            std::error_code err;
            if(std::filesystem::is_regular_file(keytab, err)) {
                Application::info("{}: set KRB5_KTNAME=`{}'", NS_FuncNameV, keytab);
                setenv("KRB5_KTNAME", keytab.c_str(), 1);

                if(auto debug = config().getString("vnc:kerberos:trace"); ! debug.empty()) {
                    Application::info("{}: set KRB5_TRACE=`{}'", NS_FuncNameV, debug);
                    setenv("KRB5_TRACE", debug.c_str(), 1);
                }
            } else {
                Application::warning("{}: {} failed, code: {}, error: {}, path: `{}'",
                                NS_FuncNameV, "is_regular_file", err.value(), err.message(), keytab);
                secInfo.authKrb5 = false;
            }
        }

        return secInfo;
    }

    bool ConnectorLtsm::rfbClipboardEnable(void) const {
        return config().getBoolean("vnc:clipboard");
    }

    bool ConnectorLtsm::rfbDesktopResizeEnabled(void) const {
        return true;
    }

    bool ConnectorLtsm::xcbAllowMessages(void) const {
        return DBusProxy::xcbAllowMessages();
    }

    uint32_t ConnectorLtsm::frameRateOption(void) const {
        constexpr uint32_t minFps = 5;
        constexpr uint32_t maxFps = 20;
        return std::clamp(frameRate_, minFps, maxFps);
    }

    bool ConnectorLtsm::xcbNoDamageOption(void) const {
        return isClientLtsmSupported() && x11NoDamage_;
    }

    void ConnectorLtsm::xcbDisableMessages(bool f) {
        DBusProxy::xcbDisableMessages(f);
    }

    int ConnectorLtsm::rfbUserKeycode(uint32_t keysym) const {
        auto it = keymap_.find(keysym);
        return it != keymap_.end() ? it->second : 0;
    }

    void ConnectorLtsm::serverRecvKeyEvent(bool pressed, uint32_t keycode, uint16_t scancode) {
        X11Server::serverRecvKeyEvent(pressed, keycode, scancode);
        idleSessionReset();
    }

    void ConnectorLtsm::serverRecvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy) {
        X11Server::serverRecvPointerEvent(mask, posx, posy);
        idleSessionReset();
    }

    bool ConnectorLtsm::isUserSession(void) const {
        return userSession_;
    }

    void ConnectorLtsm::systemClientVariables(const JsonObject & jo) {
        Application::debug(DebugType::App, "{}: count: {}", NS_FuncNameV, jo.size());

        if(auto env = jo.getObject("environments")) {
            busSetSessionEnvironments(displayNum(), env->toStdMap<std::string>());
        }

        if(auto keyboard = jo.getObject("keyboard")) {
            auto names = keyboard->getStdVector<std::string>("layouts");
            busSetSessionKeyboardLayouts(displayNum(), names);
            auto layout = keyboard->getString("current");
            auto it = std::ranges::find_if(names, [&](auto & str) {
                return Tools::lower(str).substr(0, 2) == Tools::lower(layout).substr(0, 2);
            });

            asio::dispatch(xcb_strand(), [this, group = std::distance(names.begin(), it)]() {
                if(auto xkb = static_cast<const XCB::ModuleXkb*>(RootDisplay::getExtension(XCB::Module::XKB))) {
                    // wait pause for apply layouts
                    std::this_thread::sleep_for(200ms);
                    xkb->switchLayoutGroup(group);
                }
            });
        }

        if(auto opts = jo.getObject("options")) {
            busSetSessionOptions(displayNum(), opts->toStdMap<std::string>());
            [[maybe_unused]] int clientVersion = opts->getInteger("ltsm:client", 0);
            x11NoDamage_ = opts->getBoolean("x11:nodamage", x11NoDamage_);
            frameRate_ = opts->getInteger("frame:rate", frameRate_);

            setEncodingOptions(opts->getStdListForward<std::string>("enc:opts"), frameRateOption());

            if(x11NoDamage_ && ! XCB::RootDisplay::hasError()) {
                XCB::RootDisplay::extensionDisable(XCB::Module::DAMAGE);
            }
        }
    }

    void ConnectorLtsm::systemCursorFailed(const JsonObject & jo) {
        auto cursorId = jo.getInteger("cursor");

        if(cursorId) {
            Application::debug(DebugType::App, "{}: cursor id: {:#010x}", NS_FuncNameV, cursorId);
            cursorRequest(cursorId);
        }
    }

    void ConnectorLtsm::systemKeyboardChange(const JsonObject & jo) {
        auto layout = jo.getString("layout");

        if(xcbAllowMessages()) {
            if(auto xkb = static_cast<const XCB::ModuleXkb*>(RootDisplay::getExtension(XCB::Module::XKB))) {
                Application::debug(DebugType::App, "{}: layout: {}", NS_FuncNameV, layout);
                auto names = xkb->getNames();
                auto it = std::ranges::find_if(names, [&](auto & str) {
                    return Tools::lower(str).substr(0, 2) == Tools::lower(layout).substr(0, 2);
                });

                if(it != names.end()) {
                    //Application::debug(DebugType::Input, "{}: group: `s'", NS_FuncNameV, it->c_str());
                    xkb->switchLayoutGroup(std::distance(names.begin(), it));
                } else {
                    Application::error("{}: layout not found: {}, names: [{}]",
                                       NS_FuncNameV, layout, Tools::join(names));
                }
            }
        }
    }

    void ConnectorLtsm::systemTransferFiles(const JsonObject & jo) {
        if(! isUserSession()) {
            Application::error("{}: not user session", NS_FuncNameV);
            return;
        }

        asio::co_spawn(transfer_strand_, systemTransferFilesAwait(jo), asio::detached);
    }

    asio::awaitable<void> ConnectorLtsm::systemTransferFilesAwait(JsonObject jo) {
        auto fa = jo.getArray("files");
        if(! fa) {
            Application::error("{}: incorrect format message", NS_FuncNameV);
            co_return;
        }

        Application::debug(DebugType::App, "{}: files count: {}", NS_FuncNameV, fa->size());

        // check transfer disabled
        if(config().getBoolean("transfer:file:disabled", false)) {
            Application::error("{}: administrative disable", NS_FuncNameV);
            busSendNotify(displayNum(), "Transfer Disable", "transfer is blocked, contact the administrator",
                          NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
            co_return;
        }

        size_t fmax = 0;
        size_t prettyMb = 0;

        if(config().hasKey("transfer:file:max")) {
            fmax = config().getInteger("transfer:file:max");
            prettyMb = fmax / (1024 * 1024);
        }

        for(int it = 0; it < fa->size(); ++it) {
            auto jo2 = fa->getObject(it);

            if(! jo2) {
                continue;
            }

            std::string fname = jo2->getString("file");
            size_t fsize = jo2->getInteger("size");

            if(std::ranges::any_of(transferPlanned_, [&](auto & st) { return fname == std::get<0>(st); })) {
                Application::warning("{}: found planned and skipped, file: {}", NS_FuncNameV, fname);
                continue;
            }

            // check max size
            if(fmax && fsize > fmax) {
                Application::warning("{}: file size exceeds and skipped, file: {}", NS_FuncNameV, fname);
                busSendNotify(displayNum(), "Transfer Skipped",
                              fmt::format("the file size exceeds, the allowed limit: {}M, file: {}", prettyMb, fname),
                              NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
                continue;
            }

            // add planned transfer
            transferPlanned_.emplace_back(std::move(fname), fsize);
        }

        size_t freeChannels = countFreeChannels();

        if(transferPlanned_.empty()) {
            Application::warning("{}: file list empty", NS_FuncNameV);
        } else if(! freeChannels) {
            Application::warning("{}: no free channels", NS_FuncNameV);
        } else {
            if(transferPlanned_.size() <= freeChannels) {
                // send request to manager
                busTransferFilesRequest(displayNum(), {transferPlanned_.begin(), transferPlanned_.end() });
            } else {
                // transfer background
                co_await transferFilesPartial(std::move(transferPlanned_));
            }
        }

        co_return;
    }

    asio::awaitable<void> ConnectorLtsm::transferFilesPartial(std::list<TupleFileSize>&& files) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer(ex, std::chrono::seconds(1));

        size_t freeChannels = countFreeChannels() / 3;
        using TimePointSeconds = Tools::TimePoint<std::chrono::seconds>;
        std::unique_ptr<TimePointSeconds> partial;
        auto it1 = files.begin();

        while(it1 != files.end()) {
            // wait 5s
            if(! partial || partial->check()) {
                if(! partial) {
                    partial = std::make_unique<TimePointSeconds>(5s);
                }

                if(freeChannels > countFreeChannels()) {
                    continue;
                }

                auto it2 = rangesNext(it1, freeChannels, files.end());

                try {
                    // send partial request to manager
                    busTransferFilesRequest(displayNum(), { it1, it2 });
                } catch(...) {
                    break;
                }

                it1 = it2;
            }

            co_await timer.async_wait(asio::use_awaitable);
        }
        co_return;
    }

    void ConnectorLtsm::onTransferAllow(const int32_t & display, const std::string & filepath, const std::string & tmpfile,
                                        const std::string & dstdir) {
        // filepath - client file path
        // tmpfile - server tmpfile
        // dstdir - server target directory
        Application::debug(DebugType::App, "{}: display: {}", NS_FuncNameV, display);

        if(display == displayNum()) {
            asio::co_spawn(transfer_strand_, onTransferAllowAwait(filepath, tmpfile, dstdir), asio::detached);
        }
    }

    asio::awaitable<void> ConnectorLtsm::onTransferAllowAwait(std::string filepath, std::string tmpfile, std::string dstdir) {
        auto it = std::ranges::find_if(transferPlanned_, [&](auto & st) {
            return filepath == std::get<0>(st);
        });

        if(it == transferPlanned_.end()) {
            Application::error("{}: transfer not found, file: {}", NS_FuncNameV, filepath);
            co_return;
        }

        // transfer not canceled
        if(! dstdir.empty() && ! tmpfile.empty()) {
            // create file transfer channel
            createChannel(Channel::UrlMode(Channel::ConnectorType::File, filepath, Channel::ConnectorMode::ReadOnly),
                          Channel::UrlMode(Channel::ConnectorType::File, tmpfile, Channel::ConnectorMode::WriteOnly),
                          Channel::Opts{Channel::Speed::Slow, 0});
            auto dstfile = std::filesystem::path(dstdir) / std::filesystem::path(filepath).filename();
            busTransferFileStarted(displayNum(), tmpfile, std::get<1>(*it) /* size */, dstfile);
        }

        // remove planned
        transferPlanned_.erase(it);
        co_return;
    }

    void ConnectorLtsm::onCreateChannel(const int32_t & display, const std::string & client, const std::string & cmode,
                                        const std::string & server, const std::string & smode, const std::string & speed) {
        if(display == displayNum()) {
            createChannel(Channel::UrlMode(client, cmode), Channel::UrlMode(server, smode),
                          Channel::Opts{Channel::connectorSpeed(speed), 0});
        }
    }

    void ConnectorLtsm::onDestroyChannel(const int32_t & display, const uint8_t & channel) {
        if(display == displayNum()) {
            destroyChannel(channel);
        }
    }

    void ConnectorLtsm::onCreateListener(const int32_t & display, const std::string & client, const std::string & cmode,
                                         const std::string & server, const std::string & smode, const std::string & speed, const uint8_t & limit,
                                         const uint32_t & flags) {
        if(display == displayNum()) {
            createListener(Channel::UrlMode(client, cmode), Channel::UrlMode(server, smode), limit,
                           Channel::Opts{Channel::connectorSpeed(speed), (int) flags});
        }
    }

    void ConnectorLtsm::onDestroyListener(const int32_t & display, const std::string & client, const std::string & server) {
        if(display == displayNum()) {
            destroyListener(client, server);
        }
    }

    void ConnectorLtsm::onDebugChannel(const int32_t & display, const uint8_t & channel, const bool & debug) {
        if(display == displayNum()) {
            setChannelDebug(channel, debug);
        }
    }

    void ConnectorLtsm::onLoginFailure(const int32_t & display, const std::string & msg) {
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::LoginSuccess);
        jos.push("action", false);
        jos.push("error", msg);
        static_cast<ChannelClient*>(this)->sendLtsmChannelData(static_cast<uint8_t>(ChannelType::System), jos.flush());
    }

    void ConnectorLtsm::systemChannelError(const JsonObject & jo) {
        auto channel = jo.getInteger("id");
        auto code = jo.getInteger("code");
        auto err = jo.getString("error");
        Application::info("{}: channel: {}, errno: {}, display: {}, error: `{}'",
                 NS_FuncNameV, channel, displayNum(), code, err);

        if(isUserSession())
            busSendNotify(displayNum(), "Channel Error", err.append(", errno: ").append(std::to_string(code)),
                          NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
    }

    bool ConnectorLtsm::noVncMode(void) const {
        return remoteAddress() == "127.0.0.1" &&
               config().getBoolean("vnc:novnc:allow", false);
    }
}
