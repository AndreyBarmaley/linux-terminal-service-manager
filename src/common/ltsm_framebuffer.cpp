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
    PixelFormat::PixelFormat(int bpp, int rmask, int gmask, int bmask, int amask) : bitsPixel(bpp), bytePixel(bpp >> 3)
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
        : redMax(rmax), greenMax(gmax), blueMax(bmax), alphaMax(amax), redShift(rshift), greenShift(gshift), blueShift(bshift), alphaShift(ashift),
          bitsPixel(bpp), bytePixel(bpp >> 3)
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

    uint8_t PixelFormat::red(int pixel) const
    {
        return (pixel >> redShift) & redMax;
    }

    uint8_t PixelFormat::green(int pixel) const
    {
        return (pixel >> greenShift) & greenMax;
    }

    uint8_t PixelFormat::blue(int pixel) const
    {
        return (pixel >> blueShift) & blueMax;
    }

    uint8_t PixelFormat::alpha(int pixel) const
    {
        return (pixel >> alphaShift) & alphaMax;
    }

    Color PixelFormat::color(int pixel) const
    {
        return Color(red(pixel), green(pixel), blue(pixel), alpha(pixel));
    }

    uint32_t PixelFormat::pixel(const Color & col) const
    {
        uint32_t a = col.x;
        uint32_t r = col.r;
        uint32_t g = col.g;
        uint32_t b = col.b;

        r = (r * redMax) >> 8;
        g = (g * greenMax) >> 8;
        b = (b * blueMax) >> 8;

        if(alphaMax)
        {
            a = (a * alphaMax) >> 8;
            return (a << alphaShift) | (r << redShift) | (g << greenShift) | (b << blueShift);
        }

        return (r << redShift) | (g << greenShift) | (b << blueShift);
    }

    uint32_t convertMax(uint8_t col1, uint16_t max1, uint16_t max2)
    {
        return max1 ? (col1 * max2) / max1 : 0;
    }

    uint32_t convertPixelFromTo(uint32_t pixel, const PixelFormat & pf1, const PixelFormat & pf2)
    {
        if(pf2.compare(pf1, true))
        {
            if(pf2.amax() == pf1.amax() && pf2.ashift() == pf1.ashift())
                return pixel;

            if(0 == pf2.amax() && 0 != pf1.amax())
            {
                uint32_t amask = static_cast<uint32_t>(pf1.amax()) << pf1.ashift();
                return ~amask & pixel;
            }

            if(0 != pf2.amax() && 0 == pf1.amax())
            {
                uint32_t amask = static_cast<uint32_t>(pf2.amax()) << pf2.ashift();
                return amask | pixel;
            }
        }

        uint32_t r = pf1.rmax() == pf2.rmax() ? pf1.red(pixel) : convertMax(pf1.red(pixel), pf1.rmax(), pf2.rmax());
        uint32_t g = pf1.gmax() == pf2.gmax() ? pf1.green(pixel) : convertMax(pf1.green(pixel), pf1.gmax(), pf2.gmax());
        uint32_t b = pf1.bmax() == pf2.bmax() ? pf1.blue(pixel) : convertMax(pf1.blue(pixel), pf1.bmax(), pf2.bmax());
        uint32_t a = pf1.amax() == pf2.amax() ? pf1.alpha(pixel) : convertMax(pf1.alpha(pixel), pf1.amax(), pf2.amax());
        return (a << pf2.ashift()) | (r << pf2.rshift()) | (g << pf2.gshift()) | (b << pf2.bshift());
    }

    uint32_t PixelFormat::convertFrom(const PixelFormat & pf, uint32_t pixel) const
    {
        return convertPixelFromTo(pixel, pf, *this);
    }

    uint32_t PixelFormat::convertTo(uint32_t pixel, const PixelFormat & pf) const
    {
        return convertPixelFromTo(pixel, *this, pf);
    }

    fbinfo_t::fbinfo_t(const XCB::Size & fbsz, const PixelFormat & fmt, uint32_t pitch2) : format(fmt), allocated(1)
    {
        uint32_t pitch1 = fmt.bytePerPixel() * fbsz.width;
        pitch = std::max(pitch1, pitch2);
        size_t length = pitch * fbsz.height;
        buffer = new uint8_t[length];
        std::fill(buffer, buffer + length, 0);
    }

    fbinfo_t::fbinfo_t(uint8_t* ptr, const XCB::Size & fbsz, const PixelFormat & fmt, uint32_t pitch2) : format(fmt), buffer(ptr)
    {
        uint32_t pitch1 = fmt.bytePerPixel() * fbsz.width;
        pitch = std::max(pitch1, pitch2);
    }

    fbinfo_t::~fbinfo_t()
    {
        if(allocated)
        {
            delete [] buffer;
        }
    }

    int PixelMapPalette::findColorIndex(const uint32_t & col) const
    {
        auto it = find(col);
        return it != end() ? (*it).second : -1;
    }

    int PixelMapWeight::maxWeightPixel(void) const
    {
        auto it = std::max_element(begin(), end(), [](auto & p1, auto & p2)
        {
            return p1.second < p2.second;
        });

        return it != end() ? (*it).first : 0;
    }

    FrameBuffer FrameBuffer::copyRegionFormat(const XCB::Region & reg, const PixelFormat & pf) const
    {
        FrameBuffer res(reg.toSize(), pf, 0 /* pitch auto */);
        res.blitRegion(*this, reg, XCB::Point(0, 0));

        return res;
    }

    FrameBuffer FrameBuffer::copyRegion(const XCB::Region & reg) const
    {
        FrameBuffer res(reg.toSize(), pixelFormat(), reg.width == width() ? pitchSize() : 0 /* auto */);
        res.blitRegion(*this, reg, XCB::Point(0, 0));

        return res;
    }

    void FrameBuffer::setPixelRow(const XCB::Point & pos, uint32_t pixel, size_t length)
    {
        assertm(0 <= pos.x && 0 <= pos.y, "invalid position");
        assertm(pos.x < fbreg.width && pos.y < fbreg.height, "position out of range");

        void* offset = pitchData(pos.y) + (pos.x* bytePerPixel());
        // fix out of range
        length = std::min(length, static_cast<size_t>(fbreg.width - pos.x));

        assertm(length <= static_cast<size_t>(fbreg.width) - pos.x, "position out of range");

        switch(bitsPerPixel())
        {
            case 32:
                if(auto ptr = static_cast<uint32_t*>(offset))
                {
                    std::fill(ptr, ptr + length, pixel);
                }

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
                {
                    std::fill(ptr, ptr + length, static_cast<uint16_t>(pixel));
                }

                break;

            case 8:
                if(auto ptr = static_cast<uint8_t*>(offset))
                {
                    std::fill(ptr, ptr + length, static_cast<uint8_t>(pixel));
                }

                break;

            default:
                Application::error("%s: unknown bpp: %" PRIu8, __FUNCTION__, bitsPerPixel());
                throw std::invalid_argument(NS_FuncName);
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
            {
                setPixelRow(reg.topLeft() + XCB::Point(0, yy), raw, reg.width);
            }
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
            {
                setPixelRow(reg.topLeft() + XCB::Point(0, yy), raw, reg.width);
            }
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

    uint32_t FrameBuffer::rawPixel(const void* ptr, int bpp, bool BigEndian)
    {
        switch(bpp)
        {
            case 32:
                return *static_cast<const uint32_t*>(ptr);

            case 24:
            {
                auto buf = static_cast<const uint8_t*>(ptr);
                uint32_t res = 0;

                if(BigEndian)
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
                return *static_cast<const uint16_t*>(ptr);

            case 8:
                return *static_cast<const uint8_t*>(ptr);

            default:
                break;
        }

        Application::error("%s: unknown bpp: %" PRId32, __FUNCTION__, bpp);
        throw std::invalid_argument(NS_FuncName);
    }

    uint32_t FrameBuffer::pixel(const XCB::Point & pos) const
    {
        assertm(0 <= pos.x && 0 <= pos.y, "invalid position");
        assertm(pos.x < fbreg.width && pos.y < fbreg.height, "position out of range");

        void* ptr = pitchData(pos.y) + (static_cast<ptrdiff_t>(pos.x) * bytePerPixel());
        return rawPixel(ptr, bitsPerPixel(), platformBigEndian());
    }

    struct PixelIterator : XCB::PointIterator
    {
        const Point & topLeft;
        const FrameBuffer & fb;
        const uint8_t* pitch = nullptr;

        PixelIterator(const PointIterator & it, const Point & pt, const FrameBuffer & own) : XCB::PointIterator(it), topLeft(pt), fb(own)
        {
            pitch = fb.pitchData(topLeft.y + y);
        }

        void lineChanged(void) override
        {
            pitch = fb.pitchData(topLeft.y + y);
        }

        uint32_t pixel(void) const
        {
            auto ptr = pitch + ((topLeft.x + x) * fb.bytePerPixel());
            return FrameBuffer::rawPixel(ptr, fb.bitsPerPixel(), platformBigEndian());
        }
    };

    PixelLengthList FrameBuffer::toRLE(const XCB::Region & reg) const
    {
        std::vector<PixelLength> res;
        res.reserve(reg.width * (64 < reg.height ? reg.height / 2 : reg.height));

#ifdef FB_FAST_CYCLE

        for(uint16_t py = 0; py < reg.height; ++py)
        {
            const uint8_t* pitch = pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px)
            {
                auto ptr = pitch + ((reg.x + px) * bytePerPixel());
                auto pix = rawPixel(ptr, bitsPerPixel(), platformBigEndian());

                if(res.size() && res.back().pixel() == pix)
                {
                    res.back().second++;
                }
                else
                {
                    res.emplace_back(pix, 1);
                }
            }
        }

#else

        for(auto coord = PixelIterator(reg.coordBegin(), reg, *this); coord.isValid(); ++coord)
        {
            auto pix = coord.pixel();

            if(res.size() && res.back().pixel() == pix)
            {
                res.back().second++;
            }
            else
            {
                res.emplace_back(pix, 1);
            }
        }

#endif
        return res;
    }

    void FrameBuffer::blitRegion(const FrameBuffer & fb, const XCB::Region & reg, const XCB::Point & pos)
    {
        auto dst = XCB::Region(pos, reg.toSize()).intersected(region());

        if(pixelFormat() != fb.pixelFormat())
        {
            for(auto coord = dst.coordBegin(); coord.isValid(); ++coord)
            {
                setPixel(dst + coord, fb.pixel(reg.topLeft() + coord), & fb.pixelFormat());
            }
        }
        else
        {
            for(int row = 0; row < dst.height; ++row)
            {
                auto ptr = fb.pitchData(reg.y + row) + reg.x * fb.bytePerPixel();
                size_t length = dst.width * fb.bytePerPixel();
                std::copy(ptr, ptr + length, pitchData(dst.y + row) + dst.x* bytePerPixel());
            }
        }
    }

    ColorMap FrameBuffer::colourMap(void) const
    {
        ColorMap map;
        const PixelFormat & fmt = pixelFormat();

#ifdef FB_FAST_CYCLE
        const uint8_t* pitch = nullptr;

        for(auto coord = coordBegin(); coord.isValid(); ++coord)
        {
            if(coord.isBeginLine())
            {
                pitch = pitchData(fbreg.y + coord.y);
            }

            auto ptr = pitch + ((fbreg.x + coord.x) * bytePerPixel());
            auto pix = rawPixel(ptr, bitsPerPixel(), platformBigEndian());

            map.emplace(fmt.red(pix), fmt.green(pix), fmt.blue(pix));
        }

#else

        for(auto coord = PixelIterator(coordBegin(), fbreg, *this); coord.isValid(); ++coord)
        {
            auto pix = coord.pixel();
            map.emplace(fmt.red(pix), fmt.green(pix), fmt.blue(pix));
        }

#endif
        return map;
    }

    PixelMapPalette FrameBuffer::pixelMapPalette(const XCB::Region & reg) const
    {
        PixelMapPalette map;

#ifdef FB_FAST_CYCLE

        for(uint16_t py = 0; py < reg.height; ++py)
        {
            const uint8_t* pitch = pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px)
            {
                auto ptr = pitch + ((reg.x + px) * bytePerPixel());
                map.emplace(rawPixel(ptr, bitsPerPixel(), platformBigEndian()), 0);
            }
        }

#else

        for(auto coord = PixelIterator(reg.coordBegin(), reg, *this); coord.isValid(); ++coord)
        {
            map.emplace(coord.pixel(), 0);
        }

#endif

        int index = 0;
        for(auto & pair: map)
        {
            pair.second = index++;
        }

        return map;
    }

    PixelMapWeight FrameBuffer::pixelMapWeight(const XCB::Region & reg) const
    {
        PixelMapWeight map;

#ifdef FB_FAST_CYCLE

        for(uint16_t py = 0; py < reg.height; ++py)
        {
            const uint8_t* pitch = pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px)
            {
                auto ptr = pitch + ((reg.x + px) * bytePerPixel());
                auto pix = rawPixel(ptr, bitsPerPixel(), platformBigEndian());
                auto ret = map.emplace(pix, 1);
                if(! ret.second)
                {
                    ret.first->second += 1;
                }
            }
        }

#else

        for(auto coord = PixelIterator(reg.coordBegin(), reg, *this); coord.isValid(); ++coord)
        {
            auto pix = coord.pixel();
            auto ret = map.emplace(pix, 1);
            if(! ret.second)
            {
                ret.first->second += 1;
            }
        }

#endif
        return map;
    }

    bool FrameBuffer::allOfPixel(uint32_t pixel, const XCB::Region & reg) const
    {
#ifdef FB_FAST_CYCLE

        for(uint16_t py = 0; py < reg.height; ++py)
        {
            const uint8_t* pitch = pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px)
            {
                auto ptr = pitch + ((reg.x + px) * bytePerPixel());

                if(pixel != rawPixel(ptr, bitsPerPixel(), platformBigEndian())) { return false; }
            }
        }

