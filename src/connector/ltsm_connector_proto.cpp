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
#include "ltsm_sdl_wrapper.h"
#include "ltsm_channels.h"

using namespace std::chrono_literals;

namespace LTSM::Connector
{
    /* ConnectorLtsm */
    ConnectorLtsm::~ConnectorLtsm()
    {
        rfbMessagesShutdown();
        xcbDisableMessages(true);

        if(0 < displayNum())
        {
            busConnectorTerminated(displayNum(), getpid());
            clientDisconnectedEvent(displayNum());
        }

        // waitUpdateProcess();
        Application::info("%s: connector shutdown", __FUNCTION__);
    }

    int ConnectorLtsm::communication(void)
    {
        if(0 >= busGetServiceVersion())
        {
            Application::error("%s: bus service failure", __FUNCTION__);
            return EXIT_FAILURE;
        }

        Application::info("%s: remote addr: %s", __FUNCTION__, _remoteaddr.c_str());

        _x11NoDamage = _config.getBoolean("vnc:xcb:nodamage", false);
        _frameRate = _config.getInteger("vnc:frame:rate", 16);

        if(_frameRate <= 0)
        {
            Application::warning("%s: invalid value for: `%s'", __FUNCTION__, "vnc:frame:rate");
            _frameRate = 16;
        }

        return rfbCommunication();
    }

