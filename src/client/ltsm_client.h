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

#ifndef _LTSM_CLIENT_APP_
#define _LTSM_CLIENT_APP_

#include <chrono>
#include <string>
#include <memory>
#include <thread>
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

#define LTSM_CLIENT_VERSION 20260412

namespace LTSM {
    struct ColorCursor {
        std::vector<uint8_t> pixels;
        std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> surface { nullptr, SDL_FreeSurface };
        std::unique_ptr<SDL_Cursor, void(*)(SDL_Cursor*)> cursor { nullptr, SDL_FreeCursor };
    };

    class BoostContext {
        const int concurency_ = 2;
        boost::asio::io_context ioc_{concurency_};

      protected:
        inline boost::asio::io_context & ioc(void) { return ioc_; }
        inline size_t concurency(void) const { return concurency_; }
        boost::asio::any_io_executor get_executor(void) { return ioc_.get_executor(); }

      public:
        BoostContext() = default;
        ~BoostContext() = default;
    };

    class ClientApp : public BoostContext, public Application,
#ifdef LTSM_WITH_X11
        public RFB::X11Client
#else
        public RFB::WinClient
#endif
    {
        boost::asio::signal_set signals_;
        boost::asio::cancellation_signal rfb_cancel_;
        boost::asio::strand<boost::asio::any_io_executor> sdl_strand_;
        boost::asio::cancellation_signal sdl_cancel_;
#ifdef __UNIX__
        boost::asio::strand<boost::asio::any_io_executor> x11_strand_;
        boost::asio::cancellation_signal x11_cancel_;
#endif
        std::unordered_map<uint32_t, ColorCursor> cursors;

        PixelFormat clientPf;
        RFB::SecurityInfo rfbsec;

        std::forward_list<std::string> dropFiles;
        std::forward_list<std::string> shareFolders;
        std::forward_list<std::string> videoEncodingOptions;
        std::forward_list<std::string> audioEncodingOptions;

        std::string host{"localhost"};
        std::string username, seamless, pkcs11Auth;
        std::string printerUrl, saneUrl;
        std::string passfile;

        int videoEncoding = 0;
        int audioEncoding = 0;

        std::unique_ptr<SDL::Window> window;
        XCB::Size windowSize;

        std::chrono::time_point<std::chrono::steady_clock> appStart;
        std::atomic<bool> focusLost{false};

        int xcbDpi = 0;
        int port = 5900;
        int frameRate = 0;
        int windowFlags = SDL_WINDOW_SHOWN;

        //        BinaryBuf clipboardBufRemote;
        //        BinaryBuf clipboardBufLocal;
        //        std::mutex clipboardLock;

        XCB::Size primarySize;

        bool ltsmSupport = true;
        bool windowAccel = true;
        bool xcbNoDamage = false;
        bool useXkb = true;
        bool alwaysRunning = false;
        bool serverExtDesktopSizeNego = false;
        bool capslockEnable = true;
        bool audioEnable = false;
        bool pcscEnable = false;

      protected:
        void setPixel(const XCB::Point &, uint32_t pixel) override;
        void fillPixel(const XCB::Region &, uint32_t pixel) override;
        void updateRawPixels(const XCB::Region &, std::vector<uint8_t>&&, uint32_t pitch, const PixelFormat &) override;
        void updateRawPixels2(const XCB::Region &, std::vector<uint8_t>&&, uint32_t pitch, uint32_t sdlFormat) override;
        const PixelFormat & clientFormat(void) const override;
        XCB::Size clientSize(void) const override;

        int clientPrefferedVideoEncoding(void) const override;
        int clientPrefferedAudioEncoding(void) const override;

        bool pushEventWindowResize(const XCB::Size &);
        int startSocket(std::string_view host, int port) const;

        json_plain clientOptions(void) const;
        json_plain clientEnvironments(void) const;

        bool windowFullScreen(void) const;
        bool windowResizable(void) const;

        void parseCommand(std::string_view cmd, std::string_view arg);
        void loadConfig(const std::filesystem::path &);
        void updateSecurity(void);

#ifdef __UNIX__
        boost::asio::awaitable<void> x11EventsLoop(void);
#endif
        boost::asio::awaitable<void> signalsHandler(void);
        boost::asio::awaitable<void> windowResizedEvent(const XCB::Size &);
        boost::asio::awaitable<void> sdlEventsLoop(void);
        boost::asio::awaitable<bool> sdlEventProcessing(void);
        boost::asio::awaitable<void> sdlMouseMotion(SDL_Event &&);
        boost::asio::awaitable<void> sdlMouseButton(SDL_Event &&);
        boost::asio::awaitable<void> sdlMouseWheel(SDL_Event &&);
        boost::asio::awaitable<void> sdlWindowEvent(SDL_Event &&);
        boost::asio::awaitable<void> sdlKeyboardEvent(SDL_Event &&);
        boost::asio::awaitable<void> sdlDropCompleteEvent(SDL_Event &&);
        boost::asio::awaitable<void> sdlUserEvent(SDL_Event &&);

        void sdlRenderFrame(void) const;
        void stop(void);

      public:
        ClientApp(int argc, const char** argv);

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
        bool clientLtsmSupported(void) const override {
            return ltsmSupport;
        }

        uint32_t frameRateOption(void) const override {
            return frameRate;
        }

        int start(void);
        bool isAlwaysRunning(void) const;
    };
}

#endif // _LTSM_CLIENT_APP_
