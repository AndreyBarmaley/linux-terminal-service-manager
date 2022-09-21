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
#include <vector>
#include <string_view>
#include <stdexcept>

#include "xcb/xcb.h"
#include "xcb/shm.h"
#include "xcb/randr.h"
#include "xcb/damage.h"
#include "xcb/xproto.h"

#ifdef LTSM_WITH_XKBCOMMON
#define explicit dont_use_cxx_explicit
#include "xcb/xkb.h"
#undef explicit
#include "xkbcommon/xkbcommon-x11.h"
#endif

#include "ltsm_tools.h"

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

            bool isValid(void) const { return 0 <= x && 0 <= y; }

            Point operator+(const Point & pt) const { return Point(x + pt.x, y + pt.y); }
            Point operator-(const Point & pt) const { return Point(x - pt.x, y - pt.y); }

            bool operator==(const Point & pt) const { return pt.x == x && pt.y == y; }
            bool operator!=(const Point & pt) const { return pt.x != x || pt.y != y; }
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

	struct PointIterator : Point
	{
	    const Size & 	limit;
    	    PointIterator(int16_t px, int16_t py, const Size & sz) : Point(px, py), limit(sz) {}

    	    PointIterator &	operator++(void);
    	    PointIterator &	operator--(void);

	    bool		isValid(void) const { return Point::isValid() && x < limit.width && y < limit.height; }
	};

	struct Region : public Point, public Size
	{
            Region() {}
            Region(const Point & pt, const Size & sz) : Point(pt), Size(sz) {}
            Region(const xcb_rectangle_t & rt) : Point(rt.x, rt.y), Size(rt.width, rt.height) {}
            Region(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) : Point(rx, ry), Size(rw, rh) {}

            const Point & topLeft(void) const { return *this; }
            const Size &  toSize(void) const { return *this; }

    	    PointIterator coordBegin(void) const { return PointIterator(0, 0, toSize()); }

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

            std::list<Region> divideBlocks(const Size &) const;
            std::list<Region> divideCounts(uint16_t cols, uint16_t rows) const;
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

        struct RegionPixel : std::pair<XCB::Region, uint32_t>
        {
            RegionPixel(const XCB::Region & reg, uint32_t pixel) : std::pair<XCB::Region, uint32_t>(reg, pixel) {}
            RegionPixel() {}
        
            const uint32_t &    pixel(void) const { return second; }
            const XCB::Region & region(void) const { return first; }
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

        struct PropertyReply : XCB::GenericReply<xcb_get_property_reply_t>
        {
            uint32_t    length(void) { return xcb_get_property_value_length(get()); }
            void*       value(void) { return xcb_get_property_value(get()); }

            PropertyReply(xcb_get_property_reply_t* ptr) : XCB::GenericReply<xcb_get_property_reply_t>(ptr) {}
            PropertyReply(const XCB::GenericReply<xcb_get_property_reply_t> & ptr) : XCB::GenericReply<xcb_get_property_reply_t>(ptr) {}
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
	ReplyError<Reply> getReply1(std::function<Reply*(xcb_connection_t*, Cookie, xcb_generic_error_t**)> func, xcb_connection_t* conn, Cookie cookie)
	{
    	    xcb_generic_error_t* error = nullptr;
    	    Reply* reply = func(conn, cookie, & error);
    	    return ReplyError<Reply>(reply, error);
	}

	#define getReplyFunc1(NAME,conn,...) getReply1<NAME##_reply_t,NAME##_cookie_t>(NAME##_reply,conn,NAME(conn,##__VA_ARGS__))
	#define NULL_KEYCODE 0

        struct KeyCodes : std::shared_ptr<xcb_keycode_t>
        {
//            KeyCodes(xcb_keycode_t* ptr)
//		: std::shared_ptr<xcb_keycode_t>(ptr, std::free) {}

            KeyCodes(xcb_keycode_t code)
		: std::shared_ptr<xcb_keycode_t>(new xcb_keycode_t[2]{code, NULL_KEYCODE}, std::default_delete<xcb_keycode_t[]>()) {}

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

            bool                    addRegion(xcb_drawable_t winid, xcb_xfixes_region_t regid) const;
            bool                    subtractRegion(xcb_xfixes_region_t regid, xcb_xfixes_region_t parts) const;

            uint32_t                xid(void) const { return get() ? get()->xcb : 0; }
            xcb_connection_t*       connection(void) const { return get() ? get()->conn : nullptr; }
        };

        struct PixmapInfoBase
        {
            size_t                  _depth;
	    xcb_visualid_t         _visid;

            size_t                 depth(void) const { return _depth; }
            xcb_visualid_t         visId(void) const { return _visid; }

            virtual uint8_t*       data(void) = 0;
            virtual const uint8_t* data(void) const = 0;
            virtual size_t         size(void) const = 0;

            virtual ~PixmapInfoBase() {}
            PixmapInfoBase(uint8_t d = 0, xcb_visualid_t v = 0) : _depth(d), _visid(v) {}
        };
        typedef std::shared_ptr<PixmapInfoBase> PixmapInfoReply;

        struct SHM : std::shared_ptr<shm_t>
        {
            SHM() {}
            SHM(int shmid, uint8_t* addr, xcb_connection_t*);

            PixmapInfoReply       getPixmapRegion(xcb_drawable_t, const Region &, size_t offset = 0, uint32_t planeMask = 0xFFFFFFFF) const;

            uint8_t*              data(void) { return get() ? get()->addr : nullptr; }
            const uint8_t*        data(void) const { return get() ? get()->addr : nullptr; }

            xcb_connection_t*     connection(void) const { return get() ? get()->conn : nullptr; }
            uint32_t              xid(void) const { return get() ? get()->xcb : 0; }
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

        enum class Module { SHM, DAMAGE, XFIXES, RANDR, TEST, XKB };

#ifdef LTSM_WITH_XKBCOMMON
	union xkb_notify_event_t
	{
    	    /* All XKB events share these fields. */
    	    struct
	    {
        	uint8_t		response_type;
        	uint8_t		xkb_type;
        	uint16_t	sequence;
        	xcb_timestamp_t time;
        	uint8_t		device_id;
    	    } any;

    	    xcb_xkb_new_keyboard_notify_event_t keyboard_notify;
    	    xcb_xkb_map_notify_event_t		map_notify;
    	    xcb_xkb_state_notify_event_t	state_notify;
	};
#endif

	struct RandrOutputInfo
	{
	    bool		connected;
	    xcb_randr_crtc_t	crtc;
	    uint32_t		mm_width;
	    uint32_t		mm_height;
	    std::string		name;

	    RandrOutputInfo() : connected(false), crtc(0), mm_width(0), mm_height(0) {}
	};

	struct RandrScreenInfo
	{
	    xcb_timestamp_t 	config_timestamp;
	    uint16_t 		sizeID;
	    uint16_t 		rotation;
	    uint16_t 		rate;
	    RandrScreenInfo() : config_timestamp(0), sizeID(0), rotation(0), rate(0) {}
	};

        struct connector_error : public std::runtime_error
        {
            connector_error(const char* what) : std::runtime_error(what){}
        };

        class Connector
        {
        protected:
            xcb_connection_t*       _conn = nullptr;
            const xcb_setup_t*      _setup = nullptr;

        public:
            Connector(std::string_view addr);
            virtual ~Connector();

            size_t	            pixmapDepth(size_t bitsPerPixel) const;
            size_t	            pixmapBitsPerPixel(size_t depth) const;
            const xcb_setup_t*      setup(void) const;

	    template<typename Reply, typename Cookie>
	    ReplyError<Reply> getReply2(std::function<Reply*(xcb_connection_t*, Cookie, xcb_generic_error_t**)> func, Cookie cookie) const
	    {
                return XCB::getReply1<Reply, Cookie>(func, _conn, cookie);
	    }

	    #define getReplyFunc2(NAME,conn,...) getReply2<NAME##_reply_t,NAME##_cookie_t>(NAME##_reply,NAME(conn,##__VA_ARGS__))

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

            bool                    isXkbStateNotify(const GenericEvent & ev) const;
            bool                    isXkbKeyboardNotify(const GenericEvent & ev) const;
            bool                    isXkbMapNotify(const GenericEvent & ev) const;

            void                    extendedError(const xcb_generic_error_t* error, const char* func) const;
            void                    extendedError(const GenericError &, const char* func) const;

            GenericError            checkRequest(const xcb_void_cookie_t &) const;
            virtual GenericEvent    poolEvent(void);

            GC                      createGC(xcb_drawable_t winid, uint32_t value_mask = 0, const void* value_list = nullptr);
            SHM                     createSHM(size_t, int mode = 0600, bool readOnly = false);
            Damage                  createDamage(xcb_drawable_t winid, int level = XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
            XFixesRegion            createFixesRegion(const Region &);
            XFixesRegion            createFixesRegion(const xcb_rectangle_t* rect, size_t num);
	    xcb_atom_t              getAtom(std::string_view, bool create = true) const;
	    bool                    checkAtom(std::string_view) const;
	    std::string	            getAtomName(xcb_atom_t) const;
	    size_t                  getMaxRequest(void) const;

            static bool             testConnection(std::string_view addr);

            PropertyReply           getPropertyAnyType(xcb_window_t win, xcb_atom_t prop, uint32_t offset = 0, uint32_t length = 0xFFFFFFFF);
            xcb_atom_t              getPropertyType(xcb_window_t win, xcb_atom_t prop);
            xcb_atom_t              getPropertyAtom(xcb_window_t win, xcb_atom_t prop, uint32_t offset = 0);
            xcb_window_t            getPropertyWindow(xcb_window_t win, xcb_atom_t prop, uint32_t offset = 0);
            std::string             getPropertyString(xcb_window_t win, xcb_atom_t prop, uint32_t offset = 0);
            std::list<std::string>  getPropertyStringList(xcb_window_t win, xcb_atom_t prop);
            std::vector<uint8_t>    getPropertyBinary(xcb_window_t win, xcb_atom_t prop, xcb_atom_t type);
        };

        class RootDisplay : public Connector
        {
	protected:
            xcb_screen_t*           _screen = nullptr;
            xcb_format_t*           _format = nullptr;
            xcb_visualtype_t*       _visual = nullptr;

            Damage                  _damage;
            SHM                     _shm;

            xcb_keycode_t           _minKeycode = 0;
            xcb_keycode_t           _maxKeycode = 0;

#ifdef LTSM_WITH_XKBCOMMON
            std::unique_ptr<struct xkb_context, decltype(xkb_context_unref)*> _xkbctx;
            std::unique_ptr<struct xkb_keymap, decltype(xkb_keymap_unref)*> _xkbmap;
	    std::unique_ptr<struct xkb_state, decltype(xkb_state_unref)*> _xkbstate;
	    int32_t                 _xkbdevid = -1;
#endif

        public:
            RootDisplay(std::string_view addr);
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

            const xcb_visualtype_t* visual(xcb_visualid_t) const;

            void                    resetInputs(void);
            void                    fillRegion(int r, int g, int b, const Region &);
            void                    fillBackground(int r, int g, int b);

            PixmapInfoReply         copyRootImageRegion(const Region &, uint32_t planeMask = 0xFFFFFFFF) const;

            void                    fakeInputKeycode(xcb_keycode_t, bool pressed);
            void                    fakeInputKeysym(xcb_keysym_t, bool pressed);
            void                    fakeInputButton(int button, const Point &);
            bool                    fakeInputTest(int type, int detail, int posx, int posy);

            xcb_keycode_t           keysymToKeycode(xcb_keysym_t) const;
            xcb_keycode_t           keysymGroupToKeycode(xcb_keysym_t, int group) const;
	    std::pair<xcb_keycode_t, int>
                                    keysymToKeycodeGroup(xcb_keysym_t keysym) const;
            int                     getXkbLayoutGroup(void) const;
            bool                    switchXkbLayoutGroup(int group = -1) const;
            std::list<std::string>  getXkbNames(void) const;

	    std::vector<xcb_randr_output_t>      getRandrOutputs(void) const;
	    RandrOutputInfo                      getRandrOutputInfo(const xcb_randr_output_t &) const;
	    std::vector<xcb_randr_mode_t>        getRandrOutputModes(const xcb_randr_output_t &) const;
	    std::vector<xcb_randr_crtc_t>        getRandrOutputCrtcs(const xcb_randr_output_t &) const;
	    RandrScreenInfo                      getRandrScreenInfo(void) const;
	    std::vector<xcb_randr_screen_size_t> getRandrScreenSizes(RandrScreenInfo* = nullptr) const;
	    std::vector<xcb_randr_mode_info_t>   getRandrModesInfo(void) const;


	    bool                    setRandrScreenSize(uint16_t windth, uint16_t height, uint16_t* sequence = nullptr);
	    xcb_randr_mode_t        createRandrMode(uint16_t width, uint16_t height);
	    bool                    destroyRandrMode(const xcb_randr_mode_t  &);
	    bool                    addRandrOutputMode(const xcb_randr_output_t &, const xcb_randr_mode_t &);
	    bool                    deleteRandrOutputMode(const xcb_randr_output_t &, const xcb_randr_mode_t &);

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

	    xcb_atom_t              _atoms[7] = { XCB_ATOM_NONE };
	    xcb_atom_t &            _atomPrimary;
	    xcb_atom_t &            _atomClipboard;
	    xcb_atom_t &            _atomBuffer;
	    xcb_atom_t &            _atomTargets;
	    xcb_atom_t &            _atomText;
	    xcb_atom_t &            _atomTextPlain;
	    xcb_atom_t &            _atomUTF8;

	    xcb_window_t             getOwnerSelection(const xcb_atom_t &);

	    bool                     getSelectionEvent(const xcb_atom_t &);
	    bool                     setClipboardEvent(std::vector<uint8_t> &&, std::initializer_list<xcb_atom_t>);

            bool                     sendNotifyTargets(const xcb_selection_request_event_t &);
            bool                     sendNotifySelData(const xcb_selection_request_event_t &);
            bool                     selectionClearAction(xcb_selection_clear_event_t*);
            bool                     selectionRequestAction(xcb_selection_request_event_t*);

	public:
	    RootDisplayExt(std::string_view addr);
            ~RootDisplayExt();

            GenericEvent             poolEvent(void) override;

            bool                     selectionNotifyAction(xcb_selection_notify_event_t*);
            bool                     isSelectionNotify(const GenericEvent & ev) const;

            bool                         setClipboardEvent(const uint8_t*, size_t);
            bool                         setClipboardEvent(std::vector<uint8_t> &&);
	    const std::vector<uint8_t> & getSelectionData(void) const { return _selbuf; };
	};

	typedef std::shared_ptr<RootDisplayExt> SharedDisplay;
    }
}

#endif // _LTSM_XCB_WRAPPER_
