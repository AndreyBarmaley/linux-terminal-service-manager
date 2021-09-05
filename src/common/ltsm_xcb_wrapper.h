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
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>

#include "xcb/xcb.h"
#include "xcb/shm.h"
#include "xcb/randr.h"
#include "xcb/damage.h"
#include "xcb/xproto.h"
#include "xcb/xcb_keysyms.h"

struct SDL_Keysym;

namespace LTSM
{
    namespace XCB
    {
        struct object_t
        {
            uint32_t                xcb;
            xcb_connection_t*       conn;

            object_t(uint32_t id, xcb_connection_t* xcon) : xcb(id), conn(xcon) {}
            virtual ~object_t() {}
        };

        struct gc_t : object_t
        {
            gc_t(xcb_connection_t* xcon, uint32_t id = 0) : object_t(id, xcon) {}
            ~gc_t() { if(xcb) xcb_free_gc(conn, xcb); }
        };

        struct damage_t : object_t
        {
            damage_t(xcb_connection_t* xcon, uint32_t id = 0) : object_t(id, xcon) {}
            ~damage_t() { if(xcb) xcb_damage_destroy(conn, xcb); }
        };

        struct xfixes_region_t : object_t
        {
            xfixes_region_t(xcb_connection_t* xcon, uint32_t id = 0) : object_t(id, xcon) {}
            ~xfixes_region_t() { if(xcb) xcb_xfixes_destroy_region(conn, xcb); }
        };

        struct shm_t : object_t
        {
            int                     shm;
            uint8_t*                addr;

            shm_t(int shmid, uint8_t* ptr, xcb_connection_t* xcon, uint32_t id = 0) : object_t(id, xcon), shm(shmid), addr(ptr) {}
            ~shm_t();
        };

        struct GenericError : std::shared_ptr<xcb_generic_error_t>
        {
            GenericError(xcb_generic_error_t* err)
                : std::shared_ptr<xcb_generic_error_t>(err, std::free) {}
        };

        struct GenericEvent : std::shared_ptr<xcb_generic_event_t>
        {
            GenericEvent(xcb_generic_event_t* ev)
                : std::shared_ptr<xcb_generic_event_t>(ev, std::free) {}

            const xcb_generic_error_t*  toerror(void) const { return reinterpret_cast<const xcb_generic_error_t*>(get()); }
        };

	template<typename ReplyType>
	struct GenericReply : std::shared_ptr<ReplyType>
	{
    	    GenericReply(ReplyType* ptr) : std::shared_ptr<ReplyType>(ptr, std::free)
    	    {
    	    }
	};

	template<typename ReplyType>
	struct ReplyError : std::pair<GenericReply<ReplyType>, GenericError>
	{
    	    ReplyError(ReplyType* ptr, xcb_generic_error_t* err)
		: std::pair<GenericReply<ReplyType>, GenericError>(ptr, err)
    	    {
    	    }

	    const GenericReply<ReplyType> &	reply(void) const { return std::pair<GenericReply<ReplyType>, GenericError>::first; }
	    const GenericError &		error(void) const { return std::pair<GenericReply<ReplyType>, GenericError>::second; }
	};

	template<typename Reply, typename Cookie>
	ReplyError<Reply> getReply(std::function<Reply*(xcb_connection_t*, Cookie, xcb_generic_error_t**)> func, xcb_connection_t* conn, Cookie cookie)
	{
    	    xcb_generic_error_t* error = nullptr;
    	    Reply* reply = func(conn, cookie, & error);
    	    return ReplyError<Reply>(reply, error);
	}

	#define getReplyConn(NAME,conn,cookie) getReply<NAME##_reply_t,NAME##_cookie_t>(NAME##_reply,conn,cookie)

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

        struct GC : std::shared_ptr<gc_t>
        {
            GC(xcb_drawable_t, xcb_connection_t*,
               uint32_t value_mask = 0, const void* value_list = nullptr);

            uint32_t               getid(void) const { return get() ? get()->xcb : 0; }
        };

        struct XFixesRegion : std::shared_ptr<xfixes_region_t>
        {
            XFixesRegion() {}
            XFixesRegion(const xcb_rectangle_t*, uint32_t count, xcb_connection_t*);
            XFixesRegion(xcb_window_t win, xcb_shape_kind_t kind, xcb_connection_t*);

