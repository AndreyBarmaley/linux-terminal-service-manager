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
#include "SDL.h"

namespace LTSM
{
    namespace SDL
    {
        struct Texture : std::shared_ptr<SDL_Texture>
        {
            Texture(SDL_Texture* ptr = nullptr) : std::shared_ptr<SDL_Texture>(ptr, SDL_DestroyTexture)
            {
            }

            bool            isValid(void) const
            {
                return get();
            }

            int             width(void) const;
            int             height(void) const;

            void            updateRect(const SDL_Rect*, const void* pixels, int pitch);
        };

        struct Surface : std::shared_ptr<SDL_Surface>
        {
            Surface(SDL_Surface* ptr = nullptr) : std::shared_ptr<SDL_Surface>(ptr, SDL_FreeSurface)
            {
            }

            bool            isValid(void) const
            {
                return get();
            }
            int             width(void) const
            {
                return get() ? get()->w : 0;
            }
            int             height(void) const
            {
                return get() ? get()->h : 0;
            }

            void            savePNG(const std::string &) const;
        };

        struct GenericEvent
        {
            const SDL_Event*        ptr;

            GenericEvent(const SDL_Event* ev) : ptr(ev)
            {
            }

            bool            		isValid(void) const { return ptr; }
            int             		type(void) const { return ptr ? ptr->type : 0; }
            const SDL_KeyboardEvent*    key(void) const { return ptr ? & ptr->key : nullptr; }
            const SDL_MouseMotionEvent* motion(void) const { return ptr ? & ptr->motion : nullptr; }
            const SDL_MouseButtonEvent* button(void) const { return ptr ? & ptr->button : nullptr; }
            const SDL_MouseWheelEvent*  wheel(void) const { return ptr ? & ptr->wheel : nullptr; }
        };

        class Window
        {
            SDL_Window*         _window;
            SDL_Renderer*	_renderer;
            SDL_Texture*	_display;
            SDL_Event           _event;
            bool                _accel;

        protected:

        public:
            Window(const char*, int rendsz_w, int rendsz_h, int winsz_w, int winsz_h, int flags = 0);
            ~Window();

            bool		isValid(void) const;
            bool                resize(int newsz_w, int newsz_h);

            void		renderClear(const SDL_Color*, SDL_Texture* target = nullptr);
            void		renderColor(const SDL_Color*, const SDL_Rect*, SDL_Texture* target = nullptr);
            void		renderTexture(SDL_Texture* source, const SDL_Rect* srcrt = nullptr, SDL_Texture* target = nullptr, const SDL_Rect* dstrt = nullptr);

            bool		renderReset(SDL_Texture* target = nullptr);
            void		renderPresent(void);

            Texture             createTexture(int width, int height, uint32_t format = SDL_PIXELFORMAT_ARGB8888) const;

            GenericEvent        poolEvent(void);
            static int          convertScanCodeToKeySym(SDL_Scancode);

            std::pair<int, int> scaleCoord(int posx, int posy) const;
        };
    }
}

#endif // LTSM_SDL_WRAPPER_
