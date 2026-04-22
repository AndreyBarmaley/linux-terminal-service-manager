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

#ifndef _SDL_WRAPPER_
#define _SDL_WRAPPER_

#include <memory>
#include <exception>

#include "SDL.h"
#include "ltsm_xcb_types.h"

namespace LTSM {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    inline static const int TEXTURE_FMT = SDL_PIXELFORMAT_ARGB32;
#else
    inline static const int TEXTURE_FMT = SDL_PIXELFORMAT_BGRA32;
#endif

    struct sdl_error : public std::runtime_error {
        explicit sdl_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };

    namespace SDL {
        class Texture {
            std::unique_ptr<SDL_Texture, void(*)(SDL_Texture*)> ptr_{nullptr, SDL_DestroyTexture};

          public:
            explicit Texture(SDL_Texture* ptr = nullptr)
                : ptr_{ptr, SDL_DestroyTexture} {}
            ~Texture() = default;

            inline bool isValid(void) const {
                return !! ptr_;
            }

            XCB::Size size(void) const;

            inline SDL_Texture* get(void) {
                return ptr_.get();
            }

            void updateRect(const SDL_Rect*, const void* pixels, uint32_t pitch);
        };

        class Surface {
            std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> ptr_{nullptr, SDL_FreeSurface};

          public:
            explicit Surface(SDL_Surface* ptr = nullptr)
                : ptr_{ptr, SDL_FreeSurface} {}
            ~Surface() = default;

            inline bool isValid(void) const {
                return !! ptr_;
            }

            inline SDL_Surface* get(void) {
                return ptr_.get();
            }

            int width(void) const;
            int height(void) const;
        };

        struct GenericEvent {
            const SDL_Event* ptr_;

            explicit GenericEvent(const SDL_Event* ev) : ptr_(ev) {}
            ~GenericEvent() = default;

            inline bool isValid(void) const {
                return ptr_;
            }

            inline int type(void) const {
                return ptr_->type;
            }

            inline const SDL_KeyboardEvent* key(void) const {
                return & ptr_->key;
            }

            inline const SDL_MouseMotionEvent* motion(void) const {
                return & ptr_->motion;
            }

            inline const SDL_MouseButtonEvent* button(void) const {
                return & ptr_->button;
            }

            inline const SDL_MouseWheelEvent* wheel(void) const {
                return & ptr_->wheel;
            }

            inline const SDL_WindowEvent* window(void) const {
                return & ptr_->window;
            }

            inline const SDL_DropEvent* drop(void) const {
                return & ptr_->drop;
            }

            inline const SDL_UserEvent* user(void) const {
                return & ptr_->user;
            }
        };

        class Window {
            SDL_Event event_;

            std::unique_ptr<SDL_Window, void(*)(SDL_Window*)> window_{ nullptr, SDL_DestroyWindow };
            std::unique_ptr<SDL_Renderer, void(*)(SDL_Renderer*)> renderer_{ nullptr, SDL_DestroyRenderer };
            std::unique_ptr<SDL_Texture, void(*)(SDL_Texture*)> display_{ nullptr, SDL_DestroyTexture };

            XCB::Size render_sz_;
            XCB::Size window_sz_;

            int flags_ = 0;
            bool accel_ = false;

          protected:

          public:
            Window(const std::string & title, const XCB::Size & rendsz, const XCB::Size & winsz = {}, int flags = 0, bool accel = true);
            ~Window();

            bool isValid(void) const;
            void resize(const XCB::Size & /* render sz */, const XCB::Size & /* window sz */);

            inline void resize(const XCB::Size & nsz) {
                resize(nsz, nsz);
            }

            uint32_t pixelFormat(void) const;
            const XCB::Size& geometry(void) const;

            inline SDL_Texture* display(void) {
                return display_.get();
            }

            inline SDL_Renderer* render(void) {
                return renderer_.get();
            }

            inline SDL_Window* get(void) {
                return window_.get();
            }

            void renderClear(const SDL_Color*, SDL_Texture* target = nullptr);
            void renderColor(const SDL_Color*, const SDL_Rect*, SDL_Texture* target = nullptr);

            void renderTexture(const SDL_Texture* source, const SDL_Rect* srcrt = nullptr, SDL_Texture* target = nullptr,
                               const SDL_Rect* dstrt = nullptr);

            void renderReset(SDL_Texture* target = nullptr);
            void renderPresent(bool sync = true);

            Texture createTexture(const XCB::Size &, uint32_t format = TEXTURE_FMT) const;

            GenericEvent pollEvent(void);
            static int convertScanCodeToKeySym(SDL_Scancode);

            std::pair<int, int> scaleCoord(int posx, int posy) const;
        };
    }
}

#endif // LTSM_SDL_WRAPPER_
