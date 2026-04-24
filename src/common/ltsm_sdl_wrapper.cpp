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

namespace LTSM {
    // Surface
    int SDL::Surface::width(void) const {
        assertm(!! ptr_, "invalid surface");
        return ptr_->w;
    }

    int SDL::Surface::height(void) const {
        assertm(!! ptr_, "invalid surface");
        return ptr_->h;
    }

    // Texture
    XCB::Size SDL::Texture::size(void) const {
        assertm(!! ptr_, "invalid texture");
        int width, height;

        if(0 != SDL_QueryTexture(ptr_.get(), nullptr, nullptr, & width, & height)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_QueryTexture", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        return XCB::Size(width, height);
    }

    void SDL::Texture::updateRect(const SDL_Rect* rect, const void* pixels, uint32_t pitch) {
        assertm(!! ptr_, "invalid texture");

        if(0 != SDL_UpdateTexture(ptr_.get(), rect, pixels, pitch)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_UpdateTexture", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }
    }

    SDL::Window::Window(const std::string & title, const XCB::Size & rendsz, const XCB::Size & winsz, int flags, bool accel) :
        render_sz_{rendsz}, window_sz_{winsz}, flags_{flags}, accel_{accel} {

        if(winsz.isEmpty()) {
            window_sz_ = render_sz_;
        }

        // SDL part
        window_.reset(SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_sz_.width, window_sz_.height, flags_));

        if(! window_) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_CreateWindow", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        Application::debug(DebugType::Sdl, "{}: create window, size: {}, flags: {:#08x}", NS_FuncNameV, window_sz_, flags);
        resize(render_sz_, window_sz_);
    }

    SDL::Window::~Window() {
        display_.reset();
        renderer_.reset();
        window_.reset();
    }

    void SDL::Window::resize(const XCB::Size & rsz, const XCB::Size & wsz) {

        assertm(!! window_.get(), "invalid window");

        render_sz_.width = std::max(rsz.width, static_cast<uint16_t>(640));
        render_sz_.height = std::max(rsz.height, static_cast<uint16_t>(480));

        window_sz_.width = std::max(wsz.width, static_cast<uint16_t>(640));
        window_sz_.height = std::max(wsz.height, static_cast<uint16_t>(480));

        Application::debug(DebugType::Sdl, "{}: render size: {}, window size: {}", NS_FuncNameV, render_sz_, window_sz_);
        SDL_SetWindowSize(window_.get(), window_sz_.width, window_sz_.height);

        display_.reset();
        renderer_.reset(SDL_CreateRenderer(window_.get(), -1, (accel_ ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE)));

        if(accel_ && ! renderer_) {
            accel_ = false;
            renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_SOFTWARE));
            Application::warning("{}: {} hardware accel failed, switch to software", NS_FuncNameV, "SDL_CreateRenderer");
        }

        if(! renderer_) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_CreateRenderer", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        Application::debug(DebugType::Sdl, "{}: create render, size: {}, accel: {}", NS_FuncNameV, render_sz_, accel_);
        display_.reset(SDL_CreateTexture(renderer_.get(), TEXTURE_FMT, SDL_TEXTUREACCESS_TARGET, render_sz_.width, render_sz_.height));

        if(! display_) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_CreateTexture", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_BLEND);
        SDL_Color black { .r = 0, .g = 0, .b = 0, .a = 0xFF };

        if(window_sz_ == render_sz_) {
            SDL_ResetHint(SDL_HINT_RENDER_SCALE_QUALITY);
        } else {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        }

        renderClear(& black, display_.get());
        renderReset();
    }

    const XCB::Size & SDL::Window::geometry(void) const {
        return window_sz_;
    }

