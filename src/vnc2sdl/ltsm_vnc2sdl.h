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

#ifndef _LTSM_VNC2SDL_
#define _LTSM_VNC2SDL_

#include <chrono>
#include <thread>
#include <string>
#include <memory>
#include <atomic>

#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "ltsm_sdl_wrapper.h"
#include "ltsm_xcb_wrapper.h"

#define LTSM_VNC2SDL_VERSION 20221004

namespace LTSM
{
    class Vnc2SDL : public Application, XCB::XkbClient, protected RFB::ClientDecoder
    {
        PixelFormat             format;

        std::string             host{"localhost"};
        std::string             password;
        std::string             priority;
        std::string             certificate;

        std::list<std::string>  dropFiles;

        std::unique_ptr<SDL::Window> window;
        std::unique_ptr<FrameBuffer> fb;

        XCB::Region             dirty;

        std::mutex              lockRender;

        std::chrono::time_point<std::chrono::steady_clock>
                                keyPress;

        std::chrono::time_point<std::chrono::steady_clock>
                                dropStart;

        int                     port = 5900;
        uint16_t                setWidth = 0;
        uint16_t                setHeight = 0;
        bool                    accelerated = false;
        bool                    notls = false;
        bool                    fullscreen = false;
        bool                    printer = false;
        bool                    audio = false;
#ifdef LTSM_CHANNELS
        bool                    sendOptions = false;
#endif
        std::atomic<bool>       focusLost = false;

    protected:
        void                    setPixel(const XCB::Point &, uint32_t pixel) override;
        void                    fillPixel(const XCB::Region &, uint32_t pixel) override;
        const PixelFormat &     clientPixelFormat(void) const override;
        XCB::Size               clientSize(void) const override;

        bool                    sdlEventProcessing(const SDL::GenericEvent &);
        int                     startSocket(std::string_view host, int port) const;
        void                    sendMouseState(void);

        json_plain             clientOptions(void) const;
        json_plain             clientEnvironments(void) const;

    public:
        Vnc2SDL(int argc, const char** argv);

        void                    decodingExtDesktopSizeEvent(uint16_t status, uint16_t err, const XCB::Size & sz, const std::vector<RFB::ScreenInfo> &) override;
        void                    pixelFormatEvent(const PixelFormat &, uint16_t width, uint16_t height) override;
        void                    fbUpdateEvent(void) override;
        void                    cutTextEvent(std::vector<uint8_t> &&) override;
#ifdef LTSM_CHANNELS
        void                    xkbStateChangeEvent(int) override;
        void                    decodingLtsmEvent(const std::vector<uint8_t> &) override;
#endif
        int    		        start(void) override;
    };
}

#endif // _LTSM_VNC2SDL_
