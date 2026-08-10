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

#ifndef _LIBRFB_SERVER_
#define _LIBRFB_SERVER_

#include <list>
#include <mutex>
#include <forward_list>

#include "ltsm_channels.h"
#include "librfb_extclip.h"
#include "librfb_encodings.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_async_socket.h"

namespace LTSM {
    struct XcbFrameBuffer {
        XCB::PixmapInfoReply reply;
        FrameBuffer fb;
    };

    class ClientEncodings {
        std::list<int> encs = { RFB::ENCODING_RAW };

      public:
        ClientEncodings() = default;
        ~ClientEncodings() = default;

        void setPriority(const std::vector<int> &);
        bool isPresent(int) const;
        int findPriorityFrom(std::initializer_list<int>) const;
        std::vector<int> toVector(void) const;
    };

    namespace RFB {
        int serverSelectCompatibleEncoding(const ClientEncodings & clientEncodings);

        /// ServerEncoder
        class ServerEncoder : public ChannelListener, public EncoderStream, public ExtClip {

            boost::asio::strand<boost::asio::any_io_executor> rfb_strand_;
            boost::asio::strand<boost::asio::any_io_executor> xcb_strand_;
            boost::asio::steady_timer timer_updates_;

            mutable std::forward_list<uint32_t> cursorSended;
            ClientEncodings clientEncodings;
            std::string clientAuthName;
            std::string clientAuthDomain;

            std::unique_ptr<AsyncSocketBase> stream_; /// socket layer
            std::unique_ptr<EncodingBase> encoder_;

            PixelFormat clientPf;
            ColorMap colourMap;

	    // FIXME
            std::atomic<bool> fbUpdateProcessing_{false};

            bool clientLtsmSupported = false;
            bool clientLtsmKeyboard = false;
            bool clientVideoSupported = false;
            bool clientTrueColor = true;
            bool clientBigEndian = false;
            mutable bool continueUpdatesProcessed = false;

          protected:
            friend class EncodingBase;
            friend class EncodingRaw;
            friend class EncodingRRE;
            friend class EncodingHexTile;
            friend class EncodingTRLE;
            friend class EncodingZlib;
            friend class EncodingFFmpeg;

            const EncodingBase* getEncoder(void) const {
                return encoder_.get();
            }

            const ClientEncodings & getClientEncodings(void) const {
                return clientEncodings;
            }

            // ServerEncoder
            virtual XcbFrameBuffer serverFrameBuffer(const XCB::Region &) const = 0;
            virtual std::forward_list<std::string> serverDisabledEncodings(void) const = 0;

            virtual void serverScreenUpdateRequest(void /* full update */) = 0;
            virtual void serverScreenUpdateRequest(const XCB::Region &) = 0;

            // channel listenner interface
            void recvChannelSystemEvent(const std::vector<uint8_t> &) override;

            //
            std::string serverEncryptionInfo(void) const;

            void setEncodingDebug(int v);
            void setEncodingThreads(int v);
            void setEncodingOptions(std::forward_list<std::string> &&, uint32_t frameRate);

            bool isClientLtsmSupported(void) const;
            bool isClientLtsmKeyboard(void) const;
            bool isClientSupportedEncoding(int) const;
            bool isContinueUpdatesProcessed(void) const;
            bool isEncoderFFmpeg(void) const;

            void waitUpdateProcess(void);
            void serverSelectClientEncoding(void);

            boost::asio::awaitable<bool> authVncInit(const std::string &);
            boost::asio::awaitable<bool> authVenCryptInit(const SecurityInfo &);
            boost::asio::awaitable<void> waitUpdateProcessAwait(void);
            boost::asio::awaitable<void> sendColourMapAwait(int first) const;
            boost::asio::awaitable<void> sendBellEventAwait(void) const;
            boost::asio::awaitable<void> sendCutTextEventAwait(std::vector<uint8_t>, bool ext) const;
            boost::asio::awaitable<void> sendContinuousUpdatesAwait(bool enable) const;
            boost::asio::awaitable<void> sendFrameBufferUpdateAwait(const FrameBuffer &) const;
            boost::asio::awaitable<void> sendUpdateScreenAwait(const XCB::Region &);
            boost::asio::awaitable<void> sendEncodingLtsmSupportedAwait(void) const;

