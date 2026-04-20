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

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <filesystem>

#include <boost/asio.hpp>
#include "xcb/damage.h"

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_sdl_wrapper.h"
#include "ltsm_xcb_wrapper.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM {
    class SDL2X11 : protected XCB::RootDisplay, protected SDL::Window, public XCB::SelectionSource, public XCB::SelectionRecipient {
        asio::io_context ioc_;
        boost::asio::signal_set signals_{ioc_};
        asio::strand<boost::asio::any_io_executor> sdl_strand_{ioc_.get_executor()};
        asio::strand<boost::asio::any_io_executor> x11_strand_{ioc_.get_executor()};
        asio::cancellation_signal sdl_cancel_;
        asio::cancellation_signal x11_cancel_;

        XCB::Region damage_;
        XCB::ShmIdShared shm_;

        std::unique_ptr<char, void(*)(void*)> clientClipboard{ nullptr, SDL_free };

      public:
        SDL2X11(const char* title, const XCB::Size & winsz, bool accel)
            : XCB::RootDisplay(-1), SDL::Window(title, XCB::RootDisplay::size(), winsz, 0, accel) {
        }

        /*
                void xcbShmInit(uid_t uid)
                {
                    if(auto ext = static_cast<const XCB::ModuleShm*>(XCB::RootDisplay::getExtension(XCB::Module::SHM)))
                    {
                        auto dsz = XCB::RootDisplay::size();
                        auto bpp = XCB::RootDisplay::bitsPerPixel() >> 3;

                        shm_ = ext->createShm(dsz.width * dsz.height * bpp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, false, uid);
                    }
                }
        */

        void xcbDamageNotifyEvent(const xcb_rectangle_t & rt, uint8_t level) override {
            damage_.join(rt.x, rt.y, rt.width, rt.height);
        }

        void xcbRandrScreenChangedEvent(const XCB::Size & dsz, const xcb_randr_notify_event_t & ne) override {
            SDL::Window::resize(dsz);
            damage_.assign(0, 0, dsz.width, dsz.height);
        }

        void xcbXkbGroupChangedEvent(int group) override {
        }

        void selectionReceiveData(xcb_atom_t atom, std::span<const uint8_t> buf) const override {
            SDL_SetClipboardText(std::string(buf.begin(), buf.end()).c_str());
        }

        void selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) const override {
            auto targets = selectionSourceTargets();

            auto ptr = const_cast<SDL2X11*>(this);

            if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY))) {
                std::for_each(beg, end, [&](auto & atom) {
                    if(std::ranges::any_of(targets, [&](auto & trgt) { return atom == trgt; })) {
                        return copy->convertSelection(atom, *this);
                    }
                });
            }
        }

        void selectionChangedEvent(void) const override {
            auto ptr = const_cast<SDL2X11*>(this);

            if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY))) {
                auto targets = getAtom("TARGETS");
                copy->convertSelection(targets, *this);
            }
        }

        std::vector<xcb_atom_t> selectionSourceTargets(void) const override {
            auto utf8String = getAtom("UTF8_STRING");
            auto text = getAtom("TEXT");
            auto textPlain = getAtom("text/plain;charset=utf-8");

            return { XCB_ATOM_STRING, utf8String, text, textPlain };
        }

        size_t selectionSourceSize(xcb_atom_t atom) const override {
            auto targets = selectionSourceTargets();

            if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
                return 0;
            }

            return clientClipboard ? SDL_strlen(clientClipboard.get()) : 0;
        }

        std::vector<uint8_t> selectionSourceData(xcb_atom_t atom, size_t offset, uint32_t length) const override {
            auto targets = selectionSourceTargets();

            if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
                return {};
            }

            if(clientClipboard) {
                auto len = SDL_strlen(clientClipboard.get());

                if(offset + length <= len) {
                    auto beg = clientClipboard.get() + offset;
                    return std::vector<uint8_t>(beg, beg + length);
                } else {
                    Application::error("{}: invalid length: {}, offset: {}", NS_FuncNameV, length, offset);
                }
            }

            return {};
        }

        void sdlKeyEvent(const SDL_KeyboardEvent* ev) {
            // fast close
            if(ev->keysym.sym == SDLK_ESCAPE) {
                Application::warning("{}: {}", NS_FuncNameV, "escape quit");
                throw system::system_error(asio::error::operation_aborted);
            }

            asio::post(x11_strand_, [this, keysym = ev->keysym, pressed = (ev->type == SDL_KEYDOWN)](){
                if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST))) {
                    int xksym = SDL::Window::convertScanCodeToKeySym(keysym.scancode);
                    auto keycode = XCB::RootDisplay::keysymToKeycode(0 != xksym ? xksym : keysym.sym);
                    test->screenInputKeycode(keycode, pressed);
                }
            });
        }

        void sdlMouseButtonEvent(const SDL_MouseButtonEvent* ev) {
            auto [coordX, coordY] = SDL::Window::scaleCoord(ev->x, ev->y);
            asio::post(x11_strand_, [this, button = ev->button, pos = XCB::Point(coordX, coordY), pressed = (ev->type == SDL_MOUSEBUTTONDOWN)](){
                if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST))) {
                    test->screenInputButton(button, pos, pressed);
                }
            });
        }

        void sdlMouseMotionEvent(const SDL_MouseButtonEvent* ev) {
            auto [coordX, coordY] = SDL::Window::scaleCoord(ev->x, ev->y);
            asio::post(x11_strand_, [this, pos = XCB::Point(coordX, coordY)](){
                if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST))) {
                    test->screenInputMove(pos);
                }
            });
        }

        void sdlMouseWheelEvent(const SDL_MouseWheelEvent* ev) {
            int coordX, coordY;
            SDL_GetMouseState(& coordX, & coordY);
            asio::post(x11_strand_, [this, wheelY = ev->y, pos = XCB::Point(coordX, coordY)](){
                if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtension(XCB::Module::TEST))) {
                    if(wheelY > 0) {
                        test->screenInputButtonClick(4, pos);
                    } else if(wheelY < 0) {
                        test->screenInputButtonClick(5, pos);
                    }
                }
            });
        }

        void sdlClipboardEvent(void) {
            if(clientClipboard.reset(SDL_GetClipboardText()); !!clientClipboard) {
                asio::post(x11_strand_, [this](){
                    if(auto paste = static_cast<XCB::ModulePasteSelection*>(XCB::RootDisplay::getExtension(XCB::Module::SELECTION_PASTE))) {
                        paste->setSelectionOwner(*this);
                    }
                });
            }
        }

        bool sdlEventProcessing(void) {
            auto ev = SDL::Window::pollEvent();

            if(! ev.isValid()) {
                return false;
            }

            switch(ev.type()) {
                case SDL_TEXTINPUT:
                    // handleTextInput: ev.text
                    break;

                case SDL_WINDOWEVENT:
                    if(ev.window()->event == SDL_WINDOWEVENT_EXPOSED) {
                        renderPresent(false);
                    }

                    break;

                case SDL_KEYUP:
                case SDL_KEYDOWN:
                    sdlKeyEvent(ev.key());
                    break;

                case SDL_MOUSEBUTTONUP:
                case SDL_MOUSEBUTTONDOWN:
                    sdlMouseButtonEvent(ev.button());
                    break;

                case SDL_MOUSEMOTION:
                    sdlMouseMotionEvent(ev.button());
                    break;

                case SDL_MOUSEWHEEL:
                    sdlMouseWheelEvent(ev.wheel());
                    break;

                case SDL_CLIPBOARDUPDATE:
                    if(SDL_HasClipboardText()) {
                        sdlClipboardEvent();
                    }
                    break;

                case SDL_QUIT:
                    Application::warning("{}: {}", NS_FuncNameV, "sdl quit");
                    throw system::system_error(asio::error::operation_aborted);

                default:
                    break;
            }

            return true;
        }

        asio::awaitable<void> sdlEventsLoop(void) {
            auto ex = co_await asio::this_coro::executor;
            asio::steady_timer timer{ex};

            try {
                for(;;) {
                    while(sdlEventProcessing()) {
                        std::this_thread::yield();
                    }

                    timer.expires_after(40ms);
                    co_await timer.async_wait(asio::use_awaitable);
                }
            } catch(const system::system_error& err) {
                if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                    Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                }
                asio::post(ioc_, std::bind(&SDL2X11::stop, this));
            }
        }

        asio::awaitable<void> renderProcess(void) {
            co_await asio::dispatch(x11_strand_, asio::use_awaitable);
            const size_t bytePerPixel = bitsPerPixel() >> 3;

            if(auto reply = copyRootImageRegion(damage_)) {
                const size_t alignRowBytes = reply->size() > (damage_.width * damage_.height * bytePerPixel) ?
                                                     reply->size() / damage_.height - damage_.width * bytePerPixel : 0;

                co_await asio::dispatch(sdl_strand_, asio::use_awaitable);

                SDL_Rect dstrt = { damage_.x, damage_.y, damage_.width, damage_.height };
                auto format = SDL_MasksToPixelFormatEnum(reply->bitsPerPixel(), reply->rmask, reply->gmask, reply->bmask, 0);

                if(SDL_PIXELFORMAT_UNKNOWN == format) {
                    Application::error("{}: %s failed", NS_FuncNameV, "format");
                    throw system::system_error(asio::error::operation_aborted);
                }

                if(auto tx = createTexture(damage_.toSize(), format); tx.isValid()) {
                    tx.updateRect(nullptr, reply->data(), damage_.width * bytePerPixel + alignRowBytes);
                    renderTexture(tx.get(), nullptr, nullptr, & dstrt);
                    renderPresent();
                }

                co_await asio::dispatch(x11_strand_, asio::use_awaitable);
            }
        }

        asio::awaitable<void> x11EventsLoop(void) {
            auto ex = co_await asio::this_coro::executor;
            asio::posix::stream_descriptor sd{ex, XCB::RootDisplay::getFd()};

            try {
                for(;;) {
                    if(auto err = XCB::RootDisplay::hasError()) {
                        Application::error("{}: xcb error, code: {}", NS_FuncNameV, err);
                        throw system::system_error(asio::error::operation_aborted);
                    }

                    co_await sd.async_wait(asio::posix::stream_descriptor::wait_read, asio::use_awaitable);

                    while(auto ev = XCB::RootDisplay::pollEvent()) {
                        std::this_thread::yield();
                    }

                    if(damage_.isEmpty()) {
                        continue;
                    }

                    co_await renderProcess();
                    XCB::RootDisplay::rootDamageSubtrack(damage_);
                    damage_.reset();
                }
            } catch(const system::system_error& err) {
                if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                    Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                }
                asio::post(ioc_, std::bind(&SDL2X11::stop, this));
            }

            sd.release();
        }

        asio::awaitable<void> signalsHandler(void) {
            signals_.add(SIGTERM);
            signals_.add(SIGINT);

            try {
                for(;;) {
                    int signal = co_await signals_.async_wait(asio::use_awaitable);
                    if(signal == SIGTERM || signal == SIGINT) {
                        asio::post(ioc_, std::bind(&SDL2X11::stop, this));
                        co_return;
                    }
                }
            } catch(const system::system_error& err) {
                if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                    Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            }
        }
    }

        void stop(void) {
            sdl_cancel_.emit(asio::cancellation_type::terminal);
            x11_cancel_.emit(asio::cancellation_type::terminal);
            signals_.cancel();
        }

        int start(void) {

            asio::co_spawn(ioc_, signalsHandler(), asio::detached);

            asio::co_spawn(sdl_strand_, sdlEventsLoop(),
                asio::bind_cancellation_slot(sdl_cancel_.slot(), asio::detached));

            asio::co_spawn(x11_strand_, x11EventsLoop(),
                asio::bind_cancellation_slot(x11_cancel_.slot(), asio::detached));

            // initial selection
            asio::post(x11_strand_, std::bind(&SDL2X11::selectionChangedEvent, this));

            ioc_.run();

            return EXIT_SUCCESS;
        }
    };
}

