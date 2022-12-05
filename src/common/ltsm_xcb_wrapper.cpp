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

using namespace std::chrono_literals;

namespace LTSM
{
    namespace XCB
    {
        PointIterator & PointIterator::operator++(void)
        {
            if(! isValid())
                return *this;

            if(x < limit.width)
                x++;

            if(x == limit.width && y < limit.height)
            {
                x = 0;
                y++;

                if(y == limit.height)
                {
                    x = -1;
                    y = -1;
                }
            }

            return *this;
        }

        PointIterator & PointIterator::operator--(void)
        {
            if(! isValid())
                return *this;

            if(x > 0)
                x--;

            if(x == 0 && y > 0)
            {
                x = limit.width - 1;
                y--;

                if(y == 0)
                {
                    x = -1;
                    y = -1;
                }
            }

            return *this;
        }

        Region operator- (const Region & reg, const Point & pt)
        {
            return Region(reg.x - pt.x, reg.y - pt.y, reg.width, reg.height);
        }

        Region operator+ (const Region & reg, const Point & pt)
        {
            return Region(reg.x + pt.x, reg.y + pt.y, reg.width, reg.height);
        }

        void Region::reset(void)
        {
            x = -1;
            y = -1;
            width = 0;
            height = 0;
        }

        void Region::assign(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh)
        {
            x = rx;
            y = ry;
            width = rw;
            height = rh;
        }

        void Region::assign(const Region & reg)
        {
            *this = reg;
        }

        Region Region::align(size_t val) const
        {
            Region res(x, y, width, height);

            if(auto alignX = x % val)
            {
                res.x -= alignX;
                res.width += alignX;
            }

            if(auto alignY = y % val)
            {
                res.y -= alignY;
                res.height += alignY;
            }

            if(auto alignW = res.width % val)
                res.width += val - alignW;

            if(auto alignH = res.height % val)
                res.height += val - alignH;

            return res;
        }

        void Region::join(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh)
        {
            join({rx, ry, rw, rh});
        }

        void Region::join(const Region & reg)
        {
            if(invalid())
            {
                x = reg.x;
                y = reg.y;
                width = reg.width;
                height = reg.height;
            }
            else if(! reg.empty() && *this != reg)
            {
                /* Horizontal union */
                auto xm = std::min(x, reg.x);
                width = std::max(x + width, reg.x + reg.width) - xm;
                x = xm;
                /* Vertical union */
                auto ym = std::min(y, reg.y);
                height = std::max(y + height, reg.y + reg.height) - ym;
                y = ym;
            }
        }

        bool Region::empty(void) const
        {
            return width == 0 || height == 0;
        }

        bool Region::invalid(void) const
        {
            return x == -1 && y == -1 && empty();
        }

        Region Region::intersected(const Region & reg) const
        {
            Region res;
            intersection(*this, reg, & res);
            return res;
        }

        bool Region::intersects(const Region & reg1, const Region & reg2)
        {
            // check reg1.empty || reg2.empty
            if(reg1.empty() || reg2.empty())
                return false;

            // horizontal intersection
            if(std::min(reg1.x + reg1.width, reg2.x + reg2.width) <= std::max(reg1.x, reg2.x))
                return false;

            // vertical intersection
            if(std::min(reg1.y + reg1.height, reg2.y + reg2.height) <= std::max(reg1.y, reg2.y))
                return false;

            return true;
        }

        bool Region::intersection(const Region & reg1, const Region & reg2, Region* res)
        {
            bool intersects = Region::intersects(reg1, reg2);

            if(! intersects)
                return false;

            if(! res)
                return intersects;

            // horizontal intersection
            res->x = std::max(reg1.x, reg2.x);
            res->width = std::min(reg1.x + reg1.width, reg2.x + reg2.width) - res->x;
            // vertical intersection
            res->y = std::max(reg1.y, reg2.y);
            res->height = std::min(reg1.y + reg1.height, reg2.y + reg2.height) - res->y;
            return ! res->empty();
        }

        std::list<Region> Region::divideCounts(uint16_t cols, uint16_t rows) const
        {
            uint16_t bw = width <= cols ? 1 : width / cols;
            uint16_t bh = height <= rows ? 1 : height / rows;
            return divideBlocks(Size(bw, bh));
        }

        std::list<Region> Region::divideBlocks(const Size & sz) const
        {
            std::list<Region> res;
            int cw = sz.width > width ? width : sz.width;
            int ch = sz.height > height ? height : sz.height;

            for(uint16_t yy = 0; yy < height; yy += ch)
            {
                for(uint16_t xx = 0; xx < width; xx += cw)
                {
                    uint16_t fixedw = std::min(width - xx, cw);
                    uint16_t fixedh = std::min(height - yy, ch);
                    res.emplace_back(x + xx, y + yy, fixedw, fixedh);
                }
            }

            return res;
        }

        void error(const xcb_generic_error_t* err, const char* func, const char* xcbname)
        {
            Application::error("%s: %s failed, error code: %d, major: 0x%02x, minor: 0x%04x, sequence: 0x%04x",
                               func, xcbname, static_cast<int>(err->error_code), static_cast<int>(err->major_code), err->minor_code, err->sequence);
        }

        void error(xcb_connection_t* conn, const xcb_generic_error_t* err, const char* func, const char* xcbname)
        {
#ifdef LTSM_BUILD_XCB_ERRORS
	    if(!conn || !ErrorContext(conn).error(err, func, xcbname))
#endif
    		error(err, func, xcbname);
	}
    }

    /* XCB::CursorImage */
    uint32_t* XCB::CursorImage::data(void)
    {
        return reply() ? xcb_xfixes_get_cursor_image_cursor_image(reply().get()) : nullptr;
    }

    const uint32_t* XCB::CursorImage::data(void) const
    {
        return reply() ? xcb_xfixes_get_cursor_image_cursor_image(reply().get()) : nullptr;
    }

    size_t XCB::CursorImage::size(void) const
    {
        return reply() ? xcb_xfixes_get_cursor_image_cursor_image_length(reply().get()) : 0;
    }

    namespace XCB
    {
        /* Atom */
        namespace Atom
        {
            xcb_atom_t wmName = XCB_ATOM_NONE;
            xcb_atom_t utf8String = XCB_ATOM_NONE;

            xcb_atom_t incr = XCB_ATOM_NONE;

            xcb_atom_t primary = XCB_ATOM_NONE;
            xcb_atom_t clipboard = XCB_ATOM_NONE;
            xcb_atom_t targets = XCB_ATOM_NONE;
            xcb_atom_t text = XCB_ATOM_NONE;
            xcb_atom_t textPlain = XCB_ATOM_NONE;

            std::string getName(xcb_connection_t* conn, xcb_atom_t atom)
            {
                if(atom == XCB_ATOM_NONE)
                    return "NONE";

                auto xcbReply = XCB::getReplyFunc2(xcb_get_atom_name, conn, atom);

                if(auto err = xcbReply.error())
                {
                    error(conn, err.get(), __FUNCTION__, "xcb_get_atom_name");
                    return "NONE";
                }

                if(auto reply = xcbReply.reply())
                {
                    const char* name = xcb_get_atom_name_name(reply.get());
                    size_t len = xcb_get_atom_name_name_length(reply.get());
                    return std::string(name, len);
                }

                return "NONE";
            }
        }

        PropertyReply getPropertyInfo(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t prop)
        {
            auto xcbReply = getReplyFunc2(xcb_get_property, conn, false, win, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 0);
            
            if(auto err = xcbReply.error())
                error(conn, err.get(), __FUNCTION__, "xcb_get_property");

            return xcbReply.reply();
        }
    }

    /* ModuleExtension */
    bool XCB::ModuleExtension::isEventType(const GenericEvent & ev, int type) const
    {
        // clear bit
        auto response_type = ev ? ev->response_type & ~0x80 : 0;

        if(ext && 0 < response_type)
        {
            return response_type == ext->first_event + type;
        }

        return false;
    }

    bool XCB::ModuleExtension::isEventError(const GenericEvent & ev, uint16_t* opcode) const
    {
        if(ext && ev && ev->response_type == 0)
        {
            auto err = ev.toerror();

            if(err &&
                err->major_code == ext->major_opcode)
            {
                if(opcode)
                    *opcode = err->minor_code;

                error(conn.get(), err, __FUNCTION__, "");
                return true;
            }
        }

        return false;
    }

    // XCB::ModuleDamage
    bool XCB::WindowDamageId::addRegion(const xcb_xfixes_region_t & region)
    {
        auto cookie = xcb_damage_add_checked(conn.get(), win, region);
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_damage_add");
            return false;
        }

