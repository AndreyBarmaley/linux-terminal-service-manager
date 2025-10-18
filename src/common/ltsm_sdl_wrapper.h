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
        struct Texture : std::shared_ptr<SDL_Texture> {
            Texture(SDL_Texture* ptr = nullptr)
                : std::shared_ptr<SDL_Texture>(ptr, SDL_DestroyTexture) {}

            bool isValid(void) const;

            int width(void) const;
            int height(void) const;

            void updateRect(const SDL_Rect*, const void* pixels, int pitch);
        };

        struct Surface : std::shared_ptr<SDL_Surface> {
            Surface(SDL_Surface* ptr = nullptr)
                : std::shared_ptr<SDL_Surface>(ptr, SDL_FreeSurface) {}

            bool isValid(void) const;
            int width(void) const;
            int height(void) const;
        };

        struct GenericEvent {
            const SDL_Event* ptr;

            GenericEvent(const SDL_Event* ev) : ptr(ev) {}

            bool isValid(void) const {
                return ptr;
            }

            int type(void) const {
                return ptr ? ptr->type : 0;
            }

            const SDL_KeyboardEvent* key(void) const {
                return ptr ? & ptr->key : nullptr;
            }

            const SDL_MouseMotionEvent* motion(void) const {
                return ptr ? & ptr->motion : nullptr;
            }

            const SDL_MouseButtonEvent* button(void) const {
                return ptr ? & ptr->button : nullptr;
            }

            const SDL_MouseWheelEvent* wheel(void) const {
                return ptr ? & ptr->wheel : nullptr;
            }

            const SDL_WindowEvent* window(void) const {
                return ptr ? & ptr->window : nullptr;
            }

            const SDL_DropEvent* drop(void) const {
                return ptr ? & ptr->drop : nullptr;
            }

            const SDL_UserEvent* user(void) const {
                return ptr ? & ptr->user : nullptr;
            }
        };

        class Window {
            std::unique_ptr<SDL_Window, void(*)(SDL_Window*)> _window{ nullptr, SDL_DestroyWindow };
            std::unique_ptr<SDL_Renderer, void(*)(SDL_Renderer*)> _renderer{ nullptr, SDL_DestroyRenderer };
            std::unique_ptr<SDL_Texture, void(*)(SDL_Texture*)> _display{ nullptr, SDL_DestroyTexture };
            SDL_Event _event;
            bool _accel = false;

          protected:

          public:
            Window(const std::string & title, int rendsz_w, int rendsz_h, int winsz_w = 0, int winsz_h = 0, int flags = 0,
                   bool accel = true);
            ~Window();

            bool isValid(void) const;
            bool resize(int newsz_w, int newsz_h);

            uint32_t pixelFormat(void) const;
            std::pair<int, int> geometry(void) const;

            SDL_Texture* display(void) {
                return _display.get();
            }

            SDL_Renderer* render(void) {
                return _renderer.get();
            }

            SDL_Window* get(void) {
                return _window.get();
            }

            void renderClear(const SDL_Color*, SDL_Texture* target = nullptr);
            void renderColor(const SDL_Color*, const SDL_Rect*, SDL_Texture* target = nullptr);
            void renderTexture(SDL_Texture* source, const SDL_Rect* srcrt = nullptr, SDL_Texture* target = nullptr,
                               const SDL_Rect* dstrt = nullptr);

            void renderReset(SDL_Texture* target = nullptr);
            void renderPresent(bool sync = true);

            Texture createTexture(int width, int height, uint32_t format = TEXTURE_FMT) const;

            GenericEvent pollEvent(void);
            static int convertScanCodeToKeySym(SDL_Scancode);

            std::pair<int, int> scaleCoord(int posx, int posy) const;
        };
    }
}

#endif // LTSM_SDL_WRAPPER_
