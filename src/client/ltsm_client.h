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

#ifndef _LTSM_CLIENT_
#define _LTSM_CLIENT_

#include <chrono>
#include <string>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <forward_list>

#include "ltsm_global.h"
#include "ltsm_xcb_types.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "ltsm_sdl_wrapper.h"

#ifdef LTSM_WITH_X11
 #include "librfb_x11client.h"
#else
 #include "librfb_winclient.h"
#endif

#define LTSM_VNC2SDL_VERSION 20250816

namespace LTSM
{
    struct ColorCursor
    {
        std::vector<uint8_t> pixels;
        std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> surface { nullptr, SDL_FreeSurface };
        std::unique_ptr<SDL_Cursor, void(*)(SDL_Cursor*)> cursor { nullptr, SDL_FreeCursor };
    };

    class Vnc2SDL : public Application,
#ifdef LTSM_WITH_X11
        public RFB::X11Client
#else
        public RFB::WinClient
#endif
    {
        PixelFormat clientPf;
        RFB::SecurityInfo rfbsec;

        std::forward_list<std::string> dropFiles;
        std::forward_list<std::string> shareFolders;
        std::forward_list<std::string> encodingOptions;

        std::string host{"localhost"};
        std::string username, seamless, pkcs11Auth;
        std::string printerUrl, saneUrl;
        std::string prefferedEncoding;
        std::string audioEncoding{"auto"};
        std::string passfile;

        std::unique_ptr<SDL::Window> window;
        std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> sfback { nullptr, SDL_FreeSurface };

        std::unordered_map<uint32_t, ColorCursor> cursors;

        XCB::Size windowSize;
        std::mutex renderLock;

        std::chrono::time_point<std::chrono::steady_clock> keyPress;
        std::chrono::time_point<std::chrono::steady_clock> dropStart;

        std::atomic<bool> focusLost{false};
        std::atomic<bool> needUpdate{false};

        int port = 5900;
        int frameRate = 16;
        int windowFlags = SDL_WINDOW_SHOWN; // | SDL_WINDOW_RESIZABLE;

//        BinaryBuf clipboardBufRemote;
//        BinaryBuf clipboardBufLocal;
//        std::mutex clipboardLock;

        XCB::Size primarySize;
        SDL_Event sdlEvent;

        bool ltsmSupport = true;
        bool windowAccel = true;
        bool xcbNoDamage = false;
        bool useXkb = true;
        bool sendOptions = false;
        bool alwaysRunning = false;
        bool serverExtDesktopSizeNego = false;
        bool capslockEnable = true;
        bool audioEnable = false;
        bool pcscEnable = false;

    protected:
        void setPixel(const XCB::Point &, uint32_t pixel) override;
        void fillPixel(const XCB::Region &, uint32_t pixel) override;
        void updateRawPixels(const XCB::Region &, const void*, uint32_t pitch, const PixelFormat &) override;
        void updateRawPixels2(const XCB::Region &, const void*, uint8_t depth, uint32_t pitch, uint32_t sdlFormat) override;
        void updateRawPixels3(const XCB::Region &, SDL_Surface*);
        const PixelFormat & clientFormat(void) const override;
        XCB::Size clientSize(void) const override;

        std::string clientPrefferedEncoding(void) const override;

        bool sdlEventProcessing(void);
        bool pushEventWindowResize(const XCB::Size &);
        int startSocket(std::string_view host, int port) const;

        bool sdlMouseEvent(const SDL::GenericEvent &);
        bool sdlKeyboardEvent(const SDL::GenericEvent &);
        bool sdlWindowEvent(const SDL::GenericEvent &);
        bool sdlDropFileEvent(const SDL::GenericEvent &);
        bool sdlUserEvent(const SDL::GenericEvent &);
        bool sdlQuitEvent(void);

        void windowResizedEvent(int, int);

        json_plain clientOptions(void) const;
        json_plain clientEnvironments(void) const;

        bool windowFullScreen(void) const;
        bool windowResizable(void) const;

        void parseCommand(std::string_view cmd, std::string_view arg);
        void loadConfig(const std::filesystem::path &);

    public:
        Vnc2SDL(int argc, const char** argv);

        void clientRecvDecodingDesktopSizeEvent(int status, int err, const XCB::Size & sz,
                                         const std::vector<RFB::ScreenInfo> &) override;
        void clientRecvPixelFormatEvent(const PixelFormat &, const XCB::Size &) override;
        void clientRecvFBUpdateEvent(void) override;
        //void clientRecvCutTextEvent(std::vector<uint8_t> &&) override;
        void clientRecvRichCursorEvent(const XCB::Region & reg, std::vector<uint8_t> && pixels,
                             std::vector<uint8_t> && mask) override;
        void clientRecvLtsmCursorEvent(const XCB::Region & reg, uint32_t cursorId, std::vector<uint8_t> && pixels) override;
        void clientRecvBellEvent(void) override;

#ifdef __UNIX__
        void xcbXkbGroupChangedEvent(int) override;
#endif
        void clientRecvLtsmHandshakeEvent(int flags) override;
        void systemLoginSuccess(const JsonObject &) override;

        const char* pkcs11Library(void) const override;
        bool createChannelAllow(const Channel::ConnectorType &, const std::string &,
                                const Channel::ConnectorMode &) const override;
        bool clientLtsmSupported(void) const override
        {
            return ltsmSupport;
        }

        int start(void) override;
        bool isAlwaysRunning(void) const;
    };
}

#endif // _LTSM_VNC2SDL_
