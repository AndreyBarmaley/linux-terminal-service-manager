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

#include "SDL_image.h"
#include "xcb/damage.h"

#include "ltsm_tools.h"
#include "ltsm_sdl_wrapper.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    template<typename Rect>
    Rect joinRects(std::list<Rect> & rects)
    {
        Rect around = rects.front();
        std::for_each(std::next(rects.begin()), rects.end(), [&](auto & rt)
        {
            if(rt.x < around.x)
            {
                around.w += around.x - rt.x;
                around.x = rt.x;
            }
            else if(rt.x + rt.w > around.x + around.w)
                around.w = rt.x + rt.w - around.x;

            if(rt.y < around.y)
            {
                around.h += around.y - rt.y;
                around.y = rt.y;
            }
            else if(rt.y + rt.h > around.y + around.h)
                around.h = rt.y + rt.h - around.y;
        });
        return around;
    }

    class SDL2X11 : protected XCB::RootDisplayExt, protected SDL::Window
    {
        SDL::Texture    txShadow;
	std::vector<uint8_t> selbuf;
        const int       txFormat;

    public:
        SDL2X11(int display, const std::string & title, int winsz_w, int winsz_h, bool accel)
            : XCB::RootDisplayExt(std::string(":").append(std::to_string(display))), SDL::Window(title.c_str(), width(), height(), winsz_w, winsz_h, accel),
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
            txFormat(SDL_PIXELFORMAT_ARGB32)
#else
            txFormat(SDL_PIXELFORMAT_BGRA32)
#endif
        {
            txShadow = createTexture(width(), height(), txFormat);
        }

        bool fakeInputKeysym(int type, const SDL_Keysym & keysym) const
        {
            int xksym = SDL::Window::convertScanCodeToKeySym(keysym.scancode);
            auto keycodes = XCB::RootDisplayExt::keysymToKeycodes(0 != xksym ? xksym : keysym.sym);
            return XCB::RootDisplayExt::fakeInputKeysym(type, keycodes);
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

                    fakeInputKeysym(XCB_KEY_PRESS, ev.key()->keysym);
                    break;

                case SDL_KEYUP:
                    fakeInputKeysym(XCB_KEY_RELEASE, ev.key()->keysym);
                    break;

                case SDL_MOUSEBUTTONDOWN:
                {
                    std::pair<int, int> coord = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
                    fakeInputMouse(XCB_BUTTON_PRESS, ev.button()->button, coord.first, coord.second);
                }
                break;

                case SDL_MOUSEBUTTONUP:
                {
                    std::pair<int, int> coord = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
                    fakeInputMouse(XCB_BUTTON_RELEASE, ev.button()->button, coord.first, coord.second);
                }
                break;

                case SDL_MOUSEMOTION:
                {
                    std::pair<int, int> coord = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
                    fakeInputMouse(XCB_MOTION_NOTIFY, 0, coord.first, coord.second);
                }
                break;

                case SDL_MOUSEWHEEL:
                    if(ev.wheel()->y > 0)
                    {
                        int posx, posy;
                        SDL_GetMouseState(& posx, &posy);
                        fakeInputMouse(XCB_BUTTON_PRESS, 4, posx, posy);
                        fakeInputMouse(XCB_BUTTON_RELEASE, 4, posx, posy);
                    }
                    else if(ev.wheel()->y < 0)
                    {
                        int posx, posy;
                        SDL_GetMouseState(& posx, &posy);
                        fakeInputMouse(XCB_BUTTON_PRESS, 5, posx, posy);
                        fakeInputMouse(XCB_BUTTON_RELEASE, 5, posx, posy);
                    }

                    break;

		case SDL_CLIPBOARDUPDATE:
		    if(SDL_HasClipboardText())
		    {
			if(char* ptr = SDL_GetClipboardText())
			{
                            int len = std::strlen(ptr);
			    selbuf.assign(ptr, ptr + len);
			    XCB::RootDisplayExt::setClipboardEvent(selbuf);
			    SDL_free(ptr);
			}
		    }
		    break;

                case SDL_QUIT:
                    throw std::string("sdl quit");
                    break;

                default:
                    break;
            }

            return true;
        }

        int start(void)
        {
            const int bytePerPixel = bitsPerPixel() >> 3;
            bool quit = false;
            XCB::Region damage;

            while(! quit && ! XCB::RootDisplayExt::hasError())
            {
                bool delay = ! sdlEventProcessing(quit);

        	while(auto ev = XCB::RootDisplayExt::poolEvent())
		{
        	    if(XCB::RootDisplayExt::isDamageNotify(ev))
        	    {
            		const xcb_damage_notify_event_t* notify = (xcb_damage_notify_event_t*) ev.get();
                	damage.join(notify->area.x, notify->area.y, notify->area.width, notify->area.height);
        	    }
                    else
                    if(XCB::RootDisplayExt::isRandrCRTCNotify(ev))
                    {
                        auto notify = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());

                        xcb_randr_crtc_change_t cc = notify->u.cc;
                        if(0 < cc.width && 0 < cc.height)
                        {
                            SDL::Window::resize(cc.width, cc.height);
                	    damage.assign(0, 0, cc.width, cc.height);
        		    txShadow = createTexture(cc.width, cc.height, txFormat);
                        }
                    }
                    else
                    if(XCB::RootDisplayExt::isSelectionNotify(ev))
                    {
                        auto notify = reinterpret_cast<xcb_selection_notify_event_t*>(ev.get());
                        if(XCB::RootDisplayExt::selectionNotifyAction(notify))
                        {
			    auto & selbuf = XCB::RootDisplayExt::getSelectionData();
			    SDL_SetClipboardText(std::string(selbuf.begin(), selbuf.end()).c_str());
			}
                    }
                }

                if(! damage.empty())
                {
                    delay = false;
                    if(auto reply = copyRootImageRegion(damage))
                    {
                        const int alignRowBytes = reply->size() > (damage.width * damage.height * bytePerPixel) ?
                            reply->size() / damage.height - damage.width * bytePerPixel : 0;

                        const SDL_Rect rect = { damage.x, damage.y, damage.width, damage.height };
                        txShadow.updateRect(& rect, reply->data(), damage.width * bytePerPixel + alignRowBytes);
                        renderTexture(txShadow.get());
                        renderPresent();
                        XCB::RootDisplayExt::damageSubtrack(damage);
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
    std::cout << "usage: " << prog << " --auth <xauthfile> --title <title> --display <num> --scale <width>x<height> [--accel]" << std::endl;
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

    bool accelUsed = false;

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
            if(0 == std::strcmp(argv[it], "--auth") && it + 1 < argc)
                xauth.assign(argv[it + 1]);
            else if(0 == std::strcmp(argv[it], "--title") && it + 1 < argc)
                title.assign(argv[it + 1]);
            else if(0 == std::strcmp(argv[it], "--scale") && it + 1 < argc)
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
            else if(0 == std::strcmp(argv[it], "--display") && it + 1 < argc)
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
            else if(0 == std::strcmp(argv[it], "--accel"))
                accelUsed = true;
        }

        if(xauth.size())
            setenv("XAUTHORITY", xauth.c_str(), 1);
    }

    if(argc < 2 || display < 0 || xauth.empty())
        return printHelp(argv[0]);

    try
    {
        LTSM::SDL2X11 app(display, title, winsz_w, winsz_h, accelUsed);
        return app.start();
    }
    catch(int errcode)
    {
        return errcode;
    }
    catch(std::string errstr)
    {
        std::cerr << "exception: " << errstr << std::endl;
    }

    return 0;
}