    uint32_t SDL::Window::pixelFormat(void) const {
        uint32_t format;

        if(0 != SDL_QueryTexture(display_.get(), & format, nullptr, nullptr, nullptr)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_QueryTexture", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        return format;
    }

    void SDL::Window::renderReset(SDL_Texture* target) {
        if(target) {
            int access;

            if(0 != SDL_QueryTexture(target, nullptr, & access, nullptr, nullptr)) {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_QueryTexture", SDL_GetError());
                throw sdl_error(NS_FuncNameS);
            }

            if(access != SDL_TEXTUREACCESS_TARGET) {
                Application::error("{}: not target texture", NS_FuncNameV);
                throw sdl_error(NS_FuncNameS);
            }
        }

        if(0 != SDL_SetRenderTarget(renderer_.get(), target)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_SetRenderTarget", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }
    }

    void SDL::Window::renderClear(const SDL_Color* col, SDL_Texture* target) {
        renderReset(target ? target : display_.get());

        if(0 != SDL_SetRenderDrawColor(renderer_.get(), col->r, col->g, col->b, col->a)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_RenderDrawColor", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        if(0 != SDL_RenderClear(renderer_.get())) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_RenderClear", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }
    }

    void SDL::Window::renderColor(const SDL_Color* col, const SDL_Rect* rt, SDL_Texture* target) {
        renderReset(target ? target : display_.get());

        if(0 != SDL_SetRenderDrawColor(renderer_.get(), col->r, col->g, col->b, col->a)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_RenderDrawColor", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        if(rt->w == 1 && rt->h == 1) {
            if(0 != SDL_RenderDrawPoint(renderer_.get(), rt->x, rt->y)) {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_RenderDrawPoint", SDL_GetError());
                throw sdl_error(NS_FuncNameS);
            }
        } else if(0 != SDL_RenderFillRect(renderer_.get(), rt)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_RenderFillRect", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }
    }

    void SDL::Window::renderTexture(const SDL_Texture* source, const SDL_Rect* srcrt, SDL_Texture* target, const SDL_Rect* dstrt) {
        assertm(source, "invalid texture");
        renderReset(target ? target : display_.get());

        if(0 != SDL_RenderCopy(renderer_.get(), const_cast<SDL_Texture*>(source), srcrt, dstrt)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_RenderCopy", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }
    }

    void SDL::Window::renderPresent(bool sync) {
        renderReset();

        if(sync) {
            if(0 != SDL_RenderCopy(renderer_.get(), display_.get(), nullptr, nullptr)) {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_RenderCopy", SDL_GetError());
                throw sdl_error(NS_FuncNameS);
            }
        }

        SDL_RenderPresent(renderer_.get());
    }

    bool SDL::Window::isValid(void) const {
        return window_ && renderer_ && display_;
    }

    SDL::Texture SDL::Window::createTexture(const XCB::Size & tsz, const SDL_TextureAccess & access, uint32_t format) const {
        if(auto ptr = SDL_CreateTexture(renderer_.get(), format, access, tsz.width, tsz.height)) {
            return Texture(ptr);
        }

        Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_CreateTexture", SDL_GetError());
        throw sdl_error(NS_FuncNameS);
    }

    std::pair<int, int> SDL::Window::scaleCoord(int posx, int posy) const {
        std::pair<int, int> res(0, 0);
        int winsz_w, winsz_h, rendsz_w, rendsz_h;
        SDL_GetWindowSize(window_.get(), &winsz_w, &winsz_h);

        if(0 != SDL_QueryTexture(display_.get(), nullptr, nullptr, &rendsz_w, &rendsz_h)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_QueryTexture", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        res.first = posx * rendsz_w / winsz_w;
        res.second = posy * rendsz_h / winsz_h;
        return res;
    }

    void SDL::Window::setFullScreen(bool state) {
        SDL_SetWindowFullscreen(window_.get(), state ? SDL_WINDOW_FULLSCREEN : 0);
    }

#ifdef __UNIX__
#include <X11/keysym.h>
#include <X11/XKBlib.h>

    struct XKSymScancode {
        int xksym;
        SDL_Scancode scancode;
    };

    // extern keymap from SDL_x11keyboard.c
    std::initializer_list<XKSymScancode> sdlKeyMap = {
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

    int SDL::Window::convertScanCodeToKeySym(SDL_Scancode scancode) {
        auto it = std::ranges::find_if(sdlKeyMap, [&](auto & pair) {
            return pair.scancode == scancode;
        });

        return it != sdlKeyMap.end() ? (*it).xksym : 0;
    }
#endif // __UNIX__
}