	Application::debug("%s: resource id: 0x%08x", __FUNCTION__, region);
        return true;
    }

    bool XCB::WindowDamageId::subtrackRegion(const xcb_xfixes_region_t & repair)
    {
        auto cookie = xcb_damage_subtract_checked(conn.get(), xid, repair, XCB_XFIXES_REGION_NONE);
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_damage_subtract");
            return false;
        }

	Application::debug("%s: resource id: 0x%08x", __FUNCTION__, repair);
        return true;
    }

    XCB::ModuleDamage::ModuleDamage(ConnectionShared ptr) : ModuleExtension(ptr, Module::DAMAGE)
    {
        ext = xcb_get_extension_data(conn.get(), & xcb_damage_id);

        if(! ext || ! ext->present)
            throw xcb_error(NS_FuncName);

        auto xcbReply = getReplyFunc2(xcb_damage_query_version, conn.get(), XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_damage_query_version");
            throw xcb_error(NS_FuncName);
        }

        if(auto reply = xcbReply.reply())
        {
            Application::debug("used %s extension, version: %d.%d", "DAMAGE", reply->major_version, reply->minor_version);
        }
    }

    XCB::WindowDamageIdPtr XCB::ModuleDamage::createDamage(xcb_drawable_t win, const xcb_damage_report_level_t & level) const
    {
        xcb_damage_damage_t res = xcb_generate_id(conn.get());

        auto cookie = xcb_damage_create_checked(conn.get(), res, win, level);
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_damage_create");
            res = 0;
        }

	Application::info("%s: level: 0x%02x, resource id: 0x%08x", __FUNCTION__, level, res);
        return std::make_unique<WindowDamageId>(conn, win, res);
    }

    // XCB::ModuleFixes
    XCB::ModuleFixes::ModuleFixes(ConnectionShared ptr) : ModuleExtension(ptr, Module::XFIXES)
    {
        ext = xcb_get_extension_data(conn.get(), & xcb_xfixes_id);

        if(! ext || ! ext->present)
            throw xcb_error(NS_FuncName);

        auto xcbReply = getReplyFunc2(xcb_xfixes_query_version, conn.get(), XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xfixes_query_version");
            throw xcb_error(NS_FuncName);
        }

        if(auto reply = xcbReply.reply())
        {
            Application::debug("used %s extension, version: %d.%d", "XFIXES", reply->major_version, reply->minor_version);
        }
    }

    XCB::FixesRegionIdPtr XCB::ModuleFixes::createRegion(const xcb_rectangle_t & rect) const
    {
        xcb_xfixes_region_t res = xcb_generate_id(conn.get());

        auto cookie = xcb_xfixes_create_region_checked(conn.get(), res, 1, & rect);
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xfixes_create_region");
            res = 0;
        }

	Application::debug("%s: rect: [%d,%d,%d,%d], resource id: 0x%08x", __FUNCTION__, rect.x, rect.y, rect.width, rect.height, res);
        return std::make_unique<FixesRegionId>(conn, res);
    }
 
    XCB::FixesRegionIdPtr XCB::ModuleFixes::createRegions(const xcb_rectangle_t* rects, uint32_t counts) const
    {
        xcb_xfixes_region_t res = xcb_generate_id(conn.get());

        auto cookie = xcb_xfixes_create_region_checked(conn.get(), res, counts, rects);
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xfixes_create_region");
            res = 0;
        }

	Application::debug("%s: rects: %d, resource id: 0x%08x", __FUNCTION__, counts, res);
        return std::make_unique<FixesRegionId>(conn, res);
    }

    XCB::FixesRegionIdPtr XCB::ModuleFixes::intersectRegions(const xcb_xfixes_region_t & reg1, xcb_xfixes_region_t & reg2) const
    {
        xcb_xfixes_region_t res = xcb_generate_id(conn.get());

        auto cookie = xcb_xfixes_intersect_region_checked(conn.get(), reg1, reg2, res);
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xfixes_intersect_region");
            res = 0;
        }

	Application::debug("%s: reg1 id: 0x%08x, reg2 id;0x%08x, resource id: 0x%08x", __FUNCTION__, reg1, reg2, res);
        return std::make_unique<FixesRegionId>(conn, res);
    }

    XCB::FixesRegionIdPtr XCB::ModuleFixes::unionRegions(const xcb_xfixes_region_t & reg1, xcb_xfixes_region_t & reg2) const
    {
        xcb_xfixes_region_t res = xcb_generate_id(conn.get());

        auto cookie = xcb_xfixes_union_region_checked(conn.get(), reg1, reg2, res);
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xfixes_union_region");
            res = 0;
        }

	Application::debug("%s: reg1 id: 0x%08x, reg2 id;0x%08x, resource id: 0x%08x", __FUNCTION__, reg1, reg2, res);
        return std::make_unique<FixesRegionId>(conn, res);
    }

    xcb_rectangle_t XCB::ModuleFixes::fetchRegion(const xcb_xfixes_region_t & reg) const
    {
        auto xcbReply = getReplyFunc2(xcb_xfixes_fetch_region, conn.get(), reg);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xfixes_fetch_region");
        }

        if(auto reply = xcbReply.reply())
        {
	    auto rects = xcb_xfixes_fetch_region_rectangles(reply.get());
	    int count = xcb_xfixes_fetch_region_rectangles_length(reply.get());

	    if(rects && 0 < count)
		return rects[0];
        }

	return xcb_rectangle_t{ 0, 0, 0, 0};
    }

    std::vector<xcb_rectangle_t> XCB::ModuleFixes::fetchRegions(const xcb_xfixes_region_t & reg) const
    {
	std::vector<xcb_rectangle_t> res;
        auto xcbReply = getReplyFunc2(xcb_xfixes_fetch_region, conn.get(), reg);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xfixes_fetch_region");
        }

        if(auto reply = xcbReply.reply())
        {
	    auto rects = xcb_xfixes_fetch_region_rectangles(reply.get());
	    int count = xcb_xfixes_fetch_region_rectangles_length(reply.get());

	    if(rects && 0 < count)
		res.assign(rects, rects + count);
        }

	return res;
    }

    XCB::CursorImage XCB::ModuleFixes::cursorImage(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_xfixes_get_cursor_image, conn.get());

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xfixes_get_cursor_image");
        }

        return xcbReply;
    }

    // XCB::ModuleTest
    XCB::ModuleTest::ModuleTest(ConnectionShared ptr) : ModuleExtension(ptr, Module::TEST)
    {
        ext = xcb_get_extension_data(conn.get(), & xcb_test_id);

        if(! ext || ! ext->present)
            throw xcb_error(NS_FuncName);

        auto xcbReply = getReplyFunc2(xcb_test_get_version, conn.get(), XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_test_query_version");
            throw xcb_error(NS_FuncName);
        }

        if(auto reply = xcbReply.reply())
        {
            Application::debug("used %s extension, version: %d.%d", "TEST", reply->major_version, reply->minor_version);
        }
    }

    /// @param type: XCB_KEY_PRESS, XCB_KEY_RELEASE, XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE, XCB_MOTION_NOTIFY
    /// @param detail: keycode or left 1, middle 2, right 3, scrollup 4, scrolldw 5
    bool XCB::ModuleTest::fakeInputRaw(xcb_window_t win, uint8_t type, uint8_t detail, int16_t posx, int16_t posy) const
    {
        auto cookie = xcb_test_fake_input_checked(conn.get(), type, detail, XCB_CURRENT_TIME, win, posx, posy, 0);

        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_test_fake_input");
            return false;
        }

        return true;
    }

    void XCB::ModuleTest::fakeInputClickButton(xcb_window_t win, uint8_t button, const Point & pos) const
    {
        xcb_test_fake_input(conn.get(), XCB_BUTTON_PRESS, button, XCB_CURRENT_TIME, win, pos.x, pos.y, 0);
        xcb_test_fake_input(conn.get(), XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, win, pos.x, pos.y, 0);

        xcb_flush(conn.get());
    }

    // XCB::ModuleRandr
    XCB::ModuleRandr::ModuleRandr(ConnectionShared ptr) : ModuleExtension(ptr, Module::RANDR)
    {
        ext = xcb_get_extension_data(conn.get(), & xcb_randr_id);

        if(! ext || ! ext->present)
            throw xcb_error(NS_FuncName);

        auto xcbReply = getReplyFunc2(xcb_randr_query_version, conn.get(), XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_query_version");
            throw xcb_error(NS_FuncName);
        }

        if(auto reply = xcbReply.reply())
        {
            Application::debug("used %s extension, version: %d.%d", "RANDR", reply->major_version, reply->minor_version);
        }
    }

    std::vector<xcb_randr_output_t> XCB::ModuleRandr::getOutputs(const xcb_screen_t & screen) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_resources, conn.get(), screen.root);
        std::vector<xcb_randr_output_t> res;

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_get_screen_resources");
            return res;
        }

        if(auto reply = xcbReply.reply())
        {
            xcb_randr_output_t* ptr = xcb_randr_get_screen_resources_outputs(reply.get());
            int len = xcb_randr_get_screen_resources_outputs_length(reply.get());
            res.assign(ptr, ptr + len);
        }

        return res;
    }

    XCB::RandrOutputInfo XCB::ModuleRandr::getOutputInfo(const xcb_randr_output_t & id) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_output_info, conn.get(), id, XCB_CURRENT_TIME);
        RandrOutputInfo res;

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_get_output_info");
            return res;
        }

        if(auto reply = xcbReply.reply())
        {
            auto ptr = xcb_randr_get_output_info_name(reply.get());
            int len = xcb_randr_get_output_info_name_length(reply.get());
            res.name.assign(reinterpret_cast<const char*>(ptr), len);
            res.connected = reply->connection == XCB_RANDR_CONNECTION_CONNECTED;
            res.crtc = reply->crtc;
            res.mm_width = reply->mm_width;
            res.mm_height = reply->mm_height;
        }

        return res;
    }

    std::vector<xcb_randr_mode_t> XCB::ModuleRandr::getOutputModes(const xcb_randr_output_t & id) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_output_info, conn.get(), id, XCB_CURRENT_TIME);
        std::vector<xcb_randr_mode_t> res;

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_get_output_info");
            return res;
        }

        if(auto reply = xcbReply.reply())
        {
            xcb_randr_mode_t* ptr = xcb_randr_get_output_info_modes(reply.get());
            int len = xcb_randr_get_output_info_modes_length(reply.get());
            res.assign(ptr, ptr + len);
        }

        return res;
    }

    std::vector<xcb_randr_mode_info_t> XCB::ModuleRandr::getModesInfo(const xcb_screen_t & screen) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_resources, conn.get(), screen.root);
        std::vector<xcb_randr_mode_info_t> res;

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_get_screen_resources");
            return res;
        }

        if(auto reply = xcbReply.reply())
        {
            xcb_randr_mode_info_t* ptr = xcb_randr_get_screen_resources_modes(reply.get());
            int len = xcb_randr_get_screen_resources_modes_length(reply.get());
            res.assign(ptr, ptr + len);
        }

        return res;
    }

    std::vector<xcb_randr_crtc_t> XCB::ModuleRandr::getOutputCrtcs(const xcb_randr_output_t & id) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_output_info, conn.get(), id, XCB_CURRENT_TIME);
        std::vector<xcb_randr_mode_t> res;

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_get_output_info");
            return res;
        }

        if(auto reply = xcbReply.reply())
        {
            xcb_randr_mode_t* ptr = xcb_randr_get_output_info_crtcs(reply.get());
            int len = xcb_randr_get_output_info_crtcs_length(reply.get());
            res.assign(ptr, ptr + len);
        }

        return res;
    }

    XCB::RandrScreenInfo XCB::ModuleRandr::getScreenInfo(const xcb_screen_t & screen) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_info, conn.get(), screen.root);
        XCB::RandrScreenInfo res;

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_get_screen_info");
            return res;
        }

        if(auto reply = xcbReply.reply())
        {
            res.config_timestamp = reply->config_timestamp;
            res.sizeID = reply->sizeID;
            res.rotation = reply->rotation;
            res.rate = reply->rate;
        }

        return res;
    }

    std::vector<xcb_randr_screen_size_t> XCB::ModuleRandr::getScreenSizes(const xcb_screen_t & screen, RandrScreenInfo* info) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_info, conn.get(), screen.root);
        std::vector<xcb_randr_screen_size_t> res;

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_get_screen_info");
            return res;
        }

        if(auto reply = xcbReply.reply())
        {
            xcb_randr_screen_size_t* ptr = xcb_randr_get_screen_info_sizes(reply.get());
            int len = xcb_randr_get_screen_info_sizes_length(reply.get());
            res.assign(ptr, ptr + len);

            if(info)
            {
                info->config_timestamp = reply->config_timestamp;
                info->sizeID = reply->sizeID;
                info->rotation = reply->rotation;
                info->rate = reply->rate;
            }
        }

        return res;
    }

    xcb_randr_mode_t XCB::ModuleRandr::createMode(const xcb_screen_t & screen, const XCB::Size & sz) const
    {
        auto cvt = "/usr/bin/cvt";

        std::error_code err;
        if(! std::filesystem::exists(cvt, err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), cvt, getuid());
            return 0;
        }

        std::string cmd = Tools::runcmd(Tools::StringFormat("%1 %2 %3").arg(cvt).arg(sz.width).arg(sz.height));
        auto params = Tools::split(cmd.substr(cmd.find('\n', 0) + 1), 0x20);
        params.remove_if([](auto & val) { return val.empty(); });

        // params: Modeline "1024x600_60.00"   49.00  1024 1072 1168 1312  600 603 613 624 -hsync +vsync
        if(params.size() != 13)
        {
            Application::error("%s: incorrect cvt format, params: %d", __FUNCTION__, params.size());
            return 0;
        }

        xcb_randr_mode_info_t mode_info;
        mode_info.id = 0;
        mode_info.hskew = 0;
        mode_info.mode_flags = 0;

        auto it = std::next(params.begin(), 2);
        try
        {
            float clock = std::stof(*it);
            mode_info.dot_clock =  clock * 1000000;
            it = std::next(it);
            mode_info.width = std::stod(*it);
            it = std::next(it);
            mode_info.hsync_start = std::stod(*it);
            it = std::next(it);
            mode_info.hsync_end = std::stod(*it);
            it = std::next(it);
            mode_info.htotal = std::stod(*it);
            it = std::next(it);
            mode_info.height = std::stod(*it);
            it = std::next(it);
            mode_info.vsync_start = std::stod(*it);
            it = std::next(it);
            mode_info.vsync_end = std::stod(*it);
            it = std::next(it);
            mode_info.vtotal = std::stod(*it);
            it = std::next(it);
        }
        catch(const std::exception &)
        {
            Application::error("%s: unknown format outputs from cvt", __FUNCTION__);
            return 0;
        }

        if(*it == "-hsync")
            mode_info.mode_flags |= XCB_RANDR_MODE_FLAG_HSYNC_NEGATIVE;
        else if(*it == "+hsync")
            mode_info.mode_flags |= XCB_RANDR_MODE_FLAG_HSYNC_POSITIVE;

        it = std::next(it);

        if(*it == "-vsync")
            mode_info.mode_flags |= XCB_RANDR_MODE_FLAG_VSYNC_NEGATIVE;
        else if(*it == "+vsync")
            mode_info.mode_flags |= XCB_RANDR_MODE_FLAG_VSYNC_POSITIVE;

        auto name = Tools::StringFormat("%1x%2_60").arg(mode_info.width).arg(mode_info.height);
        mode_info.name_len = name.size();
        auto xcbReply = getReplyFunc2(xcb_randr_create_mode, conn.get(), screen.root, mode_info, name.size(), name.data());

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_create_mode");
            return 0;
        }

        if(auto reply = xcbReply.reply())
        {
            Application::debug("%s: id: %08x, width: %d, height: %d", __FUNCTION__, reply->mode, mode_info.width, mode_info.height);
            return reply->mode;
        }

        return 0;
    }

    bool XCB::ModuleRandr::destroyMode(const xcb_randr_mode_t & mode) const
    {
        auto cookie = xcb_randr_destroy_mode_checked(conn.get(), mode);

        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_destroy_mode_checked");
            return false;
        }

        Application::debug("%s: id: %08x", __FUNCTION__, mode);
        return true;
    }

    bool XCB::ModuleRandr::addOutputMode(const xcb_randr_output_t & output, const xcb_randr_mode_t & mode) const
    {
        auto cookie = xcb_randr_add_output_mode_checked(conn.get(), output, mode);

        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_add_output_mode");
            return false;
        }

        Application::debug("%s: id: %08x, output: %08x", __FUNCTION__, mode, output);
        return true;
    }

    bool XCB::ModuleRandr::deleteOutputMode(const xcb_randr_output_t & output, const xcb_randr_mode_t & mode) const
    {
        auto cookie = xcb_randr_delete_output_mode_checked(conn.get(), output, mode);

        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_delete_output_mode_checked");
            return false;
        }

        Application::debug("%s: id: %08x, output: %08x", __FUNCTION__, mode, output);
        return true;
    }

    bool XCB::ModuleRandr::setScreenSize(const xcb_screen_t & screen, XCB::Size sz, uint16_t* sequence) const
    {
        // align size
        if(auto alignW = sz.width % 8)
            sz.width += 8 - alignW;

        auto screenSizes = getScreenSizes(screen);
        auto its = std::find_if(screenSizes.begin(), screenSizes.end(),
                            [&](auto & ss){ return ss.width == sz.width && ss.height == sz.height; });

        xcb_randr_mode_t mode = 0;
        xcb_randr_output_t output = 0;

        // not found
        if(its == screenSizes.end())
        {
            // add new mode
            auto outputs = getOutputs(screen);
            auto ito = std::find_if(outputs.begin(), outputs.end(),
                            [this](auto & id){ return this->getOutputInfo(id).connected; });

            if(ito == outputs.end())
            {
                Application::error("%s: %s failed, outputs count: %d", __FUNCTION__, "getOutputs", outputs.size());
                return false;
            }

            output = *ito;
            mode = createMode(screen, sz);

            if(0 == mode)
                return false;

            if(! addOutputMode(output, mode))
            {
                Application::error("%s: %s failed, modes: [%d, %d]", __FUNCTION__, "addOutputMode", sz.width, sz.height);
                destroyMode(mode);
                return false;
            }

            // fixed size
            auto modes = getModesInfo(screen);
            auto itm = std::find_if(modes.begin(), modes.end(),
                                [=](auto & val){ return val.id == mode; });

            if(itm == modes.end())
            {
                Application::error("%s: %s failed, modes: [%d, %d]", __FUNCTION__, "getModesInfo", sz.width, sz.height);
                deleteOutputMode(output, mode);
                destroyMode(mode);
                return false;
            }

            sz.width = (*itm).width;
            sz.height = (*itm).height;

            // rescan info
            screenSizes = getScreenSizes(screen);
            its = std::find_if(screenSizes.begin(), screenSizes.end(),
                            [&](auto & ss) {return ss.width == sz.width && ss.height == sz.height; });

            if(its == screenSizes.end())
            {
                Application::error("%s: %s failed, modes: [%d, %d]", __FUNCTION__, "getScreenSizes", sz.width, sz.height);
                deleteOutputMode(output, mode);
                destroyMode(mode);
                return false;
            }
        }

        auto sizeID = std::distance(screenSizes.begin(), its);
        auto screenInfo = getScreenInfo(screen);

        auto xcbReply2 = getReplyFunc2(xcb_randr_set_screen_config, conn.get(), screen.root, XCB_CURRENT_TIME,
                                       screenInfo.config_timestamp, sizeID, screenInfo.rotation /*XCB_RANDR_ROTATION_ROTATE_0*/, 0);

        if(auto err = xcbReply2.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_randr_set_screen_config");
            return false;
        }

        if(auto reply = xcbReply2.reply())
        {
            if(sequence)
                *sequence = reply->sequence;

            Application::info("%s: set size: [%d, %d], id: %d, sequence: 0x%04x", __FUNCTION__, sz.width, sz.height, sizeID, reply->sequence);
        }

        return true;
    }

    // XCB::ModuleShm
    XCB::ModuleShm::ModuleShm(ConnectionShared ptr) : ModuleExtension(ptr, Module::SHM)
    {
        ext = xcb_get_extension_data(conn.get(), & xcb_shm_id);

        if(! ext || ! ext->present)
            throw xcb_error(NS_FuncName);

        auto xcbReply = getReplyFunc2(xcb_shm_query_version, conn.get());

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_shm_query_version");
            throw xcb_error(NS_FuncName);
        }

        if(auto reply = xcbReply.reply())
        {
            Application::debug("used %s extension, version: %d.%d", "SHM", reply->major_version, reply->minor_version);
        }
    }

    XCB::ShmId::~ShmId()
    {
        reset();
    }

    void XCB::ShmId::reset(void)
    {
        if(0 < id && conn)
            xcb_shm_detach(conn.get(), id);

        if(addr)
            shmdt(addr);
    
        if(0 < shm)
            shmctl(shm, IPC_RMID, nullptr);

        id = 0;
        addr = nullptr;
        shm = -1;
    }

    XCB::ShmIdShared XCB::ModuleShm::createShm(size_t shmsz, int mode, bool readOnly, uid_t owner) const
    {
        Application::info("%s: size: %d, mode: 0x%08x, read only: %d, owner: %d", __FUNCTION__, shmsz, mode, readOnly, owner);

        int shmId = shmget(IPC_PRIVATE, shmsz, IPC_CREAT | mode);
        if(shmId == -1)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "shmget", strerror(errno), errno);
            return nullptr;
        }

        auto shmAddr = reinterpret_cast<uint8_t*>(shmat(shmId, nullptr, (readOnly ? SHM_RDONLY : 0)));
        // man shmat: check result
        if(shmAddr == reinterpret_cast<uint8_t*>(-1) && 0 != errno)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "shmaddr", strerror(errno), errno);
            return nullptr;
        }

        if(0 < owner)
        {
            shmid_ds info;

            if(0 == shmctl(shmId, IPC_STAT, & info))
            {
                info.shm_perm.uid = owner;
                if(0 != shmctl(shmId, IPC_SET, & info))
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "shmctl", strerror(errno), errno);
            }
            else
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "shmctl", strerror(errno), errno);
            }
        }

        xcb_shm_seg_t res = xcb_generate_id(conn.get());

        auto cookie = xcb_shm_attach_checked(conn.get(), res, shmId, 0);
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_shm_attach");
            return nullptr;
        }

	Application::info("%s: resource id: 0x%08x", __FUNCTION__, res);
        return std::make_shared<ShmId>(conn, shmId, shmAddr, res);
    }

    // XCB::ModuleXkb
    XCB::ModuleXkb::ModuleXkb(ConnectionShared ptr) : ModuleExtension(ptr, Module::XKB),
        ctx { nullptr, xkb_context_unref }, map { nullptr, xkb_keymap_unref }, state { nullptr, xkb_state_unref }
    {
        ext = xcb_get_extension_data(conn.get(), & xcb_xkb_id);

        if(! ext || ! ext->present)
            throw xcb_error(NS_FuncName);

        auto xcbReply = getReplyFunc2(xcb_xkb_use_extension, conn.get(), XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xkb_use_extension");
            throw xcb_error(NS_FuncName);
        }

        if(auto reply = xcbReply.reply())
        {
            Application::debug("used %s extension, version: %d.%d", "XKB", reply->serverMajor, reply->serverMinor);
        }

        devid = xkb_x11_get_core_keyboard_device_id(conn.get());

        if(0 > devid)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_get_core_keyboard_device_id");
            throw xcb_error(NS_FuncName);
        }

        ctx.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));

        if(! ctx)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xkb_context_new");
            throw xcb_error(NS_FuncName);
        }

        if(! resetMapState())
            throw xcb_error(NS_FuncName);
    }

    bool XCB::ModuleXkb::resetMapState(void)
    {
        // free state first
        state.reset();
        map.reset();

        // set new
        map.reset(xkb_x11_keymap_new_from_device(ctx.get(), conn.get(), devid, XKB_KEYMAP_COMPILE_NO_FLAGS));

        if(map)
        {
            state.reset(xkb_x11_state_new_from_device(map.get(), conn.get(), devid));

            if(! state)
            {
                Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_state_new_from_device");
                return false;
            }

            Application::debug("%s: keyboard updated, device id: %d", __FUNCTION__, devid);
            return true;
        }

        Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_keymap_new_from_device");
        return false;
    }

    std::vector<std::string> XCB::ModuleXkb::getNames(void) const
    {
        std::vector<std::string> res;
        res.reserve(4);

        auto xcbReply = getReplyFunc2(xcb_xkb_get_names, conn.get(), XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_NAME_DETAIL_GROUP_NAMES | XCB_XKB_NAME_DETAIL_SYMBOLS);
        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xkb_get_names");
            return res;
        }

        if(auto reply = xcbReply.reply())
        {
            const void* buffer = xcb_xkb_get_names_value_list(reply.get());
            xcb_xkb_get_names_value_list_t list;
            xcb_xkb_get_names_value_list_unpack(buffer, reply->nTypes, reply->indicators, reply->virtualMods,
                                                reply->groupNames, reply->nKeys, reply->nKeyAliases, reply->nRadioGroups, reply->which, & list);
            int groups = xcb_xkb_get_names_value_list_groups_length(reply.get(), & list);

            // current symbol atom: list.symbolsName;

            for(int ii = 0; ii < groups; ++ii)
                res.emplace_back(Atom::getName(conn.get(), list.groups[ii]));
        }

        return res;
    }

    int XCB::ModuleXkb::getLayoutGroup(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_xkb_get_state, conn.get(), XCB_XKB_ID_USE_CORE_KBD);
        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xkb_get_names");
            return -1;
        }

        if(auto reply = xcbReply.reply())
            return reply->group;

        return -1;
    }

    bool XCB::ModuleXkb::switchLayoutGroup(int group) const
    {
        // next
        if(group < 0)
        {
            auto names = getNames();

            if(2 > names.size())
                return false;

            group = (getLayoutGroup() + 1) % names.size();
        }

        auto cookie = xcb_xkb_latch_lock_state_checked(conn.get(), XCB_XKB_ID_USE_CORE_KBD, 0, 0, 1, group, 0, 0, 0);

        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_xkb_latch_lock_state");
            return false;
        }

        return true;
    }

    // ModuleSelection
    XCB::ModuleSelection::ModuleSelection(ConnectionShared ptr, const xcb_screen_t & screen, xcb_atom_t atom)
        : ModuleExtension(ptr, Module::SELECTION), atombuf(atom)
    {
        const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const uint32_t values[] = { screen.white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE };

        win = xcb_generate_id(conn.get());
        auto cookie = xcb_create_window_checked(conn.get(), 0, win, screen.root, -1, -1, 1, 1, 0,
                                                XCB_WINDOW_CLASS_INPUT_OUTPUT, screen.root_visual, mask, values);
    
        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_create_window");
            throw xcb_error(NS_FuncName);
        }

	Application::info("%s: resource id: 0x%08x", __FUNCTION__, win);

        // set _wm_name
        const std::string name("LTSM_WINSEL");
        xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, win,
                                Atom::wmName, Atom::utf8String, 8, name.size(), name.data());
    }

    XCB::ModuleSelection::~ModuleSelection()
    {
        if(win != XCB_WINDOW_NONE)
            xcb_destroy_window(conn.get(), win);
    }

    bool XCB::ModuleSelection::clearAction(const xcb_selection_clear_event_t* ev)
    {
        if(ev && ev->owner == win)
        {
            const std::scoped_lock guard{ lock };
            buf.clear();

            if(Application::isDebugLevel(DebugLevel::Console) ||
               Application::isDebugLevel(DebugLevel::SyslogInfo))
                Application::debug("%s: %s", __FUNCTION__, "event");

            return true;
        }

        return false;
    }

    bool XCB::ModuleSelection::setBuffer(const uint8_t* ptr, size_t len, std::initializer_list<xcb_atom_t> atoms)
    {
        const std::scoped_lock guard{ lock };
        buf.assign(ptr, ptr + len);
            
        if(buf.empty())
            return false;

        for(auto atom: atoms)
        {
            // take owner
            auto xcbReply = getReplyFunc2(xcb_get_selection_owner, conn.get(), atom);

            if(auto err = xcbReply.error())
            {
                error(conn.get(), err.get(), __FUNCTION__, "xcb_get_selection_owner");
            }
            else
            if(auto reply = xcbReply.reply())
            {
                if(reply->owner != win)
                    xcb_set_selection_owner(conn.get(), win, atom, XCB_CURRENT_TIME);
            }
        }

        xcb_flush(conn.get());
        return true;
    }

    bool XCB::ModuleSelection::sendNotifyTargets(const xcb_selection_request_event_t & ev)
    {
        // send list: Atom::targets, Atom::text, Atom::textPlain, Atom::utf8String
        auto cookie = xcb_change_property_checked(conn.get(), XCB_PROP_MODE_REPLACE, ev.requestor, ev.property,
                      XCB_ATOM, 8 * sizeof(xcb_atom_t), 4, & Atom::targets);

        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_change_property");
            return false;
        }

        xcb_selection_notify_event_t notify =
        {
            .response_type = XCB_SELECTION_NOTIFY,
            .pad0 = 0,
            .sequence = 0,
            .time = ev.time,
            .requestor = ev.requestor,
            .selection = ev.selection,
            .target = ev.target,
            .property = ev.property
        };

        xcb_send_event(conn.get(), 0, ev.requestor, XCB_EVENT_MASK_NO_EVENT, (const char*) & notify);
        xcb_flush(conn.get());

        return true;
    }

    bool XCB::ModuleSelection::sendNotifySelData(const xcb_selection_request_event_t & ev)
    {
        const std::scoped_lock guard{ lock };

        if(buf.empty())
            return false;

        size_t length = buf.size();
        size_t maxreq = xcb_get_maximum_request_length(conn.get());
        auto chunk = std::min(maxreq, length);
        auto cookie = xcb_change_property_checked(conn.get(), XCB_PROP_MODE_REPLACE, ev.requestor, ev.property, ev.target, 8, chunk, buf.data());

        if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_change_property");
            return false;
        }

        xcb_selection_notify_event_t notify =
        {
            .response_type = XCB_SELECTION_NOTIFY,
            .pad0 = 0,
            .sequence = 0,
            .time = ev.time,
            .requestor = ev.requestor,
            .selection = ev.selection,
            .target = ev.target,
            .property = ev.property
        };

        xcb_send_event(conn.get(), 0, ev.requestor, XCB_EVENT_MASK_NO_EVENT, (const char*) & notify);
        xcb_flush(conn.get());

        if(Application::isDebugLevel(DebugLevel::Console) ||
           Application::isDebugLevel(DebugLevel::SyslogInfo))
        {
            auto prop = Atom::getName(conn.get(), ev.property);
            std::string log(buf.begin(), buf.end());
            Application::debug("%s: put selection, size: %d, content: `%s', to window: 0x%08x, property: `%s'",
                        __FUNCTION__, buf.size(), log.c_str(), ev.requestor, prop.c_str());
        }

        return true;
    }

    bool XCB::ModuleSelection::requestAction(const xcb_selection_request_event_t* ev)
    {
        if(! ev)
            return false;

        if(ev->selection == Atom::clipboard || ev->selection == Atom::primary)
        {
            if(ev->target == Atom::targets)
                return sendNotifyTargets(*ev);
            else
            if(ev->property != XCB_ATOM_NONE &&
                (ev->target == Atom::text || ev->target == Atom::textPlain || ev->target == Atom::utf8String))
                return sendNotifySelData(*ev);
        }

        // other variants: discard request
        xcb_selection_notify_event_t notify =
        {
            .response_type = XCB_SELECTION_NOTIFY,
            .pad0 = 0,
            .sequence = 0,
            .time = ev->time,
            .requestor = ev->requestor,
            .selection = ev->selection,
            .target = ev->target,
            .property = XCB_ATOM_NONE
        };

        xcb_send_event(conn.get(), 0, ev->requestor, XCB_EVENT_MASK_NO_EVENT, (const char*) & notify);
        xcb_flush(conn.get());

        if(Application::isDebugLevel(DebugLevel::Console) ||
           Application::isDebugLevel(DebugLevel::SyslogTrace))
        {
            auto sel = Atom::getName(conn.get(), ev->selection);
            auto tgt = Atom::getName(conn.get(), ev->target);
            auto prop = Atom::getName(conn.get(), ev->property);
            Application::error("%s: discard, selection atom(%d, `%s'), target atom(%d, `%s'), property atom(%d, `%s'), window: 0x%08x",
                                    __FUNCTION__, ev->selection, sel.c_str(), ev->target, tgt.c_str(), ev->property, prop.c_str(), ev->requestor);
        }

        return false;
    }

    bool XCB::ModuleSelection::fixesAction(const xcb_xfixes_selection_notify_event_t* ev)
    {
        if(! ev)
            return false;

        if(ev->owner != XCB_WINDOW_NONE && ev->owner != win)
        {
            xcb_delete_property(conn.get(), win, atombuf);
            xcb_convert_selection(conn.get(), win, Atom::primary, Atom::utf8String, atombuf, XCB_CURRENT_TIME);

            std::thread([xcb = conn.get()]()
            {
                std::this_thread::sleep_for(100ms);
                xcb_flush(xcb);
            }).detach();

            return true;
        }

        return false;
    }

    void XCB::ModuleSelection::notifyIncrStart(const xcb_selection_notify_event_t & ev)
    {
        // fixme: maybe need check time
        if(incr && incr->requestor != ev.requestor && incr->sequence > ev.sequence)
        {
            Application::warning("%s: %s, win: 0x%08x, seq: 0x%04x", __FUNCTION__, "incremental request skip", ev.requestor, ev.sequence);
            return;
        }

        auto xcbReply = getReplyFunc2(xcb_get_property, conn.get(), false, win, Atom::incr, XCB_ATOM_INTEGER, 0, 1);
        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_get_property");
            return;
        }

        int selsize = 0;
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_INTEGER)
            {
                auto name = Atom::getName(conn.get(), reply->type);
                Application::warning("%s: unknown type, atom(%d, `%s')", __FUNCTION__, reply->type, name.c_str());
            }
            else
            if(reply->format != 32)
            {
                Application::warning("%s: unknown format: %d", __FUNCTION__, reply->format);
            }
            else
            {
                if(auto res = static_cast<int32_t*>(xcb_get_property_value(reply.get())))
                    selsize = *res;
            }
        }

        if(0 < selsize)
        {
            Application::notice("%s: selection size: %d, requestor win: 0x%08x, timestamp: %d", __FUNCTION__, selsize, ev.requestor, ev.time);

            incr = std::make_unique<SelectionIncrMode>(ev.requestor, selsize, ev.sequence);

            const std::scoped_lock guard{ lock };
            buf.reserve(selsize);

            // start incremental
            xcb_delete_property_checked(conn.get(), win, Atom::incr);
        }
        else
        {
            Application::warning("%s: %s, win: 0x%08x, seq: 0x%04x", __FUNCTION__, "incremental request empty", ev.requestor, ev.sequence);
        }
    }

    bool XCB::ModuleSelection::notifyIncrContinue(xcb_atom_t type)
    {
        if(! incr)
            return false;

        // get data
        auto xcbReply = getReplyFunc2(xcb_get_property, conn.get(), false, win, atombuf, type, 0, ~0);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_get_property");
            return false;
        }

        if(auto reply = xcbReply.reply())
        {
            bool ret = false;
            auto ptrbuf = reinterpret_cast<const uint8_t*>(xcb_get_property_value(reply.get()));
            int length = xcb_get_property_value_length(reply.get());

            // incremental ended
            if(0 == length)
            {
                const std::scoped_lock guard{ lock };

                if(buf.size() == incr->size)
                {
                    ret = true;
                }
                else
                {
                    Application::warning("%s: failed, requestor size: %d, receive size: %d", __FUNCTION__, incr->size, buf.size());
                    // reset stream node
                    incr.reset();
                }
            }
            else
            if(0 < length)
            {
                const std::scoped_lock guard{ lock };
                // append mode
                buf.insert(buf.end(), ptrbuf, ptrbuf + length);
            }

            // continue
            xcb_delete_property_checked(conn.get(), win, atombuf);

            return ret;
        }

        // stream not ended
        return false;
    }

    bool XCB::ModuleSelection::notifyAction(const xcb_selection_notify_event_t* ev, xcb_atom_t buftype, bool syncPrimaryClipboard)
    {
        if(! ev)
            return false;

        if(ev->property == XCB_ATOM_NONE)
            return false;

        bool allowAtom = ev->selection == Atom::primary ||
            (ev->selection == Atom::clipboard && syncPrimaryClipboard);

        if(! allowAtom)
        {
            auto name = Atom::getName(conn.get(), ev->selection);
            Application::warning("%s: skip selection, atom(%d, `%s')", __FUNCTION__, ev->selection, name.c_str());
            return false;
        }

        if(XCB_ATOM_NONE == buftype)
            return false;

        Application::debug("%s: requestor: 0x%08x, selection atom(%d, `%s'), target atom(%d, `%s'), property atom(%d, `%s'), timestamp: %d",
                    __FUNCTION__, ev->requestor, ev->selection, Atom::getName(conn.get(), ev->selection).c_str(),
                    ev->target, Atom::getName(conn.get(), ev->target).c_str(), ev->property, Atom::getName(conn.get(), ev->property).c_str(), ev->time);

        // incr mode
        if(Atom::incr == buftype)
        {
            notifyIncrStart(*ev);
            return false;
        }

        if(buftype != Atom::utf8String && buftype != Atom::text && buftype != Atom::textPlain)
        {
            auto name = Atom::getName(conn.get(), buftype);
            Application::warning("%s: unsupported type, atom(%d, `%s')", __FUNCTION__, buftype, name.c_str());

            if(incr)
                incr.reset();

            return false;
        }

        // continue incremental
        if(incr)
            return notifyIncrContinue(buftype);

        // get data
        auto xcbReply = getReplyFunc2(xcb_get_property, conn.get(), false, win, atombuf, buftype, 0, ~0);

        if(auto err = xcbReply.error())
        {
            error(conn.get(), err.get(), __FUNCTION__, "xcb_get_property");
            return false;
        }

        if(auto reply = xcbReply.reply())
        {
            bool ret = false;
            auto ptrbuf = reinterpret_cast<const uint8_t*>(xcb_get_property_value(reply.get()));
            int length = xcb_get_property_value_length(reply.get());

            if(ptrbuf && 0 < length)
            {
                const std::scoped_lock guard{ lock };

                // check equal
                if(length == buf.size() && std::equal(buf.begin(), buf.end(), ptrbuf))
                    ret = false;
                else
                {
                    buf.assign(ptrbuf, ptrbuf + length);
                    ret = true;
                }
            }

            auto cookie = xcb_delete_property_checked(conn.get(), win, atombuf);

            if(auto err = GenericError(xcb_request_check(conn.get(), cookie)))
                error(conn.get(), err.get(), __FUNCTION__, "xcb_delete_property");

            return ret;
        }

        return false;
    }

