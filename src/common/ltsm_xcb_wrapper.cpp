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
#include <exception>

#include "xcb/xtest.h"
#include "xcb/xproto.h"
#include "xcb/damage.h"
#include "xcb/randr.h"

#ifdef LTSM_BUILD_XCB_ERRORS
#include "libxcb-errors/xcb_errors.h"
#endif

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"

using namespace std::chrono_literals;

namespace LTSM
{
    namespace XCB
    {
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
	    x = -1; y = -1;
	    width = 0; height = 0;
	}

        void Region::assign(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh)
	{
	    x = rx; y = ry;
	    width = rw; height = rh;
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
            else
            if(! reg.empty() && *this != reg)
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

        std::list<Region> Region::divideCounts(size_t cw, size_t ch) const
        {
            size_t bw = width <= cw ? 1 : width / cw;
            size_t bh = height <= cw ? 1 : height / ch;
            return divideBlocks(bw, bh);
        }

        std::list<Region> Region::divideBlocks(size_t cw, size_t ch) const
        {
            std::list<Region> res;

            if(cw > width) cw = width;
            if(ch > height) ch = height;

            for(size_t yy = 0; yy < height; yy += ch)
            {
                for(size_t xx = 0; xx < width; xx += cw)
                {
                    uint16_t fixedw = std::min(width - xx, cw);
                    uint16_t fixedh = std::min(height - yy, ch);
                    res.emplace_back(x + xx, y + yy, fixedw, fixedh);
                }
            }
                
            return res;
        }
 
        size_t HasherKeyCodes::operator()(const KeyCodes & kc) const
        {
            if(kc.get())
            {
                size_t len = 0;
                while(*(kc.get() + len) != XCB_NO_SYMBOL) len++;

                return Tools::crc32b(kc.get(), len);
            }

            return 0;
        }

        void error(const char* func, const GenericError & err)
        {
            Application::error("%s error code: %d, major: 0x%02x, minor: 0x%04x, sequence: %u",
                           func, static_cast<int>(err->error_code), static_cast<int>(err->major_code), err->minor_code, err->sequence);
        }
    }

    /* XCB::SHM */
    XCB::shm_t::~shm_t()
    {
        if(xcb) xcb_shm_detach(conn, xcb);
        if(addr) shmdt(addr);
        if(0 < shm) shmctl(shm, IPC_RMID, 0);
    }

