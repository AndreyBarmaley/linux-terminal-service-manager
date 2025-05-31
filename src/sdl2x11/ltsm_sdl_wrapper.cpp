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

#include <iostream>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_sdl_wrapper.h"

namespace LTSM
{
    bool SDL::Surface::isValid(void) const
    {
        return get();
    }

    int SDL::Surface::width(void) const
    {
        return get() ? get()->w : 0;
    }

    int SDL::Surface::height(void) const
    {
        return get() ? get()->h : 0;
    }

    bool SDL::Texture::isValid(void) const
    {
        return get();
    }

    int SDL::Texture::width(void) const
    {
        if(get())
        {
            int width = 0;

            if(0 != SDL_QueryTexture(get(), nullptr, nullptr, & width, nullptr))
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_QueryTexture", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }

            return width;
        }

        return 0;
    }

    int SDL::Texture::height(void) const
    {
        if(isValid())
        {
            int height = 0;

            if(0 != SDL_QueryTexture(get(), nullptr, nullptr, nullptr, & height))
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_QueryTexture", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }

            return height;
        }

        return 0;
    }

    void SDL::Texture::updateRect(const SDL_Rect* rect, const void* pixels, int pitch)
    {
        if(isValid())
        {
            if(0 != SDL_UpdateTexture(get(), rect, pixels, pitch))
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_UpdateTexture", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }
        }
    }

    SDL::Window::Window(const std::string & title, int rendsz_w, int rendsz_h, int winsz_w, int winsz_h, int flags, bool accel)
    {
        if(winsz_w <= 0)
        {
            winsz_w = rendsz_w;
        }

        if(winsz_h <= 0)
        {
            winsz_h = rendsz_h;
        }

        // SDL part
        _window.reset(SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winsz_w, winsz_h, flags));

        if(! _window)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_CreateWindow", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        _accel = accel;
        _renderer.reset(SDL_CreateRenderer(_window.get(), -1, accel ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE));

        if(accel && ! _renderer)
        {
            _accel = false;
            _renderer.reset(SDL_CreateRenderer(_window.get(), -1, SDL_RENDERER_SOFTWARE));
            Application::warning("%s: %s hardware accel failed, switch to software", __FUNCTION__, "SDL_CreateRenderTexture");
        }

        if(! _renderer)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_CreateRender", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        _display.reset(SDL_CreateTexture(_renderer.get(), TEXTURE_FMT, SDL_TEXTUREACCESS_TARGET, rendsz_w, rendsz_h));

