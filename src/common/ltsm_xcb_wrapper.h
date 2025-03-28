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
#include <shared_mutex>
#include <stdexcept>

#include "xcb/xcb.h"
#include "xcb/shm.h"
#include "xcb/randr.h"
#include "xcb/damage.h"
#include "xcb/xproto.h"

#define explicit dont_use_cxx_explicit
#include "xcb/xkb.h"
#undef explicit
#include "xkbcommon/xkbcommon-x11.h"

#ifdef LTSM_BUILD_XCB_ERRORS
#include "libxcb-errors/xcb_errors.h"
#endif

#include "ltsm_tools.h"

namespace LTSM
{
    struct xcb_error : public std::runtime_error
    {
        explicit xcb_error(std::string_view what) : std::runtime_error(what.data()) {}
    };

    namespace XCB
    {
        struct Point
        {
            int16_t x, y;

            Point() : x(-1), y(-1) {}

            explicit Point(const xcb_point_t & pt) : x(pt.x), y(pt.y) {}

            Point(int16_t px, int16_t py) : x(px), y(py) {}

            virtual ~Point() = default;

            virtual bool isValid(void) const { return 0 <= x && 0 <= y; }

            xcb_point_t toXcbPoint(void) const { return xcb_point_t{x, y}; }

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

            virtual ~Size() = default;

            bool isEmpty(void) const { return width == 0 || height == 0; }

            void reset(void) { width = 0; height = 0; }

            bool operator==(const Size & sz) const { return sz.width == width && sz.height == height; }

            bool operator!=(const Size & sz) const { return sz.width != width || sz.height != height; }
        };

        struct PointIterator : Point
        {
            const Size limit;
            PointIterator(int16_t px, int16_t py, const Size & sz) : Point(px, py), limit(sz) {}

            PointIterator & operator++(void);
            PointIterator & operator--(void);

            bool isValid(void) const override { return Point::isValid() && x < limit.width && y < limit.height; }

            bool isBeginLine(void) const;
            bool isEndLine(void) const;
            virtual void lineChanged(void) { /* default empty */ }
        };

        struct Region : public Point, public Size
        {
            Region() = default;

            Region(const Point & pt, const Size & sz) : Point(pt), Size(sz) {}

            explicit Region(const xcb_rectangle_t & rt) : Point(rt.x, rt.y), Size(rt.width, rt.height) {}

            Region(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) : Point(rx, ry), Size(rw, rh) {}

            const Point & topLeft(void) const { return *this; }

            const Size & toSize(void) const { return *this; }

            xcb_rectangle_t toXcbRect(void) const { return xcb_rectangle_t{x, y, width, height}; }

            PointIterator coordBegin(void) const { return PointIterator(0, 0, toSize()); }

            bool operator== (const Region & rt) const { return rt.x == x && rt.y == y && rt.width == width && rt.height == height; }

            bool operator!= (const Region & rt) const { return rt.x != x || rt.y != y || rt.width != width || rt.height != height; }

            void reset(void);

            void assign(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);
            void assign(const Region &);

            void join(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);
            void join(const Region &);

            bool empty(void) const;
            bool invalid(void) const;

            Region intersected(const Region &) const;
            Region align(size_t) const;

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

        struct RegionPixel : std::pair<Region, uint32_t>
        {
            RegionPixel(const Region & reg, uint32_t pixel) : std::pair<XCB::Region, uint32_t>(reg, pixel) {}

            RegionPixel() = default;

            const uint32_t & pixel(void) const { return second; }

            const Region & region(void) const { return first; }
        };

        typedef std::shared_ptr<xcb_connection_t> ConnectionShared;

        struct GenericError : std::unique_ptr<xcb_generic_error_t, void(*)(void*)>
        {
            explicit GenericError(xcb_generic_error_t* err = nullptr)
                : std::unique_ptr<xcb_generic_error_t, void(*)(void*)>(err, std::free) {}
        };

        struct GenericEvent : std::unique_ptr<xcb_generic_event_t, void(*)(void*)>
        {
            explicit GenericEvent(xcb_generic_event_t* ev = nullptr)
                : std::unique_ptr<xcb_generic_event_t, void(*)(void*)>(ev, std::free) {}

