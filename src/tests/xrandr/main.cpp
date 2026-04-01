
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>
#include <exception>

#include "xcb/randr.h"

#include "ltsm_tools.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_application.h"

using namespace LTSM;
using namespace std::chrono_literals;

class X11Test : public XCB::RootDisplay {
  public:
    X11Test(size_t display) : XCB::RootDisplay(display) {
    }

    bool test_extinfo(void) {
        for(auto iter = xcb_setup_pixmap_formats_iterator(_setup); iter.rem; xcb_format_next(& iter)) {
            Application::info("pixmap format: depth: {}, bpp: {}", iter.data->depth, iter.data->bits_per_pixel);
        }

        for(auto dIter = xcb_screen_allowed_depths_iterator(_screen); dIter.rem; xcb_depth_next(& dIter)) {
            Application::info("allowed depth: {}, visuals: {}", dIter.data->depth, dIter.data->visuals_len);

            for(auto vIter = xcb_depth_visuals_iterator(dIter.data); vIter.rem; xcb_visualtype_next(& vIter))
                Application::info("visual id: {:#04x}, class: {:#04x}, bits per rgb value: {}, red: {:#010x}, green: {:#010x}, blue: {:#010x}, color entries: {}",
                                  vIter.data->visual_id, vIter.data->_class, vIter.data->bits_per_rgb_value, vIter.data->red_mask, vIter.data->green_mask, vIter.data->blue_mask, vIter.data->colormap_entries);
        }

        return true;
    }

    bool test_pool_events(void) {
        if(hasError()) {
    	    return false;
        }

        while(auto ev = XCB::RootDisplay::pollEvent()) {
/*
            if(XCB::RootDisplay::isRandrNotify(ev, XCB_RANDR_NOTIFY_CRTC_CHANGE)) {
                auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                xcb_randr_crtc_change_t cc = rn->u.cc;

                // window, crtc, mode, rotation, x, y, width, height
                if(0 < cc.width && 0 < cc.height) {
                    Application::info("randr crtc change notify, window: {:#010x}, crtc: {:#010x}, mode: {}, rotation: {:#06x}, geometry: [{}, {}, {}, {}], sequence: {}, timestamp: {}",
                                  cc.window, cc.crtc, cc.mode, cc.rotation, cc.x, cc.y, cc.width, cc.height, rn->sequence, cc.timestamp);
                }
            } else if(XCB::RootDisplay::isRandrNotify(ev, XCB_RANDR_NOTIFY_OUTPUT_CHANGE)) {
                auto rn = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                xcb_randr_output_change_t oc = rn->u.oc;
                // window, output, crtc, mode, rotation, connection, subpixel
                Application::info("randr output change notify, window: {:#010x}, output: {:#010x}, crtc: {:#010x}, mode: {}, rotation: {:#06x}, connection: {}, subpixel_order: {}, sequence: {}, timestamp: {}, config timestamp: {}",
                                  oc.window, oc.output, oc.crtc, oc.mode, oc.rotation, oc.connection, oc.subpixel_order, rn->sequence, oc.timestamp, oc.config_timestamp);
            } else if(XCB::RootDisplay::isRandrScreenNotify(ev)) {
                auto sc = reinterpret_cast<xcb_randr_screen_change_notify_event_t*>(ev.get());
                // windows, timestamps, sizeID, subpixel, width, height, mwidth, mheight,
                Application::info("randr screen change notify,  rotation: {:#04x}, sequence: {}, root: {:#010x}, request_window: {:#010x}, sizeID: {}, size: [{}, {}], monitor: [{}, {}], timestamp: {}, config timestamp: {}",
                                  sc->rotation, sc->sequence, sc->root, sc->request_window, sc->sizeID, sc->width, sc->height, sc->mwidth, sc->mheight, sc->timestamp, sc->config_timestamp);
            }
*/
        }

        return true;
    }

    bool test_randr_info_modes(void) {
        auto randr = static_cast<const XCB::ModuleRandr*>(getExtension(XCB::Module::RANDR));

	auto modes = randr->getModesInfo();
    	Application::info("modes: {}", modes.size());

        for(const auto & info : modes) {
            Application::info("mode {:#010x}, width: {}, height: {}, clock: {}", info.id, info.width, info.height, info.dot_clock);
        }

	return true;
    }

    bool test_randr_info_outputs(void) {
        auto randr = static_cast<const XCB::ModuleRandr*>(getExtension(XCB::Module::RANDR));
        auto outputs = randr->getOutputs();

        Application::info("outputs: {}", outputs.size());

        for(const auto & val : outputs) {
            auto info = randr->getOutputInfo(val);
            Application::info("output name: {}, connected: {}, width: {}, height: {}", info->name, (info->connected ? "+" : "-"), info->mm_width, info->mm_height);

	}

//        xcb_randr_output_t curout;
//            if(info->connected) {
//                curout = val;
//            }
    	    //auto modes2 = randr->getOutputModes(curout);
            //if(std::ranges::any_of(modes2, [&](auto & id) { return id == info.id; })) {
            //}


        for(const auto & size : randr->getScreenSizes()) {
            Application::info("screen size: {}, {}", size.width, size.height);
        }

        return true;
    }

    bool test_randr_create_mode(const XCB::Size & nsz) {
        auto randr = static_cast<const XCB::ModuleRandr*>(getExtension(XCB::Module::RANDR));
        auto sizes = randr->getScreenSizes();

        if(std::ranges::any_of(sizes, [&](auto & st) { return st.width == nsz.width && st.height == nsz.height; })) {
            Application::warning("mode present, size: {}, {}", nsz.width, nsz.height);
            return false;
        }

        return true;
    }
};

