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

#include "ltsm_sockets.h"
#include "ltsm_connector.h"
#include "ltsm_vnc_zlib.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    namespace RFB
    {
        // RFB protocol constant
        const int VERSION_MAJOR = 3;
        const int VERSION_MINOR = 8;

        const int SECURITY_TYPE_NONE = 1;
        const int SECURITY_TYPE_VNC = 2;
        const int SECURITY_TYPE_TLS = 18;
        const int SECURITY_TYPE_VENCRYPT = 19;
        const int SECURITY_VENCRYPT01_PLAIN = 19;
        const int SECURITY_VENCRYPT01_TLSNONE = 20;
        const int SECURITY_VENCRYPT01_TLSVNC = 21;
        const int SECURITY_VENCRYPT01_TLSPLAIN = 22;
        const int SECURITY_VENCRYPT01_X509NONE = 23;
        const int SECURITY_VENCRYPT01_X509VNC = 24;
        const int SECURITY_VENCRYPT01_X509PLAIN = 25;
        const int SECURITY_VENCRYPT02_PLAIN = 256;
        const int SECURITY_VENCRYPT02_TLSNONE = 257;
        const int SECURITY_VENCRYPT02_TLSVNC = 258;
        const int SECURITY_VENCRYPT02_TLSPLAIN = 259;
        const int SECURITY_VENCRYPT02_X509NONE = 260;
        const int SECURITY_VENCRYPT02_X509VNC = 261;
        const int SECURITY_VENCRYPT02_X509PLAIN = 261;

        const int SECURITY_RESULT_OK = 0;
        const int SECURITY_RESULT_ERR = 1;

        const int CLIENT_SET_PIXEL_FORMAT = 0;
        const int CLIENT_SET_ENCODINGS = 2;
        const int CLIENT_REQUEST_FB_UPDATE = 3;
        const int CLIENT_EVENT_KEY = 4;
        const int CLIENT_EVENT_POINTER = 5;
        const int CLIENT_CUT_TEXT = 6;
        const int CLIENT_SET_DESKTOP_SIZE = 251;
            
        const int SERVER_FB_UPDATE = 0;
        const int SERVER_SET_COLOURMAP = 1;
        const int SERVER_BELL = 2;
        const int SERVER_CUT_TEXT = 3;

        // RFB protocol constants
        const int ENCODING_RAW = 0;
        const int ENCODING_COPYRECT = 1;
        const int ENCODING_RRE = 2;
        const int ENCODING_CORRE = 4;
        const int ENCODING_HEXTILE = 5;
        const int ENCODING_ZLIB = 6;
        const int ENCODING_TIGHT = 7;
        const int ENCODING_ZLIBHEX = 8;
        const int ENCODING_TRLE = 15;
        const int ENCODING_ZRLE = 16;
 
        // hextile constants
        const int HEXTILE_RAW = 1;
        const int HEXTILE_BACKGROUND = 2;
        const int HEXTILE_FOREGROUND = 4;
        const int HEXTILE_SUBRECTS = 8;
        const int HEXTILE_COLOURED = 16;
        const int HEXTILE_ZLIBRAW = 32;
        const int HEXTILE_ZLIB = 64;
 
        // pseudo encodings
        const int ENCODING_DESKTOP_SIZE = -223;
        const int ENCODING_EXT_DESKTOP_SIZE = -308;

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

	struct ScreenInfo
	{
            uint32_t		id = 0;
            uint16_t		xpos = 0;
            uint16_t		ypos = 0;
            uint16_t		width = 0;
            uint16_t		height = 0;
            uint32_t		flags = 0;
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

	struct RLE : std::pair<int, size_t>
	{
	    RLE(int pixel, size_t length) : std::pair<int, size_t>(pixel, length) {}
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

            void		fillPixel(const XCB::Region &, int pixel, const PixelFormat &);
            void		fillColor(const XCB::Region &, const Color &);
            void		drawRect(const XCB::Region &, const Color &);
	    void		blitRegion(const FrameBuffer &, const XCB::Region &, int16_t dstx, int16_t dsty);

            int                 pixel(uint16_t px, uint16_t py) const;

            ColorMap            colourMap(void) const;
            PixelMapWeight      pixelMapWeight(const XCB::Region &) const;
	    std::list<RLE>      toRLE(const XCB::Region &) const;
            bool                allOfPixel(int pixel, const XCB::Region &) const;

            uint8_t*            pitchData(size_t row) const;

            Color               color(uint16_t px, uint16_t py) const { return get()->format.color(pixel(px, py)); };
            int			bytePerPixel(void) const { return get() ? get()->format.bytePerPixel() : 0; }
            uint8_t*            data(void) { return get() ? get()->buffer : nullptr; }
            size_t		size(void) const { return get() ? get()->pitch * height : 0; }
        };
    }

    namespace RRE
    {
        struct Region : std::pair<XCB::Region, int>
        {
            Region(const XCB::Region & reg, int pixel) : std::pair<XCB::Region, int>(reg, pixel) {}
            Region() {}
        };
    }

    enum class DesktopResizeMode { Undefined, Disabled, Success, ServerInform, ClientRequest };
    const char* desktopResizeModeString(const DesktopResizeMode &);

    namespace Connector
    {
        typedef std::function<int(const XCB::Region &, const RFB::FrameBuffer &)> sendEncodingFunc;

        /* Connector::VNC */
        class VNC : public SignalProxy, protected NetworkStream
        {
	    std::unique_ptr<NetworkStream> socket;	/// socket layer
	    std::unique_ptr<TLS::Stream> tls;		/// tls layer
	    std::unique_ptr<ZLib::DeflateStream> zlib;	/// zlib layer

	    NetworkStream* 	streamIn;
	    NetworkStream* 	streamOut;

            std::atomic<bool>   loopMessage;
            int                 encodingDebug;
            int                 encodingThreads;
            std::atomic<int>    pressedMask;
            std::atomic<bool>   fbUpdateProcessing;
	    std::atomic<bool>	sendBellFlag;
	    std::atomic<DesktopResizeMode> desktopResizeMode;
            RFB::PixelFormat    serverFormat;
            RFB::PixelFormat    clientFormat;
            XCB::Region         clientRegion;
            XCB::Region         serverRegion;
            std::mutex          sendGlobal;
            std::mutex		sendEncoding;
            RFB::ColorMap       colourMap;
            std::vector<int>    clientEncodings;
            std::list<std::string> disabledEncodings;
            std::list< std::future<int> > jobsEncodings;
            std::list<XCB::KeyCodes> pressedKeys;
            std::pair<sendEncodingFunc, int> prefEncodings;
	    std::vector<RFB::ScreenInfo> screensInfo;


	    // network stream interface
	    void		  sendFlush(void) override { return streamOut->sendFlush(); }
	    void		  sendRaw(const void* ptr, size_t len) override { streamOut->sendRaw(ptr, len); }
	    void                  recvRaw(void* ptr, size_t len) const override { streamIn->recvRaw(ptr, len); }
	    bool		  hasInput(void) const override { return streamIn->hasInput(); }
	    uint8_t		  peekInt8(void) const override { return streamIn->peekInt8(); }

	    // zlib wrapper
	    void		 zlibDeflateStart(size_t);
	    std::vector<uint8_t> zlibDeflateStop(void);

        protected:
            // dbus virtual signals
            void                onLoginSuccess(const int32_t & display, const std::string & userName) override;
            void                onShutdownConnector(const int32_t & display) override;
            void                onHelperWidgetStarted(const int32_t & display) override;
            void                onSendBellSignal(const int32_t & display) override;
	    void		onAddDamage(const XCB::Region &) override;

        protected:
	    void		xcbReleaseInputsEvent(void);

            void                clientSetPixelFormat(void);
	    bool		clientVenCryptHandshake(void);
            bool                clientSetEncodings(void);
            bool                clientFramebufferUpdate(void);
            void                clientKeyEvent(void);
            void                clientPointerEvent(void);
            void                clientCutTextEvent(void);
	    void		clientSetDesktopSizeEvent(void);
            void                clientDisconnectedEvent(void);

            bool                serverSendFrameBufferUpdate(const XCB::Region &);
            void                serverSendColourMap(int first);
            void                serverSendBell(void);
            void                serverSendCutText(const std::vector<uint8_t> &);
	    int			serverSendDesktopSize(const DesktopResizeMode &);

            int                 sendPixel(int pixel);
            int                 sendCPixel(int pixel);

            bool                isUpdateProcessed(void) const;
            void                waitSendingFBUpdate(void) const;

            int                 sendEncodingRaw(const XCB::Region &, const RFB::FrameBuffer &);
            int                 sendEncodingRawSubRegion(const XCB::Point &, const XCB::Region &, const RFB::FrameBuffer &, int jobId);
            int                 sendEncodingRawSubRegionRaw(const XCB::Region &, const RFB::FrameBuffer &);

            int                 sendEncodingRRE(const XCB::Region &, const RFB::FrameBuffer &, bool corre);
            int			sendEncodingRRESubRegion(const XCB::Point &, const XCB::Region &, const RFB::FrameBuffer &, int jobId, bool corre);
            int                 sendEncodingRRESubRects(const XCB::Region &, const RFB::FrameBuffer &, int jobId, int back, const std::list<RRE::Region> &, bool corre);

            int                 sendEncodingHextile(const XCB::Region &, const RFB::FrameBuffer &, bool zlibver);
            int			sendEncodingHextileSubRegion(const XCB::Point &, const XCB::Region &, const RFB::FrameBuffer &, int jobId, bool zlibver);
            int			sendEncodingHextileSubForeground(const XCB::Region &, const RFB::FrameBuffer &, int jobId, int back, const std::list<RRE::Region> &);
            int			sendEncodingHextileSubColored(const XCB::Region &, const RFB::FrameBuffer &, int jobId, int back, const std::list<RRE::Region> &);
            int			sendEncodingHextileSubRaw(const XCB::Region &, const RFB::FrameBuffer &, int jobId, bool zlibver);

            int                 sendEncodingZLib(const XCB::Region &, const RFB::FrameBuffer &);
            int			sendEncodingZLibSubRegion(const XCB::Point &, const XCB::Region &, const RFB::FrameBuffer &, int jobId);

            int                 sendEncodingTRLE(const XCB::Region &, const RFB::FrameBuffer &, bool zrle);
            int			sendEncodingTRLESubRegion(const XCB::Point &, const XCB::Region &, const RFB::FrameBuffer &, int jobId, bool zrle);
            int                 sendEncodingTRLESubPacked(const XCB::Region &, const RFB::FrameBuffer &, int jobId, size_t field, size_t rowsz, const RFB::PixelMapWeight &, bool zrle);
	    int			sendEncodingTRLESubPlain(const XCB::Region &, const RFB::FrameBuffer &, const std::list<RFB::RLE> &);
	    int			sendEncodingTRLESubPalette(const XCB::Region &, const RFB::FrameBuffer &, const RFB::PixelMapWeight &, const std::list<RFB::RLE> &);
	    int			sendEncodingTRLESubRaw(const XCB::Region &, const RFB::FrameBuffer &);


            void		renderPrimitivesTo(const XCB::Region &, RFB::FrameBuffer &);
            std::pair<sendEncodingFunc, int> selectEncodings(void);

        public:
            VNC(sdbus::IConnection* conn, const JsonObject & jo);
            ~VNC();

            int		        communication(void) override;
        };
    }
}

#endif // _LTSM_CONNECTOR_VNC_
