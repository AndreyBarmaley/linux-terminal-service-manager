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

#include <tuple>
#include <chrono>
#include <thread>
#include <iostream>

#include "ltsm_tools.h"
#include "ltsm_connector.h"
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

        if(_config->hasKey("socket:read:timeout_ms"))
        {
            auto val = _config->getInteger("socket:read:timeout_ms", 0);
            setReadTimeout(val);
            Application::info("%s: set socket read timeout: %dms", __FUNCTION__, val);
        }

        setEncodingThreads(_config->getInteger("vnc:encoding:threads", 2));
        setEncodingDebug(_config->getInteger("vnc:encoding:debug", 0));

        bool clipboardEnable = _config->getBoolean("vnc:clipboard", true);
        std::string encryptionInfo = "none";

        serverSelectEncodings();

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
                keymap.reset(new JsonObject(jc.toObject()));
                Application::notice("%s: keymap loaded: %s, items: %d", __FUNCTION__, file.c_str(), keymap->size());
            }
            else
                Application::error("%s: keymap invalid: %s", __FUNCTION__, file.c_str());
        }

        // RFB 6.1.1 version
        int protover = serverHandshakeVersion();
        if(protover == 0)
            return EXIT_FAILURE;

        // Xvfb: session request
        int screen = busStartLoginSession(_remoteaddr, "vnc");
        if(screen <= 0)
        {
            Application::error("%s: login session request: failure", __FUNCTION__);
            return EXIT_FAILURE;
        }

        if(! xcbConnect(screen))
        {
            Application::error("%s: xcb connect: failed", __FUNCTION__);
            return EXIT_FAILURE;
        }

        const xcb_visualtype_t* visual = _xcbDisplay->visual();
        if(! visual)
        {
            Application::error("%s: xcb visual empty", __FUNCTION__);
            return EXIT_FAILURE;
        }

        Application::debug("%s: login session request success, display: %d", __FUNCTION__, screen);
        Application::debug("%s: xcb max request: %d", __FUNCTION__, _xcbDisplay->getMaxRequest());

        // init server format
        serverSetPixelFormat(PixelFormat(_xcbDisplay->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0));

        // RFB 6.1.2 security
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

        if(! serverSecurityInit(protover, secInfo))
            return EXIT_FAILURE;

        busSetEncryptionInfo(screen, serverEncryptionInfo());

        // RFB 6.3.1 client init
        serverClientInit("X11 Remote Desktop");

        // wait widget started signal(onHelperWidgetStarted), 3000ms, 10 ms pause
        if(! Tools::waitCallable<std::chrono::milliseconds>(3000, 10,
                [=](){ _conn->enterEventLoopAsync(); return ! this->loginWidgetStarted; }))
        {
            Application::info("%s: something went wrong...", __FUNCTION__);
            return EXIT_FAILURE;
        }
        Application::info("%s: wait RFB messages...", __FUNCTION__);
        // xcb on
        setEnableXcbMessages(true);
        XCB::Region damageRegion(0, 0, 0, 0);
        bool clientUpdateReq = false;
        bool nodamage = _config->getBoolean("xcb:nodamage", false);
        std::vector<uint8_t> selbuf;

        while(loopMessage)
        {
            // RFB: mesage loop
            if(hasInput())
            {
                int msgType = recvInt8();

                switch(msgType)
                {
                    case RFB::CLIENT_SET_PIXEL_FORMAT:
                        clientSetPixelFormat();
                        damageRegion = _xcbDisplay->region();
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_SET_ENCODINGS:
                        if(clientSetEncodings())
                        {
                            serverSelectEncodings();

                            if(isClientEncodings(RFB::ENCODING_CONTINUOUS_UPDATES))
                            {
                                // RFB 6.7.15
                                // The server must send a EndOfContinuousUpdates message the first time
                                // it sees a SetEncodings message with the ContinuousUpdates pseudo-encoding,
                                // in order to inform the client that the extension is supported.
                                //
                                // serverSendEndContinuousUpdates();
                            }

                            // full update
                            damageRegion = _xcbDisplay->region();
                            clientUpdateReq = true;
                        }

                        break;

                    case RFB::CLIENT_REQUEST_FB_UPDATE:
                        {
                            auto [ fullUpdate, region ] = clientFramebufferUpdate();

                            if(fullUpdate)
                                damageRegion = _xcbDisplay->region();

                            if(region != clientRegion)
                                damageRegion = clientRegion = region;

                            clientUpdateReq = true;
                        }
                        break;

                    case RFB::CLIENT_EVENT_KEY:
                        clientKeyEvent(isAllowXcbMessages(), keymap.get());
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_EVENT_POINTER:
                        clientPointerEvent(isAllowXcbMessages());
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_CUT_TEXT:
                        clientCutTextEvent(isAllowXcbMessages(), clipboardEnable);
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_SET_DESKTOP_SIZE:
                        clientSetDesktopSizeEvent();
                        //clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_ENABLE_CONTINUOUS_UPDATES:
                        clientEnableContinuousUpdates();
                        //clientUpdateReq = true;
                        break;

                    default:
                        throw std::runtime_error(Tools::StringFormat("%1: RFB unknown message: %2").arg(__FUNCTION__).arg(Tools::hex(msgType, 2)));
                }
            }

            if(isAllowXcbMessages())
            {
                if(auto err = _xcbDisplay->hasError())
                {
                    setEnableXcbMessages(false);
                    Application::error("%s: xcb display error, code: %d", __FUNCTION__, err);
                    break;
                }

                // get all damages and join it
                while(auto ev = _xcbDisplay->poolEvent())
                {
                    int shmOpcode = _xcbDisplay->eventErrorOpcode(ev, XCB::Module::SHM);

                    if(0 <= shmOpcode)
                    {
                        _xcbDisplay->extendedError(ev.toerror(), "SHM extension");
                        loopMessage = false;
                        break;
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
                            busDisplayResized(_display, cc.width, cc.height);
                            desktopResizeModeChange(XCB::Size(cc.width, cc.height));
                        }
                    }
                    else if(_xcbDisplay->isSelectionNotify(ev) && clipboardEnable)
                    {
                        auto notify = reinterpret_cast<xcb_selection_notify_event_t*>(ev.get());

                        if(_xcbDisplay->selectionNotifyAction(notify))
                            selbuf = _xcbDisplay->getSelectionData();
                    }
                }

                if(nodamage)
                {
                    damageRegion = _xcbDisplay->region();
                    clientUpdateReq = true;
                }
                else if(! damageRegion.empty())
                    // fix out of screen
                    damageRegion = _xcbDisplay->region().intersected(damageRegion.align(4));

                // server action
                if(! isUpdateProcessed())
                {
                    switch(desktopResizeMode())
                    {
                        case RFB::DesktopResizeMode::ServerInform:
                        case RFB::DesktopResizeMode::ClientRequest:
                            sendEncodingDesktopSize(isAllowXcbMessages());
                            break;

                            default: break;
                    }

                    if(sendBellFlag)
                    {
                        serverSendBell();
                        sendBellFlag = false;
                    }

                    if(selbuf.size())
                    {
                        serverSendCutText(selbuf);
                        selbuf.clear();
                    }

                    if(clientUpdateReq && ! damageRegion.empty())
                    {
                        XCB::Region res;

                        if(XCB::Region::intersection(clientRegion, damageRegion, & res))
                            serverSendUpdateBackground(res);

                        damageRegion.reset();
                        clientUpdateReq = false;
                    }
                }
            }

            // dbus processing
            _conn->enterEventLoopAsync();
            // wait
            std::this_thread::sleep_for(1ms);
        }

        return EXIT_SUCCESS;
    }

    void Connector::VNC::onLoginSuccess(const int32_t & display, const std::string & userName)
    {
        if(0 < _display && display == _display)
        {
            setEnableXcbMessages(false);
            waitUpdateProcess();
            SignalProxy::onLoginSuccess(display, userName);
            setEnableXcbMessages(true);

            // fix new session size
            if(_xcbDisplay->size() != clientRegion.toSize())
            {
                if(_xcbDisplay->setRandrScreenSize(clientRegion.width, clientRegion.height))
                    Application::notice("%s: change session size [%dx%d], display: %d", __FUNCTION__, clientRegion.width, clientRegion.height, _display);
            }

            // full update
            _xcbDisplay->damageAdd(XCB::Region(0, 0, clientRegion.width, clientRegion.height));
            Application::notice("%s: dbus signal, display: %d, username: %s", __FUNCTION__, _display, userName.c_str());
        }
    }

    void Connector::VNC::onShutdownConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            setEnableXcbMessages(false);
            waitUpdateProcess();
            loopMessage = false;
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
            sendBellFlag = true;
        }
    }

    void Connector::VNC::serviceStop(void)
    {
        loopMessage = false;
    }

    bool Connector::VNC::serviceAlive(void) const
    {
        return loopMessage;
    }

    XCB::RootDisplayExt* Connector::VNC::xcbDisplay(void) const
    {
        return _xcbDisplay.get();
    }

    void Connector::VNC::serverPostProcessingFrameBuffer(FrameBuffer & fb)
    {
        renderPrimitivesToFB(fb);
    }
}
