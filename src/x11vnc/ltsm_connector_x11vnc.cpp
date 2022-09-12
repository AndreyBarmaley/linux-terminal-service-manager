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
#include "ltsm_x11vnc.h"
#include "ltsm_connector_x11vnc.h"

using namespace std::chrono_literals;

namespace LTSM
{
    /* Connector::X11VNC */
    Connector::X11VNC::X11VNC(int fd, const JsonObject & jo)
        : DisplayProxy(jo), RFB::ServerEncoder(fd)
    {
        if(! jo.getBoolean("DesktopResized"))
            desktopResizeModeDisable();
    }

    int Connector::X11VNC::communication(void)
    {
        Application::info("%s: remote addr: %s", __FUNCTION__, _remoteaddr.c_str());

        setEncodingThreads(_config->getInteger("vnc:encoding:threads", 2));
        setEncodingDebug(_config->getInteger("vnc:encoding:debug", 0));
        std::string encryptionInfo = "none";

        serverSelectEncodings();

        // set disabled and preffered encodings
        setDisabledEncodings(_config->getStdList<std::string>("vnc:encoding:blacklist"));
        setPrefferedEncodings(_config->getStdList<std::string>("vnc:encoding:preflist"));

        if(_config->hasKey("keymapfile"))
        {
            auto file = _config->getString("keymapfile");
            JsonContentFile jc(file);

            if(jc.isValid() && jc.isObject())
            {
                keymap.reset(new JsonObject(jc.toObject()));
                Application::notice("%s: keymap loaded: %s, items: %d", __FUNCTION__, file.c_str(), keymap->size());
            }
            else
                Application::error("%s: keymap invalid: %s", __FUNCTION__, file.c_str());
        }

        // Xvfb: session request
        if(! xcbConnect())
        {
            Application::error("%s: xcb connect failed", __FUNCTION__);
            return EXIT_FAILURE;
        }

        const xcb_visualtype_t* visual = _xcbDisplay->visual();
        if(! visual)
        {
            Application::error("%s: xcb visual empty", __FUNCTION__);
            return EXIT_FAILURE;
        }

        Application::debug("%s: xcb max request: %d", __FUNCTION__, _xcbDisplay->getMaxRequest());

        // RFB 6.1.1 version
        int protover = serverHandshakeVersion();
        if(protover == 0)
            return EXIT_FAILURE;

        // init server format
        serverSetPixelFormat(PixelFormat(_xcbDisplay->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0));

        // RFB 6.1.2 security
        RFB::SecurityInfo secInfo;
        secInfo.authNone = _config->getBoolean("noauth", false);
        secInfo.authVnc = _config->hasKey("passwdfile");
        secInfo.passwdFile = _config->getString("passwdfile");
        secInfo.authVenCrypt = ! _config->getBoolean("notls", false);
        secInfo.tlsPriority = "NORMAL:+ANON-ECDH:+ANON-DH";
        secInfo.tlsAnonMode = true;
        secInfo.tlsDebug = 0;

        if(Application::isDebugLevel(DebugLevel::SyslogDebug))
            secInfo.tlsDebug = 1;
        else
        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
            secInfo.tlsDebug = 3;

        if(! serverSecurityInit(protover, secInfo))
            return EXIT_FAILURE;

        // RFB 6.3.1 client init
        serverClientInit("X11VNC Remote Desktop");
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
                                // RFB 1.7.7.15
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
                        clientCutTextEvent(isAllowXcbMessages(), true);
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_SET_DESKTOP_SIZE:
                        clientSetDesktopSizeEvent();
                        //clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_ENABLE_CONTINUOUS_UPDATES:
                        clientEnableContinuousUpdates();
                        break;

                    default:
                        throw std::runtime_error(std::string("RFB unknown message: ").append(Tools::hex(msgType, 2)));
                }
            }

            if(isAllowXcbMessages())
            {
                if(auto err = _xcbDisplay->hasError())
                {
                    setEnableXcbMessages(false);
                    Application::error("%s: xcb display error connection: %d", __FUNCTION__, err);
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
                            desktopResizeModeChange(XCB::Size(cc.width, cc.height));
                        }
                    }
                    else if(_xcbDisplay->isSelectionNotify(ev))
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

            // wait
            std::this_thread::sleep_for(1ms);
        }

        return EXIT_SUCCESS;
    }

    void Connector::X11VNC::serviceStop(void)
    {
        loopMessage = false;
    }

    bool Connector::X11VNC::serviceAlive(void) const
    {
        return loopMessage;
    }

    XCB::RootDisplayExt* Connector::X11VNC::xcbDisplay(void) const
    {
        return _xcbDisplay.get();
    }
}