            const xcb_generic_error_t* toerror(void) const { return reinterpret_cast<const xcb_generic_error_t*>(get()); }
        };

        template<typename ReplyType>
        struct GenericReply : std::unique_ptr<ReplyType, void(*)(void*)>
        {
            explicit GenericReply(ReplyType* ptr) : std::unique_ptr<ReplyType, void(*)(void*)>(ptr, std::free) {}
        };

        struct PropertyReply : GenericReply<xcb_get_property_reply_t>
        {
            uint32_t length(void) { return xcb_get_property_value_length(get()); }

            void* value(void) { return xcb_get_property_value(get()); }

            PropertyReply(xcb_get_property_reply_t* ptr) : GenericReply<xcb_get_property_reply_t>(ptr) {}

            PropertyReply(GenericReply<xcb_get_property_reply_t> && ptr) noexcept : GenericReply<xcb_get_property_reply_t>(std::move(ptr)) {}
        };

        template<typename ReplyType>
        struct ReplyError : std::pair<GenericReply<ReplyType>, GenericError>
        {
            ReplyError(ReplyType* ptr, xcb_generic_error_t* err)
                : std::pair<GenericReply<ReplyType>, GenericError>(ptr, err)
            {
            }

            const GenericReply<ReplyType> & reply(void) const { return std::pair<GenericReply<ReplyType>, GenericError>::first; }

            const GenericError & error(void) const { return std::pair<GenericReply<ReplyType>, GenericError>::second; }
        };

        template<typename Reply, typename Cookie>
        ReplyError<Reply> getReply1(std::function<Reply*(xcb_connection_t*, Cookie, xcb_generic_error_t**)> func, xcb_connection_t* conn, Cookie cookie)
        {
            xcb_generic_error_t* error = nullptr;
            Reply* reply = func(conn, cookie, & error);
            return ReplyError<Reply>(reply, error);
        }

#define getReplyFunc1(NAME,conn,...) getReply1<NAME##_reply_t,NAME##_cookie_t>(NAME##_reply,conn,NAME(conn,##__VA_ARGS__))
#define getReplyUncheckedFunc1(NAME,conn,...) getReply1<NAME##_reply_t,NAME##_cookie_t>(NAME##_reply,conn,NAME##_unchecked(conn,##__VA_ARGS__))
#define NULL_KEYCODE 0

        template<typename Reply, typename Cookie>
        ReplyError<Reply> getReply2(std::function<Reply*(xcb_connection_t*, Cookie, xcb_generic_error_t**)> func, xcb_connection_t* conn, Cookie cookie)
        {
            return getReply1<Reply, Cookie>(func, conn, cookie);
        }

#define getReplyFunc2(NAME,conn,...) getReply2<NAME##_reply_t,NAME##_cookie_t>(NAME##_reply,conn,NAME(conn,##__VA_ARGS__))

        struct PixmapBase
        {
            uint32_t rmask = 0;
            uint32_t gmask = 0;
            uint32_t bmask = 0;
            uint8_t bpp = 0;

            virtual uint8_t* data(void) = 0;
            virtual const uint8_t* data(void) const = 0;
            virtual size_t size(void) const = 0;

            PixmapBase() = default;
            virtual ~PixmapBase() = default;

            PixmapBase(uint32_t rm, uint32_t gm, uint32_t bm, uint8_t pp) : rmask(rm), gmask(gm), bmask(bm), bpp(pp) {}

            uint8_t bitsPerPixel(void) const { return bpp; }

            uint8_t bytePerPixel(void) const { return bpp >> 3; }
        };

        typedef std::unique_ptr<PixmapBase> PixmapInfoReply;

        struct ShmId
        {
            std::weak_ptr<xcb_connection_t> conn;
            int shm = -1;
            uint8_t* addr = nullptr;
            xcb_shm_seg_t id = 0;

            ShmId(const std::weak_ptr<xcb_connection_t> & ptr, int s, uint8_t* a, const xcb_shm_seg_t & v) : conn(ptr), shm(s), addr(a), id(v) {}

