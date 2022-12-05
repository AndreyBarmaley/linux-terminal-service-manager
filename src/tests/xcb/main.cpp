
#include <chrono>
#include <thread>
#include <iostream>
#include <exception>

//#include <xkbcommon/xkbcommon.h>
//#include "xkbcommon/xkbcommon-x11.h"

#include "xcb/randr.h"

#include "ltsm_tools.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_application.h"

using namespace LTSM;
using namespace std::chrono_literals;

class X11Test : public XCB::RootDisplay
{
public:
    X11Test(size_t display) : XCB::RootDisplay(display)
    {
    }

    bool test_extinfo(void)
    {
        for(auto iter = xcb_setup_pixmap_formats_iterator(_setup); iter.rem; xcb_format_next(& iter))
            Application::info("pixmap format: depth:%d, bpp:%d", iter.data->depth, iter.data->bits_per_pixel);

        for(auto dIter = xcb_screen_allowed_depths_iterator(_screen); dIter.rem; xcb_depth_next(& dIter))
        {
            Application::info("allowed depth:%d, visuals:%d", dIter.data->depth, dIter.data->visuals_len);
            for(auto vIter = xcb_depth_visuals_iterator(dIter.data); vIter.rem; xcb_visualtype_next(& vIter))
                Application::info("visual id: 0x%02x, class: 0x%02x, bits per rgb value: %d, red: %08x, green: %08x, blue: %08x, color entries: %d",
                         vIter.data->visual_id, vIter.data->_class, vIter.data->bits_per_rgb_value, vIter.data->red_mask, vIter.data->green_mask, vIter.data->blue_mask, vIter.data->colormap_entries);
        }
        return true;
    }

    bool test_getimage(void)
    {
        auto damage = region();
        auto reply = copyRootImageRegion(damage);
        if(! reply) return false;

        // reply info dump
        Application::info("get_image: request size [%d, %d], reply length: %d, bits per pixel: %d, red: %08x, green: %08x, blue: %08x",
                damage.width, damage.height, reply->size(), reply->bitsPerPixel(), reply->rmask, reply->gmask, reply->bmask);

        return true;
    }

    bool test_randr(void)
    {
/*
        std::thread([=]{
            std::this_thread::sleep_for(2s);
    	    this->setRandrScreenSize(XCB::Size(1024, 768));
	}).detach();

        std::thread([=]{
            std::this_thread::sleep_for(5s);
    	    this->setRandrScreenSize(XCB::Size(1280, 800));
	}).detach();

        auto _randr = xcb_get_extension_data(_conn.get(), &xcb_randr_id);
        while(! hasError())
        {
            while(auto ev = XCB::RootDisplay::poolEvent())
            {
                if(XCB::RootDisplay::isRandrNotify(ev, XCB_RANDR_NOTIFY_CRTC_CHANGE))
                {
                    auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                    xcb_randr_crtc_change_t cc = rn->u.cc;
                    // window, crtc, mode, rotation, x, y, width, height
                    if(0 < cc.width && 0 < cc.height)
                    {
        	        Application::info("randr crtc change: %d,%d, screen: %d,%d", cc.width, cc.height, width(), height());
                    }
                }
                else
                if(XCB::RootDisplay::isRandrNotify(ev, XCB_RANDR_NOTIFY_OUTPUT_CHANGE))
                {
                    auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                    xcb_randr_output_change_t oc = rn->u.oc;
                    // window, output, crtc, mode, rotation, connection, subpixel
        	    Application::info("randr output change, connection: 0x%02x", oc.connection);
                }
                else
                if(XCB::Connector::isRandrScreenNotify(ev))
                {
                    // windows, timestamps, sizeID, subpixel, width, height, mwidth, mheight,
                    auto scn = reinterpret_cast<xcb_randr_screen_change_notify_event_t*>(ev.get());
        	    Application::info("change notify: %d,%d", scn->width, scn->height);
                }
	    }

	    std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
*/
	return true;
    }

