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

#include <sys/stat.h>

#include <tuple>
#include <chrono>
#include <thread>

#include "ltsm_application.h"
#include "librfb_x11server.h"

#ifdef LTSM_ENCODING_FFMPEG
#include "librfb_ffmpeg.h"
#endif

using namespace std::chrono_literals;

namespace LTSM
{
    void RFB::X11Server::xfixesCursorChangedEvent(void)
    {
        clientUpdateCursor = isClientSupportedEncoding(RFB::ENCODING_RICH_CURSOR);
    }

    void RFB::X11Server::damageRegionEvent(const XCB::Region & reg)
    {
        damageRegion.join(reg);
    }

    void RFB::X11Server::clipboardChangedEvent(const std::vector<uint8_t> & buf)
    {
        if(rfbClipboardEnable())
        {
            // thread async and copy buf
            std::thread([this, buf](){ this->sendCutTextEvent(buf); }).detach();
        }
    }

    void RFB::X11Server::displayConnectedEvent(void)
    {
        if(xcbNoDamageOption())
        {
            _damage.reset();
        }
    }

    void  RFB::X11Server::randrScreenSetSizeEvent(const XCB::Size & wsz)
    {
        Application::info("%s: size: [%d, %d]", __FUNCTION__, wsz.width, wsz.height);
        displayResizeProcessed = true;
    }

    void RFB::X11Server::randrScreenChangedEvent(const XCB::Size & wsz, const xcb_randr_notify_event_t & notify)
    {
        Application::info("%s: size: [%d, %d], sequence: 0x%04x", __FUNCTION__, wsz.width, wsz.height, notify.sequence);

        xcbShmInit();
        displayResizeProcessed = false;

        serverDisplayResizedEvent(wsz);

        if(isClientSupportedEncoding(RFB::ENCODING_EXT_DESKTOP_SIZE))
        {
            auto status = randrSequence == notify.sequence ?
                RFB::DesktopResizeStatus::ClientSide : RFB::DesktopResizeStatus::ServerRuntime;

            std::thread([=]()
            {
                if(status == RFB::DesktopResizeStatus::ServerRuntime)
                {
                    this->sendEncodingDesktopResize(status, RFB::DesktopResizeError::NoError, wsz);
                    this->displayResizeEvent(wsz);
                }
                else
                // clientSide
                if(this->displayResizeNegotiation)
                {
                    this->sendEncodingDesktopResize(status, RFB::DesktopResizeError::NoError, wsz);
                    this->displayResizeEvent(wsz);
                    this->displayResizeNegotiation = false;
                }
            }).detach();
        }
    }

    bool RFB::X11Server::xcbProcessingEvents(void)
    {
        auto start = std::chrono::steady_clock::now();

        while(auto ev = XCB::RootDisplay::poolEvent())
        {
            if(auto err = XCB::RootDisplay::hasError())
            {
                xcbDisableMessages(true);
                Application::error("%s: xcb error, code: %d", __FUNCTION__, err);
                return false;
            }
            else
            if(auto extShm = XCB::RootDisplay::getExtension(XCB::Module::SHM))
            {
                uint16_t opcode = 0;
                if(shm && extShm->isEventError(ev, & opcode))
                {
                    Application::warning("%s: %s error: 0x%04x", __FUNCTION__, "shm", opcode);
                    shm.reset();
                }
            }
            else
            if(auto extFixes = XCB::RootDisplay::getExtension(XCB::Module::XFIXES))
            {
                uint16_t opcode = 0;
                if(extFixes->isEventError(ev, & opcode))
                {
                    Application::warning("%s: %s error: 0x%04x", __FUNCTION__, "xfixes", opcode);
                }
            }

            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            if(200 < dt.count()) break;
        }

        return true;
    }