            uint32_t                getid(void) const { return get() ? get()->xcb : 0; }
        };

        struct Damage : std::shared_ptr<damage_t>
        {
            Damage() {}
            Damage(xcb_drawable_t, int level, xcb_connection_t*);

            bool                    addRegion(xcb_drawable_t winid, xcb_xfixes_region_t regid);
            bool                    subtractRegion(xcb_drawable_t winid, xcb_xfixes_region_t repair, xcb_xfixes_region_t parts);
            uint32_t                getid(void) const { return get() ? get()->xcb : 0; }
        };

        struct PixmapInfoBase
        {
            uint8_t                _depth;
            xcb_visualid_t         _visual;

            virtual uint8_t        depth(void) const { return _depth; }
            virtual xcb_visualid_t visual(void) const { return _visual; }

            virtual uint8_t*       data(void) = 0;
            virtual const uint8_t* data(void) const = 0;
            virtual size_t         size(void) const = 0;

            virtual ~PixmapInfoBase() {}
            PixmapInfoBase(uint8_t d = 0, xcb_visualid_t v = 0) : _depth(d), _visual(v) {}
        };
        typedef std::shared_ptr<PixmapInfoBase> PixmapInfoReply;

        struct SHM : std::shared_ptr<shm_t>
        {
            SHM() {}
            SHM(int shmid, uint8_t* addr, xcb_connection_t*);

            PixmapInfoReply       getPixmapRegion(xcb_drawable_t, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh, uint32_t offset = 0) const;
            PixmapInfoReply       getPixmapRegion(xcb_drawable_t, const xcb_rectangle_t &, uint32_t offset = 0) const;
            uint32_t              getid(void) const { return get() ? get()->xcb : 0; }
            uint8_t*              data(void) { return get() ? get()->addr : nullptr; }
            const uint8_t*        data(void) const { return get() ? get()->addr : nullptr; }
        };

        struct PixmapInfoSHM : PixmapInfoBase
        {
            SHM                    _shm;
	    size_t	           _size;

            uint8_t*               data(void) override { return _shm.data(); }
            const uint8_t*         data(void) const override { return _shm.data(); }
            size_t                 size(void) const override { return _size; }

            PixmapInfoSHM() : _size(0) {}
            PixmapInfoSHM(uint8_t depth, xcb_visualid_t vis, const SHM & shm, size_t len)
                : PixmapInfoBase(depth, vis), _shm(shm), _size(len) {}
        };

        struct PixmapInfoBuffer : PixmapInfoBase
        {
            std::vector<uint8_t>  _pixels;

            uint8_t*              data(void) override { return _pixels.data(); }
            const uint8_t*        data(void) const override { return _pixels.data(); }
            size_t                size(void) const override { return _pixels.size(); }

            PixmapInfoBuffer() {}
            PixmapInfoBuffer(uint8_t depth, xcb_visualid_t vis, size_t res = 0)
                : PixmapInfoBase(depth, vis) { _pixels.reserve(res); }
        };

        enum class Module { SHM, DAMAGE, XFIXES, RANDR, TEST };

        class Connector
        {
        protected:
            xcb_connection_t*       _conn;

        public:
            Connector(const char* addr);
            virtual ~Connector();

	    template<typename Reply, typename Cookie>
	    ReplyError<Reply> getReply(std::function<Reply*(xcb_connection_t*, Cookie, xcb_generic_error_t**)> func, Cookie cookie) const
	    {
                return XCB::getReply<Reply, Cookie>(func, _conn, cookie);
	    }

	    #define getReplyFunc(NAME,cookie) getReply<NAME##_reply_t,NAME##_cookie_t>(NAME##_reply,cookie)

	    int                     hasError(void);

            bool                    checkExtension(const Module &);
            int                     eventErrorOpcode(const GenericEvent & ev, const Module &);
            int                     eventNotify(const GenericEvent & ev, const Module &);

            bool                    isDamageNotify(const GenericEvent & ev);
            bool                    isXFixesSelectionNotify(const GenericEvent & ev);
            bool                    isXFixesCursorNotify(const GenericEvent & ev);
            bool                    isRandrScreenNotify(const GenericEvent & ev);
            bool                    isRandrOutputNotify(const GenericEvent & ev);
            bool                    isRandrCRTCNotify(const GenericEvent & ev);