    bool test_keys(void)
    {
        std::thread([=]{
	    for(int i = 0; i < 20; ++i)
	    {
		int group = this->getXkbLayoutGroup();
		Application::info("info active layout group: %d", group);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	    }
	}).detach();

        while(! hasError())
        {
            while(auto ev = XCB::RootDisplay::poolEvent())
            {
		if(isXkbKeyboardNotify(ev))
		{
		    auto xn = reinterpret_cast<xkb_notify_event_t*>(ev.get());
        	    Application::info("keyboard notify, devid: %d, old devid: %d, changed: %d", xn->keyboard_notify.deviceID, xn->keyboard_notify.oldDeviceID, xn->keyboard_notify.changed);
		}
		else
		if(isXkbMapNotify(ev))
		{
		    auto xn = reinterpret_cast<xkb_notify_event_t*>(ev.get());
        	    Application::info("map notify, deviceID: %d, ptrBtnActions %d, changed %d", xn->map_notify.deviceID, xn->map_notify.ptrBtnActions, xn->map_notify.changed);
		}
		else
		if(isXkbStateNotify(ev))
		{
		    auto xn = reinterpret_cast<xkb_notify_event_t*>(ev.get());
        	    Application::info("state notify, deviceID: %d, mods: baseMods: %d, latchedMods: %d, lockedMods: %d, group: %d, baseGroup: %d, latchedGroup: %d, lockedGroup: %d, compatState: %d, grabMods: %d, compatGrabMods: %d, lookupMods: %d, compatLoockupMods: %d, ptrBtnState: %d, changed: %d",
				xn->state_notify.deviceID, xn->state_notify.mods, xn->state_notify.baseMods, xn->state_notify.latchedMods, xn->state_notify.lockedMods,
				xn->state_notify.group, xn->state_notify.baseGroup, xn->state_notify.latchedGroup, xn->state_notify.lockedGroup, xn->state_notify.compatState,
				xn->state_notify.grabMods, xn->state_notify.compatGrabMods, xn->state_notify.lookupMods, xn->state_notify.compatLoockupMods,
				xn->state_notify.ptrBtnState, xn->state_notify.changed);
		}
	    }

	    std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

        // 2c 2e 2f
/*
        auto code1 = keysymToKeycode(0x2f);
        auto code2 = keysymToKeycode(0x2f);

        XCB::RootDisplay::fakeInputKeycode(XCB_KEY_PRESS, 0x32);
        XCB::RootDisplay::fakeInputKeycode(XCB_KEY_PRESS, 61);

        XCB::RootDisplay::fakeInputKeycode(XCB_KEY_RELEASE, 61);
        XCB::RootDisplay::fakeInputKeycode(XCB_KEY_RELEASE, 0x32);
*/

        return true;
    }

    bool test_randr_outputs(void)
    {
	auto outputs = getRandrOutputs();
	xcb_randr_output_t curout;

        Application::info("outputs: %d", outputs.size());
    
	for(auto & val : outputs)
	{
	    auto info = getRandrOutputInfo(val);
    	    Application::info("output name: %s, connected: %s, width: %d, height: %d", info.name.c_str(), (info.connected ? "+" : "-"), info.mm_width, info.mm_height);

	    if(info.connected)
		curout = val;
	}

	xcb_randr_mode_t nmode = createRandrMode(1024, 600);
        if(0 == nmode || ! addRandrOutputMode(curout, nmode))
            return false;

	auto modes = getRandrModesInfo();
        Application::info("modes: %d", modes.size());

	auto modes2 = getRandrOutputModes(curout);
	for(auto info : modes)
	{
	    if(std::any_of(modes2.begin(), modes2.end(), [&](auto & id){ return id == info.id; }))
	    {
    		Application::info("mode 0x%08x, width: %d, height: %d, clock: %d", info.id, info.width, info.height, info.dot_clock);
	    }
	}

	for(auto & size : getRandrScreenSizes())
	{
    	    Application::info("screen size: %d, %d", size.width, size.height);
	}

        return true;
    }

    bool test_xkblayoutcur(void)
    {
        //xcb_xkb_get_names_cookie_t      xcb_xkb_get_names (xcb_conn.get()ection_t *c, xcb_xkb_device_spec_t deviceSpec, uint32_t which)
        auto xcbReply = getReplyFunc2(xcb_xkb_get_state, _conn.get(), XCB_XKB_ID_USE_CORE_KBD);
        if(auto err = xcbReply.error())
        {
            Application::error("xcb_xkb_get_state: %s", "failed");
        }
        else
        if(auto reply = xcbReply.reply())
        {
            Application::info("current layout: %d", reply->group);
            return true;
        }
        return false;
    }

    bool test_xkbgroup(int group)
    {
        auto cookie = xcb_xkb_latch_lock_state_checked(_conn.get(), XCB_XKB_ID_USE_CORE_KBD, 0, 0, 1, group, 0, 0, 0);
        auto errorReq = checkRequest(cookie);
        
        if(errorReq)
        {
            //extendedError(errorReq, "xcb_xkb_latch_lock_state_checked");
            return false;
        }

        return true;
    }

    bool test_xkblayout(void)
    {
        //test_xkbgroup(1);
        //test_xkblayoutcur();

        Application::info("xkb group names1: %s", Tools::join(getXkbNames(), ",").c_str());
        Application::info("xkb layout group: %d", getXkbLayoutGroup());

        fakeInputKeycode(96, true);
        Application::info("xkb group names2: %s", Tools::join(getXkbNames(), ",").c_str());
        fakeInputKeycode(96, false);

        return false;
    }


    bool test_xkbinfo(void)
    {
        //const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };

        /*
        auto _selwin = xcb_generate_id(_conn.get());
        auto cookie = xcb_create_window_checked(_conn.get(), 0, _selwin, _screen->root, -1, -1, 1, 1, 0,
                            XCB_WINDOW_CLASS_INPUT_OUTPUT, _screen->root_visual, mask, values);

        auto errorReq = checkRequest(cookie);
        if(errorReq)
        {
            Application::error("xcb_create_window failed");
            return false;
        }
        */
        xcb_change_window_attributes(_conn.get(), _screen->root, XCB_CW_EVENT_MASK, values);
        xcb_flush(_conn.get());

        auto active = getAtom("_NET_ACTIVE_WINDOW");
        auto utf8 = getAtom("UTF8_STRING");

        while(! hasError())
        {
            while(auto ev = XCB::RootDisplay::poolEvent())
            {
                auto type = ev ? ev->response_type & ~0x80 : 0;
                if(XCB_PROPERTY_NOTIFY == type)
                {
                    auto pn = reinterpret_cast<xcb_property_notify_event_t*>(ev.get());
                    if(pn && pn->atom == active)
                    {
                        auto type = getPropertyType(_screen->root, active);
                        Application::info("property: %d, `%s'", type, getAtomName(type).c_str());

                        auto win = getPropertyWindow(_screen->root, active);
                        Application::info("property change for window id: %08x", win);

                        auto str1 = getPropertyString(win, XCB_ATOM_WM_CLASS);
                        //auto str2 = getPropertyString(win, XCB_ATOM_WM_CLASS, str1.size() + 1);
    	                Application::info("win: %08x, wmclass: `%s', %d", win, str1.c_str(),str1.size());
/*
                        auto list = getPropertyStringList(win, XCB_ATOM_WM_CLASS);
    	                Application::info("list: %d", list.size());
    	                if(1 < list.size())
                            Application::info("win: %08x, wmclass: `%s', `%s'", win, list.front().c_str(), list.back().c_str());
*/
                    }
                }
            }

	    std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

        //xcb_destroy_window(_conn.get(), _selwin);
	return true;
    }
};

class App : public Application
{
    int screen;

public:
    App(int argc, const char** argv) : Application("test"), screen(0)
    {
	if(1 < argc)
	{
	    auto val = argv[1];
    	    screen = std::stoi(val[0] == ':' ? & val[1] : val, nullptr, 0);
	}
    }

    int start(void)
    {
	auto _xcbDisplay = std::unique_ptr<X11Test>(new X11Test(screen));
        if(! _xcbDisplay)
        {
            Application::error("xcb connect: %s", "failed");
            return EXIT_FAILURE;
        }

	Application::info("xcb display info, width: %d, height: %d, depth: %d", _xcbDisplay->width(), _xcbDisplay->height(), _xcbDisplay->depth());

        const xcb_visualtype_t* visual = _xcbDisplay->visual();
        if(! visual)
        {
            Application::error("%s", "xcb visual empty");
            return EXIT_FAILURE;
        }

        Application::info("%s: xcb max request: %d", __FUNCTION__, _xcbDisplay->getMaxRequest());

    	// _xcbDisplay->test_randr();
    	// _xcbDisplay->test_extinfo();
    	// _xcbDisplay->test_getimage();

	//XCB::Region reg{10, 10, 9, 9};
	//for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
	//    Application::info("region coord: %d, %d", coord.x, coord.y);

    	//_xcbDisplay->test_randr_outputs();
	//_xcbDisplay->setRandrScreenSize(XCB::Size(1201, 887));

    	//_xcbDisplay->test_xkbinfo();
    	_xcbDisplay->test_xkblayout();

        return EXIT_SUCCESS;
    }
};

int main(int argc, const char** argv)
{
    try
    {
	App app(argc, argv);
	return app.start();
    }
    catch(const std::exception & e)
    {
    }

    return EXIT_FAILURE;
}