class TestApp : public Application {
    int screen = 0;
    std::thread events;
    std::atomic<bool> shutdown{false};

  public:
    TestApp(int argc, const char** argv) : Application("test"), screen(0) {
        if(auto env = getenv("DISPLAY")) {
            if(1 < strlen(env)) {
                screen = std::stoi(env + 1, nullptr, 0);
            }
        }

        if(1 < argc) {
            auto val = argv[1];
            screen = std::stoi(val[0] == ':' ? & val[1] : val, nullptr, 0);

        }

        setDebugLevel(DebugLevel::Debug);
        setDebugTypes({ "xcb" });
    }

    int start(void) {
        auto xcb = std::unique_ptr<X11Test>(new X11Test(screen));

        if(! xcb) {
            Application::error("xcb connect: {}", "failed");
            return EXIT_FAILURE;
        }

        Application::info("xcb display info, width: {}, height: {}, depth: {}", xcb->width(), xcb->height(), xcb->depth());

        const xcb_visualtype_t* visual = xcb->visual();

        if(! visual) {
            Application::error("{}", "xcb visual empty");
            return EXIT_FAILURE;
        }

        Application::info("{}: xcb max request: {}", NS_FuncNameV, xcb->getMaxRequest());
        // xcb->test_extinfo();

	// events loop
	events = std::thread([this, ptr = xcb.get()](){
	    while(!shutdown) {
		if(!ptr->test_pool_events()) {
		    break;
		}
        	std::this_thread::sleep_for(std::chrono::milliseconds(1));
	    }
	});

        XCB::Size nsz(1024, 600);

        Application::info("{}: INFO =====================>", NS_FuncNameV);
        xcb->test_randr_info_outputs();

        //Application::info("{}: CREATE ===================>", NS_FuncNameV);
        //xcb->test_randr_create_mode(nsz);

        //Application::info("{}: INFO =====================>", NS_FuncNameV);
        //xcb->test_randr_info_outputs();

        //Application::info("{}: CHANGE====================>", NS_FuncNameV);

/*
        auto randr = static_cast<const XCB::ModuleRandr*>(getExtension(XCB::Module::RANDR));

        std::thread([ = ] {
            std::this_thread::sleep_for(1s);
            randr->setScreenSize(1024, 768);
        }).detach();

        std::thread([ = ] {
            std::this_thread::sleep_for(5s);
            randr->setScreenSize(nsz.width, nsz.height);
        }).detach();

            auto dt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);

            if(dt.count() > 9) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
*/
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto randr = static_cast<const XCB::ModuleRandr*>(xcb->getExtension(XCB::Module::RANDR));

	auto crtcs = randr->getCrtcs();
	auto outputs = randr->getOutputs();
	auto modes = randr->getModesInfo();

        Application::info("{}: CRTCs: {}", NS_FuncNameV, crtcs.size());
        Application::info("{}: OUTPUTs: {}", NS_FuncNameV, outputs.size());
        Application::info("{}: MODEs: {}", NS_FuncNameV, modes.size());

/*
	const xcb_randr_output_t* outputId = nullptr;
	const xcb_randr_mode_info_t* modeInfo = nullptr;

        for(const auto & id : outputs) {
            if(auto info = randr->getOutputInfo(id)) {
		if(info->connected) {
    		    Application::info("{}: connected: {}", NS_FuncNameV, id);
		    outputId = & id;
		    break;
		}
	    }
	}

        auto modes = _modRandr->getModesInfo();
        if(std::ranges::none_of(modes, [&](auto & info) {
                return info.width == monitor.width && info.height == monitor.height; })) {
            // added mode
            try {
                _modRandr->cvtCreateMode(monitor.toSize());
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                return false;
            }
            // rescan modes
            modes = _modRandr->getModesInfo();
        }

	if(outputId) {
	    auto itm = std::find_if(modes.begin(), modes.end(), [&](auto & mod){
		return nsz.width == mod.width && nsz.height == mod.height; });
	    if(itm == modes.end()) {
		modeInfo = randr->cvtCreateMode(nsz);
		modes = randr->getModesInfo();
	    } else {
		modeInfo = &(*itm);
	    }
	}
*/
/*
        for(const auto & outputId : _modRandr->getOutputs()) {
            if(auto info = _modRandr->getOutputInfo(outputId);
               info && info->connected == XCB_RANDR_CONNECTION_CONNECTED) {
                _modRandr->crtcDisconnect(info->crtc);
            }
        }
*/


	//xcb->setRandrMonitors({{0,0,1024,600}});
	uint16_t sequence;
	//xcb->setRandrScreenSize(nsz, & sequence);
        //xcb->test_randr_change_events(nsz);


    	Application::info("{}: ----------------------------------", NS_FuncNameV);
	xcb->setRandrScreenSize(XCB::Size(1264, 862), & sequence);
    	Application::info("{}: seq: {}", NS_FuncNameV, sequence);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    	Application::info("{}: ----------------------------------", NS_FuncNameV);


	xcb->setRandrScreenSize(XCB::Size(1032, 768), & sequence);
    	Application::info("{}: seq: {}", NS_FuncNameV, sequence);
    	std::this_thread::sleep_for(std::chrono::seconds(1));
    	Application::info("{}: ----------------------------------", NS_FuncNameV);

	xcb->setRandrScreenSize(XCB::Size(1120, 798), & sequence);
    	Application::info("{}: seq: {}", NS_FuncNameV, sequence);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    	Application::info("{}: ----------------------------------", NS_FuncNameV);

	shutdown = true;
	events.join();

        return EXIT_SUCCESS;
    }
};

int main(int argc, const char** argv) {
    try {
        TestApp app(argc, argv);
        return app.start();
    } catch(const std::exception & e) {
    }

    return EXIT_FAILURE;
}