    XCB::SHM::SHM(int shmid, uint8_t* addr, xcb_connection_t* conn)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_shm_attach_checked(conn, id, shmid, 0);
        if(auto err = GenericError(xcb_request_check(conn, cookie)))
        {
            error("xcb_shm_attach", err);
        }
        else
        {
            reset(new shm_t(shmid, addr, conn, id));
        }
    }

    XCB::PixmapInfoReply XCB::SHM::getPixmapRegion(xcb_drawable_t winid, const Region & reg, size_t offset, uint32_t planeMask) const
    {
	xcb_generic_error_t* error;
        auto cookie = xcb_shm_get_image(connection(), winid, reg.x, reg.y, reg.width, reg.height,
                                    planeMask, XCB_IMAGE_FORMAT_Z_PIXMAP, xid(), offset);

        auto xcbReply = getReplyConn(xcb_shm_get_image, connection(), cookie);
        PixmapInfoReply res;

	if(xcbReply.error())
        {
            XCB::error("xcb_shm_get_image", xcbReply.error());
            return res;
        }

        if(auto reply = xcbReply.reply())
            res = std::make_shared<PixmapInfoSHM>(reply->depth, reply->visual, *this, reply->size);

        return res;
    }

    /* XCB::GC */
    XCB::GC::GC(xcb_drawable_t win, xcb_connection_t* conn, uint32_t value_mask, const void* value_list)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_create_gc_checked(conn, id, win, value_mask, value_list);

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
        {
            error("xcb_create_gc", err);
        }
        else
        {
            reset(new gc_t(conn, id));
        }
    }

    /* XCB::Damage */
    XCB::Damage::Damage(xcb_drawable_t win, int level, xcb_connection_t* conn)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_damage_create_checked(conn, id, win, level);

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
        {
            error("xcb_damage_create", err);
        }
        else
        {
            reset(new damage_t(conn, id));
        }
    }

    bool XCB::Damage::addRegion(xcb_drawable_t winid, xcb_xfixes_region_t regid)
    {
        auto cookie = xcb_damage_add_checked(connection(), winid, regid);
        if(auto err = GenericError(xcb_request_check(connection(), cookie)))
        {
            error("xcb_damage_add", err);
            return false;
        }
        return true;
    }

    bool XCB::Damage::subtractRegion(xcb_xfixes_region_t repair, xcb_xfixes_region_t parts)
    {
        auto cookie = xcb_damage_subtract_checked(connection(), xid(), repair, parts);
        if(auto err = GenericError(xcb_request_check(connection(), cookie)))
        {
            error("xcb_damage_subtract", err);
            return false;
        }
        return true;
    }

    /* XCB::XFixesRegion */
    XCB::XFixesRegion::XFixesRegion(const Region & reg, xcb_connection_t* conn)
    {
        const xcb_rectangle_t rect{reg.x, reg.y, reg.width, reg.height};

        auto id = xcb_generate_id(conn);
        auto cookie = xcb_xfixes_create_region_checked(conn, id, 1, & rect);

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
        {
            error("xcb_xfixes_create_region", err);
        }
        else
        {
            reset(new xfixes_region_t(conn, id));
        }
    }

    XCB::XFixesRegion::XFixesRegion(const xcb_rectangle_t* rects, uint32_t count, xcb_connection_t* conn)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_xfixes_create_region_checked(conn, id, count, rects);

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
        {
            error("xcb_xfixes_create_region", err);
        }
        else
        {
            reset(new xfixes_region_t(conn, id));
        }
    }

    XCB::XFixesRegion::XFixesRegion(xcb_window_t win, xcb_shape_kind_t kind, xcb_connection_t* conn)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_xfixes_create_region_from_window_checked(conn, id, win, kind);

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
        {
            error("xcb_xfixes_create_region", err);
        }
        else
        {
            reset(new xfixes_region_t(conn, id));
        }
    }

    XCB::XFixesRegion XCB::XFixesRegion::intersect(xcb_xfixes_region_t reg2) const
    {
        auto id = xcb_generate_id(connection());
        auto cookie = xcb_xfixes_intersect_region_checked(connection(), xid(), reg2, id);
        XCB::XFixesRegion res;

        if(auto err = GenericError(xcb_request_check(connection(), cookie)))
        {
            error("xcb_xfixes_intersect_region", err);
        }
        else
        {
            res.reset(new xfixes_region_t(connection(), id));
        }

        return res;
    }

    XCB::XFixesRegion XCB::XFixesRegion::unionreg(xcb_xfixes_region_t reg2) const
    {
        auto id = xcb_generate_id(connection());
        auto cookie = xcb_xfixes_union_region_checked(connection(), xid(), reg2, id);
        XCB::XFixesRegion res;

        if(auto err = GenericError(xcb_request_check(connection(), cookie)))
        {
            error("xcb_xfixes_union_region", err);
        }
        else
        {
            res.reset(new xfixes_region_t(connection(), id));
        }

        return res;
    }

    bool XCB::KeyCodes::isValid(void) const
    {
        return get() && *get() != XCB_NO_SYMBOL;
    }

    bool XCB::KeyCodes::operator==(const KeyCodes & kc) const
    {
        auto ptr1 = get();
        auto ptr2 = kc.get();

        if(! ptr1 || ! ptr2) return false;
        if(ptr1 == ptr2) return true;

        while(*ptr1 != XCB_NO_SYMBOL && *ptr2 != XCB_NO_SYMBOL)
        {
            if(*ptr1 != *ptr2) return false;
            ptr1++; ptr2++;
        }

        return *ptr1 == *ptr2;
    }

    /* XCB::Connector */
    XCB::Connector::Connector(const char* addr) : _conn(nullptr), _setup(nullptr)
    {
        _conn = xcb_connect(addr, nullptr);

        if(xcb_connection_has_error(_conn))
        {
            Application::error("%s: %s failed, addr: %s", __FUNCTION__, "xcb_connect", addr);
            throw std::runtime_error("XCB::Connector");
        }

        _setup = xcb_get_setup(_conn);
        if(! _setup)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xcb_get_setup");
            throw std::runtime_error("XCB::Connector");
        }
    }

    XCB::Connector::~Connector()
    {
        xcb_disconnect(_conn);
    }

    size_t XCB::Connector::pixmapBitsPerPixel(size_t depth) const
    {
        for(auto fIter = xcb_setup_pixmap_formats_iterator(_setup); fIter.rem; xcb_format_next(& fIter))
            if(fIter.data->depth == depth) return fIter.data->bits_per_pixel;

        return 0;
    }

    size_t XCB::Connector::pixmapDepth(size_t bitsPerPixel) const
    {
        for(auto fIter = xcb_setup_pixmap_formats_iterator(_setup); fIter.rem; xcb_format_next(& fIter))
            if(fIter.data->bits_per_pixel == bitsPerPixel) return fIter.data->depth;

        return 0;
    }

    const xcb_setup_t* XCB::Connector::setup(void) const
    {
        return _setup;
    }

    bool XCB::Connector::testConnection(const char* addr)
    {
        auto conn = xcb_connect(addr, nullptr);
        int err = xcb_connection_has_error(conn);
        xcb_disconnect(conn);
        return err == 0;
    }

    XCB::GenericError XCB::Connector::checkRequest(const xcb_void_cookie_t & cookie) const
    {
        return GenericError(xcb_request_check(_conn, cookie));
    }

    XCB::GC XCB::Connector::createGC(xcb_drawable_t win, uint32_t value_mask, const void* value_list)
    {
        return GC(win, _conn, value_mask, value_list);
    }

    XCB::SHM XCB::Connector::createSHM(size_t shmsz, int mode)
    {
        int shmid = shmget(IPC_PRIVATE, shmsz, IPC_CREAT | mode);
        if(shmid == -1)
        {
            Application::error("shmget failed, size: %d, error: %s", shmsz, strerror(errno));
            return SHM();
        }

        uint8_t* shmaddr = reinterpret_cast<uint8_t*>(shmat(shmid, 0, 0));
        // man shmat: check result
        if(shmaddr == reinterpret_cast<uint8_t*>(-1) && 0 != errno)
        {
            Application::error("shmaddr failed, id: %d, error: %s", shmid, strerror(errno));
            return SHM();
        }

        return SHM(shmid, shmaddr, _conn);
    }

    XCB::Damage XCB::Connector::createDamage(xcb_drawable_t win, int level)
    {
        return Damage(win, level, _conn);
    }

    XCB::XFixesRegion XCB::Connector::createFixesRegion(const Region & reg)
    {
	return XFixesRegion(reg, _conn);
    }

    XCB::XFixesRegion XCB::Connector::createFixesRegion(const xcb_rectangle_t* rect, size_t num)
    {
	return XFixesRegion(rect, num, _conn);
    }

    size_t XCB::Connector::getMaxRequest(void) const
    {
	return xcb_get_maximum_request_length(_conn);
    }

    bool XCB::Connector::checkAtom(const std::string & name) const
    {
        return XCB_ATOM_NONE != getAtom(name, false);
    }

    xcb_atom_t XCB::Connector::getAtom(const std::string & name, bool create) const
    {
	auto cookie = xcb_intern_atom(_conn, create ? 0 : 1, name.size(), name.c_str());
	auto xcbReply = getReplyFunc(xcb_intern_atom, cookie);

	if(xcbReply.error())
        {
            extendedError(xcbReply.error(), "xcb_intern_atom");
            return XCB_ATOM_NONE;
        }

	return xcbReply.reply() ? xcbReply.reply()->atom : XCB_ATOM_NONE;
    }

    std::string XCB::Connector::getAtomName(xcb_atom_t atom) const
    {
	if(atom == XCB_ATOM_NONE)
	    return std::string("NONE");

	auto cookie = xcb_get_atom_name(_conn, atom);
	auto xcbReply = getReplyFunc(xcb_get_atom_name, cookie);

	if(xcbReply.error())
        {
            extendedError(xcbReply.error(), "xcb_get_atom_name");
            return std::string();
        }

	if(xcbReply.reply())
	{
	    const char* name = xcb_get_atom_name_name(xcbReply.reply().get());
	    size_t len = xcb_get_atom_name_name_length(xcbReply.reply().get());
	    return std::string(name, len);
	}

	return std::string();
    }

    void XCB::Connector::extendedError(const GenericError & gen, const char* func) const
    {
        if(gen) extendedError(gen.get(), func);
    }

    void XCB::Connector::extendedError(const xcb_generic_error_t* err, const char* func) const
    {
#ifdef LTSM_BUILD_XCB_ERRORS
        xcb_errors_context_t* err_ctx;
        xcb_errors_context_new(_conn, &err_ctx);
        const char* major, *minor, *extension, *error;
        major = xcb_errors_get_name_for_major_code(err_ctx, err->major_code);
        minor = xcb_errors_get_name_for_minor_code(err_ctx, err->major_code, err->minor_code);
        error = xcb_errors_get_name_for_error(err_ctx, err->error_code, &extension);
        Application::error("%s error: %s:%s, %s:%s, resource %u sequence %u",
                           func, error, extension ? extension : "no_extension", major, minor ? minor : "no_minor",
                           (unsigned int) err->resource_id, (unsigned int) err->sequence);
        xcb_errors_context_free(err_ctx);
#else

        Application::error("%s error code: %d, major: 0x%02x, minor: 0x%04x, sequence: %u",
                           func, static_cast<int>(err->error_code), static_cast<int>(err->major_code), err->minor_code, err->sequence);
#endif
    }

    int XCB::Connector::hasError(void) const
    {
	return xcb_connection_has_error(_conn);
    }

    XCB::GenericEvent XCB::Connector::poolEvent(void)
    {
        return GenericEvent(xcb_poll_for_event(_conn));
    }

    bool XCB::Connector::checkExtension(const Module & module) const
    {
        if(module == Module::TEST)
        {
            auto _test = xcb_get_extension_data(_conn, &xcb_test_id);
            if(! _test || ! _test->present)
                return false;

            auto cookie = xcb_test_get_version(_conn, XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION);
	    auto xcbReply = getReplyFunc(xcb_test_get_version, cookie);

            if(xcbReply.error())
            {
                extendedError(xcbReply.error(), "xcb_test_query_version");
                return false;
            }

	    if(xcbReply.reply())
	    {
    	        Application::debug("used %s extension, version: %d.%d", "TEST", xcbReply.reply()->major_version, xcbReply.reply()->minor_version);
    	        return true;
	    }
        }
        else
        if(module == Module::DAMAGE)
        {
            auto _damage = xcb_get_extension_data(_conn, &xcb_damage_id);
            if(! _damage || ! _damage->present)
                return false;

            auto cookie = xcb_damage_query_version(_conn, XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION);
	    auto xcbReply = getReplyFunc(xcb_damage_query_version, cookie);

            if(xcbReply.error())
            {
                extendedError(xcbReply.error(), "xcb_damage_query_version");
                return false;
            }

	    if(xcbReply.reply())
	    {
    	        Application::debug("used %s extension, version: %d.%d", "DAMAGE", xcbReply.reply()->major_version, xcbReply.reply()->minor_version);
    	        return true;
	    }
        }
        else
        if(module == Module::XFIXES)
        {
            auto _xfixes = xcb_get_extension_data(_conn, &xcb_xfixes_id);
            if(! _xfixes || ! _xfixes->present)
                return false;

            auto cookie = xcb_xfixes_query_version(_conn, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
	    auto xcbReply = getReplyFunc(xcb_xfixes_query_version, cookie);

            if(xcbReply.error())
            {
                extendedError(xcbReply.error(), "xcb_xfixes_query_version");
                return false;
            }

	    if(xcbReply.reply())
            {
	        Application::debug("used %s extension, version: %d.%d", "XFIXES", xcbReply.reply()->major_version, xcbReply.reply()->minor_version);
    	        return true;
	    }
        }
        else
        if(module == Module::RANDR)
        {
            auto _randr = xcb_get_extension_data(_conn, &xcb_randr_id);
            if(! _randr || ! _randr->present)
                return false;

            auto cookie = xcb_randr_query_version(_conn, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);
	    auto xcbReply = getReplyFunc(xcb_randr_query_version, cookie);

            if(xcbReply.error())
            {
                extendedError(xcbReply.error(), "xcb_randr_query_version");
                return false;
            }

	    if(xcbReply.reply())
	    {
    	        Application::debug("used %s extension, version: %d.%d", "RANDR", xcbReply.reply()->major_version, xcbReply.reply()->minor_version);
    	        return true;
	    }
        }
        else
        if(module == Module::SHM)
        {
            auto _shm = xcb_get_extension_data(_conn, &xcb_shm_id);
	    if(! _shm || ! _shm->present)
                return false;

            auto cookie = xcb_shm_query_version(_conn);
	    auto xcbReply = getReplyFunc(xcb_shm_query_version, cookie);

            if(xcbReply.error())
            {
                extendedError(xcbReply.error(), "xcb_shm_query_version");
                return false;
            }

	    if(xcbReply.reply())
            {
	        Application::debug("used %s extension, version: %d.%d", "SHM", xcbReply.reply()->major_version, xcbReply.reply()->minor_version);
    	        return true;
	    }
        }

        return false;
    }

    int XCB::Connector::eventNotify(const GenericEvent & ev, const Module & module) const
    {
        // clear bit
        auto response_type = ev ? ev->response_type & ~0x80 : 0;

        if(0 < response_type)
        {
            if(module == Module::DAMAGE)
            {
                // for receive it, usage:
                // RootDisplay::createDamageNotify
                auto _damage = xcb_get_extension_data(_conn, &xcb_damage_id);
                if(response_type == _damage->first_event + XCB_DAMAGE_NOTIFY)
                    return XCB_DAMAGE_NOTIFY;
            }
            else
            if(module == Module::XFIXES)
            {
                // for receive it, usage input filter:
                // xcb_xfixes_select_selection_input(xcb_xfixes_selection_event_mask_t)
                // xcb_xfixes_select_cursor_input(xcb_xfixes_cursor_notify_mask_t)
                //
                auto _xfixes = xcb_get_extension_data(_conn, &xcb_xfixes_id);
                auto types = { XCB_XFIXES_SELECTION_NOTIFY, XCB_XFIXES_CURSOR_NOTIFY };
                for(auto & type : types)
                    if(response_type == _xfixes->first_event + type)
                        return type;
            }
            else
            if(module == Module::RANDR)
            {
                // for receive it, usage input filter:
                // xcb_xrandr_select_input(xcb_randr_notify_mask_t)
                //
                auto _randr = xcb_get_extension_data(_conn, &xcb_randr_id);
                auto types = { XCB_RANDR_SCREEN_CHANGE_NOTIFY, XCB_RANDR_NOTIFY };
                for(auto & type : types)
                    if(response_type == _randr->first_event + type) return type;
            }
        }

        return -1;
    }

    bool XCB::Connector::isDamageNotify(const GenericEvent & ev) const
    {
        return XCB_DAMAGE_NOTIFY == eventNotify(ev, Module::DAMAGE);
    }

    bool XCB::Connector::isXFixesSelectionNotify(const GenericEvent & ev) const
    {
        return XCB_XFIXES_SELECTION_NOTIFY == eventNotify(ev, Module::XFIXES);
    }

    bool XCB::Connector::isXFixesCursorNotify(const GenericEvent & ev) const
    {
        return XCB_XFIXES_CURSOR_NOTIFY == eventNotify(ev, Module::XFIXES);
    }

    bool XCB::Connector::isRandrScreenNotify(const GenericEvent & ev) const
    {
        return XCB_RANDR_SCREEN_CHANGE_NOTIFY == eventNotify(ev, Module::RANDR);
    }

    bool XCB::Connector::isRandrCRTCNotify(const GenericEvent & ev) const
    {
        if(XCB_RANDR_NOTIFY == eventNotify(ev, Module::RANDR))
        {
            auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
            return rn->subCode == XCB_RANDR_NOTIFY_CRTC_CHANGE;
        }

        return false;
    }

    bool XCB::Connector::isRandrOutputNotify(const GenericEvent & ev) const
    {
        if(XCB_RANDR_NOTIFY == eventNotify(ev, Module::RANDR))
        {
            auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
            return rn->subCode == XCB_RANDR_NOTIFY_OUTPUT_CHANGE;
        }

        return false;
    }

    int XCB::Connector::eventErrorOpcode(const GenericEvent & ev, const Module & module) const
    {
        if(ev && ev->response_type == 0)
        {
            auto error = ev.toerror();

            if(module == Module::TEST)
            {
                auto _test = xcb_get_extension_data(_conn, &xcb_test_id);
                if(error->major_code == _test->major_opcode)
                    return error->minor_code;
            }
            else
            if(module == Module::DAMAGE)
            {
                auto _damage = xcb_get_extension_data(_conn, &xcb_damage_id);
                if(error->major_code == _damage->major_opcode)
                    return error->minor_code;
            }
            else
            if(module == Module::XFIXES)
            {
                auto _xfixes = xcb_get_extension_data(_conn, &xcb_xfixes_id);
                if(error->major_code == _xfixes->major_opcode)
                    return error->minor_code;
            }
            else
            if(module == Module::RANDR)
            {
                auto _randr = xcb_get_extension_data(_conn, &xcb_randr_id);
                if(error->major_code == _randr->major_opcode)
                    return error->minor_code;
            }
            else
            if(module == Module::SHM)
            {
                auto _shm = xcb_get_extension_data(_conn, &xcb_shm_id);
                if(error->major_code == _shm->major_opcode)
                    return error->minor_code;
            }
        }

        return -1;
    }

    /* XCB::RootDisplay */
    XCB::RootDisplay::RootDisplay(const std::string & addr) : Connector(addr.c_str()), _screen(nullptr),
        _symbols(nullptr), _format(nullptr), _visual(nullptr)
    {
        _screen = xcb_setup_roots_iterator(_setup).data;
        if(! _screen)
        {
            Application::error("%s: %s failed", __FUNCTION__, "root screen");
            throw std::runtime_error("XCB::RootDisplay");
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
            throw std::runtime_error("XCB::RootDisplay");
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
            throw std::runtime_error("XCB::RootDisplay");
        }

        _symbols = xcb_key_symbols_alloc(_conn);
        if(! _symbols)
        {
            Application::error("%s: %s failed", __FUNCTION__, "key symbols alloc");
            throw std::runtime_error("XCB::RootDisplay");
        }

        // check extensions
        if(! checkExtension(Module::SHM))
        {
            Application::error("%s: %s failed", __FUNCTION__, "SHM extension");
            throw std::runtime_error("XCB::RootDisplay");
        }

        if(! checkExtension(Module::DAMAGE))
        {
            Application::error("%s: %s failed", __FUNCTION__, "DAMAGE extension");
            throw std::runtime_error("XCB::RootDisplay");
        }

        if(! checkExtension(Module::XFIXES))
        {
            Application::error("%s: %s failed", __FUNCTION__, "XFIXES extension");
            throw std::runtime_error("XCB::RootDisplay");
        }

        if(! checkExtension(Module::TEST))
        {
            Application::error("%s: %s failed", __FUNCTION__, "TEST extension");
            throw std::runtime_error("XCB::RootDisplay");
        }

        if(! checkExtension(Module::RANDR))
        {
            Application::error("%s: %s failed", __FUNCTION__, "RANDR extension");
            throw std::runtime_error("XCB::RootDisplay");
        }

        auto wsz = size();

        // create randr notify
        xcb_randr_select_input(_conn, _screen->root, 
            XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);

        // create damage notify
        _damage = Damage(_screen->root, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES, _conn);
        if(_damage)
        {
            auto fixreg = XFixesRegion({0, 0, wsz.width, wsz.height}, _conn);
            if(fixreg) _damage.addRegion(_screen->root, fixreg.xid());
        }

        // init shm
        const int bpp = bitsPerPixel() >> 3;
        const int pagesz = 4096;
        const size_t shmsz = ((wsz.width * wsz.height * bpp / pagesz) + 1) * pagesz;

        _shm = createSHM(shmsz, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    }

    XCB::RootDisplay::~RootDisplay()
    {
	// release all buttons
	for(int button = 1; button <= 5; button++)
    	    xcb_test_fake_input(_conn, XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, _screen->root, 0, 0, 0);

	// release all keys
	for(int key = 1; key <= 255; key++)
    	    xcb_test_fake_input(_conn, XCB_KEY_RELEASE, key, XCB_CURRENT_TIME, _screen->root, 0, 0, 0);

        xcb_key_symbols_free(_symbols);
	xcb_flush(_conn);
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
        auto cookie = xcb_get_geometry(_conn, _screen->root);
        auto reply = xcb_get_geometry_reply(_conn, cookie, nullptr);

        if(reply)
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

    std::list<xcb_randr_screen_size_t> XCB::RootDisplay::screenSizes(void) const
    {
	std::list<xcb_randr_screen_size_t> res;

        auto cookie = xcb_randr_get_screen_info(_conn, _screen->root);
        auto xcbReply = getReplyFunc(xcb_randr_get_screen_info, cookie);

        if(xcbReply.error())
        {
            extendedError(xcbReply.error(), "xcb_randr_get_screen_info");
            return res;
        }

        if(xcbReply.reply())
        {
            xcb_randr_screen_size_t* sizes = xcb_randr_get_screen_info_sizes(xcbReply.reply().get());
            int length = xcb_randr_get_screen_info_sizes_length(xcbReply.reply().get());

            for(int pos = 0; pos < length; ++pos)
        	res.push_back(sizes[pos]);
	}

	return res;
    }

    bool XCB::RootDisplay::setScreenSize(uint16_t width, uint16_t height)
    {
        auto cookie = xcb_randr_get_screen_info(_conn, _screen->root);
        auto xcbReply = getReplyFunc(xcb_randr_get_screen_info, cookie);

        if(xcbReply.error())
        {
            extendedError(xcbReply.error(), "xcb_randr_get_screen_info");
            return false;
        }

        if(xcbReply.reply())
        {
            xcb_randr_screen_size_t* sizes = xcb_randr_get_screen_info_sizes(xcbReply.reply().get());
            int length = xcb_randr_get_screen_info_sizes_length(xcbReply.reply().get());

            auto it = std::find_if(sizes, sizes + length, [=](auto & ss){ return ss.width == width && ss.height == height; });
            if(it == sizes + length)
            {
        	it = std::min_element(sizes, sizes + length, [=](auto & ss1, auto & ss2)
			    { return std::abs(ss1.width * ss1.height - width * height) < std::abs(ss2.width * ss2.height - width * height); });

        	if(it == sizes + length)
        	{
            	    Application::error("set screen size failed, unknown mode: [%d,%d]", width, height);
            	    return false;
		}

		width = it->width;
		height = it->height;
            	Application::warning("set screen size, found nearest suitable mode: [%d,%d]", width, height);
            }

            damageSubtrack(region());

            int sizeID = std::distance(sizes, it);
            auto cookie = xcb_randr_set_screen_config(_conn, _screen->root, XCB_CURRENT_TIME,
                                xcbReply.reply()->config_timestamp, sizeID, xcbReply.reply()->rotation, 0);
            auto xcbReply2 = getReplyFunc(xcb_randr_set_screen_config, cookie);

            if(! xcbReply2.error())
		return true;

            extendedError(xcbReply.error(), "xcb_randr_set_screen_config");
        }

        return false;
    }

    XCB::PixmapInfoReply XCB::RootDisplay::copyRootImageRegion(const Region & reg, uint32_t planeMask) const
    {
        if(_shm)
            return _shm.getPixmapRegion(_screen->root, reg, 0, planeMask);

        size_t pitch = reg.width * (bitsPerPixel() >> 2);
        //size_t reqLength = sizeof(xcb_get_image_request_t) + pitch * reg.height;
        uint64_t maxReqLength = xcb_get_maximum_request_length(_conn);
        PixmapInfoReply res;

        if(pitch == 0)
        {
            Application::error("copy root image error, empty size: [%d,%d], bpp: %d", reg.width, reg.height, bitsPerPixel());
            return res;
        }

        PixmapInfoBuffer* info = nullptr;
	uint16_t allowRows = maxReqLength / pitch;
	if(allowRows > reg.height)
	    allowRows = reg.height;

        for(int16_t yy = reg.y; yy < reg.y + reg.height; yy += allowRows)
        {
	    // last rows
	    if(yy + allowRows > reg.y + reg.height)
		allowRows = reg.y + reg.height - yy;

            auto cookie = xcb_get_image(_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, _screen->root, reg.x, yy, reg.width, allowRows, planeMask);
	    auto xcbReply = getReplyFunc(xcb_get_image, cookie);

	    if(xcbReply.error())
    	    {
        	extendedError(xcbReply.error(), "xcb_get_image");
		break;
    	    }

            if(xcbReply.reply())
            {
		auto reply = xcbReply.reply().get();

                if(! info)
                    info = new PixmapInfoBuffer(reply->depth, reply->visual, reg.height * pitch);

                auto length = xcb_get_image_data_length(reply);
                auto data = xcb_get_image_data(reply);

                info->_pixels.insert(info->_pixels.end(), data, data + length);
            }
        }

        res.reset(info);
        return res;
    }

    bool XCB::RootDisplay::damageAdd(const xcb_rectangle_t* rects, size_t counts)
    {
	if(_damage && rects && counts)
	{
    	    auto fixreg = XFixesRegion(rects, counts, _conn);
	    return fixreg ? _damage.addRegion(_screen->root, fixreg.xid()) : false;
	}
	return false;
    }

    bool XCB::RootDisplay::damageAdd(const Region & reg)
    {
        if(_damage)
        {
    	    auto fixreg = XFixesRegion(region().intersected(reg), _conn);
	    return fixreg ? _damage.addRegion(_screen->root, fixreg.xid()) : false;
        }

        return false;
    }

    bool XCB::RootDisplay::damageSubtrack(const Region & reg)
    {
	if(_damage)
	{
    	    auto fixreg = XFixesRegion(reg, _conn);
	    return fixreg ? _damage.subtractRegion(fixreg.xid(), XCB_XFIXES_REGION_NONE) : false;
	}
	return false;
    }

    XCB::GenericEvent XCB::RootDisplay::poolEvent(void)
    {
        GenericEvent ev = Connector::poolEvent();

        if(isDamageNotify(ev))
        {
            auto wsz = size();
            auto notify = reinterpret_cast<xcb_damage_notify_event_t*>(ev.get());
            if(notify->area.x + notify->area.width > wsz.width || notify->area.y + notify->area.height > wsz.height)
            {
        	Application::debug("damage notify: [%d,%d,%d,%d] discard", notify->area.x, notify->area.y, notify->area.width, notify->area.height);
                xcb_discard_reply(_conn, notify->sequence);
                return GenericEvent();
            }
         }
        else
        if(isRandrCRTCNotify(ev))
        {
            auto notify = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                
            xcb_randr_crtc_change_t cc = notify->u.cc;
            if(0 < cc.width && 0 < cc.height)
            {
                auto wsz = size();
                if(cc.width != wsz.width || cc.height != wsz.height)
                {
        	    Application::debug("xcb crc change notify: [%d,%d] discard", cc.width, cc.height);
                    xcb_discard_reply(_conn, notify->sequence);
                    return GenericEvent();
                }

        	Application::debug("xcb crc change notify: [%d,%d]", cc.width, cc.height);

                // init shm
                const int bpp = bitsPerPixel() >> 3;
                const int pagesz = 4096;
                const size_t shmsz = ((cc.width * cc.height * bpp / pagesz) + 1) * pagesz;
                _shm = createSHM(shmsz, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

                // create damage notify
                _damage = Damage(_screen->root, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES, _conn);
		damageAdd({0, 0, cc.width, cc.height});
            }
        }

        return ev;
    }

    XCB::KeyCodes XCB::RootDisplay::keysymToKeycodes(int keysym) const
    {
	xcb_keycode_t* ptr = _symbols ?
		    xcb_key_symbols_get_keycode(_symbols, keysym) : nullptr;
        return KeyCodes(ptr);
    }

    /*
        void XCB::RootDisplay::eventProcessing(void)
        {
            auto ev = poolEvent();
            if(! ev) return false;

            if(ev->response_type == 0)
            {
                extendedError(error, __FUNCTION__, "unknown error");
            }
            else
            {
                switch(ev->response_type & ~0x80)
                {
                    case XCB_MOTION_NOTIFY:
                        break;

                    case XCB_BUTTON_PRESS:
                    case XCB_BUTTON_RELEASE:
                    case XCB_KEY_PRESS:
                    case XCB_KEY_RELEASE:
                        break;

                    case XCB_ENTER_NOTIFY:
                    case XCB_LEAVE_NOTIFY:
                        break;

                    default:
                        extendedError(error, __FUNCTION__, "unknown event");
                        break;
                }

                return true;
            }

            return false;
        }
    */

    /// @param type: XCB_KEY_PRESS, XCB_KEY_RELEASE, XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE, XCB_MOTION_NOTIFY
    /// @param keycode
    bool XCB::RootDisplay::fakeInputKeycode(int type, uint8_t keycode)
    {
        // type: XCB_KEY_PRESS, XCB_KEY_RELEASE, XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE, XCB_MOTION_NOTIFY
        auto cookie = xcb_test_fake_input_checked(_conn, type, keycode, XCB_CURRENT_TIME, _screen->root, 0, 0, 0);
        auto errorReq = checkRequest(cookie);

        if(! errorReq)
            return true;

        extendedError(errorReq, "xcb_test_fake_input");
        return false;
    }

    bool XCB::RootDisplay::fakeInputKeysym(int type, const KeyCodes & keycodes)
    {
        if(keycodes.isValid())
        {
            auto keycode = keycodes.get();

            while(XCB_NO_SYMBOL != *keycode)
            {
                if( ! fakeInputKeycode(type, *keycode))
                    return false;

                keycode++;
            }

            return true;
        }

        return false;
    }


    /// @param type: XCB_KEY_PRESS, XCB_KEY_RELEASE, XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE, XCB_MOTION_NOTIFY
    /// @param button: left 1, middle 2, right 3, scrollup 4, scrolldw 5
    bool XCB::RootDisplay::fakeInputMouse(int type, int button, int posx, int posy)
    {
        auto cookie = xcb_test_fake_input_checked(_conn, type, button, XCB_CURRENT_TIME, _screen->root, posx, posy, 0);
        auto errorReq = checkRequest(cookie);

        if(! errorReq)
            return true;

        extendedError(errorReq, "xcb_test_fake_input");
        return false;
    }

    /*
        void XCB::RootDisplay::fillRectangles(uint32_t color, uint32_t rects_num, const xcb_rectangle_t* rects_val)
        {
            uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
            uint32_t values[]  = { color, color, 0 };
            auto back = createGC(mask, values);

            auto cookie = xcb_poly_fill_rectangle_checked(_conn, _screen->root, back.xcb, rects_num, rects_val);
            auto errorReq = checkRequest(cookie);
            if(errorReq)
                extendedError(errorReq, "xcb_poly_fill_rectangle");
        }
    */

    void XCB::RootDisplay::fillBackground(uint32_t color)
    {
        if(depth() < 24 && color > 0x0000FFFF)
        {
            // convert RGB888 to RGB565
            uint16_t r = ((color >> 24) & 0xFF) * 0x1F / 0xFF;
            uint16_t g = ((color >> 16) & 0xFF) * 0x3F / 0xFF;
            uint16_t b = (color & 0xFF) * 0x1F / 0xFF;
            color = (r << 11) | (g << 5) | b;
        }

        uint32_t colors[]  = { color, 0 };
        auto cookie = xcb_change_window_attributes(_conn, _screen->root, XCB_CW_BACK_PIXEL, colors);
        auto errorReq = checkRequest(cookie);

        if(errorReq)
            extendedError(errorReq, "xcb_change_window_attributes");
        else
        {
            auto wsz = size();
            cookie = xcb_clear_area_checked(_conn, 0, _screen->root, 0, 0, wsz.width, wsz.height);
            errorReq = checkRequest(cookie);

            if(errorReq)
                extendedError(errorReq, "xcb_clear_area");
        }
    }

    /* XCB::RootDisplayExt */
    XCB::RootDisplayExt::RootDisplayExt(const std::string & addr) : RootDisplay(addr),
        _atoms{ XCB_ATOM_NONE }, _atomPrimary(_atoms[0]), _atomClipboard(_atoms[1]), _atomBuffer(_atoms[2]),
        _atomTargets(_atoms[3]), _atomText(_atoms[4]), _atomTextPlain(_atoms[5]), _atomUTF8(_atoms[6])
    {
	const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	const uint32_t values[] = { _screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE }; 

	_selwin = xcb_generate_id(_conn);
	auto cookie = xcb_create_window_checked(_conn, 0, _selwin, _screen->root, -1, -1, 1, 1, 0,
			    XCB_WINDOW_CLASS_INPUT_OUTPUT, _screen->root_visual, mask, values);

	auto errorReq = checkRequest(cookie);
	if(! errorReq)
        {
	    _atomPrimary = getAtom("PRIMARY");
	    _atomClipboard = getAtom("CLIPBOARD");
	    _atomBuffer = getAtom("XSEL_DATA");
	    _atomTargets = getAtom("TARGETS");
	    _atomText = getAtom("TEXT");
	    _atomTextPlain = getAtom("text/plain;charset=utf-8");
	    _atomUTF8 = getAtom("UTF8_STRING");

	    const std::string name("LTSM_SEL");
	    xcb_change_property(_conn, XCB_PROP_MODE_REPLACE, _selwin,
				getAtom("_NET_WM_NAME"), _atomUTF8, 8, name.size(), name.data());

	    xcb_flush(_conn);

            // run periodic timer (555ms)
            _timerClipCheck = Tools::BaseTimer::create<std::chrono::milliseconds>(555, true,
                        [this](){ this->getSelectionEvent(this->_atomPrimary); });
        }
        else
	{
    	    extendedError(errorReq, "xcb_create_window");
            _selwin = 0;
	}
    }

    XCB::RootDisplayExt::~RootDisplayExt()
    {
        _timerClipCheck->stop();
	if(_selwin)
            xcb_destroy_window(_conn, _selwin);
    }

    bool XCB::RootDisplayExt::selectionClearAction(xcb_selection_clear_event_t* ev)
    {
	if(ev && ev->owner == _selwin)
        {
	    _selbuf.clear();

            if(Application::isDebugLevel(DebugLevel::Console) ||
                Application::isDebugLevel(DebugLevel::SyslogInfo))
                Application::info("%s: %s", __FUNCTION__, "event");
 	    return true;
        }

        return false;
    }

    xcb_window_t XCB::RootDisplayExt::getOwnerSelection(const xcb_atom_t & atomSel)
    {
	auto cookie = xcb_get_selection_owner(_conn, atomSel);
	auto xcbReply = getReplyFunc(xcb_get_selection_owner, cookie);

    	if(xcbReply.error())
    	{
    	    extendedError(xcbReply.error(), "xcb_get_selection_owner");
	    return XCB_WINDOW_NONE;
	}

	return xcbReply.reply()  ? xcbReply.reply()->owner : XCB_WINDOW_NONE;
    }

    bool XCB::RootDisplayExt::sendNotifyTargets(const xcb_selection_request_event_t & ev)
    {
	auto cookie = xcb_change_property_checked(_conn, XCB_PROP_MODE_REPLACE, ev.requestor, ev.property,
    		    XCB_ATOM, 8 * sizeof(xcb_atom_t), 4, & _atomTargets); /* send list: _atomTargets, _atomText, _atomTextPlain, _atomUTF8 */

    	auto errorReq = checkRequest(cookie);
    	if(errorReq)
	{
    	    extendedError(errorReq, "xcb_change_property");
	    return false;
	}

	xcb_selection_notify_event_t notify = { .response_type = XCB_SELECTION_NOTIFY, .pad0 = 0, .sequence = 0,
            .time = ev.time, .requestor = ev.requestor, .selection = ev.selection, .target = ev.target, .property = ev.property };

	xcb_send_event(_conn, 0, ev.requestor, XCB_EVENT_MASK_NO_EVENT, (const char*) & notify);
	xcb_flush(_conn);

        return true;
    }

    bool XCB::RootDisplayExt::sendNotifySelData(const xcb_selection_request_event_t & ev)
    {
        size_t length = _selbuf.size();
        size_t maxreq = getMaxRequest();
        auto chunk = std::min(maxreq, length);
       
	auto cookie = xcb_change_property_checked(_conn, XCB_PROP_MODE_REPLACE, ev.requestor, ev.property, ev.target, 8, chunk, _selbuf.data());
    	auto errorReq = checkRequest(cookie);

    	if(errorReq)
	{
    	    extendedError(errorReq, "xcb_change_property");
	    return false;
	}

	xcb_selection_notify_event_t notify = { .response_type = XCB_SELECTION_NOTIFY, .pad0 = 0, .sequence = 0,
            .time = ev.time, .requestor = ev.requestor, .selection = ev.selection, .target = ev.target, .property = ev.property };

	xcb_send_event(_conn, 0, ev.requestor, XCB_EVENT_MASK_NO_EVENT, (const char*) & notify);
	xcb_flush(_conn);
    
        if(Application::isDebugLevel(DebugLevel::Console) ||
            Application::isDebugLevel(DebugLevel::SyslogInfo))
	{
	    auto prop = getAtomName(ev.property);
	    std::string log(_selbuf.begin(), _selbuf.end());
            Application::info("xcb put selection, size: %d, content: `%s', to window: 0x%08x, property: `%s'", _selbuf.size(), log.c_str(), ev.requestor, prop.c_str());
	}

        return true;
    }

    bool XCB::RootDisplayExt::selectionRequestAction(xcb_selection_request_event_t* ev)
    {
	if(ev->selection == _atomClipboard || ev->selection == _atomPrimary)
	{
	    if(ev->target == _atomTargets)
		return sendNotifyTargets(*ev);
	    else
	    if((ev->target == _atomText || ev->target == _atomTextPlain || ev->target == _atomUTF8) &&
		ev->property != XCB_ATOM_NONE && ! _selbuf.empty())
		return sendNotifySelData(*ev);
	}

	// other variants: discard request
	xcb_selection_notify_event_t notify = { .response_type = XCB_SELECTION_NOTIFY, .pad0 = 0, .sequence = 0,
            .time = ev->time, .requestor = ev->requestor, .selection = ev->selection, .target = ev->target, .property = XCB_ATOM_NONE };

	xcb_send_event(_conn, 0, ev->requestor, XCB_EVENT_MASK_NO_EVENT, (const char*) & notify);
	xcb_flush(_conn);

        if(Application::isDebugLevel(DebugLevel::Console) ||
            Application::isDebugLevel(DebugLevel::SyslogInfo))
	{
	    auto sel = getAtomName(ev->selection);
	    auto tgt = getAtomName(ev->target);
	    auto prop = getAtomName(ev->property);
	    Application::error("xcb discard selection: `%s', target: `%s', property: `%s', window: 0x%08x", sel.c_str(), tgt.c_str(), prop.c_str(), ev->requestor);
	}

	return false;
    }

    bool XCB::RootDisplayExt::setClipboardEvent(std::vector<uint8_t> & buf, const xcb_atom_t & atomSel)
    {
	if(_selbuf != buf)
	{
    	    std::swap(_selbuf, buf);

	    // take owner
	    if(_selbuf.empty())
		return false;

	    auto owner = getOwnerSelection(atomSel);
	    if(owner != _selwin)
	    {
    		xcb_set_selection_owner(_conn, _selwin, atomSel, XCB_CURRENT_TIME);
		xcb_flush(_conn);
	    }

	    return true;
	}

	return false;
    }

    bool XCB::RootDisplayExt::setClipboardEvent(std::vector<uint8_t> & buf)
    {
	return setClipboardEvent(buf, _atomPrimary); // _atomClipboard);
    }

    bool XCB::RootDisplayExt::getSelectionEvent(const xcb_atom_t & atomSel)
    {
	auto owner = getOwnerSelection(atomSel);
	if(owner == XCB_WINDOW_NONE || owner == _selwin)
	    return false;

	xcb_delete_property(_conn, _selwin, _atomBuffer);
	xcb_convert_selection(_conn, _selwin, atomSel, _atomUTF8, _atomBuffer, XCB_CURRENT_TIME);

	xcb_flush(_conn);
	return true;
    }

    bool XCB::RootDisplayExt::selectionNotifyAction(xcb_selection_notify_event_t* ev)
    {
	if(! ev || ev->property == XCB_ATOM_NONE)
	    return false;

        // get type
        auto cookie1 = xcb_get_property(_conn, false, _selwin, _atomBuffer, XCB_GET_PROPERTY_TYPE_ANY, 0, 0);
	auto xcbReply1 = getReplyFunc(xcb_get_property, cookie1);

	if(xcbReply1.error())
	{
	    extendedError(xcbReply1.error(), "xcb_get_property");
	    return false;
	}

        if(! xcbReply1.reply())
            return false;

        if(xcbReply1.reply()->type != _atomUTF8 && xcbReply1.reply()->type != _atomText && xcbReply1.reply()->type != _atomTextPlain)
        {
	    auto type = getAtomName(xcbReply1.reply()->type);
            Application::error("xcb selection notify action, unknown type: %d, `%s'", xcbReply1.reply()->type, type.c_str());
            return false;
        }

        // get data
        auto cookie2 = xcb_get_property(_conn, false, _selwin, _atomBuffer, xcbReply1.reply()->type, 0, xcbReply1.reply()->bytes_after);
	auto xcbReply2 = getReplyFunc(xcb_get_property, cookie2);

	if(xcbReply2.error())
	{
	    extendedError(xcbReply1.error(), "xcb_get_property");
	    return false;
	}

        if(! xcbReply2.reply())
            return false;

	bool ret = false;
        auto ptrbuf = reinterpret_cast<const uint8_t*>(xcb_get_property_value(xcbReply2.reply().get()));
        int length = xcb_get_property_value_length(xcbReply2.reply().get());

	if(ptrbuf && 0 < length)
	{
	    // check equal
	    if(length == _selbuf.size() && std::equal(_selbuf.begin(), _selbuf.end(), ptrbuf))
		ret = false;
            else
	    {
		_selbuf.assign(ptrbuf, ptrbuf + length);
		ret = true;

                if(Application::isDebugLevel(DebugLevel::Console) ||
                    Application::isDebugLevel(DebugLevel::SyslogInfo))
		{
		    std::string log(_selbuf.begin(), _selbuf.end());
                    Application::info("xcb get selection, size: %d, content: `%s'", _selbuf.size(), log.c_str());
		}
	    }
	}

	if(auto errorReq = checkRequest(xcb_delete_property_checked(_conn, _selwin, _atomBuffer)))
	    extendedError(errorReq, "xcb_delete_property");

	xcb_flush(_conn);
	return ret;
    }

    XCB::GenericEvent XCB::RootDisplayExt::poolEvent(void)
    {
        GenericEvent ev = RootDisplay::poolEvent();
        auto response_type = ev ? ev->response_type & ~0x80 : 0;

        switch(response_type)
        {
            case XCB_SELECTION_CLEAR:
                selectionClearAction(reinterpret_cast<xcb_selection_clear_event_t*>(ev.get()));
                break;

            case XCB_SELECTION_REQUEST:
                selectionRequestAction(reinterpret_cast<xcb_selection_request_event_t*>(ev.get()));
                break;

            default:
                break;
        }

        return ev;
    }

    bool XCB::RootDisplayExt::isSelectionNotify(const GenericEvent & ev) const
    {
        auto response_type = ev ? ev->response_type & ~0x80 : 0;
        return response_type == XCB_SELECTION_NOTIFY;
    }
}
