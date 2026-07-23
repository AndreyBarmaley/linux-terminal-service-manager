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

    class BoostContext {
        const int concurency_ = 1;
        mutable boost::asio::io_context ioc_;
        boost::asio::strand<boost::asio::any_io_executor> rfb_strand_;
        boost::asio::strand<boost::asio::any_io_executor> xcb_strand_;

      protected:
        inline boost::asio::io_context & ioc(void) const { return ioc_; }
        inline size_t concurency(void) const { return concurency_; }
        inline boost::asio::any_io_executor get_executor(void) { return ioc_.get_executor(); }
        inline boost::asio::strand<boost::asio::any_io_executor> rfb_strand(void) const { return rfb_strand_; }
        inline boost::asio::strand<boost::asio::any_io_executor> xcb_strand(void) const { return xcb_strand_; }

      public:
        explicit BoostContext(int concurency)
            : concurency_{concurency}, ioc_{concurency}, rfb_strand_{ioc_.get_executor()}, xcb_strand_{ioc_.get_executor()} {}
        ~BoostContext() = default;
    };

    namespace RFB {
        int serverSelectCompatibleEncoding(const ClientEncodings & clientEncodings);

        /// ServerEncoder
        class ServerEncoder : public BoostContext, public ChannelListener, public EncoderStream, public ExtClip {

            std::forward_list<uint32_t> cursorSended;
            ClientEncodings clientEncodings;
            std::string clientAuthName;
            std::string clientAuthDomain;

            std::unique_ptr<AsyncSocketBase> stream_; /// socket layer

//            std::unique_ptr<NetworkStream> socket; /// socket layer
//            std::unique_ptr<TLS::Stream> tls; /// tls layer
            std::unique_ptr<EncodingBase> encoder;

            PixelFormat clientPf;
            ColorMap colourMap;
            std::mutex sendLock;

            std::atomic<bool> rfbMessages{true};
            std::atomic<bool> fbUpdateProcessing{false};

//            NetworkStream* streamIn = nullptr;
//            NetworkStream* streamOut = nullptr;

            bool clientLtsmSupported = false;
            bool clientLtsmKeyboard = false;
            bool clientVideoSupported = false;
            bool clientTrueColor = true;
            bool clientBigEndian = false;
            bool continueUpdatesProcessed = false;

          protected:
            friend class EncodingBase;
            friend class EncodingRaw;
            friend class EncodingRRE;
            friend class EncodingHexTile;
            friend class EncodingTRLE;
            friend class EncodingZlib;
            friend class EncodingFFmpeg;

            const EncodingBase* getEncoder(void) const {
                return encoder.get();
            }

            const ClientEncodings & getClientEncodings(void) const {
                return clientEncodings;
            }

            // ServerEncoder
            virtual XcbFrameBuffer serverFrameBuffer(const XCB::Region &) const = 0;
            virtual std::forward_list<std::string> serverDisabledEncodings(void) const = 0;

            virtual void serverScreenUpdateRequest(void /* full update */) = 0;
            virtual void serverScreenUpdateRequest(const XCB::Region &) = 0;

            // network stream interface
            void sendFlush(void) override;
            void sendRaw(const void* ptr, size_t len) override;
            void recvRaw(void* ptr, size_t len) const override;
            bool hasInput(void) const override;
            size_t hasData(void) const override;

            // channel listenner interface
            void recvChannelSystemEvent(const std::vector<uint8_t> &) override;

            //
            std::string serverEncryptionInfo(void) const;

            void setEncodingDebug(int v);
            void setEncodingThreads(int v);
            void setEncodingOptions(const std::forward_list<std::string> &, uint32_t frameRate);

            bool isClientLtsmSupported(void) const;
            bool isClientLtsmKeyboard(void) const;
            bool isClientSupportedEncoding(int) const;
            bool isContinueUpdatesProcessed(void) const;
            bool isEncoderFFmpeg(void) const;

            bool isUpdateProcessed(void) const;
            void waitUpdateProcess(void);

            void serverSelectClientEncoding(void);

            boost::asio::awaitable<bool> authVncInit(const std::string &);
            boost::asio::awaitable<bool> authVenCryptInit(const SecurityInfo &);
            boost::asio::awaitable<void> sendColourMapAwait(int first);
            boost::asio::awaitable<void> sendBellEventAwait(void);
            boost::asio::awaitable<void> sendCutTextEventAwait(std::span<const uint8_t>, bool ext) const;
            void sendFrameBufferUpdate(const FrameBuffer &);
            void sendContinuousUpdates(bool enable);
            void sendUpdateScreen(const XCB::Region &);
            void sendEncodingLtsmSupported(void);
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
            ServerEncoder();
            ~ServerEncoder() = default;

            void assignSocket(int);

            // EncoderStream interface
            const PixelFormat & clientFormat(void) const override;
            bool clientIsBigEndian(void) const override;

            boost::asio::awaitable<int> serverHandshakeVersion(void);
            boost::asio::awaitable<bool> serverSecurityInit(int protover, const SecurityInfo &);
            boost::asio::awaitable<void> serverClientInit(std::string_view, const XCB::Size & size, int depth, const PixelFormat &);
            bool rfbMessagesRunning(void) const;
            void rfbMessagesShutdown(void);

            boost::asio::awaitable<void> rfbWaitMessage(void);

            void serverSelectEncodings(void);

            void sendEncodingDesktopResize(const DesktopResizeStatus &, const DesktopResizeError &, const XCB::Size &);
            void sendEncodingRichCursor(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot);
            void sendEncodingLtsmCursor(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot);

            void sendEncodingLtsmData(std::span<const uint8_t>);
            void sendLtsmChannel(uint8_t channel, std::span<const uint8_t>);
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
            virtual void serverRecvDesktopSizeEvent(const std::vector<ScreenInfo> &) { /* empty */ }
            virtual void serverSendFBUpdateEvent(const XCB::Region &) { /* empty */ }
            virtual void serverEncodingSelectedEvent(void) { /* empty */ }
        };
    }
}

#endif // _LTSM_LIBRFB_
