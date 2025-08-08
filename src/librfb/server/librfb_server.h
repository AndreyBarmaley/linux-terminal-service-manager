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

namespace LTSM
{
    struct XcbFrameBuffer
    {
        XCB::PixmapInfoReply reply;
        FrameBuffer fb;
    };

    class ClientEncodings
    {
        std::list<int> encs = { RFB::ENCODING_RAW };

    public:
        ClientEncodings() = default;
        ~ClientEncodings() = default;
        
        void setPriority(const std::vector<int> &);
        bool isPresent(int) const;
        int findPriorityFrom(std::initializer_list<int>) const;
    };

    namespace RFB
    {
        int serverSelectCompatibleEncoding(const ClientEncodings & clientEncodings);

        /// ServerEncoder
        class ServerEncoder : public ChannelListener, public EncoderStream, public ExtClip
        {
            std::forward_list<uint32_t> cursorSended;
            ClientEncodings clientEncodings;
            std::string clientAuthName;
            std::string clientAuthDomain;

            std::unique_ptr<NetworkStream> socket; /// socket layer
#ifdef LTSM_WITH_GNUTLS
            std::unique_ptr<TLS::Stream> tls; /// tls layer
#endif
            std::unique_ptr<EncodingBase> encoder;

            PixelFormat clientPf;
            ColorMap colourMap;
            std::mutex sendLock;

            std::atomic<bool> rfbMessages{true};
            std::atomic<bool> fbUpdateProcessing{false};

            mutable size_t netStatRx = 0;
            mutable size_t netStatTx = 0;

            NetworkStream* streamIn = nullptr;
            NetworkStream* streamOut = nullptr;

            bool clientLtsmSupported = false;
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

            const EncodingBase* getEncoder(void) const
            {
                return encoder.get();
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
            uint8_t peekInt8(void) const override;

            // channel listenner interface
            void recvChannelSystem(const std::vector<uint8_t> &) override;

            //
            std::string serverEncryptionInfo(void) const;

            void setEncodingDebug(int v);
            void setEncodingThreads(int v);
            void setEncodingOptions(const std::forward_list<std::string> &);

            bool isClientLtsmSupported(void) const;
            bool isClientVideoSupported(void) const;
            bool isClientSupportedEncoding(int) const;
            bool isContinueUpdatesProcessed(void) const;

            bool isUpdateProcessed(void) const;
            void waitUpdateProcess(void);

            bool serverSelectClientEncoding(void);

#ifdef LTSM_WITH_GNUTLS
            bool authVncInit(const std::string &);
            bool authVenCryptInit(const SecurityInfo &);
#endif
            bool sendFrameBufferUpdate(const FrameBuffer &);
            void sendColourMap(int first);
            void sendBellEvent(void);
            void sendCutTextEvent(const uint8_t* buf, uint32_t len, bool ext);
            void sendContinuousUpdates(bool enable);
            bool sendUpdateSafe(const XCB::Region &);
            void sendEncodingLtsmSupported(void);
            bool serverSide(void) const override { return true; }

            void recvPixelFormat(void);
            void recvSetEncodings(void);
            void recvKeyCode(void);
            void recvPointer(void);
            void recvCutText(void);
            void recvFramebufferUpdate(void);
            void recvSetContinuousUpdates(void);
            void recvSetDesktopSize(void);

        public:
            ServerEncoder(int sockfd = 0);

            // EncoderStream interface
            const PixelFormat & clientFormat(void) const override;
            bool clientIsBigEndian(void) const override;

            int serverHandshakeVersion(void);
            bool serverSecurityInit(int protover, const SecurityInfo &);
            void serverClientInit(std::string_view, const XCB::Size & size, int depth, const PixelFormat &);
            bool rfbMessagesRunning(void) const;
            void rfbMessagesLoop(void);
            void rfbMessagesShutdown(void);

            void serverSelectEncodings(void);

            void sendEncodingDesktopResize(const DesktopResizeStatus &, const DesktopResizeError &, const XCB::Size &);
            void sendEncodingRichCursor(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot);
            void sendEncodingLtsmCursor(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot);

            void sendEncodingLtsmData(const uint8_t*, size_t);
            void sendLtsmChannelData(uint8_t channel, const uint8_t*, size_t) override final;

            void clientDisconnectedEvent(int display);
            void displayResizeEvent(const XCB::Size &);

            std::pair<std::string, std::string> authInfo(void) const;

            virtual int remoteClientVersion(void) const { return 0; }

            // server encoder events
            virtual void serverRecvPixelFormatEvent(const PixelFormat &, bool bigEndian) { /* empty */ }
            virtual void serverRecvSetEncodingsEvent(const std::vector<int> &) { /* empty */ }
            virtual void serverRecvKeyEvent(bool pressed, uint32_t keysym) { /* empty */ }
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