        if(! _display)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_CreateTexture", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        SDL_SetRenderDrawBlendMode(_renderer.get(), SDL_BLENDMODE_BLEND);
        SDL_Color black { .r = 0, .g = 0, .b = 0, .a = 0xFF };
        renderClear(& black, _display.get());
        renderReset();
    }

    SDL::Window::~Window()
    {
        _display.reset();
        _renderer.reset();
        _window.reset();
    }

    bool SDL::Window::resize(int newsz_w, int newsz_h)
    {
        newsz_w = std::max(newsz_w, 640);
        newsz_h = std::max(newsz_h, 480);
        int winsz_w, winsz_h;
        SDL_GetWindowSize(_window.get(), &winsz_w, &winsz_h);

        if(winsz_w != newsz_w && winsz_h != newsz_h)
        {
            int dispsz_w = newsz_w;
            int dispsz_h = newsz_h;
            SDL_SetWindowSize(_window.get(), newsz_w, newsz_h);
            _display.reset();
            _renderer.reset();
            _renderer.reset(SDL_CreateRenderer(_window.get(), -1, (_accel ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE)));

            if(! _renderer)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_CreateRenderer", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }

            _display.reset(SDL_CreateTexture(_renderer.get(), TEXTURE_FMT, SDL_TEXTUREACCESS_TARGET, dispsz_w, dispsz_h));

            if(! _display)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_CreateTexture", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }

            SDL_SetRenderDrawBlendMode(_renderer.get(), SDL_BLENDMODE_BLEND);
            SDL_Color black { .r = 0, .g = 0, .b = 0, .a = 0xFF };
            renderClear(& black, _display.get());
            renderReset();
            return true;
        }

        return false;
    }

    std::pair<int, int> SDL::Window::geometry(void) const
    {
        int width, height;

        if(0 != SDL_QueryTexture(_display.get(), nullptr, nullptr, & width, & height))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_QueryTexture", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        return std::make_pair(width, height);
    }

    uint32_t SDL::Window::pixelFormat(void) const
    {
        uint32_t format;

        if(0 != SDL_QueryTexture(_display.get(), & format, nullptr, nullptr, nullptr))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_QueryTexture", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        return format;
    }

    void SDL::Window::renderReset(SDL_Texture* target)
    {
        if(target)
        {
            int access;

            if(0 != SDL_QueryTexture(target, nullptr, & access, nullptr, nullptr))
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_QueryTexture", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }

            if(access != SDL_TEXTUREACCESS_TARGET)
            {
                Application::error("%s: not target texture", __FUNCTION__);
                throw sdl_error(NS_FuncName);
            }
        }

        if(0 != SDL_SetRenderTarget(_renderer.get(), target))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_SetRenderTarget", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }
    }

    void SDL::Window::renderClear(const SDL_Color* col, SDL_Texture* target)
    {
        renderReset(target ? target : _display.get());

        if(0 != SDL_SetRenderDrawColor(_renderer.get(), col->r, col->g, col->b, col->a))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_RenderDrawColor", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        if(0 != SDL_RenderClear(_renderer.get()))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_RenderClear", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }
    }

    void SDL::Window::renderColor(const SDL_Color* col, const SDL_Rect* rt, SDL_Texture* target)
    {
        renderReset(target ? target : _display.get());

        if(0 != SDL_SetRenderDrawColor(_renderer.get(), col->r, col->g, col->b, col->a))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_RenderDrawColor", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        if(rt->w == 1 && rt->h == 1)
        {
            if(0 != SDL_RenderDrawPoint(_renderer.get(), rt->x, rt->y))
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_RenderDrawPoint", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }
        }
        else if(0 != SDL_RenderFillRect(_renderer.get(), rt))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_RenderFillRect", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }
    }

    void SDL::Window::renderTexture(SDL_Texture* source, const SDL_Rect* srcrt, SDL_Texture* target, const SDL_Rect* dstrt)
    {
        if(source)
        {
            renderReset(target ? target : _display.get());

            if(0 != SDL_RenderCopy(_renderer.get(), source, srcrt, dstrt))
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_RenderCopy", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }
        }
    }

    void SDL::Window::renderPresent(void)
    {
        renderReset();

        if(0 != SDL_RenderCopy(_renderer.get(), _display.get(), nullptr, nullptr))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_RenderCopy", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        SDL_RenderPresent(_renderer.get());
    }

    bool SDL::Window::isValid(void) const
    {
        return _window && _renderer && _display;
    }

    SDL::GenericEvent SDL::Window::pollEvent(void)
    {
        return GenericEvent(SDL_PollEvent(& _event) ? & _event : nullptr);
    }

    SDL::Texture SDL::Window::createTexture(int width, int height, uint32_t format) const
    {
        return Texture(SDL_CreateTexture(_renderer.get(), format, SDL_TEXTUREACCESS_STATIC, width, height));
    }

    std::pair<int, int> SDL::Window::scaleCoord(int posx, int posy) const
    {
        std::pair<int, int> res(0, 0);
        int winsz_w, winsz_h, rendsz_w, rendsz_h;
        SDL_GetWindowSize(_window.get(), &winsz_w, &winsz_h);

        if(0 != SDL_QueryTexture(_display.get(), nullptr, nullptr, &rendsz_w, &rendsz_h))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_QueryTexture", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        res.first = posx * rendsz_w / winsz_w;
        res.second = posy * rendsz_h / winsz_h;
        return res;
    }

