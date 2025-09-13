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

#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "librfb_decodings.h"

#ifdef LTSM_DECODING
 #ifdef LTSM_DECODING_LZ4
  #include "lz4.h"
 #endif
 #ifdef LTSM_DECODING_TJPG
  #include "turbojpeg.h"
 #endif
#endif

#include "SDL.h"

namespace LTSM
{
    // DecoderStream
    int RFB::DecoderStream::recvPixel(void)
    {
        switch(clientFormat().bytePerPixel())
        {
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

        Application::error("%s: %s", __FUNCTION__, "unknown format");
        throw rfb_error(NS_FuncName);
    }

    void RFB::DecoderStream::recvRegionUpdatePixels(const XCB::Region & reg)
    {
        uint32_t pitch = reg.width * clientFormat().bytePerPixel();
        auto pixels = recvData(static_cast<size_t>(pitch) * reg.height);
        updateRawPixels(reg, pixels.data(), pitch, serverFormat());
    }

    int RFB::DecoderStream::recvCPixel(void)
    {
        if(clientFormat().bitsPerPixel() != 32)
        {
            return recvPixel();
        }

        uint32_t pixel = 0;
        auto ptr = reinterpret_cast<uint8_t*>(& pixel);

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        if(! clientFormat().leastSignificant())
            ptr++;
#else
        if(clientFormat().leastSignificant())
            ptr++;
#endif

        recvRaw(ptr, 3);
        return pixel;
    }

    size_t RFB::DecoderStream::recvRunLength(void)
    {
        size_t length = 0;

        while(true)
        {
            auto val = recvInt8();
            length += val;

            if(val != 255)
            {
                length += 1;
                break;
            }
        }

        return length;
    }

    size_t RFB::DecoderStream::recvZlibData(ZLib::InflateStream* zlib, bool uint16sz)
    {
        size_t zipsz = 0;

        if(uint16sz)
        {
            zipsz = recvIntBE16();
        }
        else
        {
            zipsz = recvIntBE32();
        }

        auto zip = recvData(zipsz);

        Application::trace(DebugType::Enc, "%s: compress data length: %lu", __FUNCTION__, zip.size());

        zlib->appendData(zip);
        return zipsz;
    }

    // DecodingBase
    RFB::DecodingBase::DecodingBase(int v) : type(v)
    {
        Application::info("%s: init decoding: %s", __FUNCTION__, encodingName(type));
    }

    int RFB::DecodingBase::getType(void) const
    {
        return type;
    }

    void RFB::DecodingBase::setThreads(int v)
    {
        threads = v;
    }

    void RFB::DecodingRaw::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        Application::debug(DebugType::Enc, "%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);

        cli.recvRegionUpdatePixels(reg);
    }

    void RFB::DecodingRRE::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        Application::debug(DebugType::Enc, "%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);

        auto subRects = cli.recvIntBE32();
        auto bgColor = cli.recvPixel();

        Application::trace(DebugType::Enc, "%s: back pixel: 0x%08x, sub rects: %" PRIu32, __FUNCTION__, bgColor, subRects);

        cli.fillPixel(reg, bgColor);

        while(0 < subRects--)
        {
            XCB::Region dst;
            auto pixel = cli.recvPixel();

            if(isCoRRE())
            {
                dst.x = cli.recvInt8();
                dst.y = cli.recvInt8();
                dst.width = cli.recvInt8();
                dst.height = cli.recvInt8();
            }
            else
            {
                dst.x = cli.recvIntBE16();
                dst.y = cli.recvIntBE16();
                dst.width = cli.recvIntBE16();
                dst.height = cli.recvIntBE16();
            }

            Application::trace(DebugType::Enc, "%s: sub region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, dst.x, dst.y,
                                   dst.width, dst.height);

            dst.x += reg.x;
            dst.y += reg.y;

            if(dst.x + dst.width > reg.x + reg.width || dst.y + dst.height > reg.y + reg.height)
            {
                Application::error("%s: %s", __FUNCTION__, "sub region out of range");
                throw rfb_error(NS_FuncName);
            }

            cli.fillPixel(dst, pixel);
        }
    }

    void RFB::DecodingHexTile::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        if(16 < reg.width || 16 < reg.height)
        {
            Application::error("%s: invalid hextile region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               reg.x, reg.y, reg.width, reg.height);
            throw rfb_error(NS_FuncName);
        }

        Application::debug(DebugType::Enc, "%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);

        updateRegionColors(cli, reg);
    }

    void RFB::DecodingHexTile::updateRegionColors(DecoderStream & cli, const XCB::Region & reg)
    {
        auto flag = cli.recvInt8();

        Application::trace(DebugType::Enc, "%s: sub encoding mask: 0x%02" PRIx8 ", sub region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16
                               "]", __FUNCTION__, flag, reg.x, reg.y, reg.width, reg.height);

        if(flag & RFB::HEXTILE_RAW)
        {
            Application::trace(DebugType::Enc, "%s: type: %s", __FUNCTION__, "raw");
            cli.recvRegionUpdatePixels(reg);
        }
        else
        {
            if(flag & RFB::HEXTILE_BACKGROUND)
            {
                bgColor = cli.recvPixel();
                Application::trace(DebugType::Enc, "%s: type: %s, pixel: 0x%08x", __FUNCTION__, "background", bgColor);
            }

            cli.fillPixel(reg, bgColor);

            if(flag & HEXTILE_FOREGROUND)
            {
                fgColor = cli.recvPixel();
                flag &= ~HEXTILE_COLOURED;

                Application::trace(DebugType::Enc, "%s: type: %s, pixel: 0x%08x", __FUNCTION__, "foreground", fgColor);
            }

            if(flag & HEXTILE_SUBRECTS)
            {
                int subRects = cli.recvInt8();
                XCB::Region dst;

                Application::trace(DebugType::Enc, "%s: type: %s, count: %d", __FUNCTION__, "subrects", subRects);

                while(0 < subRects--)
                {
                    auto pixel = fgColor;

                    if(flag & HEXTILE_COLOURED)
                    {
                        pixel = cli.recvPixel();

                        Application::trace(DebugType::Enc, "%s: type: %s, pixel: 0x%08x", __FUNCTION__, "colored", pixel);
                    }

                    auto val1 = cli.recvInt8();
                    auto val2 = cli.recvInt8();
                    dst.x = (0x0F & (val1 >> 4));
                    dst.y = (0x0F & val1);
                    dst.width = 1 + (0x0F & (val2 >> 4));
                    dst.height = 1 + (0x0F & val2);

                    Application::trace(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], pixel: 0x%08x",
                                           __FUNCTION__, "subrects", dst.x, dst.y, dst.width, dst.height, pixel);

                    dst.x += reg.x;
                    dst.y += reg.y;

                    if(dst.x + dst.width > reg.x + reg.width || dst.y + dst.height > reg.y + reg.height)
                    {
                        Application::error("%s: %s", __FUNCTION__, "sub region out of range");
                        throw rfb_error(NS_FuncName);
                    }

                    cli.fillPixel(dst, pixel);
                }
            }
        }
    }

    RFB::DecodingTRLE::DecodingTRLE(bool zip) : DecodingBase(zip ? ENCODING_ZRLE : ENCODING_TRLE)
    {
        if(zip)
        {
            zlib = std::make_unique<ZLib::InflateStream>();
        }
    }

    void RFB::DecodingTRLE::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        Application::debug(DebugType::Enc, "%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);

        const XCB::Size bsz(64, 64);

        if(isZRLE())
        {
            cli.recvZlibData(zlib.get(), false);
            DecoderWrapper wrap(zlib.get(), & cli);

            for(const auto & reg0 : reg.XCB::Region::divideBlocks(bsz))
            {
                updateSubRegion(wrap, reg0);
            }
        }
        else
        {
            for(const auto & reg0 : reg.XCB::Region::divideBlocks(bsz))
            {
                updateSubRegion(cli, reg0);
            }
        }
    }

    void RFB::DecodingTRLE::updateSubRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        auto type = cli.recvInt8();

        Application::trace(DebugType::Enc, "%s: sub encoding type: 0x%02" PRIx8 ", sub region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16
                               "], zrle: %d",
                               __FUNCTION__, type, reg.x, reg.y, reg.width, reg.height, (int) isZRLE());

        // trle raw
        if(0 == type)
        {
            Application::trace(DebugType::Enc, "%s: type: %s", __FUNCTION__, "raw");

            for(auto coord = XCB::PointIterator(0, 0, reg.toSize()); coord.isValid(); ++coord)
            {
                auto pixel = cli.recvCPixel();
                cli.setPixel(reg.topLeft() + coord, pixel);
            }

            Application::trace(DebugType::Enc, "%s: complete: %s", __FUNCTION__, "raw");
        }
        else

            // trle solid
            if(1 == type)
            {
                auto solid = cli.recvCPixel();
                cli.fillPixel(reg, solid);

                Application::trace(DebugType::Enc, "%s: type: %s, pixel: 0x%08x", __FUNCTION__, "solid", solid);
            }
            else if(2 <= type && type <= 16)
            {
                size_t field = 1;

                if(4 < type)
                {
                    field = 4;
                }
                else if(2 < type)
                {
                    field = 2;
                }

                size_t bits = field * reg.width;
                size_t rowsz = bits >> 3;

                if((rowsz << 3) < bits)
                {
                    rowsz++;
                }

                // recv palette
                std::vector<int> palette(type);

                for(auto & val : palette)
                {
                    val = cli.recvCPixel();
                }

                Application::trace(DebugType::Enc, "%s: type: %s, size: %lu", __FUNCTION__, "packed palette", palette.size());

                // recv packed rows
                for(int oy = 0; oy < reg.height; ++oy)
                {
                    Tools::StreamBitsUnpack sb(cli.recvData(rowsz), reg.width, field);

                    for(int ox = reg.width - 1; 0 <= ox; --ox)
                    {
                        auto pos = reg.topLeft() + XCB::Point(ox, oy);
                        auto index = sb.popValue(field);

                        Application::trace(DebugType::Enc, "%s: type: %s, pos: [%" PRId16 ", %" PRId16 "], index: %d", __FUNCTION__, "packed palette", pos.x,
                                               pos.y, index);

                        if(index >= palette.size())
                        {
                            Application::error("%s: %s", __FUNCTION__, "index out of range");
                            throw rfb_error(NS_FuncName);
                        }

                        cli.setPixel(pos, palette[index]);
                    }
                }

                Application::trace(DebugType::Enc, "%s: complete: %s", __FUNCTION__, "packed palette");
            }
            else if((17 <= type && type <= 127) || type == 129)
            {
                Application::error("%s: %s", __FUNCTION__, "invalid trle type");
                throw rfb_error(NS_FuncName);
            }
            else if(128 == type)
            {
                Application::trace(DebugType::Enc, "%s: type: %s", __FUNCTION__, "plain rle");

                auto coord = XCB::PointIterator(0, 0, reg.toSize());

                while(coord.isValid())
                {
                    auto pixel = cli.recvCPixel();
                    auto runLength = cli.recvRunLength();

                    Application::trace(DebugType::Enc, "%s: type: %s, pixel: 0x%08x, length: %lu", __FUNCTION__, "plain rle", pixel, runLength);

                    while(runLength--)
                    {
                        cli.setPixel(reg.topLeft() + coord, pixel);
                        ++coord;

                        if(! coord.isValid() && runLength)
                        {
                            Application::error("%s: %s", __FUNCTION__, "plain rle: coord out of range");
                            throw rfb_error(NS_FuncName);
                        }
                    }
                }

                Application::trace(DebugType::Enc, "%s: complete: %s", __FUNCTION__, "plain rle");
            }
            else if(130 <= type)
            {
                size_t palsz = type - 128;
                std::vector<int> palette(palsz);

                for(auto & val : palette)
                {
                    val = cli.recvCPixel();
                }

                Application::trace(DebugType::Enc, "%s: type: %s, size: %lu", __FUNCTION__, "rle palette", palsz);

                auto coord = XCB::PointIterator(0, 0, reg.toSize());

                while(coord.isValid())
                {
                    auto index = cli.recvInt8();

                    if(index < 128)
                    {
                        if(index >= palette.size())
                        {
                            Application::error("%s: %s", __FUNCTION__, "index out of range");
                            throw rfb_error(NS_FuncName);
                        }

                        auto pixel = palette[index];
                        cli.setPixel(reg.topLeft() + coord, pixel);
                        ++coord;
                    }
                    else
                    {
                        index -= 128;

                        if(index >= palette.size())
                        {
                            Application::error("%s: %s", __FUNCTION__, "index out of range");
                            throw rfb_error(NS_FuncName);
                        }

                        auto pixel = palette[index];
                        auto runLength = cli.recvRunLength();

                        Application::trace(DebugType::Enc, "%s: type: %s, index: %" PRIu8 ", length: %lu", __FUNCTION__, "rle palette", index, runLength);

                        while(runLength--)
                        {
                            cli.setPixel(reg.topLeft() + coord, pixel);
                            ++coord;

                            if(! coord.isValid() && runLength)
                            {
                                Application::error("%s: %s", __FUNCTION__, "rle palette: coord out of range");
                                throw rfb_error(NS_FuncName);
                            }
                        }
                    }
                }

                Application::trace(DebugType::Enc, "%s: complete: %s", __FUNCTION__, "rle palette");
            }
    }

