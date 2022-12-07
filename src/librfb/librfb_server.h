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

#include "ltsm_librfb.h"
#include "ltsm_sockets.h"
#include "ltsm_channels.h"
#include "librfb_encodings.h"

namespace LTSM
{
    struct XcbFrameBuffer
    {
        XCB::PixmapInfoReply reply;
        FrameBuffer fb;
    };

    namespace RFB
    {
        /// ServerEncoder
        class ServerEncoder : public ChannelClient, protected NetworkStream
        {
            std::vector<int>    clientEncodings;

            std::unique_ptr<NetworkStream> socket;      /// socket layer
            std::unique_ptr<TLS::Stream> tls;           /// tls layer
            std::unique_ptr<ZLib::DeflateStream> zlib;  /// zlib layer
            std::unique_ptr<EncodingBase> encoder;

            PixelFormat         clientPf;
            ColorMap            colourMap;
            std::mutex          sendLock;

            std::atomic<bool>   rfbMessages{true};
            std::atomic<bool>   fbUpdateProcessing{false};

            mutable size_t      netStatRx = 0;
            mutable size_t      netStatTx = 0;

            NetworkStream*      streamIn = nullptr;
            NetworkStream*      streamOut = nullptr;

            bool                clientTrueColor = true;
            bool                clientBigEndian = false;
            bool                continueUpdatesSupport = false;
            bool                continueUpdatesProcessed = false;

        protected:
            friend class EncodingBase;
            friend class EncodingRaw;
            friend class EncodingRRE;
            friend class EncodingHexTile;
            friend class EncodingTRLE;
            friend class EncodingZlib;

            const PixelFormat & clientFormat(void) const;

            // ServerEncoder
            virtual const PixelFormat & serverFormat(void) const = 0;
            virtual XcbFrameBuffer xcbFrameBuffer(const XCB::Region &) const = 0;

            virtual std::list<std::string> serverDisabledEncodings(void) const = 0;
            virtual std::list<std::string> serverPrefferedEncodings(void) const = 0;

            // network stream interface
            void                sendFlush(void) override;
            void                sendRaw(const void* ptr, size_t len) override;
            void                recvRaw(void* ptr, size_t len) const override;
            bool                hasInput(void) const override;
            size_t              hasData(void) const override;
            uint8_t             peekInt8(void) const override;

	    void		zlibDeflateStart(size_t);
	    void                zlibDeflateStop(bool uint16sz = false);

            std::string         serverEncryptionInfo(void) const;
            virtual void        serverSelectEncodingsEvent(void) {}

            void                setEncodingDebug(int v);
            void                setEncodingThreads(int v);
            bool                isClientEncodings(int) const;
            bool                isContinueUpdatesSupport(void) const;
            bool                isContinueUpdatesProcessed(void) const;

            bool                isUpdateProcessed(void) const;
            void                waitUpdateProcess(void);

            bool                serverSelectClientEncoding(void);

            bool                authVncInit(const std::string &);
            bool                authVenCryptInit(const SecurityInfo &);

            void                sendFrameBufferUpdate(const FrameBuffer &);
            void                sendFrameBufferUpdateRichCursor(const FrameBuffer &, uint16_t xhot, uint16_t yhot);
            void                sendColourMap(int first);
            void                sendBellEvent(void);
            void                sendCutTextEvent(const std::vector<uint8_t> &);
            void                sendContinuousUpdates(bool enable);
            bool                sendUpdateSafe(const XCB::Region &);
            void                sendEncodingLtsmSupported(void);
            void                recvChannelSystem(const std::vector<uint8_t> &) override;
            bool                serverSide(void) const override { return true; }

            void                recvPixelFormat(void);
            void                recvSetEncodings(void);
            void                recvKeyCode(void);
            void                recvPointer(void);
            void                recvCutText(void);
            void                recvFramebufferUpdate(void);
            void                recvSetContinuousUpdates(void);
            void                recvSetDesktopSize(void);

        public:
            ServerEncoder(int sockfd = 0);

            int                 serverHandshakeVersion(void);
            bool                serverSecurityInit(int protover, const SecurityInfo &);
            void                serverClientInit(std::string_view, const XCB::Size & size, int depth, const PixelFormat &);
            bool                rfbMessagesRunning(void) const;
            void                rfbMessagesLoop(void);
            void                rfbMessagesShutdown(void);

            void                serverSelectEncodings(void);

	    void		sendEncodingDesktopResize(const DesktopResizeStatus &, const DesktopResizeError &, const XCB::Size &);
            void                sendEncodingRichCursor(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot);

            void                clientDisconnectedEvent(int display);

            int                 sendPixel(uint32_t pixel);
            int                 sendCPixel(uint32_t pixel);
            int                 sendRunLength(size_t length);

            // ServerEncoder
            virtual void        recvPixelFormatEvent(const PixelFormat &, bool bigEndian) {}
            virtual void        recvSetEncodingsEvent(const std::vector<int> &) {}
            virtual void        recvKeyEvent(bool pressed, uint32_t keysym) {}
            virtual void        recvPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy) {}
            virtual void        recvCutTextEvent(const std::vector<uint8_t> &) {}
            virtual void        recvFramebufferUpdateEvent(bool full, const XCB::Region &) {}
            virtual void        recvSetContinuousUpdatesEvent(bool enable, const XCB::Region &) {}
            virtual void        recvSetDesktopSizeEvent(const std::vector<ScreenInfo> &) {}
            virtual void        sendFrameBufferUpdateEvent(const XCB::Region &) {}
            virtual void        sendLtsmEvent(uint8_t channel, const uint8_t*, size_t) override;
        };
    }
}

#endif // _LTSM_LIBRFB_
