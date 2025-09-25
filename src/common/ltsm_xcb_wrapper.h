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

#ifdef LTSM_WITH_XCB_ERRORS
#include "libxcb-errors/xcb_errors.h"
#endif

#include "ltsm_tools.h"
#include "ltsm_xcb_types.h"

namespace LTSM
{
    namespace XCB
    {
        using ConnectionShared = std::shared_ptr<xcb_connection_t>;

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

        using PixmapInfoReply = std::unique_ptr<PixmapBase>;

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

        using ShmIdShared = std::shared_ptr<ShmId>;

        struct PixmapSHM : PixmapBase
        {
            ShmIdShared shm;
            size_t len = 0;

            uint8_t* data(void) override { return shm ? shm->addr : nullptr; }

            const uint8_t* data(void) const override { return shm ? shm->addr : nullptr; }

            size_t size(void) const override { return len; }

            PixmapSHM() = default;
            PixmapSHM(uint32_t rmask, uint32_t gmask, uint32_t bmask, uint8_t bpp, const ShmIdShared & sh, size_t sz)
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

        using AuthCookie = std::vector<uint8_t>;

        struct SelectionIncrMode
        {
            xcb_window_t requestor = 0;
            uint32_t size = 0;
            uint16_t sequence = 0;

            SelectionIncrMode(xcb_window_t win, uint32_t sz, uint16_t seq) : requestor(win), size(sz), sequence(seq) {}
        };

        // Module
        enum class Module { SHM, DAMAGE, WINDAMAGE, XFIXES, WINFIXES, RANDR, TEST, XKB, SELECTION_COPY, SELECTION_PASTE };

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

        struct FixesRegionId
        {
            std::weak_ptr<xcb_connection_t> conn;
            xcb_xfixes_region_t xid = 0;

            FixesRegionId(const std::weak_ptr<xcb_connection_t> & ptr, const xcb_xfixes_region_t & v) : conn(ptr), xid(v) {}
            ~FixesRegionId();
        };

        using FixesRegionIdPtr = std::unique_ptr<FixesRegionId>;

        struct ModuleFixes : ModuleExtension
        {
            explicit ModuleFixes(const ConnectionShared &);

            FixesRegionIdPtr createRegion(const xcb_rectangle_t &) const;
            FixesRegionIdPtr createRegions(const xcb_rectangle_t*, size_t counts) const;

            FixesRegionIdPtr unionRegions(const xcb_xfixes_region_t &, xcb_xfixes_region_t &) const;
            FixesRegionIdPtr intersectRegions(const xcb_xfixes_region_t &, xcb_xfixes_region_t &) const;

            xcb_rectangle_t fetchRegion(const xcb_xfixes_region_t &) const;
            std::vector<xcb_rectangle_t> fetchRegions(const xcb_xfixes_region_t &) const;
        };

        struct ModuleWindowFixes : public ModuleFixes
        {
            xcb_window_t win = 0;

            explicit ModuleWindowFixes(const ConnectionShared &, xcb_drawable_t win);
            ~ModuleWindowFixes();

            CursorImage getCursorImage(void) const;
            std::string getCursorName(const xcb_cursor_t &) const;
        };

        struct ModuleDamage : ModuleExtension
        {
            explicit ModuleDamage(const ConnectionShared &);
        };

        struct ModuleWindowDamage : public ModuleDamage
        {
            xcb_drawable_t win = 0;
            xcb_damage_damage_t xid = 0;

            explicit ModuleWindowDamage(const ConnectionShared &, xcb_drawable_t win);
            ~ModuleWindowDamage();

            bool addRegion(const xcb_rectangle_t &) const;
            bool addRegion(const Region &) const;
            bool addRegions(const xcb_rectangle_t* rects, size_t counts) const;

            bool subtrackRegion(const xcb_rectangle_t &) const;
            bool subtrackRegion(const Region &) const;
        };

        struct ModuleTest : ModuleExtension
        {
            xcb_window_t screen = 0;
            mutable std::array<xcb_keycode_t, 16> keycodes;

            explicit ModuleTest(const ConnectionShared &, xcb_window_t win);
            ~ModuleTest();

            bool fakeInputRaw(xcb_window_t, uint8_t type, uint8_t detail, int16_t posx, int16_t posy) const;

