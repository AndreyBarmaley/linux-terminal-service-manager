/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#include <cctype>
#include <algorithm>
#include <exception>

#include "ltsm_tools.h"
#include "ltsm_font_psf.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"

namespace LTSM
{
    PixelFormat::PixelFormat(int bpp, int rmask, int gmask, int bmask, int amask) : bitsPerPixel(bpp)
    {
        redMax = Tools::maskMaxValue(rmask);
        greenMax = Tools::maskMaxValue(gmask);
        blueMax = Tools::maskMaxValue(bmask);
        alphaMax = Tools::maskMaxValue(amask);
        redShift = Tools::maskShifted(rmask);
        greenShift = Tools::maskShifted(gmask);
        blueShift = Tools::maskShifted(bmask);
        alphaShift = Tools::maskShifted(amask);
    }

    PixelFormat::PixelFormat(int bpp, int rmax, int gmax, int bmax, int amax, int rshift, int gshift, int bshift, int ashift)
        : redMax(rmax), greenMax(gmax), blueMax(bmax), alphaMax(amax), redShift(rshift), greenShift(gshift), blueShift(bshift), alphaShift(ashift), bitsPerPixel(bpp)
    {
    }

    uint32_t PixelFormat::rmask(void) const
    {
        return static_cast<uint32_t>(redMax) << redShift;
    }
    
    uint32_t PixelFormat::gmask(void) const
    {
        return static_cast<uint32_t>(greenMax) << greenShift;
    }
    
    uint32_t PixelFormat::bmask(void) const
    {
        return static_cast<uint32_t>(blueMax) << blueShift;
    }
               
    uint32_t PixelFormat::amask(void) const
    {   
        return static_cast<uint32_t>(alphaMax) << alphaShift;
    }

    uint32_t PixelFormat::red(int pixel) const
    {
        return (pixel >> redShift) & redMax;
    }
                
    uint32_t PixelFormat::green(int pixel) const
    {
        return (pixel >> greenShift) & greenMax;
    }
        
    uint32_t PixelFormat::blue(int pixel) const
    {
        return (pixel >> blueShift) & blueMax;
    }
        
    uint32_t PixelFormat::alpha(int pixel) const
    {
        return (pixel >> alphaShift) & alphaMax;
    }

    uint32_t PixelFormat::bytePerPixel(void) const
    {
        return bitsPerPixel >> 3;
    }

    Color PixelFormat::color(int pixel) const
    {
        return Color(red(pixel), green(pixel), blue(pixel), alpha(pixel));
    }

    uint32_t PixelFormat::pixel(const Color & col) const
    {
        return ((static_cast<int>(col.x) * alphaMax / 0xFF) << alphaShift) | ((static_cast<int>(col.r) * redMax / 0xFF) << redShift) |
               ((static_cast<int>(col.g) * greenMax / 0xFF) << greenShift) | ((static_cast<int>(col.b) * blueMax / 0xFF) << blueShift);
    }

    uint32_t PixelFormat::convertFrom(const PixelFormat & pf, uint32_t pixel) const
    {
        if(pf != *this)
        {
            uint32_t r = pf.redMax ? (pf.red(pixel) * redMax) / pf.redMax : 0;
            uint32_t g = pf.greenMax ? (pf.green(pixel) * greenMax) / pf.greenMax : 0;
            uint32_t b = pf.blueMax ? (pf.blue(pixel) * blueMax) / pf.blueMax : 0;
            uint32_t a = pf.alphaMax ? (pf.alpha(pixel) * alphaMax) / pf.alphaMax : 0;
            return (a << alphaShift) | (r << redShift) | (g << greenShift) | (b << blueShift);
        }
        
        return pixel;
    }

    fbinfo_t::fbinfo_t(const XCB::Size & fbsz, const PixelFormat & fmt, uint32_t pitch2) : format(fmt)
    {
        uint32_t pitch1 = fmt.bytePerPixel() * fbsz.width;
        pitch = std::max(pitch1, pitch2);
        size_t length = pitch * fbsz.height;
        buffer = new uint8_t[length];
        std::fill(buffer, buffer + length, 0);
        allocated = 1;
    }

    fbinfo_t::fbinfo_t(uint8_t* ptr, const XCB::Size & fbsz, const PixelFormat & fmt, uint32_t pitch2) : format(fmt), buffer(ptr)
    {
        uint32_t pitch1 = fmt.bytePerPixel() * fbsz.width;
        pitch = std::max(pitch1, pitch2);
    }

