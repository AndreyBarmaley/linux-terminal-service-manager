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
#include <forward_list>

#include "ltsm_librfb.h"
#include "ltsm_sockets.h"
#include "ltsm_streambuf.h"
#include "ltsm_parallels_jobs.h"

#include "turbojpeg.h"


namespace LTSM {
    namespace RFB {
        /// EncoderStream
        class EncoderStream {
          public:
            int writeHeader(StreamBuf&, int type, const XCB::Region &) const;
            int writePixel(StreamBuf&, uint32_t pixel) const;
	    int writePixelRaw(StreamBuf&, uint32_t pixel, uint8_t bpp, bool be) const;
            int writeCPixel(StreamBuf&, uint32_t pixel) const;
            int writeRunLength(StreamBuf&, uint32_t length) const;
            int writeRawRegionPixels(StreamBuf&, const XCB::Region &, const FrameBuffer &) const;

            virtual const PixelFormat & serverFormat(void) const = 0;
            virtual const PixelFormat & clientFormat(void) const = 0;
            virtual bool clientIsBigEndian(void) const = 0;
            virtual bool isDisplaySize(const XCB::Size&) const = 0;
        };

        using EncodingRet = std::pair<XCB::Region, BinaryBuf>;

        /// EncodingBase
        class EncodingBase {
          protected:
            const int type = 0;
            int threads = 2;

            static std::list<XCB::RegionPixel> rreProcessing(const XCB::Region &, const FrameBuffer &, uint32_t skipPixel);

          public:
            EncodingBase(int v);
            virtual ~EncodingBase() = default;

            virtual void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const = 0;
            virtual void reinitContext(const EncoderStream*, const XCB::Size &) { /* empty */ }
            virtual void resizedEvent(const XCB::Size &) { /* empty */ }
            virtual bool setEncodingOptions(const std::forward_list<std::string> &) {
                return false;
            }
            virtual void setFps(uint32_t) {}
            virtual const char* getTypeName(void) const = 0;

            int getType(void) const;
            void setThreads(int);
        };

        /// EncodingRaw
        class EncodingRaw : public EncodingBase {
          protected:
            EncodingRet sendRegion(const EncoderStream*, const XCB::Point &, const XCB::Region &,
                    const FrameBuffer &, int jobId) const;

          public:
            void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const override;
            const char* getTypeName(void) const override {
                return "Raw";
            }

            EncodingRaw() : EncodingBase(ENCODING_RAW) {}
        };

        /// EncodingRRE
        class EncodingRRE : public EncodingBase {
          protected:
            EncodingRet sendRegion(const EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId) const;
            void sendRects(StreamBuf&, const EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId, int back,
                           const std::list<XCB::RegionPixel> &) const;

          public:
            void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const override;
            const char* getTypeName(void) const override {
                return getType() == ENCODING_CORRE ? "CoRRE" : "RRE";
            }

            EncodingRRE(bool co = false) : EncodingBase(co ? ENCODING_CORRE : ENCODING_RRE) {}

            bool isCoRRE(void) const {
                return getType() == ENCODING_CORRE;
            }
        };

        /// EncodingHexTile
        class EncodingHexTile : public EncodingBase {
          protected:
            EncodingRet sendRegion(const EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId) const;
            void sendRegionForeground(StreamBuf&, const EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId, int back,
                                      const std::list<XCB::RegionPixel> &) const;
            void sendRegionColored(StreamBuf&, const EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId, int back,
                                   const std::list<XCB::RegionPixel> &) const;
            void sendRegionRaw(StreamBuf&, const EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId) const;

          public:
            void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const override;
            const char* getTypeName(void) const override {
                return "HexTile";
            }

            EncodingHexTile(void) : EncodingBase(ENCODING_HEXTILE) {}
        };

        /// EncodingTRLE
        class EncodingTRLE : public EncodingBase {
            std::unique_ptr<ZLib::DeflateBase> zlib_;

          protected:
            EncodingRet sendRegion(const EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId) const;
            void sendRegionPacked(StreamBuf&, const EncoderStream*, const XCB::Region &, const FrameBuffer &, int jobId, const Tools::StreamBitsPack::Field &,
                                  const PixelMapPalette &) const;
            void sendRegionPlain(StreamBuf&, const EncoderStream*, const XCB::Region &, const FrameBuffer &, const PixelLengthList &) const;
            void sendRegionPalette(StreamBuf&, const EncoderStream*, const XCB::Region &, const FrameBuffer &, const PixelMapPalette &,
                                   const PixelLengthList &) const;
            void sendRegionRaw(StreamBuf&, const EncoderStream*, const XCB::Region &, const FrameBuffer &) const;

          public:
            void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const override;
            const char* getTypeName(void) const override {
                return isZRLE() ? "ZRLE" : "TRLE";
            }

            EncodingTRLE(bool zlib = false) : EncodingBase(zlib ? ENCODING_ZRLE : ENCODING_TRLE),
                zlib_{std::make_unique<ZLib::DeflateBase>(Z_BEST_SPEED)} {}

            bool isZRLE(void) const {
                return getType() == ENCODING_ZRLE;
            }
        };

        /// EncodingZlib
        class EncodingZlib : public EncodingBase {
            std::unique_ptr<ZLib::DeflateBase> zlib_;
            int zlevel;

          protected:
            EncodingRet sendRegion(const EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId) const;

          public:
            void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const override;
            const char* getTypeName(void) const override {
                return "ZLib";
            }

            EncodingZlib(int lev = Z_BEST_SPEED);
            bool setEncodingOptions(const std::forward_list<std::string> &) override;
        };

        /// EncodingLZ4
        class EncodingLZ4 : public EncodingBase {
          protected:
            EncodingRet sendRegion(const EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId) const;

          public:
            void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const override;
            const char* getTypeName(void) const override {
                return "LZ4";
            }

            EncodingLZ4() : EncodingBase(ENCODING_LTSM_LZ4) {}
        };

        /// EncodingTJPG
        class EncodingTJPG : public EncodingBase {
            int jpegQuality = 85;
            int jpegSamp = TJSAMP_420;

          protected:
            EncodingRet sendRegion(const EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId) const;

          public:
            void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const override;
            const char* getTypeName(void) const override {
                return "TJPG";
            }

            EncodingTJPG() : EncodingBase(ENCODING_LTSM_TJPG) {}
            EncodingTJPG(int qual, int samp) : EncodingBase(ENCODING_LTSM_TJPG), jpegQuality(qual), jpegSamp(samp) {}

            bool setEncodingOptions(const std::forward_list<std::string> &) override;
        };

        /// EncodingQOI
        class EncodingQOI : public EncodingBase {
          protected:
            EncodingRet sendRegion(const EncoderStream*, const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId) const;
            BinaryBuf encodeBGRx(const FrameBuffer &, const XCB::Region &, const PixelFormat &) const;

          public:
            void writeFrameBufferTo(const EncoderStream*, const FrameBuffer&, StreamBuf&) const override;
            const char* getTypeName(void) const override {
                return isZQOI() ? "ZQOI" : "QOI";
            }

            EncodingQOI(bool lz4 = false) : EncodingBase(lz4 ? ENCODING_LTSM_ZQOI : ENCODING_LTSM_QOI) {}

            bool isZQOI(void) const {
                return getType() == ENCODING_LTSM_ZQOI;
            }
        };
    }
}

#endif // _LIBRFB_ENCODINGS_