            void screenInputReset(void) const;
            void screenInputKeycode(xcb_keycode_t, bool pressed) const;
            void screenInputButton(uint8_t button, const Point &, bool pressed) const;
            void screenInputButtonClick(uint8_t button, const Point &) const;
            void screenInputMove(const Point &) const;
        };

        /// SelectionSource interface
        class SelectionSource
        {
        public:
            virtual std::vector<xcb_atom_t> selectionSourceTargets(void) const = 0;

            virtual size_t selectionSourceSize(xcb_atom_t) const = 0;
            virtual std::vector<uint8_t> selectionSourceData(xcb_atom_t, size_t offset, uint32_t length) const = 0;
            virtual bool selectionSourceReady(xcb_atom_t) const { return true; }

            virtual void selectionSourceLock(xcb_atom_t) const {}
            virtual void selectionSourceUnlock(xcb_atom_t) const {}

            SelectionSource() = default;
            virtual ~SelectionSource() = default;
        };

        struct WindowRequest
        {
            xcb_selection_request_event_t ev;
            size_t offset;

            WindowRequest(const xcb_selection_request_event_t & r) : ev(r), offset(0) {}
        };

        class ModulePasteSelection : public ModuleExtension
        {
            const SelectionSource* source = nullptr;

            // destinations
            std::list<WindowRequest> requestsIncr;

            std::string selectionName;

            // selection type: clipboard, primary, etc
            xcb_atom_t selectionType = XCB_ATOM_NONE;

            // selection fake win source
            xcb_window_t selectionWin = XCB_WINDOW_NONE;

            // skip requestor win
            xcb_window_t skipRequestorWin = XCB_WINDOW_NONE;

            xcb_timestamp_t selectionTime = 0;

        protected:
            void discardRequestor(const ConnectionShared &, const xcb_selection_request_event_t &);
            bool removeRequestors(xcb_window_t win);

            void eventRequestDebug(const ConnectionShared &, const xcb_selection_request_event_t*, bool warn = false) const;
            void sendNotifyEvent(const ConnectionShared &, const xcb_selection_request_event_t*, xcb_atom_t) const;

            inline void sendNotifyDiscard(const ConnectionShared & ptr, const xcb_selection_request_event_t* ev) const { sendNotifyEvent(ptr, ev, XCB_ATOM_NONE); }
            inline void eventRequestWarning(const ConnectionShared & ptr, const xcb_selection_request_event_t* ev) const { eventRequestDebug(ptr, ev, true); }

        public:
            ModulePasteSelection(const ConnectionShared &, const xcb_screen_t &, xcb_atom_t = XCB_ATOM_NONE /* default: CLIPBOARD */);
            ~ModulePasteSelection();

            void setSelectionOwner(const SelectionSource &);

            void destroyNotifyEvent(const xcb_destroy_notify_event_t*);
            void propertyNotifyEvent(const xcb_property_notify_event_t*);
            void selectionClearEvent(const xcb_selection_clear_event_t*);
            void selectionRequestEvent(const xcb_selection_request_event_t*);

            void setSkipRequestor(xcb_window_t win) { skipRequestorWin = win; }
        };

        /// SelectionRecipient interface
        class SelectionRecipient
        {
        public:
            virtual void selectionReceiveData(xcb_atom_t, const uint8_t* ptr, uint32_t len) const = 0;
            virtual void selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) const = 0;
            virtual void selectionChangedEvent(void) const = 0;

            SelectionRecipient() = default;
            virtual ~SelectionRecipient() = default;
        };

        struct WindowSource
        {
            xcb_selection_notify_event_t ev;
            std::vector<uint8_t> buf;

            WindowSource(const xcb_selection_notify_event_t & r, size_t sz) : ev(r)
            {
                buf.reserve(sz);
            }
        };

        class ModuleCopySelection : public ModuleExtension
        {
            const SelectionRecipient* recipient = nullptr;

            // incr source
            std::unique_ptr<WindowSource> sourceIncr;

            std::string selectionName;

            // selection type: clipboard, primary, etc
            xcb_atom_t selectionType = XCB_ATOM_NONE;

            xcb_atom_t selectionProp = XCB_ATOM_NONE;
            xcb_atom_t selectionTrgt = XCB_ATOM_NONE;

            // selection fake win source
            xcb_window_t selectionWin = XCB_WINDOW_NONE;

            // xfixes input win
            xcb_window_t xfixesWin = XCB_WINDOW_NONE;