#ifdef __UNIX__
#include <X11/keysym.h>
#include <X11/XKBlib.h>

    struct XKSymScancode
    {
        int xksym;
        SDL_Scancode scancode;
    };

    // extern keymap from SDL_x11keyboard.c
    std::initializer_list<XKSymScancode> sdlKeyMap =
    {
        { XK_Return, SDL_SCANCODE_RETURN }, { XK_Escape, SDL_SCANCODE_ESCAPE },
        { XK_BackSpace, SDL_SCANCODE_BACKSPACE }, { XK_Tab, SDL_SCANCODE_TAB },
        { XK_Caps_Lock, SDL_SCANCODE_CAPSLOCK }, { XK_F1, SDL_SCANCODE_F1 },
        { XK_F2, SDL_SCANCODE_F2 }, { XK_F3, SDL_SCANCODE_F3 },
        { XK_F4, SDL_SCANCODE_F4 }, { XK_F5, SDL_SCANCODE_F5 },
        { XK_F6, SDL_SCANCODE_F6 }, { XK_F7, SDL_SCANCODE_F7 },
        { XK_F8, SDL_SCANCODE_F8 }, { XK_F9, SDL_SCANCODE_F9 },
        { XK_F10, SDL_SCANCODE_F10 }, { XK_F11, SDL_SCANCODE_F11 },
        { XK_F12, SDL_SCANCODE_F12 }, { XK_Print, SDL_SCANCODE_PRINTSCREEN },
        { XK_Scroll_Lock, SDL_SCANCODE_SCROLLLOCK }, { XK_Pause, SDL_SCANCODE_PAUSE },
        { XK_Insert, SDL_SCANCODE_INSERT }, { XK_Home, SDL_SCANCODE_HOME },
        { XK_Prior, SDL_SCANCODE_PAGEUP }, { XK_Delete, SDL_SCANCODE_DELETE },
        { XK_End, SDL_SCANCODE_END }, { XK_Next, SDL_SCANCODE_PAGEDOWN },
        { XK_Right, SDL_SCANCODE_RIGHT }, { XK_Left, SDL_SCANCODE_LEFT },
        { XK_Down, SDL_SCANCODE_DOWN }, { XK_Up, SDL_SCANCODE_UP },
        { XK_Num_Lock, SDL_SCANCODE_NUMLOCKCLEAR }, { XK_KP_Divide, SDL_SCANCODE_KP_DIVIDE },
        { XK_KP_Multiply, SDL_SCANCODE_KP_MULTIPLY }, { XK_KP_Subtract, SDL_SCANCODE_KP_MINUS },
        { XK_KP_Add, SDL_SCANCODE_KP_PLUS }, { XK_KP_Enter, SDL_SCANCODE_KP_ENTER },
        { XK_KP_Delete, SDL_SCANCODE_KP_PERIOD }, { XK_KP_End, SDL_SCANCODE_KP_1 },
        { XK_KP_Down, SDL_SCANCODE_KP_2 }, { XK_KP_Next, SDL_SCANCODE_KP_3 },
        { XK_KP_Left, SDL_SCANCODE_KP_4 }, { XK_KP_Begin, SDL_SCANCODE_KP_5 },
        { XK_KP_Right, SDL_SCANCODE_KP_6 }, { XK_KP_Home, SDL_SCANCODE_KP_7 },
        { XK_KP_Up, SDL_SCANCODE_KP_8 }, { XK_KP_Prior, SDL_SCANCODE_KP_9 },
        { XK_KP_Insert, SDL_SCANCODE_KP_0 }, { XK_KP_Decimal, SDL_SCANCODE_KP_PERIOD },
        { XK_KP_1, SDL_SCANCODE_KP_1 }, { XK_KP_2, SDL_SCANCODE_KP_2 },
        { XK_KP_3, SDL_SCANCODE_KP_3 }, { XK_KP_4, SDL_SCANCODE_KP_4 },
        { XK_KP_5, SDL_SCANCODE_KP_5 }, { XK_KP_6, SDL_SCANCODE_KP_6 },
        { XK_KP_7, SDL_SCANCODE_KP_7 }, { XK_KP_8, SDL_SCANCODE_KP_8 },
        { XK_KP_9, SDL_SCANCODE_KP_9 }, { XK_KP_0, SDL_SCANCODE_KP_0 },
        { XK_KP_Decimal, SDL_SCANCODE_KP_PERIOD }, { XK_Hyper_R, SDL_SCANCODE_APPLICATION },
        { XK_KP_Equal, SDL_SCANCODE_KP_EQUALS }, { XK_F13, SDL_SCANCODE_F13 },
        { XK_F14, SDL_SCANCODE_F14 }, { XK_F15, SDL_SCANCODE_F15 },
        { XK_F16, SDL_SCANCODE_F16 }, { XK_F17, SDL_SCANCODE_F17 },
        { XK_F18, SDL_SCANCODE_F18 }, { XK_F19, SDL_SCANCODE_F19 },
        { XK_F20, SDL_SCANCODE_F20 }, { XK_F21, SDL_SCANCODE_F21 },
        { XK_F22, SDL_SCANCODE_F22 }, { XK_F23, SDL_SCANCODE_F23 },
        { XK_F24, SDL_SCANCODE_F24 }, { XK_Execute, SDL_SCANCODE_EXECUTE },
        { XK_Help, SDL_SCANCODE_HELP }, { XK_Menu, SDL_SCANCODE_MENU },
        { XK_Select, SDL_SCANCODE_SELECT }, { XK_Cancel, SDL_SCANCODE_STOP },
        { XK_Redo, SDL_SCANCODE_AGAIN }, { XK_Undo, SDL_SCANCODE_UNDO },
        { XK_Find, SDL_SCANCODE_FIND }, { XK_KP_Separator, SDL_SCANCODE_KP_COMMA },
        { XK_Sys_Req, SDL_SCANCODE_SYSREQ }, { XK_Control_L, SDL_SCANCODE_LCTRL },
        { XK_Shift_L, SDL_SCANCODE_LSHIFT }, { XK_Alt_L, SDL_SCANCODE_LALT },
        { XK_Meta_L, SDL_SCANCODE_LGUI }, { XK_Super_L, SDL_SCANCODE_LGUI },
        { XK_Control_R, SDL_SCANCODE_RCTRL }, { XK_Shift_R, SDL_SCANCODE_RSHIFT },
        { XK_Alt_R, SDL_SCANCODE_RALT }, { XK_ISO_Level3_Shift, SDL_SCANCODE_RALT },
        { XK_Meta_R, SDL_SCANCODE_RGUI }, { XK_Super_R, SDL_SCANCODE_RGUI },
        { XK_Mode_switch, SDL_SCANCODE_MODE }, { XK_period, SDL_SCANCODE_PERIOD },
        { XK_comma, SDL_SCANCODE_COMMA }, { XK_slash, SDL_SCANCODE_SLASH },
        { XK_backslash, SDL_SCANCODE_BACKSLASH }, { XK_minus, SDL_SCANCODE_MINUS },
        { XK_equal, SDL_SCANCODE_EQUALS }, { XK_space, SDL_SCANCODE_SPACE },
        { XK_grave, SDL_SCANCODE_GRAVE }, { XK_apostrophe, SDL_SCANCODE_APOSTROPHE },
        { XK_bracketleft, SDL_SCANCODE_LEFTBRACKET }, { XK_bracketright, SDL_SCANCODE_RIGHTBRACKET }
    };

    int SDL::Window::convertScanCodeToKeySym(SDL_Scancode scancode)
    {
        auto it = std::find_if(sdlKeyMap.begin(), sdlKeyMap.end(),
                               [&](auto & pair)
        {
            return pair.scancode == scancode;
        });

        return it != sdlKeyMap.end() ? (*it).xksym : 0;
    }
#endif // __UNIX__
}
