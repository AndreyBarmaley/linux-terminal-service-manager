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
#include "ltsm_connector_vnc.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_channels.h"

using namespace std::chrono_literals;

namespace LTSM
{
    /* Connector::VNC */
    Connector::VNC::~VNC()
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

    int Connector::VNC::communication(void)
    {
        if(0 >= busGetServiceVersion())
        {
            Application::error("%s: bus service failure", __FUNCTION__);
            return EXIT_FAILURE;
        }

        Application::info("%s: remote addr: %s", __FUNCTION__, _remoteaddr.c_str());

        x11NoDamage = _config->getBoolean("vnc:xcb:nodamage", false);
        frameRate = _config->getInteger("vnc:frame:rate", 16);

        if(frameRate <= 0)
        {
            Application::warning("%s: invalid value for: `%s'", __FUNCTION__, "vnc:frame:rate");
            frameRate = 16;
        }
        
        return rfbCommunication();
    }

    void Connector::VNC::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t & userUid)
    {
        if(display == displayNum())
        {
            xcbDisableMessages(true);
            waitUpdateProcess();
            shmUid = userUid;
            Application::notice("%s: dbus signal, display: %" PRId32 ", username: %s, uid: %" PRIu32, __FUNCTION__, display,
                                userName.c_str(), userUid);
            int oldDisplay = displayNum();
            int newDisplay = busStartUserSession(oldDisplay, getpid(), userName, _remoteaddr, _conntype);

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

            xcbShmInit(userUid);
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
                if(! x11NoDamage)
                {
                    X11Server::serverScreenUpdateRequest();
                }
            }

            idleTimeoutSec = _config->getInteger("idle:action:timeout", 0);
            idleSession = std::chrono::steady_clock::now();
            userSession = true;
            std::thread([this]()
            {
                JsonObjectStream jos;
                jos.push("cmd", SystemCommand::LoginSuccess);
                jos.push("action", true);
                static_cast<ChannelClient*>(this)->sendLtsmChannelData(Channel::System, jos.flush());
            }).detach();
        }
    }

    void Connector::VNC::onShutdownConnector(const int32_t & display)
    {
        if(display == displayNum())
        {
            xcbDisableMessages(true);
            waitUpdateProcess();
            rfbMessagesShutdown();
            Application::notice("%s: dbus signal, display: %" PRId32, __FUNCTION__, display);
        }
    }

    void Connector::VNC::onHelperWidgetStarted(const int32_t & display)
    {
        if(display == displayNum())
        {
            Application::info("%s: dbus signal, display: %" PRId32, __FUNCTION__, display);
            loginWidgetStarted = true;
        }
    }

    void Connector::VNC::onSendBellSignal(const int32_t & display)
    {
        if(display == displayNum())
        {
            Application::info("%s: dbus signal, display: %" PRId32, __FUNCTION__, display);
            std::thread([this] { this->sendBellEvent(); }).detach();
        }
    }

    const PixelFormat & Connector::VNC::serverFormat(void) const
    {
        return serverPf;
    }

    void Connector::VNC::serverFrameBufferModifyEvent(FrameBuffer & fb) const
    {
        renderPrimitivesToFB(fb);
    }

    void Connector::VNC::serverHandshakeVersionEvent(void)
    {
        // Xvfb: session request
        int screen = busStartLoginSession(getpid(), 24, _remoteaddr, "vnc");

        if(screen <= 0)
        {
            Application::error("%s: login session request: failure", __FUNCTION__);
            throw vnc_error(NS_FuncName);
        }

        Application::info("%s: login session request success, display: %d", __FUNCTION__, screen);

        if(! xcbConnect(screen, *this))
        {
            Application::error("%s: xcb connect: failed", __FUNCTION__);
            throw vnc_error(NS_FuncName);
        }

        const xcb_visualtype_t* visual = xcbDisplay()->visual();

        if(! visual)
        {
            Application::error("%s: xcb visual empty", __FUNCTION__);
            throw vnc_error(NS_FuncName);
        }

        Application::debug(DebugType::Conn, "%s: xcb max request: %u", __FUNCTION__, xcbDisplay()->getMaxRequest());
        // init server format
        serverPf = PixelFormat(xcbDisplay()->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);

        // load keymap
        if(_config->hasKey("vnc:keymap:file"))
        {
            auto file = _config->getString("vnc:keymap:file");

            if(! file.empty())
            {
                JsonContentFile jc(file);

                if(jc.isValid() && jc.isObject())
                {
                    auto jo = jc.toObject();

                    for(auto & skey : jo.keys())
                    {
                        try
                        {
                            keymap.emplace(std::stoi(skey, nullptr, 0), jo.getInteger(skey));
                        }
                        catch(const std::exception &) { }
                    }
                }
            }
        }
    }

    std::forward_list<std::string> Connector::VNC::serverDisabledEncodings(void) const
    {
        return _config->getStdListForward<std::string>("vnc:encoding:blacklist");
    }

    void Connector::VNC::serverEncodingSelectedEvent(void)
    {
        setEncodingThreads(_config->getInteger("vnc:encoding:threads", 2));
        setEncodingDebug(_config->getInteger("vnc:encoding:debug", 0));
    }

    void Connector::VNC::serverMainLoopEvent(void)
    {
        // check idle timeout
        if(idleTimeoutSec && std::chrono::seconds(idleTimeoutSec) < std::chrono::steady_clock::now() - idleSession)
        {
            busIdleTimeoutAction(displayNum());
            idleSession = std::chrono::steady_clock::now();
        }
    }

    void Connector::VNC::serverDisplayResizedEvent(const XCB::Size & sz)
    {
        xcbShmInit(shmUid);
        busDisplayResized(displayNum(), sz.width, sz.height);
    }

    void Connector::VNC::serverEncodingsEvent(void)
    {
        if(isClientLtsmSupported())
        {
            sendEncodingLtsmSupported();
        }
    }

    void Connector::VNC::serverConnectedEvent(void)
    {
        // wait widget started signal(onHelperWidgetStarted), 3000ms, 10 ms pause
        bool waitWidgetStarted = Tools::waitCallable<std::chrono::milliseconds>(3000, 10, [=]()
        {
            return ! this->loginWidgetStarted;
        });

        if(! waitWidgetStarted)
        {
            Application::info("%s: wait loginWidgetStarted failed", "serverConnectedEvent");
            throw vnc_error(NS_FuncName);
        }

#ifdef LTSM_WITH_GSSAPI
        auto info = ServerEncoder::authInfo();

        if(! info.first.empty())
        {
            std::thread([this, login = info.first]()
            {
                this->helperSetSessionLoginPassword(this->displayNum(), login, "", false);
                // not so fast
                std::this_thread::sleep_for(300ms);
                this->busSetAuthenticateToken(this->displayNum(), login);
            }).detach();
        }

#endif
    }

    void Connector::VNC::serverSecurityInitEvent(void)
    {
        busSetEncryptionInfo(displayNum(), serverEncryptionInfo());
    }

    RFB::SecurityInfo Connector::VNC::rfbSecurityInfo(void) const
    {
        RFB::SecurityInfo secInfo;
        secInfo.authNone = true;
        secInfo.authVnc = false;
        secInfo.authVenCrypt = ! _config->getBoolean("vnc:gnutls:disable", false);
        secInfo.tlsPriority = _config->getString("vnc:gnutls:priority", "NORMAL:+ANON-ECDH:+ANON-DH");
        secInfo.tlsAnonMode = _config->getBoolean("vnc:gnutls:anonmode", true);
        secInfo.caFile = _config->getString("vnc:gnutls:cafile");
        secInfo.certFile = _config->getString("vnc:gnutls:certfile");
        secInfo.keyFile = _config->getString("vnc:gnutls:keyfile");
        secInfo.crlFile = _config->getString("vnc:gnutls:crlfile");
        secInfo.tlsDebug = _config->getInteger("vnc:gnutls:debug", 0);
#ifdef LTSM_WITH_GSSAPI
        secInfo.authKrb5 = true;
        secInfo.krb5Service = _config->getString("vnc:kerberos:service", "TERMSRV");
#endif

        if(secInfo.authKrb5)
        {
            auto keytab = _config->getString("vnc:kerberos:keytab", "/etc/ltsm/termsrv.keytab");

            if(! keytab.empty())
            {
                std::filesystem::path file(keytab);
                std::error_code err;

                if(std::filesystem::exists(keytab, err))
                {
                    Application::info("%s: set KRB5_KTNAME=`%s'", __FUNCTION__, keytab.c_str());
                    setenv("KRB5_KTNAME", keytab.c_str(), 1);
                    auto debug = _config->getString("vnc:kerberos:trace");

                    if(! debug.empty())
                    {
                        Application::info("%s: set KRB5_TRACE=`%s'", __FUNCTION__, debug.c_str());
                        setenv("KRB5_TRACE", debug.c_str(), 1);
                    }
                }
                else
                {
                    Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"),
                                       keytab.c_str(), getuid());
                }
            }
        }

        return secInfo;
    }

    bool Connector::VNC::rfbClipboardEnable(void) const
    {
        return _config->getBoolean("vnc:clipboard");
    }

    bool Connector::VNC::rfbDesktopResizeEnabled(void) const
    {
        return true;
    }

    bool Connector::VNC::xcbAllowMessages(void) const
    {
        return SignalProxy::xcbAllowMessages();
    }

    void Connector::VNC::serverScreenUpdateRequest(const XCB::Region & reg)
    {
        if(xcbAllowMessages() && ! x11NoDamage)
        {
            X11Server::serverScreenUpdateRequest(reg);
        }
    }

    size_t Connector::VNC::frameRateOption(void) const
    {
        return frameRate;
    }

    bool Connector::VNC::xcbNoDamageOption(void) const
    {
        return isClientLtsmSupported() ?
            static_cast<bool>(x11NoDamage) : false;
    }

    void Connector::VNC::xcbDisableMessages(bool f)
    {
        SignalProxy::xcbDisableMessages(f);
    }

    int Connector::VNC::rfbUserKeycode(uint32_t keysym) const
    {
        auto it = keymap.find(keysym);
        return it != keymap.end() ? it->second : 0;
    }

    void Connector::VNC::serverRecvKeyEvent(bool pressed, uint32_t keysym)
    {
        X11Server::serverRecvKeyEvent(pressed, keysym);
        idleSession = std::chrono::steady_clock::now();
    }

    void Connector::VNC::serverRecvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy)
    {
        X11Server::serverRecvPointerEvent(mask, posx, posy);
        idleSession = std::chrono::steady_clock::now();
    }

    bool Connector::VNC::isUserSession(void) const
    {
        return userSession;
    }

    void Connector::VNC::systemClientVariables(const JsonObject & jo)
    {
        Application::debug(DebugType::Conn, "%s: count: %u", __FUNCTION__, jo.size());

        if(auto env = jo.getObject("environments"))
        {
            busSetSessionEnvironments(displayNum(), env->toStdMap<std::string>());
        }

        if(auto keyboard = jo.getObject("keyboard"))
        {
            auto names = keyboard->getStdVector<std::string>("layouts");
            busSetSessionKeyboardLayouts(displayNum(), names);
            auto layout = keyboard->getString("current");
            auto it = std::find_if(names.begin(), names.end(), [&](auto & str)
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
            x11NoDamage = opts->getBoolean("x11:nodamage", x11NoDamage);
            frameRate = opts->getInteger("frame:rate", frameRate);

            setEncodingOptions(opts->getStdListForward<std::string>("enc:opts"));

            if( x11NoDamage && ! XCB::RootDisplay::hasError())
            {
                XCB::RootDisplay::extensionDisable(XCB::Module::DAMAGE);
            }
        }
    }

    void Connector::VNC::systemKeyboardChange(const JsonObject & jo)
    {
        auto layout = jo.getString("layout");

        if(xcbAllowMessages())
        {
            if(auto xkb = static_cast<const XCB::ModuleXkb*>(xcbDisplay()->getExtension(XCB::Module::XKB)))
            {
                Application::debug(DebugType::Conn, "%s: layout: %s", __FUNCTION__, layout.c_str());
                auto names = xkb->getNames();
                auto it = std::find_if(names.begin(), names.end(), [&](auto & str)
                {
                    return Tools::lower(str).substr(0, 2) == Tools::lower(layout).substr(0, 2);
                });

                if(it != names.end())
                {
                    xkb->switchLayoutGroup(std::distance(names.begin(), it));
                }
                else
                    Application::error("%s: layout not found: %s, names: [%s]",
                                       __FUNCTION__, layout.c_str(), Tools::join(names.begin(), names.end()).c_str());
            }
        }
    }

    void Connector::VNC::systemTransferFiles(const JsonObject & jo)
    {
        if(isUserSession())
        {
            auto fa = jo.getArray("files");

            if(! fa)
            {
                Application::error("%s: incorrect format message", __FUNCTION__);
                return;
            }

            Application::debug(DebugType::Conn, "%s: files count: %s", __FUNCTION__, fa->size());

            // check transfer disabled
            if(_config->getBoolean("transfer:file:disabled", false))
            {
                Application::error("%s: administrative disable", __FUNCTION__);
                busSendNotify(displayNum(), "Transfer Disable", "transfer is blocked, contact the administrator",
                              NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
                return;
            }

            size_t fmax = 0;
            size_t prettyMb = 0;

            if(_config->hasKey("transfer:file:max"))
            {
                fmax = _config->getInteger("transfer:file:max");
                prettyMb = fmax / (1024 * 1024);
            }

            std::vector<sdbus::Struct<std::string, uint32_t>> files;
            std::scoped_lock<std::mutex> guard(lockTransfer);

            for(int it = 0; it < fa->size(); ++it)
            {
                auto jo2 = fa->getObject(it);

                if(! jo2)
                {
                    continue;
                }

                std::string fname = jo2->getString("file");
                size_t fsize = jo2->getInteger("size");

                if(std::any_of(transfer.begin(), transfer.end(), [&](auto & st) { return st.first == fname; }))
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
                transfer.emplace_back(fname, fsize);
                // add target
                files.emplace_back(std::move(fname), fsize);
            }

            size_t channels = countFreeChannels();

            if(files.empty())
            {
                Application::warning("%s: file list empty", __FUNCTION__);
            }
            else if(! channels)
            {
                Application::warning("%s: no free channels", __FUNCTION__);
            }
            else
            {
                if(files.size() > channels)
                {
                    Application::warning("%s: files list is large, count: %u, channels: %u", __FUNCTION__, files.size(), channels);
                    files.resize(channels);
                }

                // send request to manager
                busTransferFilesRequest(displayNum(), files);
            }
        }
    }

    void Connector::VNC::onTransferAllow(const int32_t & display, const std::string & filepath, const std::string & tmpfile,
                                         const std::string & dstdir)
    {
        // filepath - client file path
        // tmpfile - server tmpfile
        // dstdir - server target directory
        Application::debug(DebugType::Conn, "%s: display: %" PRId32, __FUNCTION__, display);

        if(display == displayNum())
        {
            std::scoped_lock<std::mutex> guard(lockTransfer);
            auto it = std::find_if(transfer.begin(), transfer.end(), [&](auto & st)
            {
                return st.first == filepath;
            });

            if(it == transfer.end())
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
                busTransferFileStarted(displayNum(), tmpfile, (*it).second, dstfile.c_str());
            }

            // remove planned
            transfer.erase(it);
        }
    }

    void Connector::VNC::onCreateChannel(const int32_t & display, const std::string & client, const std::string & cmode,
                                         const std::string & server, const std::string & smode, const std::string & speed)
    {
        if(display == displayNum())
        {
            createChannel(Channel::UrlMode(client, cmode), Channel::UrlMode(server, smode),
                          Channel::Opts{Channel::connectorSpeed(speed), 0});
        }
    }

    void Connector::VNC::onDestroyChannel(const int32_t & display, const uint8_t & channel)
    {
        if(display == displayNum())
        {
            destroyChannel(channel);
        }
    }

    void Connector::VNC::onCreateListener(const int32_t & display, const std::string & client, const std::string & cmode,
                                          const std::string & server, const std::string & smode, const std::string & speed, const uint8_t & limit,
                                          const uint32_t & flags)
    {
        if(display == displayNum())
        {
            createListener(Channel::UrlMode(client, cmode), Channel::UrlMode(server, smode), limit,
                           Channel::Opts{Channel::connectorSpeed(speed), (int) flags});
        }
    }

    void Connector::VNC::onDestroyListener(const int32_t & display, const std::string & client, const std::string & server)
    {
        if(display == displayNum())
        {
            destroyListener(client, server);
        }
    }

    void Connector::VNC::onDebugChannel(const int32_t & display, const uint8_t & channel, const bool & debug)
    {
        if(display == displayNum())
        {
            setChannelDebug(channel, debug);
        }
    }

    void Connector::VNC::onLoginFailure(const int32_t & display, const std::string & msg)
    {
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::LoginSuccess);
        jos.push("action", false);
        jos.push("error", msg);
        static_cast<ChannelClient*>(this)->sendLtsmChannelData(Channel::System, jos.flush());
    }

    void Connector::VNC::systemChannelError(const JsonObject & jo)
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
}
