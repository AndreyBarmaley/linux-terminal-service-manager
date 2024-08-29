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
#include <forward_list>

#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "ltsm_sdl_wrapper.h"
#include "ltsm_xcb_wrapper.h"

#define LTSM_VNC2SDL_VERSION 20240304

namespace LTSM
{
    struct ColorCursor
    {
        std::vector<uint8_t> pixels;
        std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> surface { nullptr, SDL_FreeSurface };
        std::unique_ptr<SDL_Cursor, void(*)(SDL_Cursor*)> cursor { nullptr, SDL_FreeCursor };
    };

    struct SurfaceDeleter
    {
        void operator()(SDL_Surface* sf)
        {
            if(sf)
            {
                SDL_FreeSurface(sf);
            }
        }
    };

    class Vnc2SDL : public Application, public XCB::XkbClient,
        protected RFB::ClientDecoder
    {
        PixelFormat clientPf;
        RFB::SecurityInfo rfbsec;

        std::forward_list<std::string> dropFiles;
        std::forward_list<std::string> shareFolders;

        std::string host{"localhost"};
        std::string username, seamless, pkcs11Auth;
        std::string printerUrl, saneUrl;
        std::string prefferedEncoding;
        std::string audioEncoding{"auto"};
        std::string passfile;

        std::unique_ptr<SDL::Window> window;
        std::unique_ptr<SDL_Surface, SurfaceDeleter> sfback;

        std::unordered_map<uint32_t, ColorCursor> cursors;

        XCB::Size windowSize;
        std::mutex renderLock;

        std::chrono::time_point<std::chrono::steady_clock>
        keyPress;
        std::chrono::time_point<std::chrono::steady_clock>
        dropStart;

        std::atomic<bool> focusLost{false};
        std::atomic<bool> needUpdate{false};

        int port = 5900;
        int frameRate = 16;

        BinaryBuf clipboardBufRemote;
        BinaryBuf clipboardBufLocal;
        std::mutex clipboardLock;

        XCB::Size setGeometry;
        SDL_Event sdlEvent;

        bool ltsmSupport = true;
        bool accelerated = true;
        bool nodamage = false;
        bool fullscreen = false;
        bool usexkb = true;
        bool capslock = true;
        bool sendOptions = false;
        bool alwaysRunning = false;
        bool serverExtDesktopSizeSupported = false;
        bool audioEnable = false;
        bool pcscEnable = false;

    protected:
        void setPixel(const XCB::Point &, uint32_t pixel) override;
        void fillPixel(const XCB::Region &, uint32_t pixel) override;
        void updateRawPixels(const void*, const XCB::Size &, uint16_t pitch, const PixelFormat &) override;
        const PixelFormat & clientFormat(void) const override;
        XCB::Size clientSize(void) const override;
        std::string clientEncoding(void) const override;

        bool sdlEventProcessing(void);
        bool pushEventWindowResize(const XCB::Size &);
        int startSocket(std::string_view host, int port) const;
        void sendMouseState(void);
        void exitEvent(void);

        json_plain clientOptions(void) const;
        json_plain clientEnvironments(void) const;

    public:
        Vnc2SDL(int argc, const char** argv);

        void decodingExtDesktopSizeEvent(int status, int err, const XCB::Size & sz,
                                         const std::vector<RFB::ScreenInfo> &) override;
        void pixelFormatEvent(const PixelFormat &, const XCB::Size &) override;
        void fbUpdateEvent(void) override;
        void cutTextEvent(std::vector<uint8_t> &&) override;
        void richCursorEvent(const XCB::Region & reg, std::vector<uint8_t> && pixels,
                             std::vector<uint8_t> && mask) override;
        void bellEvent(void) override;

        void xkbStateChangeEvent(int) override;
        void ltsmHandshakeEvent(int flags) override;
        void systemLoginSuccess(const JsonObject &) override;

        const char* pkcs11Library(void) const override;
        bool createChannelAllow(const Channel::ConnectorType &, const std::string &,
                                const Channel::ConnectorMode &) const override;
        bool ltsmSupported(void) const override
        {
            return ltsmSupport;
        }

        int start(void) override;
        bool isAlwaysRunning(void) const;
    };
}

#endif // _LTSM_VNC2SDL_