            bool serverSide(void) const override {
                return true;
            }

            boost::asio::awaitable<void> recvLtsmProtoAwait(void);
            boost::asio::awaitable<void> recvPixelFormatAwait(void);
            boost::asio::awaitable<void> recvSetEncodingsAwait(void);
            boost::asio::awaitable<void> recvFramebufferUpdateAwait(void);
            boost::asio::awaitable<void> recvKeyCodeAwait(void);
            boost::asio::awaitable<void> recvPointerAwait(void);
            boost::asio::awaitable<void> recvCutTextAwait(void);
            boost::asio::awaitable<void> recvSetContinuousUpdatesAwait(void);
            boost::asio::awaitable<void> recvSetDesktopSizeAwait(void);

            void cursorFailed(uint32_t);

          public:
            ServerEncoder(const boost::asio::any_io_executor&);
            ~ServerEncoder() = default;

            inline boost::asio::strand<boost::asio::any_io_executor> rfb_strand(void) const { return rfb_strand_; }
            inline boost::asio::strand<boost::asio::any_io_executor> xcb_strand(void) const { return xcb_strand_; }

            void assignSocket(int);

            // EncoderStream interface
            const PixelFormat & clientFormat(void) const override;
            bool clientIsBigEndian(void) const override;

            boost::asio::awaitable<int> serverHandshakeVersion(void);
            boost::asio::awaitable<bool> serverSecurityInit(int protover, const SecurityInfo &);
            boost::asio::awaitable<void> serverClientInit(std::string_view, const XCB::Size & size, int depth, const PixelFormat &);
            void socketShutdown(void);

            boost::asio::awaitable<void> rfbWaitMessage(void);

            void serverSelectEncodings(void);

            boost::asio::awaitable<void> sendEncodingDesktopResizeAwait(const DesktopResizeStatus &, const DesktopResizeError &, const XCB::Size &) const;
            boost::asio::awaitable<void> sendEncodingRichCursorAwait(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot) const;
            boost::asio::awaitable<void> sendEncodingLtsmCursorAwait(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot) const;
            boost::asio::awaitable<void> sendEncodingLtsmDataAwait(std::span<const uint8_t>) const;
            boost::asio::awaitable<void> sendLtsmChannelAwait(uint8_t channel, std::span<const uint8_t>) const;

            void sendLtsmChannelData(uint8_t channel, std::vector<uint8_t>&&) override final;
            void sendLtsmChannelData(uint8_t channel, std::string&&) override final;

            void clientDisconnectedEvent(int display);
            void displayResizeEvent(const XCB::Size &);

            std::pair<std::string, std::string> authInfo(void) const;

            virtual bool noVncMode(void) const {
                return false;
            }

            virtual void encoderInitEvent(EncodingBase*) { /* empty */ }

            virtual uint32_t frameRateOption(void) const {
                return 16;
            }

            // server encoder events
            virtual void serverRecvPixelFormatEvent(const PixelFormat &, bool bigEndian) { /* empty */ }
            virtual void serverRecvSetEncodingsEvent(const std::vector<int> &) { /* empty */ }
            virtual void serverRecvKeyEvent(bool pressed, uint32_t keycode, uint16_t scancode) { /* empty */ }
            virtual void serverRecvPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy) { /* empty */ }
            virtual void serverRecvCutTextEvent(std::vector<uint8_t> &&) { /* empty */ }
            virtual void serverRecvFBUpdateEvent(bool incremental, const XCB::Region &) { /* empty */ }
            virtual void serverRecvSetContinuousUpdatesEvent(bool enable, const XCB::Region &) { /* empty */ }
            virtual void serverRecvDesktopSizeEvent(std::vector<ScreenInfo>&&) { /* empty */ }
            virtual void serverSendFBUpdateEvent(const XCB::Region &) { /* empty */ }
            virtual void serverEncodingSelectedEvent(void) { /* empty */ }
        };
    }
}

#endif // _LTSM_LIBRFB_