            ShmId() = default;
            ~ShmId();

            ShmId(const ShmId &) = delete;
            ShmId & operator=(const ShmId &) = delete;

            void reset(void);

            explicit operator bool(void) const { return ! conn.expired() && 0 < id; };

            const xcb_shm_seg_t & operator()(void) const { return id; };
        };

        typedef std::shared_ptr<ShmId> ShmIdShared;

        struct PixmapSHM : PixmapBase
        {
            ShmIdShared shm;
            size_t len = 0;

            uint8_t* data(void) override { return shm ? shm->addr : nullptr; }

            const uint8_t* data(void) const override { return shm ? shm->addr : nullptr; }

            size_t size(void) const override { return len; }

            PixmapSHM() = default;
            PixmapSHM(uint32_t rmask, uint32_t gmask, uint32_t bmask, uint8_t bpp, ShmIdShared sh, size_t sz)
                : PixmapBase(rmask, gmask, bmask, bpp), shm(sh), len(sz) {}
        };

        struct PixmapBuffer : PixmapBase
        {
            std::vector<uint8_t> pixels;

            uint8_t* data(void) override { return pixels.data(); }

            const uint8_t* data(void) const override { return pixels.data(); }

            size_t size(void) const override { return pixels.size(); }

            PixmapBuffer() = default;
            PixmapBuffer(uint32_t rmask, uint32_t gmask, uint32_t bmask, uint8_t bpp, size_t res = 0)
                : PixmapBase(rmask, gmask, bmask, bpp) { pixels.reserve(res); }
        };

#define ReplyCursor ReplyError<xcb_xfixes_get_cursor_image_reply_t>

        struct CursorImage : ReplyCursor
        {
            uint32_t* data(void);
            const uint32_t* data(void) const;
            size_t size(void) const;

            CursorImage(ReplyCursor && rc) : ReplyCursor(std::move(rc)) {}
        };

        enum class Module { SHM, DAMAGE, XFIXES, RANDR, TEST, XKB, SELECTION };

        union xkb_notify_event_t
        {
            /* All XKB events share these fields. */
            struct
            {
                uint8_t response_type;
                uint8_t xkb_type;
                uint16_t sequence;
                xcb_timestamp_t time;
                uint8_t device_id;
            } any;

            xcb_xkb_new_keyboard_notify_event_t keyboard_notify;
            xcb_xkb_map_notify_event_t map_notify;
            xcb_xkb_state_notify_event_t state_notify;
        };

        struct RandrOutputInfo
        {
            bool connected = false;
            xcb_randr_crtc_t crtc = 0;
            uint32_t mm_width = 0;
            uint32_t mm_height = 0;
            std::string name;

            RandrOutputInfo() = default;
        };

        struct RandrCrtcInfo
        {
            xcb_randr_mode_t mode = 0;
            xcb_timestamp_t timestamp = 0;
            int16_t x = 0;
            int16_t y = 0;
            uint16_t width = 0;
            uint16_t height = 0;
            uint16_t rotation = 0;
            uint8_t status = 0;

            RandrCrtcInfo() = default;
        };

        struct RandrScreenInfo
        {
            xcb_timestamp_t timestamp = 0;
            xcb_timestamp_t config_timestamp = 0;
            uint16_t sizeID = 0;
            uint16_t rotation = 0;
            uint16_t rate = 0;

            RandrScreenInfo() = default;
        };

        typedef std::vector<uint8_t> AuthCookie;

        struct SelectionIncrMode
        {
            xcb_window_t requestor = 0;
            uint32_t size = 0;
            uint16_t sequence = 0;

            SelectionIncrMode(xcb_window_t win, uint32_t sz, uint16_t seq) : requestor(win), size(sz), sequence(seq) {}
        };

        struct ModuleExtension
        {
            std::weak_ptr<xcb_connection_t> conn;
            Module type;
            const xcb_query_extension_reply_t* ext = nullptr;

            ModuleExtension(const std::weak_ptr<xcb_connection_t> & ptr, const Module & mod) : conn(ptr), type(mod) {}

            virtual ~ModuleExtension() = default;

