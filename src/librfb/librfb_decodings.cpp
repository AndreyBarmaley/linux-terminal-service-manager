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

            case 1: return recvInt8();
            default: break;
        }

        Application::error("%s: %s", __FUNCTION__, "unknown format");
        throw rfb_error(NS_FuncName);
    }

    int RFB::DecoderStream::recvCPixel(void)
    {   
        if(clientFormat().bitsPerPixel == 32)
        {
            auto colr = recvInt8();
            auto colg = recvInt8();
            auto colb = recvInt8();
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
            std::swap(colr, colb);
#endif
            return clientFormat().pixel(Color(colr, colg, colb));
        }

        return recvPixel();
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
            zipsz = recvIntBE16();
        else
            zipsz = recvIntBE32();

        auto zip = recvData(zipsz);

        if(Application::isDebugLevel(DebugLevel::Trace))
            Application::debug("%s: compress data length: %u", __FUNCTION__, zip.size());

        zlib->appendData(zip);
	return zipsz;
    }

    //  DecodingBase
    RFB::DecodingBase::DecodingBase(int v) : type(v)
    {
        Application::info("%s: init decoding: %s", __FUNCTION__, encodingName(type));
    }

    int RFB::DecodingBase::getType(void) const
    {   
        return type;
    }
        
    void RFB::DecodingBase::setDebug(int v)
    {   
        debug = v;
    }

    void RFB::DecodingRaw::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        if(debug)
            Application::debug("%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
            cli.setPixel(reg.topLeft() + coord, cli.recvPixel());
    }

    void RFB::DecodingRRE::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        if(debug)
            Application::debug("%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        auto subRects = cli.recvIntBE32();
        auto bgColor = cli.recvPixel();

        if(1 < debug)
            Application::debug("%s: back pixel: 0x%08x, sub rects: %" PRIu32, __FUNCTION__, bgColor, subRects);

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

            if(2 < debug)
                Application::debug("%s: sub region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, dst.x, dst.y, dst.width, dst.height);

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
            Application::error("%s: invalid hextile region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);
            throw rfb_error(NS_FuncName);
        }

        if(debug)
            Application::debug("%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        updateRegionColors(cli, reg);
    }

    void RFB::DecodingHexTile::updateRegionColors(DecoderStream & cli, const XCB::Region & reg)
    {
        auto flag = cli.recvInt8();

        if(1 < debug)
            Application::debug("%s: sub encoding mask: 0x%02" PRIx8 ", sub region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, flag, reg.x, reg.y, reg.width, reg.height);

        if(flag & RFB::HEXTILE_RAW)
        {
            if(2 < debug)
                Application::debug("%s: type: %s", __FUNCTION__, "raw");

            for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
                    cli.setPixel(reg.topLeft() + coord, cli.recvPixel());
        }
        else
        {
            if(flag & RFB::HEXTILE_BACKGROUND)
            {
                bgColor = cli.recvPixel();

                if(2 < debug)
                    Application::debug("%s: type: %s, pixel: 0x%08x", __FUNCTION__, "background", bgColor);
            }

            cli.fillPixel(reg, bgColor);

            if(flag & HEXTILE_FOREGROUND)
            {
                fgColor = cli.recvPixel();
                flag &= ~HEXTILE_COLOURED;

                if(2 < debug)
                    Application::debug("%s: type: %s, pixel: 0x%08x", __FUNCTION__, "foreground", fgColor);
            }

            if(flag & HEXTILE_SUBRECTS)
            {
                int subRects = cli.recvInt8();
                XCB::Region dst;

                if(2 < debug)
                    Application::debug("%s: type: %s, count: %d", __FUNCTION__, "subrects", subRects);

                while(0 < subRects--)
                {
                    auto pixel = fgColor;
                    if(flag & HEXTILE_COLOURED)
                    {
                        pixel = cli.recvPixel();
                        if(3 < debug)
                            Application::debug("%s: type: %s, pixel: 0x%08x", __FUNCTION__, "colored", pixel);
                    }

                    auto val1 = cli.recvInt8();
                    auto val2 = cli.recvInt8();

                    dst.x = (0x0F & (val1 >> 4));
                    dst.y = (0x0F & val1);
                    dst.width = 1 + (0x0F & (val2 >> 4));
                    dst.height = 1 + (0x0F & val2);

                    if(3 < debug)
                        Application::debug("%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], pixel: 0x%08x", __FUNCTION__, "subrects", dst.x, dst.y, dst.width, dst.height, pixel);

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
	    zlib.reset(new ZLib::InflateStream());
    }

    void RFB::DecodingTRLE::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        if(debug)
            Application::debug("%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        const XCB::Size bsz(64, 64);

        if(isZRLE())
	{
	    cli.recvZlibData(zlib.get(), false);
	    DecoderWrapper wrap(zlib.get(), & cli);

    	    for(auto & reg0: reg.XCB::Region::divideBlocks(bsz))
    		updateSubRegion(wrap, reg0);
	}
	else
	{
    	    for(auto & reg0: reg.XCB::Region::divideBlocks(bsz))
    		updateSubRegion(cli, reg0);
	}
    }

    void RFB::DecodingTRLE::updateSubRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        auto type = cli.recvInt8();

        if(1 < debug)
            Application::debug("%s: sub encoding type: 0x%02" PRIx8 ", sub region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], zrle: %d",
                        __FUNCTION__, type, reg.x, reg.y, reg.width, reg.height, (int) isZRLE());

        // trle raw
        if(0 == type)
        {
            if(2 < debug)
                Application::debug("%s: type: %s", __FUNCTION__, "raw");

            for(auto coord = XCB::PointIterator(0, 0, reg.toSize()); coord.isValid(); ++coord)
            {
                auto pixel = cli.recvCPixel();
                cli.setPixel(reg.topLeft() + coord, pixel);
            }

            if(3 < debug)
                Application::debug("%s: complete: %s", __FUNCTION__, "raw");
        }
        else
        // trle solid
        if(1 == type)
        {
            auto solid = cli.recvCPixel();

            if(2 < debug)
                Application::debug("%s: type: %s, pixel: 0x%08x", __FUNCTION__, "solid", solid);

            cli.fillPixel(reg, solid);

            if(3 < debug)
                Application::debug("%s: complete: %s", __FUNCTION__, "solid");
        }
        else
        if(2 <= type && type <= 16)
        {
            size_t field = 1;

            if(4 < type)
                field = 4;
            else
            if(2 < type)
                field = 2;

            size_t bits = field * reg.width;
            size_t rowsz = bits >> 3;
            if((rowsz << 3) < bits) rowsz++;

            //  recv palette
            std::vector<int> palette(type);
            for(auto & val : palette) val = cli.recvCPixel();

            if(2 < debug)
                Application::debug("%s: type: %s, size: %u", __FUNCTION__, "packed palette", palette.size());

            if(3 < debug)
            {
                std::string str = Tools::buffer2hexstring<int>(palette.data(), palette.size(), 8);
                Application::debug("%s: type: %s, palette: %s", __FUNCTION__, "packed palette", str.c_str());
            }

            // recv packed rows
            for(int oy = 0; oy < reg.height; ++oy)
            {
                Tools::StreamBitsUnpack sb(cli.recvData(rowsz), reg.width, field);

                for(int ox = reg.width - 1; 0 <= ox; --ox)
                {
                    auto pos = reg.topLeft() + XCB::Point(ox, oy);
                    auto index = sb.popValue(field);

                    if(4 < debug)
                        Application::debug("%s: type: %s, pos: [%" PRId16 ", %" PRId16 "], index: %d", __FUNCTION__, "packed palette", pos.x, pos.y, index);

                    if(index >= palette.size())
                    {
                        Application::error("%s: %s", __FUNCTION__, "index out of range");
                        throw rfb_error(NS_FuncName);
                    }

                    cli.setPixel(pos, palette[index]);
                }
            }

            if(3 < debug)
                Application::debug("%s: complete: %s", __FUNCTION__, "packed palette");
        }
        else
        if((17 <= type && type <= 127) || type == 129)
        {
            Application::error("%s: %s", __FUNCTION__, "invalid trle type");
            throw rfb_error(NS_FuncName);
        }
        else
        if(128 == type)
        {
            if(2 < debug)
                Application::debug("%s: type: %s", __FUNCTION__, "plain rle");

            auto coord = XCB::PointIterator(0, 0, reg.toSize());

            while(coord.isValid())
            {
                auto pixel = cli.recvCPixel();
                auto runLength = cli.recvRunLength();

                if(4 < debug)
                    Application::debug("%s: type: %s, pixel: 0x%08x, length: %u", __FUNCTION__, "plain rle", pixel, runLength);

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

            if(3 < debug)
                Application::debug("%s: complete: %s", __FUNCTION__, "plain rle");
        }
        else
        if(130 <= type)
        {
            size_t palsz = type - 128;
            std::vector<int> palette(palsz);
            
            for(auto & val: palette)
                val = cli.recvCPixel();

            if(2 < debug)
                Application::debug("%s: type: %s, size: %u", __FUNCTION__, "rle palette", palsz);

            if(3 < debug)
            {
                std::string str = Tools::buffer2hexstring<int>(palette.data(), palette.size(), 8);
                Application::debug("%s: type: %s, palette: %s", __FUNCTION__, "rle palette", str.c_str());
            }

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

                    if(4 < debug)
                        Application::debug("%s: type: %s, index: %" PRIu8 ", length: %u", __FUNCTION__, "rle palette", index, runLength);

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

            if(3 < debug)
                Application::debug("%s: complete: %s", __FUNCTION__, "rle palette");
        }
    }

    RFB::DecodingZlib::DecodingZlib() : DecodingBase(ENCODING_ZLIB)
    {
        zlib.reset(new ZLib::InflateStream());
    }

    void RFB::DecodingZlib::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        if(debug)
            Application::debug("%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

	cli.recvZlibData(zlib.get(), false);
	DecoderWrapper wrap(zlib.get(), & cli);

        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
                wrap.setPixel(reg.topLeft() + coord, wrap.recvPixel());
    }
}

