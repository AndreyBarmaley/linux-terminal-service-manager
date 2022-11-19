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

#include <sys/stat.h>

#include "ltsm_tools.h"
#include "ltsm_connector_vnc.h"
#include "ltsm_channels.h"

using namespace std::chrono_literals;

namespace LTSM
{
#ifdef LTSM_CHANNELS
    /* FuseSessionProxy */
    FuseSessionProxy::FuseSessionProxy(const std::string& address, ChannelClient & client)
#ifdef SDBUS_ADDRESS_SUPPORT
        : ProxyInterfaces(sdbus::createSessionBusConnectionWithAddress(address), LTSM::dbus_session_fuse_name, LTSM::dbus_session_fuse_path), sender(& client)
#else
        // not working
        // build test only
        : ProxyInterfaces(sdbus::createSessionBusConnection(), LTSM::dbus_session_fuse_name, LTSM::dbus_session_fuse_path), sender(& client)
#endif
    {
        registerProxy();
    }
        
    FuseSessionProxy::~FuseSessionProxy()
    {
        unregisterProxy();
    }

    void FuseSessionProxy::onRequestOpen(const std::string& path, const uint32_t& cookie, const int32_t& flags)
    {
        JsonObjectStream jos;

        jos.push("cmd", SystemCommand::FuseProxy);
        jos.push("fuse", "open");
        jos.push("path", path);
        jos.push("cookie", static_cast<size_t>(cookie));
        jos.push("flags", flags);

        sender->sendLtsmEvent(Channel::System, jos.flush());
    }

    void FuseSessionProxy::onRequestRead(const std::string& path, const uint32_t& cookie, const uint32_t& size, const int32_t& offset)
    {   
        JsonObjectStream jos;

        jos.push("cmd", SystemCommand::FuseProxy);
        jos.push("fuse", "read");
        jos.push("path", path);
        jos.push("cookie", static_cast<size_t>(cookie));
        jos.push("size", static_cast<size_t>(size));
        jos.push("offset", offset);

        sender->sendLtsmEvent(Channel::System, jos.flush());
    }

    void FuseSessionProxy::onRequestReadDir(const std::string& path, const uint32_t& cookie)
    {   
        JsonObjectStream jos;

        jos.push("cmd", SystemCommand::FuseProxy);
        jos.push("fuse", "readdir");
        jos.push("path", path);
        jos.push("cookie", static_cast<size_t>(cookie));

        sender->sendLtsmEvent(Channel::System, jos.flush());
    }

    void FuseSessionProxy::onRequestGetAttr(const std::string& path, const uint32_t& cookie)
    {
        JsonObjectStream jos;

        jos.push("cmd", SystemCommand::FuseProxy);
        jos.push("fuse", "getattr");
        jos.push("path", path);
        jos.push("cookie", static_cast<size_t>(cookie));

        sender->sendLtsmEvent(Channel::System, jos.flush());
    }
#endif

    /* Connector::VNC */
    Connector::VNC::~VNC()
    {
#ifdef LTSM_CHANNELS
        if(fuse)
        {
            try
            {
                fuse->umount();
            }
            catch(const sdbus::Error & err)
            {
            }
        }
#endif
        if(0 < displayNum())
        {
            busConnectorTerminated(displayNum());
            clientDisconnectedEvent(displayNum());
        }
    }

    int Connector::VNC::communication(void)
    {
        if(0 >= busGetServiceVersion())
        {
            Application::error("%s: bus service failure", __FUNCTION__);
            return EXIT_FAILURE;
        }

        Application::info("%s: remote addr: %s", __FUNCTION__, _remoteaddr.c_str());

        setEncodingThreads(_config->getInteger("vnc:encoding:threads", 2));
        setEncodingDebug(_config->getInteger("vnc:encoding:debug", 0));

        return rfbCommunication();
    }

