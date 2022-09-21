/***************************************************************************
 *   Copyright (C) 2022 by MultiCapture team <public.irkutsk@gmail.com>    *
 *                                                                         *
 *   Part of the MultiCapture engine:                                      *
 *   https://github.com/AndreyBarmaley/multi-capture                       *
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
 ***************************************************************************/

#ifndef _LIBRFB_CLIENT_
#define _LIBRFB_CLIENT_

#include <mutex>
#include <memory>
#include <atomic>

#include "ltsm_librfb.h"
#include "ltsm_sockets.h"

namespace LTSM
{
    namespace RFB
    {
        /* ClientDecoder */
        class ClientDecoder : protected NetworkStream
        {
            std::unique_ptr<NetworkStream> socket;           /// socket layer
            std::unique_ptr<TLS::Stream> tls;                /// tls layer
            std::unique_ptr<ZLib::InflateStream> zlib;       /// zlib layer

            NetworkStream* streamIn;
            NetworkStream* streamOut;

            int             decodingDebug = 0;
            std::atomic<bool> rfbMessages{true};
            std::mutex      sendLock;

            bool            serverTrueColor = true;
            bool            serverBigEndian = false;

            // network stream interface
            void            sendFlush(void) override;
            void            sendRaw(const void* ptr, size_t len) override;
            void            recvRaw(void* ptr, size_t len) const override;
            bool            hasInput(void) const override;
            size_t          hasData(void) const override;
            uint8_t         peekInt8(void) const override;

            // zlib wrapper
            void            zlibInflateStart(bool uint16sz = false);
            void            zlibInflateStop(void);

        protected:
            bool            authVncInit(std::string_view pass);
            bool            authVenCryptInit(std::string_view tlsPriority, int);

            void            sendPixelFormat(void);
            void            sendEncodings(std::initializer_list<int>);
            void            sendFrameBufferUpdate(bool incr);
            void            sendFrameBufferUpdate(const XCB::Region &, bool incr);
            void            sendSetDesktopSize(uint16_t width, uint16_t height);

            void            recvFBUpdateEvent(void);
            void            recvColorMapEvent(void);
            void            recvBellEvent(void);
            void            recvCutTextEvent(void);

            void            recvDecodingRaw(const XCB::Region &);
            void            recvDecodingRRE(const XCB::Region &, bool corre);
            void            recvDecodingHexTile(const XCB::Region &);
            void            recvDecodingHexTileRegion(const XCB::Region &, int & bgColor, int & fgColor);
            void            recvDecodingTRLE(const XCB::Region &, bool zrle);
            void            recvDecodingTRLERegion(const XCB::Region &, bool zrle);
            void            recvDecodingZlib(const XCB::Region &);
            void            recvDecodingLastRect(const XCB::Region &);
            void            recvDecodingExtDesktopSize(uint16_t status, uint16_t err, const XCB::Size &);

            int             recvPixel(void);
            int             recvCPixel(void);
            size_t          recvRunLength(void);

            void            setSocketStreamMode(int sockd);
            void            setInetStreamMode(void);

            virtual void    setPixel(const XCB::Point &, uint32_t pixel) = 0;
            virtual void    fillPixel(const XCB::Region &, uint32_t pixel) = 0;
            virtual const PixelFormat & clientPixelFormat(void) const = 0;
            virtual uint16_t clientWidth(void) const = 0;
            virtual uint16_t clientHeight(void) const = 0;

        public:
            ClientDecoder() = default;

            bool            rfbHandshake(bool tls, std::string_view tlsPriority, std::string_view password = "");
            bool            rfbMessagesProcessing(void) const;
            void            rfbMessagesLoop(void);
            void            rfbMessagesShutdown(void);

            void            sendKeyEvent(bool pressed, uint32_t keysym);
            void            sendPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy);
            void            sendCutTextEvent(const char*, size_t);

            virtual void    decodingExtDesktopSizeEvent(uint16_t status, uint16_t err, const XCB::Size & sz, const std::vector<RFB::ScreenInfo> &) {}
            virtual void    pixelFormatEvent(const PixelFormat &, uint16_t width, uint16_t height) {}
            virtual void    fbUpdateEvent(void) {}
            virtual void    setColorMapEvent(const std::vector<Color> &) {}
            virtual void    bellEvent(void) {}
            virtual void    cutTextEvent(const std::vector<uint8_t> &) {}
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