int printHelp(const char* prog) {
    std::cout << "usage: " << prog <<
                 " --display <:num> [--auth <xauthfile>] [--title <title>] [--scale <width>x<height>] [--accel] [--debug] [--syslog]" << std::endl;
    return EXIT_SUCCESS;
}

int main(int argc, const char** argv) {
    LTSM::XCB::Size winsz;
    bool accel = false;
    const char* xauth = nullptr;
    const char* display = nullptr;
    const char* title = "SDL2X11";

    LTSM::Application::setDebugTarget(LTSM::DebugTarget::Console);
    LTSM::Application::setDebugLevel(LTSM::DebugLevel::Info);

    if(auto val = getenv("SDL2X11_SCALE")) {
        size_t idx;

        try {
            winsz.width = std::abs(std::stoi(val, & idx, 0));
            winsz.height = std::abs(std::stoi(val + idx + 1, nullptr, 0));
        } catch(const std::invalid_argument &) {
            std::cerr << "invalid scale" << std::endl;
        }
    }

    for(int it = 1; it < argc; ++it) {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h")) {
            return printHelp(argv[0]);
        } else if(0 == std::strcmp(argv[it], "--debug")) {
            LTSM::Application::setDebugLevel(LTSM::DebugLevel::Debug);
        } else if(0 == std::strcmp(argv[it], "--accel")) {
            accel = true;
        } else if(0 == std::strcmp(argv[it], "--syslog")) {
            LTSM::Application::setDebugTarget(LTSM::DebugTarget::Syslog, "ltsm_sdl2x11");
        } else if(0 == std::strcmp(argv[it], "--auth") && it + 1 < argc) {
            xauth = argv[it + 1];
        } else if(0 == std::strcmp(argv[it], "--title") && it + 1 < argc) {
            title = argv[it + 1];
        } else if(0 == std::strcmp(argv[it], "--scale") && it + 1 < argc) {
            const char* val = argv[it + 1];
            size_t idx;

            try {
                winsz.width = std::abs(std::stoi(val, & idx, 0));
                winsz.height = std::abs(std::stoi(val + idx + 1, nullptr, 0));
            } catch(const std::invalid_argument &) {
                std::cerr << "invalid scale" << std::endl;
                return printHelp(argv[0]);
            }
        } else if(0 == std::strcmp(argv[it], "--display") && it + 1 < argc) {
            display = argv[it + 1];

        }
    }

    if(! display) {
        std::cerr << "display not found" << std::endl;
        return printHelp(argv[0]);
    } else if(display[0] != ':') {
        std::cerr << "invalid display format: " << display << std::endl;
        return printHelp(argv[0]);
    }

    if(xauth &&
        !std::filesystem::exists(xauth)) {
        std::cerr << "xauth not exists: " << xauth << std::endl;
        return -1;
    }

    if(0 > SDL_Init(SDL_INIT_VIDEO)) {
        return -1;
    }

    if(display) {
        setenv("DISPLAY", display, 1);
    }

    if(xauth) {
        setenv("XAUTHORITY", xauth, 1);
    }

    try {
        return LTSM::SDL2X11(title, winsz, accel).start();
    } catch(const std::exception & err) {
        std::cerr << "exception: " << err.what() << std::endl;
    }

    SDL_Quit();
    return 0;
}