    void Connector::VNC::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t& userUid)
    {
        if(0 < displayNum() && display == displayNum())
        {
            setEnableXcbMessages(false);
            waitUpdateProcess();
            shmExt.first.reset();

            SignalProxy::onLoginSuccess(display, userName, userUid);

            shmExt.second = userUid;
            xcbShmInit(xcbDisplay()->size());

            setEnableXcbMessages(true);
            auto & clientRegion = getClientRegion();

            // fix new session size
            if(_xcbDisplay->size() != clientRegion.toSize())
            {
                Application::warning("%s: remote request desktop size [%dx%d], display: %d", __FUNCTION__, clientRegion.width, clientRegion.height, displayNum());

                if(0 < _xcbDisplay->setRandrScreenSize(clientRegion.width, clientRegion.height))
                    Application::info("%s: change session size [%dx%d], display: %d", __FUNCTION__, clientRegion.width, clientRegion.height, displayNum());
            }

            // full update
            _xcbDisplay->damageAdd(XCB::Region(0, 0, clientRegion.width, clientRegion.height));
            //Application::notice("%s: dbus signal, display: %d, username: %s", __FUNCTION__, displayNum(), userName.c_str());

            idleTimeoutSec = _config->getInteger("idle:action:timeout", 0);
            idleSession = std::chrono::steady_clock::now();

#ifdef LTSM_CHANNELS
            userSession =  true;

            std::thread([this]
            {
                JsonObjectStream jos;
                jos.push("cmd", SystemCommand::LoginSuccess);
                jos.push("action", true);

                static_cast<ChannelClient*>(this)->sendLtsmEvent(Channel::System, jos.flush());
            }).detach();
#endif
        }
    }

    void Connector::VNC::onShutdownConnector(const int32_t & display)
    {
        if(0 < displayNum() && display == displayNum())
        {
            setEnableXcbMessages(false);
            waitUpdateProcess();
            rfbMessagesShutdown();
            Application::notice("%s: dbus signal, display: %d", __FUNCTION__, display);
        }
    }

    void Connector::VNC::onHelperWidgetStarted(const int32_t & display)
    {
        if(0 < displayNum() && display == displayNum())
        {
            Application::info("%s: dbus signal, display: %d", __FUNCTION__, display);
            loginWidgetStarted = true;
        }
    }

    void Connector::VNC::onSendBellSignal(const int32_t & display)
    {
        if(0 < displayNum() && display == displayNum())
        {
            Application::info("%s: dbus signal, display: %d", __FUNCTION__, display);

            std::thread([this]{ this->sendBellEvent(); }).detach();
        }
    }

    const PixelFormat & Connector::VNC::serverFormat(void) const
    {
        return format;
    }

    void Connector::VNC::xcbFrameBufferModify(FrameBuffer & fb) const
    {
        renderPrimitivesToFB(fb);
    }

    void Connector::VNC::serverHandshakeVersionEvent(void)
    {
        // Xvfb: session request
        int screen = busStartLoginSession(24, _remoteaddr, "vnc");
        if(screen <= 0)
        {
            Application::error("%s: login session request: failure", __FUNCTION__);
            throw vnc_error(NS_FuncName);
        }

        Application::info("%s: login session request success, display: %d", __FUNCTION__, screen);

        if(! xcbConnect(screen))
        {
            Application::error("%s: xcb connect: failed", __FUNCTION__);
            throw vnc_error(NS_FuncName);
        }

        const xcb_visualtype_t* visual = _xcbDisplay->visual();
        if(! visual)
        {
            Application::error("%s: xcb visual empty", __FUNCTION__);
            throw vnc_error(NS_FuncName);
        }

        Application::debug("%s: xcb max request: %d", __FUNCTION__, _xcbDisplay->getMaxRequest());

        // init server format
        format = PixelFormat(_xcbDisplay->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);
    }

    void Connector::VNC::serverSelectEncodingsEvent(void)
    {
        // set disabled and preffered encodings
        setDisabledEncodings(_config->getStdList<std::string>("vnc:encoding:blacklist"));
        setPrefferedEncodings(_config->getStdList<std::string>("vnc:encoding:preflist"));

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
                        try { keymap.emplace(std::stoi(skey, nullptr, 0), jo.getInteger(skey)); } catch(const std::exception &) { }
                    }
                }
            }
        }
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
        xcbShmInit(sz);
        busDisplayResized(displayNum(), sz.width, sz.height);
    }

    void Connector::VNC::serverEncodingsEvent(void)
    {
#ifdef LTSM_CHANNELS
        if(isClientEncodings(RFB::ENCODING_LTSM))
            sendEncodingLtsmSupported();
#endif
    }

    void Connector::VNC::serverConnectedEvent(void)
    {
        xcbShmInit(xcbDisplay()->size());

        // wait widget started signal(onHelperWidgetStarted), 3000ms, 10 ms pause
        if(! Tools::waitCallable<std::chrono::milliseconds>(3000, 10,
                [=](){ return ! this->loginWidgetStarted; }))
        {
            Application::info("%s: wait loginWidgetStarted failed", "serverConnectedEvent");
            throw vnc_error(NS_FuncName);
        }
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
    
    XCB::SharedDisplay Connector::VNC::xcbDisplay(void) const
    {
        return _xcbDisplay;
    }

    const XCB::SHM* Connector::VNC::xcbShm(void) const
    {
        return & shmExt.first;
    }

    bool Connector::VNC::xcbNoDamage(void) const
    {
        return _config->getBoolean("vnc:xcb:nodamage", false);
    }

    bool Connector::VNC::xcbAllow(void) const
    {
        return isAllowXcbMessages();
    }
        
    void Connector::VNC::setXcbAllow(bool f)
    {
        setEnableXcbMessages(f);
    }

    void Connector::VNC::xcbShmInit(const XCB::Size & dsz)
    {
        if(auto xcb = xcbDisplay())
        {
            const size_t pagesz = 4096;
            auto bpp = xcb->bitsPerPixel() >> 3;
            auto shmsz = ((dsz.width * dsz.height * bpp / pagesz) + 1) * pagesz;

            shmExt.first = xcb->createSHM(shmsz, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, false, shmExt.second);
        }
    }

    int Connector::VNC::rfbUserKeycode(uint32_t keysym) const
    {
        auto it = keymap.find(keysym);
        return it != keymap.end() ? it->second : 0;
    }

    void Connector::VNC::recvKeyEvent(bool pressed, uint32_t keysym)
    {
        X11Server::recvKeyEvent(pressed, keysym);
        idleSession = std::chrono::steady_clock::now();
    }

    void Connector::VNC::recvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy)
    {
        X11Server::recvPointerEvent(mask, posx, posy);
        idleSession = std::chrono::steady_clock::now();
    }