    RFB::DecodingZlib::DecodingZlib() : DecodingBase(ENCODING_ZLIB)
    {
        zlib = std::make_unique<ZLib::InflateStream>();
    }

    void RFB::DecodingZlib::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        Application::debug(DebugType::Enc, "%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);

        cli.recvZlibData(zlib.get(), false);
        //DecoderWrapper wrap(zlib.get(), & cli)

        uint32_t pitch = reg.width * cli.clientFormat().bytePerPixel();
        auto pixels = zlib->recvData(static_cast<size_t>(pitch) * reg.height);
        cli.updateRawPixels(reg, pixels.data(), pitch, cli.serverFormat());
    }

#ifdef LTSM_DECODING

#ifdef LTSM_DECODING_LZ4
    /// DecodingLZ4
    void RFB::DecodingLZ4::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        Application::debug(DebugType::Enc, "%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);

        auto lz4sz = cli.recvIntBE32();
        auto lz4buf = cli.recvData(lz4sz);

        auto pitch = cli.serverFormat().bytePerPixel() * reg.width;
        auto rawsz = pitch * reg.height;

        auto runJob = [](uint32_t rawsz, uint32_t pitch, std::vector<uint8_t> buf, XCB::Region reg, DecoderStream* cli)
        {
            // thread buf
            BinaryBuf bb(rawsz);

            int ret = LZ4_decompress_safe((const char*) buf.data(), (char*) bb.data(), buf.size(), bb.size());

            if(ret <= 0)
            {
                Application::error("%s: %s failed, ret: %d", __FUNCTION__, "LZ4_decompress_safe_continue", ret);
                throw rfb_error(NS_FuncName);
            }

            bb.resize(ret);
            cli->updateRawPixels(reg, bb.data(), pitch, cli->serverFormat());
        };

        if(1 < threads)
        {
            jobs.emplace_back(runJob, rawsz, pitch, std::move(lz4buf), reg, & cli);
        }
        else
        {
            runJob(rawsz, pitch, std::move(lz4buf), reg, & cli);
        }
    }

