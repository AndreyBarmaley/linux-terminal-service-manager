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

        size_t HasherKeyCodes::operator()(const KeyCodes & kc) const
        {
            if(kc.get())
            {
                size_t len = 0;

                while(*(kc.get() + len) != NULL_KEYCODE) len++;

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

        if(0 < shm) shmctl(shm, IPC_RMID, nullptr);
    }

    XCB::SHM::SHM(int shmid, uint8_t* addr, xcb_connection_t* conn)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_shm_attach_checked(conn, id, shmid, 0);

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
            error("xcb_shm_attach", err);
        else
            reset(new shm_t(shmid, addr, conn, id));
    }

    XCB::PixmapInfoReply XCB::SHM::getPixmapRegion(xcb_drawable_t winid, const Region & reg, size_t offset, uint32_t planeMask) const
    {
        auto xcbReply = getReplyFunc1(xcb_shm_get_image, connection(), winid, reg.x, reg.y, reg.width, reg.height,
                                      planeMask, XCB_IMAGE_FORMAT_Z_PIXMAP, xid(), offset);
        PixmapInfoReply res;

        if(auto err = xcbReply.error())
        {
            XCB::error("xcb_shm_get_image", err);
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
            error("xcb_create_gc", err);
        else
            reset(new gc_t(conn, id));
    }

    /* XCB::Damage */
    XCB::Damage::Damage(xcb_drawable_t win, int level, xcb_connection_t* conn)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_damage_create_checked(conn, id, win, static_cast<uint8_t>(level));

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
            error("xcb_damage_create", err);
        else
            reset(new damage_t(conn, id));
    }

    bool XCB::Damage::addRegion(xcb_drawable_t winid, xcb_xfixes_region_t regid) const
    {
        auto cookie = xcb_damage_add_checked(connection(), winid, regid);

        if(auto err = GenericError(xcb_request_check(connection(), cookie)))
        {
            error("xcb_damage_add", err);
            return false;
        }

        return true;
    }

    bool XCB::Damage::subtractRegion(xcb_xfixes_region_t repair, xcb_xfixes_region_t parts) const
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
            error("xcb_xfixes_create_region", err);
        else
            reset(new xfixes_region_t(conn, id));
    }

    XCB::XFixesRegion::XFixesRegion(const xcb_rectangle_t* rects, uint32_t count, xcb_connection_t* conn)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_xfixes_create_region_checked(conn, id, count, rects);

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
            error("xcb_xfixes_create_region", err);
        else
            reset(new xfixes_region_t(conn, id));
    }

    XCB::XFixesRegion::XFixesRegion(xcb_window_t win, xcb_shape_kind_t kind, xcb_connection_t* conn)
    {
        auto id = xcb_generate_id(conn);
        auto cookie = xcb_xfixes_create_region_from_window_checked(conn, id, win, kind);

        if(auto err = GenericError(xcb_request_check(conn, cookie)))
            error("xcb_xfixes_create_region", err);
        else
            reset(new xfixes_region_t(conn, id));
    }

    XCB::XFixesRegion XCB::XFixesRegion::intersect(xcb_xfixes_region_t reg2) const
    {
        auto id = xcb_generate_id(connection());
        auto cookie = xcb_xfixes_intersect_region_checked(connection(), xid(), reg2, id);
        XCB::XFixesRegion res;

        if(auto err = GenericError(xcb_request_check(connection(), cookie)))
            error("xcb_xfixes_intersect_region", err);
        else
            res.reset(new xfixes_region_t(connection(), id));

        return res;
    }

    XCB::XFixesRegion XCB::XFixesRegion::unionreg(xcb_xfixes_region_t reg2) const
    {
        auto id = xcb_generate_id(connection());
        auto cookie = xcb_xfixes_union_region_checked(connection(), xid(), reg2, id);
        XCB::XFixesRegion res;

        if(auto err = GenericError(xcb_request_check(connection(), cookie)))
            error("xcb_xfixes_union_region", err);
        else
            res.reset(new xfixes_region_t(connection(), id));

        return res;
    }

    bool XCB::KeyCodes::isValid(void) const
    {
        return get() && *get() != NULL_KEYCODE;
    }

    bool XCB::KeyCodes::operator==(const KeyCodes & kc) const
    {
        auto ptr1 = get();
        auto ptr2 = kc.get();

        if(! ptr1 || ! ptr2) return false;

        if(ptr1 == ptr2) return true;

        while(*ptr1 != NULL_KEYCODE && *ptr2 != NULL_KEYCODE)
        {
            if(*ptr1 != *ptr2) return false;

            ptr1++;
            ptr2++;
        }

        return *ptr1 == *ptr2;
    }

    /* XCB::Connector */
    XCB::Connector::Connector(std::string_view addr)
    {
        _conn = xcb_connect(addr.data(), nullptr);

        if(xcb_connection_has_error(_conn))
        {
            Application::error("%s: %s failed, addr: %s", __FUNCTION__, "xcb_connect", addr.data());
            throw xcb_error(NS_FuncName);
        }

        _setup = xcb_get_setup(_conn);

        if(! _setup)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xcb_get_setup");
            throw xcb_error(NS_FuncName);
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

    bool XCB::Connector::testConnection(std::string_view addr)
    {
        auto conn = xcb_connect(addr.data(), nullptr);
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

    XCB::SHM XCB::Connector::createSHM(size_t shmsz, int mode, bool readOnly)
    {
        int shmid = shmget(IPC_PRIVATE, shmsz, IPC_CREAT | mode);

        if(shmid == -1)
        {
            Application::error("shmget failed, size: %d, error: %s", shmsz, strerror(errno));
            return SHM();
        }

        auto shmaddr = reinterpret_cast<uint8_t*>(shmat(shmid, nullptr, (readOnly ? SHM_RDONLY : 0)));

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
        return XFixesRegion(rect, static_cast<uint32_t>(num), _conn);
    }

    size_t XCB::Connector::getMaxRequest(void) const
    {
        return xcb_get_maximum_request_length(_conn);
    }

    bool XCB::Connector::checkAtom(std::string_view name) const
    {
        return XCB_ATOM_NONE != getAtom(name, false);
    }

    xcb_atom_t XCB::Connector::getAtom(std::string_view name, bool create) const
    {
        auto xcbReply = getReplyFunc2(xcb_intern_atom, _conn, create ? 0 : 1, name.size(), name.data());

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_intern_atom");
            return XCB_ATOM_NONE;
        }

        return xcbReply.reply() ? xcbReply.reply()->atom : XCB_ATOM_NONE;
    }

    std::string XCB::Connector::getAtomName(xcb_atom_t atom) const
    {
        if(atom == XCB_ATOM_NONE)
            return std::string("NONE");

        auto xcbReply = getReplyFunc2(xcb_get_atom_name, _conn, atom);

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_get_atom_name");
            return "";
        }

        if(auto reply = xcbReply.reply())
        {
            const char* name = xcb_get_atom_name_name(reply.get());
            size_t len = xcb_get_atom_name_name_length(reply.get());
            return std::string(name, len);
        }

        return "";
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

    bool XCB::Connector::checkExtension(const Module & mod) const
    {
        if(mod == Module::TEST)
        {
            auto _test = xcb_get_extension_data(_conn, &xcb_test_id);

            if(! _test || ! _test->present)
                return false;

            auto xcbReply = getReplyFunc2(xcb_test_get_version, _conn, XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION);

            if(auto err = xcbReply.error())
            {
                extendedError(err, "xcb_test_query_version");
                return false;
            }

            if(auto reply = xcbReply.reply())
            {
                Application::debug("used %s extension, version: %d.%d", "TEST", reply->major_version, reply->minor_version);
                return true;
            }
        }
        else if(mod == Module::DAMAGE)
        {
            auto _damage = xcb_get_extension_data(_conn, &xcb_damage_id);

            if(! _damage || ! _damage->present)
                return false;

            auto xcbReply = getReplyFunc2(xcb_damage_query_version, _conn, XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION);

            if(auto err = xcbReply.error())
            {
                extendedError(err, "xcb_damage_query_version");
                return false;
            }

            if(auto reply = xcbReply.reply())
            {
                Application::debug("used %s extension, version: %d.%d", "DAMAGE", reply->major_version, reply->minor_version);
                return true;
            }
        }
        else if(mod == Module::XFIXES)
        {
            auto _xfixes = xcb_get_extension_data(_conn, &xcb_xfixes_id);

            if(! _xfixes || ! _xfixes->present)
                return false;

            auto xcbReply = getReplyFunc2(xcb_xfixes_query_version, _conn, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);

            if(auto err = xcbReply.error())
            {
                extendedError(err, "xcb_xfixes_query_version");
                return false;
            }

            if(auto reply = xcbReply.reply())
            {
                Application::debug("used %s extension, version: %d.%d", "XFIXES", reply->major_version, reply->minor_version);
                return true;
            }
        }
        else if(mod == Module::RANDR)
        {
            auto _randr = xcb_get_extension_data(_conn, &xcb_randr_id);

            if(! _randr || ! _randr->present)
                return false;

            auto xcbReply = getReplyFunc2(xcb_randr_query_version, _conn, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);

            if(auto err = xcbReply.error())
            {
                extendedError(err, "xcb_randr_query_version");
                return false;
            }

            if(auto reply = xcbReply.reply())
            {
                Application::debug("used %s extension, version: %d.%d", "RANDR", reply->major_version, reply->minor_version);
                return true;
            }
        }
        else if(mod == Module::SHM)
        {
            auto _shm = xcb_get_extension_data(_conn, &xcb_shm_id);

            if(! _shm || ! _shm->present)
                return false;

            auto xcbReply = getReplyFunc2(xcb_shm_query_version, _conn);

            if(auto err = xcbReply.error())
            {
                extendedError(err, "xcb_shm_query_version");
                return false;
            }

            if(auto reply = xcbReply.reply())
            {
                Application::debug("used %s extension, version: %d.%d", "SHM", reply->major_version, reply->minor_version);
                return true;
            }
        }
        else if(mod == Module::XKB)
        {
            auto _xkb = xcb_get_extension_data(_conn, &xcb_xkb_id);

            if(! _xkb)
                return false;

            auto xcbReply = getReplyFunc2(xcb_xkb_use_extension, _conn, XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION);

            if(auto err = xcbReply.error())
            {
                extendedError(err, "xcb_xkb_use_extension");
                return false;
            }

            if(auto reply = xcbReply.reply())
            {
                Application::debug("used %s extension, version: %d.%d", "XKB", reply->serverMajor, reply->serverMinor);
                return true;
            }
        }

        return false;
    }

    int XCB::Connector::eventNotify(const GenericEvent & ev, const Module & mod) const
    {
        // clear bit
        auto response_type = ev ? ev->response_type & ~0x80 : 0;

        if(0 < response_type)
        {
            if(mod == Module::DAMAGE)
            {
                // for receive it, usage:
                // RootDisplay::createDamageNotify
                auto _damage = xcb_get_extension_data(_conn, &xcb_damage_id);

                if(_damage && response_type == _damage->first_event + XCB_DAMAGE_NOTIFY)
                    return XCB_DAMAGE_NOTIFY;
            }
            else if(mod == Module::XFIXES)
            {
                // for receive it, usage input filter:
                // xcb_xfixes_select_selection_input(xcb_xfixes_selection_event_mask_t)
                // xcb_xfixes_select_cursor_input(xcb_xfixes_cursor_notify_mask_t)
                //
                auto _xfixes = xcb_get_extension_data(_conn, &xcb_xfixes_id);
                auto types = { XCB_XFIXES_SELECTION_NOTIFY, XCB_XFIXES_CURSOR_NOTIFY };

                for(auto & type : types)
                    if(_xfixes && response_type == _xfixes->first_event + type)
                        return type;
            }
            else if(mod == Module::RANDR)
            {
                // for receive it, usage input filter:
                // xcb_xrandr_select_input(xcb_randr_notify_mask_t)
                //
                auto _randr = xcb_get_extension_data(_conn, &xcb_randr_id);
                auto types = { XCB_RANDR_SCREEN_CHANGE_NOTIFY, XCB_RANDR_NOTIFY };

                for(auto & type : types)
                    if(_randr && response_type == _randr->first_event + type) return type;
            }
            else if(mod == Module::XKB)
            {
                // for receive it, usage input filter:
                // xcb_xkb_select_events(xcb_xkb_event_type_t)
                auto _xkb = xcb_get_extension_data(_conn, &xcb_xkb_id);

                if(_xkb && response_type == _xkb->first_event)
                {
                    auto types = { XCB_XKB_NEW_KEYBOARD_NOTIFY, XCB_XKB_MAP_NOTIFY, XCB_XKB_STATE_NOTIFY, XCB_XKB_CONTROLS_NOTIFY, XCB_XKB_INDICATOR_STATE_NOTIFY,
                                   XCB_XKB_INDICATOR_MAP_NOTIFY, XCB_XKB_NAMES_NOTIFY, XCB_XKB_COMPAT_MAP_NOTIFY, XCB_XKB_BELL_NOTIFY, XCB_XKB_ACTION_MESSAGE, XCB_XKB_ACCESS_X_NOTIFY, XCB_XKB_EXTENSION_DEVICE_NOTIFY
                                 };
                    auto xn = reinterpret_cast<xkb_notify_event_t*>(ev.get());

                    for(auto & type : types)
                        if(xn->any.xkb_type == type) return type;
                }
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

    bool XCB::Connector::isXkbStateNotify(const GenericEvent & ev) const
    {
        return XCB_XKB_STATE_NOTIFY == eventNotify(ev, Module::XKB);
    }

    bool XCB::Connector::isXkbKeyboardNotify(const GenericEvent & ev) const
    {
        return XCB_XKB_NEW_KEYBOARD_NOTIFY == eventNotify(ev, Module::XKB);
    }

    bool XCB::Connector::isXkbMapNotify(const GenericEvent & ev) const
    {
        return XCB_XKB_MAP_NOTIFY == eventNotify(ev, Module::XKB);
    }

    int XCB::Connector::eventErrorOpcode(const GenericEvent & ev, const Module & mod) const
    {
        if(ev && ev->response_type == 0)
        {
            auto error = ev.toerror();

            if(mod == Module::TEST)
            {
                auto _test = xcb_get_extension_data(_conn, &xcb_test_id);

                if(_test && error->major_code == _test->major_opcode)
                    return error->minor_code;
            }
            else if(mod == Module::DAMAGE)
            {
                auto _damage = xcb_get_extension_data(_conn, &xcb_damage_id);

                if(_damage && error->major_code == _damage->major_opcode)
                    return error->minor_code;
            }
            else if(mod == Module::XFIXES)
            {
                auto _xfixes = xcb_get_extension_data(_conn, &xcb_xfixes_id);

                if(_xfixes && error->major_code == _xfixes->major_opcode)
                    return error->minor_code;
            }
            else if(mod == Module::RANDR)
            {
                auto _randr = xcb_get_extension_data(_conn, &xcb_randr_id);

                if(_randr && error->major_code == _randr->major_opcode)
                    return error->minor_code;
            }
            else if(mod == Module::SHM)
            {
                auto _shm = xcb_get_extension_data(_conn, &xcb_shm_id);

                if(_shm && error->major_code == _shm->major_opcode)
                    return error->minor_code;
            }
            else if(mod == Module::XKB)
            {
                auto _xkb = xcb_get_extension_data(_conn, &xcb_xkb_id);

                if(_xkb && error->major_code == _xkb->major_opcode)
                    return error->minor_code;
            }
        }

        return -1;
    }

    XCB::PropertyReply XCB::Connector::getPropertyAnyType(xcb_window_t win, xcb_atom_t prop, uint32_t offset, uint32_t length)
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn, false, win, prop, XCB_GET_PROPERTY_TYPE_ANY, offset, length);

        if(xcbReply.error())
            extendedError(xcbReply.error(), "xcb_get_property");

        return xcbReply.reply();
    }

    xcb_atom_t XCB::Connector::getPropertyType(xcb_window_t win, xcb_atom_t prop)
    {
        auto reply = getPropertyAnyType(win, prop, 0, 0);
        return reply ? reply->type : XCB_ATOM_NONE;
    }

    xcb_atom_t XCB::Connector::getPropertyAtom(xcb_window_t win, xcb_atom_t prop, uint32_t offset)
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn, false, win, prop, XCB_ATOM_ATOM, offset, 1);

        if(xcbReply.error())
            extendedError(xcbReply.error(), "xcb_get_property");
        else if(auto reply = xcbReply.reply())
        {
            if(auto res = static_cast<xcb_atom_t*>(xcb_get_property_value(reply.get())))
                return *res;
        }

        return XCB_ATOM_NONE;
    }

    xcb_window_t XCB::Connector::getPropertyWindow(xcb_window_t win, xcb_atom_t prop, uint32_t offset)
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn, false, win, prop, XCB_ATOM_WINDOW, offset, 1);

        if(xcbReply.error())
            extendedError(xcbReply.error(), "xcb_get_property");
        else if(auto reply = xcbReply.reply())
        {
            if(auto res = static_cast<xcb_window_t*>(xcb_get_property_value(reply.get())))
                return *res;
        }

        return XCB_WINDOW_NONE;
    }

    std::string XCB::Connector::getPropertyString(xcb_window_t win, xcb_atom_t prop, uint32_t offset)
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn, false, win, prop, XCB_ATOM_STRING, offset, ~0);
        std::string res;

        if(xcbReply.error())
            extendedError(xcbReply.error(), "xcb_get_property");
        else if(auto reply = xcbReply.reply())
        {
            auto ptr = static_cast<const char*>(xcb_get_property_value(reply.get()));

            if(ptr) res.assign(ptr);
        }

        return res;
    }

    std::list<std::string> XCB::Connector::getPropertyStringList(xcb_window_t win, xcb_atom_t prop)
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn, false, win, prop, XCB_ATOM_STRING, 0, ~0);
        std::list<std::string> res;

        if(xcbReply.error())
            extendedError(xcbReply.error(), "xcb_get_property");
        else if(auto reply = xcbReply.reply())
        {
            int len = xcb_get_property_value_length(reply.get());
            auto ptr = static_cast<const char*>(xcb_get_property_value(reply.get()));
            res = Tools::split(std::string(ptr, ptr + len - (ptr[len - 1] ? 0 : 1 /* remove last nul */)), 0);
        }

        return res;
    }

    std::vector<uint8_t> XCB::Connector::getPropertyBinary(xcb_window_t win, xcb_atom_t prop, xcb_atom_t type)
    {
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn, false, win, prop, type, 0, ~0);
        std::vector<uint8_t> res;

        if(xcbReply.error())
            extendedError(xcbReply.error(), "xcb_get_property");
        else if(auto reply = xcbReply.reply())
        {
            int len = xcb_get_property_value_length(reply.get());
            auto ptr = static_cast<const uint8_t*>(xcb_get_property_value(reply.get()));
            res.assign(ptr, ptr + len);
        }

        return res;
    }

    /* XCB::RootDisplay */
    XCB::RootDisplay::RootDisplay(std::string_view addr) : Connector(addr)
        , _xkbctx { nullptr, xkb_context_unref }, _xkbmap { nullptr, xkb_keymap_unref }, _xkbstate { nullptr, xkb_state_unref }
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

        // check extensions
        if(! checkExtension(Module::SHM))
        {
            Application::error("%s: %s failed", __FUNCTION__, "SHM extension");
            throw xcb_error(NS_FuncName);
        }

        if(! checkExtension(Module::DAMAGE))
        {
            Application::error("%s: %s failed", __FUNCTION__, "DAMAGE extension");
            throw xcb_error(NS_FuncName);
        }

        if(! checkExtension(Module::XFIXES))
        {
            Application::error("%s: %s failed", __FUNCTION__, "XFIXES extension");
            throw xcb_error(NS_FuncName);
        }

        if(! checkExtension(Module::TEST))
        {
            Application::error("%s: %s failed", __FUNCTION__, "TEST extension");
            throw xcb_error(NS_FuncName);
        }

        if(! checkExtension(Module::RANDR))
        {
            Application::error("%s: %s failed", __FUNCTION__, "RANDR extension");
            throw xcb_error(NS_FuncName);
        }

        if(! checkExtension(Module::XKB))
        {
            Application::error("%s: %s failed", __FUNCTION__, "XKB extension");
            throw xcb_error(NS_FuncName);
        }

        /*
        	uint8_t _xkbevent;
        	if(0 == xkb_x11_setup_xkb_extension(_conn, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
        			    XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, & _xkbevent, nullptr))
        	{
                    Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_setup_xkb_extension");
        	}
        */

        _xkbdevid = xkb_x11_get_core_keyboard_device_id(_conn);

        if(_xkbdevid < 0)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_get_core_keyboard_device_id");
            throw xcb_error(NS_FuncName);
        }

        _xkbctx.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));

        if(! _xkbctx)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xkb_context_new");
            throw xcb_error(NS_FuncName);
        }

        _xkbmap.reset(xkb_x11_keymap_new_from_device(_xkbctx.get(), _conn, _xkbdevid, XKB_KEYMAP_COMPILE_NO_FLAGS));

        if(!_xkbmap)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_keymap_new_from_device");
            throw xcb_error(NS_FuncName);
        }

        _xkbstate.reset(xkb_x11_state_new_from_device(_xkbmap.get(), _conn, _xkbdevid));

        if(!_xkbstate)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_state_new_from_device");
            throw xcb_error(NS_FuncName);
        }

        uint16_t required_map_parts = (XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS | XCB_XKB_MAP_PART_MODIFIER_MAP |
                                       XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS | XCB_XKB_MAP_PART_KEY_ACTIONS | XCB_XKB_MAP_PART_VIRTUAL_MODS | XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP);
        uint16_t required_events = (XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_STATE_NOTIFY);

        auto cookie = xcb_xkb_select_events_checked(_conn, _xkbdevid, required_events, 0, required_events, required_map_parts, required_map_parts, nullptr);

        if(auto errorReq = checkRequest(cookie))
            extendedError(errorReq, "xcb_xkb_select_events");

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
        const size_t bpp = bitsPerPixel() >> 3;
        const size_t pagesz = 4096;
        const size_t shmsz = ((wsz.width* wsz.height* bpp / pagesz) + 1) * pagesz;

        _shm = createSHM(shmsz, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, false);
    }

    XCB::RootDisplay::~RootDisplay()
    {
        resetInputs();
        xcb_flush(_conn);
    }

    void XCB::RootDisplay::resetInputs(void)
    {
        // release all buttons
        for(uint8_t button = 1; button <= 5; button++)
            xcb_test_fake_input(_conn, XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, _screen->root, 0, 0, 0);

        // release all keys
        for(int key = _minKeycode;  key <= _maxKeycode; key++)
            xcb_test_fake_input(_conn, XCB_KEY_RELEASE, key, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);

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
        auto xcbReply = getReplyFunc2(xcb_get_geometry, _conn, _screen->root);

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

    XCB::PixmapInfoReply XCB::RootDisplay::copyRootImageRegion(const Region & reg, uint32_t planeMask) const
    {
        if(_shm)
            return _shm.getPixmapRegion(_screen->root, reg, 0, planeMask);

        size_t pitch = reg.width * (bitsPerPixel() >> 2);
        auto maxReqLength = xcb_get_maximum_request_length(_conn);
        PixmapInfoReply res;

        if(pitch == 0)
        {
            Application::error("copy root image error, empty size: [%d,%d], bpp: %d", reg.width, reg.height, bitsPerPixel());
            return res;
        }

        uint16_t allowRows = std::min(static_cast<uint16_t>(maxReqLength / pitch), reg.height);

        for(int16_t yy = reg.y; yy < reg.y + reg.height; yy += allowRows)
        {
            // last rows
            if(yy + allowRows > reg.y + reg.height)
                allowRows = reg.y + reg.height - yy;

            auto xcbReply = getReplyFunc2(xcb_get_image, _conn, XCB_IMAGE_FORMAT_Z_PIXMAP, _screen->root, reg.x, yy, reg.width, allowRows, planeMask);

            if(auto err = xcbReply.error())
            {
                extendedError(err, "xcb_get_image");
                break;
            }

            if(auto reply = xcbReply.reply())
            {
                if(! res)
                    res.reset(new PixmapInfoBuffer(reply->depth, reply->visual, reg.height * pitch));

                auto info = static_cast<PixmapInfoBuffer*>(res.get());
                auto length = xcb_get_image_data_length(reply.get());
                auto data = xcb_get_image_data(reply.get());
                info->_pixels.insert(info->_pixels.end(), data, data + length);
            }
        }

        return res;
    }

    bool XCB::RootDisplay::damageAdd(const xcb_rectangle_t* rects, size_t counts)
    {
        if(_damage && rects && counts)
        {
            auto fixreg = XFixesRegion(rects, static_cast<uint32_t>(counts), _conn);
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

    std::vector<xcb_randr_output_t> XCB::RootDisplay::getRandrOutputs(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_resources, _conn, _screen->root);
        std::vector<xcb_randr_output_t> res;

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_randr_get_screen_resources");
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

    XCB::RandrOutputInfo XCB::RootDisplay::getRandrOutputInfo(const xcb_randr_output_t & id) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_output_info, _conn, id, XCB_CURRENT_TIME);
        RandrOutputInfo res;

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_randr_get_output_info");
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

    std::vector<xcb_randr_mode_t> XCB::RootDisplay::getRandrOutputModes(const xcb_randr_output_t & id) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_output_info, _conn, id, XCB_CURRENT_TIME);
        std::vector<xcb_randr_mode_t> res;

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_randr_get_output_info");
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

    std::vector<xcb_randr_mode_info_t> XCB::RootDisplay::getRandrModesInfo(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_resources, _conn, _screen->root);
        std::vector<xcb_randr_mode_info_t> res;

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_randr_get_screen_resources");
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

    std::vector<xcb_randr_crtc_t> XCB::RootDisplay::getRandrOutputCrtcs(const xcb_randr_output_t & id) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_output_info, _conn, id, XCB_CURRENT_TIME);
        std::vector<xcb_randr_mode_t> res;

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_randr_get_output_info");
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

    XCB::RandrScreenInfo XCB::RootDisplay::getRandrScreenInfo(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_info, _conn, _screen->root);
        XCB::RandrScreenInfo res;

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_randr_get_screen_info");
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

    std::vector<xcb_randr_screen_size_t> XCB::RootDisplay::getRandrScreenSizes(RandrScreenInfo* info) const
    {
        auto xcbReply = getReplyFunc2(xcb_randr_get_screen_info, _conn, _screen->root);
        std::vector<xcb_randr_screen_size_t> res;

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_randr_get_screen_info");
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

    xcb_randr_mode_t XCB::RootDisplay::createRandrMode(uint16_t width, uint16_t height)
    {
        auto cvt = "/usr/bin/cvt";

        if(! std::filesystem::exists(cvt))
        {
            Application::error("%s: utility not found: %s", __FUNCTION__, cvt);
            return 0;
        }

        std::string cmd = Tools::runcmd(Tools::StringFormat("%1 %2 %3").arg(cvt).arg(width).arg(height));
        auto params = Tools::split(cmd.substr(cmd.find('\n', 0) + 1), 0x20);
        params.remove_if([](auto & val)
        {
            return val.empty();
        });

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
        auto xcbReply = getReplyFunc2(xcb_randr_create_mode, _conn, _screen->root, mode_info, name.size(), name.data());

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_randr_create_mode");
            return 0;
        }

        if(auto reply = xcbReply.reply())
        {
            Application::debug("%s: id: %08x, width: %d, height: %d", __FUNCTION__, reply->mode, mode_info.width, mode_info.height);
            return reply->mode;
        }

        return 0;
    }

    bool XCB::RootDisplay::addRandrOutputMode(const xcb_randr_output_t & output, const xcb_randr_mode_t  & mode)
    {
        auto cookie = xcb_randr_add_output_mode_checked(_conn, output, mode);

        if(auto errorReq = checkRequest(cookie))
        {
            extendedError(errorReq, "xcb_randr_add_output_mode");
            return false;
        }

        Application::debug("%s: id: %08x, output: %08x", __FUNCTION__, mode, output);
        return true;
    }

    bool XCB::RootDisplay::destroyRandrMode(const xcb_randr_mode_t & mode)
    {
        auto cookie = xcb_randr_destroy_mode_checked(_conn, mode);

        if(auto errorReq = checkRequest(cookie))
        {
            extendedError(errorReq, "xcb_randr_destroy_mode_checked");
            return false;
        }

        Application::debug("%s: id: %08x", __FUNCTION__, mode);
        return true;
    }

    bool XCB::RootDisplay::deleteRandrOutputMode(const xcb_randr_output_t & output, const xcb_randr_mode_t & mode)
    {
        auto cookie = xcb_randr_delete_output_mode_checked(_conn, output, mode);

        if(auto errorReq = checkRequest(cookie))
        {
            extendedError(errorReq, "xcb_randr_delete_output_mode_checked");
            return false;
        }

        Application::debug("%s: id: %08x, output: %08x", __FUNCTION__, mode, output);
        return true;
    }

    bool XCB::RootDisplay::setRandrScreenSize(uint16_t width, uint16_t height, uint16_t* sequence)
    {
        // align size
        if(auto alignW = width % 8)
            width += 8 - alignW;

        auto screenSizes = getRandrScreenSizes();
        auto its = std::find_if(screenSizes.begin(), screenSizes.end(), [=](auto & ss)
        {
            return ss.width == width && ss.height == height;
        });
        xcb_randr_mode_t mode = 0;
        xcb_randr_output_t output = 0;

        // not found
        if(its == screenSizes.end())
        {
            // add new mode
            auto outputs = getRandrOutputs();
            auto ito = std::find_if(outputs.begin(), outputs.end(), [this](auto & id)
            {
                return this->getRandrOutputInfo(id).connected;
            });

            if(ito == outputs.end())
            {
                Application::error("%s: connected output not found, outputs count: %d", __FUNCTION__, outputs.size());
                return false;
            }

            output = *ito;
            mode = createRandrMode(width, height);

            if(0 == mode)
                return false;

            if(! addRandrOutputMode(output, mode))
            {
                Application::error("%s: runtime error, modes: [%d, %d]", __FUNCTION__, width, height);
                destroyRandrMode(mode);
                return false;
            }

            // fixed size
            auto modes = getRandrModesInfo();
            auto itm = std::find_if(modes.begin(), modes.end(), [=](auto & val)
            {
                return val.id == mode;
            });

            if(itm == modes.end())
            {
                Application::error("%s: runtime error, modes: [%d, %d]", __FUNCTION__, width, height);
                deleteRandrOutputMode(output, mode);
                destroyRandrMode(mode);
                return false;
            }

            width = (*itm).width;
            height = (*itm).height;
            // rescan info
            screenSizes = getRandrScreenSizes();
            its = std::find_if(screenSizes.begin(), screenSizes.end(), [=](auto & ss)
            {
                return ss.width == width && ss.height == height;
            });

            if(its == screenSizes.end())
            {
                Application::error("%s: runtime error, modes: [%d, %d]", __FUNCTION__, width, height);
                deleteRandrOutputMode(output, mode);
                destroyRandrMode(mode);
                return false;
            }
        }

        auto sizeID = std::distance(screenSizes.begin(), its);
        auto screenInfo = getRandrScreenInfo();
        // clear old damages
        damageSubtrack(region());
        auto xcbReply2 = getReplyFunc2(xcb_randr_set_screen_config, _conn, _screen->root, XCB_CURRENT_TIME,
                                       screenInfo.config_timestamp, sizeID, screenInfo.rotation /*XCB_RANDR_ROTATION_ROTATE_0*/, 0);

        if(auto err = xcbReply2.error())
        {
            extendedError(err, "xcb_randr_set_screen_config");
            return false;
        }

        if(sequence)
        {
            *sequence = xcbReply2.reply()->sequence;
            Application::info("%s: sequence: 0x%04x", __FUNCTION__, *sequence);
        }

        auto nsz = size();
        if(nsz != Size(width, height))
        {
            Application::error("%s: failed, size: [%d, %d], id: %d", __FUNCTION__, width, height, sizeID);
            damageAdd({0, 0, nsz.width, nsz.height});
            //deleteRandrOutputMode(output, mode);
            //destroyRandrMode(mode);
            return false;
        }

        Application::info("%s: set size: [%d, %d], id: %d", __FUNCTION__, width, height, sizeID);
        return true;
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
                Application::warning("damage notify: [%d,%d,%d,%d] discard", notify->area.x, notify->area.y, notify->area.width, notify->area.height);
                xcb_discard_reply(_conn, notify->sequence);
                return GenericEvent();
            }
        }
        else if(isRandrCRTCNotify(ev))
        {
            auto notify = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
            xcb_randr_crtc_change_t cc = notify->u.cc;

            if(0 < cc.width && 0 < cc.height)
            {
                auto wsz = size();

                if(cc.width != wsz.width || cc.height != wsz.height)
                {
                    Application::warning("xcb crc change notify: [%d,%d] discard, current: [%d,%d]", cc.width, cc.height, wsz.width, wsz.height);
                    xcb_discard_reply(_conn, notify->sequence);
                    return GenericEvent();
                }

                Application::debug("xcb crc change notify: [%d,%d]", cc.width, cc.height);
                // init shm
                const size_t bpp = bitsPerPixel() >> 3;
                const size_t pagesz = 4096;
                const size_t shmsz = ((cc.width * cc.height * bpp / pagesz) + 1) * pagesz;
                _shm = createSHM(shmsz, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, false);
                // create damage notify
                _damage = Damage(_screen->root, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES, _conn);
                damageAdd({0, 0, cc.width, cc.height});
            }
        }

        int xkbev = eventNotify(ev, Module::XKB);
        bool resetMapState = false;

        if(XCB_XKB_MAP_NOTIFY == xkbev)
        {
            auto mn = reinterpret_cast<xcb_xkb_map_notify_event_t*>(ev.get());
            Application::debug("%s: xkb notify: %s, min keycode: %d, max keycode: %d, changed: 0x%04x", __FUNCTION__, "map",
                               mn->minKeyCode, mn->maxKeyCode, mn->changed);
            resetMapState = true;
            _minKeycode = mn->minKeyCode;
            _maxKeycode = mn->maxKeyCode;
        }
        else if(XCB_XKB_NEW_KEYBOARD_NOTIFY == xkbev)
        {
            auto kn = reinterpret_cast<xcb_xkb_new_keyboard_notify_event_t*>(ev.get());
            Application::debug("%s: xkb notify: %s, devid: %d, old devid: %d, min keycode: %d, max keycode: %d, changed: 0x%04x", __FUNCTION__, "new keyboard",
                               kn->deviceID, kn->oldDeviceID, kn->minKeyCode, kn->maxKeyCode, kn->changed);

            if(kn->deviceID == _xkbdevid && (kn->changed & XCB_XKB_NKN_DETAIL_KEYCODES))
            {
                resetMapState = true;
                _minKeycode = kn->minKeyCode;
                _maxKeycode = kn->maxKeyCode;
            }
        }
        else if(xkbev == XCB_XKB_STATE_NOTIFY)
        {
            Application::debug("%s: xkb notify: %s", __FUNCTION__, "state");
            auto sn = reinterpret_cast<xcb_xkb_state_notify_event_t*>(ev.get());
            xkb_state_update_mask(_xkbstate.get(), sn->baseMods, sn->latchedMods, sn->lockedMods,
                                  sn->baseGroup, sn->latchedGroup, sn->lockedGroup);

            if(sn->changed & XCB_XKB_STATE_PART_GROUP_STATE)
            {
                // changed layout group
                // sn->group
            }
        }

        if(resetMapState)
        {
            // free state first
            _xkbstate.reset();
            _xkbmap.reset();
            // set new
            _xkbmap.reset(xkb_x11_keymap_new_from_device(_xkbctx.get(), _conn, _xkbdevid, XKB_KEYMAP_COMPILE_NO_FLAGS));

            if(_xkbmap)
            {
                _xkbstate.reset(xkb_x11_state_new_from_device(_xkbmap.get(), _conn, _xkbdevid));

                if(_xkbstate)
                    Application::debug("%s: keyboard updated, device id: %d", __FUNCTION__, _xkbdevid);
                else
                    Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_state_new_from_device");
            }
            else
                Application::error("%s: %s failed", __FUNCTION__, "xkb_x11_keymap_new_from_device");
        }

        return ev;
    }

    std::vector<std::string> XCB::RootDisplay::getXkbNames(void) const
    {
        std::vector<std::string> res;
        res.reserve(4);

        auto xcbReply = getReplyFunc2(xcb_xkb_get_names, _conn, XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_NAME_DETAIL_GROUP_NAMES | XCB_XKB_NAME_DETAIL_SYMBOLS);
        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_xkb_get_names");
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
                res.emplace_back(getAtomName(list.groups[ii]));
        }

        return res;
    }

    int XCB::RootDisplay::getXkbLayoutGroup(void) const
    {
        auto xcbReply = getReplyFunc2(xcb_xkb_get_state, _conn, XCB_XKB_ID_USE_CORE_KBD);
        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_xkb_get_names");
            throw xcb_error(NS_FuncName);
        }

        if(auto reply = xcbReply.reply())
            return reply->group;

        return -1;
    }

    bool XCB::RootDisplay::switchXkbLayoutGroup(int group) const
    {
        // next
        if(group < 0)
        {
            auto names = getXkbNames();

            if(2 > names.size())
                return false;

            group = (getXkbLayoutGroup() + 1) % names.size();
        }

        auto cookie = xcb_xkb_latch_lock_state_checked(_conn, XCB_XKB_ID_USE_CORE_KBD, 0, 0, 1, group, 0, 0, 0);

        if(auto errorReq = checkRequest(cookie))
        {
            extendedError(errorReq, "xcb_xkb_latch_lock_state");
            return false;
        }

        return true;
    }

    xcb_keycode_t XCB::RootDisplay::keysymToKeycode(xcb_keysym_t keysym) const
    {
        return keysymGroupToKeycode(keysym, getXkbLayoutGroup());
    }

    std::pair<xcb_keycode_t, int> XCB::RootDisplay::keysymToKeycodeGroup(xcb_keysym_t keysym) const
    {
        auto empty = std::make_pair<xcb_keycode_t, int>(NULL_KEYCODE, -1);
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, _conn, _minKeycode, _maxKeycode - _minKeycode + 1);

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_get_keyboard_mapping");
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

        Application::warning("%s: keysym not found 0x%08x, group names: [%s]", __FUNCTION__, keysym, Tools::join(getXkbNames(), ",").c_str());
        return empty;
    }

    xcb_keycode_t XCB::RootDisplay::keysymGroupToKeycode(xcb_keysym_t keysym, int group) const
    {
        auto xcbReply = getReplyFunc2(xcb_get_keyboard_mapping, _conn, _minKeycode, _maxKeycode - _minKeycode + 1);

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_get_keyboard_mapping");
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
        Application::debug("%s: keysym: 0x%08x, current group: %d, keysym per keycode: %d, keysyms counts: %d, keycodes count: %d, group names: [%s]",
                           __FUNCTION__, keysym, group, keysymsPerKeycode, keysymsLength, keycodesCount, Tools::join(getXkbNames(), ",").c_str());

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

    void XCB::RootDisplay::fakeInputKeycode(xcb_keycode_t keycode, bool pressed)
    {
        fakeInputTest(pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, 0, 0);
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

                switchXkbLayoutGroup(keysymGroup);
                keycode = keysymKeycode;
            }
        }

        if(keycode != NULL_KEYCODE)
            fakeInputKeycode(keycode, pressed);
    }

    void XCB::RootDisplay::fakeInputButton(int button, const Point & pos)
    {
        xcb_test_fake_input(_conn, XCB_BUTTON_PRESS, static_cast<uint8_t>(button), XCB_CURRENT_TIME, _screen->root, pos.x, pos.y, 0);
        xcb_test_fake_input(_conn, XCB_BUTTON_RELEASE, static_cast<uint8_t>(button), XCB_CURRENT_TIME, _screen->root, pos.x, pos.y, 0);
        xcb_flush(_conn);
    }

    /// @param type: XCB_KEY_PRESS, XCB_KEY_RELEASE, XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE, XCB_MOTION_NOTIFY
    /// @param detail: keycode or left 1, middle 2, right 3, scrollup 4, scrolldw 5
    bool XCB::RootDisplay::fakeInputTest(int type, int detail, int posx, int posy)
    {
        auto cookie = xcb_test_fake_input_checked(_conn, static_cast<uint8_t>(type), static_cast<uint8_t>(detail), XCB_CURRENT_TIME, _screen->root,
                      static_cast<int16_t>(posx), static_cast<int16_t>(posy), 0);
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
        auto cookie = xcb_change_window_attributes(_conn, _screen->root, XCB_CW_BACK_PIXEL, colors);
        auto errorReq = checkRequest(cookie);

        if(errorReq)
            extendedError(errorReq, "xcb_change_window_attributes");
        else
        {
            cookie = xcb_clear_area_checked(_conn, 0, _screen->root, reg.x, reg.y, reg.width, reg.height);
            errorReq = checkRequest(cookie);

            if(errorReq)
                extendedError(errorReq, "xcb_clear_area");
        }
    }

    /* XCB::RootDisplayExt */
    XCB::RootDisplayExt::RootDisplayExt(std::string_view addr) : RootDisplay(addr),
        _atomPrimary(_atoms[0]), _atomClipboard(_atoms[1]), _atomBuffer(_atoms[2]),
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
            // set _wm_name
            const std::string name("LTSM_SEL");
            xcb_change_property(_conn, XCB_PROP_MODE_REPLACE, _selwin,
                                getAtom("_NET_WM_NAME"), _atomUTF8, 8, name.size(), name.data());
            xcb_flush(_conn);
            // run periodic timer (555ms)
            _timerClipCheck = Tools::BaseTimer::create<std::chrono::milliseconds>(555, true,
                              [this]()
            {
                this->getSelectionEvent(this->_atomPrimary);
                this->getSelectionEvent(this->_atomClipboard);
            });
        }
        else
        {
            extendedError(errorReq, "xcb_create_window");
            _selwin = 0;
        }
    }

    XCB::RootDisplayExt::~RootDisplayExt()
    {
        _timerClipCheck->stop(true);

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
                Application::debug("%s: %s", __FUNCTION__, "event");

            return true;
        }

        return false;
    }

    xcb_window_t XCB::RootDisplayExt::getOwnerSelection(const xcb_atom_t & atomSel)
    {
        auto xcbReply = getReplyFunc2(xcb_get_selection_owner, _conn, atomSel);

        if(auto err = xcbReply.error())
        {
            extendedError(err, "xcb_get_selection_owner");
            return XCB_WINDOW_NONE;
        }

        return xcbReply.reply()  ? xcbReply.reply()->owner : XCB_WINDOW_NONE;
    }

    bool XCB::RootDisplayExt::sendNotifyTargets(const xcb_selection_request_event_t & ev)
    {
        auto cookie = xcb_change_property_checked(_conn, XCB_PROP_MODE_REPLACE, ev.requestor, ev.property,
                      XCB_ATOM, 8 * sizeof(xcb_atom_t), 4, & _atomTargets); /* send list: _atomTargets, _atomText, _atomTextPlain, _atomUTF8 */

        if(auto errorReq = checkRequest(cookie))
        {
            extendedError(errorReq, "xcb_change_property");
            return false;
        }

        xcb_selection_notify_event_t notify = { .response_type = XCB_SELECTION_NOTIFY, .pad0 = 0, .sequence = 0,
                                                .time = ev.time, .requestor = ev.requestor, .selection = ev.selection, .target = ev.target, .property = ev.property
                                              };
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

        if(auto errorReq = checkRequest(cookie))
        {
            extendedError(errorReq, "xcb_change_property");
            return false;
        }

        xcb_selection_notify_event_t notify = { .response_type = XCB_SELECTION_NOTIFY, .pad0 = 0, .sequence = 0,
                                                .time = ev.time, .requestor = ev.requestor, .selection = ev.selection, .target = ev.target, .property = ev.property
                                              };
        xcb_send_event(_conn, 0, ev.requestor, XCB_EVENT_MASK_NO_EVENT, (const char*) & notify);
        xcb_flush(_conn);

        if(Application::isDebugLevel(DebugLevel::Console) ||
           Application::isDebugLevel(DebugLevel::SyslogInfo))
        {
            auto prop = getAtomName(ev.property);
            std::string log(_selbuf.begin(), _selbuf.end());
            Application::debug("xcb put selection, size: %d, content: `%s', to window: 0x%08x, property: `%s'", _selbuf.size(), log.c_str(), ev.requestor, prop.c_str());
        }

        return true;
    }

    bool XCB::RootDisplayExt::selectionRequestAction(xcb_selection_request_event_t* ev)
    {
        if(ev->selection == _atomClipboard || ev->selection == _atomPrimary)
        {
            if(ev->target == _atomTargets)
                return sendNotifyTargets(*ev);
            else if((ev->target == _atomText || ev->target == _atomTextPlain || ev->target == _atomUTF8) &&
                    ev->property != XCB_ATOM_NONE && ! _selbuf.empty())
                return sendNotifySelData(*ev);
        }

        // other variants: discard request
        xcb_selection_notify_event_t notify = { .response_type = XCB_SELECTION_NOTIFY, .pad0 = 0, .sequence = 0,
                                                .time = ev->time, .requestor = ev->requestor, .selection = ev->selection, .target = ev->target, .property = XCB_ATOM_NONE
                                              };
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

    void XCB::RootDisplayExt::setClipboardClear(void)
    {
        if(! _selbuf.empty())
            _selbuf.clear();
    }

    bool XCB::RootDisplayExt::setClipboardEvent(const uint8_t* buf, size_t len, std::initializer_list<xcb_atom_t> atoms)
    {
        _selbuf.assign(buf, buf + len);

        if(_selbuf.empty())
            return false;

        for(auto atom : atoms)
        {
            // take owner
            if(getOwnerSelection(atom) != _selwin)
                xcb_set_selection_owner(_conn, _selwin, atom, XCB_CURRENT_TIME);
        }
        xcb_flush(_conn);

        return true;
    }

    bool XCB::RootDisplayExt::setClipboardEvent(const uint8_t* buf, size_t len)
    {
        return setClipboardEvent(buf, len, { _atomPrimary, _atomClipboard });
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
        auto type = getPropertyType(_selwin, _atomBuffer);

        if(XCB_ATOM_NONE == type)
            return false;

        if(type != _atomUTF8 && type != _atomText && type != _atomTextPlain)
        {
            auto name = getAtomName(type);
            Application::error("xcb selection notify action, unsupported type: %d, `%s'", type, name.c_str());
            return false;
        }

        // get data
        auto xcbReply = getReplyFunc2(xcb_get_property, _conn, false, _selwin, _atomBuffer, type, 0, ~0);

        if(auto err = xcbReply.error())
            extendedError(err, "xcb_get_property");
        else if(auto reply = xcbReply.reply())
        {
            bool ret = false;
            auto ptrbuf = reinterpret_cast<const uint8_t*>(xcb_get_property_value(reply.get()));
            int length = xcb_get_property_value_length(reply.get());

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
                        Application::debug("xcb get selection, size: %d, content: `%s'", _selbuf.size(), log.c_str());
                    }
                }
            }

            if(auto errorReq = checkRequest(xcb_delete_property_checked(_conn, _selwin, _atomBuffer)))
                extendedError(errorReq, "xcb_delete_property");

            return ret;
        }

        return false;
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
