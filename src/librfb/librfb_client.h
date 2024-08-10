/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 **************************************************************************/

#ifndef _LIBRFB_CLIENT_
#define _LIBRFB_CLIENT_

#include <mutex>
#include <memory>
#include <atomic>

#include "ltsm_channels.h"
#include "librfb_decodings.h"

namespace LTSM
{
    namespace RFB
    {
        /* ClientDecoder */
        class ClientDecoder : public ChannelClient, protected DecoderStream
        {
            PixelFormat serverPf;

            std::unique_ptr<NetworkStream> socket;           /// socket layer
            std::unique_ptr<TLS::Stream> tls;                /// tls layer
            std::unique_ptr<ZLib::InflateStream> zlib;       /// zlib layer
            std::unique_ptr<DecodingBase> decoder;

            NetworkStream* streamIn;
            NetworkStream* streamOut;

            std::atomic<bool> rfbMessages{true};
            std::mutex      sendLock;

            bool            serverTrueColor = true;
            bool            serverBigEndian = false;
            bool            continueUpdatesSupport = false;
            bool            continueUpdatesProcessed = false;
            bool            ltsmServer = false;

        protected:
            friend class DecodingRaw;
            friend class DecodingRRE;
            friend class DecodingHexTile;
            friend class DecodingTRLE;
            friend class DecodingZlib;
            friend class DecodingFFmpeg;

            // network stream interface
            void            sendFlush(void) override;
            void            sendRaw(const void* ptr, size_t len) override;
            void            recvRaw(void* ptr, size_t len) const override;
            bool            hasInput(void) const override;
            size_t          hasData(void) const override;
            uint8_t         peekInt8(void) const override;

	    // decoder stream interface
            const PixelFormat & serverFormat(void) const override;

            bool            authVncInit(std::string_view pass);
            bool            authVenCryptInit(const SecurityInfo &);
#ifdef LTSM_WITH_GSSAPI
            bool            authGssApiInit(const SecurityInfo &);
#endif

            void            sendPixelFormat(void);
            void            sendEncodings(const std::list<int> &);
            void            sendFrameBufferUpdate(bool incr);
            void            sendFrameBufferUpdate(const XCB::Region &, bool incr);
            void            sendSetDesktopSize(const XCB::Size &);
            void            sendContinuousUpdates(bool enable, const XCB::Region &);

            void            recvFBUpdateEvent(void);
            void            recvColorMapEvent(void);
            void            recvBellEvent(void);
            void            recvCutTextEvent(void);
            void            recvContinuousUpdatesEvent(void);

            void            recvDecodingLtsm(const XCB::Region &);
            void            recvChannelSystem(const std::vector<uint8_t> &) override;
            bool            isUserSession(void) const override { return true; }

            void            recvDecodingLastRect(const XCB::Region &);
            void            recvDecodingExtDesktopSize(int status, int err, const XCB::Size &);
            void            recvDecodingRichCursor(const XCB::Region &);

            void            setSocketStreamMode(int sockd);
            void            setInetStreamMode(void);
            void            updateRegion(int type, const XCB::Region &);

        public:
            ClientDecoder() = default;

            bool            rfbHandshake(const SecurityInfo &);
            bool            rfbMessagesRunning(void) const;
            void            rfbMessagesLoop(void);
            void            rfbMessagesShutdown(void);
            bool            isContinueUpdatesSupport(void) const;
            bool            isContinueUpdatesProcessed(void) const;

            void            sendKeyEvent(bool pressed, uint32_t keysym);
            void            sendPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy);
            void            sendCutTextEvent(const char*, size_t);
            void            sendLtsmEvent(uint8_t channel, const uint8_t*, size_t) override;

            std::list<int>  supportedEncodings(void) const;

            virtual void    ltsmHandshakeEvent(int flags) { /* empty */ }
            virtual void    decodingExtDesktopSizeEvent(int status, int err, const XCB::Size & sz, const std::vector<RFB::ScreenInfo> &) { /* empty */ }
            virtual void    pixelFormatEvent(const PixelFormat &, const XCB::Size &) { /* empty */ }
            virtual void    fbUpdateEvent(void) { /* empty */ }
            virtual void    setColorMapEvent(const std::vector<Color> &) { /* empty */ }
            virtual void    bellEvent(void) { /* empty */ }
            virtual void    cutTextEvent(std::vector<uint8_t> &&) { /* empty */ }
            virtual void    richCursorEvent(const XCB::Region & reg, std::vector<uint8_t> && pixels, std::vector<uint8_t> && mask) { /* empty */ }
	    virtual void    displayResizeEvent(const XCB::Size &);
            //
            virtual bool    ltsmSupported(void) const { return false; }
        };

        class ClientDecoderSocket : public ClientDecoder
        {
        public:
            ClientDecoderSocket(int sd)
            {
                setSocketStreamMode(sd);
            }
        };

        class ClientDecoderInet : public ClientDecoder
        {
        public:
            ClientDecoderInet()
            {
                setInetStreamMode();
            }
        };
    };
}

#endif
