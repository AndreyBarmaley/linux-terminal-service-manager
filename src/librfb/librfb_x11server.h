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

#ifndef _LIBRFB_X11SRV_
#define _LIBRFB_X11SRV_

#include <memory>
#include <atomic>

#include "librfb_server.h"

namespace LTSM
{
    namespace RFB
    {
        class X11Server : protected XCB::RootDisplay, public RFB::ServerEncoder
        {
            XCB::Region         clientRegion;
            XCB::Region         damageRegion;

            std::atomic<int>    pressedMask{0};
            std::atomic<int>    randrSequence{0};

            std::atomic<bool>   displayResized{false};
            std::atomic<bool>   clientUpdateReq{false};
            std::atomic<bool>   clientUpdateCursor{false};
            std::atomic<bool>   fullscreenUpdate{false};
            std::atomic<bool>   xcbMessages{true};

            XCB::ShmIdShared    shm;

        protected:
            // root display
            void                xfixesCursorChangedEvent(void) override;
            void                damageRegionEvent(const XCB::Region &) override;
            void                randrScreenChangedEvent(const XCB::Size &, const xcb_randr_notify_event_t &) override;
            void                clipboardChangedEvent(const std::vector<uint8_t> &) override;
 
	    // rfb server encoding
            XcbFrameBuffer      xcbFrameBuffer(const XCB::Region &) const override;
            virtual void        xcbFrameBufferModify(FrameBuffer &) const {}

            XCB::RootDisplay*   xcbDisplay(void) { return this; }
            void                xcbShmInit(uid_t = 0);
            bool                xcbProcessingEvents(void);

            virtual bool        xcbAllowMessages(void) const = 0;
            virtual void        xcbDisableMessages(bool) = 0;
            virtual bool        xcbNoDamageOption(void) const = 0;

            const XCB::Region & getClientRegion(void) const { return clientRegion; }

            virtual bool        rfbClipboardEnable(void) const = 0;
            virtual bool        rfbDesktopResizeEnabled(void) const = 0;
            virtual SecurityInfo rfbSecurityInfo(void) const = 0;
            virtual int         rfbUserKeycode(uint32_t) const { return 0; }

            virtual void        serverHandshakeVersionEvent(void) {}
            virtual void        serverSelectEncodingsEvent(void) {}
            virtual void        serverSecurityInitEvent(void) {}
            virtual void        serverConnectedEvent(void) {}
            virtual void        serverMainLoopEvent(void) {}
            virtual void        serverDisplayResizedEvent(const XCB::Size &) {}
            virtual void        serverEncodingsEvent(void) {}

            void                sendUpdateRichCursor(void);

        public:
            X11Server(int fd = 0) : ServerEncoder(fd) {}

            int		        rfbCommunication(void);

            // RFB::ServerEncoder
            void                recvPixelFormatEvent(const PixelFormat &, bool bigEndian) override;
            void                recvSetEncodingsEvent(const std::vector<int> &) override;
            void                recvKeyEvent(bool pressed, uint32_t keysym) override;
            void                recvPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy) override;
            void                recvCutTextEvent(const std::vector<uint8_t> &) override;
            void                recvFramebufferUpdateEvent(bool full, const XCB::Region &) override;
            void                sendFrameBufferUpdateEvent(const XCB::Region &) override;
            void                recvSetDesktopSizeEvent(const std::vector<RFB::ScreenInfo> &) override;
        };
    }
}

#endif // _LIBRFB_X11SRV_