    void ConnectorLtsm::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t & userUid)
    {
        if(display != displayNum())
        {
            return;
        }

        xcbDisableMessages(true);
        waitUpdateProcess();
        _shmUid = userUid;
        Application::notice("%s: dbus signal, display: %" PRId32 ", username: %s, uid: %" PRIu32, __FUNCTION__, display,
                            userName.c_str(), userUid);
        int oldDisplay = displayNum();
        int newDisplay = busStartUserSession(oldDisplay, getpid(), userName, _remoteaddr, connectorType());

        if(newDisplay < 0)
        {
            Application::error("%s: %s failed", __FUNCTION__, "user session request");
            throw std::runtime_error(NS_FuncName);
        }

        if(newDisplay != oldDisplay)
        {
            // wait xcb old operations ended
            std::this_thread::sleep_for(100ms);

            if(! xcbConnect(newDisplay, *this))
            {
                Application::error("%s: %s failed", __FUNCTION__, "xcb connect");
                throw std::runtime_error(NS_FuncName);
            }

            busShutdownDisplay(oldDisplay);
        }

        xcbShmInit(_shmUid);
        xcbDisableMessages(false);
        auto & clientRegion = getClientRegion();

        // fix new session size
        if(xcbDisplay()->size() != clientRegion.toSize())
        {
            Application::warning("%s: remote request desktop size: [%" PRIu16 ", %" PRIu16 "], display: %d", __FUNCTION__,
                                 clientRegion.width, clientRegion.height, displayNum());

            if(0 < xcbDisplay()->setRandrScreenSize(clientRegion))
            {
                Application::info("%s: change session size: [%" PRIu16 ", %" PRIu16 "], display: %d", __FUNCTION__, clientRegion.width,
                                  clientRegion.height, displayNum());
            }
        }
        else
        {
            // full update
            if(! _x11NoDamage)
            {
                X11Server::serverScreenUpdateRequest();
            }
        }

        _idleTimeoutSec = _config.getInteger("session:idle:timeout", 0);
        _idleSessionTp = std::chrono::steady_clock::now();
        _userSession = true;

        busConnectorConnected(newDisplay, getpid());

        std::thread([this]()
        {
            JsonObjectStream jos;
            jos.push("cmd", SystemCommand::LoginSuccess);
            jos.push("action", true);
            static_cast<ChannelClient*>(this)->sendLtsmChannelData(static_cast<uint8_t>(ChannelType::System), jos.flush());
        }).detach();
    }

    void ConnectorLtsm::onShutdownConnector(const int32_t & display)
    {
        if(display == displayNum())
        {
            xcbDisableMessages(true);
            waitUpdateProcess();
            rfbMessagesShutdown();
            Application::notice("%s: dbus signal, display: %" PRId32, __FUNCTION__, display);
        }
    }

    void ConnectorLtsm::onHelperWidgetStarted(const int32_t & display)
    {
        if(display == displayNum())
        {
            Application::info("%s: dbus signal, display: %" PRId32, __FUNCTION__, display);
            _loginWidgetStarted = true;
        }
    }

    void ConnectorLtsm::onSendBellSignal(const int32_t & display)
    {
        if(display == displayNum())
        {
            Application::info("%s: dbus signal, display: %" PRId32, __FUNCTION__, display);
            std::thread([this] { this->sendBellEvent(); }).detach();
        }
    }

    const PixelFormat & ConnectorLtsm::serverFormat(void) const
    {
        return _serverPf;
    }

    void ConnectorLtsm::serverFrameBufferModifyEvent(FrameBuffer & fb) const
    {
        renderPrimitivesToFB(fb);
    }

    void ConnectorLtsm::loadKeymap(const std::string & file)
    {
        if(JsonContentFile jc(file); jc.isObject())
        {
            auto jo = jc.toObject();

            for(const auto & skey : jo.keys())
            {
                try
                {
                    _keymap.emplace(std::stoi(skey, nullptr, 0), jo.getInteger(skey));
                }
                catch(const std::exception & ) { }
            }
        }
    }

    void ConnectorLtsm::serverHandshakeVersionEvent(void)
    {
        // Xvfb: session request
        int screen = busStartLoginSession(getpid(), 24, _remoteaddr, "ltsm");

        if(screen <= 0)
        {
            Application::error("%s: login session request: failure", __FUNCTION__);
            throw proto_error(NS_FuncName);
        }

        Application::info("%s: login session request success, display: %d", __FUNCTION__, screen);

        if(! xcbConnect(screen, *this))
        {
            Application::error("%s: xcb connect: failed", __FUNCTION__);
            throw proto_error(NS_FuncName);
        }

        const xcb_visualtype_t* visual = xcbDisplay()->visual();

        if(! visual)
        {
            Application::error("%s: xcb visual empty", __FUNCTION__);
            throw proto_error(NS_FuncName);
        }

        Application::debug(DebugType::Xcb, "%s: xcb max request: %lu", __FUNCTION__, xcbDisplay()->getMaxRequest());
        // init server format
        _serverPf = PixelFormat(xcbDisplay()->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);

        // load keymap
        if(_config.hasKey("vnc:keymap:file"))
        {
            loadKeymap(_config.getString("vnc:keymap:file"));
        }
    }

    std::forward_list<std::string> ConnectorLtsm::serverDisabledEncodings(void) const
    {
        return _config.getStdListForward<std::string>("vnc:encoding:blacklist");
    }

    void ConnectorLtsm::serverEncodingSelectedEvent(void)
    {
        setEncodingThreads(_config.getInteger("vnc:encoding:threads", 2));
        setEncodingDebug(_config.getInteger("vnc:encoding:debug", 0));
    }

    void ConnectorLtsm::serverMainLoopEvent(void)
    {
        checkIdleTimeout();
    }

    void ConnectorLtsm::serverDisplayResizedEvent(const XCB::Size & sz)
    {
        xcbShmInit(_shmUid);
        busDisplayResized(displayNum(), sz.width, sz.height);
    }

    void ConnectorLtsm::serverEncodingsEvent(void)
    {
        if(isClientLtsmSupported())
        {
            sendEncodingLtsmSupported();
        }
    }

    void ConnectorLtsm::serverConnectedEvent(void)
    {
        // wait widget started signal(onHelperWidgetStarted), 3000ms, 100 ms pause
        bool waitWidgetStarted = Tools::waitCallable<std::chrono::milliseconds>(3000, 100, [this]()
        {
            return !! this->_loginWidgetStarted;
        });

        if(! waitWidgetStarted)
        {
            Application::error("%s: wait _loginWidgetStarted failed", "serverConnectedEvent");
            throw proto_error(NS_FuncName);
        }

#ifdef LTSM_WITH_GSSAPI
        if(auto info = ServerEncoder::authInfo(); ! info.first.empty())
        {
            const auto & login = info.first;
            helperSetSessionLoginPassword(displayNum(), login, "", false);
            // not so fast
            std::this_thread::sleep_for(50ms);
            busSetAuthenticateToken(displayNum(), login);
        }

#endif
    }

    void ConnectorLtsm::serverSecurityInitEvent(void)
    {
        busSetEncryptionInfo(displayNum(), serverEncryptionInfo());
    }

    RFB::SecurityInfo ConnectorLtsm::rfbSecurityInfo(void) const
    {
        RFB::SecurityInfo secInfo;
        secInfo.authNone = true;
        secInfo.authVnc = false;
        secInfo.authVenCrypt = ! _config.getBoolean("vnc:gnutls:disable", false);
        secInfo.tlsPriority = _config.getString("vnc:gnutls:priority", "NORMAL:+ANON-ECDH:+ANON-DH");
        secInfo.tlsAnonMode = _config.getBoolean("vnc:gnutls:anonmode", true);
        secInfo.caFile = _config.getString("vnc:gnutls:cafile");
        secInfo.certFile = _config.getString("vnc:gnutls:certfile");
        secInfo.keyFile = _config.getString("vnc:gnutls:keyfile");
        secInfo.crlFile = _config.getString("vnc:gnutls:crlfile");
        secInfo.tlsDebug = _config.getInteger("vnc:gnutls:debug", 0);
#ifdef LTSM_WITH_GSSAPI
        secInfo.authKrb5 = ! _config.getBoolean("vnc:kerberos:disable", false);
        secInfo.krb5Service = _config.getString("vnc:kerberos:service", "TERMSRV");
#endif

        if(secInfo.authKrb5)
        {
            auto keytab = _config.getString("vnc:kerberos:keytab", "/etc/ltsm/termsrv.keytab");

            if(keytab.empty())
            {
                secInfo.authKrb5 = false;
                return secInfo;
            }

            std::filesystem::path file(keytab);
            std::error_code err;

            if(std::filesystem::is_regular_file(keytab, err))
            {
                Application::info("%s: set KRB5_KTNAME=`%s'", __FUNCTION__, keytab.c_str());
                setenv("KRB5_KTNAME", keytab.c_str(), 1);

                if(auto debug = _config.getString("vnc:kerberos:trace"); ! debug.empty())
                {
                    Application::info("%s: set KRB5_TRACE=`%s'", __FUNCTION__, debug.c_str());
                    setenv("KRB5_TRACE", debug.c_str(), 1);
                }
            }
            else
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"),
                                   keytab.c_str(), getuid());
                secInfo.authKrb5 = false;
            }
        }

        return secInfo;
    }

    bool ConnectorLtsm::rfbClipboardEnable(void) const
    {
        return _config.getBoolean("vnc:clipboard");
    }

    bool ConnectorLtsm::rfbDesktopResizeEnabled(void) const
    {
        return true;
    }

    bool ConnectorLtsm::xcbAllowMessages(void) const
    {
        return DBusProxy::xcbAllowMessages();
    }

    void ConnectorLtsm::serverScreenUpdateRequest(const XCB::Region & reg)
    {
        if(xcbAllowMessages() && ! _x11NoDamage)
        {
            X11Server::serverScreenUpdateRequest(reg);
        }
    }

    size_t ConnectorLtsm::frameRateOption(void) const
    {
        return _frameRate;
    }

    bool ConnectorLtsm::xcbNoDamageOption(void) const
    {
        return isClientLtsmSupported() ?
               static_cast<bool>(_x11NoDamage) : false;
    }

    void ConnectorLtsm::xcbDisableMessages(bool f)
    {
        DBusProxy::xcbDisableMessages(f);
    }

    int ConnectorLtsm::rfbUserKeycode(uint32_t keysym) const
    {
        auto it = _keymap.find(keysym);
        return it != _keymap.end() ? it->second : 0;
    }

    void ConnectorLtsm::serverRecvKeyEvent(bool pressed, uint32_t keysym)
    {
        X11Server::serverRecvKeyEvent(pressed, keysym);
        _idleSessionTp = std::chrono::steady_clock::now();
    }

    void ConnectorLtsm::serverRecvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy)
    {
        X11Server::serverRecvPointerEvent(mask, posx, posy);
        _idleSessionTp = std::chrono::steady_clock::now();
    }

    bool ConnectorLtsm::isUserSession(void) const
    {
        return _userSession;
    }

    void ConnectorLtsm::systemClientVariables(const JsonObject & jo)
    {
        Application::debug(DebugType::App, "%s: count: %lu", __FUNCTION__, jo.size());

        if(auto env = jo.getObject("environments"))
        {
            busSetSessionEnvironments(displayNum(), env->toStdMap<std::string>());
        }

        if(auto keyboard = jo.getObject("keyboard"))
        {
            auto names = keyboard->getStdVector<std::string>("layouts");
            busSetSessionKeyboardLayouts(displayNum(), names);
            auto layout = keyboard->getString("current");
            auto it = std::find_if(names.begin(), names.end(), [ &](auto & str)
            {
                return Tools::lower(str).substr(0, 2) == Tools::lower(layout).substr(0, 2);
            });

            std::thread([group = std::distance(names.begin(), it), display = xcbDisplay()]()
            {
                if(auto xkb = static_cast<const XCB::ModuleXkb*>(display->getExtension(XCB::Module::XKB)))
                {
                    // wait pause for apply layouts
                    std::this_thread::sleep_for(200ms);
                    xkb->switchLayoutGroup(group);
                }
            }).detach();
        }

        if(auto opts = jo.getObject("options"))
        {
            busSetSessionOptions(displayNum(), opts->toStdMap<std::string>());
            _ltsmClientVersion = opts->getInteger("ltsm:client", 0);
            _x11NoDamage = opts->getBoolean("x11:nodamage", _x11NoDamage);
            _frameRate = opts->getInteger("frame:rate", _frameRate);

            setEncodingOptions(opts->getStdListForward<std::string>("enc:opts"));

            if( _x11NoDamage && ! XCB::RootDisplay::hasError())
            {
                XCB::RootDisplay::extensionDisable(XCB::Module::DAMAGE);
            }
        }
    }

    void ConnectorLtsm::systemCursorFailed(const JsonObject & jo)
    {
        auto cursorId = jo.getInteger("cursor");

        if(cursorId)
        {
    	    Application::debug(DebugType::App, "%s: cursor id: 0x%08" PRIx32, __FUNCTION__, cursorId);
            cursorFailed(cursorId);
        }
    }

    void ConnectorLtsm::systemKeyboardEvent(const JsonObject & jo)
    {
        // event supported
        if(20250808 > _ltsmClientVersion)
        {
            return;
        }

        if(xcbAllowMessages())
        {
            auto pressed = jo.getBoolean("pressed");
            auto scancode = jo.getInteger("scancode");
            auto keycode = jo.getInteger("keycode");

            auto xksym = SDL::Window::convertScanCodeToKeySym(static_cast<SDL_Scancode>(scancode));

            if(xksym == 0)
            {
                xksym = keycode;
            }

            if(auto xkb = static_cast<const XCB::ModuleXkb*>(xcbDisplay()->getExtension(XCB::Module::XKB)))
            {
                int group = xkb->getLayoutGroup();
                auto keycodeGroup = keysymToKeycodeGroup(xksym);

                if(group != keycodeGroup.second)
                {
                    xksym = keycodeGroupToKeysym(keycodeGroup.first, group);
                }
            }

    	    //Application::debug(DebugType::Input, "%s: pressed: %d, scancode: 0x%08" PRIx32 ", keycode: %", __FUNCTION__, (int) pressed, scancode, keycode);

            serverRecvKeyEvent(pressed, xksym);
            X11Server::serverScreenUpdateRequest();
        }
    }

    void ConnectorLtsm::systemKeyboardChange(const JsonObject & jo)
    {
        auto layout = jo.getString("layout");

        if(xcbAllowMessages())
        {
            if(auto xkb = static_cast<const XCB::ModuleXkb*>(xcbDisplay()->getExtension(XCB::Module::XKB)))
            {
                Application::debug(DebugType::App, "%s: layout: %s", __FUNCTION__, layout.c_str());
                auto names = xkb->getNames();
                auto it = std::find_if(names.begin(), names.end(), [ &](auto & str)
                {
                    return Tools::lower(str).substr(0, 2) == Tools::lower(layout).substr(0, 2);
                });

                if(it != names.end())
                {
    		    //Application::debug(DebugType::Input, "%s: group: `s'", __FUNCTION__, it->c_str());
                    xkb->switchLayoutGroup(std::distance(names.begin(), it));
                }
                else
                {
                    Application::error("%s: layout not found: %s, names: [%s]",
                                       __FUNCTION__, layout.c_str(), Tools::join(names.begin(), names.end()).c_str());
                }
            }
        }
    }

    void ConnectorLtsm::systemTransferFiles(const JsonObject & jo)
    {
        if(isUserSession())
        {
            auto fa = jo.getArray("files");

            if(! fa)
            {
                Application::error("%s: incorrect format message", __FUNCTION__);
                return;
            }

            Application::debug(DebugType::App, "%s: files count: %s", __FUNCTION__, fa->size());

            // check transfer disabled
            if(_config.getBoolean("transfer:file:disabled", false))
            {
                Application::error("%s: administrative disable", __FUNCTION__);
                busSendNotify(displayNum(), "Transfer Disable", "transfer is blocked, contact the administrator",
                              NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
                return;
            }

            size_t fmax = 0;
            size_t prettyMb = 0;

            if(_config.hasKey("transfer:file:max"))
            {
                fmax = _config.getInteger("transfer:file:max");
                prettyMb = fmax / (1024 * 1024);
            }

            for(int it = 0; it < fa->size(); ++it)
            {
                auto jo2 = fa->getObject(it);

                if(! jo2)
                {
                    continue;
                }

                std::string fname = jo2->getString("file");
                size_t fsize = jo2->getInteger("size");

                if(std::any_of(_transferPlanned.begin(), _transferPlanned.end(), [ &](auto & st) { return fname == std::get<0>(st); }))
                {
                    Application::warning("%s: found planned and skipped, file: %s", __FUNCTION__, fname.c_str());
                    continue;
                }

                // check max size
                if(fmax && fsize > fmax)
                {
                    Application::warning("%s: file size exceeds and skipped, file: %s", __FUNCTION__, fname.c_str());
                    busSendNotify(displayNum(), "Transfer Skipped",
                                  Tools::StringFormat("the file size exceeds, the allowed limit: %1M, file: %2").arg(prettyMb).arg(fname),
                                  NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
                    continue;
                }

                // add planned transfer
                std::scoped_lock<std::mutex> guard{_lockTransfer};
                _transferPlanned.emplace_back(std::move(fname), fsize);
            }

            size_t freeChannels = countFreeChannels();

            if(_transferPlanned.empty())
            {
                Application::warning("%s: file list empty", __FUNCTION__);
            }
            else if(! freeChannels)
            {
                Application::warning("%s: no free channels", __FUNCTION__);
            }
            else
            {
                std::scoped_lock<std::mutex> guard{_lockTransfer};

                if(_transferPlanned.size() <= freeChannels)
                {
                    // send request to manager
                    busTransferFilesRequest(displayNum(), {_transferPlanned.begin(), _transferPlanned.end() });
                }
                else
                {
                    // transfer background
                    std::thread( & ConnectorLtsm::transferFilesPartial, this, _transferPlanned).detach();
                }
            }
        }
    }

    void ConnectorLtsm::transferFilesPartial(std::list<TupleFileSize> files)
    {
        size_t freeChannels = countFreeChannels() / 3;
        using TimePointSeconds = Tools::TimePoint<std::chrono::seconds>;
        std::unique_ptr<TimePointSeconds> partial;
        auto it1 = files.begin();

        while(it1 != files.end())
        {
            // wait 5s
            if(! partial || partial->check())
            {
                if(! partial)
                {
                    partial = std::make_unique<TimePointSeconds>(5s);
                }

                if(freeChannels > countFreeChannels())
                {
                    continue;
                }

                auto it2 = Tools::nextToEnd(it1, freeChannels, files.end());

                try
                {
                    // send partial request to manager
                    busTransferFilesRequest(displayNum(), { it1, it2 });
                }
                catch(...)
                {
                    break;
                }

                it1 = it2;
            }

            std::this_thread::sleep_for(1s);
        }
    }

    void ConnectorLtsm::onTransferAllow(const int32_t & display, const std::string & filepath, const std::string & tmpfile,
                                        const std::string & dstdir)
    {
        // filepath - client file path
        // tmpfile - server tmpfile
        // dstdir - server target directory
        Application::debug(DebugType::App, "%s: display: %" PRId32, __FUNCTION__, display);

        if(display == displayNum())
        {
            std::scoped_lock<std::mutex> guard{_lockTransfer};
            auto it = std::find_if(_transferPlanned.begin(), _transferPlanned.end(), [ &](auto & st)
            {
                return filepath == std::get<0>(st);
            });

            if(it == _transferPlanned.end())
            {
                Application::error("%s: transfer not found, file: %s", __FUNCTION__, filepath.c_str());
                return;
            }

            // transfer not canceled
            if(! dstdir.empty() && ! tmpfile.empty())
            {
                // create file transfer channel
                createChannel(Channel::UrlMode(Channel::ConnectorType::File, filepath, Channel::ConnectorMode::ReadOnly),
                              Channel::UrlMode(Channel::ConnectorType::File, tmpfile, Channel::ConnectorMode::WriteOnly),
                              Channel::Opts{Channel::Speed::Slow, 0});
                auto dstfile = std::filesystem::path(dstdir) / std::filesystem::path(filepath).filename();
                busTransferFileStarted(displayNum(), tmpfile, std::get<1>(*it) /* size */, dstfile.c_str());
            }

            // remove planned
            _transferPlanned.erase(it);
        }
    }

    void ConnectorLtsm::onCreateChannel(const int32_t & display, const std::string & client, const std::string & cmode,
                                        const std::string & server, const std::string & smode, const std::string & speed)
    {
        if(display == displayNum())
        {
            createChannel(Channel::UrlMode(client, cmode), Channel::UrlMode(server, smode),
                          Channel::Opts{Channel::connectorSpeed(speed), 0});
        }
    }

    void ConnectorLtsm::onDestroyChannel(const int32_t & display, const uint8_t & channel)
    {
        if(display == displayNum())
        {
            destroyChannel(channel);
        }
    }

    void ConnectorLtsm::onCreateListener(const int32_t & display, const std::string & client, const std::string & cmode,
                                         const std::string & server, const std::string & smode, const std::string & speed, const uint8_t & limit,
                                         const uint32_t & flags)
    {
        if(display == displayNum())
        {
            createListener(Channel::UrlMode(client, cmode), Channel::UrlMode(server, smode), limit,
                           Channel::Opts{Channel::connectorSpeed(speed), (int) flags});
        }
    }

    void ConnectorLtsm::onDestroyListener(const int32_t & display, const std::string & client, const std::string & server)
    {
        if(display == displayNum())
        {
            destroyListener(client, server);
        }
    }

    void ConnectorLtsm::onDebugChannel(const int32_t & display, const uint8_t & channel, const bool & debug)
    {
        if(display == displayNum())
        {
            setChannelDebug(channel, debug);
        }
    }

    void ConnectorLtsm::onLoginFailure(const int32_t & display, const std::string & msg)
    {
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::LoginSuccess);
        jos.push("action", false);
        jos.push("error", msg);
        static_cast<ChannelClient*>(this)->sendLtsmChannelData(static_cast<uint8_t>(ChannelType::System), jos.flush());
    }

    void ConnectorLtsm::systemChannelError(const JsonObject & jo)
    {
        auto channel = jo.getInteger("id");
        auto code = jo.getInteger("code");
        auto err = jo.getString("error");
        Application::info("%s: channel: %d, errno: %d, display: %d, error: `%s'", __FUNCTION__, channel, displayNum(), code,
                          err.c_str());

        if(isUserSession())
            busSendNotify(displayNum(), "Channel Error", err.append(", errno: ").append(std::to_string(code)),
                          NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
    }

    bool ConnectorLtsm::noVncMode(void) const
    {
        return _remoteaddr == "127.0.0.1" &&
                _config.getBoolean("vnc:novnc:allow", false);
    }

    std::string ConnectorLtsm::remoteClientAddress(void) const
    {
        return _remoteaddr;
    }

    int ConnectorLtsm::remoteClientVersion(void) const
    {
        return _ltsmClientVersion;
    }
}