        protected:
            void eventNotifyDebug(const ConnectionShared &, const xcb_selection_notify_event_t*, bool warn = false) const;
            inline void eventNotifyWarning(const ConnectionShared & ptr, const xcb_selection_notify_event_t* ev) const { eventNotifyDebug(ptr, ev, true); }

            void xfixesSetSelectionOwnerEvent(const xcb_xfixes_selection_notify_event_t*);
            void xfixesSelectionWindowDestroyEvent(const xcb_xfixes_selection_notify_event_t*);
            void xfixesSelectionClientCloseEvent(const xcb_xfixes_selection_notify_event_t*);

        public:
            ModuleCopySelection(const ConnectionShared &, const xcb_screen_t &, xcb_atom_t = XCB_ATOM_NONE /* default: CLIPBOARD */);
            ~ModuleCopySelection();

            void convertSelection(xcb_atom_t target, const SelectionRecipient &);

            void propertyNotifyEvent(const xcb_property_notify_event_t*);

            void selectionNotifyEvent(const xcb_selection_notify_event_t*);
            void xfixesSelectionNotifyEvent(const xcb_xfixes_selection_notify_event_t*);

            const xcb_window_t & selectionWindow(void) const { return selectionWin; }
        };

        /// ModuleRandr
        struct ModuleRandr : ModuleExtension
        {
            xcb_window_t screen;

            explicit ModuleRandr(const ConnectionShared &, xcb_window_t);
            ~ModuleRandr();

            std::vector<xcb_randr_output_t> getOutputs(void) const;
            std::vector<xcb_randr_crtc_t> getCrtcs(void) const;
            std::vector<xcb_randr_output_t> getCrtcOutputs(const xcb_randr_crtc_t &, RandrCrtcInfo* = nullptr) const;
            std::vector<xcb_randr_mode_info_t> getModesInfo(void) const;
            std::vector<xcb_randr_mode_t> getOutputModes(const xcb_randr_output_t &, RandrOutputInfo* = nullptr) const;
            std::vector<xcb_randr_crtc_t> getOutputCrtcs(const xcb_randr_output_t &, RandrOutputInfo* = nullptr) const;
            std::vector<xcb_randr_screen_size_t> getScreenSizes(RandrScreenInfo* = nullptr) const;

            std::unique_ptr<RandrCrtcInfo> getCrtcInfo(const xcb_randr_crtc_t &) const;
            std::unique_ptr<RandrOutputInfo> getOutputInfo(const xcb_randr_output_t &) const;
            std::unique_ptr<RandrScreenInfo> getScreenInfo(void) const;

            bool setScreenSizeCompat(uint16_t width, uint16_t height, uint16_t* sequence = nullptr) const;
            bool setScreenSize(uint16_t width, uint16_t height, uint16_t dpi = 96) const;

            xcb_randr_mode_t cvtCreateMode(const Size &, int vertRef = 60) const;
            bool destroyMode(const xcb_randr_mode_t &) const;
            bool addOutputMode(const xcb_randr_output_t &, const xcb_randr_mode_t &) const;
            bool deleteOutputMode(const xcb_randr_output_t &, const xcb_randr_mode_t &) const;
            bool crtcConnectOutputsMode(const xcb_randr_crtc_t &, int16_t posx, int16_t posy, const std::vector<xcb_randr_output_t> &, const xcb_randr_mode_t &) const;
            bool crtcDisconnect(const xcb_randr_crtc_t &) const;
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
            ~ModuleXkb();

            bool resetMapState(void);

            int getLayoutGroup(void) const;
            bool switchLayoutGroup(int group = -1) const;
            std::vector<std::string> getNames(void) const;
        };

#ifdef LTSM_WITH_XCB_ERRORS
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

#ifdef LTSM_WITH_XCB_ERRORS
            std::unique_ptr<ErrorContext> _error;
#endif
        protected:
            void extendedError(const xcb_generic_error_t* error, const char* func, const char* name) const;

        public:
            Connector() = default;
            virtual ~Connector() = default;

            /// exception: xcb_error
            Connector(int displayNum, const AuthCookie* = nullptr);

            bool connectorDisplayConnect(int displayNum, const AuthCookie* = nullptr);

            size_t depthFromBpp(size_t bitsPerPixel) const;
            size_t bppFromDepth(size_t depth) const;

            std::pair<int, int> displayScreen(void) const;
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
            void bell(uint8_t percent) const;

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

