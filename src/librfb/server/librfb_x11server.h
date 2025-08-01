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
#include <shared_mutex>

#include "librfb_server.h"

namespace LTSM
{
    namespace RFB
    {
        class X11Server : protected XCB::RootDisplay, public RFB::ServerEncoder, public XCB::SelectionSource, public XCB::SelectionRecipient
        {
            std::vector<uint8_t> clientClipboard;

            XCB::Region clientRegion;
            XCB::Region damageRegion;

            mutable std::mutex serverLock;

            std::atomic<int> pressedMask{0};
            std::atomic<int> randrSequence{0};
            std::atomic<int> sendUpdateFPS{0};

            std::atomic<bool> displayResizeNegotiation{false};
            std::atomic<bool> displayResizeProcessed{false};
            std::atomic<bool> clientUpdateCursor{false};
            std::atomic<bool> fullscreenUpdateReq{false};

            XCB::ShmIdShared shm;

            mutable uint16_t clipLocalTypes = 0;
            uint16_t clipRemoteTypes = 0;

        protected:
            // root display
            void xcbFixesCursorChangedEvent(void) override;
            void xcbDamageNotifyEvent(const xcb_rectangle_t &) override;
            void xcbRandrScreenChangedEvent(const XCB::Size &, const xcb_randr_notify_event_t &) override;
            void xcbRandrScreenSetSizeEvent(const XCB::Size &) override;
            void xcbDisplayConnectedEvent(void) override;

            // selection source
            std::vector<xcb_atom_t> selectionSourceTargets(void) const override;
            bool selectionSourceReady(xcb_atom_t) const override;
            size_t selectionSourceSize(xcb_atom_t) const override;
            std::vector<uint8_t> selectionSourceData(xcb_atom_t, size_t offset, uint32_t length) const override;

            // selection recipient
            void selectionReceiveData(xcb_atom_t, const uint8_t* ptr, uint32_t len) const override;
            void selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) const override;
            void selectionChangedEvent(void) const override;

            // encoder stream
            XCB::Size displaySize(void) const override;
            
            // server encoder
            void serverScreenUpdateRequest(void) override;
            void serverScreenUpdateRequest(const XCB::Region &) override;
            XcbFrameBuffer serverFrameBuffer(const XCB::Region &) const override;

            // ext clipboard
            uint16_t extClipboardLocalTypes(void) const override;
            std::vector<uint8_t> extClipboardLocalData(uint16_t type) const override;
            void extClipboardRemoteTypesEvent(uint16_t type) override;
            void extClipboardRemoteDataEvent(uint16_t type, std::vector<uint8_t> &&) override;
            void extClipboardSendEvent(const std::vector<uint8_t> &) override;

            XCB::RootDisplay* xcbDisplay(void);
            const XCB::Region & getClientRegion(void) const;

            void xcbShmInit(uid_t = 0);
            bool xcbProcessingEvents(void);

            virtual bool xcbAllowMessages(void) const = 0;
            virtual void xcbDisableMessages(bool) = 0;
            virtual bool xcbNoDamageOption(void) const = 0;
            virtual size_t frameRateOption(void) const = 0;

            virtual bool rfbClipboardEnable(void) const = 0;
            virtual bool rfbDesktopResizeEnabled(void) const = 0;
            virtual SecurityInfo rfbSecurityInfo(void) const = 0;
            virtual int rfbUserKeycode(uint32_t) const = 0;

            // x11 server events
            virtual void serverHandshakeVersionEvent(void) {/* empty */}
            virtual void serverSecurityInitEvent(void) {/* empty */}
            virtual void serverConnectedEvent(void) {/* empty */}
            virtual void serverMainLoopEvent(void) {/* empty */}
            virtual void serverDisplayResizedEvent(const XCB::Size &) {/* empty */}
            virtual void serverEncodingsEvent(void) {/* empty */}
            virtual void serverFrameBufferModifyEvent(FrameBuffer &) const {/* empty */}

            void sendUpdateRichCursor(void);

        public:
            X11Server(int fd = -1) : ServerEncoder(fd) {}

            int rfbCommunication(void);

            // server encoder events
            void serverRecvPixelFormatEvent(const PixelFormat &, bool bigEndian) override;
            void serverRecvSetEncodingsEvent(const std::vector<int> &) override;
            void serverRecvKeyEvent(bool pressed, uint32_t keysym) override;
            void serverRecvPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy) override;
            void serverRecvCutTextEvent(std::vector<uint8_t> &&) override;
            void serverRecvFBUpdateEvent(bool incremental, const XCB::Region &) override;
            void serverSendFBUpdateEvent(const XCB::Region &) override;
            void serverRecvDesktopSizeEvent(const std::vector<RFB::ScreenInfo> &) override;
            void serverRecvSetContinuousUpdatesEvent(bool enable, const XCB::Region & reg) override;
        };
    }
}

#endif // _LIBRFB_X11SRV_
