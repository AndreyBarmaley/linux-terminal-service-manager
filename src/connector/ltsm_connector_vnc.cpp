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

using namespace std::chrono_literals;

namespace LTSM
{
    /* Connector::VNC */
    Connector::VNC::VNC(sdbus::IConnection* conn, const JsonObject & jo) : SignalProxy(conn, jo, "vnc")
    {
        registerProxy();
    }

    Connector::VNC::~VNC()
    {
        if(0 < _display) busConnectorTerminated(_display);

        unregisterProxy();
        clientDisconnectedEvent(_display);
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

    void Connector::VNC::onLoginSuccess(const int32_t & display, const std::string & userName)
    {
        if(0 < _display && display == _display)
        {
            setEnableXcbMessages(false);
            waitUpdateProcess();
            SignalProxy::onLoginSuccess(display, userName);
            setEnableXcbMessages(true);
            auto & clientRegion = getClientRegion();

            // fix new session size
            if(_xcbDisplay->size() != clientRegion.toSize())
            {
                Application::warning("%s: remote request desktop size [%dx%d], display: %d", __FUNCTION__, clientRegion.width, clientRegion.height, _display);

                if(0 < _xcbDisplay->setRandrScreenSize(clientRegion.width, clientRegion.height))
                    Application::notice("%s: change session size [%dx%d], display: %d", __FUNCTION__, clientRegion.width, clientRegion.height, _display);
            }

            // full update
            _xcbDisplay->damageAdd(XCB::Region(0, 0, clientRegion.width, clientRegion.height));
            Application::notice("%s: dbus signal, display: %d, username: %s", __FUNCTION__, _display, userName.c_str());

            userSession = userName;
        }
    }

    void Connector::VNC::onShutdownConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            setEnableXcbMessages(false);
            waitUpdateProcess();
            rfbMessagesShutdown();
            Application::notice("%s: dbus signal, display: %d", __FUNCTION__, display);
        }
    }

    void Connector::VNC::onHelperWidgetStarted(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("%s: dbus signal, display: %d", __FUNCTION__, display);
            loginWidgetStarted = true;
        }
    }

    void Connector::VNC::onSendBellSignal(const int32_t & display)
    {
        if(0 < _display && display == _display)
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
        int screen = busStartLoginSession(_remoteaddr, "vnc");
        if(screen <= 0)
        {
            Application::error("%s: login session request: failure", __FUNCTION__);
            throw std::runtime_error(__FUNCTION__);
        }

        Application::info("%s: login session request success, display: %d", __FUNCTION__, screen);

        if(! xcbConnect(screen))
        {
            Application::error("%s: xcb connect: failed", __FUNCTION__);
            throw std::runtime_error(__FUNCTION__);
        }

        const xcb_visualtype_t* visual = _xcbDisplay->visual();
        if(! visual)
        {
            Application::error("%s: xcb visual empty", __FUNCTION__);
            throw std::runtime_error(__FUNCTION__);
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

    void Connector::VNC::serverMainLoopEvent(void)
    {
        // dbus processing
        _conn->enterEventLoopAsync();
    }

    void Connector::VNC::serverDisplayResizedEvent(const XCB::Size & sz)
    {
        busDisplayResized(_display, sz.width, sz.height);
    }

    void Connector::VNC::serverEncodingsEvent(void)
    {
    }

    void Connector::VNC::serverConnectedEvent(void)
    {
        // wait widget started signal(onHelperWidgetStarted), 3000ms, 10 ms pause
        if(! Tools::waitCallable<std::chrono::milliseconds>(3000, 10,
                [=](){ _conn->enterEventLoopAsync(); return ! this->loginWidgetStarted; }))
        {
            Application::info("%s: wait loginWidgetStarted failed", __FUNCTION__);
            throw std::runtime_error(__FUNCTION__);
        }
    }

    void Connector::VNC::serverSecurityInitEvent(void)
    {
        busSetEncryptionInfo(_display, serverEncryptionInfo());
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
    
    XCB::RootDisplayExt* Connector::VNC::xcbDisplay(void)
    {
        return _xcbDisplay.get();
    }

    const XCB::RootDisplayExt* Connector::VNC::xcbDisplay(void) const
    {
        return _xcbDisplay.get();
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
            
    int Connector::VNC::rfbUserKeycode(uint32_t keysym) const
    {
        auto it = keymap.find(keysym);
        return it != keymap.end() ? it->second : 0;
    }
}