#ifdef LTSM_BUILD_XCB_ERRORS
    XCB::ErrorContext::ErrorContext(xcb_connection_t* conn)
    {
        xcb_errors_context_new(conn, & ctx);
    }

    XCB::ErrorContext::~ErrorContext()
    {
        if(ctx)
            xcb_errors_context_free(ctx);
    }

    bool XCB::ErrorContext::error(const xcb_generic_error_t* err, const char* func, const char* xcbname) const
    {
        if(! ctx)
            return false;

        const char* extension = nullptr;
        const char* major = xcb_errors_get_name_for_major_code(ctx, err->major_code);
        const char* minor = xcb_errors_get_name_for_minor_code(ctx, err->major_code, err->minor_code);
        const char* error = xcb_errors_get_name_for_error(ctx, err->error_code, & extension);

        Application::error("%s: %s failed, error: %s, extension: %s, major: %s, minor: %s, resource: 0x%08x, sequence: 0x%04x",
                       func, xcbname, error, (extension ? extension : "none"), major, (minor ? minor : "none"),
                       (unsigned int) err->resource_id, (unsigned int) err->sequence);

        return true;
    }
#endif

    /* XCB::Connector */
    XCB::Connector::Connector(size_t displayNum, const AuthCookie* cookie)
    {
        displayConnect(displayNum, cookie);
    }

    void XCB::Connector::displayConnect(size_t displayNum, const AuthCookie* cookie)
    {
        auto displayAddr = std::string(":").append(std::to_string(displayNum));
        
        if(cookie) 
        {
            std::string_view magic{"MIT-MAGIC-COOKIE-1"};
            xcb_auth_info_t auth;
            auth.name = (char*) magic.data();
            auth.namelen = magic.size();
            auth.data = (char*) cookie->data();
            auth.datalen = cookie->size();

            _conn = ConnectionShared(xcb_connect_to_display_with_auth_info(displayAddr.c_str(), & auth, nullptr));
        }
        else
        {
            _conn = ConnectionShared(xcb_connect(displayAddr.c_str(), nullptr));
        }

        if(xcb_connection_has_error(_conn.get()))
        {
            Application::error("%s: %s failed, addr: %s", __FUNCTION__, "xcb_connect", displayAddr.c_str());
            throw xcb_error(NS_FuncName);
        }

        _setup = xcb_get_setup(_conn.get());

        if(! _setup)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xcb_get_setup");
            throw xcb_error(NS_FuncName);
        }