    void RFB::DecodingLZ4::waitUpdateComplete(void)
    {
        for(auto & job: jobs)
        {
            if(job.joinable())
            {
                job.join();
            }
        }

        jobs.clear();
    }
#endif // LTSM_DECODING_LZ4

#ifdef LTSM_DECODING_TJPG
    /// DecodingTJPG
    void RFB::DecodingTJPG::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        Application::debug(DebugType::Enc, "%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);

        auto jpgsz = cli.recvIntBE32();
        auto jpgbuf = cli.recvData(jpgsz);

        auto runJob = [](std::vector<uint8_t> buf, XCB::Region reg, DecoderStream* cli)
        {
            std::unique_ptr<void, int(*)(void*)> jpeg{ tjInitDecompress(), tjDestroy };

            if(jpeg)
            {
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
                const int pixfmt = TJPF_RGBX;
#else
                const int pixfmt = TJPF_BGRX;
#endif
                auto pitch = reg.width * tjPixelSize[pixfmt];

                // thread buf
                auto jpegData = tjAlloc(pitch* reg.height);

                if(0 > tjDecompress2(jpeg.get(), buf.data(), buf.size(),
                                     jpegData, reg.width, 0, reg.height, TJPF_BGRX, TJFLAG_FASTDCT))
                {
#ifdef tjGetErrorCode
                    int err = tjGetErrorCode(jpeg.get());
                    const char* str = tjGetErrorStr2(jpeg.get());
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "tjDecompress", str, err);
#else
                    Application::error("%s: %s failed, error: `%s'", __FUNCTION__, "tjDecompress", tjGetErrorStr());
#endif
                }
                else
                {
#ifndef SDL_PIXELFORMAT_XBGR8888
 #define SDL_PIXELFORMAT_XBGR8888 SDL_PIXELFORMAT_ABGR8888
#endif

#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
                    cli->updateRawPixels2(reg, jpegData, 32, pitch, SDL_PIXELFORMAT_RGBX8888);
#else
		    // deb10, turbojpeg-1.5.2
                    cli->updateRawPixels2(reg, jpegData, 32, pitch, SDL_PIXELFORMAT_XBGR8888);
#endif
                    tjFree(jpegData);
                }
            }
            else
            {
                Application::error("%s: %s failed", __FUNCTION__, "tjInitDecompress");
            }
        };

