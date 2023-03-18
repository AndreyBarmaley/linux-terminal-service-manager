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
 ***************************************************************************/

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <stdexcept>

#include "xcb/damage.h"

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_sdl_wrapper.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    class SDL2X11 : protected XCB::RootDisplay, protected SDL::Window
    {
        XCB::Region damage;
        XCB::ShmIdShared shm;
        std::unique_ptr<char, void(*)(void*)> clipboard{ nullptr, SDL_free };

    public:
        SDL2X11(int display, const std::string & title, int winsz_w, int winsz_h)
            : XCB::RootDisplay(display), SDL::Window(title.c_str(), width(), height(), winsz_w, winsz_h)
        {
        }

/*
        void xcbShmInit(uid_t uid)
        {
            if(auto ext = static_cast<const XCB::ModuleShm*>(XCB::RootDisplay::getExtension(XCB::Module::SHM)))
            {
                auto dsz = XCB::RootDisplay::size();
                auto bpp = XCB::RootDisplay::bitsPerPixel() >> 3;

                shm = ext->createShm(dsz.width * dsz.height * bpp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, false, uid);
            }
        }
*/

        bool sdlFakeInputTest(int type, const SDL_Keysym & keysym)
        {
            if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST)))
            {
                int xksym = SDL::Window::convertScanCodeToKeySym(keysym.scancode);
                auto keycode = XCB::RootDisplay::keysymToKeycode(0 != xksym ? xksym : keysym.sym);
                return keycode != XCB_NO_SYMBOL ? test->fakeInputRaw(root(), type, keycode, 0, 0) : false;
            }
            return false;
        }

        void xfixesSelectionChangedEvent(void) override
        {
        }

        void xfixesCursorChangedEvent(void) override
        {
        }

        void damageRegionEvent(const XCB::Region & area) override
        {
            damage.join(area);
        }

        void randrScreenChangedEvent(const XCB::Size & dsz, const xcb_randr_notify_event_t & ne) override
        {
            SDL::Window::resize(dsz.width, dsz.height);
            damage.assign(0, 0, dsz.width, dsz.height);
        }

        void xkbGroupChangedEvent(int group) override
        {
        }

        void clipboardChangedEvent(const std::vector<uint8_t> & buf) override
        {
            SDL_SetClipboardText(std::string(buf.begin(), buf.end()).c_str());
        }

        bool sdlEventProcessing(bool & quit)
        {
            auto ev = SDL::Window::poolEvent();

            if(! ev.isValid()) return false;

            switch(ev.type())
            {
                case SDL_TEXTINPUT:
                    // handleTextInput: ev.text
                    break;

                case SDL_KEYDOWN:

                    // fast close
                    if(ev.key()->keysym.sym == SDLK_ESCAPE)
                    {
                        quit = true;
                        break;
                    }

                    sdlFakeInputTest(XCB_KEY_PRESS, ev.key()->keysym);
                    break;

                case SDL_KEYUP:
                    sdlFakeInputTest(XCB_KEY_RELEASE, ev.key()->keysym);
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST)))
                    {
                        auto [coordX, coordY] = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
                        test->fakeInputRaw(root(), XCB_BUTTON_PRESS, ev.button()->button, coordX, coordY);
                    }
                    break;

                case SDL_MOUSEBUTTONUP:
                    if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST)))
                    {
                        auto [coordX, coordY] = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
                        test->fakeInputRaw(root(), XCB_BUTTON_RELEASE, ev.button()->button, coordX, coordY);
                    }
                    break;

                case SDL_MOUSEMOTION:
                    if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST)))
                    {
                        auto [coordX, coordY] = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
                        test->fakeInputRaw(root(), XCB_MOTION_NOTIFY, 0, coordX, coordY);
                    }
                    break;

                case SDL_MOUSEWHEEL:
                    if(ev.wheel()->y > 0)
                    {
                        int posx, posy;
                        SDL_GetMouseState(& posx, &posy);
                        fakeInputButton(4, XCB::Point(posx, posy));
                    }
                    else
                    if(ev.wheel()->y < 0)
                    {
                        int posx, posy;
                        SDL_GetMouseState(& posx, &posy);
                        fakeInputButton(5, XCB::Point(posx, posy));
                    }

                    break;

                case SDL_CLIPBOARDUPDATE:
                    if(SDL_HasClipboardText())
                    {
                        clipboard.reset(SDL_GetClipboardText());

                        if(clipboard)
                        {
                            auto len = SDL_strlen(clipboard.get());
                            XCB::RootDisplay::setClipboard((const uint8_t*) clipboard.get(), len);
                        }
                    }
                    break;

                case SDL_QUIT:
                    Application::warning("%s: %s", __FUNCTION__, "SDL quit event");
                    throw sdl_error(NS_FuncName);

                default:
                    break;
            }

            return true;
        }

        int start(void)
        {
            const size_t bytePerPixel = bitsPerPixel() >> 3;

            bool quit = false;

            while(! quit)
            {
                bool delay = ! sdlEventProcessing(quit);

                // processing xcb events
                while(auto ev = XCB::RootDisplay::poolEvent())
                {
                    if(auto err = XCB::RootDisplay::hasError())
                    {
                        Application::error("%s: xcb error, code: %d", __FUNCTION__, err);
                        return EXIT_SUCCESS;
                    }
                }

                if(! damage.empty())
                {
                    delay = false;

                    if(auto reply = copyRootImageRegion(damage))
                    {
                        const size_t alignRowBytes = reply->size() > (damage.width * damage.height * bytePerPixel) ?
                                                     reply->size() / damage.height - damage.width * bytePerPixel : 0;

                        SDL_Rect dstrt = { damage.x, damage.y, damage.width, damage.height };

                        auto format = SDL_MasksToPixelFormatEnum(reply->bitsPerPixel(), reply->rmask, reply->gmask, reply->bmask, 0);
                        if(SDL_PIXELFORMAT_UNKNOWN == format)
                            throw sdl_error("unknown pixel format");

                        auto tx = createTexture(damage.width, damage.height, format);
                        tx.updateRect(nullptr, reply->data(), damage.width * bytePerPixel + alignRowBytes);

                        renderTexture(tx.get(), nullptr, nullptr, & dstrt);
                        renderPresent();

                        XCB::RootDisplay::damageSubtrack(damage);
                    }

                    damage.reset();
                }

                if(delay)
                    SDL_Delay(5);
            }

            return EXIT_SUCCESS;
        }
    };
}