            bool isModule(const Module & mod) const { return mod == type; }

            bool isEventType(const GenericEvent &, int) const;
            bool isEventError(const GenericEvent &, uint16_t* opcode = nullptr) const;
        };

        struct WindowDamageId
        {
            std::weak_ptr<xcb_connection_t> conn;
            xcb_drawable_t win = 0;
            xcb_damage_damage_t xid = 0;

            WindowDamageId(const std::weak_ptr<xcb_connection_t> & ptr, const xcb_drawable_t & w, const xcb_damage_damage_t & v) : conn(ptr), win(w), xid(v) {}
            ~WindowDamageId();

            bool valid(void) const { return ! conn.expired() && 0 < xid; };

            const xcb_damage_damage_t & id(void) const { return xid; };

            bool addRegion(const xcb_xfixes_region_t &);
            bool subtrackRegion(const xcb_xfixes_region_t &);
        };

        typedef std::unique_ptr<WindowDamageId> WindowDamageIdPtr;

        struct ModuleDamage : ModuleExtension
        {
            explicit ModuleDamage(const ConnectionShared &);

            WindowDamageIdPtr createDamage(xcb_drawable_t win, const xcb_damage_report_level_t &) const;
        };

        struct FixesRegionId
        {
            std::weak_ptr<xcb_connection_t> conn;
            xcb_xfixes_region_t xid = 0;

            FixesRegionId(const std::weak_ptr<xcb_connection_t> & ptr, const xcb_xfixes_region_t & v) : conn(ptr), xid(v) {}
            ~FixesRegionId();

            bool valid(void) const { return ! conn.expired() && 0 < xid; };

            const xcb_xfixes_region_t & id(void) const { return xid; };
        };

        typedef std::unique_ptr<FixesRegionId> FixesRegionIdPtr;

        struct ModuleFixes : ModuleExtension
        {
            explicit ModuleFixes(const ConnectionShared &);

            FixesRegionIdPtr createRegion(const xcb_rectangle_t &) const;
            FixesRegionIdPtr createRegions(const xcb_rectangle_t*, size_t counts) const;

            FixesRegionIdPtr unionRegions(const xcb_xfixes_region_t &, xcb_xfixes_region_t &) const;
            FixesRegionIdPtr intersectRegions(const xcb_xfixes_region_t &, xcb_xfixes_region_t &) const;

            xcb_rectangle_t fetchRegion(const xcb_xfixes_region_t &) const;
            std::vector<xcb_rectangle_t> fetchRegions(const xcb_xfixes_region_t &) const;

            CursorImage getCursorImage(void) const;
            std::string getCursorName(const xcb_cursor_t &) const;
        };

        struct ModuleTest : ModuleExtension
        {
            explicit ModuleTest(const ConnectionShared &);

            bool fakeInputRaw(xcb_window_t, uint8_t type, uint8_t detail, int16_t posx, int16_t posy) const;
            void fakeInputClickButton(xcb_window_t win, uint8_t button, const Point &) const;
        };

        struct ModuleSelection : ModuleExtension
        {
            xcb_window_t win = XCB_WINDOW_NONE;

            std::vector<uint8_t> buf;
            std::mutex lock;

            std::unique_ptr<SelectionIncrMode> incr;
            xcb_atom_t atombuf = XCB_ATOM_NONE;

            ModuleSelection(const ConnectionShared &, const xcb_screen_t &, xcb_atom_t);
            ~ModuleSelection();

            bool sendNotifyTargets(const xcb_selection_request_event_t &);
            bool sendNotifySelData(const xcb_selection_request_event_t &);

            bool clearAction(const xcb_selection_clear_event_t*);
            bool requestAction(const xcb_selection_request_event_t*);
            bool fixesAction(const xcb_xfixes_selection_notify_event_t*);

            void notifyIncrStart(const xcb_selection_notify_event_t &);
            bool notifyIncrContinue(xcb_atom_t type);
            bool notifyAction(const xcb_selection_notify_event_t*, xcb_atom_t, bool syncPrimaryClipboard);

