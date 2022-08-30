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
        Color(uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca = 0) : r(cr), g(cg), b(cb), x(ca) {}

#ifdef LTSM_WITH_SDBUS
        explicit Color(const sdbus::Struct<uint8_t, uint8_t, uint8_t> & tuple) : x(0)
        {
            std::tie(r, g, b) = tuple;
        }
#endif
        int toARGB(void) const
        {
            return (static_cast<int>(x) << 24) | (static_cast<int>(r) << 16) | (static_cast<int>(g) << 8) | b;
        }

        bool operator== (const Color & col) const
        {
            return r == col.r && g == col.g && b == col.b && x == col.x;
        }

        bool operator!= (const Color & col) const
        {
            return r != col.r || g != col.g || b != col.b || x != col.x;
        }
    };

    struct HasherColor
    {
        size_t operator()(const Color & col) const
        {
            return std::hash<size_t>()(col.toARGB());
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
        uint16_t            redMax = 0;
        uint16_t            greenMax = 0;
        uint16_t            blueMax = 0;
        uint16_t            alphaMax = 0;

        uint8_t             redShift = 0;
        uint8_t             greenShift = 0;
        uint8_t             blueShift = 0;
        uint8_t             alphaShift = 0;

        uint8_t             bitsPerPixel = 0;

        PixelFormat() = default;
        PixelFormat(int bpp, int rmask, int gmask, int bmask, int amask);
        PixelFormat(int bpp, int rmax, int gmax, int bmax, int amax, int rshift, int gshift, int bshift, int ashift);

        bool operator!= (const PixelFormat & pf) const
        {
            return bitsPerPixel != pf.bitsPerPixel ||
                   redMax != pf.redMax || greenMax != pf.greenMax || blueMax != pf.blueMax || alphaMax != pf.alphaMax ||
                   redShift != pf.redShift || greenShift != pf.greenShift || blueShift != pf.blueShift || alphaShift != pf.alphaShift;
        }

        uint32_t        rmask(void) const;
        uint32_t        gmask(void) const;
        uint32_t        bmask(void) const;
        uint32_t        amask(void) const;

        uint32_t        red(int pixel) const;
        uint32_t        green(int pixel) const;
        uint32_t        blue(int pixel) const;
        uint32_t        alpha(int pixel) const;

        uint32_t        bytePerPixel(void) const;
        Color           color(int pixel) const;
        uint32_t        pixel(const Color & col) const;
        uint32_t        convertFrom(const PixelFormat & pf, uint32_t pixel) const;
    };

    struct fbinfo_t
    {
        PixelFormat     format;
        uint8_t*        buffer = nullptr;
        uint32_t        pitch = 0;
        uint8_t         allocated = 0;

        fbinfo_t(const XCB::Size &, const PixelFormat & fmt, uint32_t pitch2 = 0);
        fbinfo_t(uint8_t* ptr, const XCB::Size &, const PixelFormat & fmt, uint32_t pitch2 = 0);
        ~fbinfo_t();

        fbinfo_t(const fbinfo_t &) = delete;
        fbinfo_t & operator=(const fbinfo_t &) = delete;
    };

    struct PixelLength : std::pair<uint32_t /* pixel */, uint32_t /* length */>
    {
	PixelLength(uint32_t pixel, uint32_t length) : std::pair<uint32_t, uint32_t>(pixel, length) {}

        const uint32_t &        pixel(void) const { return first; }
        const uint32_t &        length(void) const { return second; }
    };

    class FrameBuffer
    {
        std::shared_ptr<fbinfo_t> fbptr;
	XCB::Region	fbreg;
        bool            owner;

    public:
        FrameBuffer(const XCB::Region & reg, const FrameBuffer & fb)
            : fbptr(fb.fbptr), fbreg(reg.topLeft() + fb.fbreg.topLeft(), reg.toSize()), owner(false) {}

        FrameBuffer(const XCB::Size & rsz, const PixelFormat & fmt, uint16_t pitch = 0)
            : fbptr(std::make_shared<fbinfo_t>(rsz, fmt, pitch)), fbreg(XCB::Point(0, 0), rsz), owner(true) {}

        FrameBuffer(uint8_t* p, const XCB::Region & reg, const PixelFormat & fmt, uint16_t pitch = 0)
            : fbptr(std::make_shared<fbinfo_t>(p, reg.toSize(), fmt, pitch)), fbreg(reg), owner(true) {}

	XCB::PointIterator coordBegin(void) const { return XCB::PointIterator(0, 0, fbreg.toSize()); }

        void		setPixelRow(const XCB::Point &, uint32_t pixel, size_t length);

        void		setPixel(const XCB::Point &, uint32_t pixel, const PixelFormat* = nullptr);
        void		setColor(const XCB::Point &, const Color &);

        bool		renderChar(int ch, const Color &, const XCB::Point &);
        void		renderText(const std::string &, const Color &, const XCB::Point &);

        void		fillPixel(const XCB::Region &, uint32_t pixel, const PixelFormat* = nullptr);
        void		fillColor(const XCB::Region &, const Color &);
        void		drawRect(const XCB::Region &, const Color &);
	void		blitRegion(const FrameBuffer &, const XCB::Region &, const XCB::Point &);

        uint32_t        pixel(const XCB::Point &) const;

        ColorMap        colourMap(void) const;
        PixelMapWeight  pixelMapWeight(const XCB::Region &) const;
	std::list<PixelLength> toRLE(const XCB::Region &) const;
        bool            allOfPixel(uint32_t pixel, const XCB::Region &) const;

        size_t          width(void) const;
        size_t          height(void) const;
        size_t          pitchSize(void) const;
        uint8_t*        pitchData(size_t row) const;

	const XCB::Region & region(void) const { return fbreg; }
        const PixelFormat & pixelFormat(void) const { return fbptr->format; }

        Color           color(const XCB::Point &) const;
        uint32_t	bitsPerPixel(void) const;
        uint32_t	bytePerPixel(void) const;
    };
}

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
 #define RGB565  LTSM::PixelFormat(16, 0x0000001F, 0x000007E0, 0x0000F800, 0)
 #define BGR565  LTSM::PixelFormat(16, 0x0000F800, 0x000007E0, 0x0000001F, 0)
 #define RGB24   LTSM::PixelFormat(24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0)
 #define BGR24   LTSM::PixelFormat(24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0)
 #define RGBA32  LTSM::PixelFormat(32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
 #define BGRA32  LTSM::PixelFormat(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
 #define ARGB32  LTSM::PixelFormat(32, 0x0000FF00, 0x00FF0000, 0xFF000000, 0x000000FF)
 #define ABGR32  LTSM::PixelFormat(32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF)
#else
 #define RGB565  LTSM::PixelFormat(16, 0x0000F800, 0x000007E0, 0x0000001F, 0)
 #define BGR565  LTSM::PixelFormat(16, 0x0000001F, 0x000007E0, 0x0000F800, 0)
 #define RGB24   LTSM::PixelFormat(24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0)
 #define BGR24   LTSM::PixelFormat(24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0)
 #define RGBA32  LTSM::PixelFormat(32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF)
 #define BGRA32  LTSM::PixelFormat(32, 0x0000FF00, 0x00FF0000, 0xFF000000, 0x000000FF)
 #define ARGB32  LTSM::PixelFormat(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
 #define ABGR32  LTSM::PixelFormat(32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
#endif

#endif // _LTSM_FRAMEBUFFER_