    /* Connector::X11Server */
    int RFB::X11Server::rfbCommunication(void)
    {
        serverSelectEncodings();

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
        serverClientInit("X11 Remote Desktop", XCB::RootDisplay::size(), XCB::RootDisplay::depth(), serverFormat());

        timerNotActivated->stop();

        xcbShmInit();
        serverConnectedEvent();

        Application::info("%s: wait RFB messages...", __FUNCTION__);

        // xcb on
        xcbDisableMessages(false);

        // process rfb messages background
        auto rfbThread = std::thread([=]()
        {
            this->rfbMessagesLoop();
        });

    	bool nodamage = xcbNoDamageOption();
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

            if(! xcbAllowMessages())
            {
                std::this_thread::sleep_for(20ms);
                continue;
            }

            // processing xcb events
            if(! xcbProcessingEvents())
            {
                mainLoop = false;
                continue;
            }

            if(displayResizeProcessed || displayResizeNegotiation)
            {
                // wait loop
                std::this_thread::sleep_for(5ms);
                continue;
            }

            if(nodamage || fullscreenUpdate)
            {
                damageRegion = XCB::RootDisplay::region();
                fullscreenUpdate = false;
                clientUpdateReq = true;
            }
            else
            if(! damageRegion.empty())
            {
                // fix out of screen
                damageRegion = XCB::RootDisplay::region().intersected(damageRegion.align(4));
            }

#ifdef LTSM_ENCODING_FFMPEG
    	    if(isClientEncoding(RFB::ENCODING_FFMPEG_X264))
	    {
                if(auto ffmpeg = static_cast<const RFB::EncodingFFmpeg*>(getEncoder()))
                {
                    auto upms = ffmpeg->updateTimeMS();

		    // fps 25: 40ms
		    if(upms < 40)
                    {
                        Application::trace("%s: update time ms: %d", __FUNCTION__, upms);
            	        continue;
                    }
                }

                clientUpdateReq = true;
            	damageRegion = XCB::RootDisplay::region();
            }
#endif

            if(isUpdateProcessed())
            {
                // wait loop
                std::this_thread::sleep_for(5ms);
                continue;
            }

            if(damageRegion.empty())
            {
                // wait loop
                std::this_thread::sleep_for(5ms);
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

            if(clientUpdateCursor && ! isUpdateProcessed())
            {
                sendUpdateRichCursor();
                clientUpdateCursor = false;
            }
        } // main loop

        waitUpdateProcess();

        if(rfbThread.joinable())
            rfbThread.join();

        return EXIT_SUCCESS;
    }

    void  RFB::X11Server::recvPixelFormatEvent(const PixelFormat &, bool bigEndian)
    {
        if(serverFormat() != clientFormat())
        {
            Application::warning("%s: client/server format not optimal", __FUNCTION__);
        }
    }

    void  RFB::X11Server::recvSetEncodingsEvent(const std::vector<int> &)
    {
        serverSelectEncodings();
        serverEncodingsEvent();

        if(isClientSupportedEncoding(RFB::ENCODING_EXT_DESKTOP_SIZE) && rfbDesktopResizeEnabled())
        {
            std::thread([this]
            {
                this->sendEncodingDesktopResize(RFB::DesktopResizeStatus::ServerRuntime, RFB::DesktopResizeError::NoError, XCB::RootDisplay::size());
            }).detach();
        }
    }

    void RFB::X11Server::recvKeyEvent(bool pressed, uint32_t keysym)
    {
        if(xcbAllowMessages())
        {
            if(auto keycode = rfbUserKeycode(keysym))
                XCB::RootDisplay::fakeInputKeycode(keycode, 0 < pressed);
            else
                XCB::RootDisplay::fakeInputKeysym(keysym, 0 < pressed);
        }

        clientUpdateReq = true;
    }