#ifdef LTSM_CHANNELS
    bool Connector::VNC::isUserSession(void) const
    {
        return userSession;
    }

    void Connector::VNC::systemClientVariables(const JsonObject & jo)
    {
        Application::debug("%s: count: %d", __FUNCTION__, jo.size());

        if(auto env = jo.getObject("environments"))
            busSetSessionEnvironments(displayNum(), env->toStdMap<std::string>());

        if(auto keyboard = jo.getObject("keyboard"))
        {
            auto names = keyboard->getStdVector<std::string>("layouts");
            busSetSessionKeyboardLayouts(displayNum(), names);

            auto layout = keyboard->getString("current");

            auto it = std::find_if(names.begin(), names.end(), [&](auto & str)
                    { return Tools::lower(str).substr(0,2) == Tools::lower(layout).substr(0,2); });

            std::thread([group = std::distance(names.begin(), it), display = _xcbDisplay]()
            {
                // wait pause for apply layouts
                std::this_thread::sleep_for(200ms);
                display->switchXkbLayoutGroup(group);
            }).detach();
        }

        if(auto opts = jo.getObject("options"))
        {
            busSetSessionOptions(displayNum(), opts->toStdMap<std::string>());
        }
    }

    void Connector::VNC::systemKeyboardChange(const JsonObject & jo)
    {
        auto layout = jo.getString("layout");

        if(isAllowXcbMessages())
        {
            Application::debug("%s: layout: %s", __FUNCTION__, layout.c_str());

            auto names = _xcbDisplay->getXkbNames();

            auto it = std::find_if(names.begin(), names.end(), [&](auto & str)
                        { return Tools::lower(str).substr(0,2) == Tools::lower(layout).substr(0,2); });

            if(it != names.end())
                _xcbDisplay->switchXkbLayoutGroup(std::distance(names.begin(), it));
            else
                Application::error("%s: layout not found: %s, names: [%s]", __FUNCTION__, layout.c_str(), Tools::join(names).c_str());
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

            Application::debug("%s: files count: %s", __FUNCTION__, fa->size());

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
                    continue;

                std::string fname = jo2->getString("file");
                size_t fsize = jo2->getInteger("size");

                if(std::any_of(transfer.begin(), transfer.end(), [&](auto & st){ return st.first == fname; }))
                {
                    Application::warning("%s: found planned and skipped, file: %s", __FUNCTION__, fname.c_str());
                    continue;
                }

                // check max size
                if(fmax && fsize > fmax)
                {
                    Application::warning("%s: file size exceeds and skipped, file: %s", __FUNCTION__, fname.c_str());
                    busSendNotify(displayNum(), "Transfer Skipped", Tools::StringFormat("the file size exceeds, the allowed limit: %1M, file: %2").arg(prettyMb).arg(fname),
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
            else
            if(! channels)
            {
                Application::warning("%s: no free channels", __FUNCTION__);
            }
            else
            {
                if(files.size() > channels)
                {
                    Application::warning("%s: files list is large, count: %d, channels: %d", __FUNCTION__, files.size(), channels);
                    files.resize(channels);
                }

                // send request to manager
                busTransferFilesRequest(displayNum(), files);
            }
        }
    }

    void Connector::VNC::onTransferAllow(const int32_t& display, const std::string& filepath, const std::string& tmpfile, const std::string & dstdir)
    {
        // filepath - client file path
        // tmpfile - server tmpfile
        // dstdir - server target directory
        Application::debug("%s: display: %d", __FUNCTION__, display);

        if(0 < displayNum() && display == displayNum())
        {
            std::scoped_lock<std::mutex> guard(lockTransfer);

            auto it = std::find_if(transfer.begin(), transfer.end(), [&](auto & st){ return st.first == filepath; });
            if(it == transfer.end())
            {
                Application::error("%s: transfer not found, file: %s", __FUNCTION__, filepath.c_str());
                return;
            }

            // transfer not canceled
            if(! dstdir.empty() && ! tmpfile.empty())
            {
                // create file transfer channel
                createChannel(Channel::createUrl(Channel::ConnectorType::File, filepath), Channel::ConnectorMode::ReadOnly,
                        Channel::createUrl(Channel::ConnectorType::File, tmpfile), Channel::ConnectorMode::WriteOnly, Channel::Speed::Slow);

                auto dstfile = std::filesystem::path(dstdir) / std::filesystem::path(filepath).filename();
                busTransferFileStarted(displayNum(), tmpfile, (*it).second, dstfile.c_str());
            }

            // remove planned
            transfer.erase(it);
        }
    }

    void Connector::VNC::onCreateChannel(const int32_t & display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode, const std::string& speed)
    {
        if(0 < displayNum() && display == displayNum())
        {
            createChannel(client, Channel::connectorMode(cmode), server, Channel::connectorMode(smode), Channel::connectorSpeed(speed));
        }
    }

    void Connector::VNC::onDestroyChannel(const int32_t& display, const uint8_t& channel)
    {
        if(0 < displayNum() && display == displayNum())
        {
            destroyChannel(channel);
        }
    }

    void Connector::VNC::onCreateListener(const int32_t& display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode, const std::string& speed, const uint8_t& limit)
    {
        if(0 < displayNum() && display == displayNum())
        {
            createListener(client, Channel::connectorMode(cmode), server, Channel::ListenMode{Channel::connectorMode(smode), limit}, Channel::connectorSpeed(speed));
        }
    }

    void Connector::VNC::onDestroyListener(const int32_t& display, const std::string& client, const std::string& server)
    {
        if(0 < displayNum() && display == displayNum())
        {
            destroyListener(client, server);
        }
    }

    void Connector::VNC::onDebugChannel(const int32_t& display, const uint8_t& channel, const bool& debug)
    {
        if(0 < displayNum() && display == displayNum())
        {
            setChannelDebug(channel, debug);
        }
    }

    void Connector::VNC::systemFuseProxy(const JsonObject & jo)
    {
        std::string cmd = jo.getString("fuse");
        std::string path = jo.getString("path");
        size_t cookie = jo.getInteger("cookie");
        bool error = jo.getBoolean("error");
        int errno2 = jo.getInteger("errno");

        if(error)
            Application::warning("%s: fuse failed: %s, display: %d, path: `%s', cookie: %d, errno: %d", __FUNCTION__, cmd.c_str(), displayNum(), path.c_str(), cookie, errno2);
        else
            Application::debug("%s: fuse cmd: %s, display: %d, path: `%s', cookie: %d", __FUNCTION__, cmd.c_str(), displayNum(), path.c_str(), cookie);

        if(! fuse)
        {
            Application::warning("%s: fuse not started, display: %d", __FUNCTION__, displayNum());
            return;
        }

        try
        {
            if(cmd == "getattr")
            {
                if(auto st = jo.getObject("stat"))
                    fuse->replyGetAttr(error, errno2, path, cookie, st->toStdMap<int>());
            }
            else
            if(cmd == "readdir")
            {
                if(auto ja = jo.getArray("names"))
                    fuse->replyReadDir(error, errno2, path, cookie, ja->toStdVector<std::string>());
            }
            else
            if(cmd == "open")
            {
                fuse->replyOpen(error, errno2, path, cookie);
            }
            else
            if(cmd == "read")
            {
                fuse->replyRead(error, errno2, path, cookie, jo.getString("data"));
            }
            else
            {
                Application::warning("%s: unknown cmd: %s, display: %d", __FUNCTION__, cmd.c_str(), displayNum());
            }
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: sdbus %s: %s, display: %d", __FUNCTION__, err.getName().c_str(), err.getMessage().c_str(), displayNum());
            fuse.reset();
        }
    }

    void Connector::VNC::onFuseSessionStart(const int32_t& display, const std::string& dbusAddresses, const std::string& mountPoint)
    {
        Application::info("%s: display: %d, dbus address: %s, mount point: %s", __FUNCTION__, display, dbusAddresses.c_str(), mountPoint.c_str());
        int ver = 0;

        if(0 < displayNum() && display == displayNum())
        {
            try
            {
#ifdef SDBUS_ADDRESS_SUPPORT
                fuse = std::make_unique<FuseSessionProxy>(dbusAddresses, static_cast<ChannelClient &>(*this));
                ver = fuse->getVersion();
#else
                Application::warning("%s: sdbus address not supported, use 1.2 version", __FUNCTION__);
#endif
            }
            catch(const sdbus::Error & err)
            {
                Application::error("%s: sdbus %s: %s, display: %d", __FUNCTION__, err.getName().c_str(), err.getMessage().c_str(), display);
                fuse.reset();
            }

            if(0 < ver)
            {
                if(! fuse->mount(mountPoint))
                {
                    Application::warning("%s: %s: failed, path: `%s'", __FUNCTION__, "fuse mount", mountPoint.c_str());
                    fuse->shutdown();
                    fuse.reset();
                }
            }
        }
    }

    void Connector::VNC::systemTokenAuth(const JsonObject & jo)
    {
        std::string action = jo.getString("action");
        std::string serial = jo.getString("serial");

        LTSM::Application::info("%s: action: %s, display: %d, serial: %s", __FUNCTION__, action.c_str(), displayNum(), serial.c_str());

        if(action == "attach")
        {
            if(auto ja = jo.getArray("certs"))
                tokenAuthAttached(displayNum(), serial, jo.getString("description"), ja->toStdVector<std::string>());
        }
        else
        if(action == "detach")
        {
            tokenAuthDetached(displayNum(), serial);
        }
        else
        if(action == "reply")
        {
            tokenAuthReply(displayNum(), serial, jo.getInteger("cert"), jo.getString("decrypt"));
        }
        else
        {
            Application::warning("%s: unknown action: %s, display: %d", __FUNCTION__, action.c_str(), displayNum());
        }
    }

    void Connector::VNC::onTokenAuthCheckPkcs7(const int32_t& display, const std::string& serial, const std::string& pin, const uint32_t& cert, const std::vector<uint8_t>& pkcs7)
    {
        if(0 < displayNum() && display == displayNum())
        {
            JsonObjectStream jos;
            jos.push("cmd", SystemCommand::TokenAuth);
            jos.push("action", "check");
            jos.push("serial", serial);
            jos.push("pin", pin);
            jos.push("cert", static_cast<size_t>(cert));
            jos.push("data", Tools::convertBinary2JsonString(RawPtr(pkcs7.data(), pkcs7.size())));

            static_cast<ChannelClient*>(this)->sendLtsmEvent(Channel::System, jos.flush());
        }
    }

    void Connector::VNC::onLoginFailure(const int32_t & display, const std::string & msg)
    {
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::LoginSuccess);
        jos.push("action", false);
        jos.push("error", msg);

        static_cast<ChannelClient*>(this)->sendLtsmEvent(Channel::System, jos.flush());
    }

    void Connector::VNC::systemChannelError(const JsonObject & jo)
    {
        auto channel = jo.getInteger("id");
        auto code = jo.getInteger("code");
        auto err = jo.getString("error");

        Application::info("%s: channel: %d, errno: %d, display: %d, error: `%s'", __FUNCTION__, channel, displayNum(), code, err.c_str());

        if(isUserSession())
            busSendNotify(displayNum(), "Channel Error", err.append(", errno: ").append(std::to_string(code)),
                                NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
    }
#endif

}