    fbinfo_t::~fbinfo_t()
    {
        if(allocated)
            delete [] buffer;
    }

    int PixelMapWeight::maxWeightPixel(void) const
    {
        auto it = std::max_element(begin(), end(), [](auto & p1, auto & p2)
        {
            return p1.second < p2.second;
        });
        return it != end() ? (*it).first : 0;
    }

    FrameBuffer FrameBuffer::copyRegion(const XCB::Region* reg) const
    {
        FrameBuffer res;
        res.fbreg = reg ? XCB::Region(0, 0, reg->width, reg->height) : XCB::Region(0, 0, fbreg.width, fbreg.height);
        res.fbptr = std::make_shared<fbinfo_t>(res.fbreg.toSize(), pixelFormat());
        res.owner = true;
        res.blitRegion(*this, reg ? *reg : res.fbreg, XCB::Point(0, 0));
        return res;
    }

    void FrameBuffer::setPixelRow(const XCB::Point & pos, uint32_t pixel, size_t length)
    {
        if(pos.isValid() && pos.x < fbreg.width && pos.y < fbreg.height)
        {
            void* offset = pitchData(pos.y) + (pos.x * bytePerPixel());
            // fix out of range
            length = std::min(length, static_cast<size_t>(fbreg.width - pos.x));
            auto bpp = bitsPerPixel();

            if(length > static_cast<size_t>(fbreg.width) - pos.x)
            {
                Application::error("%s: out of range, x: %d, width: %d, length: %d", __FUNCTION__, pos.x, fbreg.width, length);
                throw std::out_of_range("FrameBuffer::setPixelRow");
            }

            switch(bpp)
            {
                case 32:
                    if(auto ptr = static_cast<uint32_t*>(offset))
                        std::fill(ptr, ptr + length, pixel);

                    break;

                case 24:
                    if(auto ptr = static_cast<uint8_t*>(offset))
                    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                        uint8_t v1 = pixel;
                        uint8_t v2 = pixel >> 8;
                        uint8_t v3 = pixel >> 16;
#else
                        uint8_t v1 = pixel >> 16;
                        uint8_t v2 = pixel >> 8;
                        uint8_t v3 = pixel;
#endif

                        while(length--)
                        {
                            *ptr++ = v1;
                            *ptr++ = v2;
                            *ptr++ = v3;
                        }
                    }

                    break;

                case 16:
                    if(auto ptr = static_cast<uint16_t*>(offset))
                        std::fill(ptr, ptr + length, static_cast<uint16_t>(pixel));

                    break;

                case 8:
                    if(auto ptr = static_cast<uint8_t*>(offset))
                        std::fill(ptr, ptr + length, static_cast<uint8_t>(pixel));

                    break;

                default:
                    Application::error("%s: unknown bpp: %d", __FUNCTION__, bpp);
                    throw std::invalid_argument("FrameBuffer::setPixelRow");
            }
        }
        else
        {
            Application::error("%s: out of range, x: %d, y: %d, width: %d, height: %d", __FUNCTION__, pos.x, pos.y, fbreg.width, fbreg.height);
            throw std::out_of_range("FrameBuffer::setPixelRow");
         }
    }

    void FrameBuffer::setPixel(const XCB::Point & pos, uint32_t pixel, const PixelFormat* fmt)
    {
        auto raw = fmt ? pixelFormat().convertFrom(*fmt, pixel) : pixel;
        setPixelRow(pos, raw, 1);
    }

    void FrameBuffer::fillPixel(const XCB::Region & reg0, uint32_t pixel, const PixelFormat* fmt)
    {
        XCB::Region reg;

        if(XCB::Region::intersection(region(), reg0, & reg))
        {
            auto raw = fmt ? pixelFormat().convertFrom(*fmt, pixel) : pixel;

            for(int yy = 0; yy < reg.height; ++yy)
                setPixelRow(reg.topLeft() + XCB::Point(0, yy), raw, reg.width);
        }
    }

    void FrameBuffer::setColor(const XCB::Point & pos, const Color & col)
    {
        auto raw = pixelFormat().pixel(col);
        setPixelRow(pos, raw, 1);
    }

    void FrameBuffer::fillColor(const XCB::Region & reg0, const Color & col)
    {
        XCB::Region reg;

        if(XCB::Region::intersection(region(), reg0, & reg))
        {
            auto raw = pixelFormat().pixel(col);

            for(int yy = 0; yy < reg.height; ++yy)
                setPixelRow(reg.topLeft() + XCB::Point(0, yy), raw, reg.width);
        }
    }

    void FrameBuffer::drawRect(const XCB::Region & reg0, const Color & col)
    {
        XCB::Region reg;

        if(XCB::Region::intersection(region(), reg0, & reg))
        {
            auto raw = pixelFormat().pixel(col);
            setPixelRow(reg.topLeft(), raw, reg.width);
            setPixelRow(reg.topLeft() + XCB::Point(0, reg.height - 1), raw, reg.width);

            for(int yy = 1; yy < reg.height - 1; ++yy)
            {
                setPixelRow(reg.topLeft() + XCB::Point(0, yy), raw, 1);
                setPixelRow(reg.topLeft() + XCB::Point(reg.width - 1, yy), raw, 1);
            }
        }
    }

    uint32_t FrameBuffer::pixel(const XCB::Point & pos) const
    {
        if(pos.isValid() && pos.x < fbreg.width && pos.y < fbreg.height)
        {
            void* ptr = pitchData(pos.y) + (pos.x * bytePerPixel());
            auto bpp = bitsPerPixel();

            switch(bpp)
            {
                case 32:
                    return *static_cast<uint32_t*>(ptr);

                case 24:
                {
                    auto buf = static_cast<uint8_t*>(ptr);
                    uint32_t res = 0;

                    if(big_endian)
                    {
                        res |= buf[0];
                        res <<= 8;
                        res |= buf[1];
                        res <<= 8;
                        res |= buf[2];
                    }
                    else
                    {
                        res |= buf[2];
                        res <<= 8;
                        res |= buf[1];
                        res <<= 8;
                        res |= buf[0];
                    }

                    return res;
                }

                case 16:
                    return *static_cast<uint16_t*>(ptr);

                case 8:
                    return *static_cast<uint8_t*>(ptr);

                default:
                    Application::error("%s: unknown bpp: %d", __FUNCTION__, bpp);
                    throw std::invalid_argument("FrameBuffer::pixel");
            }
        }

        Application::error("%s: out of range, x: %d, y: %d, width: %d, height: %d", __FUNCTION__, pos.x, pos.y, fbreg.width, fbreg.height);
        throw std::out_of_range("FrameBuffer::pixel");
    }

    std::list<PixelLength> FrameBuffer::toRLE(const XCB::Region & reg) const
    {
        std::list<PixelLength> res;

        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
        {
            auto pix = pixel(reg.topLeft() + coord);

            if(0 < coord.x && res.back().pixel() == pix)
                res.back().second++;
            else
                res.emplace_back(pix, 1);
        }

        return res;
    }

    void FrameBuffer::blitRegion(const FrameBuffer & fb, const XCB::Region & reg, const XCB::Point & pos)
    {
        auto dst = XCB::Region(pos, reg.toSize()).intersected(region());

        if(pixelFormat() != fb.pixelFormat())
        {
            for(auto coord = dst.coordBegin(); coord.isValid(); ++coord)
                setPixel(dst + coord, fb.pixel(reg.topLeft() + coord), & fb.pixelFormat());
        }
        else
        {
            for(int row = 0; row < dst.height; ++row)
            {
                auto ptr = fb.pitchData(reg.y + row) + reg.x * fb.bytePerPixel();
                size_t length = dst.width * fb.bytePerPixel();
                std::copy(ptr, ptr + length, pitchData(dst.y + row) + dst.x * bytePerPixel());
            }
        }
    }

    ColorMap FrameBuffer::colourMap(void) const
    {
        ColorMap map;
        const PixelFormat & fmt = pixelFormat();

        for(auto coord = coordBegin(); coord.isValid(); ++coord)
        {
            auto pix = pixel(coord);
            map.emplace(fmt.red(pix), fmt.green(pix), fmt.blue(pix));
        }

        return map;
    }

    PixelMapWeight FrameBuffer::pixelMapWeight(const XCB::Region & reg) const
    {
        PixelMapWeight map;

        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
        {
            auto pix = pixel(reg.topLeft() + coord);
            auto it = map.find(pix);

            if(it != map.end())
                (*it).second += 1;
            else
                map.emplace(pix, 1);
        }

        return map;
    }

    bool FrameBuffer::allOfPixel(uint32_t pixel, const XCB::Region & reg) const
    {
        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
            if(pixel != this->pixel(reg.topLeft() + coord)) return false;

        return true;
    }

    bool FrameBuffer::renderChar(int ch, const Color & col, const XCB::Point & pos)
    {
        if(std::isprint(ch))
        {
            size_t offsetx = ch * _systemfont.width * _systemfont.height >> 3;

            if(offsetx >= sizeof(_systemfont.data))
                return false;

            bool res = false;

            for(int yy = 0; yy < _systemfont.height; ++yy)
            {
                if(pos.y + yy < 0) continue;

                size_t offsety = yy * _systemfont.width >> 3;

                if(offsetx + offsety >= sizeof(_systemfont.data))
                    continue;

                int line = *(_systemfont.data + offsetx + offsety);

                for(int xx = 0; xx < _systemfont.width; ++xx)
                {
                    if(pos.x + xx < 0) continue;

                    if(0x80 & (line << xx))
                    {
                        setColor(pos + XCB::Point(xx, yy), col);
                        res = true;
                    }
                }
            }

            return res;
        }

        return false;
    }

    void FrameBuffer::renderText(const std::string & str, const Color & col, const XCB::Point & pos)
    {
        int offset = 0;

        for(auto & ch : str)
        {
            renderChar(ch, col, XCB::Point(pos.x + offset, pos.y));
            offset += _systemfont.width;
        }
    }

    Color FrameBuffer::color(const XCB::Point & pos) const
    {
        return pixelFormat().color(pixel(pos));
    }

    uint32_t FrameBuffer::bitsPerPixel(void) const
    {
        return pixelFormat().bitsPerPixel;
    }

    uint32_t FrameBuffer::bytePerPixel(void) const
    {
        return pixelFormat().bytePerPixel();
    }

    size_t FrameBuffer::width(void) const
    {
        return fbreg.width;
    }

    size_t FrameBuffer::height(void) const
    {
        return fbreg.height;
    }

    uint8_t* FrameBuffer::pitchData(size_t row) const
    {
        uint32_t col = 0;

        if(! owner)
        {
            col = bytePerPixel() * fbreg.x;
            row = fbreg.y + row;
        }

        return fbptr->buffer + (fbptr->pitch * row) + col;
    }

    size_t FrameBuffer::pitchSize(void) const
    {
        return owner ? fbptr->pitch : bytePerPixel() * fbreg.width;
    }
} // LTSM

