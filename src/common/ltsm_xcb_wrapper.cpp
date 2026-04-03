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

#include <errno.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <filesystem>

#include "xcb/xtest.h"
#include "xcb/xproto.h"
#include "xcb/damage.h"
#include "xcb/randr.h"

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_framebuffer.h"

using namespace std::chrono_literals;

namespace LTSM {
    namespace XCB {
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        const PixelFormat pfx30 = RGB30;
        const PixelFormat pf888 = RGBX32;
        const PixelFormat pf565 = RGB565;
        const PixelFormat pf555 = RGB555;
#else
        const PixelFormat pfx30 = BGR30;
        const PixelFormat pf888 = BGRX32;
        const PixelFormat pf565 = BGR565;
        const PixelFormat pf555 = BGR555;
#endif

        inline xcb_rectangle_t regionToXcb(const Region & reg) {
            return xcb_rectangle_t{reg.x, reg.y, reg.width, reg.height};
        }

        void error(const xcb_generic_error_t* err, std::string_view func, std::string_view xcbname) {
            Application::error("{}: {} failed, error code: {}, major: {:#04x}, minor: {:#06x}, sequence: {}",
                               func, xcbname, err->error_code, err->major_code, err->minor_code, err->sequence);
        }

        void error(xcb_connection_t* conn, const xcb_generic_error_t* err, std::string_view func, std::string_view xcbname) {
#ifdef LTSM_WITH_XCB_ERRORS

            if(! conn || ! ErrorContext(conn).error(err, func, xcbname))
#endif
                error(err, func, xcbname);
        }

        /* Atom */
        namespace Atom {
            xcb_atom_t timestamp = XCB_ATOM_NONE;
            xcb_atom_t wmName = XCB_ATOM_NONE;
            xcb_atom_t utf8String = XCB_ATOM_NONE;

            xcb_atom_t incr = XCB_ATOM_NONE;

            xcb_atom_t primary = XCB_ATOM_NONE;
            xcb_atom_t clipboard = XCB_ATOM_NONE;
            xcb_atom_t targets = XCB_ATOM_NONE;

            xcb_atom_t getAtom(xcb_connection_t* conn, std::string_view name, bool create) {
                auto xcbReply = XCB::getReplyFunc2(xcb_intern_atom, conn, create ? 0 : 1, name.size(), name.data());

                if(const auto & err = xcbReply.error()) {
                    error(conn, err.get(), NS_FuncNameV, "xcb_intern_atom");
                    return XCB_ATOM_NONE;
                }

                if(const auto & reply = xcbReply.reply()) {
                    return reply->atom;
                }

                return XCB_ATOM_NONE;
            }

            std::string getName(xcb_connection_t* conn, xcb_atom_t atom) {
                if(atom == XCB_ATOM_NONE) {
                    return "NONE";
                }

                auto xcbReply = XCB::getReplyFunc2(xcb_get_atom_name, conn, atom);

                if(const auto & err = xcbReply.error()) {
                    error(conn, err.get(), NS_FuncNameV, "xcb_get_atom_name");
                    return "NONE";
                }

                if(const auto & reply = xcbReply.reply()) {
                    const char* name = xcb_get_atom_name_name(reply.get());
                    size_t len = xcb_get_atom_name_name_length(reply.get());
                    return std::string(name, len);
                }

                return "NONE";
            }
        } // Atom

        PropertyReply getPropertyInfo(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t prop) {
            auto xcbReply = getReplyFunc2(xcb_get_property, conn, false, win, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 0);

            if(const auto & err = xcbReply.error()) {
                error(conn, err.get(), NS_FuncNameV, "xcb_get_property");
            }

            return PropertyReply(std::move(xcbReply.first));
        }

        AtomName::AtomName(xcb_connection_t* conn, xcb_atom_t at) : name{Atom::getName(conn, at)}, atom{at} {
        }
    } // XCB
} // LTSM

template <>
struct fmt::formatter<LTSM::XCB::AtomName> {
    constexpr auto parse(fmt::format_parse_context& ctx) {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const LTSM::XCB::AtomName& at, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "atom[{:#010x}, {}]", at.atom, at.name);
    }
};

namespace LTSM {
    /* XCB::CursorImage */
    uint32_t* XCB::CursorImage::data(void) {
        return reply() ? xcb_xfixes_get_cursor_image_cursor_image(reply().get()) : nullptr;
    }

    const uint32_t* XCB::CursorImage::data(void) const {
        return reply() ? xcb_xfixes_get_cursor_image_cursor_image(reply().get()) : nullptr;
    }

    size_t XCB::CursorImage::size(void) const {
        return reply() ? xcb_xfixes_get_cursor_image_cursor_image_length(reply().get()) : 0;
    }

    /* ModuleExtension */
    bool XCB::ModuleExtension::isEventType(const GenericEvent & ev, int type) const {
        // clear bit
        auto response_type = ev ? ev->response_type & ~0x80 : 0;

        if(ext && 0 < response_type) {
            return response_type == ext->first_event + type;
        }

        return false;
    }

    bool XCB::ModuleExtension::isEventError(const GenericEvent & ev, uint16_t* opcode) const {
        if(ext && ev && ev->response_type == 0) {
            auto err = ev.toerror();

            if(err &&
               err->major_code == ext->major_opcode) {
                if(opcode) {
                    *opcode = err->minor_code;
                }

                if(auto ptr = conn.lock()) {
                    error(ptr.get(), err, NS_FuncNameV, "");
                }

                return true;
            }
        }

        return false;
    }

    // XCB::ModuleFixes
    XCB::FixesRegionId::~FixesRegionId() {
        if(auto ptr = conn.lock(); 0 < xid && ptr) {
            xcb_xfixes_destroy_region(ptr.get(), xid);
        }
    }

    XCB::ModuleFixes::ModuleFixes(const ConnectionShared & ptr) : ModuleExtension(ptr, Module::XFIXES) {
        ext = xcb_get_extension_data(ptr.get(), & xcb_xfixes_id);

        if(! ext || ! ext->present) {
            Application::error("{}: extension not found: {}", NS_FuncNameV, "XFIXES");
            throw xcb_error(NS_FuncNameS);
        }

        auto xcbReply = getReplyFunc2(xcb_xfixes_query_version, ptr.get(), XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_query_version");
            throw xcb_error(NS_FuncNameS);
        }

        if(const auto & reply = xcbReply.reply()) {
            Application::debug(DebugType::Xcb, "{}: extension version: {}.{}",
                               NS_FuncNameV, reply->major_version, reply->minor_version);
        }
    }

    XCB::FixesRegionIdPtr XCB::ModuleFixes::createRegion(const xcb_rectangle_t & rect) const {
        if(auto ptr = conn.lock()) {
            xcb_xfixes_region_t res = xcb_generate_id(ptr.get());

            auto cookie = xcb_xfixes_create_region_checked(ptr.get(), res, 1, & rect);

            if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_create_region");
                res = 0;
            }

            Application::debug(DebugType::Xcb, "{}: rect: {}, resource id: {:#010x}",
                               NS_FuncNameV, rect, res);

            return std::make_unique<FixesRegionId>(conn, res);
        }

