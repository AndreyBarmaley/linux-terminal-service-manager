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

#include <memory>

#include "ltsm_channels.h"
#include "librfb_extclip.h"
#include "librfb_decodings.h"
#include "ltsm_async_socket.h"

#include <boost/asio/experimental/channel.hpp>

namespace LTSM {
    namespace RFB {
        using TransferChannel = boost::asio::experimental::channel<void(boost::system::error_code, BinaryBuf)>;
        constexpr int TransferChannelSize = 4;

        /* ClientDecoder */
        class ClientDecoder : public ChannelClient, public DecoderRender, public ExtClip {

            boost::asio::strand<boost::asio::any_io_executor> rfb_strand_;
            boost::asio::strand<boost::asio::any_io_executor> xcb_strand_;
            boost::asio::steady_timer incr_update_timer_;
            mutable TransferChannel packets_channel_;

            std::unique_ptr<AsyncSocketBase> stream_; /// socket layer
            std::unique_ptr<DecodingBase> decoder_;

            PixelFormat server_pf_;
            int server_ltsm_version_ = 0;
            bool server_true_color_ = true;
            bool server_big_endian_ = false;

            bool continueUpdatesSupport = false;
            mutable bool continueUpdatesProcessed = false;

          protected:
            friend class DecodingRaw;
            friend class DecodingRRE;
            friend class DecodingHexTile;
            friend class DecodingTRLE;
            friend class DecodingZlib;
            friend class DecodingFFmpeg;

            bool serverBigEndian(void) const {
                return server_big_endian_;
            }

            // decoder stream interface
            const PixelFormat & serverFormat(void) const override;

            boost::asio::awaitable<void> authVncInitAwait(std::string_view pass) const;
            boost::asio::awaitable<bool> authVenCryptInitAwait(const SecurityInfo &);

#ifdef LTSM_WITH_GSSAPI
            boost::asio::awaitable<bool> authGssApiInitAwait(const SecurityInfo &);
#endif

            void sendPixelFormat(void) const;
            void sendEncodings(const std::list<int> &) const;
            void sendFrameBufferUpdate(bool incr) const;
            void sendFrameBufferUpdate(const XCB::Region &, bool incr) const;
            void sendContinuousUpdates(bool enable, const XCB::Region&) const;
            void sendSetDesktopSize(const XCB::Size &) const;
            void sendCutText(std::span<const uint8_t>, bool ext) const;
            void sendKeyEvent(bool pressed, uint32_t keysym, uint16_t scancode) const;
            void sendPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy) const;

            boost::asio::awaitable<void> rfbRequestIncrUpdate(void);
            boost::asio::awaitable<void> recvFBUpdateRegionAwait(void);

            boost::asio::awaitable<void> recvLtsmProtoAwait(void);
            boost::asio::awaitable<void> recvFBUpdateEventAwait(void);
            boost::asio::awaitable<void> recvColorMapEventAwait(void);
            boost::asio::awaitable<void> recvBellEventAwait(void);
            boost::asio::awaitable<void> recvCutTextEventAwait(void);
            void recvContinuousUpdatesEvent(void);

            boost::asio::awaitable<void> recvDecodingLtsmAwait(const XCB::Region &);
            boost::asio::awaitable<void> recvDecodingLastRectAwait(const XCB::Region &);
            boost::asio::awaitable<void> recvDecodingLtsmCursorAwait(const XCB::Region &);
            boost::asio::awaitable<void> recvDecodingRichCursorAwait(const XCB::Region &);
            boost::asio::awaitable<void> recvDecodingExtDesktopSizeAwait(int status, int err, const XCB::Size &);
            boost::asio::awaitable<void> recvDecodingUpdateRegionAwait(int type, const XCB::Region &);

            boost::asio::awaitable<void> rfbHostConnectAwait(std::string_view host, uint16_t port, bool no_delay = false);
            boost::asio::awaitable<void> rfbHandshakeAwait(const SecurityInfo &);
            boost::asio::awaitable<void> rfbRecvMessagesAwait(void);
            boost::asio::awaitable<void> rfbSendMessagesAwait(void);

            void asyncSendBuf(BinaryBuf&&) const;

            void asyncSendBuf(std::span<const uint8_t> span) const {
                asyncSendBuf(BinaryBuf(span));
            }

            void asyncSendBuf(std::span<const char> span) const {
                asyncSendBuf(BinaryBuf(span));
            }

          public:
            ClientDecoder(const boost::asio::any_io_executor& ctx)
                : ChannelClient(ctx)
                , rfb_strand_{ctx}
                , xcb_strand_{ctx}
                , incr_update_timer_{rfb_strand_}
                , packets_channel_{ctx, TransferChannelSize} {
            }

            inline boost::asio::strand<boost::asio::any_io_executor> rfb_strand(void) const { return rfb_strand_; }
            inline boost::asio::strand<boost::asio::any_io_executor> xcb_strand(void) const { return xcb_strand_; }

            void rfbMessagesShutdown(void);
            bool isContinueUpdatesSupport(void) const;
            bool isContinueUpdatesProcessed(void) const;
            bool isDecoderFFmpeg(void) const;

            void sendLtsmChannelDataPriv(CID, std::vector<uint8_t>&&) const final;

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
            virtual void clientRecvRichCursorEvent(const XCB::Point&, const XCB::Size&, std::vector<uint8_t> && pixels, std::vector<uint8_t> && mask) { /* empty */ }
            virtual void clientRecvLtsmCursorEvent(const XCB::Point&, const XCB::Size&, uint32_t cursorId, std::vector<uint8_t> && pixels) { /* empty */ }
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
