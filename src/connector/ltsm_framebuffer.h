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

#ifndef _LTSM_FRAMEBUFFER_
#define _LTSM_FRAMEBUFFER_

#include <list>
#include <tuple>

#include "ltsm_global.h"
#include "ltsm_xcb_wrapper.h"

#ifdef LTSM_WITH_SDBUS
#include "ltsm_dbus_proxy.h"
#endif

namespace LTSM
{
    struct Color
    {
        uint8_t             r, g, b, x;

        Color() : r(0), g(0), b(0), x(0) {}
        Color(uint8_t cr, uint8_t cg, uint8_t cb) : r(cr), g(cg), b(cb), x(0) {}

#ifdef LTSM_WITH_SDBUS
        Color(const sdbus::Struct<uint8_t, uint8_t, uint8_t> & tuple) : x(0)
        {
            std::tie(r, g, b) = tuple;
        }
#endif
        int toRGB888(void) const
        {
            return (static_cast<int>(r) << 16) | (static_cast<int>(g) << 8) | b;
        }

        bool operator== (const Color & col) const
        {
            return r == col.r && g == col.g && b == col.b;
        }

        bool operator!= (const Color & col) const
        {
            return r != col.r || g != col.g || b != col.b;
        }
    };

    struct HasherColor
    {
        size_t operator()(const Color & col) const
        {
            return std::hash<size_t>()(col.toRGB888());
        }
    };

    struct ColorMap : INTSET<Color, HasherColor>
    {
    };

    struct PixelMapWeight : INTMAP<uint32_t /* pixel */, uint32_t /* weight */>
    {
        int                 maxWeightPixel(void) const;
    };

    struct PixelFormat
    {
        uint8_t             bitsPerPixel;
        uint8_t             depth;
        uint8_t             flags;

        uint8_t             redShift;
        uint8_t             greenShift;
        uint8_t             blueShift;

        uint16_t            redMax;
        uint16_t            greenMax;
        uint16_t            blueMax;

        enum { BigEndian = 0x01, TrueColor = 0x02 };

        PixelFormat() : bitsPerPixel(0), depth(0), flags(0), redShift(0),
            greenShift(0), blueShift(0), redMax(0), greenMax(0), blueMax(0) {}

        PixelFormat(int bpp, int dep, bool be, bool trucol, int rmask, int gmask, int bmask);
        PixelFormat(int bpp, int dep, bool be, bool trucol, int rmax, int gmax, int bmax, int rshift, int gshift, int bshift);

        bool operator!= (const PixelFormat & pf) const
        {
            return trueColor() != pf.trueColor() || bitsPerPixel != pf.bitsPerPixel ||
                   redMax != pf.redMax || greenMax != pf.greenMax || blueMax != pf.blueMax ||
                   redShift != pf.redShift || greenShift != pf.greenShift || blueShift != pf.blueShift;
        }

        bool bigEndian(void) const
        {
            return flags & BigEndian;
        }

        bool trueColor(void) const
        {
            return flags & TrueColor;
        }

        uint32_t red(int pixel) const
        {
            return (pixel >> redShift) & redMax;
        }

        uint32_t green(int pixel) const
        {
            return (pixel >> greenShift) & greenMax;
        }

        uint32_t blue(int pixel) const
        {
            return (pixel >> blueShift) & blueMax;
        }

        uint32_t bytePerPixel(void) const
        {
            return bitsPerPixel >> 3;
        }

        Color color(int pixel) const
        {
            return Color(red(pixel), green(pixel), blue(pixel));
        }

        uint32_t pixel(const Color & col) const
        {
            return ((static_cast<int>(col.r) * redMax / 0xFF) << redShift) |
                   ((static_cast<int>(col.g) * greenMax / 0xFF) << greenShift) | ((static_cast<int>(col.b) * blueMax / 0xFF) << blueShift);
        }

        uint32_t convertFrom(const PixelFormat & pf, uint32_t pixel) const
        {
            if(pf != *this)
            {
                uint32_t r = (pf.red(pixel) * redMax) / pf.redMax;
                uint32_t g = (pf.green(pixel) * greenMax) / pf.greenMax;
                uint32_t b = (pf.blue(pixel) * blueMax) / pf.blueMax;
                return (r << redShift) | (g << greenShift) | (b << blueShift);
            }
            return pixel;
        }
    };

    struct fbinfo_t
    {
        bool            allocated;
        uint32_t        pitch;
        uint8_t*        buffer;
        PixelFormat     format;

        fbinfo_t(const XCB::Size &, const PixelFormat & fmt);
        fbinfo_t(uint8_t* ptr, const XCB::Size &, const PixelFormat & fmt);
        ~fbinfo_t();
    };

    struct PixelLength : std::pair<uint32_t /* pixel */, uint32_t /* length */>
    {
	PixelLength(uint32_t pixel, uint32_t length) : std::pair<uint32_t, uint32_t>(pixel, length) {}

        const uint32_t &        pixel(void) const { return first; }
        const uint32_t &        length(void) const { return second; }
    };

    class FrameBuffer
    {
    protected:
        std::shared_ptr<fbinfo_t> fbptr;
	XCB::Region	fbreg;
        bool            owner;

    public:
//        FrameBuffer(const XCB::Region & reg, const PixelFormat & fmt)
//            : fbptr(std::make_shared<fbinfo_t>(reg.toSize(), fmt)), fbreg(reg) {}

        FrameBuffer(uint8_t* p, const XCB::Region & reg, const PixelFormat & fmt)
            : fbptr(std::make_shared<fbinfo_t>(p, reg.toSize(), fmt)), fbreg(reg), owner(true) {}

	XCB::PointIterator coordBegin(void) const { return XCB::PointIterator(0, 0, fbreg.toSize()); }

        void		setPixelRow(const XCB::Point &, uint32_t pixel, size_t length);

        void		setPixel(const XCB::Point &, uint32_t pixel, const PixelFormat &);
        void		setColor(const XCB::Point &, const Color &);

        bool		renderChar(int ch, const Color &, const XCB::Point &);
        void		renderText(const std::string &, const Color &, const XCB::Point &);

        void		fillPixel(const XCB::Region &, uint32_t pixel, const PixelFormat &);
        void		fillColor(const XCB::Region &, const Color &);
        void		drawRect(const XCB::Region &, const Color &);
	void		blitRegion(const FrameBuffer &, const XCB::Region &, const XCB::Point &);

        uint32_t        pixel(const XCB::Point &) const;

        ColorMap        colourMap(void) const;
        PixelMapWeight  pixelMapWeight(const XCB::Region &) const;
	std::list<PixelLength> toRLE(const XCB::Region &) const;
        bool            allOfPixel(uint32_t pixel, const XCB::Region &) const;

        uint8_t*        pitchData(size_t row) const;
        size_t          pitchSize(void) const;
        size_t          width(void) const;
        size_t          height(void) const;

	const XCB::Region & region(void) const { return fbreg; }
        const PixelFormat & pixelFormat(void) const { return fbptr->format; }

        Color           color(const XCB::Point &) const;
        uint32_t	bitsPerPixel(void) const;
        uint32_t	bytePerPixel(void) const;
    };
}

#endif // _LTSM_FRAMEBUFFER_