#ifdef LTSM_BUILD_XCB_ERRORS
        _error = std::make_unique<ErrorContext>(_conn.get());
#endif

        Atom::wmName = getAtom("_NET_WM_NAME");
        Atom::utf8String = getAtom("UTF8_STRING");

        Atom::primary = getAtom("PRIMARY");
        Atom::clipboard = getAtom("CLIPBOARD");
        Atom::targets = getAtom("TARGETS");
        Atom::text = getAtom("TEXT");
        Atom::textPlain = getAtom("text/plain;charset=utf-8");
        Atom::incr = getAtom("INCR");
    }

    xcb_connection_t* XCB::Connector::xcb_ptr(void)
    {
        return _conn.get();
    }

    const xcb_connection_t* XCB::Connector::xcb_ptr(void) const
    {
        return _conn.get();
    }

    size_t XCB::Connector::bppFromDepth(size_t depth) const
    {
        for(auto fIter = xcb_setup_pixmap_formats_iterator(_setup); fIter.rem; xcb_format_next(& fIter))
            if(fIter.data->depth == depth) return fIter.data->bits_per_pixel;

        return 0;
    }

    size_t XCB::Connector::depthFromBpp(size_t bitsPerPixel) const
    {
        for(auto fIter = xcb_setup_pixmap_formats_iterator(_setup); fIter.rem; xcb_format_next(& fIter))
            if(fIter.data->bits_per_pixel == bitsPerPixel) return fIter.data->depth;

        return 0;
    }

    const xcb_setup_t* XCB::Connector::setup(void) const
    {
        return _setup;
    }

    XCB::GenericError XCB::Connector::checkRequest(const xcb_void_cookie_t & cookie) const
    {
        return GenericError(xcb_request_check(_conn.get(), cookie));
    }

    size_t XCB::Connector::getMaxRequest(void) const
    {
        return xcb_get_maximum_request_length(_conn.get());
    }

    bool XCB::Connector::checkAtom(std::string_view name) const
    {
        return XCB_ATOM_NONE != getAtom(name, false);
    }

    xcb_atom_t XCB::Connector::getAtom(std::string_view name, bool create) const
    {
        auto xcbReply = getReplyFunc2(xcb_intern_atom, _conn.get(), create ? 0 : 1, name.size(), name.data());

        if(auto err = xcbReply.error())
        {
            extendedError(err.get(), __FUNCTION__, "xcb_intern_atom");
            return XCB_ATOM_NONE;
        }

        return xcbReply.reply() ? xcbReply.reply()->atom : XCB_ATOM_NONE;
    }

    std::string XCB::Connector::getAtomName(xcb_atom_t atom) const
    {
        return Atom::getName(_conn.get(), atom);
    }

    void XCB::Connector::extendedError(const xcb_generic_error_t* err, const char* func, const char* xcbname) const
    {
#ifdef LTSM_BUILD_XCB_ERRORS
        if(! _error || ! _error->error(err, func, xcbname))
#endif
        {
            XCB::error(err, func, xcbname);
        }
    }

    int XCB::Connector::hasError(void) const
    {
        return xcb_connection_has_error(_conn.get());
    }

    bool XCB::Connector::deleteProperty(xcb_window_t win, xcb_atom_t prop) const
    {
        auto cookie = xcb_delete_property_checked(_conn.get(), win, prop);

        if(auto err = checkRequest(cookie))
        {
            extendedError(err.get(), __FUNCTION__, "xcb_delete_property");
            return false;
        }

        return true;
    }

    std::list<xcb_atom_t> XCB::Connector::getPropertiesList(xcb_window_t win) const
    {
        auto xcbReply = getReplyFunc2(xcb_list_properties, _conn.get(), win);
        std::list<xcb_atom_t> res;

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_list_properties");
        else
        if(auto reply = xcbReply.reply())
        {
            auto beg = xcb_list_properties_atoms(reply.get());
            auto end = beg + xcb_list_properties_atoms_length(reply.get());

            if(beg && beg < end)
                res.assign(beg, end);
        }

        return res;
    }

    XCB::PropertyReply XCB::Connector::getPropertyInfo(xcb_window_t win, xcb_atom_t prop) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 0);

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");

        return xcbReply.reply();
    }

    xcb_atom_t XCB::Connector::getPropertyType(xcb_window_t win, xcb_atom_t prop) const
    {
        auto reply = getPropertyInfo(win, prop);
        return reply ? reply->type : XCB_ATOM_NONE;
    }

    xcb_atom_t XCB::Connector::getPropertyAtom(xcb_window_t win, xcb_atom_t prop, uint32_t offset) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_ATOM, offset, 1);

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_ATOM)
            {
                auto name1 = getAtomName(reply->type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "atom", reply->type, name1.c_str(), name2.c_str());
            }
            else
            if(auto res = static_cast<xcb_atom_t*>(xcb_get_property_value(reply.get())))
            {
                return *res;
            }
        }

        return XCB_ATOM_NONE;
    }

    std::list<xcb_atom_t> XCB::Connector::getPropertyAtomList(xcb_window_t win, xcb_atom_t prop) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_ATOM, 0, ~0);
        std::list<xcb_atom_t> res;

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_ATOM)
            {
                auto name1 = getAtomName(reply->type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "atom", reply->type, name1.c_str(), name2.c_str());
            }
            else
            {
                auto beg = static_cast<xcb_atom_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(xcb_atom_t);

                if(beg && beg < end)
                    res.assign(beg, end);
            }
        }

        return res;
    }

    xcb_window_t XCB::Connector::getPropertyWindow(xcb_window_t win, xcb_atom_t prop, uint32_t offset) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_WINDOW, offset, 1);

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_WINDOW)
            {
                auto name1 = getAtomName(reply->type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "window", reply->type, name1.c_str(), name2.c_str());
            }
            else
            if(auto res = static_cast<xcb_window_t*>(xcb_get_property_value(reply.get())))
                return *res;
        }

        return XCB_WINDOW_NONE;
    }

    std::list<xcb_window_t> XCB::Connector::getPropertyWindowList(xcb_window_t win, xcb_atom_t prop) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_WINDOW, 0, ~0);
        std::list<xcb_window_t> res;

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_WINDOW)
            {
                auto name1 = getAtomName(reply->type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "window", reply->type, name1.c_str(), name2.c_str());
            }
            else
            {
                auto beg = static_cast<xcb_window_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(xcb_window_t);

                if(beg && beg < end)
                    res.assign(beg, end);
            }
        }

        return res;
    }

    uint32_t XCB::Connector::getPropertyCardinal(xcb_window_t win, xcb_atom_t prop, uint32_t offset) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_CARDINAL, offset, 1);

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_CARDINAL)
            {
                auto name1 = getAtomName(reply->type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "cardinal", reply->type, name1.c_str(), name2.c_str());
            }
            else
            if(reply->format == 32)
            {
                if(auto res = static_cast<uint32_t*>(xcb_get_property_value(reply.get())))
                    return *res;
            }
            else
            {
                auto name2 = getAtomName(prop);
                Application::warning("%s: unknown format: %d, property: %s", __FUNCTION__, reply->format, name2.c_str());
            }
        }

        return 0;
    }

    std::list<uint32_t> XCB::Connector::getPropertyCardinalList(xcb_window_t win, xcb_atom_t prop) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_CARDINAL, 0, ~0);
        std::list<uint32_t> res;

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_CARDINAL)
            {
                auto name1 = getAtomName(reply->type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "cardinal", reply->type, name1.c_str(), name2.c_str());
            }
            else
            if(reply->format == 32)
            {
                auto beg = static_cast<uint32_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(uint32_t);

                if(beg && beg < end)
                    res.assign(beg, end);
            }
            else
            {
                auto name2 = getAtomName(prop);
                Application::warning("%s: unknown format: %d, property: %s", __FUNCTION__, reply->format, name2.c_str());
            }
        }

        return res;
    }

    int XCB::Connector::getPropertyInteger(xcb_window_t win, xcb_atom_t prop, uint32_t offset) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_INTEGER, offset, 1);

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_INTEGER)
            {
                auto name1 = getAtomName(reply->type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "integer", reply->type, name1.c_str(), name2.c_str());
            }
            else
            if(reply->format == 8)
            {
                if(auto res = static_cast<int8_t*>(xcb_get_property_value(reply.get())))
                    return *res;
            }
            else
            if(reply->format == 16)
            {
                if(auto res = static_cast<int16_t*>(xcb_get_property_value(reply.get())))
                    return *res;
            }
            else
            if(reply->format == 32)
            {
                if(auto res = static_cast<int32_t*>(xcb_get_property_value(reply.get())))
                    return *res;
            }
            else
            if(reply->format == 64)
            {
                if(auto res = static_cast<int64_t*>(xcb_get_property_value(reply.get())))
                    return *res;
            }
            else
            {
                auto name2 = getAtomName(prop);
                Application::warning("%s: unknown format: %d, property: %s", __FUNCTION__, reply->format, name2.c_str());
            }
        }

        return 0;
    }

    std::list<int> XCB::Connector::getPropertyIntegerList(xcb_window_t win, xcb_atom_t prop) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, XCB_ATOM_INTEGER, 0, ~0);
        std::list<int> res;

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            if(reply->type != XCB_ATOM_INTEGER)
            {
                auto name1 = getAtomName(reply->type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "integer", reply->type, name1.c_str(), name2.c_str());
            }
            else
            if(reply->format == 8)
            {
                auto beg = static_cast<int8_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(int8_t);

                if(beg && beg < end)
                    res.assign(beg, end);
            }
            else
            if(reply->format == 16)
            {
                auto beg = static_cast<int16_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(int16_t);

                if(beg && beg < end)
                    res.assign(beg, end);
            }
            else
            if(reply->format == 32)
            {
                auto beg = static_cast<int32_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(int32_t);

                if(beg && beg < end)
                    res.assign(beg, end);
            }
            else
            if(reply->format == 64)
            {
                auto beg = static_cast<int64_t*>(xcb_get_property_value(reply.get()));
                auto end = beg + xcb_get_property_value_length(reply.get()) / sizeof(int64_t);

                if(beg && beg < end)
                    res.assign(beg, end);
            }
            else
            {
                auto name2 = getAtomName(prop);
                Application::warning("%s: unknown format: %d, property: %s", __FUNCTION__, reply->format, name2.c_str());
            }
        }

        return res;
    }

    std::string XCB::Connector::getPropertyString(xcb_window_t win, xcb_atom_t prop) const
    {
        auto type = getPropertyType(win, prop);

        if(type != XCB_ATOM_STRING)
        {
            if(type != Atom::utf8String)
            {
                auto name1 = getAtomName(type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "string", type, name1.c_str(), name2.c_str());
                return "";
            }

            type = Atom::utf8String;
        }

        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, type, 0, ~0);

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            auto ptr = static_cast<const char*>(xcb_get_property_value(reply.get()));
            auto len = xcb_get_property_value_length(reply.get());

            if(ptr && 0 < len)
                return std::string(ptr, ptr + len);
        }

        return "";
    }

    std::list<std::string> XCB::Connector::getPropertyStringList(xcb_window_t win, xcb_atom_t prop) const
    {
        std::list<std::string> res;
        auto type = getPropertyType(win, prop);

        if(type != XCB_ATOM_STRING)
        {
            if(type != Atom::utf8String)
            {
                auto name1 = getAtomName(type);
                auto name2 = getAtomName(prop);
                Application::warning("%s: type not %s, type atom(%d, `%s'), property: %s", __FUNCTION__, "string", type, name1.c_str(), name2.c_str());
                return res;
            }

            type = Atom::utf8String;
        }

        auto xcbReply = getReplyFunc2(xcb_get_property, _conn.get(), false, win, prop, type, 0, ~0);

        if(auto err = xcbReply.error())
            extendedError(err.get(), __FUNCTION__, "xcb_get_property");
        else
        if(auto reply = xcbReply.reply())
        {
            auto beg = static_cast<const char*>(xcb_get_property_value(reply.get()));
            auto end = beg + xcb_get_property_value_length(reply.get());

            while(beg && beg < end)
            {
                res.emplace_back(beg);
                beg += res.back().size() + 1;
            }
        }

        return res;
    }

    /* XCB::RootDisplay */
    XCB::RootDisplay::RootDisplay(size_t displayNum, const AuthCookie* auth) : Connector(displayNum, auth)
    {
        rootConnect(displayNum, auth);
    }

    void XCB::RootDisplay::rootConnect(size_t displayNum, const AuthCookie* auth)
    {
        _minKeycode = _setup->min_keycode;
        _maxKeycode = _setup->max_keycode;

        _screen = xcb_setup_roots_iterator(_setup).data;

        if(! _screen)
        {
            Application::error("%s: %s failed", __FUNCTION__, "root screen");
            throw xcb_error(NS_FuncName);
        }

        // init format
        for(auto fIter = xcb_setup_pixmap_formats_iterator(_setup); fIter.rem; xcb_format_next(& fIter))
        {
            if(fIter.data->depth == _screen->root_depth)
            {
                _format = fIter.data;
                break;
            }
        }

        if(! _format)
        {
            Application::error("%s: %s failed", __FUNCTION__, "init format");
            throw xcb_error(NS_FuncName);
        }

        // init visual
        for(auto dIter = xcb_screen_allowed_depths_iterator(_screen); dIter.rem; xcb_depth_next(& dIter))
        {
            for(auto vIter = xcb_depth_visuals_iterator(dIter.data); vIter.rem; xcb_visualtype_next(& vIter))
            {
                if(_screen->root_visual == vIter.data->visual_id)
                {
                    _visual = vIter.data;
                    break;
                }
            }
        }

        if(! _visual)
        {
            Application::error("%s: %s failed", __FUNCTION__, "init visual");
            throw xcb_error(NS_FuncName);
        }

        _modShm = std::make_unique<ModuleShm>(_conn);
        _modDamage = std::make_unique<ModuleDamage>(_conn);
        _modFixes = std::make_unique<ModuleFixes>(_conn);
        _modTest = std::make_unique<ModuleTest>(_conn);
        _modRandr = std::make_unique<ModuleRandr>(_conn);
        _modXkb = std::make_unique<ModuleXkb>(_conn);
        _modSelection = std::make_unique<ModuleSelection>(_conn, *_screen, getAtom("XSEL_DATA"));

        // notify xfixes filter
        xcb_xfixes_select_cursor_input(_conn.get(), _screen->root, XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
        xcb_xfixes_select_selection_input(_conn.get(), _screen->root, Atom::primary, XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER);

        // notify xkb filter
        uint16_t required_map_parts = (XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS | XCB_XKB_MAP_PART_MODIFIER_MAP |
                                       XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS | XCB_XKB_MAP_PART_KEY_ACTIONS | XCB_XKB_MAP_PART_VIRTUAL_MODS | XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP);
        uint16_t required_events = (XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_STATE_NOTIFY);

        auto cookie = xcb_xkb_select_events_checked(_conn.get(), _modXkb->devid, required_events, 0, required_events, required_map_parts, required_map_parts, nullptr);

        if(auto err = checkRequest(cookie))
            extendedError(err.get(), __FUNCTION__, "xcb_xkb_select_events");

        // create randr notify
        xcb_randr_select_input(_conn.get(), _screen->root,
                               XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);

        // create damage notify
        createFullScreenDamage();

        xcb_flush(_conn.get());
    }

    void XCB::RootDisplay::reconnect(size_t displayNum, const AuthCookie* auth)
    {
        _damage.reset();
        _modShm.reset();
        _modDamage.reset();
        _modFixes.reset();
        _modTest.reset();
        _modRandr.reset();
        _modXkb.reset();
        _modSelection.reset();

        _screen = nullptr;
        _format = nullptr;
        _visual = nullptr;

        displayConnect(displayNum, auth);
        rootConnect(displayNum, auth);
    }

    XCB::RootDisplay::~RootDisplay()
    {
        if(_conn)
        {
            resetInputs();
            xcb_flush(_conn.get());
        }
    }

    bool XCB::RootDisplay::createFullScreenDamage(void)
    {
        if(_modDamage && _modFixes)
        {
            _damage = _modDamage->createDamage(_screen->root, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);

            if(auto regid = _modFixes->createRegion(region().toXcb()))
            {
                _damage->addRegion(regid->id());
                return true;
            }
        }

        return false;
    }

    const XCB::ModuleExtension* XCB::RootDisplay::getExtension(const Module & mod) const
    {
        switch(mod)
        {
            case Module::SHM:       return _modShm.get();
            case Module::DAMAGE:    return _modDamage.get();
            case Module::XFIXES:    return _modFixes.get();
            case Module::RANDR:     return _modRandr.get();
            case Module::TEST:      return _modTest.get();
            case Module::XKB:       return _modXkb.get();
            case Module::SELECTION: return _modSelection.get();
        }

        return nullptr;
    }

    bool XCB::RootDisplay::isDamageNotify(const GenericEvent & ev) const
    {
        return _modDamage ? _modDamage->isEventType(ev, XCB_DAMAGE_NOTIFY) : false;
    }

    bool XCB::RootDisplay::isXFixesSelectionNotify(const GenericEvent & ev) const
    {
        return _modFixes ? _modFixes->isEventType(ev, XCB_XFIXES_SELECTION_NOTIFY) : false;
    }

    bool XCB::RootDisplay::isXFixesCursorNotify(const GenericEvent & ev) const
    {
        return _modFixes ? _modFixes->isEventType(ev, XCB_XFIXES_CURSOR_NOTIFY) : false;
    }

    bool XCB::RootDisplay::isRandrScreenNotify(const GenericEvent & ev) const
    {
        // for receive it, usage input filter: xcb_xrandr_select_input(xcb_randr_notify_mask_t)
        return _modRandr ? _modRandr->isEventType(ev, XCB_RANDR_SCREEN_CHANGE_NOTIFY) : false;
    }

    bool XCB::RootDisplay::isRandrNotify(const GenericEvent & ev, const xcb_randr_notify_t & notify) const
    {
        if(_modRandr && _modRandr->isEventType(ev, XCB_RANDR_NOTIFY))
        {
            auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
            return rn->subCode == notify;
        }

        return false;
    }

    bool XCB::RootDisplay::isXkbNotify(const GenericEvent & ev, int notify) const
    {
        if(_modXkb && _modXkb->isEventType(ev, 0))
        {
            auto xn = reinterpret_cast<xkb_notify_event_t*>(ev.get());
            return xn->any.xkb_type == notify;
        }

        return false;
    }

    bool XCB::RootDisplay::setRandrScreenSize(const XCB::Size & sz, uint16_t* sequence)
    {
        if(_modRandr)
        {
            auto area = region();

            // clear all damages
            damageSubtrack(area);

            auto res = _modRandr->setScreenSize(*_screen, sz, sequence);
            if(! res)
                damageAdd(area);

            // so, new damage after rand notify
            return res;
        }

        return 0;
    }

    void XCB::RootDisplay::resetInputs(void)
    {
        if(_modTest)
        {
            // release all buttons
            for(uint8_t button = 1; button <= 5; button++)
                xcb_test_fake_input(_conn.get(), XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, _screen->root, 0, 0, 0);

            // release all keys
            for(int key = _minKeycode;  key <= _maxKeycode; key++)
                xcb_test_fake_input(_conn.get(), XCB_KEY_RELEASE, key, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);

            xcb_flush(_conn.get());
        }
    }

    size_t XCB::RootDisplay::bitsPerPixel(void) const
    {
        return _format ? _format->bits_per_pixel : 0;
    }

    size_t XCB::RootDisplay::scanlinePad(void) const
    {
        return _format ? _format->scanline_pad : 0;
    }

    const xcb_visualtype_t* XCB::RootDisplay::visual(xcb_visualid_t id) const
    {
        for(auto dIter = xcb_screen_allowed_depths_iterator(_screen); dIter.rem; xcb_depth_next(& dIter))
        {
            for(auto vIter = xcb_depth_visuals_iterator(dIter.data); vIter.rem; xcb_visualtype_next(& vIter))
            {
                if(id == vIter.data->visual_id)
                    return vIter.data;
            }
        }

        return nullptr;
    }

    const xcb_visualtype_t* XCB::RootDisplay::visual(void) const
    {
        return _visual;
    }

    size_t XCB::RootDisplay::depth(void) const
    {
        return _screen->root_depth;
    }

    XCB::Region XCB::RootDisplay::region(void) const
    {
        return Region(Point(0, 0), size());
    }

    XCB::Size XCB::RootDisplay::size(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_geometry, _conn.get(), _screen->root);

        if(auto reply = xcbReply.reply())
            return Size(reply->width, reply->height);

        return Size(_screen->width_in_pixels, _screen->height_in_pixels);
    }

    uint16_t XCB::RootDisplay::width(void) const
    {
        return size().width;
    }

    uint16_t XCB::RootDisplay::height(void) const
    {
        return size().height;
    }

    xcb_drawable_t XCB::RootDisplay::root(void) const
    {
        return _screen->root;
    }

    XCB::PixmapInfoReply XCB::RootDisplay::copyRootImageRegion(const Region & reg, ShmIdShared shm) const
    {
        Application::debug("%s: region: [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);
        const uint32_t planeMask = 0xFFFFFFFF;

        if(shm && 0 < shm->id)
        {
            auto xcbReply = getReplyFunc1(xcb_shm_get_image, _conn.get(), _screen->root, reg.x, reg.y, reg.width, reg.height,
                                      planeMask, XCB_IMAGE_FORMAT_Z_PIXMAP, shm->id, 0);
            PixmapInfoReply res;

            if(auto reply = xcbReply.reply())
            {
                auto visptr = visual(reply->visual);
                auto bpp = bppFromDepth(reply->depth);

                if(visptr)
                    return std::make_shared<PixmapSHM>(visptr->red_mask, visptr->green_mask, visptr->blue_mask, bpp, std::move(shm), reply->size);
            }

            if(auto err = xcbReply.error())
                extendedError(err.get(), __FUNCTION__, "xcb_shm_get_image");

            shm->reset();
        }

        size_t pitch = reg.width * (bitsPerPixel() >> 3);
        PixmapInfoReply res;

        if(pitch == 0)
        {
            Application::error("%s: copy root image error, empty size: [%d,%d], bpp: %d", __FUNCTION__, reg.width, reg.height, bitsPerPixel());
            return res;
        }

        auto maxReqLength = xcb_get_maximum_request_length(_conn.get());
        uint16_t allowRows = std::min(static_cast<uint16_t>(maxReqLength / pitch), reg.height);

        Application::debug("%s: max request size: %d, allow rows: %d", __FUNCTION__, maxReqLength, allowRows);

        for(int16_t yy = reg.y; yy < reg.y + reg.height; yy += allowRows)
        {
            // last rows
            if(yy + allowRows > reg.y + reg.height)
                allowRows = reg.y + reg.height - yy;

            auto xcbReply = getReplyFunc2(xcb_get_image, _conn.get(), XCB_IMAGE_FORMAT_Z_PIXMAP, _screen->root, reg.x, yy, reg.width, allowRows, planeMask);

            if(auto err = xcbReply.error())
            {
                extendedError(err.get(), __FUNCTION__, "xcb_get_image");
                break;
            }

            if(auto reply = xcbReply.reply())
            {
                if(! res)
                {
                    auto visptr = visual(reply->visual);
                    auto bitsPerPixel = bppFromDepth(reply->depth);

                    if(! visptr)
                    {
                        Application::error("%s: unknown visual id: %d", __FUNCTION__, reply->visual);
                        break;
                    }

                    res.reset(new PixmapBuffer(visptr->red_mask, visptr->green_mask, visptr->blue_mask, bitsPerPixel, reg.height * pitch));
                }

                auto info = static_cast<PixmapBuffer*>(res.get());

                auto data = xcb_get_image_data(reply.get());
                auto length = xcb_get_image_data_length(reply.get());
                Application::debug("%s: receive length: %d", __FUNCTION__, length);

                info->pixels.insert(info->pixels.end(), data, data + length);
            }
        }

        return res;
    }

    bool XCB::RootDisplay::damageAdd(const xcb_rectangle_t* rects, size_t counts)
    {
        if(_damage && _modFixes && rects && counts)
        {
            if(auto regid = _modFixes->createRegions(rects, counts))
	    {
                _damage->addRegion(regid->id());
                return true;
	    }
        }

        return false;
    }

    bool XCB::RootDisplay::damageAdd(const Region & region)
    {
        if(_damage && _modFixes)
        {
            if(auto regid = _modFixes->createRegion(region.toXcb()))
            {
                _damage->addRegion(regid->id());
                return true;
            }
        }

        return false;
    }

    bool XCB::RootDisplay::damageSubtrack(const Region & region)
    {
        if(_damage && _modFixes)
        {
            if(auto regid = _modFixes->createRegion(region.toXcb()))
            {
                _damage->subtrackRegion(regid->id());
                return true;
            }
        }

        return false;
    }

    bool XCB::RootDisplay::setClipboard(const uint8_t* buf, size_t len)
    {
        return _modSelection && _modSelection->setBuffer(buf, len, { Atom::primary, Atom::clipboard });
    }

    XCB::GenericEvent XCB::RootDisplay::poolEvent(void)
    {
        auto ev = GenericEvent(xcb_poll_for_event(_conn.get()));

        if(isDamageNotify(ev))
        {
            auto wsz = size();
            auto dn = reinterpret_cast<xcb_damage_notify_event_t*>(ev.get());

            if(dn->area.x + dn->area.width > wsz.width || dn->area.y + dn->area.height > wsz.height)
            {
                Application::warning("%s: damage discard, region: [%d,%d,%d,%d], level: 0x%02x, sequence: 0x%04x, timestamp: %d",
                                        __FUNCTION__, dn->area.x, dn->area.y, dn->area.width, dn->area.height, dn->level, dn->sequence, dn->timestamp);
                xcb_discard_reply(_conn.get(), dn->sequence);
                return GenericEvent();
            }

    	    Application::debug("%s: damage notify, region: [%d,%d,%d,%d], level: 0x%02x, sequence: 0x%04x, timestamp: %d",
                                        __FUNCTION__, dn->area.x, dn->area.y, dn->area.width, dn->area.height, dn->level, dn->sequence, dn->timestamp);
            damageRegionEvent(dn->area);
        }
        else
        if(isXFixesSelectionNotify(ev))
        {
	    auto sn = reinterpret_cast<xcb_xfixes_selection_notify_event_t*>(ev.get());

	    Application::debug("%s: selection notify, window: 0x%08x, owner: 0x%08x, selection atom(%d, `%s'), time1: %d, time2: %d",
        		    __FUNCTION__, sn->window, sn->owner, sn->selection, getAtomName(sn->selection).c_str(), sn->timestamp, sn->selection_timestamp);

            if(_modSelection)
                _modSelection->fixesAction(sn);

            xfixesSelectionChangedEvent();
        }
        else
        if(isXFixesCursorNotify(ev))
        {
	    auto cn = reinterpret_cast<xcb_xfixes_cursor_notify_event_t*>(ev.get());

    	    Application::debug("%s: cursor notify, serial: 0x%08x, name: atom(%d, `%s'), sequence: 0x%04x, timestamp: %d",
                            __FUNCTION__, cn->cursor_serial, cn->name, getAtomName(cn->name).c_str(), cn->sequence, cn->timestamp);

            xfixesCursorChangedEvent();
        }
        else
        if(isRandrNotify(ev, XCB_RANDR_NOTIFY_CRTC_CHANGE))
        {
            auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
            xcb_randr_crtc_change_t cc = rn->u.cc;

            if(0 < cc.width && 0 < cc.height)
            {
                auto wsz = size();

                if(cc.width != wsz.width || cc.height != wsz.height)
                {
                    Application::warning("%s: crtc change discard, size: [%d,%d], current: [%d,%d], sequence: 0x%04x, timestamp: %d",
                                            __FUNCTION__, cc.width, cc.height, wsz.width, wsz.height, rn->sequence, cc.timestamp);
                    xcb_discard_reply(_conn.get(), rn->sequence);
                    return GenericEvent();
                }

                Application::debug("%s: crtc change notify, size: [%d,%d], crtc: %d, mode: %d, rotation: 0x%04x, sequence: 0x%04x, timestamp: %d",
				    __FUNCTION__, cc.width, cc.height, cc.crtc, cc.mode, cc.rotation, rn->sequence, cc.timestamp);

                createFullScreenDamage();
                randrScreenChangedEvent(XCB::Size(cc.width, cc.height), *rn);
            }
        }
        else
        if(isXkbNotify(ev, XCB_XKB_MAP_NOTIFY))
        {
            auto mn = reinterpret_cast<xcb_xkb_map_notify_event_t*>(ev.get());
            Application::debug("%s: xkb notify: %s, min keycode: %d, max keycode: %d, changed: 0x%04x, sequence: 0x%04x, timestamp: %d",
			__FUNCTION__, "map", mn->minKeyCode, mn->maxKeyCode, mn->changed, mn->sequence, mn->time);

            _minKeycode = mn->minKeyCode;
            _maxKeycode = mn->maxKeyCode;

            _modXkb->resetMapState();
        }
        else
        if(isXkbNotify(ev, XCB_XKB_NEW_KEYBOARD_NOTIFY))
        {
            auto kn = reinterpret_cast<xcb_xkb_new_keyboard_notify_event_t*>(ev.get());
            Application::debug("%s: xkb notify: %s, devid: %d, old devid: %d, min keycode: %d, max keycode: %d, changed: 0x%04x, sequence: 0x%04x, timestamp: %d",
			__FUNCTION__, "new keyboard", kn->deviceID, kn->oldDeviceID, kn->minKeyCode, kn->maxKeyCode, kn->changed, kn->sequence, kn->time);

            if(kn->deviceID == _modXkb->devid && (kn->changed & XCB_XKB_NKN_DETAIL_KEYCODES))
            {
                _minKeycode = kn->minKeyCode;
                _maxKeycode = kn->maxKeyCode;

                Application::info("%s: reset map, devid: %d", __FUNCTION__, kn->deviceID);
                _modXkb->resetMapState();
            }
        }
        else
        if(isXkbNotify(ev, XCB_XKB_STATE_NOTIFY))
        {
            auto sn = reinterpret_cast<xcb_xkb_state_notify_event_t*>(ev.get());
            Application::debug("%s: xkb notify: %s, xkb type: 0x%02x, devid: %d, mods: 0x%02x, group: 0x%02x, changed: 0x%04x, sequence: 0x%04x, timestamp: %d",
			__FUNCTION__, "state", sn->xkbType, sn->deviceID, sn->mods, sn->group, sn->changed, sn->sequence, sn->time);

            xkb_state_update_mask(_modXkb->state.get(), sn->baseMods, sn->latchedMods, sn->lockedMods,
                                  sn->baseGroup, sn->latchedGroup, sn->lockedGroup);

            if(sn->changed & XCB_XKB_STATE_PART_GROUP_STATE)
            {
                // changed layout group
                xkbGroupChangedEvent(sn->group);
            }
        }

        auto response_type = ev ? ev->response_type & ~0x80 : 0;

        if(_modSelection)
        {
            switch(response_type)
            {
                case XCB_SELECTION_CLEAR:
                    _modSelection->clearAction(reinterpret_cast<xcb_selection_clear_event_t*>(ev.get()));
                    break;

                case XCB_SELECTION_REQUEST:
                    _modSelection->requestAction(reinterpret_cast<xcb_selection_request_event_t*>(ev.get()));
                    break;

                case XCB_SELECTION_NOTIFY:
                    if(_modSelection->notifyAction(reinterpret_cast<xcb_selection_notify_event_t*>(ev.get()), getPropertyType(_modSelection->win, _modSelection->atombuf), false))
                    {
                        const std::scoped_lock guard{ _modSelection->lock };
                        clipboardChangedEvent(_modSelection->buf);
                    }
                    break;

                default:
                    break;
            }
        }

        return ev;
    }

    xcb_keycode_t XCB::RootDisplay::keysymToKeycode(xcb_keysym_t keysym) const
    {
        return keysymGroupToKeycode(keysym, _modXkb ? _modXkb->getLayoutGroup() : 0);
    }

    std::pair<xcb_keycode_t, int> XCB::RootDisplay::keysymToKeycodeGroup(xcb_keysym_t keysym) const
    {
        auto empty = std::make_pair<xcb_keycode_t, int>(NULL_KEYCODE, -1);
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, _conn.get(), _minKeycode, _maxKeycode - _minKeycode + 1);

        if(auto err = xcbReply.error())
        {
            extendedError(err.get(), __FUNCTION__, "xcb_get_keyboard_mapping");
            return empty;
        }

        auto reply = xcbReply.reply();

        if(! reply)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xcb_get_keyboard_mapping");
            return empty;
        }

        const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply.get());

        if(! keysyms)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xcb_get_keyboard_mapping_keysyms");
            return empty;
        }

        int keysymsPerKeycode = reply->keysyms_per_keycode;

        if(1 > keysymsPerKeycode)
        {
            Application::error("%s: %s failed", __FUNCTION__, "keysyms_per_keycode");
            return empty;
        }

        int keysymsLength = xcb_get_keyboard_mapping_keysyms_length(reply.get());
        int keycodesCount = keysymsLength / keysymsPerKeycode;
        Application::debug("%s: keysym: 0x%08x, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d",
                           __FUNCTION__, keysym, keysymsPerKeycode, keysymsLength, keycodesCount);
        // shifted/unshifted
        int groupsCount = keysymsPerKeycode >> 1;

        for(int group = 0; group < groupsCount; ++group)
        {
            for(int ii = 0; ii < keycodesCount; ++ii)
            {
                auto keycode = _minKeycode + ii;
                int index = ii * keysymsPerKeycode + group * 2;

                if(index + 1 >= keysymsLength)
                {
                    Application::error("%s: index out of range %d, current group: %d, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d",
                                       __FUNCTION__, index, group, keysymsPerKeycode, keysymsLength, keycodesCount);
                    return empty;
                }

                // check normal/shifted keysyms
                if(keysym == keysyms[index] || keysym == keysyms[index + 1])
                    return std::make_pair(keycode, group);
            }
        }

        if(_modXkb)
        {
            Application::warning("%s: keysym not found 0x%08x, group names: [%s]", __FUNCTION__, keysym, Tools::join(_modXkb->getNames(), ",").c_str());
        }

        return empty;
    }

    xcb_keycode_t XCB::RootDisplay::keysymGroupToKeycode(xcb_keysym_t keysym, int group) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, _conn.get(), _minKeycode, _maxKeycode - _minKeycode + 1);

        if(auto err = xcbReply.error())
        {
            extendedError(err.get(), __FUNCTION__, "xcb_get_keyboard_mapping");
            return NULL_KEYCODE;
        }

        auto reply = xcbReply.reply();

        if(! reply)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xcb_get_keyboard_mapping");
            return NULL_KEYCODE;
        }

        const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply.get());

        if(! keysyms)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xcb_get_keyboard_mapping_keysyms");
            return NULL_KEYCODE;
        }

        int keysymsPerKeycode = reply->keysyms_per_keycode;

        if(1 > keysymsPerKeycode)
        {
            Application::error("%s: %s failed", __FUNCTION__, "keysyms_per_keycode");
            return NULL_KEYCODE;
        }

        // [shifted, unshifted] pairs
        int groupsCount = keysymsPerKeycode >> 1;

        if(0 > group || groupsCount <= group)
        {
            Application::error("%s: unknown group: %d, groups count: %d", __FUNCTION__, group, groupsCount);
            return NULL_KEYCODE;
        }

        int keysymsLength = xcb_get_keyboard_mapping_keysyms_length(reply.get());
        int keycodesCount = keysymsLength / keysymsPerKeycode;

        Application::debug("%s: keysym: 0x%08x, current group: %d, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d",
                           __FUNCTION__, keysym, group, keysymsPerKeycode, keysymsLength, keycodesCount);

        for(int ii = 0; ii < keycodesCount; ++ii)
        {
            auto keycode = _minKeycode + ii;
            int index = ii * keysymsPerKeycode + group * 2;

            if(index + 1 >= keysymsLength)
            {
                Application::error("%s: index out of range %d, current group: %d, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d",
                                   __FUNCTION__, index, group, keysymsPerKeycode, keysymsLength, keycodesCount);
                return NULL_KEYCODE;
            }

            // check normal keysyms
            if(keysym == keysyms[index])
                return keycode;

            // check shifted keysyms
            if(keysym == keysyms[index + 1])
                return keycode;
        }

        Application::warning("%s: keysym not found 0x%08x, curent group: %d", __FUNCTION__, keysym, group);
        return NULL_KEYCODE;
    }

    void XCB::RootDisplay::fakeInputKeycode(xcb_keycode_t keycode, bool pressed) const
    {
        if(_modTest)
            _modTest->fakeInputRaw(_screen->root, pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, 0, 0);
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
                    Application::debug("%s: keysym 0x%08x was found the another group %d, switched it", __FUNCTION__, keysym, keysymGroup);

                if(_modXkb)
                    _modXkb->switchLayoutGroup(keysymGroup);

                keycode = keysymKeycode;
            }
        }

        if(keycode != NULL_KEYCODE)
            fakeInputKeycode(keycode, pressed);
    }

    void XCB::RootDisplay::fakeInputButton(int button, const Point & pos) const
    {
        if(_modTest)
            _modTest->fakeInputClickButton(_screen->root, button, pos);
    }

    void XCB::RootDisplay::fillBackground(int r, int g, int b)
    {
        fillRegion(r, g, b, region());
    }

    void XCB::RootDisplay::fillRegion(int r, int g, int b, const Region & reg)
    {
        uint32_t color = (r << 16) | (g << 8) | b;

        if(depth() < 24 && color > 0x0000FFFF)
        {
            // convert RGB888 to RGB565
            r = ((color >> 24) & 0xFF) * 0x1F / 0xFF;
            g = ((color >> 16) & 0xFF) * 0x3F / 0xFF;
            b = (color & 0xFF) * 0x1F / 0xFF;
            color = (r << 11) | (g << 5) | b;
        }

        uint32_t colors[]  = { color };
        auto cookie = xcb_change_window_attributes(_conn.get(), _screen->root, XCB_CW_BACK_PIXEL, colors);

        if(auto err = checkRequest(cookie))
            extendedError(err.get(), __FUNCTION__, "xcb_change_window_attributes");
        else
        {
            cookie = xcb_clear_area_checked(_conn.get(), 0, _screen->root, reg.x, reg.y, reg.width, reg.height);

            if(auto err = checkRequest(cookie))
                extendedError(err.get(), __FUNCTION__, "xcb_clear_area");
        }
    }

    /// XkbClient
    XCB::XkbClient::XkbClient()
    {
        conn.reset(xcb_connect(nullptr, nullptr));
        if(xcb_connection_has_error(conn.get()))
            throw xcb_error("xcb_connect");

        auto setup = xcb_get_setup(conn.get());
        if(! setup)
            throw xcb_error("xcb_get_setup");

        minKeycode = setup->min_keycode;
        maxKeycode = setup->max_keycode;

        xkbext = xcb_get_extension_data(conn.get(), &xcb_xkb_id);
        if(! xkbext)
            throw xcb_error("xkb_get_extension_data");

        auto xcbReply = getReplyFunc2(xcb_xkb_use_extension, conn.get(), XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION);

        if(xcbReply.error())
            throw xcb_error("xcb_xkb_use_extension");

        xkbdevid = xkb_x11_get_core_keyboard_device_id(conn.get());
        if(xkbdevid < 0)
            throw xcb_error("xkb_x11_get_core_keyboard_device_id");

        xkbctx.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
        if(! xkbctx)
            throw xcb_error("xkb_context_new");

        xkbmap.reset(xkb_x11_keymap_new_from_device(xkbctx.get(), conn.get(), xkbdevid, XKB_KEYMAP_COMPILE_NO_FLAGS));
        if(!xkbmap)
            throw xcb_error("xkb_x11_keymap_new_from_device");
    
        xkbstate.reset(xkb_x11_state_new_from_device(xkbmap.get(), conn.get(), xkbdevid));
        if(!xkbstate)
            throw xcb_error("xkb_x11_state_new_from_device");

        // XCB_XKB_MAP_PART_KEY_TYPES, XCB_XKB_MAP_PART_KEY_SYMS, XCB_XKB_MAP_PART_MODIFIER_MAP, XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS
        // XCB_XKB_MAP_PART_KEY_ACTIONS, XCB_XKB_MAP_PART_VIRTUAL_MODS, XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP
        uint16_t required_map_parts = 0;
        uint16_t required_events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_STATE_NOTIFY;

        auto cookie = xcb_xkb_select_events_checked(conn.get(), xkbdevid, required_events, 0, required_events, required_map_parts, required_map_parts, nullptr);
        if(GenericError(xcb_request_check(conn.get(), cookie)))
            throw xcb_error("xcb_xkb_select_events");
    }

    std::string XCB::XkbClient::atomName(xcb_atom_t atom) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_atom_name, conn.get(), atom);
        if(auto reply = xcbReply.reply())
        {
            const char* name = xcb_get_atom_name_name(reply.get());
            size_t len = xcb_get_atom_name_name_length(reply.get());
            return std::string(name, name + len);
        }
 
        return "NONE";
    }

    int XCB::XkbClient::xkbGroup(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_xkb_get_state, conn.get(), XCB_XKB_ID_USE_CORE_KBD);
        if(auto err = xcbReply.error())
            throw xcb_error("xcb_xkb_get_names");
 
        if(auto reply = xcbReply.reply())
            return reply->group;
 
        return -1;
    }

    std::vector<std::string> XCB::XkbClient::xkbNames(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_xkb_get_names, conn.get(), XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_NAME_DETAIL_GROUP_NAMES | XCB_XKB_NAME_DETAIL_SYMBOLS);

        if(xcbReply.error())
            throw xcb_error("xcb_xkb_get_names");

        std::vector<std::string> res;
        res.reserve(4);

        if(auto reply = xcbReply.reply())
        {
            const void *buffer = xcb_xkb_get_names_value_list(reply.get());
            xcb_xkb_get_names_value_list_t list;

            xcb_xkb_get_names_value_list_unpack(buffer, reply->nTypes, reply->indicators, reply->virtualMods,
                                            reply->groupNames, reply->nKeys, reply->nKeyAliases, reply->nRadioGroups, reply->which, & list);
            int groups = xcb_xkb_get_names_value_list_groups_length(reply.get(), & list);

            for(int ii = 0; ii < groups; ++ii)
                res.emplace_back(atomName(list.groups[ii]));
        }
    
        return res;
    }

    xcb_keysym_t XCB::XkbClient::keycodeGroupToKeysym(xcb_keycode_t keycode, int group, bool shifted) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, conn.get(), minKeycode, maxKeycode - minKeycode + 1);
 
        auto reply = xcbReply.reply();
        if(! reply)
            throw xcb_error("xcb_get_keyboard_mapping");

        const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply.get());
        if(! keysyms)
            throw xcb_error("xcb_get_keyboard_mapping_keysyms");

        int keysymsPerKeycode = reply->keysyms_per_keycode;
        if(1 > keysymsPerKeycode)
            throw xcb_error("keysyms_per_keycode");
        
        int keysymsLength = xcb_get_keyboard_mapping_keysyms_length(reply.get());
        int keycodesCount = keysymsLength / keysymsPerKeycode;
        Application::debug("%s: keycode: 0x%02x, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d",
                           __FUNCTION__, keycode, keysymsPerKeycode, keysymsLength, keycodesCount);

        int index = (keycode - minKeycode) * keysymsPerKeycode + group * 2;
        if(index + 1 >= keysymsLength)
        {
                Application::error("%s: index out of range %d, current group: %d, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d",
                                   __FUNCTION__, index, group, keysymsPerKeycode, keysymsLength, keycodesCount);
            return 0;
        }

        return shifted ? keysyms[index + 1] : keysyms[index];
    }

    std::pair<xcb_keycode_t, int> XCB::XkbClient::keysymToKeycodeGroup(xcb_keysym_t keysym) const
    {
        auto empty = std::make_pair<xcb_keycode_t, int>(NULL_KEYCODE, -1);
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, conn.get(), minKeycode, maxKeycode - minKeycode + 1);
 
        auto reply = xcbReply.reply();
        if(! reply)
            throw xcb_error("xcb_get_keyboard_mapping");

        const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply.get());
        if(! keysyms)
            throw xcb_error("xcb_get_keyboard_mapping_keysyms");

        int keysymsPerKeycode = reply->keysyms_per_keycode;
        if(1 > keysymsPerKeycode)
            throw xcb_error("keysyms_per_keycode");
        
        int keysymsLength = xcb_get_keyboard_mapping_keysyms_length(reply.get());
        int keycodesCount = keysymsLength / keysymsPerKeycode;
        Application::debug("%s: keysym: 0x%08x, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d",
                           __FUNCTION__, keysym, keysymsPerKeycode, keysymsLength, keycodesCount);
        // shifted/unshifted
        int groupsCount = keysymsPerKeycode >> 1;

        for(int group = 0; group < groupsCount; ++group)
        {
            for(int ii = 0; ii < keycodesCount; ++ii)
            {
                auto keycode = minKeycode + ii;
                int index = ii * keysymsPerKeycode + group * 2;

                if(index + 1 >= keysymsLength)
                {
                    Application::error("%s: index out of range %d, current group: %d, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d",
                                       __FUNCTION__, index, group, keysymsPerKeycode, keysymsLength, keycodesCount);
                    return empty;
                }
                
                // check normal/shifted keysyms
                if(keysym == keysyms[index] || keysym == keysyms[index + 1])
                    return std::make_pair(keycode, group);
            }
        }
        
        Application::warning("%s: keysym not found 0x%08x, group names: [%s]", __FUNCTION__, keysym, Tools::join(xkbNames(), ",").c_str());
        return empty;
    }

    bool XCB::XkbClient::xcbError(void) const
    {
        return error;
    }

    bool XCB::XkbClient::xcbEventProcessing(void)
    {
        if(int err = xcb_connection_has_error(conn.get()))
        {
            Application::error("%s: xcb error: code: %d", __FUNCTION__, err);
            error = true;
            return false;
        }

        auto ev = GenericEvent(xcb_poll_for_event(conn.get()));
        auto type = ev ? ev->response_type & ~0x80 : 0;

        if(type != 0)
        {
            bool resetMapState = false;

            if(xkbext->first_event == type)
            {
                auto xkbev = ev->pad0;
                if(XCB_XKB_MAP_NOTIFY == xkbev)
                {
                    auto mn = reinterpret_cast<xcb_xkb_map_notify_event_t*>(ev.get());
                    resetMapState = true;
                    minKeycode = mn->minKeyCode;
                    maxKeycode = mn->maxKeyCode;
                }
                else
                if(XCB_XKB_NEW_KEYBOARD_NOTIFY == xkbev)
                {
                    if(auto kn = reinterpret_cast<xcb_xkb_new_keyboard_notify_event_t*>(ev.get()))
                    {
                        if(kn->deviceID == xkbdevid && (kn->changed & XCB_XKB_NKN_DETAIL_KEYCODES))
                        {
                            resetMapState = true;
                            minKeycode = kn->minKeyCode;
                            maxKeycode = kn->maxKeyCode;
                        }
                    }
                }
                else
                if(xkbev == XCB_XKB_STATE_NOTIFY)
                {
                    if(auto sn = reinterpret_cast<xcb_xkb_state_notify_event_t*>(ev.get()))
                    {
                        xkb_state_update_mask(xkbstate.get(), sn->baseMods, sn->latchedMods, sn->lockedMods,
                                                      sn->baseGroup, sn->latchedGroup, sn->lockedGroup);
                        if(sn->changed & XCB_XKB_STATE_PART_GROUP_STATE)
                            xkbStateChangeEvent(sn->group);
                    }
                }
            }

            if(resetMapState)
            {
                // free state first
                xkbstate.reset();
                xkbmap.reset();

                // set new
                xkbmap.reset(xkb_x11_keymap_new_from_device(xkbctx.get(), conn.get(), xkbdevid, XKB_KEYMAP_COMPILE_NO_FLAGS));
                xkbstate.reset(xkb_x11_state_new_from_device(xkbmap.get(), conn.get(), xkbdevid));

                xkbStateResetEvent();
            }
        }

        return true;
    }
}