        if(1 < threads)
        {
            jobs.emplace_back(runJob, std::move(jpgbuf), reg, & cli);
        }
        else
        {
            runJob(std::move(jpgbuf), reg, & cli);
        }
    }

    void RFB::DecodingTJPG::waitUpdateComplete(void)
    {
        for(auto & job: jobs)
        {
            if(job.joinable())
            {
                job.join();
            }
        }

        jobs.clear();
    }
#endif // LTSM_DECODING_TJPG

#ifdef LTSM_DECODING_QOI
    /// DecodingQOI
    void RFB::DecodingQOI::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        Application::debug(DebugType::Enc, "%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);

        auto len = cli.recvIntBE32();
        auto buf = cli.recvData(len);

        auto pitch = cli.serverFormat().bytePerPixel() * reg.width;
        auto rawsz = pitch * reg.height;

        auto runJob = [this](uint32_t rawsz, uint32_t pitch, std::vector<uint8_t> buf, XCB::Region reg, DecoderStream* cli)
        {
            auto bb = this->decodeBGRx(buf, reg.toSize(), cli->serverFormat(), pitch);
            assertm(bb.size() == static_cast<size_t>(pitch) * reg.height, "invalid pitch");

            cli->updateRawPixels(reg, bb.data(), pitch, cli->serverFormat());
        };

        if(1 < threads)
        {
            jobs.emplace_back(runJob, rawsz, pitch, std::move(buf), reg, & cli);
        }
        else
        {
            runJob(rawsz, pitch, std::move(buf), reg, & cli);
        }
    }

    void RFB::DecodingQOI::waitUpdateComplete(void)
    {
        for(auto & job: jobs)
        {
            if(job.joinable())
            {
                job.join();
            }
        }

        jobs.clear();
    }

    namespace QOI
    {
        enum Tag
        {
            INDEX = 0x00,
            DIFF = 0x40,
            LUMA = 0x80,
            RUN = 0xC0,
            RGB = 0xFE,
            RGBA = 0xFF,
            MASK2 = 0xC0
        };

        inline uint32_t packBGRx(const Color & col, const PixelFormat & pf)
        {
            return (static_cast<uint32_t>(col.b) << pf.bshift()) |
                    (static_cast<uint32_t>(col.g) << pf.gshift()) | (static_cast<uint32_t>(col.r) << pf.rshift());
        }

        inline Color unpackBGRx(uint32_t pixel, const PixelFormat & pf)
        {
            Color col;
            col.r = (pixel & pf.rmask()) >> pf.rshift();
            col.g = (pixel & pf.gmask()) >> pf.gshift();
            col.b = (pixel & pf.bmask()) >> pf.bshift();
            return col;
        }

        inline uint8_t hashIndex64RGB(const Color & col)
        {
            return (col.r * 3 + col.g * 5 + col.b * 7) % 64;
        }
    }

    BinaryBuf RFB::DecodingQOI::decodeBGRx(const std::vector<uint8_t> & buf, const XCB::Size & rsz, const PixelFormat & clientPf, uint32_t pitch) const
    {
        std::array<int64_t, 64> hashes;
        hashes.fill(-1);

        int64_t prevPixel = -1;
        std::uint8_t run = 0;

        StreamBufRef sb(buf.data(), buf.size());
        BinaryBuf res(static_cast<size_t>(pitch) * rsz.height, 0);
        FrameBuffer fb(res.data(), XCB::Region{0,0,rsz.width,rsz.height}, clientPf, pitch);

        for(int16_t py = 0; py < rsz.height; ++py)
        {
            for(int16_t px = 0; px < rsz.width; ++px)
            {
                if(run)
                {
                    run--;
                    fb.setPixel(XCB::Point{px,py}, prevPixel);
                    continue;
                }

                if(0 == sb.last())
                {
                    Application::error("%s: %s", __FUNCTION__, "unknown format");
                    throw rfb_error(NS_FuncName);
                }

                auto type = sb.readInt8();

                if(type == QOI::Tag::RGB)
                {
                    Color col;
                    col.r = sb.readInt8();
                    col.g = sb.readInt8();
                    col.b = sb.readInt8();

                    prevPixel = QOI::packBGRx(col, clientPf);
                    fb.setPixel(XCB::Point{px,py}, prevPixel);
                    hashes[QOI::hashIndex64RGB(col)] = prevPixel;
                    continue;
                }

                if((type & QOI::Tag::MASK2) == QOI::Tag::INDEX)
                {
                    if(hashes.size() <= type)
                    {
                        Application::error("%s: %s", __FUNCTION__, "unknown index");
                        throw rfb_error(NS_FuncName);
                    }

                    if(0 > hashes[type])
                    {
                        Application::error("%s: %s", __FUNCTION__, "unknown type");
                        throw rfb_error(NS_FuncName);
                    }

                    prevPixel = hashes[type];
                    fb.setPixel(XCB::Point{px,py}, prevPixel);
                    continue;
                }

                if((type & QOI::Tag::MASK2) == QOI::Tag::DIFF)
                {
                    auto col = QOI::unpackBGRx(prevPixel, clientPf);

                    col.r += ((type >> 4) & 0x03) - 2;
                    col.g += ((type >> 2) & 0x03) - 2;
                    col.b += ( type & 0x03) - 2;

                    prevPixel = QOI::packBGRx(col, clientPf);
                    fb.setPixel(XCB::Point{px,py}, prevPixel);
                    hashes[QOI::hashIndex64RGB(col)] = prevPixel;
                    continue;
                }

                if((type & QOI::Tag::MASK2) == QOI::Tag::LUMA)
                {
                    auto lm = sb.readInt8();
                    int8_t vg = (type & 0x3f) - 32;

                    auto col = QOI::unpackBGRx(prevPixel, clientPf);

                    col.r += vg - 8 + ((lm >> 4) & 0x0f);
                    col.g += vg;
                    col.b += vg - 8 + (lm & 0x0f);

                    prevPixel = QOI::packBGRx(col, clientPf);
                    fb.setPixel(XCB::Point{px,py}, prevPixel);
                    hashes[QOI::hashIndex64RGB(col)] = prevPixel;
                    continue;
                }

                if((type & QOI::Tag::MASK2) == QOI::Tag::RUN)
                {
                    run = type & 0x3f;
                    fb.setPixel(XCB::Point{px,py}, prevPixel);
                    continue;
                }
                else
                {
                    Application::error("%s: %s", __FUNCTION__, "unknown tag");
                    throw rfb_error(NS_FuncName);
                }
            }
        }

        return res;
    }
#endif // LTSM_DECODING_QOI
#endif // LTSM_DECODING
}
