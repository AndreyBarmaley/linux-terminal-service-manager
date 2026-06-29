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

#include <algorithm>

#include <boost/asio/post.hpp>
#include <boost/endian.hpp>

#include "ltsm_application.h"
#include "librfb_decodings.h"
#include "ltsm_tools.h"

#ifdef LTSM_DECODING
#ifdef LTSM_DECODING_LZ4
#include "lz4.h"
#endif
#ifdef LTSM_DECODING_TJPG
#include "turbojpeg.h"
#endif
#endif

#include "SDL.h"

namespace LTSM {
    // AsioDecoderStream
    void RFB::AsioDecoderStream::recvRaw(void* ptr, size_t len) const {
        sock_.sync_recv_buf(ptr, len);
    }

    // DecoderStream
    std::vector<uint8_t> RFB::DecoderStream::recvData(size_t len) const {
        std::vector<uint8_t> res(len);
        if(len) {
            recvRaw(res.data(), res.size());
        }
        return res;
    }

    uint8_t RFB::DecoderStream::recvInt8(void) const {
        uint8_t byte;
        recvRaw(& byte, 1);
        return byte;
    }

    uint16_t RFB::DecoderStream::recvIntLE16(void) const {
        uint16_t v;
        recvRaw(& v, 2);
        return boost::endian::little_to_native(v);
    }

    uint32_t RFB::DecoderStream::recvIntLE32(void) const {
        uint32_t v;
        recvRaw(& v, 4);
        return boost::endian::little_to_native(v);
    }

    uint16_t RFB::DecoderStream::recvIntBE16(void) const {
        uint16_t v;
        recvRaw(& v, 2);
        return boost::endian::big_to_native(v);
    }

    uint32_t RFB::DecoderStream::recvIntBE32(void) const {
        uint32_t v;
        recvRaw(& v, 4);
        return boost::endian::big_to_native(v);
    }

    uint32_t RFB::DecoderStream::recvPixel(const PixelFormat & clientPf) const {
        switch(clientPf.bytePerPixel()) {
            case 4:
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
                return recvIntBE32();
#else
                return recvIntLE32();
#endif
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
                return recvIntBE16();
#else
                return recvIntLE16();
#endif

            case 1:
                return recvInt8();

            default:
                break;
        }

        Application::error("{}: {}", NS_FuncNameV, "unknown format");
        throw rfb_error(NS_FuncNameS);
    }

    uint32_t RFB::DecoderStream::recvCPixel(const PixelFormat & clientPf) const {
        if(clientPf.bitsPerPixel() != 32) {
            return recvPixel(clientPf);
        }

        uint32_t pixel = 0;
        auto ptr = reinterpret_cast<uint8_t*>(& pixel);

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)

        if(! clientPf.leastSignificant()) {
            ptr++;
        }

#else

        if(clientPf.leastSignificant()) {
            ptr++;
        }

#endif

