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

#ifndef _LIBRFB_DECODINGS_
#define _LIBRFB_DECODINGS_

#include <vector>
#include <functional>

#include "ltsm_librfb.h"
#include "ltsm_streambuf.h"
#include "ltsm_boost_socket.h"

#ifdef LTSM_WITH_BOOST
#include <boost/asio/post.hpp>
#endif

#ifdef LTSM_DECODING_TJPG
#include "turbojpeg.h"
#endif

#include "ltsm_zlib.h"

namespace LTSM {
    namespace RFB {
        using PostDecoderJobCb = std::function<BinaryBuf(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &)>;

        /// DecoderStream
        class DecoderStream {
          public:
            DecoderStream() = default;
            virtual ~DecoderStream() = default;

            virtual void recvRaw(void*, size_t) const = 0;

            std::vector<uint8_t> recvData(size_t) const;
            uint8_t recvInt8(void) const;
            uint16_t recvIntLE16(void) const;
            uint32_t recvIntLE32(void) const;
            uint16_t recvIntBE16(void) const;
            uint32_t recvIntBE32(void) const;

            uint32_t recvPixel(const PixelFormat &) const;
            uint32_t recvCPixel(const PixelFormat &) const;
            uint32_t recvRunLength(void) const;
        };

        class AsioDecoderStream : public DecoderStream {
            AsyncSocketBase& sock_;

          public:
            AsioDecoderStream(AsyncSocketBase& st) : sock_(st) {}
            ~AsioDecoderStream() = default;

            void recvRaw(void*, size_t) const override;
        };

        /// DecoderRender
        class DecoderRender {
          public:
            DecoderRender() = default;
            virtual ~DecoderRender() = default;

            virtual const PixelFormat & serverFormat(void) const = 0;
            virtual const PixelFormat & clientFormat(void) const = 0;

            virtual void setPixel(const XCB::Point &, uint32_t pixel) const = 0;
            virtual void fillPixel(const XCB::Region &, uint32_t pixel) const = 0;

            virtual void updateRawPixels(const XCB::Region &, std::vector<uint8_t>&& buf, uint32_t pitch, const PixelFormat &) const = 0;

            virtual XCB::Size clientSize(void) const = 0;
            virtual void postDecoderJob(PostDecoderJobCb &&, std::vector<uint8_t> &&, const XCB::Region &, uint32_t pitch, const PixelFormat &) const = 0;
            virtual void waitDecoderJobs(void) const {}

            virtual int clientPrefferedVideoEncoding(void) const {
                return 0;
            }
            virtual int clientPrefferedAudioEncoding(void) const {
                return 0;
            }
        };

        /// DecodingBase
        class DecodingBase {
            const int type_ = 0;

          public:
            DecodingBase(int v);
            virtual ~DecodingBase() = default;

            virtual void updateRegionBuf(BinaryBuf &&, const DecoderRender &, const XCB::Region &) { /* empty */ }
            virtual void updateRegionStream(const DecoderStream &, const DecoderRender &, const XCB::Region &) { /* empty */ }
            virtual void resizedEvent(const XCB::Size &) { /* empty */ }

            int type(void) const;
        };

        /// DecodingRaw
        class DecodingRaw : public DecodingBase {
          public:
            void updateRegionBuf(BinaryBuf &&, const DecoderRender &, const XCB::Region &) override;

            DecodingRaw() : DecodingBase(ENCODING_RAW) {}
        };

        /// DecodingRRE
        class DecodingRRE : public DecodingBase {
          public:
            void updateRegionStream(const DecoderStream &, const DecoderRender &, const XCB::Region &) override;

            DecodingRRE(bool co = false) : DecodingBase(co ? ENCODING_CORRE : ENCODING_RRE) {}

            bool isCoRRE(void) const {
                return type() == ENCODING_CORRE;
            }
        };

        /// DecodingHexTile
        class DecodingHexTile : public DecodingBase {
            int bgColor = -1;
            int fgColor = -1;

          protected:
            void updateRegionStreamColors(const DecoderStream &, const DecoderRender &, const XCB::Region &);

          public:
            void updateRegionStream(const DecoderStream &, const DecoderRender &, const XCB::Region &) override;

            DecodingHexTile(bool zlib = false) : DecodingBase(zlib ? ENCODING_ZLIBHEX : ENCODING_HEXTILE) {}

            bool isZlibHex(void) const {
                return type() == ENCODING_ZLIBHEX;
            }
        };

        /// DecodingTRLE
        class DecodingTRLE : public DecodingBase {
            std::unique_ptr<ZLib::InflateBase> zlib_;

          protected:
            void updateSubRegion(const DecoderStream &, const DecoderRender &, const XCB::Region &);

          public:
            void updateRegionStream(const DecoderStream &, const DecoderRender &, const XCB::Region &) override;

            DecodingTRLE(bool zip = false) : DecodingBase(zip ? ENCODING_ZRLE : ENCODING_TRLE),
                zlib_{std::make_unique<ZLib::InflateBase>()} {}

            bool isZRLE(void) const {
                return type() == ENCODING_ZRLE;
            }
        };

        /// DecodingZlib
        class DecodingZlib : public DecodingBase {
            std::unique_ptr<ZLib::InflateBase> zlib_;

          public:
            void updateRegionBuf(BinaryBuf &&, const DecoderRender &, const XCB::Region &) override;

            DecodingZlib() : DecodingBase(ENCODING_ZLIB),
                zlib_{std::make_unique<ZLib::InflateBase>()} {}
        };

#ifdef LTSM_DECODING
#ifdef LTSM_DECODING_LZ4
        /// DecodingLZ4
        class DecodingLZ4 : public DecodingBase {

          protected:
            std::vector<uint8_t> decodeLZ4(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &) const;

          public:
            void updateRegionBuf(BinaryBuf &&, const DecoderRender &, const XCB::Region &) override;

            DecodingLZ4() : DecodingBase(ENCODING_LTSM_LZ4) {}
        };
#endif
#ifdef LTSM_DECODING_TJPG
        /// DecodingTJPG
        class DecodingTJPG : public DecodingBase {

            // TJPF: RGBX, BGRX, XRGB, XBGR
            // from lowest to highest memory address
            const int pixfmt = TJPF_BGRX;

          protected:
            std::vector<uint8_t> decodeJPG(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &) const;

          public:
            void updateRegionBuf(BinaryBuf &&, const DecoderRender &, const XCB::Region &) override;

            DecodingTJPG() : DecodingBase(ENCODING_LTSM_TJPG) {}
        };
#endif
        /// DecodingQOI
        class DecodingQOI : public DecodingBase {

          protected:
#ifdef LTSM_DECODING_LZ4
            std::vector<uint8_t> decodeZQOI(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &) const;
#endif
            std::vector<uint8_t> decodeBGRx(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &) const;

          public:
            void updateRegionBuf(BinaryBuf &&, const DecoderRender &, const XCB::Region &) override;

            DecodingQOI(bool lz4 = false) : DecodingBase(lz4 ? ENCODING_LTSM_ZQOI : ENCODING_LTSM_QOI) {}

            bool isZQOI(void) const {
                return type() == ENCODING_LTSM_ZQOI;
            }
        };
#endif
    }
}

#endif // _LIBRFB_DECODINGS_
