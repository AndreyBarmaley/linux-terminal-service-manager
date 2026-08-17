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

#include <cmath>
#include <chrono>
#include <cassert>
#include <numeric>
#include <cstring>
#include <utility>

#include <boost/endian.hpp>

#include "ltsm_tools.h"
#include "librfb_server.h"
#include "ltsm_application.h"
#include "librfb_encodings.h"

#include "lz4.h"
#include "turbojpeg.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM {
    const int HEADER_REGION_SIZE = 12;

    // EncodePacket
    RFB::EncodePacket::EncodePacket(uint32_t rez, bool t2)
        : StreamBuf(HEADER_REGION_SIZE + sizeof(uint32_t) + rez), type_v2_{t2} {

        if(type_v2_) {
            // v2 format: <header><uint32><data>
            writeZero(HEADER_REGION_SIZE + sizeof(uint32_t));
        } else {
            // v1 format: <header><data>
            writeZero(HEADER_REGION_SIZE);
        }
    }

    void RFB::EncodePacket::writeHeader(uint32_t type, const XCB::Region & reg) {
        auto ptr = rawbuf().data();
        // header format: <regx:be16,regy:be16,regw:be16,regh:be16,type:be32>

        endian::store_big_u16(ptr, reg.x);
        ptr += sizeof(uint16_t);
        endian::store_big_u16(ptr, reg.y);
        ptr += sizeof(uint16_t);
        endian::store_big_u16(ptr, reg.width);
        ptr += sizeof(uint16_t);
        endian::store_big_u16(ptr, reg.height);
        ptr += sizeof(uint16_t);
        endian::store_big_u32(ptr, type);
    }

    std::span<const uint8_t> RFB::EncodePacket::span(void) const {
        return std::span{ rawbuf() };
    }

    std::span<uint8_t> RFB::EncodePacket::encodeData(void) {
        auto span = std::span{ rawbuf() };
        const uint32_t data_off = type_v2_ ? HEADER_REGION_SIZE + sizeof(uint32_t) : HEADER_REGION_SIZE;
        assert(last() >= data_off);
        return span.subspan(data_off);
    }

    void RFB::EncodePacket::writeData(std::span<const uint8_t> buf) {
        const uint32_t data_off = type_v2_ ? HEADER_REGION_SIZE + sizeof(uint32_t) : HEADER_REGION_SIZE;
        assert(last() >= data_off);
        rawbuf().resize(data_off);
        write(buf);
    }

    void RFB::EncodePacket::writeDataSize(uint32_t len) {
        if(!type_v2_) {
            return;
        }

        const uint32_t data_off = HEADER_REGION_SIZE + sizeof(uint32_t);

        if(len) {
            assert(last() >= (data_off + len));
            auto ptr = rawbuf().data() + HEADER_REGION_SIZE;
            endian::store_big_u32(ptr, len);
            rawbuf().resize(data_off + len);
        } else {
            assert(last() >= data_off);
            const uint32_t data_sz = last() - data_off;

            auto ptr = rawbuf().data() + HEADER_REGION_SIZE;
            endian::store_big_u32(ptr, data_sz);
        }
    }

    int RFB::EncodePacket::writeRawPixel(uint32_t pixel, uint8_t bpp, bool be) {
        switch(bpp) {
            case 4:
                if(be) {
                    writeIntBE32(pixel);
                } else {
                    writeIntLE32(pixel);
                }
                return 4;

            case 2:
                if(be) {
                    writeIntBE16(pixel);
                } else {
                    writeIntLE16(pixel);
                }
                return 2;

            case 1:
                writeInt8(pixel);
                return 1;

            default:
                Application::error("{}: {}", NS_FuncNameV, "unknown pixel format");
                break;
        }

        throw rfb_error(NS_FuncNameS);
    }

    int RFB::EncodePacket::writeRunLength(uint32_t length) {
        if(0 == length) {
            Application::error("{}: {}", NS_FuncNameV, "length is zero");
            throw rfb_error(NS_FuncNameS);
        }

        int res = 0;

        while(255 < length) {
            writeInt8(255);
            res += 1;
            length -= 255;
        }

        writeInt8((length - 1) % 255);
        return res + 1;
    }

    int RFB::EncodePacket::writePixel(const EncoderStream* st, uint32_t pixel) {
        return writeRawPixel(st->clientFormat().convertFrom(st->serverFormat(), pixel),
                             st->clientFormat().bytePerPixel(), st->clientIsBigEndian());
    }

    int RFB::EncodePacket::writeCPixel(const EncoderStream* st, uint32_t pixel) {
        if(st->clientFormat().bitsPerPixel() != 32) {
            return writePixel(st, pixel);
        }

        if(! st->serverFormat().compare(st->clientFormat(), true /* skip alpha */)) {
            pixel = st->clientFormat().convertFrom(st->serverFormat(), pixel);
        }

        auto ptr = reinterpret_cast<const uint8_t*>(& pixel);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)

        if(! st->serverFormat().leastSignificant()) {
            ptr++;
        }

#else

        if(st->serverFormat().leastSignificant()) {
            ptr++;
        }