int printHelp(const char* prog)
{
    std::cout << "usage: " << prog << " --auth <xauthfile> --title <title> --display <num> --scale <width>x<height> [--debug] [--syslog]" << std::endl;
    return EXIT_SUCCESS;
}

int main(int argc, const char** argv)
{
    int display = -1;
    int winsz_w = 0;
    int winsz_h = 0;
    std::string xauth;
    std::string geometry;
    std::string title = "SDL2X11";

    LTSM::Application::setDebug(LTSM::DebugTarget::Console, LTSM::DebugLevel::Info);

    if(auto val = getenv("SDL2X11_SCALE"))
    {
        size_t idx;

        try
        {
            winsz_w = std::stoi(val, & idx, 0);
            winsz_h = std::stoi(val + idx + 1, nullptr, 0);
        }
        catch(const std::invalid_argument &)
        {
            std::cerr << "invalid scale" << std::endl;
            winsz_w = 0;
            winsz_h = 0;
        }
    }

    if(1 < argc)
    {
        if(0 == std::strcmp(argv[1], "--help") || 0 == std::strcmp(argv[1], "-h"))
            return printHelp(argv[0]);

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--debug"))
                LTSM::Application::setDebugLevel(LTSM::DebugLevel::Debug);
            else
            if(0 == std::strcmp(argv[it], "--syslog"))
                LTSM::Application::setDebugTarget(LTSM::DebugTarget::Syslog);
            else
            if(0 == std::strcmp(argv[it], "--auth") && it + 1 < argc)
                xauth.assign(argv[it + 1]);
            else
            if(0 == std::strcmp(argv[it], "--title") && it + 1 < argc)
                title.assign(argv[it + 1]);
            else
            if(0 == std::strcmp(argv[it], "--scale") && it + 1 < argc)
            {
                const char* val = argv[it + 1];
                size_t idx;

                try
                {
                    winsz_w = std::stoi(val, & idx, 0);
                    winsz_h = std::stoi(val + idx + 1, nullptr, 0);
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "invalid scale" << std::endl;
                    return printHelp(argv[0]);
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--display") && it + 1 < argc)
            {
                const char* val = argv[it + 1];

                try
                {
                    display = std::stoi(val[0] == ':' ? & val[1] : val, nullptr, 0);
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "invalid display" << std::endl;
                    return printHelp(argv[0]);
                }
            }
        }

        if(xauth.size())
            setenv("XAUTHORITY", xauth.c_str(), 1);
    }

    if(argc < 2 || display < 0 || xauth.empty())
        return printHelp(argv[0]);

    if(0 > SDL_Init(SDL_INIT_VIDEO))
        return -1;

    try
    {
        LTSM::SDL2X11 app(display, title, winsz_w, winsz_h);
        return app.start();
    }
    catch(const std::exception & err)
    {
        std::cerr << "exception: " << err.what() << std::endl;
    }

    SDL_Quit();

    return 0;
}
