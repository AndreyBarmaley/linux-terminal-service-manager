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

#include <cstring>
#include <iomanip>
#include <iostream>
#include <algorithm>

#include "xcb/xtest.h"
#include "xcb/xproto.h"
#include "xcb/damage.h"

#ifdef LTSM_BUILD_XCB_ERRORS
#include "libxcb-errors/xcb_errors.h"
#endif

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    size_t XCB::HasherKeyCodes::operator()(const KeyCodes & kc) const
    {
        if(kc.get())
        {
            size_t len = 0;

            while(*(kc.get() + len) != XCB_NO_SYMBOL) len++;

            return Tools::crc32b(kc.get(), len);
        }

        return 0;
    }

    /* XCB::SHM */
    XCB::shm_t::~shm_t()
    {
        if(xcb) xcb_shm_detach(conn, xcb);

        if(addr) shmdt(addr);

        if(0 < shm) shmctl(shm, IPC_RMID, 0);
    }

    XCB::SHM::SHM(int shmid, uint8_t* addr, xcb_connection_t* xcon)
        : std::shared_ptr<shm_t>(std::make_shared<shm_t>(shmid, addr, xcon))
    {
        auto xcbid = xcb_generate_id(xcon);
        auto cookie = xcb_shm_attach_checked(xcon, xcbid, shmid, 0);
        get()->xcb = xcbid;
        get()->error = xcb_request_check(xcon, cookie);
    }

    const xcb_generic_error_t* XCB::SHM::error(void) const
    {
        return get() ? get()->error : nullptr;
    }

    xcb_shm_get_image_cookie_t XCB::SHM::getPixmapRegionRequest(xcb_drawable_t winid, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh, uint32_t offset) const
    {
        return getPixmapRegionRequest(winid, { rx, ry, rw, rh }, offset);
    }

    xcb_shm_get_image_cookie_t XCB::SHM::getPixmapRegionRequest(xcb_drawable_t winid, const xcb_rectangle_t & rect, uint32_t offset) const
    {
        return xcb_shm_get_image_unchecked(get()->conn, winid, rect.x, rect.y, rect.width, rect.height,
                                           ~0 /* plane mask */, XCB_IMAGE_FORMAT_Z_PIXMAP, get()->xcb, offset);
    }

    std::pair<bool, XCB::PixmapInfo> XCB::SHM::getPixmapRegion(xcb_drawable_t winid, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh, uint32_t offset) const
    {
        return getPixmapRegion(winid, { rx, ry, rw, rh }, offset);
    }

    std::pair<bool, XCB::PixmapInfo> XCB::SHM::getPixmapRegion(xcb_drawable_t winid, const xcb_rectangle_t & rect, uint32_t offset) const
    {
        auto cookie = xcb_shm_get_image(get()->conn, winid, rect.x, rect.y, rect.width, rect.height,
                                        ~0 /* plane mask */, XCB_IMAGE_FORMAT_Z_PIXMAP, get()->xcb, offset);
        std::unique_ptr<xcb_shm_get_image_reply_t, decltype(&std::free)> shmReply { xcb_shm_get_image_reply(get()->conn, cookie, nullptr), &std::free };
        std::pair<bool, PixmapInfo> res;
        auto reply = shmReply.get();
        res.first = reply;

        if(reply)
        {
            res.second.depth = reply->depth;
            res.second.size = reply->size;
            res.second.visual = reply->visual;
        }

        return res;
    }

    /* XCB::GC */
    XCB::GC::GC(xcb_drawable_t winid, xcb_connection_t* xcon, uint32_t value_mask, const void* value_list)
        : std::shared_ptr<gc_t>(std::make_shared<gc_t>(xcon))
    {
        auto xcbid = xcb_generate_id(xcon);
        auto cookie = xcb_create_gc_checked(xcon, xcbid, winid, value_mask, value_list);
        get()->xcb = xcbid;
        get()->error = xcb_request_check(xcon, cookie);
    }

    const xcb_generic_error_t* XCB::GC::error(void) const
    {
        return get() ? get()->error : nullptr;
    }

    /* XCB::Damage */
    XCB::Damage::Damage(xcb_drawable_t winid, int level, xcb_connection_t* xcon)
        : std::shared_ptr<damage_t>(std::make_shared<damage_t>(xcon))
    {
        auto xcbid = xcb_generate_id(xcon);
        auto cookie = xcb_damage_create_checked(xcon, xcbid, winid, level);
        get()->xcb = xcbid;
        get()->error = xcb_request_check(xcon, cookie);
    }

    const xcb_generic_error_t* XCB::Damage::error(void) const
    {
        return get() ? get()->error : nullptr;
    }

    XCB::GenericError XCB::Damage::addRegion(xcb_drawable_t winid, xcb_xfixes_region_t regid)
    {
        auto cookie = xcb_damage_add_checked(get()->conn, winid, regid);
        return GenericError(xcb_request_check(get()->conn, cookie));
    }

    XCB::GenericError XCB::Damage::subtractRegion(xcb_drawable_t winid, xcb_xfixes_region_t repair, xcb_xfixes_region_t parts)
    {
        auto cookie = xcb_damage_subtract_checked(get()->conn, winid, repair, parts);
        return GenericError(xcb_request_check(get()->conn, cookie));
    }

    /* XCB::XFixesRegion */
    XCB::XFixesRegion::XFixesRegion(const xcb_rectangle_t* rects, uint32_t count, xcb_connection_t* xcon)
        : std::shared_ptr<xfixes_region_t>(std::make_shared<xfixes_region_t>(xcon))
    {
        auto xcbid = xcb_generate_id(xcon);
        auto cookie = xcb_xfixes_create_region_checked(xcon, xcbid, count, rects);
        get()->xcb = xcbid;
        get()->error = xcb_request_check(xcon, cookie);
    }

    XCB::XFixesRegion::XFixesRegion(xcb_window_t win, xcb_shape_kind_t kind, xcb_connection_t* xcon)
        : std::shared_ptr<xfixes_region_t>(std::make_shared<xfixes_region_t>(xcon))
    {
        auto xcbid = xcb_generate_id(xcon);
        auto cookie = xcb_xfixes_create_region_from_window_checked(xcon, xcbid, win, kind);
        get()->xcb = xcbid;
        get()->error = xcb_request_check(xcon, cookie);
    }

    const xcb_generic_error_t* XCB::XFixesRegion::error(void) const
    {
        return get() ? get()->error : nullptr;
    }

    /* XCB::Connector */
    XCB::Connector::Connector(const char* addr) : _conn(nullptr)
    {
        _conn = xcb_connect(addr, nullptr);

        if(xcb_connection_has_error(_conn))
            throw std::string("connect error ").append(addr);
    }

    XCB::Connector::~Connector()
    {
        xcb_disconnect(_conn);
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

    XCB::SHM XCB::Connector::createSHM(size_t shmsz, int mode)
    {
        int shmid = shmget(IPC_PRIVATE, shmsz, IPC_CREAT | mode);

        if(shmid == -1)
        {
            Application::error("shmget failed, size: %d, error: %s", shmsz, strerror(errno));
            return XCB::SHM();
        }

        uint8_t* shmaddr = reinterpret_cast<uint8_t*>(shmat(shmid, 0, 0));

        // man shmat: check result
        if(shmaddr == reinterpret_cast<uint8_t*>(-1) && 0 != errno)
        {
            Application::error("shmaddr failed, id: %d, error: %s", shmid, strerror(errno));
            return XCB::SHM();
        }

        SHM shm(shmid, shmaddr, _conn);

        if(shm->error)
            extendedError(shm->error, "xcb_shm_attach_checked");

        return shm;
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

    XCB::GenericEvent XCB::Connector::poolEvent(void)
    {
        return GenericEvent(xcb_poll_for_event(_conn));
    }

    bool XCB::Connector::checkExtensionSHM(void)
    {
        xcb_generic_error_t* error;
        auto _shm = xcb_get_extension_data(_conn, &xcb_shm_id);

        if(! _shm || ! _shm->present)
            return false;

        auto cookie = xcb_shm_query_version(_conn);
        auto version = xcb_shm_query_version_reply(_conn, cookie, &error);

        if(error)
        {
            extendedError(error, "xcb_shm_query_version");
            free(error);
            return false;
        }
        else
            Application::debug("used %s extension, version: %d.%d", "SHM", version->major_version, version->minor_version);

        free(version);
        return true;
    }

    bool XCB::Connector::checkExtensionXFIXES(void)
    {
        xcb_generic_error_t* error;
        auto _xfixes = xcb_get_extension_data(_conn, &xcb_xfixes_id);

        if(! _xfixes || ! _xfixes->present)
            return false;

        auto cookie = xcb_xfixes_query_version(_conn, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
        auto version = xcb_xfixes_query_version_reply(_conn, cookie, &error);

        if(error)
        {
            extendedError(error, "xcb_xfixes_query_version");
            free(error);
            return false;
        }
        else
            Application::debug("used %s extension, version: %d.%d", "XFIXES", version->major_version, version->minor_version);

        free(version);
        return true;
    }

    bool XCB::Connector::checkExtensionDAMAGE(void)
    {
        xcb_generic_error_t* error;
        auto _damage = xcb_get_extension_data(_conn, &xcb_damage_id);

        if(! _damage || ! _damage->present)
            return false;

        auto cookie = xcb_damage_query_version(_conn, XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION);
        auto version = xcb_damage_query_version_reply(_conn, cookie, &error);

        if(error)
        {
            extendedError(error, "xcb_damage_query_version");
            free(error);
            return false;
        }
        else
            Application::debug("used %s extension, version: %d.%d", "DAMAGE", version->major_version, version->minor_version);

        free(version);
        return true;
    }

    bool XCB::Connector::checkExtensionTEST(void)
    {
        xcb_generic_error_t* error;
        auto _xtest = xcb_get_extension_data(_conn, &xcb_test_id);

        if(! _xtest || ! _xtest->present)
            return false;

        auto cookie = xcb_test_get_version_unchecked(_conn, XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION);
        auto version = xcb_test_get_version_reply(_conn, cookie, &error);

        if(error)
        {
            extendedError(error, "xcb_test_query_version");
            free(error);
            return false;
        }
        else
            Application::debug("used %s extension, version: %d.%d", "TEST", version->major_version, version->minor_version);

        free(version);
        return true;
    }

    /* XCB::RootDisplay */
    XCB::RootDisplay::RootDisplay(const std::string & addr) : Connector(addr.c_str()), _screen(nullptr), _symbols(nullptr),
        _format(nullptr), _visual(nullptr)
    {
        auto setup = xcb_get_setup(_conn);
        _screen = xcb_setup_roots_iterator(setup).data;

        if(! _screen)
            throw std::string("root window not found");

        // init format
        for(auto fIter = xcb_setup_pixmap_formats_iterator(setup); fIter.rem; xcb_format_next(& fIter))
        {
            if(fIter.data->depth == _screen->root_depth)
            {
                _format = fIter.data;
                break;
            }
        }

        if(! _format)
            throw std::string("xcb format not found");

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
            throw std::string("xcb visual not found");

        _symbols = xcb_key_symbols_alloc(_conn);

        if(! _symbols)
            throw std::string("xcb_key_symbols_alloc error");

        // check extensions
        if(! checkExtensionSHM())
            throw std::string("failed: ").append("SHM extension");

        if(! checkExtensionDAMAGE())
            throw std::string("failed: ").append("DAMAGE extension");

        if(! checkExtensionXFIXES())
            throw std::string("failed: ").append("XFIXES extension");

        if(! checkExtensionTEST())
            throw std::string("failed: ").append("TEST extension");
    }

    XCB::RootDisplay::~RootDisplay()
    {
        xcb_key_symbols_free(_symbols);
    }

    int XCB::RootDisplay::bitsPerPixel(void) const
    {
        return _format ? _format->bits_per_pixel : 0;
    }

    int XCB::RootDisplay::bitsPerPixel(int depth) const
    {
        auto setup = xcb_get_setup(_conn);

        for(auto fIter = xcb_setup_pixmap_formats_iterator(setup); fIter.rem; xcb_format_next(& fIter))
            if(fIter.data->depth == depth) return fIter.data->bits_per_pixel;

        return 0;
    }

    int XCB::RootDisplay::scanlinePad(void) const
    {
        return _format ? _format->scanline_pad : 0;
    }

    const xcb_visualtype_t* XCB::RootDisplay::findVisual(xcb_visualid_t id) const
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

    int XCB::RootDisplay::depth(void) const
    {
        return _screen->root_depth;
    }

    int XCB::RootDisplay::width(void) const
    {
        return _screen->width_in_pixels;
    }

    int XCB::RootDisplay::height(void) const
    {
        return _screen->height_in_pixels;
    }

    XCB::GC XCB::RootDisplay::createGC(uint32_t value_mask, const void* value_list)
    {
        GC gc(_screen->root, _conn, value_mask, value_list);

        if(gc->error)
            extendedError(gc->error, "xcb_create_gc_checked");

        return gc;
    }

    std::pair<bool, XCB::PixmapInfo> XCB::RootDisplay::copyRootImageRegion(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh, uint8_t* buf) const
    {
        size_t pitch = rw * (bitsPerPixel() >> 2);
        //size_t reqLength = sizeof(xcb_get_image_request_t) + pitch * rh;
        uint64_t maxReqLength = xcb_get_maximum_request_length(_conn);
        std::pair<bool, PixmapInfo> res;

        if(pitch == 0)
        {
            Application::error("copy root image error, empty size: %d, %d, bpp: %d", rw, rh, bitsPerPixel());
            return res;
        }

	uint16_t allowRows = maxReqLength / pitch;
	if(allowRows > rh)
	    allowRows = rh;

        for(int16_t yy = ry; yy < ry + rh; yy += allowRows)
        {
	    // last rows
	    if(yy + allowRows > ry + rh)
		allowRows = ry + rh - yy;

            auto cookie = xcb_get_image(_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, _screen->root, rx, yy, rw, allowRows, ~0);
            std::unique_ptr<xcb_get_image_reply_t, decltype(&std::free)> xcbReply { xcb_get_image_reply(_conn, cookie, nullptr), &std::free };

            auto reply = xcbReply.get();
            res.first = reply;

            if(reply)
            {
                auto length = xcb_get_image_data_length(reply);
                auto ptr = xcb_get_image_data(reply);

                res.second.depth = reply->depth;
                res.second.size += length;
                res.second.visual = reply->visual;

                std::memcpy(buf, ptr, length);
                buf += length;
            }
        }

        return res;
    }

    std::pair<bool, XCB::PixmapInfo> XCB::RootDisplay::copyRootImageRegion(const SHM & shmInfo, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) const
    {
        return copyRootImageRegion(shmInfo, { rx, ry, rw, rh});
    }

    std::pair<bool, XCB::PixmapInfo> XCB::RootDisplay::copyRootImageRegion(const SHM & shmInfo, const xcb_rectangle_t & rect) const
    {
        return shmInfo.getPixmapRegion(_screen->root, rect, 0);
    }

    std::pair<bool, XCB::PixmapInfo> XCB::RootDisplay::copyRootImage(const SHM & shmInfo) const
    {
        return shmInfo.getPixmapRegion(_screen->root, 0, 0, _screen->width_in_pixels, _screen->height_in_pixels, 0);
    }

    XCB::Damage XCB::RootDisplay::createDamageNotify(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh, int level)
    {
        return createDamageNotify({rx, ry, rw, rh}, level);
    }

    XCB::Damage XCB::RootDisplay::createDamageNotify(const xcb_rectangle_t & rect, int level)
    {
        // make damage region
        auto xfixesRegion = XFixesRegion(& rect, 1, _conn);

        if(xfixesRegion->error)
            extendedError(xfixesRegion->error, "xcb_xfixes_create_region_checked");

        auto damageRegion = Damage(_screen->root, level, _conn);

        if(! damageRegion->error)
        {
            auto err = damageRegion.addRegion(_screen->root, xfixesRegion->xcb);

            if(err)
                extendedError(err.get(), "xcb_damage_add_checked");
        }
        else
            extendedError(damageRegion->error, "xcb_damage_create_checked");

        return damageRegion;
    }

    bool XCB::RootDisplay::damageSubtrack(const Damage & damage, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh)
    {
        return damageSubtrack(damage, { rx, ry, rw, rh});
    }

    bool XCB::RootDisplay::damageSubtrack(const Damage & damage, const xcb_rectangle_t & rect)
    {
        auto repair = XFixesRegion(& rect, 1, _conn);

        if(repair->error)
            extendedError(repair->error, "xcb_xfixes_create_region_checked");

        auto cookie = xcb_damage_subtract_checked(_conn, damage.get()->xcb, repair.get()->xcb, XCB_XFIXES_REGION_NONE);
        auto error = checkRequest(cookie);

        if(! error)
            return true;

        extendedError(error, "xcb_damage_subtract_checked");
        return false;
    }

    XCB::KeyCodes XCB::RootDisplay::keysymToKeycodes(int keysym) const
    {
        if(_symbols)
        {
            auto ptr = xcb_key_symbols_get_keycode(_symbols, keysym);
            return KeyCodes(ptr);
        }

        return 0;
    }

    int XCB::RootDisplay::getEventSHM(const GenericEvent & ev)
    {
        if(! ev) return -1;

        auto _shm = xcb_get_extension_data(_conn, &xcb_shm_id);
        auto error = reinterpret_cast<const xcb_generic_error_t*>(ev.get());

        if(ev->response_type == 0)
        {
            if(error->major_code == _shm->major_opcode)
                extendedError(error, "SHM extension");
        }
        else if(ev->response_type >= _shm->first_event && ev->response_type <= _shm->first_event + 6)
        {
            // XCB_SHM_QUERY_VERSION  0
            // XCB_SHM_ATTACH         1
            // XCB_SHM_DETACH         2
            // XCB_SHM_PUT_IMAGE      3
            // XCB_SHM_GET_IMAGE      4
            // XCB_SHM_CREATE_PIXMAP  5
            // XCB_SHM_ATTACH_FD      6
            // XCB_SHM_CREATE_SEGMENT 7
            return ev->response_type - _shm->first_event;
        }

        return -1;
    }

    bool XCB::RootDisplay::isEventSHM(const GenericEvent & ev, int filter)
    {
        int res = getEventSHM(ev);
        return filter < 0 ? 0 <= res : filter == res;
    }

    int XCB::RootDisplay::getEventDAMAGE(const GenericEvent & ev)
    {
        if(! ev) return -1;

        auto _damage = xcb_get_extension_data(_conn, &xcb_damage_id);
        auto error = reinterpret_cast<const xcb_generic_error_t*>(ev.get());

        if(ev->response_type == 0)
        {
            if(error->major_code == _damage->major_opcode)
            {
                if(error->minor_code != XCB_DAMAGE_SUBTRACT &&
                   error->minor_code != XCB_DAMAGE_CREATE)
                    extendedError(error, "DAMAGE extension");
            }
        }
        else if(ev->response_type >= _damage->first_event && ev->response_type <= _damage->first_event + 4)
        {
            // XCB_DAMAGE_NOTIFY   0
            // XCB_DAMAGE_CREATE   1
            // XCB_DAMAGE_DESTROY  2
            // XCB_DAMAGE_SUBTRACT 3
            // XCB_DAMAGE_ADD      4
            return ev->response_type - _damage->first_event;
        }

        return -1;
    }

    bool XCB::RootDisplay::isEventDAMAGE(const GenericEvent & ev, int filter)
    {
        int res = getEventDAMAGE(ev);
        return filter < 0 ? 0 <= res : filter == res;
    }

    int XCB::RootDisplay::getEventTEST(const GenericEvent & ev)
    {
        if(! ev) return -1;

        auto _xtest = xcb_get_extension_data(_conn, &xcb_test_id);
        auto error = reinterpret_cast<const xcb_generic_error_t*>(ev.get());

        if(ev->response_type == 0)
        {
            if(error->major_code == _xtest->major_opcode)
                extendedError(error, "TEST extension");
        }
        else if(ev->response_type >= _xtest->first_event && ev->response_type <= _xtest->first_event + 3)
        {
            // XCB_TEST_GET_VERSION    0
            // XCB_TEST_COMPARE_CURSOR 1
            // XCB_TEST_FAKE_INPUT     2
            // XCB_TEST_GRAB_CONTROL   3
            return ev->response_type - _xtest->first_event;
        }

        return -1;
    }

    bool XCB::RootDisplay::isEventTEST(const GenericEvent & ev, int filter)
    {
        int res = getEventTEST(ev);
        return filter < 0 ? 0 <= res : filter == res;
    }

    int XCB::RootDisplay::getEventXFIXES(const GenericEvent & ev)
    {
        if(! ev) return -1;

        auto _xfixes = xcb_get_extension_data(_conn, &xcb_xfixes_id);
        auto error = reinterpret_cast<const xcb_generic_error_t*>(ev.get());

        if(ev->response_type == 0)
        {
            if(error->major_code == _xfixes->major_opcode)
                extendedError(error, "XFIXES extension");
        }
        else if(ev->response_type >= _xfixes->first_event && ev->response_type <= _xfixes->first_event + 32)
            return ev->response_type - _xfixes->first_event;

        return -1;
    }

    bool XCB::RootDisplay::isEventXFIXES(const GenericEvent & ev, int filter)
    {
        int res = getEventXFIXES(ev);
        return filter < 0 ? 0 <= res : filter == res;
    }

    /*
        void XCB::RootDisplay::eventProcessing(void)
        {
            auto ev = poolEvent();
            if(! ev) return false;

            if(ev->response_type == 0)
            {
                extendedError(error, __PRETTY_FUNCTION__, "unknown error");
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
                        extendedError(error, __PRETTY_FUNCTION__, "unknown event");
                        break;
                }

                return true;
            }

            return false;
        }
    */

    bool XCB::RootDisplay::fakeInputKeysym(int type, const KeyCodes & keycodes, bool wait) const
    {
        if(keycodes.isValid())
        {
            auto keycode = keycodes.get();

            while(XCB_NO_SYMBOL != *keycode)
            {
                if(wait)
                {
                    auto cookie = xcb_test_fake_input_checked(_conn, type, *keycode, XCB_CURRENT_TIME, _screen->root, 0, 0, 0);
                    auto error = checkRequest(cookie);

                    if(error)
                    {
                        extendedError(error, "xcb_test_fake_input");
                        return false;
                    }
                }
                else
                    xcb_test_fake_input(_conn, type, *keycode, XCB_CURRENT_TIME, _screen->root, 0, 0, 0);

                keycode++;
            }

            return true;
        }

        return false;
    }

    bool XCB::RootDisplay::fakeInputMouse(int type, int buttons, int posx, int posy, bool wait) const
    {
        if(wait)
        {
            auto cookie = xcb_test_fake_input_checked(_conn, type, buttons, XCB_CURRENT_TIME, _screen->root, posx, posy, 0);
            auto error = checkRequest(cookie);

            if(! error)
                return true;

            extendedError(error, "xcb_test_fake_input");
        }
        else
        {
            xcb_test_fake_input(_conn, type, buttons, XCB_CURRENT_TIME, _screen->root, posx, posy, 0);
            return true;
        }

        return false;
    }

    /*
        void XCB::RootDisplay::fillRectangles(uint32_t color, uint32_t rects_num, const xcb_rectangle_t* rects_val)
        {
            uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
            uint32_t values[]  = { color, color, 0 };
            auto back = createGC(mask, values);

            auto cookie = xcb_poly_fill_rectangle_checked(_conn, _screen->root, back.xcb, rects_num, rects_val);
            auto error = checkRequest(cookie);
            if(error)
                extendedError(error, "xcb_poly_fill_rectangle_checked");
        }
    */

    void XCB::RootDisplay::fillBackground(uint32_t color)
    {
        uint32_t colors[]  = { color, 0 };
        auto cookie = xcb_change_window_attributes(_conn, _screen->root, XCB_CW_BACK_PIXEL, colors);
        auto error = checkRequest(cookie);

        if(error)
            extendedError(error, "xcb_change_window_attributes");
        else
        {
            cookie = xcb_clear_area_checked(_conn, 0, _screen->root, 0, 0, width(), height());
            error = checkRequest(cookie);

            if(error)
                extendedError(error, "xcb_clear_area_checked");
        }
    }
}
