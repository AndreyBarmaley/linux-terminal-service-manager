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

#ifndef _LIBRFB_ENCODINGS_
#define _LIBRFB_ENCODINGS_

#include <list>
#include <future>

#include "ltsm_librfb.h"
#include "ltsm_sockets.h"

namespace LTSM
{
    namespace RFB
    {
        /// EncoderStream
        class EncoderStream : public NetworkStream
        {
        public:
            int                 sendHeader(int type, const XCB::Region &);
            int                 sendPixel(uint32_t pixel);
            int                 sendCPixel(uint32_t pixel);
            int                 sendRunLength(size_t length);
            int                 sendZlibData(ZLib::DeflateStream*, bool uint16sz = false);

            virtual const PixelFormat & serverFormat(void) const = 0;
            virtual const PixelFormat & clientFormat(void) const = 0;
            virtual bool clientIsBigEndian(void) const = 0;
        };

	/// EncoderWrapper
        class EncoderWrapper : public EncoderStream
        {
        protected:
            BinaryBuf*             buffer = nullptr;
            EncoderStream*         owner = nullptr;

        public:
            EncoderWrapper(BinaryBuf* bb, EncoderStream* st) : buffer(bb), owner(st) {}

            EncoderWrapper(const EncoderWrapper &) = delete;
            EncoderWrapper & operator=(const EncoderWrapper &) = delete;

            bool                    hasInput(void) const override;
            size_t                  hasData(void) const override;
            uint8_t                 peekInt8(void) const override;

            void                    sendRaw(const void* ptr, size_t len) override;
            void                    recvRaw(void* ptr, size_t len) const override;

            const PixelFormat &     serverFormat(void) const override { return owner->serverFormat(); }
            const PixelFormat &     clientFormat(void) const override { return owner->clientFormat(); }
            bool                    clientIsBigEndian(void) const override { return owner->clientIsBigEndian(); }
        };

        typedef std::pair<XCB::Region, BinaryBuf> EncodingRet;

        /// EncodingBase
        class EncodingBase
        {
        protected:
            std::list< std::future<EncodingRet> > jobs;

            const int           type = 0;
            int                 debug = 0;
            int                 threads = 2;

            void                sendRawRegionPixels(EncoderStream*, EncoderStream*, const XCB::Region &, const FrameBuffer &);

            static std::list<XCB::RegionPixel> rreProcessing(const XCB::Region &, const FrameBuffer &, int skipPixel);

        public:
            EncodingBase(int v);
            virtual ~EncodingBase() = default;

            virtual void        sendFrameBuffer(EncoderStream*, const FrameBuffer &) = 0;
            virtual void        resizedEvent(const XCB::Size &) { /* empty */ }

            int                 getType(void) const;
            bool                jobsEmpty(void) const;

            virtual void        setDebug(int);
            void                setThreads(int);
        };

        /// EncodingRaw
        class EncodingRaw : public EncodingBase
        {
            BinaryBuf           buf;

        protected:
            std::pair<XCB::Region, BinaryBuf>
                               sendRegion(EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);

        public:
            void                sendFrameBuffer(EncoderStream*, const FrameBuffer &) override;

            EncodingRaw() : EncodingBase(ENCODING_RAW) {}
        };

        /// EncodingRRE
        class EncodingRRE : public EncodingBase
        {
        protected:
            EncodingRet         sendRegion(EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            void                sendRects(EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &);


        public:
            void                sendFrameBuffer(EncoderStream*, const FrameBuffer &) override;

            EncodingRRE(bool co) : EncodingBase(co ? ENCODING_CORRE : ENCODING_RRE) {}

            bool                isCoRRE(void) const { return getType() == ENCODING_CORRE; }
        };

        /// EncodingHexTile
        class EncodingHexTile : public EncodingBase
        {
        protected:
            EncodingRet         sendRegion(EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            void                sendRegionForeground(EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &);
            void                sendRegionColored(EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &);
            void                sendRegionRaw(EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId);

        public:
            void                sendFrameBuffer(EncoderStream*, const FrameBuffer &) override;

            EncodingHexTile(void) : EncodingBase(ENCODING_HEXTILE) {}
        };

        /// EncodingTRLE
        class EncodingTRLE : public EncodingBase
        {
            std::unique_ptr<ZLib::DeflateStream> zlib;

        protected:
            EncodingRet         sendRegion(EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            void                sendRegionPacked(EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId, size_t field, const PixelMapWeight &);
            void                sendRegionPlain(EncoderStream*, const XCB::Region &, const FrameBuffer &, const PixelLengthList &);
            void                sendRegionPalette(EncoderStream*, const XCB::Region &, const FrameBuffer &, const PixelMapWeight &, const PixelLengthList &);
            void                sendRegionRaw(EncoderStream*, const XCB::Region &, const FrameBuffer &);

        public:
            void                sendFrameBuffer(EncoderStream*, const FrameBuffer &) override;

            EncodingTRLE(bool zlib);

            bool                isZRLE(void) const { return getType() == ENCODING_ZRLE; }
        };

        /// EncodingZlib
        class EncodingZlib : public EncodingBase
        {
            std::unique_ptr<ZLib::DeflateStream> zlib;
            BinaryBuf           buf;

        protected:
            EncodingRet         sendRegion(EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);

        public:
            void                sendFrameBuffer(EncoderStream*, const FrameBuffer &) override;

            EncodingZlib(int zlevel = Z_BEST_SPEED);
        };
    }
}

#endif // _LIBRFB_ENCODINGS_