        recvRaw(ptr, 3);
        return pixel;
    }

    uint32_t RFB::DecoderStream::recvRunLength(void) const {
        uint32_t length = 0;

        while(true) {
            auto val = recvInt8();
            length += val;

            if(val != 255) {
                length += 1;
                break;
            }
        }

        return length;
    }

    // DecodingBase
    RFB::DecodingBase::DecodingBase(int v) : type_(v) {
        Application::info("{}: init decoding: {}", NS_FuncNameV, encodingName(type_));
    }

    int RFB::DecodingBase::type(void) const {
        return type_;
    }

    void RFB::DecodingRaw::updateRegionBuf(std::vector<uint8_t> && buf, const DecoderRender & rend, const XCB::Region & reg) {
        Application::debug(DebugType::Enc, "{}: decoding region {}, data length: {}", NS_FuncNameV, reg, buf.size());

        const uint32_t pitch = reg.width * rend.clientFormat().bytePerPixel();
        // server transform pixels to client format
        rend.updateRawPixels(reg, std::move(buf), pitch, rend.clientFormat());
    }

    void RFB::DecodingRRE::updateRegionStream(const DecoderStream & st, const DecoderRender & rend, const XCB::Region & reg) {
        Application::debug(DebugType::Enc, "{}: decoding region {}", NS_FuncNameV, reg);

        auto subRects = st.recvIntBE32();
        auto bgColor = st.recvPixel(rend.clientFormat());

        Application::trace(DebugType::Enc, "{}: back pixel: {:#010x}, sub rects: {}", NS_FuncNameV, bgColor, subRects);

        rend.fillPixel(reg, bgColor);

        while(0 < subRects--) {
            XCB::Region dst;
            auto pixel = st.recvPixel(rend.clientFormat());

            if(isCoRRE()) {
                dst.x = st.recvInt8();
                dst.y = st.recvInt8();
                dst.width = st.recvInt8();
                dst.height = st.recvInt8();
            } else {
                dst.x = st.recvIntBE16();
                dst.y = st.recvIntBE16();
                dst.width = st.recvIntBE16();
                dst.height = st.recvIntBE16();
            }

            Application::trace(DebugType::Enc, "{}: sub region: {}", NS_FuncNameV, dst);

            dst.x += reg.x;
            dst.y += reg.y;

            if(dst.x + dst.width > reg.x + reg.width || dst.y + dst.height > reg.y + reg.height) {
                Application::error("{}: {}", NS_FuncNameV, "sub region out of range");
                throw rfb_error(NS_FuncNameS);
            }

            rend.fillPixel(dst, pixel);
        }
    }

    void RFB::DecodingHexTile::updateRegionStream(const DecoderStream & st, const DecoderRender & rend, const XCB::Region & reg) {
        if(16 < reg.width || 16 < reg.height) {
            Application::error("{}: invalid hextile region: {}", NS_FuncNameV, reg);
            throw rfb_error(NS_FuncNameS);
        }

        Application::debug(DebugType::Enc, "{}: decoding region: {}", NS_FuncNameV, reg);

        updateRegionStreamColors(st, rend, reg);
    }

    void RFB::DecodingHexTile::updateRegionStreamColors(const DecoderStream & st, const DecoderRender & rend, const XCB::Region & reg) {
        auto flag = st.recvInt8();

        Application::trace(DebugType::Enc, "{}: sub encoding mask: {:#04x}, sub region: {}",
                           NS_FuncNameV, flag, reg);

        if(flag & RFB::HEXTILE_RAW) {
            Application::trace(DebugType::Enc, "{}: type: {}", NS_FuncNameV, "raw");
            const uint32_t pitch = reg.width * rend.clientFormat().bytePerPixel();
            auto pixels = st.recvData(static_cast<size_t>(pitch) * reg.height);
            rend.updateRawPixels(reg, std::move(pixels), pitch, rend.serverFormat());
            return;
        }

        if(flag & RFB::HEXTILE_BACKGROUND) {
            bgColor = st.recvPixel(rend.clientFormat());
            Application::trace(DebugType::Enc, "{}: type: {}, pixel: {:#010x}", NS_FuncNameV, "background", bgColor);
        }

        rend.fillPixel(reg, bgColor);

        if(flag & HEXTILE_FOREGROUND) {
            fgColor = st.recvPixel(rend.clientFormat());
            flag &= ~HEXTILE_COLOURED;

            Application::trace(DebugType::Enc, "{}: type: {}, pixel: {:#010x}", NS_FuncNameV, "foreground", fgColor);
        }

        if(flag & HEXTILE_SUBRECTS) {
            int subRects = st.recvInt8();
            XCB::Region dst;

            Application::trace(DebugType::Enc, "{}: type: {}, count: {}", NS_FuncNameV, "subrects", subRects);

            while(0 < subRects--) {
                auto pixel = fgColor;

                if(flag & HEXTILE_COLOURED) {
                    pixel = st.recvPixel(rend.clientFormat());

                    Application::trace(DebugType::Enc, "{}: type: {}, pixel: {:#010x}", NS_FuncNameV, "colored", pixel);
                }

                auto val1 = st.recvInt8();
                auto val2 = st.recvInt8();
                dst.x = (0x0F & (val1 >> 4));
                dst.y = (0x0F & val1);
                dst.width = 1 + (0x0F & (val2 >> 4));
                dst.height = 1 + (0x0F & val2);

                Application::trace(DebugType::Enc, "{}: type: {}, region: {}, pixel: {:#010x}",
                                   NS_FuncNameV, "subrects", dst, pixel);

                dst.x += reg.x;
                dst.y += reg.y;

                if(dst.x + dst.width > reg.x + reg.width || dst.y + dst.height > reg.y + reg.height) {
                    Application::error("{}: {}", NS_FuncNameV, "sub region out of range");
                    throw rfb_error(NS_FuncNameS);
                }

                rend.fillPixel(dst, pixel);
            }
        }
    }

    /// BufferWrapper
    class BufferWrapper : public RFB::DecoderStream {
        StreamBufRef sb_;

      public:
        BufferWrapper(std::span<const uint8_t> buf) {
            sb_.reset(buf.data(), buf.size());
        }

        void recvRaw(void* ptr, size_t len) const override {
            sb_.readTo(ptr, len);
        }
    };
 
    void RFB::DecodingTRLE::updateRegionStream(const DecoderStream & st, const DecoderRender & rend, const XCB::Region & reg) {
        Application::debug(DebugType::Enc, "{}: decoding region {}", NS_FuncNameV, reg);

        const XCB::Size bsz(64, 64);

        if(isZRLE()) {
            auto len = st.recvIntBE32();
            auto zip = st.recvData(len);
            auto buf = zlib_->inflateData(zip, Z_SYNC_FLUSH);
    
            Application::trace(DebugType::Enc, "{}: inflate data, in length: {}, out length: {}", NS_FuncNameV, zip.size(), buf.size());
            BufferWrapper wrap(buf);

            for(const auto & reg0 : reg.XCB::Region::divideBlocks(bsz)) {
                updateSubRegion(wrap, rend, reg0);
            }
        } else {
            for(const auto & reg0 : reg.XCB::Region::divideBlocks(bsz)) {
                updateSubRegion(st, rend, reg0);
            }
        }
    }

    void RFB::DecodingTRLE::updateSubRegion(const DecoderStream & st, const DecoderRender & rend, const XCB::Region & reg) {
        auto type = st.recvInt8();

        Application::trace(DebugType::Enc, "{}: sub encoding type: {:#04x}, sub region: {}, zrle: {}",
                           NS_FuncNameV, type, reg, (int) isZRLE());

        // trle raw
        if(0 == type) {
            Application::trace(DebugType::Enc, "{}: type: {}", NS_FuncNameV, "raw");

            for(auto coord = XCB::PointIterator(0, 0, reg.toSize()); coord.isValid(); ++coord) {
                auto pixel = st.recvCPixel(rend.clientFormat());
                rend.setPixel(reg.topLeft() + coord, pixel);
            }

            Application::trace(DebugType::Enc, "{}: complete: {}", NS_FuncNameV, "raw");
        } else if(1 == type) {
            // trle solid
            auto solid = st.recvCPixel(rend.clientFormat());
            rend.fillPixel(reg, solid);

            Application::trace(DebugType::Enc, "{}: type: {}, pixel: {:#010x}", NS_FuncNameV, "solid", solid);
        } else if(2 <= type && type <= 16) {
            size_t field = 1;

            if(4 < type) {
                field = 4;
            } else if(2 < type) {
                field = 2;
            }

            size_t bits = field * reg.width;
            size_t rowsz = bits >> 3;

            if((rowsz << 3) < bits) {
                rowsz++;
            }

            // recv palette
            std::vector<int> palette(type);

            for(auto & val : palette) {
                val = st.recvCPixel(rend.clientFormat());
            }

            Application::trace(DebugType::Enc, "{}: type: {}, size: {}", NS_FuncNameV, "packed palette", palette.size());

            // recv packed rows
            for(int oy = 0; oy < reg.height; ++oy) {
                Tools::StreamBitsUnpack sb(st.recvData(rowsz), reg.width, field);

                for(int ox = reg.width - 1; 0 <= ox; --ox) {
                    auto pos = reg.topLeft() + XCB::Point(ox, oy);
                    auto index = sb.popValue(field);

                    Application::trace(DebugType::Enc, "{}: type: {}, pos: {}, index: {}", NS_FuncNameV, "packed palette", pos, index);

                    if(index >= palette.size()) {
                        Application::error("{}: {}", NS_FuncNameV, "index out of range");
                        throw rfb_error(NS_FuncNameS);
                    }

                    rend.setPixel(pos, palette[index]);
                }
            }

            Application::trace(DebugType::Enc, "{}: complete: {}", NS_FuncNameV, "packed palette");
        } else if((17 <= type && type <= 127) || type == 129) {
            Application::error("{}: {}", NS_FuncNameV, "invalid trle type");
            throw rfb_error(NS_FuncNameS);
        } else if(128 == type) {
            Application::trace(DebugType::Enc, "{}: type: {}", NS_FuncNameV, "plain rle");

            auto coord = XCB::PointIterator(0, 0, reg.toSize());

            while(coord.isValid()) {
                auto pixel = st.recvCPixel(rend.clientFormat());
                auto runLength = st.recvRunLength();

                Application::trace(DebugType::Enc, "{}: type: {}, pixel: {:#010x}, length: {}", NS_FuncNameV, "plain rle", pixel, runLength);

                while(runLength--) {
                    rend.setPixel(reg.topLeft() + coord, pixel);
                    ++coord;

                    if(! coord.isValid() && runLength) {
                        Application::error("{}: {}", NS_FuncNameV, "plain rle: coord out of range");
                        throw rfb_error(NS_FuncNameS);
                    }
                }
            }

            Application::trace(DebugType::Enc, "{}: complete: {}", NS_FuncNameV, "plain rle");
        } else if(130 <= type) {
            size_t palsz = type - 128;
            std::vector<int> palette(palsz);

            for(auto & val : palette) {
                val = st.recvCPixel(rend.clientFormat());
            }

            Application::trace(DebugType::Enc, "{}: type: {}, size: {}", NS_FuncNameV, "rle palette", palsz);

            auto coord = XCB::PointIterator(0, 0, reg.toSize());

            while(coord.isValid()) {
                auto index = st.recvInt8();

                if(index < 128) {
                    if(index >= palette.size()) {
                        Application::error("{}: {}", NS_FuncNameV, "index out of range");
                        throw rfb_error(NS_FuncNameS);
                    }

                    auto pixel = palette[index];
                    rend.setPixel(reg.topLeft() + coord, pixel);
                    ++coord;
                } else {
                    index -= 128;

                    if(index >= palette.size()) {
                        Application::error("{}: {}", NS_FuncNameV, "index out of range");
                        throw rfb_error(NS_FuncNameS);
                    }

                    auto pixel = palette[index];
                    auto runLength = st.recvRunLength();

                    Application::trace(DebugType::Enc, "{}: type: {}, index: {}, length: {}", NS_FuncNameV, "rle palette", index, runLength);

                    while(runLength--) {
                        rend.setPixel(reg.topLeft() + coord, pixel);
                        ++coord;

                        if(! coord.isValid() && runLength) {
                            Application::error("{}: {}", NS_FuncNameV, "rle palette: coord out of range");
                            throw rfb_error(NS_FuncNameS);
                        }
                    }
                }
            }

            Application::trace(DebugType::Enc, "{}: complete: {}", NS_FuncNameV, "rle palette");
        }
    }

    void RFB::DecodingZlib::updateRegionBuf(std::vector<uint8_t> && buf, const DecoderRender & rend, const XCB::Region & reg) {
        Application::debug(DebugType::Enc, "{}: decoding region: {}, data length: {}", NS_FuncNameV, reg, buf.size());

        const uint32_t pitch = reg.width * rend.clientFormat().bytePerPixel();
        auto pixels = zlib_->inflateData(buf, Z_SYNC_FLUSH);

        assert(pixels.size() >= static_cast<size_t>(pitch) * reg.height);
        rend.updateRawPixels(reg, std::move(pixels), pitch, rend.serverFormat());
    }