            bool setBuffer(const uint8_t* buf, size_t len, std::initializer_list<xcb_atom_t> atoms);
        };

        struct ModuleRandr : ModuleExtension
        {
            explicit ModuleRandr(const ConnectionShared &);

            std::vector<xcb_randr_output_t> getOutputs(const xcb_screen_t &) const;
            std::vector<xcb_randr_crtc_t> getCrtcs(const xcb_screen_t &) const;
            std::vector<xcb_randr_output_t> getCrtcOutputs(const xcb_randr_crtc_t &, RandrCrtcInfo* = nullptr) const;
            std::vector<xcb_randr_mode_info_t> getModesInfo(const xcb_screen_t &) const;
            std::vector<xcb_randr_mode_t> getOutputModes(const xcb_randr_output_t &, RandrOutputInfo* = nullptr) const;
            std::vector<xcb_randr_crtc_t> getOutputCrtcs(const xcb_randr_output_t &, RandrOutputInfo* = nullptr) const;
            std::vector<xcb_randr_screen_size_t> getScreenSizes(const xcb_screen_t &, RandrScreenInfo* = nullptr) const;

            std::unique_ptr<RandrCrtcInfo> getCrtcInfo(const xcb_randr_crtc_t &) const;
            std::unique_ptr<RandrOutputInfo> getOutputInfo(const xcb_randr_output_t &) const;
            std::unique_ptr<RandrScreenInfo> getScreenInfo(const xcb_screen_t &) const;

            bool setScreenSizeCompat(const xcb_screen_t &, Size, uint16_t* sequence = nullptr) const;

            bool setScreenSize(const xcb_screen_t &, uint16_t width, uint16_t height, uint16_t dpi = 96) const;
            xcb_randr_mode_t cvtCreateMode(const xcb_screen_t &, const Size &, int vertRef = 60) const;
            bool destroyMode(const xcb_randr_mode_t &) const;
            bool addOutputMode(const xcb_randr_output_t &, const xcb_randr_mode_t &) const;
            bool deleteOutputMode(const xcb_randr_output_t &, const xcb_randr_mode_t &) const;
            bool crtcConnectOutputsMode(const xcb_screen_t &, const xcb_randr_crtc_t &, int16_t posx, int16_t posy, const std::vector<xcb_randr_output_t> &, const xcb_randr_mode_t &) const;
            bool crtcDisconnect(const xcb_screen_t &, const xcb_randr_crtc_t &) const;
        };

        struct ModuleShm : ModuleExtension
        {
            explicit ModuleShm(const ConnectionShared &);

            ShmIdShared createShm(size_t shmsz, int mode, bool readOnly, uid_t owner = 0) const;
        };

        struct ModuleXkb : ModuleExtension
        {
            std::unique_ptr<struct xkb_context, decltype(xkb_context_unref)*> ctx;
            std::unique_ptr<struct xkb_keymap, decltype(xkb_keymap_unref)*> map;
            std::unique_ptr<struct xkb_state, decltype(xkb_state_unref)*> state;

            int32_t devid = -1;

            explicit ModuleXkb(const ConnectionShared &);

            bool resetMapState(void);

            int getLayoutGroup(void) const;
            bool switchLayoutGroup(int group = -1) const;
            std::vector<std::string> getNames(void) const;
        };

#ifdef LTSM_BUILD_XCB_ERRORS
        struct ErrorContext
        {
            xcb_errors_context_t* ctx = nullptr;

            explicit ErrorContext(xcb_connection_t*);
            ~ErrorContext();

            bool error(const xcb_generic_error_t* err, const char* func, const char* xcbname) const;
        };

#endif

        class Connector
        {
        protected:
            ConnectionShared _conn;
            const xcb_setup_t* _setup = nullptr;

#ifdef LTSM_BUILD_XCB_ERRORS
            std::unique_ptr<ErrorContext> _error;
#endif
        protected:
            void extendedError(const xcb_generic_error_t* error, const char* func, const char* name) const;

        public:
            Connector() = default;
            virtual ~Connector() = default;

            /// exception: xcb_error
            Connector(int displayNum, const AuthCookie* = nullptr);

