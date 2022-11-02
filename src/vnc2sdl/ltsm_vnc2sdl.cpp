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
 **************************************************************************/

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "librfb_client.h"
#include "ltsm_vnc2sdl.h"

using namespace std::chrono_literals;

namespace LTSM
{
    void printHelp(const char* prog)
    {
        std::cout << "version: " << LTSM_VNC2SDL_VERSION << std::endl;
        std::cout << "usage: " << prog << " --host <localhost> [--port 5900] [--password <pass>] [--notls] [--noxkb] [--debug] [--tls-priority <string>] [--fullscreen] [--geometry WIDTHxHEIGHT] [--ca-file <path>] [--cert-file <path>] [--key-file <path>] [--noaccel] [--pulse [XDG_RUNTIME_DIR/pulse/native]] [--printer [sock://127.0.0.1:9100]] [--sane [sock://127.0.0.1:6566]] [--pcscd [unix:///var/run/pcscd/pcscd.comm]]" << std::endl;
    }

    Vnc2SDL::Vnc2SDL(int argc, const char** argv)
        : Application("ltsm_vnc2sdl")
    {
        Application::setDebugLevel(DebugLevel::SyslogInfo);

        rfbsec.authVenCrypt = true;
        rfbsec.tlsDebug = 2;

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                printHelp(argv[0]);
                throw 0;
            }
        }

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--nocaps"))
                capslock = false;
            else
            if(0 == std::strcmp(argv[it], "--noaccel"))
                accelerated = false;
            else
            if(0 == std::strcmp(argv[it], "--notls"))
                rfbsec.authVenCrypt = false;
            else
            if(0 == std::strcmp(argv[it], "--noxkb"))
                usexkb = false;
            else
            if(0 == std::strcmp(argv[it], "--fullscreen"))
                fullscreen = true;
            else
            if(0 == std::strcmp(argv[it], "--printer"))
            {
                printerUrl = "sock://127.0.0.1:9100";

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                        Application::error("%s: parse %s failed, unknown url: %s", __FUNCTION__, "printer", argv[it + 1]);
                    else
                        printerUrl.assign(argv[it + 1]);

                    it = it + 1;
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--sane"))
            {
                saneUrl = "sock://127.0.0.1:6566";

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                        Application::error("%s: parse %s failed, unknown url: %s", __FUNCTION__, "sane", argv[it + 1]);
                    else
                        saneUrl.assign(argv[it + 1]);

                    it = it + 1;
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--pulse"))
            {
                if(auto runtime = getenv("XDG_RUNTIME_DIR"))
                {
                    auto path = std::filesystem::path(runtime) / "pulse" / "native";
                    pulseUrl = std::string("unix://").append(path.native());
                }

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                        Application::error("%s: parse %s failed, unknown url: %s", __FUNCTION__, "pulse", argv[it + 1]);
                    else
                        pulseUrl.assign(argv[it + 1]);

                    it = it + 1;
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--pcscd"))
            {
                pcscdUrl = "unix:///var/run/pcscd/pcscd.comm";

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                        Application::error("%s: parse %s failed, unknown url: %s", __FUNCTION__, "pcscd", argv[it + 1]);
                    else
                        pcscdUrl.assign(argv[it + 1]);

                    it = it + 1;
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--debug"))
                Application::setDebugLevel(DebugLevel::Console);
            else
            if(0 == std::strcmp(argv[it], "--host") && it + 1 < argc)
            {
                host.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--tls-priority") && it + 1 < argc)
            {
                rfbsec.tlsPriority.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--password") && it + 1 < argc)
            {
                rfbsec.passwdFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--port") && it + 1 < argc)
            {
                try
                {
                    port = std::stoi(argv[it + 1]);
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "incorrect port number" << std::endl;
                    port = 5900;
                }
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--geometry") && it + 1 < argc)
            {
                size_t idx;

                try
                {
                    setWidth = std::stoi(argv[it + 1], & idx, 0);
                    setHeight = std::stoi(argv[it + 1] + idx + 1, nullptr, 0);
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "invalid geometry" << std::endl;
                    setWidth = setHeight = 0;
                }

                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--ca-file") && it + 1 < argc)
            {
                rfbsec.caFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--cert-file") && it + 1 < argc)
            {
                rfbsec.certFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--key-file") && it + 1 < argc)
            {
                rfbsec.keyFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            {
                throw std::invalid_argument(argv[it]);
            }
        }

        if(fullscreen)
        {
            SDL_DisplayMode mode;
            if(0 == SDL_GetDisplayMode(0, 0, & mode))
            {
                setWidth = mode.w;
                setHeight = mode.h;

                if(setWidth < setHeight)
                    std::swap(setWidth, setHeight);
            }
        }
    }

    int Vnc2SDL::start(void)
    {
        auto ipaddr = TCPSocket::resolvHostname(host);
        int sockfd = TCPSocket::connect(ipaddr, port);

        if(0 > sockfd)
            return -1;

        RFB::ClientDecoder::setSocketStreamMode(sockfd);

        rfbsec.authVnc = ! rfbsec.passwdFile.empty();
        rfbsec.tlsAnonMode = rfbsec.keyFile.empty();

        // connected
        if(! rfbHandshake(rfbsec))
            return -1;

        // rfb thread: process rfb messages
        auto thrfb = std::thread([=]()
        {
            try
            {
                this->rfbMessagesLoop();
            }
            catch(const std::exception & err)
            {
                Application::error("%s: exception: %s", "start", err.what());
            }
            catch(...)
            {
            }

            this->rfbMessagesShutdown();
        });

        // xcb thread: wait xkb event
        auto thxcb = std::thread([this]()
        {
            while(this->rfbMessagesRunning())
            {
                if(! xcbEventProcessing())
                    break;

                std::this_thread::sleep_for(200ms);
            }
        });

        // main thread: sdl processing
        SDL_Event ev;

        auto clipDelay = std::chrono::steady_clock::now();
        std::shared_ptr<char> clipBoard(SDL_GetClipboardText(), SDL_free);

        if(isContinueUpdatesSupport())
            sendContinuousUpdates(true, { XCB::Point(0,0), clientSize() });

        while(true)
        {
            if(! rfbMessagesRunning())
                break;

            if(xcbError())
                break;

#ifdef LTSM_CHANNELS
            if(! dropFiles.empty() &&
                std::chrono::steady_clock::now() - dropStart > 700ms)
            {
                ChannelClient::sendSystemTransferFiles(dropFiles);
                dropFiles.clear();
            }
#endif
            // send clipboard
            if(std::chrono::steady_clock::now() - clipDelay > 300ms &&
                ! focusLost && SDL_HasClipboardText())
            {
                std::thread([clip = clipBoard, ptr = SDL_GetClipboardText(), this]() mutable
                {
                    if(! clip || std::strcmp(clip.get(), ptr))
                    {
                        this->sendCutTextEvent(ptr, SDL_strlen(ptr));
                        clip.reset(ptr);
                    }
                    else
                    {
                        SDL_free(ptr);
                    }
                }).detach();

                clipDelay = std::chrono::steady_clock::now();
            }

            if(! SDL_PollEvent(& ev))
            {
                std::this_thread::sleep_for(5ms);
                continue;
            }

            if(! sdlEventProcessing(& ev))
            {
                std::this_thread::sleep_for(5ms);
                continue;
            }

        }

        rfbMessagesShutdown();

        if(thrfb.joinable())
            thrfb.join();

        if(thxcb.joinable())
            thxcb.join();

        return 0;
    }

    void Vnc2SDL::sendMouseState(void)
    {
        int posx, posy;
        uint8_t buttons = SDL_GetMouseState(& posx, & posy);
        //auto [coordX, coordY] = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
        sendPointerEvent(buttons, posx, posy);
    }

    void Vnc2SDL::exitEvent(void)
    {
        RFB::ClientDecoder::rfbMessagesShutdown();
    }

    bool Vnc2SDL::sdlEventProcessing(const SDL::GenericEvent & ev)
    {
        const std::scoped_lock<std::mutex> lock(lockRender);

        switch(ev.type())
        {
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                sendMouseState();
                break;

            case SDL_MOUSEWHEEL:
                // scroll up
                if(ev.wheel()->y > 0)
                {
                    // press
                    int posx, posy;
                    uint8_t buttons = SDL_GetMouseState(& posx, & posy);
                    sendPointerEvent(0x08 | buttons, posx, posy);
                    // release
                    sendMouseState();
                }
                else
                if(ev.wheel()->y < 0)
                {
                    // press
                    int posx, posy;
                    uint8_t buttons = SDL_GetMouseState(& posx, & posy);
                    sendPointerEvent(0x10 | buttons, posx, posy);
                    // release
                    sendMouseState();
                }
                break;

            case SDL_WINDOWEVENT:
                if(SDL_WINDOWEVENT_EXPOSED == ev.window()->event)
                    window->renderPresent();
                if(SDL_WINDOWEVENT_FOCUS_GAINED == ev.window()->event)
                    focusLost = false;
                else
                if(SDL_WINDOWEVENT_FOCUS_LOST == ev.window()->event)
                    focusLost = true;
                break;

            case SDL_KEYDOWN:
                // ctrl + F10 -> fast close
                if(ev.key()->keysym.sym == SDLK_F10 &&
                    (KMOD_CTRL & SDL_GetModState()))
                {
                    exitEvent();
                    return true;
                }
                // key press delay 200 ms
                if(std::chrono::steady_clock::now() - keyPress < 200ms)
                {
                    keyPress = std::chrono::steady_clock::now();
                    break;
                }
                // continue
            case SDL_KEYUP:
                if(ev.key()->keysym.sym == 0x40000000 && ! capslock)
                {
                    auto mod = SDL_GetModState();
                    SDL_SetModState(static_cast<SDL_Keymod>(mod & ~KMOD_CAPS));
                    Application::notice("%s: CAPS reset", __FUNCTION__);
                    return true;
                }
                else
                {
                    int xksym = SDL::Window::convertScanCodeToKeySym(ev.key()->keysym.scancode);
                    if(xksym == 0) xksym = ev.key()->keysym.sym;

                    if(usexkb)
                    {
                        auto keycodeGroup = keysymToKeycodeGroup(xksym);
                        int group = xkbGroup();

                        if(group != keycodeGroup.second)
                            xksym = keycodeGroupToKeysym(keycodeGroup.first, group);
                    }

                    sendKeyEvent(ev.type() == SDL_KEYDOWN, xksym);
                }
                break;

            case SDL_DROPFILE:
                {
                    std::unique_ptr<char, void(*)(void*)> drop{ ev.drop()->file, SDL_free };
                    dropFiles.emplace_back(drop.get());
                    dropStart = std::chrono::steady_clock::now();
                }
                break;

            case SDL_USEREVENT:
                {
                    // resize event
                    if(ev.user()->code == 777 || ev.user()->code == 775)
                    {
                        XCB::Size sz;
                        sz.width = (size_t) ev.user()->data1;
                        sz.height = (size_t) ev.user()->data2;
                        bool contUpdateResume = ev.user()->code == 777;

                        fb.reset(new FrameBuffer(sz, format));

                        if(fullscreen)
                            window.reset(new SDL::Window("VNC2SDL", sz.width, sz.height,
                                    0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP, accelerated));
                        else
                            window->resize(sz.width, sz.height);

                        if(contUpdateResume)
                            sendContinuousUpdates(true, {0, 0, sz.width, sz.height});
                        else
                            sendFrameBufferUpdate(false);
                    }
                }
                break;

            case SDL_QUIT:
                exitEvent();
                return true;
        }

        return false;
    }

    void Vnc2SDL::decodingExtDesktopSizeEvent(uint16_t status, uint16_t err, const XCB::Size & sz, const std::vector<RFB::ScreenInfo> & screens)
    {
        // 1. server info: status: 0x00, error: 0x00
        if(status == 0 && err == 0)
        {
            bool resize = false;

            auto [ winWidth, winHeight ] = window->geometry();
            
            if(setWidth && setHeight)
            {
                // 2. send client size
                sendSetDesktopSize(setWidth, setHeight);
            }
            else
            if(winWidth != sz.width || winHeight != sz.height)
            {
                // 2. send client size
                sendSetDesktopSize(sz.width, sz.height);
            } 
        }
        else
        // 3. server reply
        if(status == 1)
        {
            if(0 == err)
            {
                if(setWidth && setHeight &&
                    setWidth == sz.width && setHeight == sz.height)
                    setWidth = setHeight = 0;

                bool contUpdateResume = false;

                if(isContinueUpdatesProcessed() && fb)
                {
                    sendContinuousUpdates(false, XCB::Region(0, 0, fb->width(), fb->height()));
                    contUpdateResume = true;
                }

                // create event for resize (resized in main thread)
                SDL_Event event;
                event.type = SDL_USEREVENT;
                event.user.code = contUpdateResume ? 777 : 775;
                event.user.data1 = (void*)(ptrdiff_t) sz.width;
                event.user.data2 = (void*)(ptrdiff_t) sz.height;
    
                if(0 > SDL_PushEvent(& event))
                    Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_PushEvent", SDL_GetError());
            }
            else
            {
                Application::info("%s: status: %d, error code: %d", __FUNCTION__, status, err);
            }
        }
    }

    void Vnc2SDL::fbUpdateEvent(void)
    {
        const std::scoped_lock<std::mutex> lock(lockRender);

        SDL_Rect area{ .x = dirty.x, .y = dirty.y, .w = dirty.width, .h = dirty.height };
        if(0 != SDL_UpdateTexture(window->display(), & area, fb->pitchData(dirty.y) + dirty.x * fb->bytePerPixel(), fb->pitchSize()))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_UpdateTexture", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        window->renderPresent();
        dirty.reset();
    }

    void Vnc2SDL::pixelFormatEvent(const PixelFormat & pf, uint16_t width, uint16_t height) 
    {
        Application::info("%s: width: %d, height: %d", __FUNCTION__, width, height);

        const std::scoped_lock<std::mutex> lock(lockRender);

        if(! window)
            window.reset(new SDL::Window("VNC2SDL", width, height, 0, 0, 0, accelerated));

        if(setWidth && setHeight)
        {
            auto [ winWidth, winHeight ] = window->geometry();
            if(setWidth == winWidth && setHeight == winHeight)
                setWidth = setHeight = 0;
        }

        int bpp;
        uint32_t rmask, gmask, bmask, amask;

        if(SDL_TRUE != SDL_PixelFormatEnumToMasks(window->pixelFormat(), &bpp, &rmask, &gmask, &bmask, &amask))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_PixelFormatEnumToMasks", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        bool contUpdateResume = false;

        if(isContinueUpdatesProcessed() && fb)
        {
            sendContinuousUpdates(false, XCB::Region(0, 0, fb->width(), fb->height()));
            contUpdateResume = true;
        }

        format = PixelFormat(bpp, rmask, gmask, bmask, amask);
        fb.reset(new FrameBuffer(XCB::Size(width, height), format));

        if(contUpdateResume)
            sendContinuousUpdates(true, {0, 0, width, height});
    }

    void Vnc2SDL::setPixel(const XCB::Point & dst, uint32_t pixel)
    {
        const std::scoped_lock<std::mutex> lock(lockRender);

        fb->setPixel(dst, pixel);
        dirty.join(dst.x, dst.y, 1, 1);
    }

    void Vnc2SDL::fillPixel(const XCB::Region & dst, uint32_t pixel)
    {
        const std::scoped_lock<std::mutex> lock(lockRender);

        fb->fillPixel(dst, pixel);
        dirty.join(dst);
    }

    const PixelFormat & Vnc2SDL::clientPixelFormat(void) const
    {
        return format;
    }

    XCB::Size Vnc2SDL::clientSize(void) const
    {
        auto [ width, height ] = window->geometry();
        return XCB::Size(width, height);
    }

    void Vnc2SDL::cutTextEvent(std::vector<uint8_t> && buf)
    {
        buf.push_back(0);
        if(0 != SDL_SetClipboardText(reinterpret_cast<const char*>(buf.data())))
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_SetClipboardText", SDL_GetError());
    }

#ifdef LTSM_CHANNELS
    void Vnc2SDL::decodingLtsmEvent(const std::vector<uint8_t> &)
    {
        if(! sendOptions)
        {
            auto names = xkbNames();
            auto group = xkbGroup();

            sendSystemClientVariables(clientOptions(), clientEnvironments(), names, (0 <= group && group < names.size() ? names[group] : ""));
            sendOptions = true;
        }
    }

    void Vnc2SDL::xkbStateChangeEvent(int group)
    {
        if(usexkb)
            sendSystemKeyboardChange(xkbNames(), group);
    }

    json_plain Vnc2SDL::clientEnvironments(void) const
    {
        // locale
        std::initializer_list<std::pair<int, std::string>> lcall = { { LC_CTYPE, "LC_TYPE" }, { LC_NUMERIC, "LC_NUMERIC" }, { LC_TIME, "LC_TIME" },
                                        { LC_COLLATE, "LC_COLLATE" }, { LC_MONETARY, "LC_MONETARY" }, { LC_MESSAGES, "LC_MESSAGES" } };
        JsonObjectStream jo;

        for(auto & lc : lcall)
        {
            auto ptr = std::setlocale(lc.first, "");
            jo.push(lc.second, ptr ? ptr : "C");
        }
        // lang
        auto lang = std::getenv("LANG");
        jo.push("LANG", lang ? lang : "C");

        // timezone
        jo.push("TZ", Tools::getTimeZone());

        return jo.flush();
    }

    json_plain Vnc2SDL::clientOptions(void) const
    {
        // other
        JsonObjectStream jo;
        jo.push("hostname", "localhost");
        jo.push("ipaddr", "127.0.0.1");
        jo.push("username", Tools::getLocalUsername());
        jo.push("platform", SDL_GetPlatform());

        if(! rfbsec.certFile.empty())
        {
            jo.push("certificate", Tools::fileToString(rfbsec.certFile));
        }

        if(! printerUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "printer", printerUrl.c_str());
            jo.push("printer", printerUrl);
        }

        if(! saneUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "sane", saneUrl.c_str());
            jo.push("sane", saneUrl);
        }

        if(! pcscdUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "pcscd", pcscdUrl.c_str());
            jo.push("pcscd", pcscdUrl);
        }

        if(! pulseUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "pulse", pulseUrl.c_str());
            jo.push("pulseaudio", pulseUrl);
        }

//       jo.push("programVersion", "XXXXX");
//       jo.push("programName", "XXXXX");

        return jo.flush();
    }

#endif
}

int main(int argc, const char** argv)
{
    int res = 0;

    if(0 > SDL_Init(SDL_INIT_VIDEO))
    {
        std::cerr << "sdl init video failed" << std::endl;
        return -1;
    }

    try
    {
        LTSM::Vnc2SDL app(argc, argv);
        res = app.start();
    }
    catch(const std::invalid_argument & err)
    {
        std::cerr << "unknown params: " << err.what() << std::endl;
        LTSM::printHelp(argv[0]);
        return -1;
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("exception: %s", err.what());
        LTSM::Application::info("program: %s", "terminate...");
    }
    catch(int val)
    {
        res = val;
    }

    SDL_Quit();
    return res;
}