#ifdef LTSM_DECODING
#ifdef LTSM_DECODING_LZ4
    /// DecodingLZ4
    void RFB::DecodingLZ4::updateRegionBuf(std::vector<uint8_t> && buf, const DecoderRender & rend, const XCB::Region & reg) {
        Application::debug(DebugType::Enc, "{}: decoding region: {}, data length: {}", NS_FuncNameV, reg, buf.size());

        const uint32_t pitch = rend.serverFormat().bytePerPixel() * reg.width;
        auto runJob = std::bind(&DecodingLZ4::decodeLZ4, this,
                                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
        // move job to thread pool
        rend.postDecoderJob(std::move(runJob), std::move(buf), reg, pitch, rend.serverFormat());
    }

    std::vector<uint8_t> lz4DecompressSafe(std::span<const uint8_t> buf, size_t outlen) {
        std::vector<uint8_t> res(outlen);
        int ret = LZ4_decompress_safe((const char*) buf.data(), (char*) res.data(), buf.size(), res.size());
        if(ret < 0) {
            Application::error("{}: {} failed, ret: {}", NS_FuncNameV, "LZ4_decompress_safe", ret);
            throw rfb_error(NS_FuncNameS);
        }
        res.resize(ret);
        return res;
    }

    std::vector<uint8_t> RFB::DecodingLZ4::decodeLZ4(const std::vector<uint8_t> & buf, const XCB::Region & reg, uint32_t pitch, const PixelFormat & pf) const {
        return lz4DecompressSafe(buf, pitch * reg.height);
    }
#endif // LTSM_DECODING_LZ4

#ifdef LTSM_DECODING_TJPG
    /// DecodingTJPG
    void RFB::DecodingTJPG::updateRegionBuf(std::vector<uint8_t> && buf, const DecoderRender & rend, const XCB::Region & reg) {
        Application::debug(DebugType::Enc, "{}: decoding region: {}, data length: {}", NS_FuncNameV, reg, buf.size());

        const uint32_t pitch = reg.width * tjPixelSize[pixfmt];
        auto runJob = std::bind(&DecodingTJPG::decodeJPG, this,
                                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
        // format transformed: jpg pixel data only in this format
        const PixelFormat jpgFormat = platformBigEndian() ? XRGB32 : BGRX32;
        // move job to thread pool
        rend.postDecoderJob(std::move(runJob), std::move(buf), reg, pitch, jpgFormat);
    }

    std::vector<uint8_t> RFB::DecodingTJPG::decodeJPG(const std::vector<uint8_t> & buf, const XCB::Region & reg, uint32_t pitch, const PixelFormat & pf) const {
        std::unique_ptr<void, int(*)(void*)> jpeg{ tjInitDecompress(), tjDestroy };

        if(! jpeg) {
            Application::error("{}: {} failed", NS_FuncNameV, "tjInitDecompress");
            throw rfb_error(NS_FuncNameS);
        }

        // thread buf
        std::vector<uint8_t> res(pitch * reg.height);

        if(0 > tjDecompress2(jpeg.get(), buf.data(), buf.size(),
            res.data(), reg.width, 0, reg.height, pixfmt, TJFLAG_FASTDCT)) {
#ifdef tjGetErrorCode
            int err = tjGetErrorCode(jpeg.get());
            const char* str = tjGetErrorStr2(jpeg.get());
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "tjDecompress", str, err);
#else
            Application::error("{}: {} failed, error: `{}'", NS_FuncNameV, "tjDecompress", tjGetErrorStr());
#endif
            throw rfb_error(NS_FuncNameS);
        }

        return res;
    }
#endif // LTSM_DECODING_TJPG

    /// DecodingQOI
    void RFB::DecodingQOI::updateRegionBuf(std::vector<uint8_t> && buf, const DecoderRender & rend, const XCB::Region & reg) {
        Application::debug(DebugType::Enc, "{}: decoding region: {}, data length: {}", NS_FuncNameV, reg, buf.size());

        const uint32_t pitch = rend.serverFormat().bytePerPixel() * reg.width;

        if(isZQOI()) {
#ifdef LTSM_DECODING_LZ4
            auto runJob = std::bind(&DecodingQOI::decodeZQOI, this,
                                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
            // move job to thread pool
            rend.postDecoderJob(std::move(runJob), std::move(buf), reg, pitch, rend.serverFormat());
            return;
#else
            Application::error("{}: {} failed", NS_FuncNameV, "encoding type");
            throw rfb_error(NS_FuncNameS);
#endif
        }

        auto runJob = std::bind(&DecodingQOI::decodeBGRx, this,
                                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
        // move job to thread pool
        rend.postDecoderJob(std::move(runJob), std::move(buf), reg, pitch, rend.serverFormat());
    }

    namespace QOI {
        enum Tag {
            INDEX = 0x00,
            DIFF = 0x40,
            LUMA = 0x80,
            RUN = 0xC0,
            RGB = 0xFE,
            RGBA = 0xFF,
            MASK2 = 0xC0
        };

        inline uint32_t packBGRx(const Color & col, const PixelFormat & pf) {
            return (static_cast<uint32_t>(col.b) << pf.bshift()) |
                   (static_cast<uint32_t>(col.g) << pf.gshift()) | (static_cast<uint32_t>(col.r) << pf.rshift());
        }

        inline Color unpackBGRx(uint32_t pixel, const PixelFormat & pf) {
            Color col;
            col.r = (pixel & pf.rmask()) >> pf.rshift();
            col.g = (pixel & pf.gmask()) >> pf.gshift();
            col.b = (pixel & pf.bmask()) >> pf.bshift();
            return col;
        }

        inline uint8_t hashIndex64RGB(const Color & col) {
            return (col.r * 3 + col.g * 5 + col.b * 7) % 64;
        }
    }

#ifdef LTSM_DECODING_LZ4
    std::vector<uint8_t> RFB::DecodingQOI::decodeZQOI(const std::vector<uint8_t> & buf, const XCB::Region & rsz, uint32_t pitch, const PixelFormat & pf) const {
        return decodeBGRx(lz4DecompressSafe(buf, pitch * rsz.height), rsz, pitch, pf);
     }
#endif

    std::vector<uint8_t> RFB::DecodingQOI::decodeBGRx(const std::vector<uint8_t> & buf, const XCB::Region & rsz, uint32_t pitch, const PixelFormat & pf) const {
        std::array<int64_t, 64> hashes;
        hashes.fill(-1);

        int64_t prevPixel = -1;
        std::uint8_t run = 0;

        StreamBufRef sb(buf.data(), buf.size());
        std::vector<uint8_t> res(static_cast<size_t>(pitch) * rsz.height, 0);
        FrameBuffer fb(res.data(), XCB::Region{0, 0, rsz.width, rsz.height}, pf, pitch);

        for(int16_t py = 0; py < rsz.height; ++py) {
            for(int16_t px = 0; px < rsz.width; ++px) {
                if(run) {
                    run--;
                    fb.setPixel(XCB::Point{px, py}, prevPixel);
                    continue;
                }

                if(0 == sb.last()) {
                    Application::error("{}: {}", NS_FuncNameV, "unknown format");
                    throw rfb_error(NS_FuncNameS);
                }

                auto type = sb.readInt8();

                if(type == QOI::Tag::RGB) {
                    Color col;
                    col.r = sb.readInt8();
                    col.g = sb.readInt8();
                    col.b = sb.readInt8();

                    prevPixel = QOI::packBGRx(col, pf);
                    fb.setPixel(XCB::Point{px, py}, prevPixel);
                    hashes[QOI::hashIndex64RGB(col)] = prevPixel;
                    continue;
                }

                if((type & QOI::Tag::MASK2) == QOI::Tag::INDEX) {
                    if(hashes.size() <= type) {
                        Application::error("{}: {}", NS_FuncNameV, "unknown index");
                        throw rfb_error(NS_FuncNameS);
                    }

                    if(0 > hashes[type]) {
                        Application::error("{}: {}", NS_FuncNameV, "unknown type");
                        throw rfb_error(NS_FuncNameS);
                    }

                    prevPixel = hashes[type];
                    fb.setPixel(XCB::Point{px, py}, prevPixel);
                    continue;
                }

                if((type & QOI::Tag::MASK2) == QOI::Tag::DIFF) {
                    auto col = QOI::unpackBGRx(prevPixel, pf);

                    col.r += ((type >> 4) & 0x03) - 2;
                    col.g += ((type >> 2) & 0x03) - 2;
                    col.b += (type & 0x03) - 2;

                    prevPixel = QOI::packBGRx(col, pf);
                    fb.setPixel(XCB::Point{px, py}, prevPixel);
                    hashes[QOI::hashIndex64RGB(col)] = prevPixel;
                    continue;
                }

                if((type & QOI::Tag::MASK2) == QOI::Tag::LUMA) {
                    auto lm = sb.readInt8();
                    int8_t vg = (type & 0x3f) - 32;

                    auto col = QOI::unpackBGRx(prevPixel, pf);

                    col.r += vg - 8 + ((lm >> 4) & 0x0f);
                    col.g += vg;
                    col.b += vg - 8 + (lm & 0x0f);

                    prevPixel = QOI::packBGRx(col, pf);
                    fb.setPixel(XCB::Point{px, py}, prevPixel);
                    hashes[QOI::hashIndex64RGB(col)] = prevPixel;
                    continue;
                }

                if((type & QOI::Tag::MASK2) == QOI::Tag::RUN) {
                    run = type & 0x3f;
                    fb.setPixel(XCB::Point{px, py}, prevPixel);
                    continue;
                } else {
                    Application::error("{}: {}", NS_FuncNameV, "unknown tag");
                    throw rfb_error(NS_FuncNameS);
                }
            }
        }

        return res;
    }
#endif // LTSM_DECODING
}
