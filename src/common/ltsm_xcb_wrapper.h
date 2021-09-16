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
#include <array>
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
	struct Point
	{
	    int16_t x, y;

	    Point() : x(-1), y(-1) {}
	    Point(const xcb_point_t & pt) : x(pt.x), y(pt.y) {}
	    Point(int16_t px, int16_t py) : x(px), y(py) {}
	};

	struct Size
	{
	    uint16_t width, height;

	    Size() : width(0), height(0) {}
	    Size(uint16_t sw, uint16_t sh) : width(sw), height(sh) {}

	    bool isEmpty(void) const { return width == 0 || height == 0; }

            bool operator==(const Size & sz) const { return sz.width == width && sz.height == height; }
            bool operator!=(const Size & sz) const { return sz.width != width || sz.height != height; }
	};

	struct Region : public Point, public Size
	{
            Region() {}
            Region(const Point & pt, const Size & sz) : Point(pt), Size(sz) {}
            Region(const xcb_rectangle_t & rt) : Point(rt.x, rt.y), Size(rt.width, rt.height) {}
            Region(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) : Point(rx, ry), Size(rw, rh) {}

            const Point & toPoint(void) const { return *this; }
            const Size &  toSize(void) const { return *this; }

            bool        operator== (const Region & rt) const { return rt.x == x && rt.y == y && rt.width == width && rt.height == height; }
            bool        operator!= (const Region & rt) const { return rt.x != x || rt.y != y || rt.width != width || rt.height != height; }

            void        reset(void);

            void        assign(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);
            void        assign(const Region &);

            void        join(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);
            void        join(const Region &);

            bool        empty(void) const;
            bool        invalid(void) const;

            Region      intersected(const Region &) const;
            Region      align(size_t) const;

            static bool intersects(const Region &, const Region &);
            static bool intersection(const Region &, const Region &, Region* res);

            std::list<Region> divideBlocks(size_t cw, size_t ch) const;
            std::list<Region> divideCounts(size_t cw, size_t ch) const;
	};

        Region operator- (const Region &, const Point &);
        Region operator+ (const Region &, const Point &);

        struct HasherRegion
        {
            size_t operator()(const Region & reg) const
            {
                return std::hash<uint64_t>()((static_cast<uint64_t>(reg.x) << 48) | (static_cast<uint64_t>(reg.y) << 32) |
                                             (static_cast<uint64_t>(reg.width) << 16) | static_cast<uint64_t>(reg.height));
            }
        };

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
            GenericEvent() {}
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
            KeyCodes(xcb_keycode_t* ptr)
		: std::shared_ptr<xcb_keycode_t>(ptr, std::free) {}

            KeyCodes(uint8_t code)
		: std::shared_ptr<xcb_keycode_t>(new xcb_keycode_t[2]{code, XCB_NO_SYMBOL}, std::default_delete<xcb_keycode_t[]>()) {}

            bool isValid(void) const;
            bool operator==(const KeyCodes &) const;
        };

        struct HasherKeyCodes
        {
            size_t operator()(const KeyCodes & kc) const;
        };

        struct GC : std::shared_ptr<gc_t>
        {
            GC(xcb_drawable_t, xcb_connection_t*,
               uint32_t value_mask = 0, const void* value_list = nullptr);

            uint32_t               xid(void) const { return get() ? get()->xcb : 0; }
            xcb_connection_t*      connection(void) const { return get() ? get()->conn : nullptr; }
        };

        struct XFixesRegion : std::shared_ptr<xfixes_region_t>
        {
            XFixesRegion() {}

            XFixesRegion(const Region &, xcb_connection_t*);
            XFixesRegion(const xcb_rectangle_t*, uint32_t count, xcb_connection_t*);
            XFixesRegion(xcb_window_t win, xcb_shape_kind_t kind, xcb_connection_t*);

            uint32_t                xid(void) const { return get() ? get()->xcb : 0; }
            xcb_connection_t*       connection(void) const { return get() ? get()->conn : nullptr; }

            XFixesRegion            intersect(xcb_xfixes_region_t) const;
            XFixesRegion            unionreg(xcb_xfixes_region_t) const;
        };

        struct Damage : std::shared_ptr<damage_t>
        {
            Damage() {}
            Damage(xcb_drawable_t, int level, xcb_connection_t*);

            bool                    addRegion(xcb_drawable_t winid, xcb_xfixes_region_t regid);
            bool                    subtractRegion(xcb_xfixes_region_t regid, xcb_xfixes_region_t parts);

            uint32_t                xid(void) const { return get() ? get()->xcb : 0; }
            xcb_connection_t*       connection(void) const { return get() ? get()->conn : nullptr; }
        };

        struct PixmapInfoBase
        {
            size_t                  _depth;
            const xcb_visualtype_t* _visual;

            size_t                 depth(void) const { return _depth; }
	    size_t		   redMask(void) const { return _visual ? _visual->red_mask : 0; }
	    size_t		   greenMask(void) const { return _visual ? _visual->red_mask : 0; }
	    size_t		   blueMask(void) const { return _visual ? _visual->red_mask : 0; }
            const xcb_visualtype_t* visual(void) const { return _visual; }

            virtual uint8_t*       data(void) = 0;
            virtual const uint8_t* data(void) const = 0;
            virtual size_t         size(void) const = 0;

            virtual ~PixmapInfoBase() {}
            PixmapInfoBase(uint8_t d = 0, const xcb_visualtype_t* v = nullptr) : _depth(d), _visual(v) {}
        };
        typedef std::shared_ptr<PixmapInfoBase> PixmapInfoReply;

        struct SHM : std::shared_ptr<shm_t>
        {
            SHM() {}
            SHM(int shmid, uint8_t* addr, xcb_connection_t*);

            PixmapInfoReply       getPixmapRegion(xcb_drawable_t, const Region &, size_t offset = 0, size_t planeMask = 0) const;

            uint8_t*              data(void) { return get() ? get()->addr : nullptr; }
            const uint8_t*        data(void) const { return get() ? get()->addr : nullptr; }

            xcb_connection_t*     connection(void) const { return get() ? get()->conn : nullptr; }
            uint32_t              xid(void) const { return get() ? get()->xcb : 0; }
        };

        struct PixmapInfoSHM : PixmapInfoBase
        {
            SHM                    _shm;
	    size_t	           _size;
	    xcb_visualid_t         _visid;

            uint8_t*               data(void) override { return _shm.data(); }
            const uint8_t*         data(void) const override { return _shm.data(); }
            size_t                 size(void) const override { return _size; }

            PixmapInfoSHM() : _size(0), _visid(0) {}
            PixmapInfoSHM(uint8_t depth, xcb_visualid_t vis, const SHM & shm, size_t len)
                : PixmapInfoBase(depth), _shm(shm), _size(len), _visid(vis) {}
        };

        struct PixmapInfoBuffer : PixmapInfoBase
        {
            std::vector<uint8_t>  _pixels;

            uint8_t*              data(void) override { return _pixels.data(); }
            const uint8_t*        data(void) const override { return _pixels.data(); }
            size_t                size(void) const override { return _pixels.size(); }

            PixmapInfoBuffer() {}
            PixmapInfoBuffer(uint8_t depth, const xcb_visualtype_t* vis, size_t res = 0)
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

	    int                     hasError(void) const;

            bool                    checkExtension(const Module &) const;
            int                     eventErrorOpcode(const GenericEvent & ev, const Module &) const;
            int                     eventNotify(const GenericEvent & ev, const Module &) const;

            bool                    isDamageNotify(const GenericEvent & ev) const;
            bool                    isXFixesSelectionNotify(const GenericEvent & ev) const;
            bool                    isXFixesCursorNotify(const GenericEvent & ev) const;
            bool                    isRandrScreenNotify(const GenericEvent & ev) const;
            bool                    isRandrOutputNotify(const GenericEvent & ev) const;
            bool                    isRandrCRTCNotify(const GenericEvent & ev) const;

            void                    extendedError(const xcb_generic_error_t* error, const char* func) const;
            void                    extendedError(const GenericError &, const char* func) const;

            GenericError            checkRequest(const xcb_void_cookie_t &) const;
            virtual GenericEvent    poolEvent(void);

            GC                      createGC(xcb_drawable_t winid, uint32_t value_mask = 0, const void* value_list = nullptr);
            SHM                     createSHM(size_t, int mode = 0600);
            Damage                  createDamage(xcb_drawable_t winid, int level = XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
            XFixesRegion            createFixesRegion(const Region &);
            XFixesRegion            createFixesRegion(const xcb_rectangle_t* rect, size_t num);
	    xcb_atom_t              getAtom(const std::string &, bool create = true) const;
	    bool                    checkAtom(const std::string &) const;
	    std::string	            getAtomName(xcb_atom_t) const;
	    size_t                  getMaxRequest(void) const;

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
            SHM                     _shm;

        public:
            RootDisplay(const std::string & addr);
            ~RootDisplay();

            uint16_t		    width(void) const;
            uint16_t		    height(void) const;
            Region                  region(void) const;
            Size                    size(void) const;
            size_t                  depth(void) const;
            size_t	            bitsPerPixel(void) const;
            size_t		    scanlinePad(void) const;
            const xcb_visualtype_t* visual(void) const;
            xcb_drawable_t          root(void) const;
            size_t	            depth(size_t bitsPerPixel) const;
            size_t	            bitsPerPixel(size_t depth) const;

            const xcb_visualtype_t* findVisual(xcb_visualid_t) const;
            void                    fillBackground(uint32_t color);

	    std::list<xcb_randr_screen_size_t>
				    screenSizes(void) const;
	    bool	            setScreenSize(uint16_t windth, uint16_t height);

            PixmapInfoReply         copyRootImageRegion(const Region &, size_t planeMask = 0) const;

            bool                    fakeInputKeysym(int type, const KeyCodes &);
            bool                    fakeInputKeycode(int type, uint8_t keycode);
            bool                    fakeInputMouse(int type, int buttons, int posx, int posy);
            KeyCodes                keysymToKeycodes(int) const;

            bool                    damageAdd(const xcb_rectangle_t*, size_t);
            bool                    damageAdd(const Region &);
            bool                    damageSubtrack(const Region &);

            GenericEvent            poolEvent(void) override;
        };

	class RootDisplayExt : public RootDisplay
	{
        protected:
            xcb_window_t            _selwin;
	    std::vector<uint8_t>    _selbuf;
            std::unique_ptr<Tools::BaseTimer>
                                    _timerClipCheck;

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
            bool                     selectionClearAction(xcb_selection_clear_event_t*);
            bool                     selectionRequestAction(xcb_selection_request_event_t*);

	public:
	    RootDisplayExt(const std::string & addr);
            ~RootDisplayExt();

            GenericEvent             poolEvent(void) override;

            bool                     selectionNotifyAction(xcb_selection_notify_event_t*);
            bool                     isSelectionNotify(const GenericEvent & ev) const;

            bool                         setClipboardEvent(std::vector<uint8_t> &);
	    const std::vector<uint8_t> & getSelectionData(void) const { return _selbuf; };
	};

	typedef std::shared_ptr<RootDisplayExt> SharedDisplay;
    }
}

#endif // _LTSM_XCB_WRAPPER_