            virtual bool displayConnect(int displayNum, const AuthCookie* = nullptr);


            size_t depthFromBpp(size_t bitsPerPixel) const;
            size_t bppFromDepth(size_t depth) const;

            const xcb_setup_t* setup(void) const;

            xcb_connection_t* xcb_ptr(void);
            const xcb_connection_t* xcb_ptr(void) const;

            int hasError(void) const;
            static const char* errorString(int err);

            GenericError checkRequest(const xcb_void_cookie_t &) const;

            xcb_atom_t getAtom(std::string_view, bool create = true) const;
            bool checkAtom(std::string_view) const;
            std::string getAtomName(xcb_atom_t) const;

            size_t getMaxRequest(void) const;

            bool setWindowGeometry(xcb_window_t win, const Region &);
            std::list<xcb_window_t> getWindowChilds(xcb_window_t win) const;

            bool deleteProperty(xcb_window_t win, xcb_atom_t prop) const;
            PropertyReply getPropertyInfo(xcb_window_t win, xcb_atom_t prop) const;
            xcb_atom_t getPropertyType(xcb_window_t win, xcb_atom_t prop) const;

            std::list<xcb_atom_t> getPropertiesList(xcb_window_t win) const;

            xcb_atom_t getPropertyAtom(xcb_window_t win, xcb_atom_t prop, uint32_t offset = 0) const;
            xcb_window_t getPropertyWindow(xcb_window_t win, xcb_atom_t prop, uint32_t offset = 0) const;
            uint32_t getPropertyCardinal(xcb_window_t win, xcb_atom_t prop, uint32_t offset = 0) const;
            int getPropertyInteger(xcb_window_t win, xcb_atom_t prop, uint32_t offset = 0) const;
            std::string getPropertyString(xcb_window_t win, xcb_atom_t prop) const;

            std::list<xcb_atom_t> getPropertyAtomList(xcb_window_t win, xcb_atom_t prop) const;
            std::list<xcb_window_t> getPropertyWindowList(xcb_window_t win, xcb_atom_t prop) const;
            std::list<uint32_t> getPropertyCardinalList(xcb_window_t win, xcb_atom_t prop) const;
            std::list<int> getPropertyIntegerList(xcb_window_t win, xcb_atom_t prop) const;
            std::list<std::string> getPropertyStringList(xcb_window_t win, xcb_atom_t prop) const;
        };

        class RootDisplay : public Connector
        {
        protected:
            std::unique_ptr<ModuleShm> _modShm;
            std::unique_ptr<ModuleDamage> _modDamage;
            std::unique_ptr<ModuleFixes> _modFixes;
            std::unique_ptr<ModuleTest> _modTest;
            std::unique_ptr<ModuleRandr> _modRandr;
            std::unique_ptr<ModuleXkb> _modXkb;
            std::unique_ptr<ModuleSelection> _modSelection;

            xcb_screen_t* _screen = nullptr;
            xcb_format_t* _format = nullptr;
            xcb_visualtype_t* _visual = nullptr;

            mutable std::shared_mutex _lockGeometry;
            std::unique_ptr<WindowDamageId> _damage;

            xcb_keycode_t _minKeycode = 0;
            xcb_keycode_t _maxKeycode = 0;

        protected:

            bool createFullScreenDamage(void);

            bool isDamageNotify(const GenericEvent &) const;
            bool isXFixesSelectionNotify(const GenericEvent &) const;
            bool isXFixesCursorNotify(const GenericEvent &) const;
            bool isRandrScreenNotify(const GenericEvent &) const;
            bool isRandrNotify(const GenericEvent &, const xcb_randr_notify_t &) const;
            bool isXkbNotify(const GenericEvent & ev, int notify) const;

            bool selectionFixesAction(xcb_xfixes_selection_notify_event_t*);
            bool selectionClearAction(xcb_selection_clear_event_t*);
            bool selectionRequestAction(xcb_selection_request_event_t*);
            bool selectionNotifyAction(xcb_selection_notify_event_t*, bool syncPrimaryClipboard = false);


        public:
            RootDisplay() = default;
            ~RootDisplay();

