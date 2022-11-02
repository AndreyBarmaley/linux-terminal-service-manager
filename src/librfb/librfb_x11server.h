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
        class X11Server : public RFB::ServerEncoder
        {
            XCB::Region         clientRegion;
            XCB::Region         damageRegion;

            std::atomic<bool>   displayResized{false};
            std::atomic<int>    pressedMask{0};
            std::atomic<int>    randrSequence{0};

            bool                clientUpdateReq = false;

        protected:
	    // rfb server encoding
            XcbFrameBuffer      xcbFrameBuffer(const XCB::Region &) const override;
            virtual void        xcbFrameBufferModify(FrameBuffer &) const {}

            virtual XCB::RootDisplayExt*       xcbDisplay(void) = 0;
            virtual const XCB::RootDisplayExt* xcbDisplay(void) const = 0;

            virtual bool        xcbAllow(void) const = 0;
            virtual bool        xcbNoDamage(void) const = 0;
            virtual void        setXcbAllow(bool) = 0;
            virtual const XCB::SHM* xcbShm(void) const { return nullptr; }

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