    void RFB::X11Server::recvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy)
    {
        if(xcbAllowMessages())
        {
            auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST));
            if(! test)
                return;

            if(pressedMask ^ mask)
            {
                for(int num = 0; num < 8; ++num)
                {
                    int bit = 1 << num;

                    if(bit & mask)
                    {
                        if(Application::isDebugLevel(DebugLevel::Trace))
                            Application::debug("%s: xfb fake input pressed: %d", __FUNCTION__, num + 1);

                        test->fakeInputRaw(XCB::RootDisplay::root(), XCB_BUTTON_PRESS, num + 1, posx, posy);
                        pressedMask |= bit;
                    }
                    else if(bit & pressedMask)
                    {
                        if(Application::isDebugLevel(DebugLevel::Trace))
                            Application::debug("%s: xfb fake input released: %d", __FUNCTION__, num + 1);

                        test->fakeInputRaw(XCB::RootDisplay::root(), XCB_BUTTON_RELEASE, num + 1, posx, posy);
                        pressedMask &= ~bit;
                    }
                }
            }
            else
            {
                if(Application::isDebugLevel(DebugLevel::Trace))
                    Application::debug("%s: xfb fake input move, posx: %d, posy: %d", __FUNCTION__, posx, posy);

                test->fakeInputRaw(XCB::RootDisplay::root(), XCB_MOTION_NOTIFY, 0, posx, posy);
            }
        }

        clientUpdateReq = true;
    }

    void RFB::X11Server::recvCutTextEvent(const std::vector<uint8_t> & buf)
    {
        if(xcbAllowMessages() && rfbClipboardEnable())
        {
            size_t maxreq = XCB::RootDisplay::getMaxRequest();
            XCB::RootDisplay::setClipboard(buf.data(), std::min(maxreq, buf.size()));
        }

        clientUpdateReq = true;
    }

    void RFB::X11Server::recvFramebufferUpdateEvent(bool fullUpdateReq, const XCB::Region & region)
    {
        if(! xcbAllowMessages())
        {
            fullscreenUpdate = true;
            return;
        }

        if(fullUpdateReq)
            fullscreenUpdate = true;

        if(region != clientRegion)
        {
            fullscreenUpdate = true;
            clientRegion = region;
        }

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

        if(desktop.x != 0 && desktop.y != 0)
        {
            Application::error("%s: incorrect desktop size: [%d, %d, %d, %d]", __FUNCTION__, desktop.x, desktop.y, desktop.width, desktop.height);
            sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::InvalidScreenLayout, XCB::RootDisplay::size());
        }
        else
        if(! xcbAllowMessages())
        {
            Application::error("%s: xcb disabled", __FUNCTION__);
            sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources, XCB::Size{0, 0});
        }
        else
        if(RootDisplay::size() == desktop.toSize())
	{
            sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::NoError, XCB::RootDisplay::size());
	}
        else
        {
            displayResizeNegotiation = true;

            std::thread([&, sz = desktop.toSize()]
            {
                uint16_t sequence = 0;

		waitUpdateProcess();

                if(XCB::RootDisplay::setRandrScreenSize(sz, & sequence))
                {
                    randrSequence = sequence;
                }
                else
                {
                    sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources, XCB::RootDisplay::size());
            	    displayResizeNegotiation = false;
                    randrSequence = 0;
                }
            }).detach();
        }
    }

    void RFB::X11Server::sendUpdateRichCursor(void)
    {
        if(auto fixes = static_cast<const XCB::ModuleFixes*>(XCB::RootDisplay::getExtension(XCB::Module::XFIXES)))
        {
            XCB::CursorImage replyCursor = fixes->getCursorImage();
            auto reply = replyCursor.reply();

            if(auto ptr = replyCursor.data())
            {
                size_t argbSize = reply->width * reply->height;
                size_t dataSize = replyCursor.size();

                if(dataSize == argbSize)
                {
                    auto cursorRegion = XCB::Region(reply->x, reply->y, reply->width, reply->height);
                    auto cursorFB = FrameBuffer(reinterpret_cast<uint8_t*>(ptr), cursorRegion, ARGB32);

                    sendEncodingRichCursor(cursorFB, reply->xhot, reply->yhot);
                }
            }
        }
    }

    void RFB::X11Server::sendFrameBufferUpdateEvent(const XCB::Region & reg)
    {
        XCB::RootDisplay::damageSubtrack(reg);

        if(clientUpdateCursor)
        {
            sendUpdateRichCursor();
            clientUpdateCursor = false;
        }
    }

    void RFB::X11Server::xcbShmInit(uid_t uid)
    {
        if(auto ext = static_cast<const XCB::ModuleShm*>(XCB::RootDisplay::getExtension(XCB::Module::SHM)))
        {
            auto dsz = XCB::RootDisplay::size();
            auto bpp = XCB::RootDisplay::bitsPerPixel() >> 3;

            shm = ext->createShm(dsz.width * dsz.height * bpp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, false, uid);
        }
    }

    XcbFrameBuffer RFB::X11Server::xcbFrameBuffer(const XCB::Region & reg) const
    {
        Application::debug("%s: region [%d, %d, %d, %d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        auto pixmapReply = XCB::RootDisplay::copyRootImageRegion(reg, shm);
        if(! pixmapReply)
        {
            Application::error("%s: %s", __FUNCTION__, "xcb copy region empty");
            throw rfb_error(NS_FuncName);
        }

        if(Application::isDebugLevel(DebugLevel::Trace))
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
