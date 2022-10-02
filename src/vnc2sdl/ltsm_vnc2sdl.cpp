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
        std::cout << "usage: " << prog << " --host <localhost> [--port 5900] [--password <pass>] [--notls] [--debug] [--priority <string>] [--fullscreen] [--geometry WIDTHxHEIGHT] [--certificate <path>] [--accel] [--printer <target>]" << std::endl;
    }

    const char* findParam(int argc, const char** argv, std::string_view param, bool single)
    {
        auto beg = & argv[0];
        auto end = beg + argc;

        auto it = std::find_if(beg, end, [&](auto & ptr){ return param == ptr; });
        if(it == end)
            return nullptr;

        return single ? *it : *std::next(it);
    }

    Vnc2SDL::Vnc2SDL(int argc, const char** argv)
        : Application("ltsm_vnc2sdl", argc, argv)
    {
        Application::setDebugLevel(DebugLevel::SyslogInfo);

        if(auto ptr = findParam(argc, argv, "--help", true))
        {
            printHelp(argv[0]);
            throw 0;
        }

        if(auto ptr = findParam(argc, argv, "--accel", true))
            accelerated = true;

        if(auto ptr = findParam(argc, argv, "--notls", true))
            notls = true;

        if(auto ptr = findParam(argc, argv, "--fullscreen", true))
            fullscreen = true;

        if(auto ptr = findParam(argc, argv, "--debug", true))
            Application::setDebugLevel(DebugLevel::Console);

        if(auto ptr = findParam(argc, argv, "--host", false))
            host.assign(ptr);

        if(auto ptr = findParam(argc, argv, "--priority", false))
            priority.assign(ptr);

        if(auto ptr = findParam(argc, argv, "--password", false))
            password.assign(ptr);

        if(auto ptr = findParam(argc, argv, "--printer", false))
            printer.assign(ptr);

        try
        {
            if(auto ptr = findParam(argc, argv, "--port", false))
                port = std::stoi(ptr);
        }
        catch(const std::invalid_argument &)
        {
            std::cerr << "incorrect port number" << std::endl;
            port = 5900;
        }

        if(auto ptr = findParam(argc, argv, "--geometry", false))
        {
            size_t idx;

            try
            {
                setWidth = std::stoi(ptr, & idx, 0);
                setHeight = std::stoi(ptr + idx + 1, nullptr, 0);
            }
            catch(const std::invalid_argument &)
            {
                std::cerr << "invalid geometry" << std::endl;
                setWidth = setHeight = 0;
            }
        }

        if(auto ptr = findParam(argc, argv, "--certificate", false))
        {
            std::filesystem::path file(ptr);

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
        }
    }

    int Vnc2SDL::start(void)
    {
        auto ipaddr = TCPSocket::resolvHostname(host);
        int sockfd = TCPSocket::connect(ipaddr, port);

        if(0 == sockfd)
            return -1;

        RFB::ClientDecoder::setSocketStreamMode(sockfd);

        if(! rfbHandshake(! notls, priority, password))
            return -1;

        // process rfb messages background
        auto th = std::thread([=]()
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

        // sdl event
        SDL::GenericEvent ev(nullptr);

        while(rfbMessagesProcessing())
        {
            bool sleep = false;

            if(! xcbEventProcessing())
                break;

            if(1)
            {
                const std::scoped_lock<std::mutex> lock(lockRender);
                ev = window->poolEvent();

                if(! sdlEventProcessing(ev))
                    sleep = true;
            }

#ifdef LTSM_CHANNELS
            if(! dropFiles.empty() &&
                std::chrono::steady_clock::now() - dropStart > 1500ms)
            {
                ChannelClient::sendSystemTransferFiles(dropFiles);
                dropFiles.clear();
                sleep = false;
            }
#endif
            if(sleep)
                std::this_thread::sleep_for(1ms);
        }

        if(th.joinable())
            th.join();

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
        if(! ev.isValid())
            return false;

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
                if(ev.key()->keysym.sym == SDLK_ESCAPE)
                {
                    // fast close
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

            case SDL_CLIPBOARDUPDATE:
                if(SDL_HasClipboardText())
                {
                    clipboard.reset(SDL_GetClipboardText());

                    if(clipboard)
                    {
                        auto len = SDL_strlen(clipboard.get());
                        sendCutTextEvent(clipboard.get(), len);
                    }
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
            sendSystemClientVariables(clientOptions(), clientEnvironments(), clientKeyboardLayouts());
            sendOptions = true;
        }
    }

    void Vnc2SDL::xkbStateChangeEvent(int group)
    {
        sendSystemKeyboardChange(xkbNames(), group);
    }

    JsonObject Vnc2SDL::clientEnvironments(void) const
    {
        // locale
        std::initializer_list<std::pair<int, std::string>> lcall = { { LC_CTYPE, "LC_TYPE" }, { LC_NUMERIC, "LC_NUMERIC" }, { LC_TIME, "LC_TIME" },
                                        { LC_COLLATE, "LC_COLLATE" }, { LC_MONETARY, "LC_MONETARY" }, { LC_MESSAGES, "LC_MESSAGES" } };
        JsonObject jo;

        for(auto & lc : lcall)
        {
            auto ptr = std::setlocale(lc.first, "");
            jo.addString(lc.second, ptr ? ptr : "C");
        }
        // lang
        auto lang = std::getenv("LANG");
        jo.addString("LANG", lang ? lang : "C");

        // timezone
        jo.addString("TZ", Tools::getTimeZone());

        return jo;
    }

    JsonObject Vnc2SDL::clientOptions(void) const
    {
        // other
        JsonObject jo;
        jo.addString("hostname", "localhost");
        jo.addString("ipaddr", "127.0.0.1");
        jo.addString("username", Tools::getUsername());
        jo.addString("platform", SDL_GetPlatform());
        jo.addString("printer", printer);

//        jo.addBoolean("redirectaudio", false);
//        jo.addBoolean("redirectmicrofone", false);
//        jo.addBoolean("redirectvideo", false);
//        jo.addBoolean("redirectscanner", false);

//       jo.addString("programVersion", "XXXXX");
//       jo.addString("programName", "XXXXX");

        return jo;
    }

    JsonArray Vnc2SDL::clientKeyboardLayouts(void) const
    {
        return xkbNames();
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