            void                    extendedError(const xcb_generic_error_t* error, const char* func) const;
            void                    extendedError(const GenericError &, const char* func) const;

            GenericError            checkRequest(const xcb_void_cookie_t &) const;
            virtual GenericEvent    poolEvent(void);

            GC                      createGC(xcb_drawable_t winid, uint32_t value_mask = 0, const void* value_list = nullptr);
            SHM                     createSHM(size_t, int mode = 0600);
            Damage                  createDamage(xcb_drawable_t winid, int level = XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
            XFixesRegion            createFixesRegion(const xcb_rectangle_t* rect, size_t num);
	    xcb_atom_t              getAtom(const std::string &, bool create = true) const;
	    bool                    checkAtom(const std::string &) const;
	    std::string	            getAtomName(xcb_atom_t) const;
	    size_t                  getMaxRequest(void) const;

            bool                    damageSubtrack(const Damage &, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);
            static bool             testConnection(const char* addr);
        };

        class RootDisplay : public Connector
        {
	protected:
            xcb_screen_t*           _screen;
            xcb_key_symbols_t*      _symbols;
            xcb_format_t*           _format;
            xcb_visualtype_t*       _visual;

            Damage                  _damage;
            XFixesRegion            _xfixes;
            SHM                     _shm;

        public:
            RootDisplay(const std::string & addr);
            ~RootDisplay();

            int			    width(void) const;
            int			    height(void) const;
            std::pair<uint16_t,uint16_t> size(void) const;
            int	                    depth(void) const;
            int	                    bitsPerPixel(void) const;
            int	                    bitsPerPixel(int depth) const;
            int			    scanlinePad(void) const;
            const xcb_visualtype_t* visual(void) const;
            xcb_drawable_t          root(void) const;

            const xcb_visualtype_t* findVisual(xcb_visualid_t) const;
            void                    fillBackground(uint32_t color);

	    std::list<xcb_randr_screen_size_t>
				    screenSizes(void) const;
	    bool	            setScreenSize(uint16_t windth, uint16_t height);

            PixmapInfoReply         copyRootImageRegion(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) const;

            bool                    fakeInputKeysym(int type, const KeyCodes &, bool wait = true) const;
            bool                    fakeInputMouse(int type, int buttons, int posx, int posy, bool wait = true) const;
            KeyCodes                keysymToKeycodes(int) const;

            bool                    damageSubtrack(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);

            GenericEvent            poolEvent(void) override;
        };

	class RootDisplayExt : public RootDisplay
	{
        protected:
            xcb_window_t            _selwin;
	    std::vector<uint8_t>    _selbuf;

	    xcb_atom_t              _atoms[7];
	    xcb_atom_t &            _atomPrimary;
	    xcb_atom_t &            _atomClipboard;
	    xcb_atom_t &            _atomBuffer;
	    xcb_atom_t &            _atomTargets;
	    xcb_atom_t &            _atomText;
	    xcb_atom_t &            _atomTextPlain;
	    xcb_atom_t &            _atomUTF8;

	    xcb_window_t             getOwnerSelection(const xcb_atom_t &);

	    bool                     getSelectionEvent(const xcb_atom_t &);
	    bool                     setClipboardEvent(std::vector<uint8_t> &, const xcb_atom_t &);

            bool                     sendNotifyTargets(const xcb_selection_request_event_t &);
            bool                     sendNotifySelData(const xcb_selection_request_event_t &);

	public:
	    RootDisplayExt(const std::string & addr);
            ~RootDisplayExt();

            bool                     selectionClearAction(xcb_selection_clear_event_t*);
            bool                     selectionRequestAction(xcb_selection_request_event_t*);
            bool                     selectionNotifyAction(xcb_selection_notify_event_t*);

            bool                     setClipboardEvent(std::vector<uint8_t> &);
	    bool                     getClipboardEvent(void);

	    const std::vector<uint8_t> & getSelectionData(void) const { return _selbuf; };
	};
    }
}

#endif // _LTSM_XCB_WRAPPER_
