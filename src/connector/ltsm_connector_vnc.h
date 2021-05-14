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

#ifndef _LTSM_CONNECTOR_VNC_
#define _LTSM_CONNECTOR_VNC_

#include <mutex>
#include <tuple>
#include <memory>
#include <future>
#include <atomic>
#include <functional>

#include "ltsm_connector.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    namespace RFB
    {
        struct Region
        {
            int16_t     x, y;
            uint16_t    w, h;

            Region() : x(0), y(0), w(0), h(0) {}
            Region(const xcb_rectangle_t & rt) : x(rt.x),  y(rt.y), w(rt.width), h(rt.height) {}
            Region(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) : x(rx), y(ry), w(rw), h(rh) {}
            Region(const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & tuple)
            {
                std::tie(x, y, w, h) = tuple;
            }

            void        reset(void);
            void        assign(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);

            bool        empty(void) const;
            bool        intersects(const Region &) const;

            Region      intersected(const Region &) const;
            static bool intersection(const Region &, const Region &, Region*);

            std::list<Region> divideBlocks(size_t cw, size_t ch) const;
            std::list<Region> divideCounts(size_t cw, size_t ch) const;

            bool        operator==(const Region & rt) const
            {
                return rt.x == x && rt.y == y && rt.w == w && rt.h == h;
            }

            bool        operator!=(const Region & rt) const
            {
                return rt.x != x || rt.y != y || rt.w != w || rt.h != h;
            }

            void        join(const Region &);
        };

        struct HasherRegion
        {
            size_t operator()(const Region & reg) const
            {
                return std::hash<uint64_t>()((static_cast<uint64_t>(reg.x) << 48) | (static_cast<uint64_t>(reg.y) << 32) |
                                             (static_cast<uint64_t>(reg.w) << 16) | static_cast<uint64_t>(reg.h));
            }
        };

        struct Color
        {
            uint8_t             r, g, b;

            Color() : r(0), g(0), b(0) {}
            Color(uint8_t cr, uint8_t cg, uint8_t cb) : r(cr), g(cg), b(cb) {}
            Color(const sdbus::Struct<uint8_t, uint8_t, uint8_t> & tuple)
            {
                std::tie(r, g, b) = tuple;
            }

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

        struct PixelMapWeight : INTMAP<int, int>
        {
            int                 maxWeightPixel(void) const;
        };

        struct PixelFormat
        {
            uint8_t             bitsPerPixel;
            uint8_t             depth;
            uint8_t             bigEndian;
            uint8_t             trueColor;

            uint16_t            redMax;
            uint16_t            greenMax;
            uint16_t            blueMax;

            uint8_t             redShift;
            uint8_t             greenShift;
            uint8_t             blueShift;

            PixelFormat() : bitsPerPixel(0), depth(0), bigEndian(0), trueColor(0), redMax(0), greenMax(0), blueMax(0),
                redShift(0), greenShift(0), blueShift(0) {}

            PixelFormat(int bpp, int dep, int be, int tc, int rmask, int gmask, int bmask);

            PixelFormat(int bpp, int dep, int be, int tc, int rmax, int gmax, int bmax, int rshift, int gshift, int bshift) : bitsPerPixel(bpp), depth(dep),
                bigEndian(be), trueColor(tc), redMax(rmax), greenMax(gmax), blueMax(bmax), redShift(rshift), greenShift(gshift), blueShift(bshift) {}

            bool                operator!= (const PixelFormat & pf) const
            {
                return trueColor != pf.trueColor || bitsPerPixel != pf.bitsPerPixel ||
                       redMax != pf.redMax || greenMax != pf.greenMax || blueMax != pf.blueMax ||
                       redShift != pf.redShift || greenShift != pf.greenShift || blueShift != pf.blueShift;
            }

            int red(int pixel) const
            {
                return (pixel >> redShift) & redMax;
            }

            int green(int pixel) const
            {
                return (pixel >> greenShift) & greenMax;
            }

            int blue(int pixel) const
            {
                return (pixel >> blueShift) & blueMax;
            }

            int bytePerPixel(void) const
            {
                return bitsPerPixel >> 3;
            }

            Color color(int pixel) const
            {
                return Color(red(pixel), green(pixel), blue(pixel));
            }

            int pixel(const Color & col) const
            {
                return ((static_cast<int>(col.r) * redMax / 0xFF) << redShift) |
                       ((static_cast<int>(col.g) * greenMax / 0xFF) << greenShift) | ((static_cast<int>(col.b) * blueMax / 0xFF) << blueShift);
            }

/*
            int convertTo(int pixel, const PixelFormat & pf) const
            {
                if(pf != *this)
                {
                    int r = (red(pixel) * pf.redMax) / redMax;
                    int g = (green(pixel) * pf.greenMax) / greenMax;
                    int b = (blue(pixel) * pf.blueMax) / blueMax;
                    return (r << pf.redShift) | (g << pf.greenShift) | (b << pf.blueShift);
                }
                return pixel;
            }
*/
            int convertFrom(const PixelFormat & pf, int pixel) const
            {
                if(pf != *this)
                {
                    int r = (pf.red(pixel) * redMax) / pf.redMax;
                    int g = (pf.green(pixel) * greenMax) / pf.greenMax;
                    int b = (pf.blue(pixel) * blueMax) / pf.blueMax;
                    return (r << redShift) | (g << greenShift) | (b << blueShift);
                }
                return pixel;
            }
        };

        struct fbinfo_t
        {
            uint32_t            pitch;
            uint8_t*            buffer;
            PixelFormat         format;
            bool                allocated;

            fbinfo_t(uint16_t width, uint16_t height, const PixelFormat & fmt);
            fbinfo_t(uint8_t* ptr, uint16_t width, uint16_t height, const PixelFormat & fmt);
            ~fbinfo_t();
        };

        struct FrameBuffer : std::shared_ptr<fbinfo_t>
        {
            uint16_t            width;
            uint16_t            height;
            uint32_t            offset;

            FrameBuffer(uint16_t w, uint16_t h, const PixelFormat & fmt)
                : std::shared_ptr<fbinfo_t>(std::make_shared<fbinfo_t>(w, h, fmt)), width(w), height(h), offset(0) {}

            FrameBuffer(uint8_t* p, uint16_t w, uint16_t h, const PixelFormat & fmt)
                : std::shared_ptr<fbinfo_t>(std::make_shared<fbinfo_t>(p, w, h, fmt)), width(w), height(h), offset(0) {}

            void		setPixelRaw(uint16_t px, uint16_t py, int pixel);

            void		setPixel(uint16_t px, uint16_t py, int pixel, const PixelFormat &);
            void		setColor(uint16_t px, uint16_t py, const Color &);

            bool		renderChar(int ch, const Color &, int px, int py);
            void		renderText(const std::string &, const Color &, int px, int py);

            void		fillPixel(const Region &, int pixel, const PixelFormat &);
            void		fillColor(const Region &, const Color &);
            void		drawRect(const Region &, const Color &);

            int                 pixel(uint16_t px, uint16_t py) const;

            ColorMap            colourMap(void) const;
            PixelMapWeight      pixelMapWeight(const Region &) const;
            bool                allOfPixel(int pixel, const Region &) const;

            uint8_t*            pitchData(size_t row) const;

            Color               color(uint16_t px, uint16_t py) const { return get()->format.color(pixel(px, py)); };
            int			bytePerPixel(void) const { return get() ? get()->format.bytePerPixel() : 0; }
            uint8_t*            data(void) { return get() ? get()->buffer : nullptr; }
            size_t		size(void) const { return get() ? get()->pitch * height : 0; }
        };
    }

    namespace Connector
    {
        typedef std::function<int(const RFB::Region &, const RFB::FrameBuffer &)> sendEncodingFunc;

        /* Connector::VNC */
        class VNC : public BaseStream, public SignalProxy
        {
            std::atomic<bool>   loopMessage;
            int                 encodingDebug;
            std::atomic<int>    pressedMask;
            std::atomic<bool>   fbUpdateComplete;
            RFB::PixelFormat    serverFormat;
            RFB::PixelFormat    clientFormat;
            RFB::Region         clientRegion;
            RFB::Region         serverRegion;
            std::mutex          sendGlobal;
            std::mutex		sendEncoding;
            RFB::Region         joinRegion;
            RFB::ColorMap       colourMap;
            std::vector<int>    clientEncodings;
            std::list<std::string> disabledEncodings;
            std::list< std::future<int> > jobsEncodings;
            INTSET<XCB::KeyCodes, XCB::HasherKeyCodes> pressedKeys;
            std::pair<sendEncodingFunc, std::string> prefEncodings;

        protected:
            // dbus virtual signals
            void                onLoginSuccess(const int32_t & display, const std::string & userName) override;
            void                onShutdownConnector(const int32_t & display) override;
            void                onHelperWidgetStarted(const int32_t & display) override;
            void                onSendBellSignal(const int32_t & display) override;

        protected:
	    void		xcbReleaseInputsEvent(void);

            void                clientSetPixelFormat(void);
            void                clientSetEncodings(void);
            bool                clientFramebufferUpdate(void);
            void                clientKeyEvent(void);
            void                clientPointerEvent(void);
            void                clientCutTextEvent(void);
            void                clientDisconnectedEvent(void);

            void                serverSendFrameBufferUpdate(const RFB::Region &);
            void                serverSendColourMap(int first);
            void                serverSendBell(void);
            void                serverSendCutText(const std::string &);

            int                 sendPixel(int pixel);
            void                waitSendUpdateFBComplete(void) const;

            int                 sendEncodingRaw(const RFB::Region &, const RFB::FrameBuffer &);
            int                 sendEncodingRawSubRegion(const RFB::Region &, const RFB::FrameBuffer &, int threadId);
            int                 sendEncodingRawSubRegionRaw(const RFB::Region &, const RFB::FrameBuffer &);

            int                 sendEncodingRRE(const RFB::Region &, const RFB::FrameBuffer &);
            int			sendEncodingRRESubRegion(const RFB::Region &, const RFB::FrameBuffer &, int threadId);

            int                 sendEncodingCoRRE(const RFB::Region &, const RFB::FrameBuffer &);
            int			sendEncodingCoRRESubRegion(const RFB::Region &, const RFB::FrameBuffer &, int threadId);

            int                 sendEncodingHextile(const RFB::Region &, const RFB::FrameBuffer &);
            int			sendEncodingHextileSubRegion(const RFB::Region &, const RFB::FrameBuffer &, int threadId);
            int			sendEncodingHextileSubRegionRaw(const RFB::Region &, const RFB::FrameBuffer &, int threadId);

            int                 sendEncodingZLib(const RFB::Region &, const RFB::FrameBuffer &);
            int			sendEncodingZLibSubRegion(const RFB::Region &, const RFB::FrameBuffer &, int threadId);

            int                 sendEncodingSmall(const RFB::Region &, const RFB::FrameBuffer &);

            void		renderPrimitivesTo(const RFB::Region &, RFB::FrameBuffer &);
            std::pair<sendEncodingFunc, std::string> selectEncodings(void);

        public:
            VNC(FILE* fd1, FILE* fd2, sdbus::IConnection* conn, const JsonObject & jo)
                : BaseStream(fd1, fd2), SignalProxy(conn, jo, "vnc"), loopMessage(false),
                  encodingDebug(0), pressedMask(0), fbUpdateComplete(false)
            {
                registerProxy();
            }

            ~VNC()
            {
                busConnectorTerminated(_display);
                unregisterProxy();
            }

            int		        communication(void) override;
        };
    }
}

#endif // _LTSM_CONNECTOR_VNC_
