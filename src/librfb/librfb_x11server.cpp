/***********************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#include "ltsm_application.h"
#include "librfb_x11server.h"

using namespace std::chrono_literals;

namespace LTSM
{
    /* Connector::X11Server */
    int RFB::X11Server::rfbCommunication(void)
    {
        serverSelectEncodings();
        serverSelectEncodingsEvent();

        // vnc session not activated trigger
        auto timerNotActivated = Tools::BaseTimer::create<std::chrono::seconds>(30, false, [this]()
        {
            if(this->rfbMessagesRunning())
            {
                Application::error("session timeout trigger: %s", "rfbMessagesRunning");
                throw rfb_error(NS_FuncName);
            }
        });

        // RFB 6.1.1 version
        int protover = serverHandshakeVersion();
        if(protover == 0)
            return EXIT_FAILURE;

        serverHandshakeVersionEvent();

        // RFB 6.1.2 security
        if(! serverSecurityInit(protover, rfbSecurityInfo()))
            return EXIT_FAILURE;

        serverSecurityInitEvent();

        // RFB 6.3.1 client init
        serverClientInit("X11 Remote Desktop", xcbDisplay()->size(), xcbDisplay()->depth(), serverFormat());

        timerNotActivated->stop();
        serverConnectedEvent();

        Application::info("%s: wait RFB messages...", __FUNCTION__);

        // xcb on
        setXcbAllow(true);
        bool nodamage = xcbNoDamage();

        // process rfb messages background
        auto rfbThread = std::thread([=]()
        {
            try
            {
                this->rfbMessagesLoop();
            }
            catch(const std::exception & err)
            {
                Application::error("%s: exception: %s", "X11Server::rfbCommunication", err.what());
            }
            catch(...)
            {
            }

            this->rfbMessagesShutdown();
        });

        bool mainLoop = true;

        // main loop
        while(mainLoop)
        {
            serverMainLoopEvent();

            if(! rfbMessagesRunning())
            {
                mainLoop = false;
                continue;
            }

            if(! xcbAllow())
            {
                std::this_thread::sleep_for(50ms);
                continue;
            }

            if(auto err = xcbDisplay()->hasError())
            {
                setXcbAllow(false);
                Application::error("%s: xcb display error, code: %d", __FUNCTION__, err);
                mainLoop = false;
                continue;
            }

            // processing xcb events
            while(auto ev = xcbDisplay()->poolEvent())
            {
                if(0 <= xcbDisplay()->eventErrorOpcode(ev, XCB::Module::SHM))
                {
                    xcbDisplay()->extendedError(ev.toerror(), __FUNCTION__, "");
                    mainLoop = false;
                    break;
                }

                if(xcbDisplay()->isDamageNotify(ev))
                {
                    auto notify = reinterpret_cast<xcb_damage_notify_event_t*>(ev.get());
                    damageRegion.join(notify->area);
                }
                else if(xcbDisplay()->isRandrCRTCNotify(ev))
                {
                    auto notify = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                    xcb_randr_crtc_change_t cc = notify->u.cc;

                    if(0 < cc.width && 0 < cc.height)
                    {
                        Application::info("%s: xcb randr notify, size: [%d, %d], sequence: 0x%04x", __FUNCTION__, cc.width, cc.height, notify->sequence);

                        const XCB::Size dsz(cc.width, cc.height);
                        serverDisplayResizedEvent(dsz);

                        if(isClientEncodings(RFB::ENCODING_EXT_DESKTOP_SIZE))
                        {
                            auto status = randrSequence == 0 || randrSequence != notify->sequence ?
                                RFB::DesktopResizeStatus::ServerRuntime : RFB::DesktopResizeStatus::ClientSide;

                            std::thread([=]
                            {
                                this->sendEncodingDesktopResize(status, RFB::DesktopResizeError::NoError, dsz);
                                this->displayResized = true;
                            }).detach();
                        }
                    }
                }
                else if(xcbDisplay()->isSelectionNotify(ev) && rfbClipboardEnable())
                {
                    auto notify = reinterpret_cast<xcb_selection_notify_event_t*>(ev.get());

                    if(xcbDisplay()->selectionNotifyAction(notify))
                    {
                        std::thread([this]
                        {
                            auto selbuf = this->xcbDisplay()->getSelectionData();
                            this->sendCutTextEvent(selbuf);
                        }).detach();
                    }
                }
            } // xcb pool events

            if(nodamage)
            {
                damageRegion = xcbDisplay()->region();
                clientUpdateReq = true;
            }
            else if(! damageRegion.empty())
                // fix out of screen
                damageRegion = xcbDisplay()->region().intersected(damageRegion.align(4));

            // send busy
            if(isUpdateProcessed())
            {
                // wait loop
                std::this_thread::sleep_for(1ms);
                continue;
            }

            if(damageRegion.empty())
            {
                // wait loop
                std::this_thread::sleep_for(1ms);
                continue;
            }

            if(clientUpdateReq || isContinueUpdatesProcessed())
            {
                XCB::Region res;

                if(XCB::Region::intersection(clientRegion, damageRegion, & res))
                {
                    // background job
                    std::thread([&, reg = std::move(res)]()
                    {
                        if(! sendUpdateSafe(reg))
                            rfbMessagesShutdown(); 
                    }).detach();
                }

                damageRegion.reset();
                clientUpdateReq = false;
            }
        } // main loop

        if(rfbThread.joinable())
            rfbThread.join();

        return EXIT_SUCCESS;
    }

    void  RFB::X11Server::recvPixelFormatEvent(const PixelFormat &, bool bigEndian)
    {
        damageRegion = xcbDisplay()->region();
        clientUpdateReq = true;
    }

    void  RFB::X11Server::recvSetEncodingsEvent(const std::vector<int> &)
    {
        serverSelectEncodings();

        // full update
        damageRegion = xcbDisplay()->region();
        clientUpdateReq = true;

        serverEncodingsEvent();

        if(isClientEncodings(RFB::ENCODING_EXT_DESKTOP_SIZE) && rfbDesktopResizeEnabled())
        {
            std::thread([this]
            {
                this->sendEncodingDesktopResize(RFB::DesktopResizeStatus::ServerRuntime, RFB::DesktopResizeError::NoError, xcbDisplay()->size());
            }).detach();
        }
    }

    void RFB::X11Server::recvKeyEvent(bool pressed, uint32_t keysym)
    {
        if(xcbAllow())
        {
            if(auto keycode = rfbUserKeycode(keysym))
                xcbDisplay()->fakeInputKeycode(keycode, 0 < pressed);
            else
                xcbDisplay()->fakeInputKeysym(keysym, 0 < pressed);
        }

        clientUpdateReq = true;
    }

    void RFB::X11Server::recvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy)
    {
        if(xcbAllow())
        {
            if(pressedMask ^ mask)
            {
                for(int num = 0; num < 8; ++num)
                {
                    int bit = 1 << num;

                    if(bit & mask)
                    {
                        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
                            Application::debug("%s: xfb fake input pressed: %d", __FUNCTION__, num + 1);

                        xcbDisplay()->fakeInputTest(XCB_BUTTON_PRESS, num + 1, posx, posy);
                        pressedMask |= bit;
                    }
                    else if(bit & pressedMask)
                    {
                        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
                            Application::debug("%s: xfb fake input released: %d", __FUNCTION__, num + 1);

                        xcbDisplay()->fakeInputTest(XCB_BUTTON_RELEASE, num + 1, posx, posy);
                        pressedMask &= ~bit;
                    }
                }
            }
            else
            {
                if(Application::isDebugLevel(DebugLevel::SyslogTrace))
                    Application::debug("%s: xfb fake input move, posx: %d, posy: %d", __FUNCTION__, posx, posy);

                xcbDisplay()->fakeInputTest(XCB_MOTION_NOTIFY, 0, posx, posy);
            }
        }

        clientUpdateReq = true;
    }

    void RFB::X11Server::recvCutTextEvent(const std::vector<uint8_t> & buf)
    {
        if(xcbAllow() && rfbClipboardEnable())
        {
            size_t maxreq = xcbDisplay()->getMaxRequest();
            size_t chunk = std::min(maxreq, buf.size());

            xcbDisplay()->setClipboardEvent(buf.data(), chunk);
        }

        clientUpdateReq = true;
    }

    void RFB::X11Server::recvFramebufferUpdateEvent(bool fullUpdate, const XCB::Region & region)
    {
        if(displayResized && ! fullUpdate)
        {
            if(region.toSize() != xcbDisplay()->size())
            {
                Application::info("%s: display resized, skipped client old size: [%d, %d]", __FUNCTION__, region.width, region.height);
                return;
            }
            else
            {
                fullUpdate = true;
                displayResized = false;
                Application::info("%s: display resized, new size: [%d, %d]", __FUNCTION__, region.width, region.height);
            }
        }

        if(fullUpdate)
            damageRegion = clientRegion = xcbDisplay()->region();
        else
            clientRegion = xcbDisplay()->region().intersected(region);

        if(region != clientRegion)
            damageRegion = clientRegion = region;


        clientUpdateReq = true;
    }

    void RFB::X11Server::recvSetDesktopSizeEvent(const std::vector<RFB::ScreenInfo> & screens)
    {
        XCB::Region desktop(0, 0, 0, 0);
        for(auto & info : screens)
        {
            Application::info("%s: screen id: 0x%08x, region: [%d, %d, %d, %d], flags: 0x%08x", __FUNCTION__, info.id, info.posx, info.posy, info.width, info.height, info.flags);
            desktop.join(XCB::Region(info.posx, info.posy, info.width, info.height));
        }

        if(desktop.x != 0 || desktop.y != 0)
        {
            Application::error("%s: incorrect desktop size: [%d, %d, %d, %d]", __FUNCTION__, desktop.x, desktop.y, desktop.width, desktop.height);
            sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::InvalidScreenLayout, xcbDisplay()->size());
        }
        else
        if(xcbAllow())
        {
            std::thread([&, sz = desktop.toSize()]
            {
                uint16_t sequence = 0;
                if(xcbDisplay()->setRandrScreenSize(sz.width, sz.height, & sequence))
                    randrSequence = sequence;
                else
                {
                    sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources, xcbDisplay()->size());
                    randrSequence = 0;
                }
            }).detach();
        }
    }

    void RFB::X11Server::sendFrameBufferUpdateEvent(const XCB::Region & reg)
    {
        xcbDisplay()->damageSubtrack(reg);
    }

    XcbFrameBuffer RFB::X11Server::xcbFrameBuffer(const XCB::Region & reg) const
    {
        Application::debug("%s: region [%d, %d, %d, %d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        auto pixmapReply = xcbDisplay()->copyRootImageRegion(reg, xcbShm());
        if(! pixmapReply)
        {
            Application::error("%s: %s", __FUNCTION__, "xcb copy region empty");
            throw rfb_error(NS_FuncName);
        }

        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
        {
            Application::debug("%s: request size [%d, %d], reply: length: %d, bits per pixel: %d, red: %08x, green: %08x, blue: %08x",
                            __FUNCTION__, reg.width, reg.height, pixmapReply->size(), pixmapReply->bitsPerPixel(), pixmapReply->rmask, pixmapReply->gmask, pixmapReply->bmask);
        }

        // fix align
        if(pixmapReply->size() != reg.width * reg.height * pixmapReply->bytePerPixel())
        {
            Application::error("%s: region not aligned, reply size: %d, reg size: [%d, %d], byte per pixel: %d", __FUNCTION__, pixmapReply->size(), reg.width, reg.height, pixmapReply->bytePerPixel());
            throw rfb_error(NS_FuncName);
        }

        FrameBuffer fb(pixmapReply->data(), reg, serverFormat());
        xcbFrameBufferModify(fb);

        return XcbFrameBuffer{pixmapReply, fb};
    }
}