        enum InitModules { All = 0xFFFF, Shm = 0x0001, Damage = 0x0002, XFixes = 0x0004, RandR = 0x0008, Test = 0x0010, Xkb = 0x0020, SelCopy = 0x0040, SelPaste = 0x0080 };

        class RootDisplay : public Connector
        {
        protected:
            std::unique_ptr<ModuleShm> _modShm;
            std::unique_ptr<ModuleWindowFixes> _modWinFixes;
            std::unique_ptr<ModuleWindowDamage> _modWinDamage;
            std::unique_ptr<ModuleTest> _modTest;
            std::unique_ptr<ModuleRandr> _modRandr;
            std::unique_ptr<ModuleXkb> _modXkb;
            std::unique_ptr<ModulePasteSelection> _modSelectionPaste;
            std::unique_ptr<ModuleCopySelection> _modSelectionCopy;

            xcb_screen_t* _screen = nullptr;
            xcb_format_t* _format = nullptr;
            xcb_visualtype_t* _visual = nullptr;

            mutable std::shared_mutex _lockGeometry;

        protected:

            const ModuleExtension* getExtensionConst(const Module &) const;
            bool createFullScreenDamage(void);

            bool isDamageNotify(const GenericEvent &) const;
            bool isXFixesSelectionNotify(const GenericEvent &) const;
            bool isXFixesCursorNotify(const GenericEvent &) const;
            bool isRandrScreenNotify(const GenericEvent &) const;
            bool isRandrNotify(const GenericEvent &, const xcb_randr_notify_t &) const;
            bool isXkbNotify(const GenericEvent & ev, int notify) const;

        public:
            RootDisplay() = default;
            RootDisplay(int displayNum, const AuthCookie* = nullptr);

            bool displayConnect(int displayNum, int modules = InitModules::All, const AuthCookie* = nullptr);

            uint16_t width(void) const;
            uint16_t height(void) const;
            Region region(void) const;
            Size size(void) const;
            size_t depth(void) const;
            size_t bitsPerPixel(void) const;
            size_t scanlinePad(void) const;
            const xcb_visualtype_t* visual(void) const;
            xcb_window_t root(void) const;

            void displayReconnect(int displayNum, int modules = InitModules::All, const AuthCookie* = nullptr);

            ModuleExtension* getExtension(const Module &);
            const ModuleExtension* getExtension(const Module &) const;
            void extensionDisable(const Module &);

            bool setRandrScreenSize(const Size &, uint16_t* sequence = nullptr);
            bool setRandrMonitors(const std::vector<Region> & monitors);

            // root display events
            virtual void xcbDisplayConnectedEvent(void) { /*default empty */ }
            virtual void xcbFixesCursorChangedEvent(void) { /*default empty */ }
            virtual void xcbDamageNotifyEvent(const xcb_rectangle_t &) { /*default empty */ }
            virtual void xcbRandrScreenSetSizeEvent(const Size &) { /*default empty */ }
            virtual void xcbRandrScreenChangedEvent(const Size &, const xcb_randr_notify_event_t &) { /*default empty */ }
            virtual void xcbXkbGroupChangedEvent(int) { /*default empty */ }

            const xcb_visualtype_t* visual(xcb_visualid_t) const;

            void fillRegion(uint8_t r, uint8_t g, uint8_t b, const Region &);
            void fillBackground(uint8_t r, uint8_t g, uint8_t b);

            PixmapInfoReply copyRootImageRegion(const Region &, ShmIdShared = nullptr) const;

            xcb_keycode_t keysymToKeycode(xcb_keysym_t) const;
            xcb_keycode_t keysymToKeycodeAuto(xcb_keysym_t) const;
            xcb_keycode_t keysymGroupToKeycode(xcb_keysym_t, int group) const;
            std::pair<xcb_keycode_t, int> keysymToKeycodeGroup(xcb_keysym_t keysym) const;
            xcb_keysym_t keycodeGroupToKeysym(xcb_keycode_t, int group, bool shifted = false) const;

            bool rootDamageAddRegion(const Region &);
            bool rootDamageAddRegions(const xcb_rectangle_t*, size_t);
            bool rootDamageSubtrack(const Region &);

            GenericEvent pollEvent(void);
            Size updateGeometrySize(void) const;
        };
    }
}

#endif // _LTSM_XCB_WRAPPER_
