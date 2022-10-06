/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

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
        std::cout << "usage: " << prog << " --host <localhost> [--port 5900] [--password <pass>] [--notls] [--debug] [--tls_priority <string>] [--fullscreen] [--geometry WIDTHxHEIGHT] [--certificate <path>] [--accel] [--printer <target>]" << std::endl;
    }

    Vnc2SDL::Vnc2SDL(int argc, const char** argv)
        : Application("ltsm_vnc2sdl", argc, argv)
    {
        Application::setDebugLevel(DebugLevel::SyslogInfo);

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
            if(0 == std::strcmp(argv[it], "--accel"))
                accelerated = true;
            else
            if(0 == std::strcmp(argv[it], "--notls"))
                notls = true;
            else
            if(0 == std::strcmp(argv[it], "--fullscreen"))
                fullscreen = true;
            else
            if(0 == std::strcmp(argv[it], "--printer"))
                printer = true;
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
            if(0 == std::strcmp(argv[it], "--tls_priority") && it + 1 < argc)
            {
                priority.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--password") && it + 1 < argc)
            {
                password.assign(argv[it + 1]);
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
            if(0 == std::strcmp(argv[it], "--certificate") && it + 1 < argc)
            {
                std::filesystem::path file(argv[it + 1]);

                if(std::filesystem::exists(file))
                {
                    std::ifstream ifs(file, std::ios::binary);
                    if(ifs.is_open())
                    {
                        auto fsz = std::filesystem::file_size(file);
                        certificate.resize(fsz);
                        ifs.read(certificate.data(), certificate.size());
                        ifs.close();
                    }
                    else
                    {
                        std::cerr << "error read certificate" << std::endl;
                    }
                }
                else
                {
                    std::cerr << "certificate not found" << std::endl;
                }

                it = it + 1;
            }
            else
            {
                std::cerr << "unknown params: " << argv[it] << std::endl;
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

        // connected
        if(! rfbHandshake(! notls, priority, password))
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
                Application::error("%s: exception: %s", __FUNCTION__, err.what());
            }
            catch(...)
            {
            }

            this->rfbMessagesShutdown();
        });

        // clipboard thread: check clipboard
        auto thclip = std::thread([this]()
        {
            std::unique_ptr<char, void(*)(void*)> clip{ SDL_GetClipboardText(), SDL_free };

            while(this->rfbMessagesRunning())
            {
                if(SDL_HasClipboardText())
                {
                    std::unique_ptr<char, void(*)(void*)> buf{ SDL_GetClipboardText(), SDL_free };
                    if(! clip || std::strcmp(clip.get(), buf.get()))
                    {
                        auto len = SDL_strlen(buf.get());
                        this->sendCutTextEvent(buf.get(), len);
                        clip.reset(buf.release());
                    }
                }

                std::this_thread::sleep_for(300ms);
            }
        });

        // xcb thread: wait xkb event
        auto thxcb = std::thread([this]()
        {
            while(this->rfbMessagesRunning())
            {
                xcbEventProcessing();
                std::this_thread::sleep_for(300ms);
            }
        });

        // main thread: sdl processing
        SDL_Event ev;

        while(true)
        {
            if(! rfbMessagesRunning())
                break;

            if(xcbError())
                break;

            if(! SDL_PollEvent(& ev))
            {
                std::this_thread::sleep_for(1ms);
                continue;
            }

#ifdef LTSM_CHANNELS
            if(! dropFiles.empty() &&
                std::chrono::steady_clock::now() - dropStart > 700ms)
            {
                ChannelClient::sendSystemTransferFiles(dropFiles);
                dropFiles.clear();
            }
#endif

            if(! sdlEventProcessing(& ev))
            {
                std::this_thread::sleep_for(1ms);
                continue;
            }
        }

        rfbMessagesShutdown();

        if(thrfb.joinable())
            thrfb.join();

        if(thxcb.joinable())
            thxcb.join();

        if(thclip.joinable())
            thclip.join();

        return 0;
    }

    void Vnc2SDL::sendMouseState(void)
    {
        int posx, posy;
        uint8_t buttons = SDL_GetMouseState(& posx, & posy);
        //auto [coordX, coordY] = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
        sendPointerEvent(buttons, posx, posy);
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
/*
                if(SDL_WINDOWEVENT_SIZE_CHANGED == current.window.event)
                    resizeWindow(Size(current.window.data1, current.window.data2));
*/
                break;

            case SDL_KEYDOWN:
                // ctrl + F10 -> fast close
                if(ev.key()->keysym.sym == SDLK_F10 &&
                    (KMOD_CTRL & SDL_GetModState()))
                {
                    RFB::ClientDecoder::rfbMessagesShutdown();
                    return true;
                }
                // key press delay 300 ms
                if(std::chrono::steady_clock::now() - keyPress < 300ms)
                {
                    std::this_thread::sleep_for(300ms);
                    keyPress = std::chrono::steady_clock::now();
                }
                // continue
            case SDL_KEYUP:
                {
                    int xksym = SDL::Window::convertScanCodeToKeySym(ev.key()->keysym.scancode);
                    if(xksym == 0) xksym = ev.key()->keysym.sym;

                    auto keycodeGroup = keysymToKeycodeGroup(xksym);
                    int group = xkbGroup();

                    if(group != keycodeGroup.second)
                        xksym = keycodeGroupToKeysym(keycodeGroup.first, group);

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

            case SDL_QUIT:
                RFB::ClientDecoder::rfbMessagesShutdown();
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

                const std::scoped_lock<std::mutex> lock(lockRender);
                bool contUpdateResume = false;

                if(isContinueUpdates() && fb)
                {
                    sendContinuousUpdates(false, XCB::Region(0, 0, fb->width(), fb->height()));
                    contUpdateResume = true;
                }

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

        if(isContinueUpdates() && fb)
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
        fb->setPixel(dst, pixel);
        dirty.join(dst.x, dst.y, 1, 1);
    }

    void Vnc2SDL::fillPixel(const XCB::Region & dst, uint32_t pixel)
    {
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
            sendSystemClientVariables(clientOptions(), clientEnvironments(), xkbNames());
            sendOptions = true;
        }
    }

    void Vnc2SDL::xkbStateChangeEvent(int group)
    {
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
        jo.push("username", Tools::getUsername());
        jo.push("platform", SDL_GetPlatform());

        if(printer)
            jo.push("printer", "socket://127.0.0.1:9100");

//        jo.push("redirectaudio", false);
//        jo.push("redirectmicrofone", false);
//        jo.push("redirectvideo", false);
//        jo.push("redirectscanner", false);

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
        return -1;

    try
    {
        LTSM::Vnc2SDL app(argc, argv);
        res = app.start();
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("local exception: %s", err.what());
        LTSM::Application::info("program: %s", "terminate...");
    }
    catch(int val)
    {
        res = val;
    }

    SDL_Quit();
    return res;
}