#else

        for(auto coord = PixelIterator(reg.coordBegin(), reg, *this); coord.isValid(); ++coord)
            if(pixel != coord.pixel()) { return false; }

#endif
        return true;
    }

    bool FrameBuffer::renderChar(int ch, const Color & col, const XCB::Point & pos)
    {
        if(! std::isprint(ch))
        {
            return false;
        }

        size_t offsetx = ch * _systemfont.width * _systemfont.height >> 3;

        if(offsetx >= sizeof(_systemfont.data))
        {
            return false;
        }

        bool res = false;

        for(int yy = 0; yy < _systemfont.height; ++yy)
        {
            if(pos.y + yy < 0) { continue; }

            size_t offsety = yy * _systemfont.width >> 3;

            if(offsetx + offsety >= sizeof(_systemfont.data))
            {
                continue;
            }

            int line = *(_systemfont.data + offsetx + offsety);

            for(int xx = 0; xx < _systemfont.width; ++xx)
            {
                if(pos.x + xx < 0) { continue; }

                if(0x80 & (line << xx))
                {
                    setColor(pos + XCB::Point(xx, yy), col);
                    res = true;
                }
            }
        }

        return res;
    }

    void FrameBuffer::renderText(const std::string & str, const Color & col, const XCB::Point & pos)
    {
        int offset = 0;

        for(const auto & ch : str)
        {
            renderChar(ch, col, XCB::Point(pos.x + offset, pos.y));
            offset += _systemfont.width;
        }
    }

    Color FrameBuffer::color(const XCB::Point & pos) const
    {
        return pixelFormat().color(pixel(pos));
    }

    const uint8_t & FrameBuffer::bitsPerPixel(void) const
    {
        return pixelFormat().bitsPerPixel();
    }

    const uint8_t & FrameBuffer::bytePerPixel(void) const
    {
        return pixelFormat().bytePerPixel();
    }

    const uint16_t & FrameBuffer::width(void) const
    {
        return fbreg.width;
    }

    const uint16_t & FrameBuffer::height(void) const
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

        return fbptr->buffer + (fbptr->pitch* row) + col;
    }

    size_t FrameBuffer::pitchSize(void) const
    {
        return owner ? fbptr->pitch : bytePerPixel() * fbreg.width;
    }

    RawPtr<uint8_t> FrameBuffer::rawPtr(void) const
    {
        return RawPtr<uint8_t>(pitchData(0), pitchSize() * height());
    }
} // LTSM

