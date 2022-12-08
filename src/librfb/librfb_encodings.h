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
#include <mutex>
#include <future>

#include "ltsm_librfb.h"

namespace LTSM
{
    namespace RFB
    {
        class ServerEncoder;

        class EncodingBase
        {
        protected:
            std::list< std::future<void> > jobs;

            std::mutex		busy;
            const int           type = 0;
            int                 debug = 0;
            int                 threads = 2;

            void                sendHeader(ServerEncoder*, const XCB::Region &);
            void                sendRawRegion(ServerEncoder*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            void                sendRawRegionPixels(ServerEncoder*, const XCB::Region &, const FrameBuffer &);

            static std::list<XCB::RegionPixel> rreProcessing(const XCB::Region &, const FrameBuffer &, int skipPixel);

        public:
            EncodingBase(int v);
            virtual ~EncodingBase() = default;

            virtual void        sendFrameBuffer(ServerEncoder &, const FrameBuffer &) = 0;

            int                 getType(void) const;
            bool                jobsEmpty(void) const;

            void                setDebug(int);
            void                setThreads(int);
        };

        /// EncodingRaw
        class EncodingRaw : public EncodingBase
        {
        public:
            void                sendFrameBuffer(ServerEncoder &, const FrameBuffer &) override;

            EncodingRaw() : EncodingBase(ENCODING_RAW) {}
        };

        /// EncodingRRE
        class EncodingRRE : public EncodingBase
        {
        protected:
            void                sendRegion(ServerEncoder*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            void                sendRects(ServerEncoder*, const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &);


        public:
            void                sendFrameBuffer(ServerEncoder &, const FrameBuffer &) override;

            EncodingRRE(bool co) : EncodingBase(co ? ENCODING_CORRE : ENCODING_RRE) {}

            bool                isCoRRE(void) const { return getType() == ENCODING_CORRE; }
        };

        /// EncodingHexTile
        class EncodingHexTile : public EncodingBase
        {
        protected:
            void                sendRegion(ServerEncoder*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            void                sendRegionForeground(ServerEncoder*, const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &);
            void                sendRegionColored(ServerEncoder*, const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &);
            void                sendRegionRaw(ServerEncoder*, const XCB::Region &, const FrameBuffer &, int jobId);

        public:
            void                sendFrameBuffer(ServerEncoder &, const FrameBuffer &) override;

            EncodingHexTile(bool zlib) : EncodingBase(zlib ? ENCODING_ZLIBHEX : ENCODING_HEXTILE) {}

            bool                isZlibHex(void) const { return getType() == ENCODING_ZLIBHEX; }
        };

        /// EncodingTRLE
        class EncodingTRLE : public EncodingBase
        {
        protected:
            void                sendRegion(ServerEncoder*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            void                sendRegionPacked(ServerEncoder*, const XCB::Region &, const FrameBuffer &, int jobId, size_t field, const PixelMapWeight &);
            void                sendRegionPlain(ServerEncoder*, const XCB::Region &, const FrameBuffer &, const std::list<PixelLength> &);
            void                sendRegionPalette(ServerEncoder*, const XCB::Region &, const FrameBuffer &, const PixelMapWeight &, const std::list<PixelLength> &);
            void                sendRegionRaw(ServerEncoder*, const XCB::Region &, const FrameBuffer &);

        public:
            void                sendFrameBuffer(ServerEncoder &, const FrameBuffer &) override;

            EncodingTRLE(bool zlib) : EncodingBase(zlib ? ENCODING_ZRLE : ENCODING_TRLE) {}

            bool                isZRLE(void) const { return getType() == ENCODING_ZRLE; }
        };

        /// EncodingZlib
        class EncodingZlib : public EncodingBase
        {
        protected:
            void                sendRegion(ServerEncoder*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);

        public:
            void                sendFrameBuffer(ServerEncoder &, const FrameBuffer &) override;

            EncodingZlib() : EncodingBase(ENCODING_ZLIB) {}
        };
    }
}

#endif // _LIBRFB_ENCODINGS_
