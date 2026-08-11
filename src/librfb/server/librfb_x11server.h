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
#include <mutex>

#include "librfb_server.h"

namespace LTSM {
    namespace RFB {
        class X11Server : public ServerEncoder, public XCB::SelectionSource, public XCB::SelectionRecipient, protected XCB::RootDisplay {
            boost::asio::io_context & ioc_;

            std::vector<uint8_t> clientClipboard_;

            XCB::Region clientRegion;
            XCB::Region damageRegion;

            boost::asio::signal_set signals_;
            boost::asio::cancellation_signal xcb_cancel_;
            boost::asio::cancellation_signal rfb_cancel_;
            boost::asio::cancellation_signal srv_cancel_;
            boost::asio::steady_timer timer_update_;
            boost::asio::steady_timer clipboard_ready_;

            mutable std::mutex serverLock;
            std::chrono::time_point<std::chrono::steady_clock> frameTimePoint;

            std::atomic<int> pressedMask{0};
            std::atomic<int> randrSequence{0};
            std::atomic<int> sendUpdateFPS{0};

            std::atomic<bool> displayResizeNegotiation{false};
            std::atomic<bool> displayResizeProcessed{false};
            std::atomic<bool> clientUpdateCursor{false};
            std::atomic<bool> fullscreenUpdateReq{false};

            XCB::ShmIdShared shm;

            int rfbStartingCode_ = 0;
            uint16_t clipLocalTypes = 0;
            uint16_t clipRemoteTypes = 0;

          protected:
            boost::asio::awaitable<void> rfbStart(void);
            void rfbStop(void);

            // root display
            void xcbFixesCursorChangedEvent(void) override;
            void xcbDamageNotifyEvent(const xcb_rectangle_t &, uint8_t level) override;
            void xcbRandrScreenChangedEvent(const XCB::Size &, const xcb_randr_notify_event_t &) override;
            void xcbRandrScreenSetSizeEvent(const XCB::Size &) override;
            void xcbDisplayConnectedEvent(void) override;

            // selection source
            std::vector<xcb_atom_t> selectionSourceTargets(void) const override;
            bool selectionSourceReady(xcb_atom_t) const override;
            size_t selectionSourceSize(xcb_atom_t) const override;
            std::vector<uint8_t> selectionSourceData(xcb_atom_t, size_t offset, uint32_t length) const override;

            // selection recipient
            void selectionReceiveData(xcb_atom_t, std::vector<uint8_t>&&) const override;
            void selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) override;
            void selectionChangedEvent(void) const override;

            // encoder stream
            bool isDisplaySize(const XCB::Size&) const override;

            // server encoder
            void serverScreenUpdateRequest(void) override;
            void serverScreenUpdateRequest(const XCB::Region &) override;
            XcbFrameBuffer serverFrameBuffer(const XCB::Region &) const override;
            
            // ext clipboard
            uint16_t extClipboardLocalTypes(void) const override;
            boost::asio::awaitable<clipboard_buf> extClipboardLocalDataAwait(uint16_t type) override;
            boost::asio::awaitable<void> extClipboardRemoteDataAwait(uint16_t type, std::vector<uint8_t>) override;
            boost::asio::awaitable<void> extClipboardRemoteTypesAwait(uint16_t types) override;
            boost::asio::awaitable<void> extClipboardSendAwait(std::span<const uint8_t>) const override;
            boost::asio::awaitable<bool> extClipboardSourceReadyAwait(xcb_atom_t atom);

            XCB::RootDisplay* xcbDisplay(void);
            XCB::Region getClientRegion(void) const;

            boost::asio::awaitable<void> xcbShmInit(uid_t = 0, const XCB::Size* sz = nullptr);

            boost::asio::awaitable<XCB::Size> xcbDisplaySize(void) const;
            boost::asio::awaitable<XCB::Region> xcbDisplayRegion(void) const;
            boost::asio::awaitable<uint16_t> xcbDisplayDepth(void) const;

            boost::asio::awaitable<void> xcbEventsLoop(void);
            boost::asio::awaitable<void> rfbReceiveMessages(void);
            boost::asio::awaitable<void> signalsHandler(void);
            boost::asio::awaitable<void> serverUpdateLoop(void);
            boost::asio::awaitable<void> serverUpdateProcess(void);
            boost::asio::awaitable<void> sendUpdateRichCursorAwait(void);

            // server interface
            virtual void stop(void) noexcept = 0;

            virtual bool xcbAllowMessages(void) const = 0;
            virtual void xcbDisableMessages(bool) = 0;
            virtual bool xcbNoDamageOption(void) const = 0;

            virtual bool rfbClipboardEnable(void) const = 0;
            virtual bool rfbDesktopResizeEnabled(void) const = 0;
            virtual SecurityInfo rfbSecurityInfo(void) const = 0;
            virtual int rfbUserKeycode(uint32_t) const = 0;

            // x11 server events
            virtual void serverHandshakeVersionEvent(void) {/* empty */}
            virtual void serverSecurityInitEvent(void) {/* empty */}
            virtual void serverConnectedEvent(void) {/* empty */}
            virtual void serverDisplayResizedEvent(const XCB::Size &) {/* empty */}
            virtual void serverEncodingsEvent(void) {/* empty */}
            virtual void serverFrameBufferModifyEvent(FrameBuffer &) const {/* empty */}

          public:
            X11Server(boost::asio::io_context & ctx)
                : RFB::ServerEncoder(ctx.get_executor()), ioc_{ctx}, signals_{ctx.get_executor()},
                    timer_update_{ctx.get_executor()}, clipboard_ready_{ctx.get_executor()} {}
            ~X11Server() = default;

            boost::asio::awaitable<int> rfbCommunicationAwait(void);

            // server encoder events
            void serverRecvPixelFormatEvent(const PixelFormat &, bool bigEndian) override;
            void serverRecvSetEncodingsEvent(const std::vector<int> &) override;
            void serverRecvKeyEvent(bool pressed, uint32_t keycode, uint16_t scancode) override;
            void serverRecvPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy) override;
            void serverRecvCutTextEvent(std::vector<uint8_t> &&) override;
            void serverRecvFBUpdateEvent(bool incremental, const XCB::Region &) override;
            void serverSendFBUpdateEvent(const XCB::Region &) const override;
            void serverRecvDesktopSizeEvent(std::vector<RFB::ScreenInfo>&&) override;
            void serverRecvSetContinuousUpdatesEvent(bool enable, const XCB::Region & reg) override;
        };
    }
}

#endif // _LIBRFB_X11SRV_
