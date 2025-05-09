
#include <chrono>
#include <thread>
#include <cstring>
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
            Application::info("pixmap format: depth: %d, bpp: %d", iter.data->depth, iter.data->bits_per_pixel);

        for(auto dIter = xcb_screen_allowed_depths_iterator(_screen); dIter.rem; xcb_depth_next(& dIter))
        {
            Application::info("allowed depth: %d, visuals: %d", dIter.data->depth, dIter.data->visuals_len);

            for(auto vIter = xcb_depth_visuals_iterator(dIter.data); vIter.rem; xcb_visualtype_next(& vIter))
                Application::info("visual id: 0x%02x, class: 0x%02x, bits per rgb value: %d, red: %08x, green: %08x, blue: %08x, color entries: %d",
                         vIter.data->visual_id, vIter.data->_class, vIter.data->bits_per_rgb_value, vIter.data->red_mask, vIter.data->green_mask, vIter.data->blue_mask, vIter.data->colormap_entries);
        }
        return true;
    }

    bool test_randr_change_events(const XCB::Size & nsz)
    {
        auto randr = static_cast<const XCB::ModuleRandr*>( getExtension(XCB::Module::RANDR) );

        std::thread([=]{
            std::this_thread::sleep_for(1s);
    	    randr->setScreenSize(1024, 768);
	}).detach();

        std::thread([=]{
            std::this_thread::sleep_for(5s);
    	    randr->setScreenSize(nsz.width, nsz.height);
	}).detach();

        auto start = std::chrono::steady_clock::now();

        auto _randr = xcb_get_extension_data(_conn.get(), &xcb_randr_id);
        while(! hasError())
        {
            while(auto ev = XCB::RootDisplay::pollEvent())
            {
                if(XCB::RootDisplay::isRandrNotify(ev, XCB_RANDR_NOTIFY_CRTC_CHANGE))
                {
                    auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                    xcb_randr_crtc_change_t cc = rn->u.cc;
                    // window, crtc, mode, rotation, x, y, width, height
                    if(0 < cc.width && 0 < cc.height)
                    {
                        Application::info("randr crtc change notify, window: 0x%08" PRIx32 ", crtc: 0x%08" PRIx32 ", mode: %" PRIu32 ", rotation: 0x%04" PRIx16 ", geometry: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], sequence: 0x%04" PRIx16 ", timestamp: %" PRIu32,
                             cc.window, cc.crtc, cc.mode, cc.rotation, cc.x, cc.y, cc.width, cc.height, rn->sequence, cc.timestamp);
                    }
                }
                else
                if(XCB::RootDisplay::isRandrNotify(ev, XCB_RANDR_NOTIFY_OUTPUT_CHANGE))
                {
                    auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                    xcb_randr_output_change_t oc = rn->u.oc;
                    // window, output, crtc, mode, rotation, connection, subpixel
                    Application::info("randr output change notify, window: 0x%08" PRIx32 ", output: 0x%08" PRIx32 ", crtc: 0x%08" PRIx32 ", mode: %" PRIu32 ", rotation: 0x%04" PRIx16 ", connection: %" PRIu8 ", subpixel_order: %" PRIu8 ", sequence: 0x%04" PRIx16 ", timestamp: %" PRIu32 ", config timestamp: %" PRIu32,
                            oc.window, oc.output, oc.crtc, oc.mode, oc.rotation, oc.connection, oc.subpixel_order, rn->sequence, oc.timestamp, oc.config_timestamp);
                }
                else
                if(XCB::RootDisplay::isRandrScreenNotify(ev))
                {
                    // windows, timestamps, sizeID, subpixel, width, height, mwidth, mheight,
                    auto sc = reinterpret_cast<xcb_randr_screen_change_notify_event_t*>(ev.get());

                    Application::info("randr screen change notify,  rotation: 0x%02" PRIx8 ", sequence: 0x%04" PRIx16 ", root: 0x%08" PRIx32 ", request_window: 0x%08" PRIx32 ", sizeID: %" PRIu16 ", size: [%" PRIu16 ", %" PRIu16 "], monitor: [%" PRIu16 ", %" PRIu16 "], timestamp: %" PRIu32 ", config timestamp: %" PRIu32,
                            sc->rotation, sc->sequence, sc->root, sc->request_window, sc->sizeID, sc->width, sc->height, sc->mwidth, sc->mheight, sc->timestamp, sc->config_timestamp);
                }
	    }

            auto dt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
            if(dt.count() > 9)
            {
                break;
            }

	    std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return true;
    }

    bool test_randr_info_outputs(void)
    {
        auto randr = static_cast<const XCB::ModuleRandr*>( getExtension(XCB::Module::RANDR) );

	auto outputs = randr->getOutputs();
	xcb_randr_output_t curout;

        Application::info("outputs: %d", outputs.size());
    
	for(auto & val : outputs)
	{
	    auto info = randr->getOutputInfo(val);
    	    Application::info("output name: %s, connected: %s, width: %d, height: %d", info->name.c_str(), (info->connected ? "+" : "-"), info->mm_width, info->mm_height);

	    if(info->connected)
		curout = val;
	}

	auto modes = randr->getModesInfo();
        Application::info("modes: %d", modes.size());

	auto modes2 = randr->getOutputModes(curout);
	for(auto info : modes)
	{
	    if(std::any_of(modes2.begin(), modes2.end(), [&](auto & id){ return id == info.id; }))
	    {
    		Application::info("mode 0x%08x, width: %d, height: %d, clock: %d", info.id, info.width, info.height, info.dot_clock);
	    }
	}

	for(auto & size : randr->getScreenSizes())
	{
    	    Application::info("screen size: %d, %d", size.width, size.height);
	}

        return true;
    }

    bool test_randr_create_mode(const XCB::Size & nsz)
    {
        auto randr = static_cast<const XCB::ModuleRandr*>( getExtension(XCB::Module::RANDR) );
        auto sizes = randr->getScreenSizes();

	if(std::any_of(sizes.begin(), sizes.end(), [&](auto & st){ return st.width == nsz.width && st.height == nsz.height; }))
        {
    	    Application::warning("mode present, size: %d, %d", nsz.width, nsz.height);
            return false;
        }

        return true;
    }
};

class TestApp : public Application
{
    int screen;

public:
    TestApp(int argc, const char** argv) : Application("test"), screen(0)
    {
        if(auto env = getenv("DISPLAY"))
        {
            if(1 < strlen(env))
                screen = std::stoi(env + 1, nullptr, 0);
        }

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
        _xcbDisplay->test_extinfo();
        return EXIT_SUCCESS;

        XCB::Size nsz(1024, 600);

        Application::info("%s: INFO =====================>", __FUNCTION__);
    	_xcbDisplay->test_randr_info_outputs();

        Application::info("%s: CREATE ===================>", __FUNCTION__);
        _xcbDisplay->test_randr_create_mode(nsz);

        Application::info("%s: INFO =====================>", __FUNCTION__);
    	_xcbDisplay->test_randr_info_outputs();

        Application::info("%s: CHANGE====================>", __FUNCTION__);
    	_xcbDisplay->test_randr_change_events(nsz);

        return EXIT_SUCCESS;
    }
};

int main(int argc, const char** argv)
{
    try
    {
	TestApp app(argc, argv);
	return app.start();
    }
    catch(const std::exception & e)
    {
    }

    return EXIT_FAILURE;
}
