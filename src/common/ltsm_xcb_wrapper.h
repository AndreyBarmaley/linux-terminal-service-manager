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

#ifndef _LTSM_XCB_WRAPPER_
#define _LTSM_XCB_WRAPPER_

#include <list>
#include <memory>

#include "xcb/xcb.h"
#include "xcb/shm.h"
#include "xcb/damage.h"
#include "xcb/xproto.h"
#include "xcb/xcb_keysyms.h"

struct SDL_Keysym;

namespace LTSM
{
    namespace XCB
    {
        struct module_t
        {
            uint32_t                xcb;
            xcb_connection_t*       conn;
            xcb_generic_error_t*    error;

            module_t(uint32_t id, xcb_connection_t* xcon) : xcb(id), conn(xcon), error(nullptr) {}
            ~module_t()
            {
                if(error) std::free(error);
            }
        };

        struct gc_t : module_t
        {
            gc_t(xcb_connection_t* xcon) : module_t(0, xcon) {}
            ~gc_t()
            {
                if(xcb) xcb_free_gc(conn, xcb);
            }
        };

        struct damage_t : module_t
        {
            damage_t(xcb_connection_t* xcon) : module_t(0, xcon) {}
            ~damage_t()
            {
                if(xcb) xcb_damage_destroy(conn, xcb);
            }
        };

        struct xfixes_region_t : module_t
        {
            xfixes_region_t(xcb_connection_t* xcon) : module_t(0, xcon) {}
            ~xfixes_region_t()
            {
                if(xcb) xcb_xfixes_destroy_region(conn, xcb);
            }

        };

        struct shm_t : module_t
        {
            int                     shm;
            uint8_t*                addr;

            shm_t(int id, uint8_t* ptr, xcb_connection_t* xcon) : module_t(0, xcon), shm(id), addr(ptr) {}
            ~shm_t();
        };

        struct GenericError : std::shared_ptr<xcb_generic_error_t>
        {
            GenericError(xcb_generic_error_t* err) : std::shared_ptr<xcb_generic_error_t>(err, std::free)
            {
            }
        };

        struct GenericEvent : std::shared_ptr<xcb_generic_event_t>
        {
            GenericEvent(xcb_generic_event_t* ev) : std::shared_ptr<xcb_generic_event_t>(ev, std::free)
            {
            }
        };

        struct KeyCodes : std::shared_ptr<xcb_keycode_t>
        {
            KeyCodes(xcb_keycode_t* ptr) : std::shared_ptr<xcb_keycode_t>(ptr, std::free)
            {
            }

            bool isValid(void) const
            {
                return get() && *get() != XCB_NO_SYMBOL;
            }
        };

        struct HasherKeyCodes
        {
            size_t operator()(const KeyCodes & kc) const;
        };

        struct XFixesRegion : std::shared_ptr<xfixes_region_t>
        {
            XFixesRegion() {}
            XFixesRegion(const xcb_rectangle_t*, uint32_t count, xcb_connection_t*);
            XFixesRegion(xcb_window_t win, xcb_shape_kind_t kind, xcb_connection_t*);

            const xcb_generic_error_t* error(void) const;
        };

        struct Damage : std::shared_ptr<damage_t>
        {
            Damage() {}
            Damage(xcb_drawable_t, int level, xcb_connection_t*);

            const xcb_generic_error_t* error(void) const;

            GenericError            addRegion(xcb_drawable_t winid, xcb_xfixes_region_t regid);
            GenericError            subtractRegion(xcb_drawable_t winid, xcb_xfixes_region_t repair, xcb_xfixes_region_t parts);

            bool isValid(void) const
            {
                return get() && get()->xcb && ! get()->error;
            }
        };

        struct GC : std::shared_ptr<gc_t>
        {
            GC(xcb_drawable_t, xcb_connection_t*,
               uint32_t value_mask = 0, const void* value_list = nullptr);

            const xcb_generic_error_t* error(void) const;
        };

        struct PixmapInfo
        {
            uint8_t                 depth;
            xcb_visualid_t          visual;
            uint32_t                size;