        return nullptr;
    }

    XCB::FixesRegionIdPtr XCB::ModuleFixes::createRegions(const xcb_rectangle_t* rects, size_t counts) const {
        if(auto ptr = conn.lock()) {
            xcb_xfixes_region_t res = xcb_generate_id(ptr.get());

            auto cookie = xcb_xfixes_create_region_checked(ptr.get(), res, counts, rects);

            if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_create_region");
                res = 0;
            }

            Application::debug(DebugType::Xcb, "{}: rects: {}, resource id: {:#010x}", NS_FuncNameV, counts, res);
            return std::make_unique<FixesRegionId>(conn, res);
        }

        return nullptr;
    }

    XCB::FixesRegionIdPtr XCB::ModuleFixes::intersectRegions(const xcb_xfixes_region_t & reg1, xcb_xfixes_region_t & reg2) const {
        if(auto ptr = conn.lock()) {
            xcb_xfixes_region_t res = xcb_generate_id(ptr.get());

            auto cookie = xcb_xfixes_intersect_region_checked(ptr.get(), reg1, reg2, res);

            if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_intersect_region");
                res = 0;
            }

            Application::debug(DebugType::Xcb, "{}: reg1 id: {:#010x}, reg2 id: {:#010x}, resource id: {:#010x}", NS_FuncNameV, reg1, reg2, res);
            return std::make_unique<FixesRegionId>(conn, res);
        }

        return nullptr;
    }

    XCB::FixesRegionIdPtr XCB::ModuleFixes::unionRegions(const xcb_xfixes_region_t & reg1, xcb_xfixes_region_t & reg2) const {
        if(auto ptr = conn.lock()) {
            xcb_xfixes_region_t res = xcb_generate_id(ptr.get());

            auto cookie = xcb_xfixes_union_region_checked(ptr.get(), reg1, reg2, res);

            if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_union_region");
                res = 0;
            }

            Application::debug(DebugType::Xcb, "{}: reg1 id: {:#010x}, reg2 id: {:#010x}, resource id: {:#010x}", NS_FuncNameV, reg1, reg2, res);
            return std::make_unique<FixesRegionId>(conn, res);
        }

        return nullptr;
    }

    xcb_rectangle_t XCB::ModuleFixes::fetchRegion(const xcb_xfixes_region_t & reg) const {
        if(auto ptr = conn.lock()) {
            auto xcbReply = getReplyFunc2(xcb_xfixes_fetch_region, ptr.get(), reg);

            if(const auto & err = xcbReply.error()) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_fetch_region");
            }

            if(const auto & reply = xcbReply.reply()) {
                auto rects = xcb_xfixes_fetch_region_rectangles(reply.get());
                int count = xcb_xfixes_fetch_region_rectangles_length(reply.get());

                if(rects && 0 < count) {
                    return rects[0];
                }
            }
        }

        return xcb_rectangle_t{0, 0, 0, 0};
    }

    std::vector<xcb_rectangle_t> XCB::ModuleFixes::fetchRegions(const xcb_xfixes_region_t & reg) const {
        if(auto ptr = conn.lock()) {
            auto xcbReply = getReplyFunc2(xcb_xfixes_fetch_region, ptr.get(), reg);

            if(const auto & err = xcbReply.error()) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_fetch_region");
            }

            if(const auto & reply = xcbReply.reply()) {
                auto rects = xcb_xfixes_fetch_region_rectangles(reply.get());
                int count = xcb_xfixes_fetch_region_rectangles_length(reply.get());

                if(rects && 0 < count) {
                    return std::vector<xcb_rectangle_t>(rects, rects + count);
                }
            }
        }

        return {};
    }

    // XCB::ModuleWindowFixes
    XCB::ModuleWindowFixes::ModuleWindowFixes(const ConnectionShared & ptr, xcb_window_t wid) : ModuleFixes(ptr), win(wid) {
        type = Module::WINFIXES;
        xcb_xfixes_select_cursor_input(ptr.get(), win, XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
    }

    XCB::ModuleWindowFixes::~ModuleWindowFixes() {
        if(auto ptr = conn.lock()) {
            xcb_xfixes_select_cursor_input(ptr.get(), win, 0);
        }
    }

    XCB::CursorImage XCB::ModuleWindowFixes::getCursorImage(void) const {
        if(auto ptr = conn.lock()) {
            auto xcbReply = getReplyFunc2(xcb_xfixes_get_cursor_image, ptr.get());

            if(const auto & err = xcbReply.error()) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_get_cursor_image");
            }

            return xcbReply;
        }

        return ReplyCursor{nullptr, nullptr};
    }

    std::string XCB::ModuleWindowFixes::getCursorName(const xcb_cursor_t & cur) const {
        if(auto ptr = conn.lock()) {
            auto xcbReply = getReplyFunc2(xcb_xfixes_get_cursor_name, ptr.get(), cur);

            if(const auto & err = xcbReply.error()) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xfixes_get_cursor_name");
            }

            if(const auto & reply = xcbReply.reply()) {
                auto ptr = xcb_xfixes_get_cursor_name_name(reply.get());
                int len = xcb_xfixes_get_cursor_name_name_length(reply.get());

                if(ptr && 0 < len) {
                    return std::string(ptr, ptr + len);
                }
            }
        }

        return "";
    }

    // XCB::ModuleDamage
    XCB::ModuleDamage::ModuleDamage(const ConnectionShared & ptr) : ModuleExtension(ptr, Module::DAMAGE) {
        ext = xcb_get_extension_data(ptr.get(), & xcb_damage_id);

        if(! ext || ! ext->present) {
            Application::error("{}: extension not found: {}", NS_FuncNameV, "DAMAGE");
            throw xcb_error(NS_FuncNameS);
        }

        auto xcbReply = getReplyFunc2(xcb_damage_query_version, ptr.get(), XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_damage_query_version");
            throw xcb_error(NS_FuncNameS);
        }

        if(const auto & reply = xcbReply.reply()) {
            Application::debug(DebugType::Xcb, "{}: extension version: {}.{}",
                               NS_FuncNameV, reply->major_version, reply->minor_version);
        }
    }

    // XCB::ModuleWindowDamage
    XCB::ModuleWindowDamage::ModuleWindowDamage(const ConnectionShared & ptr, xcb_drawable_t draw)
        : ModuleDamage(ptr), win(draw) {
        type = Module::WINDAMAGE;
        xid = xcb_generate_id(ptr.get());

        auto cookie = xcb_damage_create_checked(ptr.get(), xid, win, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_damage_create");
            throw xcb_error(NS_FuncNameS);
        }

        Application::debug(DebugType::Xcb, "{}: resource id: {:#010x}", NS_FuncNameV, xid);
    }

    XCB::ModuleWindowDamage::~ModuleWindowDamage() {
        if(auto ptr = conn.lock(); 0 < xid && ptr) {
            xcb_damage_destroy(ptr.get(), xid);
        }
    }

    bool XCB::ModuleWindowDamage::addRegion(const Region & reg) const {
        return addRegion(regionToXcb(reg));
    }

    bool XCB::ModuleWindowDamage::addRegion(const xcb_rectangle_t & reg) const {
        if(addRegions(& reg, 1)) {
            Application::debug(DebugType::Xcb, "{}: damage: {:#010x}, window: {:#010x}, region: {}",
                               NS_FuncNameV, xid, win, reg);

            return true;
        }

        return false;
    }

    bool XCB::ModuleWindowDamage::addRegions(const xcb_rectangle_t* rects, size_t counts) const {
        if(! rects || 0 == counts) {
            return false;
        }

        if(auto ptr = conn.lock()) {
            xcb_xfixes_region_t regid = xcb_generate_id(ptr.get());

            xcb_xfixes_create_region(ptr.get(), regid, counts, rects);
            xcb_damage_add_checked(ptr.get(), win, regid);
            xcb_xfixes_destroy_region(ptr.get(), regid);

            return true;
        }

        return false;
    }

    bool XCB::ModuleWindowDamage::subtrackRegion(const Region & reg) const {
        return subtrackRegion(regionToXcb(reg));
    }

    bool XCB::ModuleWindowDamage::subtrackRegion(const xcb_rectangle_t & reg) const {
        if(auto ptr = conn.lock()) {
            xcb_xfixes_region_t regid = xcb_generate_id(ptr.get());

            xcb_xfixes_create_region_checked(ptr.get(), regid, 1, & reg);
            xcb_damage_subtract_checked(ptr.get(), xid, regid, XCB_XFIXES_REGION_NONE);
            xcb_xfixes_destroy_region(ptr.get(), regid);

            Application::debug(DebugType::Xcb, "{}: damage: {:#010x}, window: {:#010x}, region: {}",
                               NS_FuncNameV, xid, win, reg);

            return true;
        }

        return false;
    }

    // XCB::ModuleTest
    XCB::ModuleTest::ModuleTest(const ConnectionShared & ptr, xcb_window_t root) : ModuleExtension(ptr, Module::TEST), screen(root) {
        ext = xcb_get_extension_data(ptr.get(), & xcb_test_id);

        if(! ext || ! ext->present) {
            Application::error("{}: extension not found: {}", NS_FuncNameV, "XTEST");
            throw xcb_error(NS_FuncNameS);
        }

        auto xcbReply = getReplyFunc2(xcb_test_get_version, ptr.get(), XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_test_query_version");
            throw xcb_error(NS_FuncNameS);
        }

        if(const auto & reply = xcbReply.reply()) {
            Application::debug(DebugType::Xcb, "{}: extension, version: {}.{}",
                               NS_FuncNameV, reply->major_version, reply->minor_version);
        }

        std::ranges::fill(keycodes, NULL_KEYCODE);
    }

    XCB::ModuleTest::~ModuleTest() {
        screenInputReset();
    }

    /// @param type: XCB_KEY_PRESS, XCB_KEY_RELEASE, XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE, XCB_MOTION_NOTIFY
    /// @param detail: keycode or left 1, middle 2, right 3, scrollup 4, scrolldw 5
    bool XCB::ModuleTest::fakeInputRaw(xcb_window_t win, uint8_t type, uint8_t detail, int16_t posx, int16_t posy) const {
        if(auto ptr = conn.lock()) {
            auto cookie = xcb_test_fake_input_checked(ptr.get(), type, detail, XCB_CURRENT_TIME, win, posx, posy, 0);

            if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_test_fake_input");
                return false;
            }

            return true;
        }

        return false;
    }

    void XCB::ModuleTest::screenInputKeycode(xcb_keycode_t keycode, bool pressed) const {
        if(auto ptr = conn.lock()) {
            xcb_test_fake_input(ptr.get(), pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, XCB_CURRENT_TIME, screen, 0, 0, 0);
            xcb_flush(ptr.get());
        }
    }

    void XCB::ModuleTest::screenInputButton(uint8_t button, const Point & pos, bool pressed) const {
        if(auto ptr = conn.lock()) {
            xcb_test_fake_input(ptr.get(), pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, screen, pos.x, pos.y, 0);
            xcb_flush(ptr.get());
        }
    }

    void XCB::ModuleTest::screenInputMove(const Point & pos) const {
        if(auto ptr = conn.lock()) {
            xcb_test_fake_input(ptr.get(), XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, screen, pos.x, pos.y, 0);
            xcb_flush(ptr.get());
        }
    }

    void XCB::ModuleTest::screenInputButtonClick(uint8_t button, const Point & pos) const {
        if(auto ptr = conn.lock()) {
            xcb_test_fake_input(ptr.get(), XCB_BUTTON_PRESS, button, XCB_CURRENT_TIME, screen, pos.x, pos.y, 0);
            xcb_test_fake_input(ptr.get(), XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, screen, pos.x, pos.y, 0);

            xcb_flush(ptr.get());
        }
    }

    void XCB::ModuleTest::screenInputReset(void) const {
        if(auto ptr = conn.lock()) {
            // release all buttons
            for(uint8_t button = 1; button <= 5; button++) {
                xcb_test_fake_input(ptr.get(), XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, screen, 0, 0, 0);
            }

            // release pressed keys
            for(auto & code : keycodes) {
                if(code != NULL_KEYCODE) {
                    xcb_test_fake_input(ptr.get(), XCB_KEY_RELEASE, code, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
                    code = NULL_KEYCODE;
                }
            }

            xcb_flush(ptr.get());
        }
    }

    // XCB::ModuleRandr
    XCB::ModuleRandr::ModuleRandr(const ConnectionShared & ptr, xcb_window_t root) : ModuleExtension(ptr, Module::RANDR), screen(root) {
        ext = xcb_get_extension_data(ptr.get(), & xcb_randr_id);

        if(! ext || ! ext->present) {
            Application::error("{}: extension not found: {}", NS_FuncNameV, "RANDR");
            throw xcb_error(NS_FuncNameS);
        }

        auto xcbReply = getReplyFunc2(xcb_randr_query_version, ptr.get(), XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_query_version");
            throw xcb_error(NS_FuncNameS);
        }

        if(const auto & reply = xcbReply.reply()) {
            Application::debug(DebugType::Xcb, "{}: extension version: {}.{}",
                               NS_FuncNameV, reply->major_version, reply->minor_version);
        }

        // create randr notify
        xcb_randr_select_input(ptr.get(), screen,
                               XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);
    }

    XCB::ModuleRandr::~ModuleRandr() {
        if(auto ptr = conn.lock()) {
            xcb_randr_select_input(ptr.get(), screen, 0);
        }
    }

    XCB::RandrScreenResources XCB::ModuleRandr::getScreenResources(void) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return {};
        }

        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_resources, ptr.get(), screen);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_get_screen_resources");
            return {};
        }

        return RandrScreenResources(std::move(xcbReply.first));
    }

    std::span<const xcb_randr_output_t> XCB::RandrScreenResources::getOutputs(void) const {
        xcb_randr_output_t* ptr = xcb_randr_get_screen_resources_outputs(get());
        size_t len = xcb_randr_get_screen_resources_outputs_length(get());

        return {ptr, len};
    }

    std::span<const xcb_randr_crtc_t> XCB::RandrScreenResources::getCrtcs(void) const {
        xcb_randr_crtc_t* ptr = xcb_randr_get_screen_resources_crtcs(get());
        size_t len = xcb_randr_get_screen_resources_crtcs_length(get());

        return {ptr, len};
    }

    std::span<const xcb_randr_mode_info_t> XCB::RandrScreenResources::getModesInfo(void) const {
        xcb_randr_mode_info_t* ptr = xcb_randr_get_screen_resources_modes(get());
        size_t len = xcb_randr_get_screen_resources_modes_length(get());

        return {ptr, len};
    }

    bool XCB::RandrScreenResources::findMode(const Size& sz, xcb_randr_mode_t* res) const {
        auto modes = getModesInfo();

        auto it = std::ranges::find_if(modes, [&](auto & info) {
            return info.width == sz.width && info.height == sz.height;
        });

        if(it == modes.end()) {
            return false;
        }
        
        if(res) {
            *res = it->id;
        }
        
        return true;
    }

    std::unique_ptr<XCB::RandrCrtcInfo> XCB::ModuleRandr::getCrtcInfo(const xcb_randr_crtc_t & id, const xcb_timestamp_t & configTime) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return nullptr;
        }

        auto xcbReply = getReplyFunc2(xcb_randr_get_crtc_info, ptr.get(), id, configTime);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_get_crtc_info");
            return nullptr;
        }

        if(const auto & reply = xcbReply.reply()) {
            auto res = std::make_unique<RandrCrtcInfo>();

            res->mode = reply->mode;
            res->timestamp = reply->timestamp;
            res->x = reply->x;
            res->y = reply->y;
            res->width = reply->width;
            res->height = reply->height;
            res->rotation = reply->rotation;
            res->status = reply->status;

            return res;
        }

        return nullptr;
    }

    XCB::RandrOutputInfo::RandrOutputInfo(const xcb_randr_get_output_info_reply_t & reply) {
        connected = reply.connection == XCB_RANDR_CONNECTION_CONNECTED;
        crtc = reply.crtc;
        mm_width = reply.mm_width;
        mm_height = reply.mm_height;

        if(auto ptr = xcb_randr_get_output_info_name(&reply)) {
            int len = xcb_randr_get_output_info_name_length(&reply);
            name.assign(reinterpret_cast<const char*>(ptr), len);
        }

        if(auto ptr = xcb_randr_get_output_info_modes(&reply)) {
            int len = xcb_randr_get_output_info_modes_length(&reply);
            modes.assign(ptr, ptr + len);
        }

        if(auto ptr = xcb_randr_get_output_info_crtcs(&reply)) {
            int len = xcb_randr_get_output_info_crtcs_length(&reply);
            crtcs.assign(ptr, ptr + len);
        }
    }

    bool XCB::RandrOutputInfo::modeValid(const xcb_randr_mode_t & mode) const {
        return std::ranges::any_of(modes, [&](auto & val) {
            return mode == val;
        });
    }

    bool XCB::RandrOutputInfo::crtcValid(const xcb_randr_crtc_t & crtc) const {
        return std::ranges::any_of(crtcs, [&](auto & val) {
            return crtc == val;
        });
    }

    std::unique_ptr<XCB::RandrOutputInfo> XCB::ModuleRandr::getOutputInfo(const xcb_randr_output_t & id, const xcb_timestamp_t & configTime) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return nullptr;
        }

        auto xcbReply = getReplyFunc2(xcb_randr_get_output_info, ptr.get(), id, configTime);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_get_output_info");
            return nullptr;
        }

        if(const auto & reply = xcbReply.reply()) {
            return std::make_unique<RandrOutputInfo>(*reply);
        }

        return nullptr;
    }

    std::unique_ptr<XCB::RandrScreenInfo> XCB::ModuleRandr::getScreenInfo(void) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return nullptr;
        }

        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_info, ptr.get(), screen);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_get_screen_info");
            return nullptr;
        }

        if(const auto & reply = xcbReply.reply()) {
            auto res = std::make_unique<XCB::RandrScreenInfo>();

            res->config_timestamp = reply->config_timestamp;
            res->sizeID = reply->sizeID;
            res->rotation = reply->rotation;
            res->rate = reply->rate;

            return res;
        }

        return nullptr;
    }

    xcb_randr_mode_t XCB::ModuleRandr::cvtCreateMode(const XCB::Size & sz, int vertRef) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            throw xcb_error(NS_FuncNameS);
        }

        if(0 >= vertRef) {
            Application::error("{}: vertRef incorrect", NS_FuncNameV);
            throw xcb_error(NS_FuncNameS);
        }

        auto cvt = "/usr/bin/cvt";
        std::error_code err;

        if(! std::filesystem::exists(cvt, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                               NS_FuncNameV, "exists", err.value(), err.message(), cvt);
            throw xcb_error(NS_FuncNameS);
        }

        std::string cmd = Tools::runcmd(fmt::format("{} {} {} {}", cvt, sz.width, sz.height, vertRef));
        auto params = Tools::split(cmd.substr(cmd.find('\n', 0) + 1), 0x20);
        std::erase_if(params, [](auto & val) {
            return val.empty();
        });

        // params: Modeline "1024x600_60.00"   49.00  1024 1072 1168 1312  600 603 613 624 -hsync +vsync
        if(params.size() != 13) {
            Application::error("{}: incorrect cvt format, params: {}", NS_FuncNameV, params.size());
            throw xcb_error(NS_FuncNameS);
        }

        xcb_randr_mode_info_t mode_info;
        mode_info.id = 0;
        mode_info.hskew = 0;
        mode_info.mode_flags = 0;

        auto it = std::next(params.begin(), 2);

        try {
            float clock = std::stof(*it);

            // dummy drv limited 300
            if(clock > 300) {
                return cvtCreateMode(sz, vertRef < 10 ? vertRef - 1 : vertRef - 5);
            }

            mode_info.dot_clock = clock * 1000000;
            it = std::next(it);
            mode_info.width = std::stoi(*it);
            it = std::next(it);
            mode_info.hsync_start = std::stoi(*it);
            it = std::next(it);
            mode_info.hsync_end = std::stoi(*it);
            it = std::next(it);
            mode_info.htotal = std::stoi(*it);
            it = std::next(it);
            mode_info.height = std::stoi(*it);
            it = std::next(it);
            mode_info.vsync_start = std::stoi(*it);
            it = std::next(it);
            mode_info.vsync_end = std::stoi(*it);
            it = std::next(it);
            mode_info.vtotal = std::stoi(*it);
            it = std::next(it);
        } catch(const std::exception &) {
            Application::error("{}: unknown format outputs from cvt", NS_FuncNameV);
            throw xcb_error(NS_FuncNameS);
        }

        if(*it == "-hsync") {
            mode_info.mode_flags |= XCB_RANDR_MODE_FLAG_HSYNC_NEGATIVE;
        } else if(*it == "+hsync") {
            mode_info.mode_flags |= XCB_RANDR_MODE_FLAG_HSYNC_POSITIVE;
        }

        it = std::next(it);

        if(*it == "-vsync") {
            mode_info.mode_flags |= XCB_RANDR_MODE_FLAG_VSYNC_NEGATIVE;
        } else if(*it == "+vsync") {
            mode_info.mode_flags |= XCB_RANDR_MODE_FLAG_VSYNC_POSITIVE;
        }

        auto name = fmt::format("{}x{}_{}", mode_info.width, mode_info.height, vertRef);
        mode_info.name_len = name.size();
        auto xcbReply = getReplyFunc2(xcb_randr_create_mode, ptr.get(), screen, mode_info, name.size(), name.data());

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_create_mode");
            throw xcb_error(NS_FuncNameS);
        }

        if(const auto & reply = xcbReply.reply()) {
            Application::debug(DebugType::Xcb, "{}: id: {:#010x}, mode: {}", NS_FuncNameV, reply->mode, Size(mode_info.width, mode_info.height));
            return reply->mode;
        }

        throw xcb_error(NS_FuncNameS);
    }

    bool XCB::ModuleRandr::destroyMode(const xcb_randr_mode_t & mode) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        auto cookie = xcb_randr_destroy_mode_checked(ptr.get(), mode);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_destroy_mode");
            return false;
        }

        Application::debug(DebugType::Xcb, "{}: id: {:#010x}", NS_FuncNameV, mode);
        return true;
    }

    bool XCB::ModuleRandr::addOutputMode(const xcb_randr_output_t & output, const xcb_randr_mode_t & mode) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        auto cookie = xcb_randr_add_output_mode_checked(ptr.get(), output, mode);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_add_output_mode");
            return false;
        }

        Application::debug(DebugType::Xcb, "{}: output: {:#010x}, mode: {:#010x}", NS_FuncNameV, output, mode);
        return true;
    }

    bool XCB::ModuleRandr::deleteOutputMode(const xcb_randr_output_t & output, const xcb_randr_mode_t & mode) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        auto cookie = xcb_randr_delete_output_mode_checked(ptr.get(), output, mode);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_delete_output_mode");
            return false;
        }

        Application::debug(DebugType::Xcb, "{}: output: {:#010x}, mode: {:#010x}", NS_FuncNameV, output, mode);
        return true;
    }

    bool XCB::ModuleRandr::crtcConnectOutputsMode(const xcb_randr_crtc_t & crtc, const xcb_randr_mode_t & mode,
        const Point & pos, std::span<const xcb_randr_output_t> outputs, const xcb_timestamp_t & configTime, uint16_t* sequence) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        auto xcbReply = getReplyFunc2(xcb_randr_set_crtc_config, ptr.get(), crtc, XCB_CURRENT_TIME, configTime,
                                          pos.x, pos.y, mode, XCB_RANDR_ROTATION_ROTATE_0, outputs.size(), outputs.data());

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_set_crtc_config");
            return false;
        }

        if(const auto & reply = xcbReply.reply()) {
            if(reply->status == XCB_RANDR_SET_CONFIG_SUCCESS) {
                if(sequence) {
                    *sequence = reply->sequence;
                }

                return true;
            }
            
            Application::warning("{}: {} failed, status: {}",
                                 NS_FuncNameV, "xcb_randr_set_crtc_config", reply->status);
        }

        return false;
    }

    bool XCB::ModuleRandr::setDisplaySize(const XCB::Region & monitor, const xcb_randr_crtc_t & crtc,
            const xcb_randr_output_t & output, const RandrScreenResources & res, uint16_t* sequence) const {

        xcb_randr_mode_t mode;

        if(! res.findMode(monitor.toSize(), &mode)) {
            try {
                mode = cvtCreateMode(monitor.toSize(), 20);
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                return false;
            }
        }

        Application::debug(DebugType::Xcb, "{}: mode found: {}, size: {}", NS_FuncNameV, mode, monitor.toSize());

        crtcDisconnect(crtc, res->config_timestamp);

        if(! setScreenSize(monitor.toSize())) {
            return false;
        }

        if(! addOutputMode(output, mode)) {
            return false;
        }

        return crtcConnectOutputsMode(crtc, mode, monitor.topLeft(), {&output, 1}, res->config_timestamp, sequence);
    }

    bool XCB::ModuleRandr::crtcDisconnect(const xcb_randr_crtc_t & crtc, const xcb_timestamp_t & configTime) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        auto xcbReply = getReplyFunc2(xcb_randr_set_crtc_config, ptr.get(), crtc, XCB_CURRENT_TIME, configTime,
                                      0, 0, XCB_NONE, XCB_RANDR_ROTATION_ROTATE_0, 0, nullptr);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_randr_set_crtc_config");
            return false;
        }

        return true;
    }

    bool XCB::ModuleRandr::setScreenSize(const Size & sz, uint16_t dpi) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        // align size
        Application::debug(DebugType::Xcb,  "{}: size: {}, dpi: {}", NS_FuncNameV, sz, dpi);

        uint32_t mm_width = sz.width * 25.4 / dpi;
        uint32_t mm_height = sz.height * 25.4 / dpi;

        auto cookie = xcb_randr_set_screen_size_checked(ptr.get(), screen, sz.width, sz.height, mm_width, mm_height);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, " xcb_randr_set_screen_size");
            return false;
        }

        return true;
    }

    // XCB::ModuleShm
    XCB::ModuleShm::ModuleShm(const ConnectionShared & ptr) : ModuleExtension(ptr, Module::SHM) {
        ext = xcb_get_extension_data(ptr.get(), & xcb_shm_id);

        if(! ext || ! ext->present) {
            Application::error("{}: extension not found: {}", NS_FuncNameV, "SHM");
            throw xcb_error(NS_FuncNameS);
        }

        auto xcbReply = getReplyFunc2(xcb_shm_query_version, ptr.get());

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_shm_query_version");
            throw xcb_error(NS_FuncNameS);
        }

        if(const auto & reply = xcbReply.reply()) {
            Application::debug(DebugType::Xcb, "{}: extension version: {}.{}",
                               NS_FuncNameV, reply->major_version, reply->minor_version);
        }
    }

    XCB::ShmId::~ShmId() {
        reset();
    }

    void XCB::ShmId::reset(void) {
        if(0 < id) {
            if(auto ptr = conn.lock()) {
                xcb_shm_detach(ptr.get(), id);
            }
        }

        if(addr) {
            shmdt(addr);
        }

        if(0 <= shm) {
            shmctl(shm, IPC_RMID, nullptr);
        }

        id = 0;
        addr = nullptr;
        shm = -1;
    }

    XCB::ShmIdShared XCB::ModuleShm::createShm(size_t shmsz, int mode, bool readOnly, uid_t owner) const {
        Application::debug(DebugType::Xcb, "{}: size: {}, mode: {:#010x}, read only: {}, owner: {}", NS_FuncNameV, shmsz, mode, (int) readOnly, owner);

        const size_t pagesz = 4096;

        // shmsz: align page 4096
        shmsz = Tools::alignUp(shmsz, 4096);
        int shmId = shmget(IPC_PRIVATE, shmsz, IPC_CREAT | mode);

        if(shmId == -1) {
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "shmget", strerror(errno), errno);
            return nullptr;
        }

        auto shmAddr = reinterpret_cast<uint8_t*>(shmat(shmId, nullptr, (readOnly ? SHM_RDONLY : 0)));

        // man shmat: check result
        if(shmAddr == reinterpret_cast<uint8_t*>(-1) && 0 != errno) {
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "shmaddr", strerror(errno), errno);
            return nullptr;
        }

        if(0 < owner) {
            shmid_ds info;

            if(-1 == shmctl(shmId, IPC_STAT, & info)) {
                Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "shmctl", strerror(errno), errno);
            } else {
                info.shm_perm.uid = owner;

                if(-1 == shmctl(shmId, IPC_SET, & info)) {
                    Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "shmctl", strerror(errno), errno);
                }
            }
        }

        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return nullptr;
        }

        xcb_shm_seg_t res = xcb_generate_id(ptr.get());

        auto cookie = xcb_shm_attach_checked(ptr.get(), res, shmId, 0);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_shm_attach");
            return nullptr;
        }

        Application::debug(DebugType::Xcb, "{}: resource id: {:#010x}", NS_FuncNameV, res);
        return std::make_shared<ShmId>(conn, shmId, shmAddr, shmsz, owner, res);
    }

    // XCB::ModuleXkb
    XCB::ModuleXkb::ModuleXkb(const ConnectionShared & ptr) : ModuleExtension(ptr, Module::XKB),
        ctx { nullptr, xkb_context_unref }, map { nullptr, xkb_keymap_unref }, state { nullptr, xkb_state_unref } {
        ext = xcb_get_extension_data(ptr.get(), & xcb_xkb_id);

        if(! ext || ! ext->present) {
            Application::error("{}: extension not found: {}", NS_FuncNameV, "XKB");
            throw xcb_error(NS_FuncNameS);
        }

        auto xcbReply = getReplyFunc2(xcb_xkb_use_extension, ptr.get(), XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xkb_use_extension");
            throw xcb_error(NS_FuncNameS);
        }

        if(const auto & reply = xcbReply.reply()) {
            Application::debug(DebugType::Xcb, "{}: extension version: {}.{}",
                               NS_FuncNameV, reply->serverMajor, reply->serverMinor);
        }

        devid = xkb_x11_get_core_keyboard_device_id(ptr.get());

        if(0 > devid) {
            Application::error("{}: {} failed", NS_FuncNameV, "xkb_x11_get_core_keyboard_device_id");
            throw xcb_error(NS_FuncNameS);
        }

        ctx.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));

        if(! ctx) {
            Application::error("{}: {} failed", NS_FuncNameV, "xkb_context_new");
            throw xcb_error(NS_FuncNameS);
        }

        if(! resetMapState()) {
            throw xcb_error(NS_FuncNameS);
        }

        // notify xkb filter
        const uint16_t required_map_parts = (XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS | XCB_XKB_MAP_PART_MODIFIER_MAP |
                                             XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS | XCB_XKB_MAP_PART_KEY_ACTIONS | XCB_XKB_MAP_PART_VIRTUAL_MODS | XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP);
        const uint16_t required_events = (XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_STATE_NOTIFY);

        xcb_xkb_select_events(ptr.get(), devid, required_events, 0, required_events, required_map_parts, required_map_parts, nullptr);
    }

    XCB::ModuleXkb::~ModuleXkb() {
        if(auto ptr = conn.lock()) {
            const uint16_t clear = (XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_STATE_NOTIFY);
            xcb_xkb_select_events(ptr.get(), devid, 0, clear, 0, 0, 0, nullptr);
        }
    }

    bool XCB::ModuleXkb::resetMapState(void) {
        // free state first
        state.reset();
        map.reset();

        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        // set new
        map.reset(xkb_x11_keymap_new_from_device(ctx.get(), ptr.get(), devid, XKB_KEYMAP_COMPILE_NO_FLAGS));

        if(map) {
            state.reset(xkb_x11_state_new_from_device(map.get(), ptr.get(), devid));

            if(! state) {
                Application::error("{}: {} failed", NS_FuncNameV, "xkb_x11_state_new_from_device");
                return false;
            }

            Application::debug(DebugType::Xcb, "{}: keyboard updated, device id: {}", NS_FuncNameV, devid);
            return true;
        }

        Application::error("{}: {} failed", NS_FuncNameV, "xkb_x11_keymap_new_from_device");
        return false;
    }

    std::vector<std::string> XCB::ModuleXkb::getNames(void) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return {};
        }

        std::vector<std::string> res;
        res.reserve(4);

        auto xcbReply = getReplyFunc2(xcb_xkb_get_names, ptr.get(), XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_NAME_DETAIL_GROUP_NAMES | XCB_XKB_NAME_DETAIL_SYMBOLS);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xkb_get_names");
            return res;
        }

        if(const auto & reply = xcbReply.reply()) {
            const void* buffer = xcb_xkb_get_names_value_list(reply.get());
            xcb_xkb_get_names_value_list_t list;
            xcb_xkb_get_names_value_list_unpack(buffer, reply->nTypes, reply->indicators, reply->virtualMods,
                                                reply->groupNames, reply->nKeys, reply->nKeyAliases, reply->nRadioGroups, reply->which, & list);
            int groups = xcb_xkb_get_names_value_list_groups_length(reply.get(), & list);

            // current symbol atom: list.symbolsName;

            for(int ii = 0; ii < groups; ++ii) {
                res.emplace_back(Atom::getName(ptr.get(), list.groups[ii]));
            }
        }

        return res;
    }

    int XCB::ModuleXkb::getLayoutGroup(void) const {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return -1;
        }

        auto xcbReply = getReplyFunc2(xcb_xkb_get_state, ptr.get(), XCB_XKB_ID_USE_CORE_KBD);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xkb_get_names");
            return -1;
        }

        if(const auto & reply = xcbReply.reply()) {
            return reply->group;
        }

        return -1;
    }

    bool XCB::ModuleXkb::switchLayoutGroup(int group) const {
        // next
        if(group < 0) {
            auto names = getNames();

            if(2 > names.size()) {
                return false;
            }

            group = (getLayoutGroup() + 1) % names.size();
        }

        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        auto cookie = xcb_xkb_latch_lock_state_checked(ptr.get(), XCB_XKB_ID_USE_CORE_KBD, 0, 0, 1, group, 0, 0, 0);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_xkb_latch_lock_state");
            return false;
        }

        return true;
    }

    // ModulePasteSelection
    XCB::ModulePasteSelection::ModulePasteSelection(const ConnectionShared & ptr, const xcb_screen_t & screen, xcb_atom_t atom)
        : ModuleExtension(ptr, Module::SELECTION_PASTE) {
        const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const uint32_t values[] = { screen.white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE };

        selectionWin = xcb_generate_id(ptr.get());
        auto cookie = xcb_create_window_checked(ptr.get(), 0, selectionWin, screen.root, -1, -1, 1, 1, 0,
                                                XCB_WINDOW_CLASS_INPUT_OUTPUT, screen.root_visual, mask, values);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_create_window");
            throw xcb_error(NS_FuncNameS);
        }

        // set _wm_name
        std::string_view name{"LTSM_SELECTION_PASTE"};

        xcb_change_property(ptr.get(), XCB_PROP_MODE_REPLACE, selectionWin,
                            Atom::wmName, Atom::utf8String, 8, name.size(), name.data());

        selectionAtom = AtomName(ptr.get(), atom != XCB_ATOM_NONE ? atom : Atom::clipboard);
        Application::debug(DebugType::Xcb, "{}: window id: {:#010x}, selection: {}", NS_FuncNameV, selectionWin, selectionAtom);
    }

    XCB::ModulePasteSelection::~ModulePasteSelection() {
        removeRequestors(XCB_WINDOW_NONE);

        if(auto ptr = conn.lock(); selectionWin != XCB_WINDOW_NONE && ptr) {
            xcb_destroy_window(ptr.get(), selectionWin);
        }
    }

    void XCB::ModulePasteSelection::sendNotifyEvent(const ConnectionShared & ptr, const xcb_selection_request_event_t* ev, xcb_atom_t atom) const {
        const xcb_selection_notify_event_t notify = {
            .response_type = XCB_SELECTION_NOTIFY,
            .pad0 = 0,
            .sequence = 0,
            .time = ev->time,
            .requestor = ev->requestor,
            .selection = ev->selection,
            .target = ev->target,
            .property = atom
        };

        xcb_send_event(ptr.get(), 0, ev->requestor, XCB_EVENT_MASK_NO_EVENT, (const char*) & notify);
        xcb_flush(ptr.get());
    }

    void XCB::ModulePasteSelection::eventRequestDebug(const ConnectionShared & ptr, const xcb_selection_request_event_t* ev, bool warn) const {
        auto sel = AtomName(ptr.get(), ev->selection);
        auto tgt = AtomName(ptr.get(), ev->target);
        auto prop = AtomName(ptr.get(), ev->property);

        if(warn) {
            Application::warning("{}: EVENT[ sequence: {}, time: {}, owner: {:#010x}, requestor: {:#010x}, selection {}, target {}, property {} ]",
                                 NS_FuncNameV, ev->sequence, ev->time, ev->owner, ev->requestor, sel, tgt, prop);
        } else {
            Application::debug(DebugType::Xcb, "{}: EVENT[ sequence: {}, time: {}, owner: {:#010x}, requestor: {:#010x}, selection {}, target {}, property {} ]",
                               NS_FuncNameV, ev->sequence, ev->time, ev->owner, ev->requestor, ev->selection, sel, tgt, prop);
        }
    }

    void XCB::ModulePasteSelection::discardRequestor(const ConnectionShared & ptr, const xcb_selection_request_event_t & ev) {
        sendNotifyDiscard(ptr, & ev);
        eventRequestWarning(ptr, & ev);

        if(source) {
            source->selectionSourceUnlock(ev.target);
        }

        // reset events filter
        const uint32_t values[] = { XCB_EVENT_MASK_NO_EVENT };
        xcb_change_window_attributes(ptr.get(), ev.requestor, XCB_CW_EVENT_MASK, values);
    }

    bool XCB::ModulePasteSelection::removeRequestors(xcb_window_t win) {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return false;
        }

        bool removed = false;

        for(const auto & req : requestsIncr) {
            if(0 == win || req.ev.requestor == win) {
                discardRequestor(ptr, req.ev);
                removed = true;
            }
        }

        if(XCB_WINDOW_NONE == win) {
            requestsIncr.clear();
        } else {
            std::erase_if(requestsIncr, [ = ](auto & req) {
                return req.ev.requestor == win;
            });
        }

        xcb_flush(ptr.get());
        return removed;
    }

    void XCB::ModulePasteSelection::destroyNotifyEvent(const xcb_destroy_notify_event_t* ev) {
        Application::debug(DebugType::Xcb, "{}: owner: {:#010x}, selection {}, EVENT[ sequence: {}, event: {:#010x}, window: {:#010x}]",
                           NS_FuncNameV, selectionWin, selectionAtom, ev->sequence, ev->event, ev->window);

        if(ev->window) {
            if(removeRequestors(ev->window)) {
                Application::warning("{}: destroy requestor: {:#010x}", NS_FuncNameV, ev->window);
            }
        }
    }

    void XCB::ModulePasteSelection::selectionClearEvent(const xcb_selection_clear_event_t* ev) {
        Application::debug(DebugType::Xcb, "{}: owner: {:#010x}, selection {}, EVENT[ sequence: {}, time: {}, owner: {:#010x}, selection: {:#010x} ]",
                           NS_FuncNameV, selectionWin, selectionAtom, ev->sequence, ev->time, ev->owner, ev->selection);

        if(ev->owner == selectionWin && ev->selection == selectionAtom.atom && requestsIncr.size()) {
            if(removeRequestors(XCB_WINDOW_NONE)) {
                Application::warning("{}: clear all requestsIncr", NS_FuncNameV);
            }
        }
    }

    void XCB::ModulePasteSelection::propertyNotifyEvent(const xcb_property_notify_event_t* ev) {
        Application::debug(DebugType::Xcb, "{}: owner: {:#010x}, selection {}, EVENT[ sequence: {}, window: {:#010x}, atom: {:#010x}, time: {}, state: {:#04x} ]",
                           NS_FuncNameV, selectionWin, selectionAtom, ev->sequence, ev->window, ev->atom, ev->time, ev->state);

        if(ev->state != XCB_PROPERTY_DELETE) {
            return;
        }

        if(! source) {
            return;
        }

        auto it = std::ranges::find_if(requestsIncr, [ = ](auto & req) {
            return req.ev.requestor == ev->window && req.ev.property == ev->atom && req.ev.time < ev->time;
        });

        if(it == requestsIncr.end()) {
            return;
        }

        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        // calculate incremental blocksz
        const size_t datasz = source->selectionSourceSize(it->ev.target);
        const size_t maxreq = xcb_get_maximum_request_length(ptr.get());

        auto blocksz = std::min(maxreq, datasz);
        blocksz = std::min(datasz - it->offset, blocksz);
        bool remove = false;

        if(blocksz) {
            // incremental put data
            if(auto buf = source->selectionSourceData(it->ev.target, it->offset, blocksz); buf.size()) {
                auto cookie = xcb_change_property_checked(ptr.get(), XCB_PROP_MODE_REPLACE, it->ev.requestor,
                              it->ev.property, it->ev.target, 8, buf.size(), buf.data());

                if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
                    discardRequestor(ptr, it->ev);
                    Application::error("{}: invalid request, failed {}", NS_FuncNameV, "xcb_change_property");
                    remove = true;
                } else {
                    // incremental continue
                    it->offset += blocksz;
                }
            } else {
                discardRequestor(ptr, it->ev);
                Application::error("{}: invalid buffer, failed {}", NS_FuncNameV, "selectionSourceData");
                remove = true;
            }
        } else {
            // end data marker: length is zero
            xcb_change_property(ptr.get(), XCB_PROP_MODE_REPLACE, it->ev.requestor, it->ev.property,
                                it->ev.target, 8, 0, nullptr);

            // reset events filter
            const uint32_t values[] = { XCB_EVENT_MASK_NO_EVENT };
            xcb_change_window_attributes(ptr.get(), it->ev.requestor, XCB_CW_EVENT_MASK, values);

            remove = true;
        }

        if(remove) {
            source->selectionSourceUnlock(it->ev.target);
            requestsIncr.erase(it);
        }

        xcb_flush(ptr.get());
    }

    void XCB::ModulePasteSelection::selectionRequestEvent(const xcb_selection_request_event_t* ev) {
        Application::debug(DebugType::Xcb, "{}: owner: {:#010x}, selection {}", NS_FuncNameV, selectionWin, selectionAtom);

        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        if(! source) {
            sendNotifyDiscard(ptr, ev);
            Application::error("{}: source empty", NS_FuncNameV);
            return;
        }

        if(ev->selection != selectionAtom.atom) {
            sendNotifyDiscard(ptr, ev);
            eventRequestWarning(ptr, ev);
            Application::warning("{}: invalid request, unknown {}", NS_FuncNameV, "selection");
            return;
        }

        if(ev->owner != selectionWin) {
            sendNotifyDiscard(ptr, ev);
            eventRequestWarning(ptr, ev);
            Application::warning("{}: invalid request, unknown {}", NS_FuncNameV, "owner");
            return;
        }

        if(ev->requestor == XCB_WINDOW_NONE) {
            sendNotifyDiscard(ptr, ev);
            eventRequestWarning(ptr, ev);
            Application::warning("{}: invalid request, unknown {}", NS_FuncNameV, "requestor");
            return;
        }

        if(ev->requestor == skipRequestorWin) {
            Application::debug(DebugType::Xcb, "{}: skip requestor: {:#010x}", NS_FuncNameV, skipRequestorWin);
            sendNotifyDiscard(ptr, ev);
            return;
        }

        if(ev->property == XCB_ATOM_NONE) {
            sendNotifyDiscard(ptr, ev);
            eventRequestWarning(ptr, ev);
            Application::warning("{}: invalid request, unknown {}", NS_FuncNameV, "property");
            return;
        }

        if(! source) {
            sendNotifyDiscard(ptr, ev);
            Application::error("{}: invalid request, unknown {}", NS_FuncNameV, "source");
            return;
        }

        if(0 == selectionTime) {
            selectionTime = ev->time;
        }

        auto targets = source->selectionSourceTargets();
        targets.push_back(Atom::timestamp);

        // send targets
        if(ev->target == Atom::targets) {
            xcb_change_property(ptr.get(), XCB_PROP_MODE_REPLACE, ev->requestor, ev->property,
                                XCB_ATOM_ATOM, 32, targets.size(), targets.data());

            sendNotifyEvent(ptr, ev, ev->property);
            return;
        }

        if(ev->target == Atom::timestamp) {
            eventRequestDebug(ptr, ev);

            auto tmp = std::to_string(selectionTime);
            tmp.append(1, 0x0a);

            xcb_change_property(ptr.get(), XCB_PROP_MODE_REPLACE, ev->requestor, ev->property,
                                Atom::timestamp, 8, tmp.size(), tmp.data());

            sendNotifyEvent(ptr, ev, ev->property);
            return;
        }

        // target not found
        if(std::ranges::none_of(targets, [&](auto & atom) { return atom == ev->target; })) {
            sendNotifyDiscard(ptr, ev);
            eventRequestWarning(ptr, ev);
            Application::warning("{}: invalid request, unknown {} atom", NS_FuncNameV, "target");
            return;
        }

        if(! source->selectionSourceReady(ev->target)) {
            sendNotifyDiscard(ptr, ev);
            eventRequestWarning(ptr, ev);
            Application::error("{}: target not ready", NS_FuncNameV);
            return;
        }

        eventRequestDebug(ptr, ev);

        auto it = std::ranges::find_if(requestsIncr, [ = ](auto & req) {
            return req.ev.requestor == ev->requestor && req.ev.selection == ev->selection &&
                   req.ev.target == ev->target && req.ev.property == ev->property;
        });

        // found requestor
        if(it != requestsIncr.end()) {
            source->selectionSourceUnlock(it->ev.target);
            // remove old req and continue
            requestsIncr.erase(it);
        }

        const size_t datasz = source->selectionSourceSize(ev->target);
        const size_t maxreq = xcb_get_maximum_request_length(ptr.get());

        if(datasz > maxreq) {
            eventRequestWarning(ptr, ev);

            // incremental mode: add job
            source->selectionSourceLock(ev->target);

            // add event filter: or XCB_EVENT_MASK_NO_EVENT for disable
            const uint32_t values[] = { XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE };
            xcb_change_window_attributes(ptr.get(), ev->requestor, XCB_CW_EVENT_MASK, values);

            xcb_change_property(ptr.get(), XCB_PROP_MODE_REPLACE, ev->requestor, ev->property,
                                Atom::incr, 32, 1, & datasz);

            requestsIncr.push_back(*ev);
            sendNotifyEvent(ptr, ev, ev->property);
        } else {
            source->selectionSourceLock(ev->target);

            // normal mode: put data
            auto buf = source->selectionSourceData(ev->target, 0, datasz);
            xcb_change_property(ptr.get(), XCB_PROP_MODE_REPLACE, ev->requestor, ev->property,
                                ev->target, 8, buf.size(), buf.data());
            sendNotifyEvent(ptr, ev, ev->property);

            source->selectionSourceUnlock(ev->target);
        }
    }

    void XCB::ModulePasteSelection::setSelectionOwner(const SelectionSource & src) {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        Application::debug(DebugType::Xcb, "{}: window id: {:#010x}, selection {}",
                           NS_FuncNameV, selectionWin, selectionAtom);

        source = std::addressof(src);
        selectionTime = 0;

        xcb_set_selection_owner(ptr.get(), selectionWin, selectionAtom.atom, XCB_CURRENT_TIME);
        xcb_flush(ptr.get());
    }

    // ModuleCopySelection
    XCB::ModuleCopySelection::ModuleCopySelection(const ConnectionShared & ptr, const xcb_screen_t & screen, xcb_atom_t atom)
        : ModuleExtension(ptr, Module::SELECTION_COPY) {
        const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const uint32_t values[] = { screen.white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE };

        selectionWin = xcb_generate_id(ptr.get());
        auto cookie = xcb_create_window_checked(ptr.get(), 0, selectionWin, screen.root, -1, -1, 1, 1, 0,
                                                XCB_WINDOW_CLASS_INPUT_OUTPUT, screen.root_visual, mask, values);

        if(auto err = GenericError(xcb_request_check(ptr.get(), cookie))) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_create_window");
            throw xcb_error(NS_FuncNameS);
        }

        // set _wm_name
        std::string_view name{"LTSM_SELECTION_COPY"};

        xcb_change_property(ptr.get(), XCB_PROP_MODE_REPLACE, selectionWin,
                            Atom::wmName, Atom::utf8String, 8, name.size(), name.data());

        selectionProp = Atom::getAtom(ptr.get(), "XSEL_DATA", true);
        selectionAtom = AtomName(ptr.get(), atom != XCB_ATOM_NONE ? atom : Atom::clipboard);

        xcb_xfixes_select_selection_input(ptr.get(), screen.root, selectionAtom.atom,
                                          XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                                          XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                                          XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);

        xfixesWin = screen.root;

        Application::debug(DebugType::Xcb, "{}: window id: {:#010x}, selection {}", NS_FuncNameV, selectionWin, selectionAtom);
    }

    XCB::ModuleCopySelection::~ModuleCopySelection() {
        if(auto ptr = conn.lock(); selectionWin != XCB_WINDOW_NONE && ptr) {
            xcb_xfixes_select_selection_input(ptr.get(), xfixesWin, selectionAtom.atom, 0);
            xcb_destroy_window(ptr.get(), selectionWin);
        }
    }

    void XCB::ModuleCopySelection::eventNotifyDebug(const ConnectionShared & ptr, const xcb_selection_notify_event_t* ev, bool warn) const {
        auto sel = AtomName(ptr.get(), ev->selection);
        auto tgt = AtomName(ptr.get(), ev->target);
        auto prop = AtomName(ptr.get(), ev->property);

        if(warn) {
            Application::warning("{}: EVENT[ sequence: {}, time: {}, requestor: {:#010x}, selection {}, target {}, property {} ]",
                                 NS_FuncNameV, ev->sequence, ev->time, ev->requestor, sel, tgt, prop);
        } else {
            Application::debug(DebugType::Xcb, "{}: EVENT[ sequence: {}, time: {}, requestor: {:#010x}, selection {}, target {}, property {} ]",
                               NS_FuncNameV, ev->sequence, ev->time, ev->requestor, sel, tgt, prop);
        }
    }

    void XCB::ModuleCopySelection::propertyNotifyEvent(const xcb_property_notify_event_t* ev) {
        Application::debug(DebugType::Xcb, "{}: owner: {:#010x}, selection: {}, EVENT[ sequence: {}, window: {:#010x}, atom: {:#010x}, time: {}, state: {:#04x} ]",
                           NS_FuncNameV, selectionWin, selectionAtom, ev->sequence, ev->window, ev->atom, ev->time, ev->state);

        if(ev->state != XCB_PROPERTY_NEW_VALUE) {
            return;
        }

        if(ev->window != selectionWin || ev->atom != selectionProp) {
            return;
        }

        if(! sourceIncr) {
            return;
        }

        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        auto xcbReply = getReplyFunc2(xcb_get_property, ptr.get(), false, selectionWin, selectionProp, XCB_GET_PROPERTY_TYPE_ANY, 0, ~0);

        if(const auto & err = xcbReply.error()) {
            error(ptr.get(), err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            auto ptr = static_cast<const uint8_t*>(xcb_get_property_value(reply.get()));
            auto len = xcb_get_property_value_length(reply.get());

            if(ptr && len) {
                sourceIncr->buf.insert(sourceIncr->buf.end(), ptr, ptr + len);
            } else {
                recipient->selectionReceiveData(reply->type, sourceIncr->buf);
            }
        }

        xcb_delete_property(ptr.get(), selectionWin, selectionProp);
        xcb_flush(ptr.get());
    }

    void XCB::ModuleCopySelection::selectionNotifyEvent(const xcb_selection_notify_event_t* ev) {
        Application::debug(DebugType::Xcb, "{}: owner: {:#010x}, selection: {}", NS_FuncNameV, selectionWin, selectionAtom);

        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        if(ev->selection != selectionAtom.atom) {
            eventNotifyWarning(ptr, ev);
            Application::warning("{}: invalid notify, unknown {}", NS_FuncNameV, "selection");
            return;
        }

        if(ev->property != selectionProp) {
            if(ev->property != XCB_ATOM_NONE) {
                eventNotifyWarning(ptr, ev);
                Application::warning("{}: invalid notify, unknown {}", NS_FuncNameV, "property");
            }

            return;
        }

        if(ev->requestor != selectionWin) {
            eventNotifyWarning(ptr, ev);
            Application::warning("{}: invalid notify, unknown {}", NS_FuncNameV, "requestor");
            return;
        }

        if(! recipient) {
            Application::error("{}: invalid notify, unknown {}", NS_FuncNameV, "recipient");
            return;
        }

        if(Atom::targets == ev->target) {
            eventNotifyDebug(ptr, ev);

            auto xcbReply = getReplyFunc2(xcb_get_property, ptr.get(), false, selectionWin, selectionProp, XCB_ATOM_ATOM, 0, ~0);

            if(const auto & err = xcbReply.error()) {
                error(ptr.get(), err.get(), NS_FuncNameV, "xcb_get_property");
            } else if(const auto & reply = xcbReply.reply()) {
                auto ptr = static_cast<xcb_atom_t*>(xcb_get_property_value(reply.get()));
                auto len = xcb_get_property_value_length(reply.get()) / sizeof(xcb_atom_t);

                if(ptr) {
                    recipient->selectionReceiveTargets(ptr, ptr + len);
                } else {
                    Application::warning("{}: property empty", NS_FuncNameV);
                }
            }
        } else if(selectionTrgt == ev->target) {
            eventNotifyDebug(ptr, ev);

            if(auto info = XCB::getPropertyInfo(ptr.get(), selectionWin, selectionProp)) {
                auto xcbReply = getReplyFunc2(xcb_get_property, ptr.get(), false, selectionWin, selectionProp, info->type, 0, ~0);

                if(const auto & err = xcbReply.error()) {
                    error(ptr.get(), err.get(), NS_FuncNameV, "xcb_get_property");
                } else if(const auto & reply = xcbReply.reply()) {
                    auto buf = xcb_get_property_value(reply.get());
                    auto len = xcb_get_property_value_length(reply.get());
                    auto typeAtom = AtomName(ptr.get(), reply->type);

                    if(buf && len) {
                        if(Atom::incr == typeAtom.atom) {
                            auto psize = static_cast<const uint32_t*>(buf);
                            Application::debug(DebugType::Xcb, "{}: incr size: {}", NS_FuncNameV, *psize);
                            sourceIncr = std::make_unique<WindowSource>(*ev, *psize);
                        } else {
                            if(selectionTrgt != typeAtom.atom) {
                                eventNotifyWarning(ptr, ev);
                                Application::warning("{}: reply not correct, type {}, format: {}", NS_FuncNameV, typeAtom, reply->format);
                            }

                            recipient->selectionReceiveData(reply->type, {(const uint8_t*) buf, (size_t) len});
                        }
                    } else {
                        eventNotifyWarning(ptr, ev);

                        Application::warning("{}: reply empty, type {}, format: {}",
                                             NS_FuncNameV, typeAtom, reply->format);
                    }
                }
            } else {
                eventNotifyWarning(ptr, ev);
                Application::warning("{}: {} failed", NS_FuncNameV, "getPropertyInfo");
            }
        } else {
            eventNotifyWarning(ptr, ev);
            Application::warning("{}: invalid notify, unknown {}", NS_FuncNameV, "target");
        }

        xcb_delete_property(ptr.get(), selectionWin, selectionProp);
        xcb_flush(ptr.get());
    }

    void XCB::ModuleCopySelection::xfixesSelectionNotifyEvent(const xcb_xfixes_selection_notify_event_t* ev) {
        switch(ev->subtype) {
            case XCB_XFIXES_SELECTION_EVENT_SET_SELECTION_OWNER:
                xfixesSetSelectionOwnerEvent(ev);
                break;

            case XCB_XFIXES_SELECTION_EVENT_SELECTION_WINDOW_DESTROY:
                xfixesSelectionWindowDestroyEvent(ev);
                break;

            case XCB_XFIXES_SELECTION_EVENT_SELECTION_CLIENT_CLOSE:
                xfixesSelectionClientCloseEvent(ev);
                break;

            default:
                break;
        }
    }

    void XCB::ModuleCopySelection::xfixesSelectionClientCloseEvent(const xcb_xfixes_selection_notify_event_t* ev) {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        auto selAtom = AtomName(ptr.get(), ev->selection);
        Application::debug(DebugType::Xcb, "{}: EVENT[ sequence: {}, window: {:#010x}, owner: {:#010x}, selection {}, time: {}, selection time: {} ]",
                           NS_FuncNameV, ev->sequence, ev->window, ev->owner, selAtom, ev->timestamp, ev->selection_timestamp);
    }

    void XCB::ModuleCopySelection::xfixesSelectionWindowDestroyEvent(const xcb_xfixes_selection_notify_event_t* ev) {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        auto selAtom = AtomName(ptr.get(), ev->selection);
        Application::debug(DebugType::Xcb, "{}: EVENT[ sequence: {}, window: {:#010x}, owner: {:#010x}, selection {}, time: {}, selection time: {} ]",
                           NS_FuncNameV, ev->sequence, ev->window, ev->owner, selAtom, ev->timestamp, ev->selection_timestamp);
    }

    void XCB::ModuleCopySelection::xfixesSetSelectionOwnerEvent(const xcb_xfixes_selection_notify_event_t* ev) {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        if(ev->selection == selectionAtom.atom) {
            auto selAtom = AtomName(ptr.get(), ev->selection);
            Application::debug(DebugType::Xcb, "{}: EVENT[ sequence: {}, window: {:#010x}, owner: {:#010x}, selection {}, time: {}, selection time: {} ]",
                               NS_FuncNameV, ev->sequence, ev->window, ev->owner, selAtom, ev->timestamp, ev->selection_timestamp);

            if(sourceIncr) {
                sourceIncr.reset();
            }

            if(recipient) {
                recipient->selectionChangedEvent();
            }
        }
    }

    void XCB::ModuleCopySelection::convertSelection(xcb_atom_t target, const SelectionRecipient & rcpt) {
        auto ptr = conn.lock();

        if(! ptr) {
            Application::warning("{}: weak_ptr invalid", NS_FuncNameV);
            return;
        }

        Application::debug(DebugType::Xcb, "{}: window id: {:#010x}, selection: {}, target: {:#010x}",
                           NS_FuncNameV, selectionWin, selectionAtom, target);

        selectionTrgt = target;
        recipient = std::addressof(rcpt);

        if(sourceIncr) {
            // data cached
            if(target == sourceIncr->ev.target) {
                std::thread([this, target = sourceIncr->ev.target, buf = sourceIncr->buf]() {
                    std::this_thread::sleep_for(10ms);
                    recipient->selectionReceiveData(target, buf);
                }).detach();
                return;
            } else if(target != Atom::targets) {
                sourceIncr.reset();
            }
        }

        xcb_convert_selection(ptr.get(), selectionWin, selectionAtom.atom, selectionTrgt, selectionProp, XCB_CURRENT_TIME);
        xcb_flush(ptr.get());
    }

#ifdef LTSM_WITH_XCB_ERRORS
    XCB::ErrorContext::ErrorContext(xcb_connection_t* conn) {
        xcb_errors_context_new(conn, & ctx);
    }

    XCB::ErrorContext::~ErrorContext() {
        if(ctx) {
            xcb_errors_context_free(ctx);
        }
    }

    bool XCB::ErrorContext::error(const xcb_generic_error_t* err, std::string_view func, std::string_view xcbname) const {
        if(! ctx) {
            return false;
        }

        const char* extension = nullptr;
        const char* major = xcb_errors_get_name_for_major_code(ctx, err->major_code);
        const char* minor = xcb_errors_get_name_for_minor_code(ctx, err->major_code, err->minor_code);
        const char* error = xcb_errors_get_name_for_error(ctx, err->error_code, & extension);

        Application::error("{}: {} failed, error: {}, extension: {}, major: {}, minor: {}, resource: {:#010x}, sequence: {}",
                           func, xcbname, error, (extension ? extension : "none"), major, (minor ? minor : "none"),
                           err->resource_id, err->sequence);

        return true;
    }

#endif

    std::string getLocalAddr(int displayNum) {
        if(displayNum < 0) {
            auto env = getenv("DISPLAY");
            return std::string(env ? env : "");
        }

        return fmt::format(":{}", displayNum);
    }

    /* XCB::Connector */
    XCB::Connector::Connector(int displayNum, const AuthCookie* cookie) {
        if(! connectorDisplayConnect(displayNum, cookie)) {
            throw xcb_error(NS_FuncNameS);
        }
    }

    const char* XCB::Connector::errorString(int err) {
        switch(err) {
            case XCB_CONN_ERROR:
                return "XCB_CONN_ERROR";

            case XCB_CONN_CLOSED_EXT_NOTSUPPORTED:
                return "XCB_CONN_CLOSED_EXT_NOTSUPPORTED";

            case XCB_CONN_CLOSED_MEM_INSUFFICIENT:
                return "XCB_CONN_CLOSED_MEM_INSUFFICIENT";

            case XCB_CONN_CLOSED_REQ_LEN_EXCEED:
                return "XCB_CONN_CLOSED_REQ_LEN_EXCEED";

            case XCB_CONN_CLOSED_PARSE_ERR:
                return "XCB_CONN_CLOSED_PARSE_ERR";

            case XCB_CONN_CLOSED_INVALID_SCREEN:
                return "XCB_CONN_CLOSED_INVALID_SCREEN";

            default:
                break;
        }

        return "XCB_CONN_UNKNOWN";
    }

    bool XCB::Connector::connectorDisplayConnect(int displayNum, const AuthCookie* cookie) {
        auto displayAddr = getLocalAddr(displayNum);

        if(cookie) {
            std::string_view magic{"MIT-MAGIC-COOKIE-1"};
            xcb_auth_info_t auth;
            auth.name = (char*) magic.data();
            auth.namelen = magic.size();
            auth.data = (char*) cookie->data();
            auth.datalen = cookie->size();

            auto ptr = xcb_connect_to_display_with_auth_info(displayAddr.c_str(), & auth, nullptr);
            _conn.reset(ptr, xcb_disconnect);
        } else {
            auto ptr = xcb_connect(displayAddr.c_str(), nullptr);
            _conn.reset(ptr, xcb_disconnect);
        }

        if(int err = xcb_connection_has_error(_conn.get())) {
            Application::error("{}: {} failed, addr: `{}', error: `{}'",
                               NS_FuncNameV, "xcb_connect", displayAddr, Connector::errorString(err));
            return false;
        }

        _setup = xcb_get_setup(_conn.get());

        if(! _setup) {
            Application::error("{}: {} failed", NS_FuncNameV, "xcb_get_setup");
            return false;
        }

#ifdef LTSM_WITH_XCB_ERRORS
        _error = std::make_unique<ErrorContext>(_conn.get());
#endif

        Atom::timestamp = getAtom("TIMESTAMP");
        Atom::wmName = getAtom("_NET_WM_NAME");
        Atom::utf8String = getAtom("UTF8_STRING");

        Atom::primary = getAtom("PRIMARY");
        Atom::clipboard = getAtom("CLIPBOARD");
        Atom::targets = getAtom("TARGETS");

        return true;
    }

    xcb_connection_t* XCB::Connector::xcb_ptr(void) {
        return _conn.get();
    }

    const xcb_connection_t* XCB::Connector::xcb_ptr(void) const {
        return _conn.get();
    }

    size_t XCB::Connector::bppFromDepth(size_t depth) const {
        for(auto fIter = xcb_setup_pixmap_formats_iterator(_setup); fIter.rem; xcb_format_next(& fIter))
            if(fIter.data->depth == depth) {
                return fIter.data->bits_per_pixel;
            }

        return 0;
    }

    size_t XCB::Connector::depthFromBpp(size_t bitsPerPixel) const {
        for(auto fIter = xcb_setup_pixmap_formats_iterator(_setup); fIter.rem; xcb_format_next(& fIter))
            if(fIter.data->bits_per_pixel == bitsPerPixel) {
                return fIter.data->depth;
            }

        return 0;
    }

    const xcb_setup_t* XCB::Connector::setup(void) const {
        return _setup;
    }

    XCB::GenericError XCB::Connector::checkRequest(const xcb_void_cookie_t & cookie) const {
        return GenericError(xcb_request_check(_conn.get(), cookie));
    }

    size_t XCB::Connector::getMaxRequest(void) const {
        return xcb_get_maximum_request_length(_conn.get());
    }

    bool XCB::Connector::checkAtom(std::string_view name) const {
        return XCB_ATOM_NONE != getAtom(name, false);
    }

    xcb_atom_t XCB::Connector::getAtom(std::string_view name, bool create) const {
        return Atom::getAtom(_conn.get(), name, create);
    }

    std::string XCB::Connector::getAtomName(xcb_atom_t atom) const {
        return Atom::getName(_conn.get(), atom);
    }

    void XCB::Connector::extendedError(const xcb_generic_error_t* err, std::string_view func, std::string_view xcbname) const {
#ifdef LTSM_WITH_XCB_ERRORS

        if(! _error || ! _error->error(err, func, xcbname))
#endif
        {
            XCB::error(err, func, xcbname);
        }
    }

    int XCB::Connector::hasError(void) const {
        return xcb_connection_has_error(_conn.get());
    }

    bool XCB::Connector::setWindowGeometry(xcb_window_t win, const Region & geom) {
        uint16_t mask = 0;

        mask |= XCB_CONFIG_WINDOW_X;
        mask |= XCB_CONFIG_WINDOW_Y;
        mask |= XCB_CONFIG_WINDOW_WIDTH;
        mask |= XCB_CONFIG_WINDOW_HEIGHT;

        const uint32_t values[] = { (uint32_t) geom.x, (uint32_t) geom.y, geom.width, geom.height };
        auto cookie = xcb_configure_window_checked(_conn.get(), win, mask, values);

        if(auto err = checkRequest(cookie)) {
            extendedError(err.get(), NS_FuncNameV, "xcb_configure_window");
            return false;
        }

        return true;
    }

    std::list<xcb_window_t> XCB::Connector::getWindowChilds(xcb_window_t win) const {
        std::list<xcb_window_t> res;

        auto xcbReply = getReplyFunc2(xcb_query_tree, _conn.get(), win);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_query_tree");
        } else if(const auto & reply = xcbReply.reply()) {
            xcb_window_t* ptr = xcb_query_tree_children(reply.get());
            int len = xcb_query_tree_children_length(reply.get());

            res.assign(ptr, ptr + len);
        }

        return res;
    }

    bool XCB::Connector::deleteProperty(xcb_window_t win, xcb_atom_t prop) const {
        auto cookie = xcb_delete_property_checked(_conn.get(), win, prop);

        if(auto err = checkRequest(cookie)) {
            extendedError(err.get(), NS_FuncNameV, "xcb_delete_property");
            return false;
        }

        return true;
    }

    std::list<xcb_atom_t> XCB::Connector::getPropertiesList(xcb_window_t win) const {
        auto xcbReply = getReplyFunc2(xcb_list_properties, _conn.get(), win);
        std::list<xcb_atom_t> res;

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_list_properties");
        } else if(const auto & reply = xcbReply.reply()) {
            auto beg = xcb_list_properties_atoms(reply.get());
            auto end = beg + xcb_list_properties_atoms_length(reply.get());

            if(beg && beg < end) {
                res.assign(beg, end);
            }
        }

        return res;
    }

    XCB::PropertyReply XCB::Connector::getPropertyInfo(xcb_window_t win, xcb_atom_t prop) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 0);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        }

        return XCB::PropertyReply(std::move(xcbReply.first));
    }

    xcb_atom_t XCB::Connector::getPropertyType(xcb_window_t win, xcb_atom_t prop) const {
        auto reply = getPropertyInfo(win, prop);
        return reply ? reply->type : XCB_ATOM_NONE;
    }

    xcb_atom_t XCB::Connector::getPropertyAtom(xcb_window_t win, xcb_atom_t prop, uint32_t offset) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_ATOM, offset, 1);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            if(reply->type != XCB_ATOM_ATOM) {
                auto typeAtom = AtomName(_conn.get(), reply->type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "atom", typeAtom, propAtom);
            } else if(auto res = static_cast<xcb_atom_t*>(xcb_get_property_value(reply.get()))) {
                return *res;
            }
        }

        return XCB_ATOM_NONE;
    }

    std::list<xcb_atom_t> XCB::Connector::getPropertyAtomList(xcb_window_t win, xcb_atom_t prop) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_ATOM, 0, ~0);
        std::list<xcb_atom_t> res;

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            if(reply->type != XCB_ATOM_ATOM) {
                auto typeAtom = AtomName(_conn.get(), reply->type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "atom", typeAtom, propAtom);
            } else {
                auto beg = static_cast<xcb_atom_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(xcb_atom_t);

                if(beg && beg < end) {
                    res.assign(beg, end);
                }
            }
        }

        return res;
    }

    xcb_window_t XCB::Connector::getPropertyWindow(xcb_window_t win, xcb_atom_t prop, uint32_t offset) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_WINDOW, offset, 1);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            if(reply->type != XCB_ATOM_WINDOW) {
                auto typeAtom = AtomName(_conn.get(), reply->type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "window", typeAtom, propAtom);
            } else if(auto res = static_cast<xcb_window_t*>(xcb_get_property_value(reply.get()))) {
                return *res;
            }
        }

        return XCB_WINDOW_NONE;
    }

    std::list<xcb_window_t> XCB::Connector::getPropertyWindowList(xcb_window_t win, xcb_atom_t prop) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_WINDOW, 0, ~0);
        std::list<xcb_window_t> res;

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            if(reply->type != XCB_ATOM_WINDOW) {
                auto typeAtom = AtomName(_conn.get(), reply->type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "window", typeAtom, propAtom);
            } else {
                auto beg = static_cast<xcb_window_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(xcb_window_t);

                if(beg && beg < end) {
                    res.assign(beg, end);
                }
            }
        }

        return res;
    }

    uint32_t XCB::Connector::getPropertyCardinal(xcb_window_t win, xcb_atom_t prop, uint32_t offset) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_CARDINAL, offset, 1);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            if(reply->type != XCB_ATOM_CARDINAL) {
                auto typeAtom = AtomName(_conn.get(), reply->type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "cardinal", typeAtom, propAtom);
            } else if(reply->format == 32) {
                if(auto res = static_cast<uint32_t*>(xcb_get_property_value(reply.get()))) {
                    return *res;
                }
            } else {
                auto name2 = getAtomName(prop);
                Application::warning("{}: unknown format: {}, property: {}", NS_FuncNameV, reply->format, name2);
            }
        }

        return 0;
    }

    std::list<uint32_t> XCB::Connector::getPropertyCardinalList(xcb_window_t win, xcb_atom_t prop) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_CARDINAL, 0, ~0);
        std::list<uint32_t> res;

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            if(reply->type != XCB_ATOM_CARDINAL) {
                auto typeAtom = AtomName(_conn.get(), reply->type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "cardinal", typeAtom, propAtom);
            } else if(reply->format == 32) {
                auto beg = static_cast<uint32_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(uint32_t);

                if(beg && beg < end) {
                    res.assign(beg, end);
                }
            } else {
                auto name2 = getAtomName(prop);
                Application::warning("{}: unknown format: {}, property: {}", NS_FuncNameV, reply->format, name2);
            }
        }

        return res;
    }

    int XCB::Connector::getPropertyInteger(xcb_window_t win, xcb_atom_t prop, uint32_t offset) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_INTEGER, offset, 1);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            if(reply->type != XCB_ATOM_INTEGER) {
                auto typeAtom = AtomName(_conn.get(), reply->type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "integer", typeAtom, propAtom);
            } else if(reply->format == 8) {
                if(auto res = static_cast<int8_t*>(xcb_get_property_value(reply.get()))) {
                    return *res;
                }
            } else if(reply->format == 16) {
                if(auto res = static_cast<int16_t*>(xcb_get_property_value(reply.get()))) {
                    return *res;
                }
            } else if(reply->format == 32) {
                if(auto res = static_cast<int32_t*>(xcb_get_property_value(reply.get()))) {
                    return *res;
                }
            } else if(reply->format == 64) {
                if(auto res = static_cast<int64_t*>(xcb_get_property_value(reply.get()))) {
                    return *res;
                }
            } else {
                auto name2 = getAtomName(prop);
                Application::warning("{}: unknown format: {}, property: {}", NS_FuncNameV, reply->format, name2);
            }
        }

        return 0;
    }

    std::list<int> XCB::Connector::getPropertyIntegerList(xcb_window_t win, xcb_atom_t prop) const {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_INTEGER, 0, ~0);
        std::list<int> res;

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            if(reply->type != XCB_ATOM_INTEGER) {
                auto typeAtom = AtomName(_conn.get(), reply->type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "integer", typeAtom, propAtom);
            } else if(reply->format == 8) {
                auto beg = static_cast<int8_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(int8_t);

                if(beg && beg < end) {
                    res.assign(beg, end);
                }
            } else if(reply->format == 16) {
                auto beg = static_cast<int16_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(int16_t);

                if(beg && beg < end) {
                    res.assign(beg, end);
                }
            } else if(reply->format == 32) {
                auto beg = static_cast<int32_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(int32_t);

                if(beg && beg < end) {
                    res.assign(beg, end);
                }
            } else if(reply->format == 64) {
                auto beg = static_cast<int64_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(int64_t);

                if(beg && beg < end) {
                    res.assign(beg, end);
                }
            } else {
                auto name2 = getAtomName(prop);
                Application::warning("{}: unknown format: {}, property: {}", NS_FuncNameV, reply->format, name2);
            }
        }

        return res;
    }

    std::string XCB::Connector::getPropertyString(xcb_window_t win, xcb_atom_t prop) const {
        auto type = getPropertyType(win, prop);

        if(type != XCB_ATOM_STRING) {
            if(type != Atom::utf8String) {
                auto typeAtom = AtomName(_conn.get(), type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "string", typeAtom, propAtom);
                return "";
            }

            type = Atom::utf8String;
        }

        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, type, 0, ~0);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            auto ptr = static_cast<const char*>(xcb_get_property_value(reply.get()));
            auto len = xcb_get_property_value_length(reply.get());

            if(ptr && 0 < len) {
                return std::string(ptr, ptr + len);
            }
        }

        return "";
    }

    std::list<std::string> XCB::Connector::getPropertyStringList(xcb_window_t win, xcb_atom_t prop) const {
        std::list<std::string> res;
        auto type = getPropertyType(win, prop);

        if(type != XCB_ATOM_STRING) {
            if(type != Atom::utf8String) {
                auto typeAtom = AtomName(_conn.get(), type);
                auto propAtom = AtomName(_conn.get(), prop);
                Application::warning("{}: type not {}, type {}, property {}", NS_FuncNameV, "string", typeAtom, propAtom);
                return res;
            }

            type = Atom::utf8String;
        }

        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, type, 0, ~0);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_property");
        } else if(const auto & reply = xcbReply.reply()) {
            auto beg = static_cast<const char*>(xcb_get_property_value(reply.get()));
            auto end = beg + xcb_get_property_value_length(reply.get());

            while(beg && beg < end) {
                res.emplace_back(beg);
                beg += res.back().size() + 1;
            }
        }

        return res;
    }

    std::pair<int, int> XCB::Connector::displayScreen(void) const {
        int display, screen;
        char* host;
        xcb_parse_display(nullptr, &host, &display, &screen);

        if(host) {
            free(host);
        }

        return std::make_pair(display, screen);
    }

    void XCB::Connector::bell(uint8_t percent) const {
        Application::notice("{}: beep", NS_FuncNameV);

        xcb_bell(_conn.get(), percent);
        xcb_flush(_conn.get());
    }

    /* XCB::RootDisplay */
    XCB::RootDisplay::RootDisplay(int displayNum, const AuthCookie* auth) {
        if(! displayConnect(displayNum, InitModules::All, auth)) {
            throw xcb_error(NS_FuncNameS);
        }
    }

    bool XCB::RootDisplay::displayConnect(int displayNum, int modules, const AuthCookie* auth) {
        if(! connectorDisplayConnect(displayNum, auth)) {
            return false;
        }

        _screen = xcb_setup_roots_iterator(_setup).data;

        if(! _screen) {
            Application::error("{}: {} failed", NS_FuncNameV, "root screen");
            return false;
        }

        // init format
        for(auto fIter = xcb_setup_pixmap_formats_iterator(_setup); fIter.rem; xcb_format_next(& fIter)) {
            if(fIter.data->depth == _screen->root_depth) {
                _format = fIter.data;
                break;
            }
        }

        if(! _format) {
            Application::error("{}: {} failed", NS_FuncNameV, "init format");
            return false;
        }

        // init visual
        for(auto dIter = xcb_screen_allowed_depths_iterator(_screen); dIter.rem; xcb_depth_next(& dIter)) {
            for(auto vIter = xcb_depth_visuals_iterator(dIter.data); vIter.rem; xcb_visualtype_next(& vIter)) {
                if(_screen->root_visual == vIter.data->visual_id) {
                    _visual = vIter.data;
                    break;
                }
            }
        }

        if(! _visual) {
            Application::error("{}: {} failed", NS_FuncNameV, "init visual");
            return false;
        }

        // required
        _modWinFixes = std::make_unique<ModuleWindowFixes>(_conn, _screen->root);

        // safe init modules
        if(modules & InitModules::Test) {
            try {
                _modTest = std::make_unique<ModuleTest>(_conn, _screen->root);
            } catch(const std::exception & err) {
                Application::warning("{}: {} failed", NS_FuncNameV, "ModuleTest");
            }
        }

        if(modules & InitModules::Shm) {
            try {
                _modShm = std::make_unique<ModuleShm>(_conn);
            } catch(const std::exception & err) {
                Application::warning("{}: {} failed", NS_FuncNameV, "ModuleShm");
            }
        }

        if(modules & InitModules::Damage) {
            try {
                _modWinDamage = std::make_unique<ModuleWindowDamage>(_conn, _screen->root);
            } catch(const std::exception & err) {
                Application::warning("{}: {} failed", NS_FuncNameV, "ModuleWindowDamage");
            }
        }

        if(modules & InitModules::RandR) {
            try {
                _modRandr = std::make_unique<ModuleRandr>(_conn, _screen->root);
            } catch(const std::exception & err) {
                Application::warning("{}: {} failed", NS_FuncNameV, "ModuleRandr");
            }
        }

        if(modules & InitModules::Xkb) {
            try {
                _modXkb = std::make_unique<ModuleXkb>(_conn);
            } catch(const std::exception & err) {
                Application::warning("{}: {} failed", NS_FuncNameV, "ModuleXkb");
            }
        }

        if(modules & InitModules::SelPaste) {
            try {
                _modSelectionPaste = std::make_unique<ModulePasteSelection>(_conn, *_screen);
            } catch(const std::exception & err) {
                Application::warning("{}: {} failed", NS_FuncNameV, "ModulePasteSelection");
            }
        }

        if(modules & InitModules::SelCopy) {
            try {
                _modSelectionCopy = std::make_unique<ModuleCopySelection>(_conn, *_screen);

                if(_modSelectionPaste) {
                    _modSelectionPaste->setSkipRequestor(_modSelectionCopy->selectionWindow());
                }
            } catch(const std::exception & err) {
                Application::warning("{}: {} failed", NS_FuncNameV, "ModuleCopySelection");
            }
        }

        updateGeometrySize();

        // create damage notify
        createFullScreenDamage();
        xcb_flush(_conn.get());

        xcbDisplayConnectedEvent();
        return true;
    }

    void XCB::RootDisplay::displayReconnect(int displayNum, int modules, const AuthCookie* auth) {
        _modSelectionPaste.reset();
        _modSelectionCopy.reset();
        _modWinDamage.reset();
        _modTest.reset();
        _modRandr.reset();
        _modShm.reset();
        _modXkb.reset();
        _modWinFixes.reset();

        //auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtensionConst(XCB::Module::TEST));

        _screen = nullptr;
        _format = nullptr;
        _visual = nullptr;

        displayConnect(displayNum, modules, auth);
    }

    XCB::Size XCB::RootDisplay::updateGeometrySize(void) const {
        auto xcbReply = getReplyFunc2(xcb_get_geometry, _conn.get(), _screen->root);

        if(const auto & reply = xcbReply.reply()) {
            std::unique_lock guard{ _lockGeometry };
            _screen->width_in_pixels = reply->width;
            _screen->height_in_pixels = reply->height;
        }

        return {_screen->width_in_pixels, _screen->height_in_pixels};
    }

    bool XCB::RootDisplay::createFullScreenDamage(void) {
        if(_modWinDamage) {
            _modWinDamage->addRegion(regionToXcb(region()));
            return true;
        }

        return false;
    }

    XCB::ModuleExtension* XCB::RootDisplay::getExtension(const Module & mod) {
        return const_cast<XCB::ModuleExtension*>(getExtensionConst(mod));
    }

    const XCB::ModuleExtension* XCB::RootDisplay::getExtension(const Module & mod) const {
        return getExtensionConst(mod);
    }

    const XCB::ModuleExtension* XCB::RootDisplay::getExtensionConst(const Module & mod) const {
        switch(mod) {
            case Module::SHM:
                return _modShm.get();

            case Module::DAMAGE:
            case Module::WINDAMAGE:
                return _modWinDamage.get();

            case Module::XFIXES:
            case Module::WINFIXES:
                return _modWinFixes.get();

            case Module::RANDR:
                return _modRandr.get();

            case Module::TEST:
                return _modTest.get();

            case Module::XKB:
                return _modXkb.get();

            case Module::SELECTION_PASTE:
                return _modSelectionPaste.get();

            case Module::SELECTION_COPY:
                return _modSelectionCopy.get();
        }

        return nullptr;
    }

    void XCB::RootDisplay::extensionDisable(const Module & mod) {
        switch(mod) {
            case Module::DAMAGE:
            case Module::WINDAMAGE:
                if(_modWinDamage) {
                    _modWinDamage.reset();
                    xcb_flush(_conn.get());
                }

                break;

            case Module::SELECTION_PASTE:
                if(_modSelectionPaste) {
                    _modSelectionPaste.reset();
                    xcb_flush(_conn.get());
                }

                break;

            case Module::SELECTION_COPY:
                if(_modSelectionCopy) {
                    _modSelectionCopy.reset();
                    xcb_flush(_conn.get());
                }

                break;

            case Module::XFIXES:
            case Module::WINFIXES:
                break;

            case Module::XKB:
                break;

            case Module::RANDR:
                if(_modRandr) {
                    _modRandr.reset();
                    xcb_flush(_conn.get());
                }

                break;

            case Module::TEST:
                if(_modTest) {
                    _modTest.reset();
                    xcb_flush(_conn.get());
                }

                break;

            case Module::SHM:
                if(_modShm) {
                    _modShm.reset();
                    xcb_flush(_conn.get());
                }

                break;
        }
    }

    bool XCB::RootDisplay::isDamageNotify(const GenericEvent & ev) const {
        return _modWinDamage ? _modWinDamage->isEventType(ev, XCB_DAMAGE_NOTIFY) : false;
    }

    bool XCB::RootDisplay::isXFixesSelectionNotify(const GenericEvent & ev) const {
        return _modWinFixes ? _modWinFixes->isEventType(ev, XCB_XFIXES_SELECTION_NOTIFY) : false;
    }

    bool XCB::RootDisplay::isXFixesCursorNotify(const GenericEvent & ev) const {
        return _modWinFixes ? _modWinFixes->isEventType(ev, XCB_XFIXES_CURSOR_NOTIFY) : false;
    }

    bool XCB::RootDisplay::isRandrScreenNotify(const GenericEvent & ev) const {
        // for receive it, usage input filter: xcb_xrandr_select_input(xcb_randr_notify_mask_t)
        return _modRandr ? _modRandr->isEventType(ev, XCB_RANDR_SCREEN_CHANGE_NOTIFY) : false;
    }

    bool XCB::RootDisplay::isRandrNotify(const GenericEvent & ev, const xcb_randr_notify_t & notify) const {
        if(_modRandr && _modRandr->isEventType(ev, XCB_RANDR_NOTIFY)) {
            auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
            return rn->subCode == notify;
        }

        return false;
    }

    bool XCB::RootDisplay::isXkbNotify(const GenericEvent & ev, int notify) const {
        if(_modXkb && _modXkb->isEventType(ev, 0)) {
            auto xn = reinterpret_cast<xkb_notify_event_t*>(ev.get());
            return xn->any.xkb_type == notify;
        }

        return false;
    }

    bool XCB::RootDisplay::setRandrMonitors(const std::vector<Region> & monitors) {
        if(! _modRandr) {
            return false;
        }

        Region screenArea;

        for(const auto & monitor : monitors) {
            screenArea.join(monitor);
        }

        if(screenArea.x) {
            screenArea.width += std::abs(screenArea.x);
            screenArea.x = 0;
        }

        if(screenArea.y) {
            screenArea.height += std::abs(screenArea.y);
            screenArea.y = 0;
        }

        screenArea.width = Tools::alignUp(screenArea.width, 8);
        Application::debug(DebugType::Xcb, "{}: screen area: {}", NS_FuncNameV, screenArea);

        auto resources = _modRandr->getScreenResources();
        if(! resources) {
            return false;
        }

        auto outputs = resources.getOutputs();
        auto crtcs = resources.getCrtcs();

        assert(crtcs.size() && outputs.size());
        assert(crtcs.size() <= outputs.size());
        const size_t maxmonitors = std::min(crtcs.size(), monitors.size());

        std::vector<xcb_randr_mode_t> modes;
        modes.reserve(monitors.size());

        for(size_t it = 0; it < maxmonitors; ++it) {
            const auto & monitor = monitors[it];
            xcb_randr_mode_t mode;

            if(! resources.findMode(monitor.toSize(), &mode)) {
                try {
                    mode = _modRandr->cvtCreateMode(monitor.toSize(), 20);
                } catch(const std::exception & err) {
                    Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                    return false;
                }
            }

            Application::debug(DebugType::Xcb, "{}: mode found: {}, size: {}", NS_FuncNameV, mode, monitor.toSize());
            modes.push_back(mode);
        }

        // diconnect all connected
        for(const auto & id: resources.getOutputs()) {
            if(auto info = _modRandr->getOutputInfo(id)) {
                if(info->connected) {
                    _modRandr->crtcDisconnect(info->crtc, resources->config_timestamp);
                }
            }
        }

        if(! _modRandr->setScreenSize(screenArea.toSize())) {
            return false;
        }

        bool success = true;

        for(size_t it = 0; it < maxmonitors; ++it) {
            const auto & monitor = monitors[it];
            const auto & mode = modes[it];
            const auto & output = outputs[it];
            const auto & crtc = crtcs[it];

            if(! _modRandr->addOutputMode(output, mode)) {
                success = false;
                continue;
            }
            
            if(! _modRandr->crtcConnectOutputsMode(crtc, mode, monitor.topLeft(), {&output, 1}, resources->config_timestamp)) {
                success = false;
            }
        }

        return success;
    }

    bool XCB::RootDisplay::setRandrScreenSize(const XCB::Size & sz, uint16_t* sequence) {
        if(! _modRandr) {
            return false;
        }

        const Region monitor(0, 0, Tools::alignUp(sz.width, 8), sz.height);
        Application::debug(DebugType::Xcb, "{}: size: {}", NS_FuncNameV, monitor.toSize());

        auto resources = _modRandr->getScreenResources();
        if(! resources) {
            return false;
        }

        auto outputs = resources.getOutputs();
        auto crtcs = resources.getCrtcs();

        if(outputs.empty()) {
            Application::error("{}: {} failed", NS_FuncNameV, "getOutputs");
            return false;
        }

        if(crtcs.empty()) {
            Application::error("{}: {} failed", NS_FuncNameV, "getCrtcs");
            return false;
        }

        auto crtc = _modRandr->getCrtcInfo(crtcs[0], resources->config_timestamp);
        if(! crtc) {
            return false;
        }

        if(Application::isDebugLevel(DebugLevel::Debug)) {
            Application::debug(DebugType::Xcb, "{}: crtc: {} - mode: {}, time: {}, x: {}, y: {}, width: {}, height: {}, status: {}", NS_FuncNameV,
                               crtcs[0], crtc->mode, crtc->timestamp, crtc->x, crtc->y, crtc->width, crtc->height, (int) crtc->status);

            if(auto out = _modRandr->getOutputInfo(outputs[0], resources->config_timestamp)) {
                Application::debug(DebugType::Xcb, "{}: output: {} - modes: {}, crtcs: {}, name: `{}', crtc: {}, connected: {}", NS_FuncNameV,
                               outputs[0], out->modes.size(), out->crtcs.size(), out->name, out->crtc, (int) out->connected);
            }
        }

        if(_modRandr->setDisplaySize(monitor, crtcs[0], outputs[0], resources, sequence)) {
            // success
            return true;
        }

        // restore mode
        _modRandr->setDisplaySize(Region{crtc->x, crtc->y, crtc->width, crtc->height}, crtcs[0], outputs[0], resources);

        return false;
    }

    size_t XCB::RootDisplay::bitsPerPixel(void) const {
        return _format ? _format->bits_per_pixel : 0;
    }

    size_t XCB::RootDisplay::scanlinePad(void) const {
        return _format ? _format->scanline_pad : 0;
    }

    const xcb_visualtype_t* XCB::RootDisplay::visual(xcb_visualid_t id) const {
        for(auto dIter = xcb_screen_allowed_depths_iterator(_screen); dIter.rem; xcb_depth_next(& dIter)) {
            for(auto vIter = xcb_depth_visuals_iterator(dIter.data); vIter.rem; xcb_visualtype_next(& vIter)) {
                if(id == vIter.data->visual_id) {
                    return vIter.data;
                }
            }
        }

        return nullptr;
    }

    const xcb_visualtype_t* XCB::RootDisplay::visual(void) const {
        return _visual;
    }

    size_t XCB::RootDisplay::depth(void) const {
        return _screen->root_depth;
    }

    XCB::Region XCB::RootDisplay::region(void) const {
        std::shared_lock guard{ _lockGeometry };
        return { 0, 0, _screen->width_in_pixels, _screen->height_in_pixels };
    }

    XCB::Size XCB::RootDisplay::size(void) const {
        std::shared_lock guard{ _lockGeometry };
        return { _screen->width_in_pixels, _screen->height_in_pixels };
    }

    uint16_t XCB::RootDisplay::width(void) const {
        std::shared_lock guard{ _lockGeometry };
        return _screen->width_in_pixels;
    }

    uint16_t XCB::RootDisplay::height(void) const {
        std::shared_lock guard{ _lockGeometry };
        return _screen->height_in_pixels;
    }

    xcb_window_t XCB::RootDisplay::root(void) const {
        return _screen->root;
    }

    XCB::PixmapInfoReply XCB::RootDisplay::copyRootImageRegion(const Region & reg, ShmIdShared shm) const {
        Application::debug(DebugType::Xcb, "{}: region: {}", NS_FuncNameV, reg);
        const uint32_t planeMask = 0xFFFFFFFF;

        if(shm && 0 < shm->id) {
            auto xcbReply = getReplyFunc2(xcb_shm_get_image, _conn.get(), _screen->root, reg.x, reg.y, reg.width, reg.height,
                                          planeMask, XCB_IMAGE_FORMAT_Z_PIXMAP, shm->id, 0);
            PixmapInfoReply res;

            if(const auto & err = xcbReply.error()) {
                // после быстрого изменения размера здесь ошибка
                // xcb_shm_get_image failed, error: Match, extension: none, major: Shm, minor: GetImage
                //
                if(err->error_code == 8 && err->major_code == 0x82 && err->minor_code == 0x04) {
                    throw xcb_error_busy(NS_FuncNameS);
                }

                // error code: 8, major: 0x82, minor: 0x04
                extendedError(err.get(), NS_FuncNameV, "xcb_shm_get_image");
            } else if(const auto & reply = xcbReply.reply()) {
                auto visptr = visual(reply->visual);
                auto bpp = bppFromDepth(reply->depth);

                if(visptr) {
                    return std::make_unique<PixmapSHM>(visptr->red_mask, visptr->green_mask, visptr->blue_mask, bpp, std::move(shm), reply->size);
                }
            }

            shm->reset();
        }

        size_t pitch = reg.width * (bitsPerPixel() >> 3);
        PixmapInfoReply res;

        if(pitch == 0) {
            Application::error("{}: copy root image error, empty size: {}, bpp: {}", NS_FuncNameV, reg.toSize(), bitsPerPixel());
            return res;
        }

        uint32_t maxReqLength = xcb_get_maximum_request_length(_conn.get());
        uint16_t allowRows = std::min(static_cast<uint16_t>(maxReqLength / pitch), reg.height);

        Application::debug(DebugType::Xcb, "{}: max request size: {}, allow rows: {}", NS_FuncNameV, maxReqLength, allowRows);

        for(int32_t yy = reg.y; yy < reg.y + reg.height; yy += allowRows) {
            // last rows
            if(yy + allowRows > reg.y + reg.height) {
                allowRows = reg.y + reg.height - yy;
            }

            auto xcbReply = getReplyFunc2(xcb_get_image, _conn.get(), XCB_IMAGE_FORMAT_Z_PIXMAP, _screen->root, reg.x, yy, reg.width, allowRows, planeMask);

            if(const auto & err = xcbReply.error()) {
                // после быстрого изменения размера здесь ошибка
                // xcb_get_image failed, error code: 8, major: 0x49, minor: 0x00
                //
                if(err->error_code == 8 && err->major_code == 0x49 && err->minor_code == 0) {
                    throw xcb_error_busy(NS_FuncNameS);
                }

                extendedError(err.get(), NS_FuncNameV, "xcb_get_image");
                break;
            }

            if(const auto & reply = xcbReply.reply()) {
                if(! res) {
                    auto visptr = visual(reply->visual);
                    auto bitsPerPixel = bppFromDepth(reply->depth);

                    if(! visptr) {
                        Application::error("{}: unknown visual id: {:#010x}", NS_FuncNameV, reply->visual);
                        break;
                    }

                    res = std::make_unique<PixmapBuffer>(visptr->red_mask, visptr->green_mask, visptr->blue_mask, bitsPerPixel, reg.height * pitch);
                }

                auto info = static_cast<PixmapBuffer*>(res.get());

                auto data = xcb_get_image_data(reply.get());
                auto length = xcb_get_image_data_length(reply.get());
                Application::debug(DebugType::Xcb, "{}: receive length: {}", NS_FuncNameV, length);

                info->pixels.insert(info->pixels.end(), data, data + length);
            }
        }

        return res;
    }

    bool XCB::RootDisplay::rootDamageAddRegions(const xcb_rectangle_t* rects, size_t counts) {
        if(_modWinDamage && _modWinDamage->addRegions(rects, counts)) {
            xcb_flush(_conn.get());
            return true;
        }

        return false;
    }

    bool XCB::RootDisplay::rootDamageAddRegion(const Region & reg) {
        if(_modWinDamage && _modWinDamage->addRegion(reg)) {
            xcb_flush(_conn.get());
            return true;
        }

        return false;
    }

    bool XCB::RootDisplay::rootDamageSubtrack(const Region & reg) {
        if(_modWinDamage && _modWinDamage->subtrackRegion(reg)) {
            xcb_flush(_conn.get());
            return true;
        }

        return false;
    }

    XCB::GenericEvent XCB::RootDisplay::pollEvent(void) {
        auto ev = GenericEvent(xcb_poll_for_event(_conn.get()));

        if(isDamageNotify(ev)) {
            auto wsz = size();
            auto dn = reinterpret_cast<xcb_damage_notify_event_t*>(ev.get());

            if(dn->area.x + dn->area.width > wsz.width || dn->area.y + dn->area.height > wsz.height) {
                Application::warning("{}: damage discard, region: {}, level: {}, sequence: {}, timestamp: {}",
                                     NS_FuncNameV, dn->area, dn->level, dn->sequence, dn->timestamp);
                xcb_discard_reply(_conn.get(), dn->sequence);
                return GenericEvent();
            }

            //            Application::debug(DebugType::Xcb, "{}: damage notify, region: {}, level: {}, sequence: {}, timestamp: {}",
            //                               NS_FuncNameV, dn->area, dn->level, dn->sequence, dn->timestamp);

            xcbDamageNotifyEvent(dn->area);
        } else if(isXFixesSelectionNotify(ev)) {
            auto sn = reinterpret_cast<xcb_xfixes_selection_notify_event_t*>(ev.get());
            auto selAtom = AtomName(_conn.get(), sn->selection);
            Application::debug(DebugType::Xcb, "{}: selection notify, subtype: {}, window: {:#010x}, owner: {:#010x}, selection {}, time1: {}, time2: {}",
                               NS_FuncNameV, sn->subtype, sn->window, sn->owner, selAtom, sn->timestamp, sn->selection_timestamp);

            if(_modSelectionCopy) {
                _modSelectionCopy->xfixesSelectionNotifyEvent(sn);
            }
        } else if(isXFixesCursorNotify(ev)) {
            auto cn = reinterpret_cast<xcb_xfixes_cursor_notify_event_t*>(ev.get());
            auto nameAtom = AtomName(_conn.get(), cn->name);

            Application::debug(DebugType::Xcb, "{}: cursor notify, serial: {:#010x}, name {}, sequence: {}, timestamp: {}",
                               NS_FuncNameV, cn->cursor_serial, nameAtom, cn->sequence, cn->timestamp);

            xcbFixesCursorChangedEvent();
        } else if(isRandrNotify(ev, XCB_RANDR_NOTIFY_CRTC_CHANGE)) {
            auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
            const xcb_randr_crtc_change_t & cc = rn->u.cc;

            if(0 < cc.width && 0 < cc.height) {
                auto wsz = updateGeometrySize();

                if(cc.width != wsz.width || cc.height != wsz.height) {
                    Application::warning("{}: crtc change discard, size: {}, current: {}, sequence: {}, timestamp: {}",
                                         NS_FuncNameV, Size(cc.width, cc.height), wsz, rn->sequence, cc.timestamp);
                    xcb_discard_reply(_conn.get(), rn->sequence);
                    return GenericEvent();
                }

                Application::debug(DebugType::Xcb, "{}: crtc change notify, size: {}, crtc: {:#010x}, mode: {}, rotation: {:#06x}, sequence: {}, timestamp: {}",
                                   NS_FuncNameV, Size(cc.width, cc.height), cc.crtc, cc.mode, cc.rotation, rn->sequence, cc.timestamp);

                //if(createFullScreenDamage()) {
                //    xcb_flush(_conn.get());
                //}

                xcbRandrScreenChangedEvent(wsz, *rn);
            }
        } else if(isXkbNotify(ev, XCB_XKB_MAP_NOTIFY)) {
            auto mn = reinterpret_cast<xcb_xkb_map_notify_event_t*>(ev.get());
            Application::debug(DebugType::Xcb, "{}: xkb notify: {}, min keycode: {}, max keycode: {}, changed: {:#06x}, sequence: {}, timestamp: {}",
                               NS_FuncNameV, "map", mn->minKeyCode, mn->maxKeyCode, mn->changed, mn->sequence, mn->time);

            /*
                        if(auto setup = const_cast<xcb_setup_t*>(xcb_get_setup(_conn.get())))
                        {
                            setup->min_keycode = mn->minKeyCode;
                            setup->max_keycode = mn->maxKeyCode;
                        }
            */
            _modXkb->resetMapState();
        } else if(isXkbNotify(ev, XCB_XKB_NEW_KEYBOARD_NOTIFY)) {
            auto kn = reinterpret_cast<xcb_xkb_new_keyboard_notify_event_t*>(ev.get());
            Application::debug(DebugType::Xcb, "{}: xkb notify: {}, devid: {}, old devid: {}, min keycode: {}, max keycode: {}, changed: {:#06x}, sequence: {}, timestamp: {}",
                               NS_FuncNameV, "new keyboard", kn->deviceID, kn->oldDeviceID, kn->minKeyCode, kn->maxKeyCode, kn->changed, kn->sequence, kn->time);

            if(kn->deviceID == _modXkb->devid && (kn->changed & XCB_XKB_NKN_DETAIL_KEYCODES)) {
                /*
                                if(auto setup = const_cast<xcb_setup_t*>(xcb_get_setup(_conn.get())))
                                {
                                    setup->min_keycode = kn->minKeyCode;
                                    setup->max_keycode = kn->maxKeyCode;
                                }
                */
                Application::debug(DebugType::Xcb, "{}: reset map, devid: {}", NS_FuncNameV, kn->deviceID);
                _modXkb->resetMapState();
            }
        } else if(isXkbNotify(ev, XCB_XKB_STATE_NOTIFY)) {
            auto sn = reinterpret_cast<xcb_xkb_state_notify_event_t*>(ev.get());
            Application::debug(DebugType::Xcb, "{}: xkb notify: {}, xkb type: {:#04x}, devid: {}, mods: {:#04x}, group: {}, changed: {:#06x}, sequence: {}, timestamp: {}",
                               NS_FuncNameV, "state", sn->xkbType, sn->deviceID, sn->mods, sn->group, sn->changed, sn->sequence, sn->time);

            xkb_state_update_mask(_modXkb->state.get(), sn->baseMods, sn->latchedMods, sn->lockedMods,
                                  sn->baseGroup, sn->latchedGroup, sn->lockedGroup);

            if(sn->changed & XCB_XKB_STATE_PART_GROUP_STATE) {
                // changed layout group
                xcbXkbGroupChangedEvent(sn->group);
            }
        }

        auto response_type = ev ? ev->response_type & ~0x80 : 0;

        switch(response_type) {
            case XCB_DESTROY_NOTIFY:
                if(_modSelectionPaste) {
                    _modSelectionPaste->destroyNotifyEvent(reinterpret_cast<xcb_destroy_notify_event_t*>(ev.get()));
                }

                break;

            case XCB_PROPERTY_NOTIFY:
                if(_modSelectionPaste) {
                    _modSelectionPaste->propertyNotifyEvent(reinterpret_cast<xcb_property_notify_event_t*>(ev.get()));
                }

                if(_modSelectionCopy) {
                    _modSelectionCopy->propertyNotifyEvent(reinterpret_cast<xcb_property_notify_event_t*>(ev.get()));
                }

                break;

            case XCB_SELECTION_CLEAR:
                if(_modSelectionPaste) {
                    _modSelectionPaste->selectionClearEvent(reinterpret_cast<xcb_selection_clear_event_t*>(ev.get()));
                }

                break;

            case XCB_SELECTION_REQUEST:
                if(_modSelectionPaste) {
                    _modSelectionPaste->selectionRequestEvent(reinterpret_cast<xcb_selection_request_event_t*>(ev.get()));
                }

                break;

            case XCB_SELECTION_NOTIFY:
                if(_modSelectionCopy) {
                    _modSelectionCopy->selectionNotifyEvent(reinterpret_cast<xcb_selection_notify_event_t*>(ev.get()));
                }

                break;

            default:
                break;
        }

        return ev;
    }

    xcb_keycode_t XCB::RootDisplay::keysymToKeycode(xcb_keysym_t keysym) const {
        return keysymGroupToKeycode(keysym, _modXkb ? _modXkb->getLayoutGroup() : 0);
    }

    std::pair<xcb_keycode_t, int> XCB::RootDisplay::keysymToKeycodeGroup(xcb_keysym_t keysym) const {
        auto empty = std::make_pair<xcb_keycode_t, int>(NULL_KEYCODE, -1);
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, _conn.get(), _setup->min_keycode, _setup->max_keycode - _setup->min_keycode + 1);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_keyboard_mapping");
            return empty;
        }

        const auto & reply = xcbReply.reply();

        if(! reply) {
            Application::error("{}: {} failed", NS_FuncNameV, "xcb_get_keyboard_mapping");
            return empty;
        }

        const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply.get());

        if(! keysyms) {
            Application::error("{}: {} failed", NS_FuncNameV, "xcb_get_keyboard_mapping_keysyms");
            return empty;
        }

        int keysymsPerKeycode = reply->keysyms_per_keycode;

        if(1 > keysymsPerKeycode) {
            Application::error("{}: {} failed", NS_FuncNameV, "keysyms_per_keycode");
            return empty;
        }

        int keysymsLength = xcb_get_keyboard_mapping_keysyms_length(reply.get());
        int keycodesCount = keysymsLength / keysymsPerKeycode;

        Application::trace(DebugType::Xcb, "{}: keysym: {:#010x}, keysym per keycode: {}, keysyms counts: {}, keycodes count: {}",
                           NS_FuncNameV, keysym, keysymsPerKeycode, keysymsLength, keycodesCount);

        // shifted/unshifted
        int groupsCount = keysymsPerKeycode >> 1;

        for(int group = 0; group < groupsCount; ++group) {
            for(int ii = 0; ii < keycodesCount; ++ii) {
                auto keycode = _setup->min_keycode + ii;
                int index = ii * keysymsPerKeycode + group * 2;

                if(index + 1 >= keysymsLength) {
                    Application::error("{}: index out of range {}, current group: {}, keysym per keycode: {}, keysyms counts: {}, keycodes count: {}",
                                       NS_FuncNameV, index, group, keysymsPerKeycode, keysymsLength, keycodesCount);
                    return empty;
                }

                // check normal/shifted keysyms
                if(keysym == keysyms[index] || keysym == keysyms[index + 1]) {
                    return std::make_pair(keycode, group);
                }
            }
        }

        if(_modXkb) {
            Application::warning("{}: keysym not found {:#010x}, group names: [{}]", NS_FuncNameV, keysym, Tools::join(_modXkb->getNames(), ","));
        }

        return empty;
    }

    xcb_keycode_t XCB::RootDisplay::keysymGroupToKeycode(xcb_keysym_t keysym, int group) const {
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, _conn.get(), _setup->min_keycode, _setup->max_keycode - _setup->min_keycode + 1);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_keyboard_mapping");
            return NULL_KEYCODE;
        }

        const auto & reply = xcbReply.reply();

        if(! reply) {
            Application::error("{}: {} failed", NS_FuncNameV, "xcb_get_keyboard_mapping");
            return NULL_KEYCODE;
        }

        const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply.get());

        if(! keysyms) {
            Application::error("{}: {} failed", NS_FuncNameV, "xcb_get_keyboard_mapping_keysyms");
            return NULL_KEYCODE;
        }

        int keysymsPerKeycode = reply->keysyms_per_keycode;

        if(1 > keysymsPerKeycode) {
            Application::error("{}: {} failed", NS_FuncNameV, "keysyms_per_keycode");
            return NULL_KEYCODE;
        }

        // [shifted, unshifted] pairs
        int groupsCount = keysymsPerKeycode >> 1;

        if(0 > group || groupsCount <= group) {
            Application::error("{}: unknown group: {}, groups count: {}", NS_FuncNameV, group, groupsCount);
            return NULL_KEYCODE;
        }

        int keysymsLength = xcb_get_keyboard_mapping_keysyms_length(reply.get());
        int keycodesCount = keysymsLength / keysymsPerKeycode;

        Application::debug(DebugType::Xcb, "{}: keysym: {:#010x}, current group: {}, keysym per keycode: {}, keysyms counts: {}, keycodes count: {}",
                           NS_FuncNameV, keysym, group, keysymsPerKeycode, keysymsLength, keycodesCount);

        for(int ii = 0; ii < keycodesCount; ++ii) {
            auto keycode = _setup->min_keycode + ii;
            int index = ii * keysymsPerKeycode + group * 2;

            if(index + 1 >= keysymsLength) {
                Application::error("{}: index out of range {}, current group: {}, keysym per keycode: {}, keysyms counts: {}, keycodes count: {}",
                                   NS_FuncNameV, index, group, keysymsPerKeycode, keysymsLength, keycodesCount);
                return NULL_KEYCODE;
            }

            // check normal keysyms
            if(keysym == keysyms[index]) {
                return keycode;
            }

            // check shifted keysyms
            if(keysym == keysyms[index + 1]) {
                return keycode;
            }
        }

        Application::warning("{}: keysym not found {:#010x}, curent group: {}", NS_FuncNameV, keysym, group);
        return NULL_KEYCODE;
    }

    xcb_keysym_t XCB::RootDisplay::keycodeGroupToKeysym(xcb_keycode_t keycode, int group, bool shifted) const {
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, _conn.get(), _setup->min_keycode, _setup->max_keycode - _setup->min_keycode + 1);

        if(const auto & err = xcbReply.error()) {
            extendedError(err.get(), NS_FuncNameV, "xcb_get_keyboard_mapping");
            return 0;
        }

        const auto & reply = xcbReply.reply();

        if(! reply) {
            Application::error("{}: {} failed", NS_FuncNameV, "xcb_get_keyboard_mapping");
            return 0;
        }

        const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply.get());

        if(! keysyms) {
            Application::error("{}: {} failed", NS_FuncNameV, "xcb_get_keyboard_mapping_keysyms");
            return 0;
        }

        int keysymsPerKeycode = reply->keysyms_per_keycode;

        if(1 > keysymsPerKeycode) {
            Application::error("{}: {} failed", NS_FuncNameV, "keysyms_per_keycode");
            return 0;
        }

        int keysymsLength = xcb_get_keyboard_mapping_keysyms_length(reply.get());
        int keycodesCount = keysymsLength / keysymsPerKeycode;

        Application::debug(DebugType::Xcb, "{}: keycode: {:#04x}, keysym per keycode: {}, keysyms counts: {}, keycodes count: {}",
                           NS_FuncNameV, keycode, keysymsPerKeycode, keysymsLength, keycodesCount);

        int index = (keycode - _setup->min_keycode) * keysymsPerKeycode + group * 2;

        if(index + 1 >= keysymsLength) {
            Application::error("{}: index out of range {}, current group: {}, keysym per keycode: {}, keysyms counts: {}, keycodes count: {}",
                               NS_FuncNameV, index, group, keysymsPerKeycode, keysymsLength, keycodesCount);
            return 0;
        }

        return shifted ? keysyms[index + 1] : keysyms[index];
    }

    xcb_keycode_t XCB::RootDisplay::keysymToKeycodeAuto(xcb_keysym_t keysym) const {
        auto keycode = keysymToKeycode(keysym);

        if(keycode == NULL_KEYCODE) {
            auto [keysymKeycode, keysymGroup] = keysymToKeycodeGroup(keysym);

            if(keysymKeycode != NULL_KEYCODE) {
                if(_modXkb) {
                    _modXkb->switchLayoutGroup(keysymGroup);
                }

                keycode = keysymKeycode;
            }
        }

        return keycode;
    }
    /*
        void XCB::RootDisplay::fakeInputKeycode(xcb_keycode_t keycode, bool pressed) const
        {
            if(_modTest)
            {
                _modTest->fakeInputRaw(_screen->root, pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, 0, 0);
            }
        }

        void XCB::RootDisplay::fakeInputKeysym(xcb_keysym_t keysym, bool pressed)
        {
            auto keycode = keysymToKeycode(keysym);

            if(keycode == NULL_KEYCODE)
            {
                auto [keysymKeycode, keysymGroup] = keysymToKeycodeGroup(keysym);

                if(keysymKeycode != NULL_KEYCODE)
                {
                    if(pressed)
                    {
                        Application::debug(DebugType::Xcb, "{}: keysym {:#010x} was found the another group {}, switched it", NS_FuncNameV, keysym, keysymGroup);
                    }

                    if(_modXkb)
                    {
                        _modXkb->switchLayoutGroup(keysymGroup);
                    }

                    keycode = keysymKeycode;
                }
            }

            if(keycode != NULL_KEYCODE)
            {
                fakeInputKeycode(keycode, pressed);
            }
        }
    */
    void XCB::RootDisplay::fillBackground(uint8_t r, uint8_t g, uint8_t b) {
        fillRegion(r, g, b, region());
    }

    void XCB::RootDisplay::fillRegion(uint8_t r, uint8_t g, uint8_t b, const Region & reg) {
        uint32_t color = 0;

        switch(depth()) {
            case 30:
                color = pfx30.pixel({r, g, b, 0});
                break;

            case 24:
                color = pf888.pixel({r, g, b, 0});
                break;

            case 16:
                color = pf565.pixel({r, g, b, 0});
                break;

            case 15:
                color = pf555.pixel({r, g, b, 0});
                break;

            default:
                Application::error("{}: unknown depth: {}", NS_FuncNameV, depth());
                throw xcb_error(NS_FuncNameS);
        }

        uint32_t colors[] = { color };
        auto cookie = xcb_change_window_attributes(_conn.get(), _screen->root, XCB_CW_BACK_PIXEL, colors);

        if(auto err = checkRequest(cookie)) {
            extendedError(err.get(), NS_FuncNameV, "xcb_change_window_attributes");
        } else {
            cookie = xcb_clear_area_checked(_conn.get(), 0, _screen->root, reg.x, reg.y, reg.width, reg.height);

            if(auto err = checkRequest(cookie)) {
                extendedError(err.get(), NS_FuncNameV, "xcb_clear_area");
            }
        }
    }
}
