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

#ifndef _LIBRFB_CLIENT_
#define _LIBRFB_CLIENT_

#include <mutex>
#include <memory>

#include "ltsm_channels.h"
#include "librfb_extclip.h"
#include "librfb_decodings.h"
#include "ltsm_async_socket.h"

namespace LTSM {
    namespace RFB {
        /* ClientDecoder */
        class ClientDecoder : public ChannelClient, public DecoderRender, public ExtClip {

            boost::asio::strand<boost::asio::any_io_executor> rfb_strand_;
            boost::asio::steady_timer incr_update_timer_;

            std::unique_ptr<AsyncSocketBase> stream_; /// socket layer
            std::unique_ptr<DecodingBase> decoder_;

            PixelFormat server_pf_;
            int server_ltsm_version_ = 0;
            bool server_true_color_ = true;
            bool server_big_endian_ = false;

            bool continueUpdatesSupport = false;
            bool continueUpdatesProcessed = false;

          protected:
            friend class DecodingRaw;
            friend class DecodingRRE;
            friend class DecodingHexTile;
            friend class DecodingTRLE;
            friend class DecodingZlib;
            friend class DecodingFFmpeg;

            inline const boost::asio::strand<boost::asio::any_io_executor> & rfb_strand(void) const { return rfb_strand_; }

            // decoder stream interface
            const PixelFormat & serverFormat(void) const override;

            boost::asio::awaitable<void> authVncInitAwait(std::string_view pass) const;
            boost::asio::awaitable<bool> authVenCryptInitAwait(const SecurityInfo &);

#ifdef LTSM_WITH_GSSAPI
            boost::asio::awaitable<bool> authGssApiInitAwait(const SecurityInfo &);
#endif

            boost::asio::awaitable<void> sendPixelFormatAwait(void) const;
            boost::asio::awaitable<void> sendEncodingsAwait(const std::list<int> &) const;
            boost::asio::awaitable<void> sendFrameBufferUpdateAwait(bool incr) const;
            boost::asio::awaitable<void> sendFrameBufferUpdateAwait(const XCB::Region &, bool incr) const;
            boost::asio::awaitable<void> sendContinuousUpdatesAwait(bool enable, const XCB::Region &);
            boost::asio::awaitable<void> sendSetDesktopSizeAwait(const XCB::Size &);
            boost::asio::awaitable<void> sendCutTextEventAwait(std::span<const uint8_t>, bool ext);
            boost::asio::awaitable<void> sendLtsmChannelAwait(uint8_t channel, std::span<const uint8_t>);

            boost::asio::awaitable<void> rfbRequestIncrUpdate(void);
            boost::asio::awaitable<void> recvFBUpdateRegionAwait(void);

            boost::asio::awaitable<void> recvLtsmProtoAwait(void);
            boost::asio::awaitable<void> recvFBUpdateEventAwait(void);
            boost::asio::awaitable<void> recvColorMapEventAwait(void);
            boost::asio::awaitable<void> recvBellEventAwait(void);
            boost::asio::awaitable<void> recvCutTextEventAwait(void);
            boost::asio::awaitable<void> recvContinuousUpdatesEventAwait(void);

            boost::asio::awaitable<void> recvDecodingLtsmAwait(const XCB::Region &);
            boost::asio::awaitable<void> recvDecodingLastRectAwait(const XCB::Region &);
            boost::asio::awaitable<void> recvDecodingLtsmCursorAwait(const XCB::Region &);
            boost::asio::awaitable<void> recvDecodingRichCursorAwait(const XCB::Region &);
            boost::asio::awaitable<void> recvDecodingExtDesktopSizeAwait(int status, int err, const XCB::Size &);
            boost::asio::awaitable<void> recvDecodingUpdateRegionAwait(int type, const XCB::Region &);

            void recvChannelSystemEvent(const std::vector<uint8_t> &) override;
            bool isUserSession(void) const override {
                return true;
            }

            boost::asio::awaitable<void> rfbHostConnectAwait(std::string_view host, uint16_t port, bool no_delay = false);
            boost::asio::awaitable<bool> rfbHandshakeAwait(const SecurityInfo &);
            boost::asio::awaitable<void> rfbMessagesLoopAwait(void);
            boost::asio::awaitable<void> sendKeyEventAwait(bool pressed, uint32_t keysym, uint16_t scancode);
            boost::asio::awaitable<void> sendPointerEventAwait(uint8_t buttons, uint16_t posx, uint16_t posy);

          public:
            ClientDecoder(const boost::asio::any_io_executor& ctx)
                : rfb_strand_{ctx}
                , incr_update_timer_{rfb_strand_} {
            }

            void sendCutText(std::vector<uint8_t>&&, bool ext);

            void rfbMessagesShutdown(void);
            bool isContinueUpdatesSupport(void) const;
            bool isContinueUpdatesProcessed(void) const;
            bool isDecoderFFmpeg(void) const;

            void sendLtsmChannelData(uint8_t channel, std::vector<uint8_t>&&) override;
            void sendLtsmChannelData(uint8_t channel, std::string&&) override;

            static std::list<int> supportedEncodings(bool extclip = false);

            // client decoder events
            virtual void clientRecvLtsmHandshakeEvent(int flags) { /* empty */ }
            virtual void clientRecvLtsmDataEvent(std::vector<uint8_t>) { /* empty */ }

            virtual void clientRecvDecodingDesktopSizeEvent(int status, int err, const XCB::Size & sz,
                    const std::vector<RFB::ScreenInfo> &) { /* empty */ }

            virtual void clientRecvPixelFormatEvent(const PixelFormat &, const XCB::Size &) { /* empty */ }
            virtual void clientRecvFBUpdateEvent(void) { /* empty */ }
            virtual void clientRecvSetColorMapEvent(const std::vector<Color> &) { /* empty */ }
            virtual void clientRecvBellEvent(void) { /* empty */ }
            virtual void clientRecvCutTextEvent(std::vector<uint8_t> &&) { /* empty */ }
            virtual void clientRecvRichCursorEvent(const XCB::Region & reg, std::vector<uint8_t> && pixels, std::vector<uint8_t> && mask) { /* empty */ }
            virtual void clientRecvLtsmCursorEvent(const XCB::Region & reg, uint32_t cursorId, std::vector<uint8_t> && pixels) { /* empty */ }
            virtual void displayResizeEvent(const XCB::Size &);
            //
            virtual bool clientLtsmSupported(void) const {
                return false;
            }

            inline int remoteLtsmVersion(void) const {
                return server_ltsm_version_;
            }

            virtual uint32_t frameRateOption(void) const { return 16; }

            virtual void decoderInitEvent(DecodingBase*) { /* empty */ }
        };
    };
}

#endif