#endif
        write(std::span{ptr, 3});
        return 3;
    }

    int RFB::EncodePacket::writeRawRegionPixels(const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb) {
        int ret = 0;
	for(uint16_t py = 0; py < reg.height; ++py) {
            const uint8_t* pitch = fb.pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px) {
                auto ptr = pitch + ((reg.x + px) * fb.bytePerPixel());
                auto pix = FrameBuffer::rawPixel(ptr, fb.bitsPerPixel(), platformBigEndian());
                ret += writePixel(st, pix);
            }
        }
	return ret;
    }

    BinaryBuf RFB::EncodePacket::getRawRegionPixels(const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb) {
        EncodePacket sb(reg.width * reg.height * fb.bytePerPixel());
        sb.writeRawRegionPixels(st, reg, fb);
	return sb.rawbuf();
    }

    // EncodingBase
    RFB::EncodingBase::EncodingBase(int v) : type_(v) {
        Application::info("{}: init encoding: {}", NS_FuncNameV, encodingName(type_));
    }

    int RFB::EncodingBase::getType(void) const {
        return type_;
    }

    std::list<XCB::RegionPixel> RFB::EncodingBase::rreProcessing(const XCB::Region & badreg, const FrameBuffer & fb,
            uint32_t skipPixel) {
        std::list<XCB::RegionPixel> goods;
        std::list<XCB::Region> bads1 = { badreg };
        std::list<XCB::Region> bads2;

        do {
            while(! bads1.empty()) {
                for(const auto & subreg : bads1.front().divideCounts(2, 2)) {
                    auto pixel = fb.pixel(subreg.topLeft());

                    if((subreg.width == 1 && subreg.height == 1) || fb.allOfPixel(pixel, subreg)) {
                        if(pixel != skipPixel) {
                            if(goods.empty()) {
                                goods.emplace_back(subreg, pixel);
                                continue;
                            }
                            auto& back = goods.back();
                            // maybe join prev
                            if(back.first.y == subreg.y && back.first.height == subreg.height &&
                               back.first.x - back.first.width == subreg.x && back.second == pixel) {
                                back.first.width -= subreg.width;
                            } else {
                                goods.emplace_back(subreg, pixel);
                            }
                        }
                    } else {
                        bads2.push_back(subreg);
                    }
                }

                bads1.pop_front();
            }

            if(bads2.empty()) {
                break;
            }

            bads2.swap(bads1);
            bads2.clear();
        } while(! bads1.empty());

        return goods;
    }

    // EncodingRaw
    RFB::FrameBufferPackets RFB::EncodingRaw::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        const XCB::Point top(reg0.x, reg0.y);
        // single thread: stream spec
        auto buf = writeRegionTo(st, top, fb, reg0 - top);

        RFB::FrameBufferPackets packets;
        packets.emplace_back(std::move(buf));
        
        return packets;
    }

    RFB::EncodingRet RFB::EncodingRaw::writeRegionTo(const EncoderStream* st, const XCB::Point & top,
            const FrameBuffer & fb, XCB::Region reg) const {
        Application::debug(DebugType::Enc, "{}:  region: {}", NS_FuncNameV, reg);

        // make encoder region packet
        EncodePacket sb(fb.width() * fb.height() * fb.bytePerPixel(), false /* type v1 */);
        sb.writeHeader(getType(), reg + top);
        sb.writeRawRegionPixels(st, reg, fb);

        return std::move(sb.rawbuf());
    }

    // EncodingRRE
    RFB::FrameBufferPackets RFB::EncodingRRE::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz = isCoRRE() ? XCB::Size(64, 64) : XCB::Size(128, 128);
        auto regions = reg0.divideBlocks(bsz);

        auto runJob = std::bind(&EncodingRRE::writeRegionTo, this, st, top, std::cref(fb), std::placeholders::_1);
        FrameBufferPackets packets;

        // move job to thread pool
        for(auto & reg : regions) {
            jobs_.emplace_back(st->postEncoderJob(std::move(runJob), reg - top));
        }

        // and wait jobs
        for(auto & job : jobs_) {
            packets.emplace_back(job.get());
        }

        jobs_.clear();
        return packets;
    }

    RFB::EncodingRet RFB::EncodingRRE::writeRegionTo(const EncoderStream* st, const XCB::Point & top, const FrameBuffer & fb, XCB::Region reg) const {

        // make encoder region packet
        EncodePacket sb(4096, false /* type v1 */);
        sb.writeHeader(getType(), reg + top);

        auto map = fb.pixelMapWeight(reg);
        if(map.empty()) {
            Application::error("{}: {}", NS_FuncNameV, "pixels map is empty");
            throw rfb_error(NS_FuncNameS);
        }

        if(map.size() > 1) {
            auto back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = rreProcessing(reg, fb, back);
            //const size_t rawLength = reg.width * reg.height * fb.bytePerPixel();
            //const size_t rreLength = 4 + fb.bytePerPixel() + goods.size() * (fb.bytePerPixel() + (isCoRRE() ? 4 : 8));

            Application::debug(DebugType::Enc, "{}: region: {}, back pixel {:#010x}, sub rects: {}",
                               NS_FuncNameV, reg + top, back, goods.size());

            writeRectsTo(sb, st, reg, fb, back, goods);
        } else {
            // if(map.size() == 1)
            int back = fb.pixel(reg.topLeft());

            Application::debug(DebugType::Enc, "{}: region: {}, back pixel {:#010x}, {}",
                               NS_FuncNameV, reg + top, back, "solid");

            // num sub rects
            sb.writeIntBE32(1);
            // back pixel
            sb.writePixel(st, back);
            /* one fake sub region : RRE requires */
            // subrect pixel
            sb.writePixel(st, back);

            // subrect region (relative coords)
            if(isCoRRE()) {
                sb.writeInt8(0).
            	    writeInt8(0).
            	    writeInt8(1).
            	    writeInt8(1);
            } else {
                sb.writeIntBE16(0).
            	    writeIntBE16(0).
            	    writeIntBE16(1).
            	    writeIntBE16(1);
            }
        }

        return std::move(sb.rawbuf());
    }

    void RFB::EncodingRRE::writeRectsTo(EncodePacket& sb, const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
                                     int back, const std::list<XCB::RegionPixel> & rreList) const {
        // num sub rects
        sb.writeIntBE32(rreList.size());
        // back pixel
        sb.writePixel(st, back);

        for(const auto & pair : rreList) {
            // subrect pixel
            sb.writePixel(st, pair.pixel());
            auto & region = pair.region();

            // subrect region (relative coords)
            if(isCoRRE()) {
                sb.writeInt8(region.x - reg.x);
                sb.writeInt8(region.y - reg.y);
                sb.writeInt8(region.width);
                sb.writeInt8(region.height);
            } else {
                sb.writeIntBE16(region.x - reg.x);
                sb.writeIntBE16(region.y - reg.y);
                sb.writeIntBE16(region.width);
                sb.writeIntBE16(region.height);
            }

            Application::trace(DebugType::Enc, "{}: region: {}, back pixel {:#010x}",
                               NS_FuncNameV, region - reg.topLeft(), pair.pixel());
        }
    }

    // EncodingHexTile
    RFB::FrameBufferPackets RFB::EncodingHexTile::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz(16, 16);
        auto regions = reg0.divideBlocks(bsz);

        auto runJob = std::bind(&EncodingHexTile::writeRegionTo, this, st, top, std::cref(fb), std::placeholders::_1);
        FrameBufferPackets packets;

        // move job to thread pool
        for(auto & reg : regions) {
            jobs_.emplace_back(st->postEncoderJob(std::move(runJob), reg - top));
        }

        // and wait jobs
        for(auto & job : jobs_) {
            packets.emplace_back(job.get());
        }

        jobs_.clear();
        return packets;
    }

    RFB::EncodingRet RFB::EncodingHexTile::writeRegionTo(const EncoderStream* st, const XCB::Point & top,
            const FrameBuffer & fb, XCB::Region reg) const {
        // make encoder region packet
        EncodePacket sb(4096, false /* type v1 */);
        sb.writeHeader(getType(), reg + top);

        auto map = fb.pixelMapWeight(reg);
        if(map.empty()) {
            Application::error("{}: {}", NS_FuncNameV, "pixels map is empty");
            throw rfb_error(NS_FuncNameS);
        }

        if(map.size() == 1) {
            int back = fb.pixel(reg.topLeft());

            Application::debug(DebugType::Enc, "{}: region: {}, back pixel: {:#010x}, {}",
                               NS_FuncNameV, reg + top, back, "solid");

            // hextile flags
            sb.writeInt8(RFB::HEXTILE_BACKGROUND);
            sb.writePixel(st, back);
        } else if(map.size() > 1) {
            // no wait, worked
            auto back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = rreProcessing(reg, fb, back);
            // all other color
            bool foreground = std::ranges::all_of(goods, [col = goods.front().second](auto & pair) {
                return pair.pixel() == col;
            });

            const size_t hextileRawLength = 1 + reg.width * reg.height * fb.bytePerPixel();

            if(foreground) {
                const size_t hextileForegroundLength = 2 + 2 * fb.bytePerPixel() + goods.size() * 2;

                // compare with raw
                if(hextileRawLength < hextileForegroundLength) {
                    Application::debug(DebugType::Enc, "{}: region: {}, {}",
                                       NS_FuncNameV, reg + top, "raw");

                    writeRegionToRaw(sb, st, reg, fb);
                } else {
                    Application::debug(DebugType::Enc, "{}: region: {}, back pixel: {:#010x}, sub rects: {}, {}",
                                       NS_FuncNameV, reg + top, back, goods.size(), "foreground");

                    writeRegionToForeground(sb, st, reg, fb, back, goods);
                }
            } else {
                const size_t hextileColoredLength = 2 + fb.bytePerPixel() + goods.size() * (2 + fb.bytePerPixel());

                // compare with raw
                if(hextileRawLength < hextileColoredLength) {
                    Application::debug(DebugType::Enc, "{}: region: {}, {}",
                                       NS_FuncNameV, reg + top, "raw");

                    writeRegionToRaw(sb, st, reg, fb);
                } else {
                    Application::debug(DebugType::Enc, "{}: region: {}, back pixel: {:#010x}, sub rects: {}, {}",
                                       NS_FuncNameV, reg + top, back, goods.size(), "colored");

                    writeRegionToColored(sb, st, reg, fb, back, goods);
                }
            }
        }

        return std::move(sb.rawbuf());
    }

    void RFB::EncodingHexTile::writeRegionToColored(EncodePacket& sb, const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
            int back, const std::list<XCB::RegionPixel> & rreList) const {
        // hextile flags
        sb.writeInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_COLOURED | RFB::HEXTILE_SUBRECTS);
        // hextile background
        sb.writePixel(st, back);
        // hextile subrects
        sb.writeInt8(rreList.size());

        for(const auto & pair : rreList) {
            auto & region = pair.region();
            sb.writePixel(st, pair.pixel());
            sb.writeInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            sb.writeInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            Application::trace(DebugType::Enc, "{}: region: {}, back pixel: {:#010x}",
                               NS_FuncNameV, region - reg.topLeft(), pair.pixel());
        }
    }

    void RFB::EncodingHexTile::writeRegionToForeground(EncodePacket& sb, const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
            int back, const std::list<XCB::RegionPixel> & rreList) const {
        // hextile flags
        sb.writeInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_FOREGROUND | RFB::HEXTILE_SUBRECTS);
        // hextile background
        sb.writePixel(st, back);
        // hextile foreground
        sb.writePixel(st, rreList.front().second);
        // hextile subrects
        sb.writeInt8(rreList.size());

        for(const auto & pair : rreList) {
            auto & region = pair.region();
            sb.writeInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            sb.writeInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            Application::trace(DebugType::Enc, "{}: region: {}", NS_FuncNameV, region - reg.topLeft());
        }
    }

    void RFB::EncodingHexTile::writeRegionToRaw(EncodePacket& sb, const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb) const {
        // hextile flags
        sb.writeInt8(RFB::HEXTILE_RAW);
        sb.writeRawRegionPixels(st, reg, fb);
    }

    // EncodingTRLE
    RFB::FrameBufferPackets RFB::EncodingTRLE::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        const XCB::Size bsz(64, 64);
        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);

        auto runJob = std::bind(&EncodingTRLE::writeRegionTo, this, st, top, std::cref(fb), std::placeholders::_1);
        FrameBufferPackets packets;

        // move job to thread pool
        for(auto & reg : regions) {
            jobs_.emplace_back(st->postEncoderJob(std::move(runJob), reg - top));
        }

        // and wait jobs
        for(auto & job : jobs_) {
            packets.emplace_back(job.get());
        }

        jobs_.clear();
        return packets;
    }

    RFB::EncodingRet RFB::EncodingTRLE::writeRegionTo(const EncoderStream* st, const XCB::Point & top,
            const FrameBuffer & fb, XCB::Region reg) const {
        // make encoder region packet
        EncodePacket sb(reg.width * reg.height * 8 / 3, false /* type v1 */);
        sb.writeHeader(getType(), reg + top);

        auto map = fb.pixelMapPalette(reg);
        if(map.size() == 1) {
            int back = fb.pixel(reg.topLeft());

            Application::debug(DebugType::Enc, "{}: region: {}, back pixel: {:#010x}, {}",
                               NS_FuncNameV, reg + top, back, "solid");

            // subencoding type: solid tile
            sb.writeInt8(1);
            sb.writeCPixel(st, back);
        } else if(2 <= map.size() && map.size() <= 16) {
            auto fieldWidth = Tools::StreamBitsPack::Field::Val1;

            if(4 < map.size()) {
                fieldWidth = Tools::StreamBitsPack::Field::Val4;
            } else if(2 < map.size()) {
                fieldWidth = Tools::StreamBitsPack::Field::Val2;
            }

            Application::debug(DebugType::Enc, "{}: region: {}, palsz: {}, packed: {}",
                               NS_FuncNameV, reg + top, map.size(), static_cast<int>(fieldWidth));

            writeRegionToPacked(sb, st, reg, fb, fieldWidth, map);
        } else {
            auto rleList = fb.FrameBuffer::toRLE(reg);
            // rle plain size
            const size_t rlePlainLength = std::accumulate(rleList.begin(), rleList.end(), 1,
            [](int v, auto & pair) {
                return v + 3 + std::floor((pair.second - 1) / 255.0) + 1;
            });

            // rle palette size (2, 127)
            const size_t rlePaletteLength = 1 < rleList.size() &&
                                            rleList.size() < 128 ? std::accumulate(rleList.begin(), rleList.end(), 1 + 3 * map.size(),
            [](int v, auto & pair) {
                return v + 1 + std::floor((pair.second - 1) / 255.0) + 1;
            }) : 0xFFFF;

            // raw length
            const size_t rawLength = 1 + 3 * reg.width * reg.height;

            if(rlePlainLength < rlePaletteLength && rlePlainLength < rawLength) {
                Application::debug(DebugType::Enc, "{}: region: {}, length: {}, rle plain",
                                   NS_FuncNameV, reg + top, rleList.size());

                writeRegionToPlain(sb, st, reg, fb, rleList);
            } else if(rlePaletteLength < rlePlainLength && rlePaletteLength < rawLength) {
                Application::debug(DebugType::Enc, "{}: region: {}, pal size: {}, length: {}, rle palette",
                                   NS_FuncNameV, reg + top, map.size(), rleList.size());

                writeRegionToPalette(sb, st, reg, fb, map, rleList);
            } else {
                Application::debug(DebugType::Enc, "{}: region: {}, raw",
                                   NS_FuncNameV, reg + top);

                writeRegionToRaw(sb, st, reg, fb);
            }
        }

        return std::move(sb.rawbuf());
    }

    void RFB::EncodingTRLE::writeRegionToPacked(EncodePacket& sb, const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
            const Tools::StreamBitsPack::Field & field, const PixelMapPalette & pal) const {
        // subencoding type: packed palette
        sb.writeInt8(pal.size());

        // send palette
        for(const auto & pair : pal) {
            sb.writeCPixel(st, pair.first);
        }

        const size_t rez = (reg.width * reg.height) >> 2;
        Tools::StreamBitsPack sbp(rez ? rez : 32);

        // send packed rows
        for(uint16_t py = 0; py < reg.height; ++py) {
            const uint8_t* pitch = fb.pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px) {
                auto ptr = pitch + ((reg.x + px) * fb.bytePerPixel());
                auto pix = FrameBuffer::rawPixel(ptr, fb.bitsPerPixel(), platformBigEndian());
                auto index = pal.findColorIndex(pix);
                assertm(0 <= index, "palette color not found");
                sbp.pushValue(index, field);
            }

            sbp.pushAlign();
        }

        sb.write(sbp.toVector());

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto & vec = sbp.toVector();
            std::string str = Tools::hexString(vec, 2);
            Application::debug(DebugType::Enc, "{}: packed stream: {}", NS_FuncNameV, str);
        }
    }

    void RFB::EncodingTRLE::writeRegionToPlain(EncodePacket& sb, const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
                                            const PixelLengthList & rle) const {
        // subencoding type: rle plain
        sb.writeInt8(128);

        // send rle content
        for(const auto & pair : rle) {
            sb.writeCPixel(st, pair.pixel());
            sb.writeRunLength(pair.length());
        }
    }

    void RFB::EncodingTRLE::writeRegionToPalette(EncodePacket& sb, const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
            const PixelMapPalette & pal, const PixelLengthList & rle) const {
        // subencoding type: rle palette
        sb.writeInt8(pal.size() + 128);

        // send palette
        for(const auto & pair : pal) {
            sb.writeCPixel(st, pair.first);
        }

        // send rle indexes
        for(const auto & pair : rle) {
            auto index = pal.findColorIndex(pair.pixel());
            assertm(0 <= index, "palette color not found");

            if(1 == pair.length()) {
                sb.writeInt8(index);
            } else {
                sb.writeInt8(index + 128);
                sb.writeRunLength(pair.length());
            }
        }
    }

    void RFB::EncodingTRLE::writeRegionToRaw(EncodePacket& sb, const EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb) const {
        // subencoding type: raw
        sb.writeInt8(0);

        // send pixels
        for(uint16_t py = 0; py < reg.height; ++py) {
            const uint8_t* pitch = fb.pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px) {
                auto ptr = pitch + ((reg.x + px) * fb.bytePerPixel());
                auto pix = FrameBuffer::rawPixel(ptr, fb.bitsPerPixel(), platformBigEndian());
                sb.writeCPixel(st, pix);
            }
        }
    }

    // EncodingZRLE
    RFB::FrameBufferPackets RFB::EncodingZRLE::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        const XCB::Size bsz(64, 64);
        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);

        auto runJob = std::bind(&EncodingZRLE::writeRegionTo, this, st, top, std::cref(fb), std::placeholders::_1);
        FrameBufferPackets packets;

        // move job to thread pool
        for(auto & reg : regions) {
            jobs_.emplace_back(st->postEncoderJob(std::move(runJob), reg - top));
        }

        // and wait jobs
        for(auto & job : jobs_) {
            auto trle = job.get();
            // skipped header (12 bytes)
            auto header = std::span(trle.data(), HEADER_REGION_SIZE);
            StreamBuf sb(HEADER_REGION_SIZE + zlib_->deflateBound(trle.size()));
            // make encoder region packet
            sb.write(header);
            auto zip = zlib_->deflateData(std::span(trle).subspan(HEADER_REGION_SIZE), Z_SYNC_FLUSH);
            sb.writeIntBE32(zip.size());
            sb.write(zip);

            packets.emplace_back(std::move(sb.rawbuf()));
        }

        jobs_.clear();
        return packets;
    }

    // EncodingZlib
    RFB::EncodingZlib::EncodingZlib(int lev) : EncodingBase(ENCODING_ZLIB), zlevel(lev) {
        if(zlevel < Z_BEST_SPEED || zlevel > Z_BEST_COMPRESSION) {
            zlevel = Z_BEST_SPEED;
        }

        zlib_ = std::make_unique<ZLib::DeflateBase>(zlevel);
    }

    RFB::FrameBufferPackets RFB::EncodingZlib::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        const XCB::Point top(reg0.x, reg0.y);

        // single thread: zlib stream spec
        auto buf = writeRegionTo(st, top, fb, reg0 - top);

        FrameBufferPackets packets;
        packets.emplace_back(std::move(buf));

        return packets;
    }

    RFB::EncodingRet RFB::EncodingZlib::writeRegionTo(const EncoderStream* st, const XCB::Point & top,
            const FrameBuffer & fb, XCB::Region reg) const {

        BinaryBuf bb;

	if(st->isDisplaySize(fb.region()) &&
	    st->serverFormat() == st->clientFormat()) {
    	    bb = zlib_->deflateData(fb.span(), Z_SYNC_FLUSH);
	} else {
            bb = EncodePacket::getRawRegionPixels(st, reg, fb);
            bb = zlib_->deflateData(bb, Z_SYNC_FLUSH);
        }

        // make encoder region packet
        EncodePacket sb(bb.size(), true /* type v2 */);
        sb.writeHeader(getType(), reg + top);
        sb.writeData(bb);
        sb.writeDataSize(bb.size());

        return std::move(sb.rawbuf());
    }

    bool RFB::EncodingZlib::setEncodingOptions(const std::forward_list<std::string> & encopts) {
        bool fullscreenUpdate = false;

        for(const auto & str : encopts) {
            // parce zlevel
            if(startsWith(str, "zlev")) {
                if(auto it = str.find(':'); it != str.npos) {
                    if(++it == str.npos) {
                        continue;
                    }

                    try {
                        zlevel = std::stoi(str.substr(it));

                        if(zlevel < Z_BEST_SPEED || zlevel > Z_BEST_COMPRESSION) {
                            Application::warning("{}: incorrect value, zlevel: {}", NS_FuncNameV, zlevel);
                            zlevel = Z_BEST_SPEED;
                        }
                    } catch(...) {
                    }

                    Application::info("{}: set zlevel: {}", NS_FuncNameV, zlevel);
                    zlib_ = std::make_unique<ZLib::DeflateBase>(zlevel);
                }
            }
        }

        return fullscreenUpdate;
    }

    /// EncodingLZ4
    RFB::FrameBufferPackets RFB::EncodingLZ4::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        // calculate block size
        XCB::Size bsz;
        const size_t blocksz = 256 * 256;

        if(st->isDisplaySize(fb.region())) {
            bsz = 1 < st->encodingThreads() ?
                  XCB::Size(fb.width(), fb.height() / st->encodingThreads()) :
                  fb.region().toSize();
        } else if(fb.width() * fb.height() < blocksz) {
            // one rect
            bsz = fb.region().toSize();
        } else {
            bsz = XCB::Size(fb.width(), blocksz / fb.height());
        }

        assertm(! bsz.isEmpty(), "block size empty");

        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);

        auto runJob = std::bind(&EncodingLZ4::writeRegionTo, this, st, top, std::cref(fb), std::placeholders::_1);
        FrameBufferPackets packets;

        // move job to thread pool
        for(auto & reg : regions) {
            jobs_.emplace_back(st->postEncoderJob(std::move(runJob), reg - top));
        }

        // and wait jobs
        for(auto & job : jobs_) {
            packets.emplace_back(job.get());
        }

        jobs_.clear();
        return packets;
    }

    int lz4CompressFastTo(std::span<const uint8_t> buf, std::span<uint8_t> res) {
        const int acceleration = 1;
        int ret = LZ4_compress_fast((const char*) buf.data(), (char*) res.data(), buf.size(), res.size(), acceleration);

        if(ret < 0) {
            Application::error("{}: {} failed, ret: {}", NS_FuncNameV, "LZ4_compress_fast", ret);
            throw rfb_error(NS_FuncNameS);
        }

        return ret;
    }

    RFB::EncodingRet RFB::EncodingLZ4::writeRegionTo(const EncoderStream* st, const XCB::Point & top,
            const FrameBuffer & fb, XCB::Region reg) const {


        if(fb.width() == reg.width) {
            auto buf = std::span{ fb.pitchData(reg.y),
                                  fb.pitchSize() * reg.height };
            return writeCompressPacket(buf, reg + top);
        }
        
        auto fb2 = fb.copyRegion(reg);
        auto buf = std::span{ fb2.pitchData(0),
                              fb2.pitchSize() * reg.height };
        return writeCompressPacket(buf, reg + top);
    }

    RFB::EncodingRet RFB::EncodingLZ4::writeCompressPacket(std::span<const uint8_t> buf, XCB::Region reg) const {
        size_t lz4Len = LZ4_compressBound(buf.size());
        EncodePacket sb(lz4Len, true /* type v2 */);

        sb.writeZero(lz4Len);
        lz4Len = lz4CompressFastTo(buf, sb.encodeData());

        sb.writeHeader(getType(), reg);
        sb.writeDataSize(lz4Len);

        return std::move(sb.rawbuf());
    }

    /// EncodingTJPG
    bool RFB::EncodingTJPG::setEncodingOptions(const std::forward_list<std::string> & encopts) {
        bool fullscreenUpdate = false;

        for(const auto & str : encopts) {
            // parce quality
            if(startsWith(str, "qual")) {
                if(auto it = str.find(':'); it != str.npos) {
                    if(++it == str.npos) {
                        continue;
                    }

                    try {
                        jpegQuality = std::stoi(str.substr(it));

                        if(10 > jpegQuality || 100 < jpegQuality) {
                            Application::warning("{}: incorrect value, quality: {}", NS_FuncNameV, jpegQuality);
                            jpegQuality = 85;
                        } else {
                            fullscreenUpdate = true;
                        }
                    } catch(...) {
                    }

                    Application::info("{}: set quality: {}", NS_FuncNameV, jpegQuality);
                }
            } else if(startsWith(str, "samp")) {
                // parce sample
                if(auto it = str.find(':'); it != str.npos) {
                    if(++it == str.npos) {
                        continue;
                    }

                    if(0 == str.compare(it, 3, "420")) {
                        jpegSamp = TJSAMP_420;
                        fullscreenUpdate = true;
                    } else if(0 == str.compare(it, 3, "422")) {
                        jpegSamp = TJSAMP_422;
                        fullscreenUpdate = true;
                    } else if(0 == str.compare(it, 3, "440")) {
                        jpegSamp = TJSAMP_440;
                        fullscreenUpdate = true;
                    } else if(0 == str.compare(it, 3, "444")) {
                        jpegSamp = TJSAMP_444;
                        fullscreenUpdate = true;
                    } else if(0 == str.compare(it, 3, "411")) {
                        jpegSamp = TJSAMP_411;
                        fullscreenUpdate = true;
                    } else if(0 == str.compare(it, 4, "gray")) {
                        jpegSamp = TJSAMP_GRAY;
                        fullscreenUpdate = true;
                    }

                    Application::info("{}: set sample: {}", NS_FuncNameV, str.substr(it));
                }
            }
        }

        return fullscreenUpdate;
    }

    RFB::FrameBufferPackets RFB::EncodingTJPG::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        // calculate block size
        XCB::Size bsz;
        const size_t blocksz = 256 * 256;

        if(st->isDisplaySize(fb.region())) {
            bsz = 1 < st->encodingThreads() ?
                  XCB::Size(fb.width(), fb.height() / st->encodingThreads()) :
                  fb.region().toSize();
        } else if(fb.width() * fb.height() < blocksz) {
            // one rect
            bsz = fb.region().toSize();
        } else {
            bsz = XCB::Size(fb.width(), blocksz / fb.height());
        }

        assertm(! bsz.isEmpty(), "block size empty");

        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);

        auto runJob = std::bind(&EncodingTJPG::writeRegionTo, this, st, top, std::cref(fb), std::placeholders::_1);
        FrameBufferPackets packets;

        // move job to thread pool
        for(auto & reg : regions) {
            jobs_.emplace_back(st->postEncoderJob(std::move(runJob), reg - top));
        }

        // and wait jobs
        for(auto & job : jobs_) {
            packets.emplace_back(job.get());
        }

        jobs_.clear();
        return packets;
    }

    RFB::EncodingRet RFB::EncodingTJPG::writeRegionTo(const EncoderStream* st, const XCB::Point & top, const FrameBuffer & fb, XCB::Region reg) const {
        std::unique_ptr<void, int(*)(void*)> jpeg{ tjInitCompress(), tjDestroy };

        if(! jpeg) {
            Application::error("{}: {} failed", NS_FuncNameV, "tjInitCompress");
            throw rfb_error(NS_FuncNameS);
        }

        // TJPF: RGBX, BGRX, XRGB, XBGR
        // from lowest to highest memory address
        // Xorg always 0xXXRRGGBB
        const int pixFmt = TJPF_BGRX;

        long unsigned int jpegSize = tjBufSize(reg.width, reg.height, jpegSamp);
        // thread buffer
        EncodePacket sb(jpegSize, true /* type v2 */);
        sb.writeZero(jpegSize);

        auto span = sb.encodeData();
        auto jpegBuf = span.data();
        int ret = 0;

        if(fb.pixelFormat().bitsPerPixel() != 24) {
            const auto jpgPixelFormat = platformBigEndian() ? XRGB32 : BGRX32;
            auto fb2 = fb.copyRegionFormat(reg, jpgPixelFormat);

            ret = tjCompress2(jpeg.get(), fb2.pitchData(0), reg.width, fb2.pitchSize(), reg.height, pixFmt,
                              & jpegBuf, & jpegSize, jpegSamp, jpegQuality, TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
        } else if(fb.width() == reg.width) {
            ret = tjCompress2(jpeg.get(), fb.pitchData(reg.y), reg.width, fb.pitchSize(), reg.height, pixFmt,
                              & jpegBuf, & jpegSize, jpegSamp, jpegQuality, TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
        } else {
            auto fb2 = fb.copyRegion(reg);
            ret = tjCompress2(jpeg.get(), fb2.pitchData(0), reg.width, fb2.pitchSize(), reg.height, pixFmt,
                              & jpegBuf, & jpegSize, jpegSamp, jpegQuality, TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
        }

        if(0 > ret) {
#ifdef tjGetErrorCode
            int err = tjGetErrorCode(jpeg.get());
            const char* str = tjGetErrorStr2(jpeg.get());
            Application::error("{}: {} failed, error: `{}', code: {}", NS_FuncNameV, "tjCompress", str, err);
#else
            Application::error("{}: {} failed, error: `{}'", NS_FuncNameV, "tjCompress", tjGetErrorStr());
#endif
            throw rfb_error(NS_FuncNameS);
        }

        sb.writeHeader(getType(), reg + top);
        sb.writeDataSize(jpegSize);

        return std::move(sb.rawbuf());
    }

    /// EncodingQOI
    RFB::FrameBufferPackets RFB::EncodingQOI::getFrameBufferPackets(const EncoderStream* st, const FrameBuffer& fb) const {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "{}: type: {}, region: {}", NS_FuncNameV,
                           getTypeName(), reg0);

        // calculate block size
        XCB::Size bsz;
        const size_t blocksz = 256 * 256;

        if(st->isDisplaySize(fb.region())) {
            bsz = 1 < st->encodingThreads() ?
                  XCB::Size(fb.width(), fb.height() / st->encodingThreads()) :
                  fb.region().toSize();
        } else if(fb.width() * fb.height() < blocksz) {
            // one rect
            bsz = fb.region().toSize();
        } else {
            bsz = XCB::Size(fb.width(), blocksz / fb.height());
        }

        assertm(! bsz.isEmpty(), "block size empty");

        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);

        auto runJob = std::bind(&EncodingQOI::writeRegionTo, this, st, top, std::cref(fb), std::placeholders::_1);
        FrameBufferPackets packets;

        // move job to thread pool
        for(auto & reg : regions) {
            jobs_.emplace_back(st->postEncoderJob(std::move(runJob), reg - top));
        }

        // and wait jobs
        for(auto & job : jobs_) {
            packets.emplace_back(job.get());
        }

        jobs_.clear();
        return packets;
    }

    RFB::EncodingRet RFB::EncodingQOI::writeRegionTo(const EncoderStream* st, const XCB::Point & top, const FrameBuffer & fb, XCB::Region reg) const {

        EncodePacket sb(reg.width * reg.height * 4 / 3, true /* type v2 */);
        writeEncodeBGRx(fb, reg, st->clientFormat(), sb);
        sb.writeHeader(getType(), reg + top);
        sb.writeDataSize();

        return std::move(sb.rawbuf());
    }

    namespace QOI {
        enum Tag {
            INDEX = 0x00,
            DIFF = 0x40,
            LUMA = 0x80,
            RUN = 0xC0,
            RGB = 0xFE,
            RGBA = 0xFF
        };

        inline uint8_t hashIndex64RGB(const Color & col) {
            return (col.r * 3 + col.g * 5 + col.b * 7) % 64;
        }
    }

    void RFB::EncodingQOI::writeEncodeBGRx(const FrameBuffer & fb, const XCB::Region & reg, const PixelFormat & clientPf, EncodePacket & sb) const {
        std::array<int64_t, 64> hashes;
        hashes.fill(-1);

        int64_t prevPixel = -1;
        std::uint8_t run = 0;

        for(int16_t py = 0; py < reg.height; ++py) {
            for(int16_t px = 0; px < reg.width; ++px) {
                const bool pixelLast = (py == reg.height - 1) && (px == reg.width - 1);
                const uint32_t pixel = clientPf.pixel(fb.color(reg.topLeft() + XCB::Point{px, py}));

                // QOI::Tag::RUN
                if(pixel == prevPixel) {
                    run++;

                    if(run == 62 || pixelLast) {
                        sb.writeInt8(QOI::Tag::RUN | (run - 1));
                        run = 0;
                    }

                    continue;
                }

                if(run) {
                    sb.writeInt8(QOI::Tag::RUN | (run - 1));
                    run = 0;
                }

                auto col = clientPf.color(pixel);

                // QOI::Tag::INDEX
                const uint8_t index = QOI::hashIndex64RGB(col);

                if(hashes[index] == pixel) {
                    sb.writeInt8(QOI::Tag::INDEX | index);
                    prevPixel = pixel;
                    continue;
                }

                hashes[index] = pixel;

                if(prevPixel < 0) {
                    sb.writeInt8(QOI::Tag::RGB);
                    sb.writeInt8(col.r);
                    sb.writeInt8(col.g);
                    sb.writeInt8(col.b);
                    prevPixel = pixel;
                    continue;
                }

                auto pcol = clientPf.color(prevPixel);

                const int8_t vr = col.r - pcol.r;
                const int8_t vg = col.g - pcol.g;
                const int8_t vb = col.b - pcol.b;

                // QOI::Tag::DIFF
                if(vr > static_cast<int8_t>(-3) && vr < static_cast<int8_t>(2) &&
                   vg > static_cast<int8_t>(-3) && vg < static_cast<int8_t>(2) &&
                   vb > static_cast<int8_t>(-3) && vb < static_cast<int8_t>(2)) {
                    sb.writeInt8(QOI::Tag::DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2));
                    prevPixel = pixel;
                    continue;
                }

                const int8_t vg_r = vr - vg;
                const int8_t vg_b = vb - vg;

                // QOI::Tag::LUMA
                if(vg_r > static_cast<int8_t>(-9) && vg_r < static_cast<int8_t>(8) &&
                   vg > static_cast<int8_t>(-33) && vg < static_cast<int8_t>(32) &&
                   vg_b > static_cast<int8_t>(-9) && vg_b < static_cast<int8_t>(8)) {
                    sb.writeInt8(QOI::Tag::LUMA | (vg + 32));
                    sb.writeInt8((vg_r + 8) << 4 | (vg_b + 8));
                    prevPixel = pixel;
                    continue;
                }

                // QOI::Tag::RGB
                sb.writeInt8(QOI::Tag::RGB);
                sb.writeInt8(col.r);
                sb.writeInt8(col.g);
                sb.writeInt8(col.b);

                prevPixel = pixel;
            }
        }

        // padding
        const std::array<uint8_t, 8> qoiPadding{0, 0, 0, 0, 0, 0, 0, 1};
        sb.write(qoiPadding);
    }

    /// EncodingQOI
    RFB::EncodingRet RFB::EncodingZQOI::writeRegionTo(const EncoderStream* st, const XCB::Point & top, const FrameBuffer & fb, XCB::Region reg) const {

        EncodePacket bb(reg.width * reg.height * 4 / 3);
        writeEncodeBGRx(fb, reg, st->clientFormat(), bb);

        size_t lz4Len = LZ4_compressBound(bb.last());
        EncodePacket sb(lz4Len, true /* type v2 */);

        sb.writeZero(lz4Len);
        lz4Len = lz4CompressFastTo(bb.rawbuf(), sb.encodeData());

        sb.writeHeader(getType(), reg + top);
        sb.writeDataSize(lz4Len);

        return std::move(sb.rawbuf());
    }
}