#ifdef LTSM_WITH_PNG
#include <png.h>
namespace PNG
{
    bool save(const LTSM::FrameBuffer & fb, std::string_view file)
    {
        if(fb.pixelFormat().amask() && fb.pixelFormat() != RGBA32)
        {
            LTSM::FrameBuffer back(LTSM::XCB::Size(fb.width(), fb.height()), RGBA32);
            back.blitRegion(fb, fb.region(), LTSM::XCB::Point(0, 0));
            return save(back, file);
        }
        else
        if(fb.pixelFormat() != RGB24)
        {
            LTSM::FrameBuffer back(LTSM::XCB::Size(fb.width(), fb.height()), RGB24);
            back.blitRegion(fb, fb.region(), LTSM::XCB::Point(0, 0));
            return save(back, file);
        }

        // write png
        png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png_ptr)
            return false;

        png_infop info_ptr = png_create_info_struct(png_ptr);
        if (!info_ptr)
            return false;

        setjmp(png_jmpbuf(png_ptr));
        std::unique_ptr<FILE, decltype(fclose)*> fp{fopen(file.data(), "wb"), fclose};

        png_init_io(png_ptr, fp.get());
        png_set_IHDR(png_ptr, info_ptr, fb.width(), fb.height(), 8,
                fb.pixelFormat().amask() ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
        png_write_info(png_ptr, info_ptr);

        for(size_t row = 0; row < fb.height(); row++)
            png_write_row(png_ptr, fb.pitchData(row));

        png_write_end(png_ptr, nullptr);
        png_destroy_write_struct(&png_ptr, &info_ptr);

        return true;
    }
}
#endif