#ifdef LTSM_WITH_PNG
#include <png.h>
namespace PNG
{
    bool save(const LTSM::FrameBuffer & fb, const std::string & file)
    {
        if(fb.pixelFormat().amask() && fb.pixelFormat() != RGBA32)
        {
            LTSM::FrameBuffer back(LTSM::XCB::Size(fb.width(), fb.height()), RGBA32);
            back.blitRegion(fb, fb.region(), LTSM::XCB::Point(0, 0));
            return save(back, file);
        }
        else if(fb.pixelFormat() != RGB24)
        {
            LTSM::FrameBuffer back(LTSM::XCB::Size(fb.width(), fb.height()), RGB24);
            back.blitRegion(fb, fb.region(), LTSM::XCB::Point(0, 0));
            return save(back, file);
        }

        // write png
        png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);

        if (!png_ptr)
        {
            return false;
        }

        png_infop info_ptr = png_create_info_struct(png_ptr);

        if (!info_ptr)
        {
            return false;
        }

        setjmp(png_jmpbuf(png_ptr));
        std::unique_ptr<FILE, int(*)(FILE*)> fp{fopen(file.c_str(), "wb"), fclose};

        png_init_io(png_ptr, fp.get());
        png_set_IHDR(png_ptr, info_ptr, fb.width(), fb.height(), 8,
                     fb.pixelFormat().amask() ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
        png_write_info(png_ptr, info_ptr);

        for(size_t row = 0; row < fb.height(); row++)
        {
            png_write_row(png_ptr, fb.pitchData(row));
        }

        png_write_end(png_ptr, nullptr);
        png_destroy_write_struct(&png_ptr, &info_ptr);

        return true;
    }
}

#endif