            RootDisplay(int displayNum, const AuthCookie* = nullptr);

            bool displayConnect(int displayNum, const AuthCookie* = nullptr) override;

            uint16_t width(void) const;
            uint16_t height(void) const;
            Region region(void) const;
            Size size(void) const;
            size_t depth(void) const;
            size_t bitsPerPixel(void) const;
            size_t scanlinePad(void) const;
            const xcb_visualtype_t* visual(void) const;
            xcb_window_t root(void) const;

            void reconnect(int displayNum, const AuthCookie* = nullptr);
            const ModuleExtension* getExtension(const Module &) const;

            bool setRandrScreenSize(const Size &, uint16_t* sequence = nullptr);
            bool setRandrMonitors(const std::vector<Region> & monitors);

            virtual void displayConnectedEvent(void) { /*default empty */ }

            virtual void xfixesSelectionChangedEvent(void) { /*default empty */ }

            virtual void xfixesCursorChangedEvent(void) { /*default empty */ }

            virtual void damageRegionEvent(const Region &) { /*default empty */ }

            virtual void randrScreenSetSizeEvent(const Size &) { /*default empty */ }

            virtual void randrScreenChangedEvent(const Size &, const xcb_randr_notify_event_t &) { /*default empty */ }

            virtual void xkbGroupChangedEvent(int) { /*default empty */ }

            virtual void clipboardChangedEvent(const std::vector<uint8_t> &) { /*default empty */ }

            const xcb_visualtype_t* visual(xcb_visualid_t) const;

            void fillRegion(int r, int g, int b, const Region &);
            void fillBackground(int r, int g, int b);

            bool setClipboard(const uint8_t*, size_t);

            PixmapInfoReply copyRootImageRegion(const Region &, ShmIdShared = nullptr) const;

            void resetInputs(void);
            void fakeInputKeycode(xcb_keycode_t, bool pressed) const;
            void fakeInputKeysym(xcb_keysym_t, bool pressed);
            void fakeInputButton(int button, const Point &) const;

            xcb_keycode_t keysymToKeycode(xcb_keysym_t) const;
            xcb_keycode_t keysymGroupToKeycode(xcb_keysym_t, int group) const;
            std::pair<xcb_keycode_t, int>
            keysymToKeycodeGroup(xcb_keysym_t keysym) const;

            bool damageAdd(const xcb_rectangle_t*, size_t);
            bool damageAdd(const Region &);
            bool damageSubtrack(const Region &);
            void damageDisable(void);

            GenericEvent poolEvent(void);
            Size updateGeometrySize(void) const;
        };

        class XkbClient
        {
            std::unique_ptr<xcb_connection_t, decltype(xcb_disconnect)*> conn{ nullptr, xcb_disconnect };
            std::unique_ptr<xkb_context, decltype(xkb_context_unref)*> xkbctx{ nullptr, xkb_context_unref };
            std::unique_ptr<xkb_keymap, decltype(xkb_keymap_unref)*> xkbmap{ nullptr, xkb_keymap_unref };
            std::unique_ptr<xkb_state, decltype(xkb_state_unref)*> xkbstate{ nullptr, xkb_state_unref };

            const xcb_query_extension_reply_t* xkbext = nullptr;
            int32_t xkbdevid = -1;
            xcb_keycode_t minKeycode = 0;
            xcb_keycode_t maxKeycode = 0;
            std::atomic<bool> error{false};

        public:
            XkbClient();

            int xkbGroup(void) const;
            std::vector<std::string> xkbNames(void) const;
            std::string atomName(xcb_atom_t) const;

            bool xcbEventProcessing(void);
            bool xcbError(void) const;

            std::pair<xcb_keycode_t, int>
            keysymToKeycodeGroup(xcb_keysym_t) const;
            xcb_keysym_t keycodeGroupToKeysym(xcb_keycode_t, int group, bool shifted = false) const;

            void bell(uint8_t percent) const;

            virtual void xkbStateChangeEvent(int) { /*default empty */ }

            virtual void xkbStateResetEvent(void) { /*default empty */ }
        };
    }
}

#endif // _LTSM_XCB_WRAPPER_
