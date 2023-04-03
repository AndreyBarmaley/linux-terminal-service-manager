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

#ifndef _LTSM_VNC2SDL_
#define _LTSM_VNC2SDL_

#include <chrono>
#include <string>
#include <memory>
#include <atomic>
#include <stdexcept>

#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "ltsm_sdl_wrapper.h"
#include "ltsm_xcb_wrapper.h"

#ifdef LTSM_RUTOKEN
#include "pki-core/common.h"
#endif

#define LTSM_VNC2SDL_VERSION 20221115

namespace LTSM
{
    struct token_error : public std::runtime_error
    {
        explicit token_error(const std::string & what) : std::runtime_error(what){}
        explicit token_error(const char* what) : std::runtime_error(what){}
    };

    class TokenAuthInterface
    {
    protected:
        virtual void attachedDevice(const std::string & serial) = 0;
        virtual void detachedDevice(const std::string & serial) = 0;

    public:
        TokenAuthInterface() = default;
        virtual ~TokenAuthInterface() = default;


        virtual std::vector<uint8_t> decryptPkcs7(const std::string & serial, const std::string & pin, int cert, const std::vector<uint8_t> & pkcs7) = 0;
    };

#ifdef LTSM_RUTOKEN
    class RutokenWrapper : public TokenAuthInterface
    {
        std::thread thscan;
        std::atomic<bool> shutdown{false};
        ChannelClient* sender = nullptr;

    protected:
        void attachedDevice(const std::string & serial) override;
        void detachedDevice(const std::string & serial) override;

    public:
        RutokenWrapper(const std::string &, ChannelClient &);
        ~RutokenWrapper();

        std::vector<uint8_t> decryptPkcs7(const std::string & serial, const std::string & pin, int cert, const std::vector<uint8_t> & pkcs7) override;
    };
#endif

    struct ColorCursor
    {
        std::vector<uint8_t> pixels;
        std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> surface { nullptr, SDL_FreeSurface };
        std::unique_ptr<SDL_Cursor, void(*)(SDL_Cursor*)> cursor { nullptr, SDL_FreeCursor };
    };

    class Vnc2SDL : public Application, public XCB::XkbClient, protected RFB::ClientDecoder
    {
        PixelFormat             clientPf;
        RFB::SecurityInfo       rfbsec;

        std::list<std::string>  dropFiles;
        std::string             host{"localhost"};
        std::string             username, seamless, share, tokenLib;
        std::string             printerUrl, pcscdUrl, pulseUrl, saneUrl;

        std::unique_ptr<SDL::Window> window;
        std::unique_ptr<FrameBuffer> fb;
        std::unique_ptr<TokenAuthInterface> token;

        std::unordered_map<uint32_t, ColorCursor> cursors;

        XCB::Region             dirty;
        std::mutex              renderLock;

        std::chrono::time_point<std::chrono::steady_clock>
                                keyPress;
        std::chrono::time_point<std::chrono::steady_clock>
                                dropStart;

        std::atomic<bool>       focusLost = false;

        int                     port = 5900;

        BinaryBuf               clipboardBufRemote;
        BinaryBuf               clipboardBufLocal;
        std::mutex              clipboardLock;

        uint16_t                setWidth = 0;
        uint16_t                setHeight = 0;

        bool                    accelerated = true;
        bool                    fullscreen = false;
        bool                    usexkb = true;
        bool                    capslock = true;
        bool                    sendOptions = false;
        bool                    alwaysRunning = false;
	bool			x264Decoding = false;

    protected:
        void                    setPixel(const XCB::Point &, uint32_t pixel) override;
        void                    fillPixel(const XCB::Region &, uint32_t pixel) override;
	void    		updateRawPixels(const void*, size_t width, size_t height, uint16_t pitch, int bpp, uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask) override;
        const PixelFormat &     clientFormat(void) const override;
        XCB::Size               clientSize(void) const override;
	bool			clientX264(void) const override;

        bool                    sdlEventProcessing(const SDL::GenericEvent &);
        int                     startSocket(std::string_view host, int port) const;
        void                    sendMouseState(void);
        void                    exitEvent(void);

        json_plain             clientOptions(void) const;
        json_plain             clientEnvironments(void) const;

    public:
        Vnc2SDL(int argc, const char** argv);

        void                    decodingExtDesktopSizeEvent(uint16_t status, uint16_t err, const XCB::Size & sz, const std::vector<RFB::ScreenInfo> &) override;
        void                    pixelFormatEvent(const PixelFormat &, uint16_t width, uint16_t height) override;
        void                    fbUpdateEvent(void) override;
        void                    cutTextEvent(std::vector<uint8_t> &&) override;
        void                    richCursorEvent(const XCB::Region & reg, std::vector<uint8_t> && pixels, std::vector<uint8_t> && mask) override;

        void                    xkbStateChangeEvent(int) override;
        void                    ltsmHandshakeEvent(int flags) override;
        void                    systemFuseProxy(const JsonObject &) override;
        void                    systemTokenAuth(const JsonObject &) override;
        void                    systemLoginSuccess(const JsonObject &) override;

        int    		        start(void) override;
        bool                    isAlwaysRunning(void) const;
    };
}

#endif // _LTSM_VNC2SDL_
