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

#include <tuple>
#include <cctype>
#include <string>
#include <chrono>
#include <thread>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <iterator>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_font_psf.h"
#include "ltsm_connector.h"
#include "ltsm_connector_vnc.h"

using namespace std::chrono_literals;

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

        const int SERVER_FB_UPDATE = 0;
        const int SERVER_SET_COLOURMAP = 1;
        const int SERVER_BELL = 2;
        const int SERVER_CUT_TEXT = 3;

        const char* encodingName(int type);

        PixelFormat::PixelFormat(int bpp, int dep, int be, int tc, int rmask, int gmask, int bmask)
            : bitsPerPixel(bpp), depth(dep), bigEndian(be), trueColor(tc)
        {
            redMax = Tools::maskMaxValue(rmask);
            greenMax = Tools::maskMaxValue(gmask);
            blueMax = Tools::maskMaxValue(bmask);
            redShift = Tools::maskShifted(rmask);
            greenShift = Tools::maskShifted(gmask);
            blueShift = Tools::maskShifted(bmask);
        }

        fbinfo_t::fbinfo_t(uint16_t width, uint16_t height, const PixelFormat & fmt)
            : pitch(0), buffer(nullptr), format(fmt), allocated(true)
        {
            pitch = fmt.bytePerPixel() * width;
            size_t length = pitch * height;
            buffer = new uint8_t[length];
            std::fill(buffer, buffer + length, 0);
        }

        fbinfo_t::fbinfo_t(uint8_t* ptr, uint16_t width, uint16_t height, const PixelFormat & fmt)
            : pitch(0), buffer(ptr), format(fmt), allocated(false)
        {
            pitch = fmt.bytePerPixel() * width;
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

        uint8_t* FrameBuffer::pitchData(size_t row) const
        {
            return get()->buffer + offset + (get()->pitch * row);
        }

        void FrameBuffer::setPixelRaw(uint16_t px, uint16_t py, int pixel)
        {
            if(px < width && py < height)
            {
                uint8_t* ptr = pitchData(py) + (px * bytePerPixel());

                switch(bytePerPixel())
                {
                    case 4:
                        *reinterpret_cast<uint32_t*>(ptr) = pixel;
                        break;

                    case 2:
                        *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(pixel);
                        break;

                    default:
                        *ptr = static_cast<uint8_t>(pixel);
                        break;
                }
            }
        }

        void FrameBuffer::setPixel(uint16_t px, uint16_t py, int val, const PixelFormat & pf)
        {
            int pixel = get()->format.convertFrom(pf, val);
            setPixelRaw(px, py, pixel);
        }

        void FrameBuffer::fillPixel(const Region & reg, int val, const PixelFormat & pf)
        {
            int pixel = get()->format.convertFrom(pf, val);

            for(int yy = 0; yy < reg.h; ++yy)
                for(int xx = 0; xx < reg.w; ++xx)
                    setPixelRaw(reg.x + xx, reg.y + yy, pixel);
        }

        void FrameBuffer::setColor(uint16_t px, uint16_t py, const Color & col)
        {
            int pixel = get()->format.pixel(col);
            setPixelRaw(px, py, pixel);
        }

        void FrameBuffer::fillColor(const Region & reg, const Color & col)
        {
            int pixel = get()->format.pixel(col);

            for(int yy = 0; yy < reg.h; ++yy)
                for(int xx = 0; xx < reg.w; ++xx)
                    setPixelRaw(reg.x + xx, reg.y + yy, pixel);
        }

        void FrameBuffer::drawRect(const Region & reg, const Color & col)
        {
            int pixel = get()->format.pixel(col);

            for(int xx = 0; xx < reg.w; ++xx)
            {
                setPixelRaw(reg.x + xx, reg.y, pixel);
                setPixelRaw(reg.x + xx, reg.y + reg.h - 1, pixel);
            }

            for(int yy = 1; yy < reg.h - 1; ++yy)
            {
                setPixelRaw(reg.x, reg.y + yy, pixel);
                setPixelRaw(reg.x + reg.w - 1, reg.y + yy, pixel);
            }
        }

        int FrameBuffer::pixel(uint16_t px, uint16_t py) const
        {
            if(px < width && py < height)
            {
                uint8_t* ptr = pitchData(py) + (px * bytePerPixel());

                if(bytePerPixel() == 4)
                    return *reinterpret_cast<uint32_t*>(ptr);

                if(bytePerPixel() == 2)
                    return *reinterpret_cast<uint16_t*>(ptr);

                return *ptr;
            }

            return 0;
        }

	std::list<RLE> FrameBuffer::toRLE(const Region & reg) const
	{
	    std::list<RLE> res;

            for(int yy = 0; yy < reg.h; ++yy)
            {
                for(int xx = 0; xx < reg.w; ++xx)
                {
                    int pix = pixel(xx + reg.x, yy + reg.y);

		    if(0 < xx && res.back().first == pix)
			res.back().second++;
		    else
			res.emplace_back(pix, 1);
		}
	    }

	    return res;
	}

	void FrameBuffer::blitRegion(const FrameBuffer & fb, const Region & reg, int16_t dstx, int16_t dsty)
	{
	    Region dst = Region(dstx, dsty, reg.w, reg.h).intersected({0, 0, reg.w, reg.h});

    	    if(get()->format != fb.get()->format)
    	    {
        	for(int yy = 0; yy < dst.w; ++yy)
            	    for(int xx = 0; xx < dst.h; ++xx)
                	setPixel(dst.x + xx, dst.y + yy, fb.pixel(reg.x + xx, reg.y + yy), fb.get()->format);
    	    }
    	    else
    	    {
        	for(int row = 0; row < dst.h; ++row)
        	{
            	    auto ptr = fb.pitchData(reg.y + row) + reg.x * fb.get()->format.bytePerPixel();
            	    size_t length = dst.w * fb.get()->format.bytePerPixel();
            	    std::copy(ptr, ptr + length, pitchData(row));
        	}
    	    }
	}

        ColorMap FrameBuffer::colourMap(void) const
        {
            ColorMap map;
            const PixelFormat & fmt = get()->format;

            for(int yy = 0; yy < height; ++yy)
            {
                for(int xx = 0; xx < width; ++xx)
                {
                    int pix = pixel(xx, yy);
                    map.emplace(fmt.red(pix), fmt.green(pix), fmt.blue(pix));
                }
            }

            return map;
        }

        PixelMapWeight FrameBuffer::pixelMapWeight(const RFB::Region & reg) const
        {
            PixelMapWeight map;

            for(int yy = 0; yy < reg.h; ++yy)
            {
                for(int xx = 0; xx < reg.w; ++xx)
                {
                    int val = pixel(reg.x + xx, reg.y + yy);
                    auto it = map.find(val);

                    if(it != map.end())
                        (*it).second += 1;
                    else
                        map.emplace(val, 1);
                }
            }

            return map;
        }

        bool FrameBuffer::allOfPixel(int value, const RFB::Region & reg) const
        {
            for(int yy = 0; yy < reg.h; ++yy)
                for(int xx = 0; xx < reg.w; ++xx)
                    if(value != pixel(reg.x + xx, reg.y + yy)) return false;

            return true;
        }

        bool FrameBuffer::renderChar(int ch, const Color & col, int px, int py)
        {
            if(std::isprint(ch))
            {
                size_t offsetx = ch * _systemfont.width * _systemfont.height >> 3;

                if(offsetx >= sizeof(_systemfont.data))
                    return false;

                bool res = false;

                for(int yy = 0; yy < _systemfont.height; ++yy)
                {
                    if(py + yy < 0) continue;

                    size_t offsety = yy * _systemfont.width >> 3;

                    if(offsetx + offsety >= sizeof(_systemfont.data))
                        continue;

                    int line = *(_systemfont.data + offsetx + offsety);

                    for(int xx = 0; xx < _systemfont.width; ++xx)
                    {
                        if(px + xx < 0) continue;

                        if(0x80 & (line << xx))
                        {
                            setColor(px + xx, py + yy, col);
                            res = true;
                        }
                    }
                }

                return res;
            }

            return false;
        }

        void FrameBuffer::renderText(const std::string & str, const Color & col, int px, int py)
        {
            int offset = 0;

            for(auto & ch : str)
            {
                renderChar(ch, col, px + offset, py);
                offset += _systemfont.width;
            }
        }

        Region operator- (const Region & reg, const Point & pt)
        {
            return Region(reg.x - pt.x, reg.y - pt.y, reg.w, reg.h);
        }
            
        Region operator+ (const Region & reg, const Point & pt)
        {
            return Region(reg.x + pt.x, reg.y + pt.y, reg.w, reg.h);
        }

        void Region::assign(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh)
        {
            x = rx;
            y = ry;
            w = rw;
            h = rh;
        }

        void Region::reset(void)
        {
            assign(-1, -1, 0, 0);
        }

        bool Region::invalid(void) const
        {
            return x == -1 && y == -1 && w == 0 && h == 0;
        }

        bool Region::empty(void) const
        {
            return w == 0 || h == 0;
        }

        bool Region::intersects(const Region & rt) const
        {
            if(empty() || rt.empty())
                return false;

            // horizontal intersection
            if(std::min(x + w, rt.x + rt.w) <= std::max(x, rt.x))
                return false;

            // vertical intersection
            if(std::min(y + h, rt.y + rt.h) <= std::max(y, rt.y))
                return false;

            return true;
        }

        Region Region::intersected(const Region & reg) const
        {
            Region res;
            intersection(*this, reg, & res);
            return res;
        }

        bool Region::intersection(const Region & rt1, const Region & rt2, Region* res)
        {
            if(! res)
                return rt1.intersects(rt2);

            if(rt1.empty() || rt2.empty())
            {
                res->w = 0;
                res->h = 0;
                return false;
            }

            // horizontal intersection
            res->x = std::max(rt1.x, rt2.x);
            res->w = std::min(rt1.x + rt1.w, rt2.x + rt2.w) - res->x;
            // vertical intersection
            res->y = std::max(rt1.y, rt2.y);
            res->h = std::min(rt1.y + rt1.h, rt2.y + rt2.h) - res->y;
            return ! res->empty();
        }

        std::list<Region> Region::divideCounts(size_t cw, size_t ch) const
        {
            size_t bw = w <= cw ? 1 : w / cw;
            size_t bh = h <= cw ? 1 : h / ch;
            return divideBlocks(bw, bh);
        }

        std::list<Region> Region::divideBlocks(size_t cw, size_t ch) const
        {
            std::list<Region> res;

            if(cw > w) cw = w;
            if(ch > h) ch = h;

            for(size_t yy = 0; yy < h; yy += ch)
            {
                for(size_t xx = 0; xx < w; xx += cw)
                {
                    uint16_t fixedw = std::min(w - xx, cw);
                    uint16_t fixedh = std::min(h - yy, ch);
                    res.emplace_back(x + xx, y + yy, fixedw, fixedh);
                }
            }

            return res;
        }

        void Region::join(const Region & rt)
        {
	    if(invalid())
	    {
		x = rt.x;
		y = rt.y;
		w = rt.w;
		h = rt.h;
	    }
	    else
            if(! rt.empty() && *this != rt)
            {
                /* Horizontal union */
                auto xm = std::min(x, rt.x);
                w = std::max(x + w, rt.x + rt.w) - xm;
		x = xm;
                /* Vertical union */
                auto ym = std::min(y, rt.y);
                h = std::max(y + h, rt.y + rt.h) - ym;
		y = ym;
            }
        }
    }

    /* Connector::VNC */
    int Connector::VNC::communication(void)
    {
	if(0 >= busGetServiceVersion())
	{
            Application::error("%s", "bus service failure");
            return EXIT_FAILURE;
	}

        const std::string remoteaddr = Tools::getenv("REMOTE_ADDR", "local");
        Application::info("connected: %s\n", remoteaddr.c_str());
        Application::info("using encoding threads: %d", _encodingThreads);
        encodingDebug = _config->getInteger("encoding:debug", 0);
        prefEncodings = selectEncodings();
        disabledEncodings = _config->getStdList<std::string>("encoding:blacklist");

        bool tlsDisable = _config->getBoolean("gnutls:disable", false);
        int tlsDebug = _config->getInteger("gnutls:debug", 3);
        std::string encriptionInfo = "none";

        if(! disabledEncodings.empty())
            disabledEncodings.remove_if([](auto & str){ return 0 == Tools::lower(str).compare("raw"); });

        // RFB 6.1.1 version
        const std::string version = Tools::StringFormat("RFB 00%1.00%2\n").arg(RFB::VERSION_MAJOR).arg(RFB::VERSION_MINOR);
        sendString(version).sendFlush();
        std::string magick = recvString(12);
        Application::debug("RFB 6.1.1, handshake version: %s", magick.c_str());

        if(magick != version)
        {
            Application::error("%s", "handshake failure");
            return EXIT_FAILURE;
        }

        // Xvfb: session request
        int screen = busStartLoginSession(remoteaddr, "vnc");
        if(screen <= 0)
        {
            Application::error("%s", "login session request failure");
            return EXIT_FAILURE;
        }

        Application::debug("login session request success, display: %d", screen);
	Application::info("server default encoding: %s", RFB::encodingName(prefEncodings.second));

        if(! xcbConnect(screen))
        {
            Application::error("%s", "xcb connect failed");
            return EXIT_FAILURE;
        }

        const xcb_visualtype_t* visual = _xcbDisplay->visual();
        if(! visual)
        {
            Application::error("%s", "xcb visual empty");
            return EXIT_FAILURE;
        }

        // init server format
#ifdef __ORDER_LITTLE_ENDIAN__
        const int bigEndian = 0;
#else
        const int bigEndian = 1;
#endif
        serverFormat = RFB::PixelFormat(_xcbDisplay->bitsPerPixel(), _xcbDisplay->depth(), bigEndian, 1, visual->red_mask, visual->green_mask, visual->blue_mask);

        // RFB 6.1.2 security
	if(tlsDisable)
	{
	    sendInt8(1);
	    sendInt8(RFB::SECURITY_TYPE_NONE);
	    sendFlush();
        }
	else
	{
    	    sendInt8(2);
	    sendInt8(RFB::SECURITY_TYPE_VENCRYPT);
	    sendInt8(RFB::SECURITY_TYPE_NONE);
	    sendFlush();
	}
        int clientSecurity = recvInt8();
        Application::debug("RFB 6.1.2, client security: 0x%02x", clientSecurity);

        if(clientSecurity == RFB::SECURITY_TYPE_NONE)
            sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
        else
        if(clientSecurity == RFB::SECURITY_TYPE_VENCRYPT)
	{
	    // VenCrypt version
	    sendInt8(0).sendInt8(2).sendFlush();
	    // client req
	    int majorVer = recvInt8();
	    int minorVer = recvInt8();
    	    Application::debug("RFB 6.2.19, client vencrypt version: %d.%d", majorVer, minorVer);

	    if(majorVer != 0 || (minorVer < 1 || minorVer > 2))
	    {
		// send unsupported
		sendInt8(255).sendFlush();
        	Application::error("error: %s", "unsupported vencrypt version");
        	return EXIT_FAILURE;
	    }

	    // send supported
	    sendInt8(0);

	    if(minorVer == 1)
	    {
		sendInt8(1).sendInt8(RFB::SECURITY_VENCRYPT01_TLSNONE).sendFlush();

		int res = recvInt8();
    		Application::debug("RFB 6.2.19.0.1, client choice vencrypt security: 0x%02x", res);

		if(res != RFB::SECURITY_VENCRYPT01_TLSNONE)
        	{
		    Application::error("error: %s", "unsupported vencrypt security");
        	    return EXIT_FAILURE;
		}
	    }
	    else
	    if(minorVer == 2)
	    {
		sendInt8(1).sendIntBE32(RFB::SECURITY_VENCRYPT02_TLSNONE).sendFlush();

		int res = recvIntBE32();
    		Application::debug("RFB 6.2.19.0.2, client choice vencrypt security: 0x%08x", res);

		if(res != RFB::SECURITY_VENCRYPT02_TLSNONE)
        	{
		    Application::error("error: %s", "unsupported vencrypt security");
        	    return EXIT_FAILURE;
		}
	    }
    	    else
	    {
		sendInt8(0).sendFlush();
		Application::error("error: %s", "unsupported vencrypt security");
    		return EXIT_FAILURE;
	    }

            sendInt8(1).sendFlush();

	    if(! tlsInitHandshake(tlsDebug))
		return EXIT_FAILURE;

            encriptionInfo = tls->sessionDescription();
            sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
	}
	else
        {
            const char* err = "no matching security types";
            sendIntBE32(RFB::SECURITY_RESULT_ERR).sendRaw(err, strlen(err)).sendFlush();

            Application::error("error: %s", err);
            return EXIT_FAILURE;
        }

    	busSetEncriptionInfo(screen, encriptionInfo);

        // RFB 6.3.1 client init
        int clientSharedFlag = recvInt8();
        Application::debug("RFB 6.3.1, client shared: 0x%02x", clientSharedFlag);
        // RFB 6.3.2 server init
        sendIntBE16(_xcbDisplay->width());
        sendIntBE16(_xcbDisplay->height());
        Application::debug("server send: pixel format, bpp: %d, depth: %d, be: %d, truecol: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                           serverFormat.bitsPerPixel, serverFormat.depth, serverFormat.bigEndian, serverFormat.trueColor,
                           serverFormat.redMax, serverFormat.redShift, serverFormat.greenMax, serverFormat.greenShift, serverFormat.blueMax, serverFormat.blueShift);
        clientFormat = serverFormat;
        // send pixel format
        sendInt8(serverFormat.bitsPerPixel);
        sendInt8(serverFormat.depth);
        sendInt8(serverFormat.bigEndian);
        sendInt8(serverFormat.trueColor);
        sendIntBE16(serverFormat.redMax);
        sendIntBE16(serverFormat.greenMax);
        sendIntBE16(serverFormat.blueMax);
        sendInt8(serverFormat.redShift);
        sendInt8(serverFormat.greenShift);
        sendInt8(serverFormat.blueShift);
        // send padding
        sendInt8(0);
        sendInt8(0);
        sendInt8(0);
        // send name desktop
        const std::string desktopName("X11 Remote Desktop");
        sendIntBE32(desktopName.size());
        sendString(desktopName).sendFlush();
        Application::info("%s", "connector starting, wait RFB messages...");

        // wait widget started signal
        while(! loopMessage)
        {
            // dbus processing
            _conn->enterEventLoopAsync();
            // wait
            std::this_thread::sleep_for(1ms);
        }

        // xcb on
        _xcbDisableMessages = false;
        serverRegion.assign(0, 0, _xcbDisplay->width(), _xcbDisplay->height());
	bool clientUpdateReq = false;

        while(loopMessage)
        {
            // RFB: mesage loop
            if(hasInput())
            {
                int msgType = recvInt8();

                switch(msgType)
                {
                    case RFB::CLIENT_SET_PIXEL_FORMAT:
                        clientSetPixelFormat();
                        joinRegion.join(serverRegion);
			clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_SET_ENCODINGS:
                        if(clientSetEncodings())
                        {
                            // full update
                            joinRegion.join(serverRegion);
			    clientUpdateReq = true;
                        }
                        break;

                    case RFB::CLIENT_REQUEST_FB_UPDATE:
                        // full update
                        if(clientFramebufferUpdate())
                            joinRegion.join(serverRegion);
			clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_EVENT_KEY:
                        clientKeyEvent();
			clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_EVENT_POINTER:
                        clientPointerEvent();
			clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_CUT_TEXT:
                        clientCutTextEvent();
			clientUpdateReq = true;
                        break;

                    default:
                        throw std::string("RFB unknown message: ").append(Tools::hex(msgType, 2));
                        break;
                }
            }

            if(! _xcbDisableMessages)
            {
                // get all damages and join it
                while(auto ev = _xcbDisplay->poolEvent())
                {
                    int ret = _xcbDisplay->getEventSHM(ev);

                    if(0 <= ret)
                    {
                        Application::error("get event shm: return code: %d", ret);
                        continue;
                    }

                    ret = _xcbDisplay->getEventTEST(ev);

                    if(0 <= ret)
                    {
                        Application::error("get event test: return code: %d", ret);
                        continue;
                    }

                    if(_xcbDisplay->isEventDAMAGE(ev, XCB_DAMAGE_NOTIFY))
                    {
                	const xcb_damage_notify_event_t* notify = (xcb_damage_notify_event_t*) ev.get();
                	joinRegion.join(notify->area);
		    }
                }

                // server action
                if(clientUpdateReq && ! joinRegion.empty() && ! fbUpdateProcessing)
                {
                    RFB::Region res;

                    if(RFB::Region::intersection(clientRegion, joinRegion, & res))
		    {
			fbUpdateProcessing = true;
			// background job
                        std::thread([=](){ this->serverSendFrameBufferUpdate(res); }).detach();
		    }
                    joinRegion.reset();
		    clientUpdateReq = false;
                }
            }

            // dbus processing
            _conn->enterEventLoopAsync();
            // wait
            std::this_thread::sleep_for(3ms);
        }

        return EXIT_SUCCESS;
    }

    void Connector::VNC::waitSendingFBUpdate(void) const
    {
        while(fbUpdateProcessing || ! jobsEncodings.empty())
        {
            std::this_thread::sleep_for(3ms);
        }
    }

    void Connector::VNC::clientSetPixelFormat(void)
    {
        waitSendingFBUpdate();

        // RFB: 6.4.1
        RFB::PixelFormat pf;
        // skip padding
        recvSkip(3);
        pf.bitsPerPixel = recvInt8();
        pf.depth = recvInt8();
        pf.bigEndian = recvInt8();
        pf.trueColor = recvInt8();
        pf.redMax = recvIntBE16();
        pf.greenMax = recvIntBE16();
        pf.blueMax = recvIntBE16();
        pf.redShift = recvInt8();
        pf.greenShift = recvInt8();
        pf.blueShift = recvInt8();
        // skip padding
        recvSkip(3);

        Application::info("RFB 6.4.1, set pixel format, bpp: %d, depth: %d, be: %d, truecol: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                          pf.bitsPerPixel, pf.depth, pf.bigEndian, pf.trueColor,
                          pf.redMax, pf.redShift, pf.greenMax,
                          pf.greenShift, pf.blueMax, pf.blueShift);

        switch(pf.bytePerPixel())
        {
            case 4:
            case 2:
            case 1:
                break;

            default:
                throw std::string("unknown client pixel format");
        }

	if(pf.trueColor == 0 || pf.redMax == 0 || pf.greenMax == 0 || pf.blueMax == 0)
            throw std::string("unsupported pixel format");

        clientFormat = pf;
        if(colourMap.size()) colourMap.clear();
    }

    bool Connector::VNC::clientSetEncodings(void)
    {
        waitSendingFBUpdate();

        // RFB: 6.4.2
        // skip padding
        recvSkip(1);

	int previousType = prefEncodings.second;
        int numEncodings = recvIntBE16();
        Application::info("RFB 6.4.2, set encodings, counts: %d", numEncodings);

        clientEncodings.clear();
        clientEncodings.reserve(numEncodings);

        while(0 < numEncodings--)
        {
            int encoding = recvIntBE32();

            if(! disabledEncodings.empty())
            {
                auto enclower = Tools::lower(RFB::encodingName(encoding));

                if(std::any_of(disabledEncodings.begin(), disabledEncodings.end(),
                               [&](auto & str) { return enclower == Tools::lower(str); }))
                {
                    Application::info("RFB request encodings: %s (disabled)", RFB::encodingName(encoding));
                    continue;
                }
            }

            clientEncodings.push_back(encoding);
            const char* name = RFB::encodingName(encoding);

            if(0 == std::strcmp(name, "unknown"))
                Application::debug("RFB request encodings: 0x%08x", encoding);
            else
                Application::debug("RFB request encodings: %s", RFB::encodingName(encoding));
        }

        prefEncodings = selectEncodings();
	Application::info("server select encoding: %s", RFB::encodingName(prefEncodings.second));

	return previousType != prefEncodings.second;
    }

    bool Connector::VNC::clientFramebufferUpdate(void)
    {
        // RFB: 6.4.3
        int incremental = recvInt8();
        clientRegion.x = recvIntBE16();
        clientRegion.y = recvIntBE16();
        clientRegion.w = recvIntBE16();
        clientRegion.h = recvIntBE16();
        Application::debug("RFB 6.4.3, request update fb, region [%d, %d, %d, %d], incremental: %d",
                           clientRegion.x, clientRegion.y, clientRegion.w, clientRegion.h, incremental);
        bool fullUpdate = incremental == 0;

        if(fullUpdate)
            clientRegion = serverRegion;
        else
        {
            clientRegion = serverRegion.intersected(clientRegion);

            if(clientRegion.empty())
                Application::error("client region intersection with display [%d, %d] failed", serverRegion.w, serverRegion.h);
        }

        return fullUpdate;
    }

    void Connector::VNC::clientKeyEvent(void)
    {
        // RFB: 6.4.4
        int pressed = recvInt8();
        recvSkip(2);
        int keysym = recvIntBE32();
        Application::debug("RFB 6.4.4, key event (%s), keysym: 0x%04x", (pressed ? "pressed" : "released"), keysym);

        if(! _xcbDisableMessages)
        {
            auto keyCodes = _xcbDisplay->keysymToKeycodes(keysym);
            // no wait xcb replies
            std::thread([=]()
            {
                _xcbDisplay->fakeInputKeysym(0 < pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keyCodes);
            }).detach();

            if(0 < pressed)
                pressedKeys.push_back(keyCodes);
            else
                pressedKeys.remove(keyCodes);
        }
    }

    void Connector::VNC::clientPointerEvent(void)
    {
        // RFB: 6.4.5
        int mask = recvInt8();
        int posx = recvIntBE16();
        int posy = recvIntBE16();
        Application::debug("RFB 6.4.5, pointer event, mask: 0x%02x, posx: %d, posy: %d", mask, posx, posy);

        if(! _xcbDisableMessages)
        {
            // no wait xcb replies
            std::thread([=]()
            {
                if(this->pressedMask ^ mask)
                {
                    for(int num = 0; num < 8; ++num)
                    {
                        int bit = 1 << num;

                        if(bit & mask)
                        {
			    if(1 < encodingDebug)
                        	Application::debug("xfb fake input pressed: %d", num + 1);
                            _xcbDisplay->fakeInputMouse(XCB_BUTTON_PRESS, num + 1, posx, posy);
                            this->pressedMask |= bit;
                        }
                        else if(bit & pressedMask)
                        {
			    if(1 < encodingDebug)
                        	Application::debug("xfb fake input released: %d", num + 1);
                            _xcbDisplay->fakeInputMouse(XCB_BUTTON_RELEASE, num + 1, posx, posy);
                            this->pressedMask &= ~bit;
                        }
                    }
                }
                else
                {
		    if(1 < encodingDebug)
                	Application::debug("xfb fake input move, posx: %d, posy: %d", posx, posy);
                    _xcbDisplay->fakeInputMouse(XCB_MOTION_NOTIFY, 0, posx, posy);
                }
            }).detach();
        }
    }

    void Connector::VNC::clientCutTextEvent(void)
    {
        // RFB: 6.4.6

        // skip padding
        recvSkip(3);
        size_t length = recvIntBE32();

        Application::debug("RFB 6.4.6, cut text event, length: %d", length);

        if(! _xcbSelectionOwner)
        {
            std::string addr = std::string(":").append(std::to_string(_display));
            _xcbSelectionOwner.reset(new XCB::SelectionOwner(addr));
        }

        std::string buffer;
	size_t maxReq = _xcbSelectionOwner->getMaxRequest();
        size_t limit = std::min(length, maxReq);

        buffer.reserve(limit);

        if(limit < length)
	    Application::error("request limited: %d", maxReq);

        while(0 < length--)
        {
            int ch = recvInt8();

            if(buffer.size() < limit)
                buffer.append(1, ch);
        }

        _xcbSelectionOwner->setClipboard(buffer);
    }

    void Connector::VNC::clientDisconnectedEvent(void)
    {
        Application::debug("%s", "RFB disconnected");

        if(! _xcbDisableMessages)
	    xcbReleaseInputsEvent();

        if(_xcbSelectionOwner)
            _xcbSelectionOwner->stopEvent();
    }

    void Connector::VNC::serverSendColourMap(int first)
    {
        const std::lock_guard<std::mutex> lock(sendGlobal);
        Application::debug("server send: colour map, first: %d, colour map length: %d", first, colourMap.size());
        // RFB: 6.5.2
        sendInt8(RFB::SERVER_SET_COLOURMAP);
        sendInt8(0); // padding
        sendIntBE16(first); // first color
        sendIntBE16(colourMap.size());

        for(auto & col : colourMap)
        {
            sendIntBE16(col.r);
            sendIntBE16(col.g);
            sendIntBE16(col.b);
        }

	sendFlush();
    }

    void Connector::VNC::serverSendBell(void)
    {
        const std::lock_guard<std::mutex> lock(sendGlobal);
        Application::debug("server send: %s", "bell");
        // RFB: 6.5.3
        sendInt8(RFB::SERVER_BELL);
	sendFlush();
    }

    void Connector::VNC::serverSendCutText(const std::string & text)
    {
        const std::lock_guard<std::mutex> lock(sendGlobal);
        Application::debug("server send: cut text, length: %d", text.size());
        // RFB: 6.5.4
        sendInt8(RFB::SERVER_CUT_TEXT);
        sendInt8(0); // padding
        sendIntBE32(text.size());

        for(auto & ch : text)
            sendInt8(ch);

	sendFlush();
    }

    void Connector::VNC::renderPrimitivesTo(const RFB::Region & reg, RFB::FrameBuffer & fb)
    {
        for(auto & ptr : _renderPrimitives)
        {
            switch(ptr->type)
            {
                case RenderType::RenderRect:
                    if(auto prim = static_cast<RenderRect*>(ptr.get()))
                    {
                        RFB::Region section;

                        if(RFB::Region::intersection(reg, prim->region, & section))
                        {
                            if(prim->fill)
                                fb.fillColor(section, prim->color);
                            else
                                fb.drawRect(section, prim->color);
                        }
                    }

                    break;

                case RenderType::RenderText:
                    if(auto prim = static_cast<RenderText*>(ptr.get()))
                    {
                        if(RFB::Region::intersection(reg, prim->region, nullptr))
                            fb.renderText(prim->text, prim->color, std::get<0>(prim->region) - reg.x, std::get<1>(prim->region) - reg.y);
                    }

                    break;

                default:
                    break;
            }
        }
    }

    void Connector::VNC::serverSendFrameBufferUpdate(const RFB::Region & reg)
    {
        const std::lock_guard<std::mutex> lock(sendGlobal);
        auto reply = _xcbDisplay->copyRootImageRegion(_shmInfo, reg.x, reg.y, reg.w, reg.h);

        if(reply.first)
        {
            const int bytePerPixel = _xcbDisplay->bitsPerPixel() >> 3;
            int alignRow = 0;

            if(encodingDebug)
            {
                if(const xcb_visualtype_t* visual = _xcbDisplay->findVisual(reply.second.visual))
                {
                    Application::debug("shm request size [%d, %d], reply: length: %d, depth: %d, bpp: %d, bits per rgb value: %d, red: %08x, green: %08x, blue: %08x, color entries: %d",
                                       reg.w, reg.h, reply.second.size, reply.second.depth, _xcbDisplay->bitsPerPixel(reply.second.depth), visual->bits_per_rgb_value, visual->red_mask, visual->green_mask, visual->blue_mask, visual->colormap_entries);
                }
            }

            // fix align
            if(reply.second.size > (reg.w * reg.h * bytePerPixel))
                alignRow = reply.second.size / (reg.h * bytePerPixel) - reg.w;

            Application::debug("server send fb update: [%d, %d, %d, %d]", reg.x, reg.y, reg.w, reg.h);
            // RFB: 6.5.1
            sendInt8(RFB::SERVER_FB_UPDATE);
            // padding
            sendInt8(0);
            RFB::FrameBuffer shmFrameBuffer(_shmInfo->addr, reg.w + alignRow, reg.h, serverFormat);
            // check render primitives
            renderPrimitivesTo(reg, shmFrameBuffer);
            // send encodings
            int encodingLength = prefEncodings.first(reg, shmFrameBuffer);

            if(encodingDebug)
            {
                int rawLength = 14 /* raw header for one region */ + reg.w * reg.h * clientFormat.bytePerPixel();
                float optimize = 100.0f - encodingLength * 100 / static_cast<float>(rawLength);
                Application::debug("encoding %s optimize: %.*f%% (send: %d, raw: %d), region(%d, %d)", RFB::encodingName(prefEncodings.second), 2, optimize, encodingLength, rawLength, reg.w, reg.h);
            }

            _xcbDisplay->damageSubtrack(_damageInfo, reg.x, reg.y, reg.w, reg.h);
	    sendFlush();
        }
        else
            Application::error("xcb call error: %s", "copyRootImageRegion");

        fbUpdateProcessing = false;
    }

    int Connector::VNC::sendPixel(int pixel)
    {
        // break connection
        if(! loopMessage)
            return 0;

        if(clientFormat.trueColor)
        {
            switch(clientFormat.bytePerPixel())
            {
                case 4:
                    sendInt32(clientFormat.convertFrom(serverFormat, pixel));
                    return 4;

                case 2:
                    sendInt16(clientFormat.convertFrom(serverFormat, pixel));
                    return 2;

                case 1:
                    sendInt8(clientFormat.convertFrom(serverFormat, pixel));
                    return 1;

                default:
                    break;
            }
        }
        else if(colourMap.size())
            Application::error("%s", "not usable");

        throw std::string("send pixel: unknown format");
        return 0;
    }

    int Connector::VNC::sendCPixel(int pixel)
    {
        // break connection
        if(! loopMessage)
            return 0;

        if(clientFormat.trueColor && clientFormat.bitsPerPixel == 32)
        {
            int pixel2 = clientFormat.convertFrom(serverFormat, pixel);

    	    int red = clientFormat.red(pixel2);
    	    int green = clientFormat.green(pixel2);
    	    int blue = clientFormat.blue(pixel2);

#ifdef __ORDER_LITTLE_ENDIAN__
	    std::swap(red, blue);
#endif
    	    sendInt8(red);
            sendInt8(green);
            sendInt8(blue);
            return 3;
        }

	return sendPixel(pixel);
    }

    void Connector::VNC::xcbReleaseInputsEvent(void)
    {
        // send release pointer event
        if(pressedMask)
        {
            for(int num = 0; num < 8; ++num)
            {
                int bit = 1 << num;
                if(bit & pressedMask)
                {
                    Application::debug("xfb fake input released: %d", num + 1);
                    _xcbDisplay->fakeInputMouse(XCB_BUTTON_RELEASE, num + 1, 0, 0);
                    pressedMask &= ~bit;
                }
            }
        }

        // send release key codes
        if(! pressedKeys.empty())
        {
            for(auto & keyCodes : pressedKeys)
                _xcbDisplay->fakeInputKeysym(XCB_KEY_RELEASE, keyCodes);

            pressedKeys.clear();
        }
    }

    void Connector::VNC::onLoginSuccess(const int32_t & display, const std::string & userName)
    {
        if(0 < _display && display == _display)
        {
	    xcbReleaseInputsEvent();

            _xcbDisableMessages = true;
            waitSendingFBUpdate();

            SignalProxy::onLoginSuccess(display, userName);
            // full update
            joinRegion.join(serverRegion);
        }
    }

    void Connector::VNC::onShutdownConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            _xcbDisableMessages = true;
            waitSendingFBUpdate();

            loopMessage = false;
            Application::info("dbus signal: shutdown connector, display: %d", display);
        }
    }

    void Connector::VNC::onHelperWidgetStarted(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: helper started, display: %d", display);
            loopMessage = true;
        }
    }

    void Connector::VNC::onSendBellSignal(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: send bell, display: %d", display);
            serverSendBell();
        }
    }
}