            PixmapInfo() : depth(0), visual(0), size(0) {}
        };

        struct SHM : std::shared_ptr<shm_t>
        {
            SHM() {}
            SHM(int shmid, uint8_t* addr, xcb_connection_t*);

            const xcb_generic_error_t* error(void) const;

            bool isValid(void) const
            {
                return get() && get()->xcb && 0 < get()->shm && ! get()->error;
            }

            xcb_shm_get_image_cookie_t getPixmapRegionRequest(xcb_drawable_t, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh, uint32_t offset = 0) const;
            xcb_shm_get_image_cookie_t getPixmapRegionRequest(xcb_drawable_t, const xcb_rectangle_t &, uint32_t offset = 0) const;

            std::pair<bool, PixmapInfo> getPixmapRegion(xcb_drawable_t, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh, uint32_t offset = 0) const;
            std::pair<bool, PixmapInfo> getPixmapRegion(xcb_drawable_t, const xcb_rectangle_t &, uint32_t offset = 0) const;
        };

        class Connector
        {
        protected:
            xcb_connection_t*       _conn;

            void                    extendedError(const xcb_generic_error_t* error, const char* func) const;
            void                    extendedError(const GenericError &, const char* func) const;

        public:
            Connector(const char* addr);
            ~Connector();

            bool                    checkExtensionSHM(void);
            bool                    checkExtensionDAMAGE(void);
            bool                    checkExtensionXFIXES(void);
            bool                    checkExtensionTEST(void);

            GenericError            checkRequest(const xcb_void_cookie_t &) const;
            GenericEvent            poolEvent(void);

            SHM                     createSHM(size_t, int mode = 0600);

            static bool             testConnection(const char* addr);
        };

        class RootDisplay : public Connector
        {
            xcb_screen_t*           _screen;
            xcb_key_symbols_t*      _symbols;
            xcb_format_t*           _format;
            xcb_visualtype_t*       _visual;

        public:
            RootDisplay(const std::string & addr);
            ~RootDisplay();

            GC                      createGC(uint32_t value_mask = 0, const void* value_list = nullptr);
            Damage                  createDamageNotify(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh, int level = XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
            Damage                  createDamageNotify(const xcb_rectangle_t &, int level = XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);

            int			    width(void) const;
            int			    height(void) const;
            int	                    depth(void) const;
            int	                    bitsPerPixel(void) const;
            int	                    bitsPerPixel(int depth) const;
            int			    scanlinePad(void) const;
            const xcb_visualtype_t* visual(void) const;

            const xcb_visualtype_t* findVisual(xcb_visualid_t) const;
            void                    fillBackground(uint32_t color);

            std::pair<bool, XCB::PixmapInfo> copyRootImage(const SHM &) const;
            std::pair<bool, XCB::PixmapInfo> copyRootImageRegion(const SHM &, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) const;
            std::pair<bool, XCB::PixmapInfo> copyRootImageRegion(const SHM &, const xcb_rectangle_t &) const;

            int                     getEventSHM(const GenericEvent & ev);
            int                     getEventDAMAGE(const GenericEvent & ev);
            int                     getEventTEST(const GenericEvent & ev);
            int                     getEventXFIXES(const GenericEvent & ev);

            bool                    isEventSHM(const GenericEvent & ev, int filter);
            bool                    isEventDAMAGE(const GenericEvent & ev, int filter);
            bool                    isEventTEST(const GenericEvent & ev, int filter);
            bool                    isEventXFIXES(const GenericEvent & ev, int filter);

            bool                    fakeInputKeysym(int type, const KeyCodes &, bool wait = true) const;
            bool                    fakeInputMouse(int type, int buttons, int posx, int posy, bool wait = true) const;
            KeyCodes                keysymToKeycodes(int) const;

            bool                    damageSubtrack(const Damage &, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);
            bool                    damageSubtrack(const Damage &, const xcb_rectangle_t &);
        };
    }
}

#endif // _LTSM_XCB_WRAPPER_
