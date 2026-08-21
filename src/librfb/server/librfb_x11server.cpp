/***********************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#include <sys/stat.h>

#include <tuple>
#include <chrono>

#include "ltsm_application.h"
#include "ltsm_sdl_wrapper.h"
#include "librfb_x11server.h"

#ifdef LTSM_ENCODING_FFMPEG
#include "librfb_ffmpeg.h"
#endif

using namespace boost;
using namespace std::chrono_literals;

namespace LTSM {
    XCB::RootDisplay* RFB::X11Server::xcbDisplay(void) {
        return this;
    }

    XCB::Region RFB::X11Server::getClientRegion(void) const {
        return XCB::Rectangle(clientRegion_.load());
    }

    void RFB::X11Server::xcbFixesCursorChangedEvent(void) {
        clientUpdateCursor_ = isClientSupportedEncoding(ENCODING_RICH_CURSOR);
    }

    void RFB::X11Server::xcbDamageNotifyEvent(const xcb_rectangle_t & rt, uint8_t level) {
        damagePool_.push(rt);
    }

    XCB::Region RFB::X11Server::joinAllDamages(void) {
        XCB::Region res;
        // get all damages
        damagePool_.consume_all([&res](const auto& rt) {
            res.join(rt.x, rt.y, rt.width, rt.height);
        });
        return res;
    }

    void RFB::X11Server::xcbDisplayConnectedEvent(void) {
        ExtClip::x11AtomsUpdate(*this);

        if(xcbNoDamageOption()) {
            XCB::RootDisplay::extensionDisable(XCB::Module::DAMAGE);
        }

        if(rfbClipboardEnable()) {
            // init selection copy
            selectionChangedEvent();
        } else {
            XCB::RootDisplay::extensionDisable(XCB::Module::SELECTION_COPY);
            XCB::RootDisplay::extensionDisable(XCB::Module::SELECTION_PASTE);
        }
    }

    void RFB::X11Server::xcbRandrScreenSetSizeEvent(const XCB::Size & wsz) {
        Application::info("{}: size: {}", NS_FuncNameV, wsz);
        displayResizeProcessed_ = true;
    }

    void RFB::X11Server::xcbRandrScreenChangedEvent(const XCB::Size & wsz, const xcb_randr_notify_event_t & notify) {
        Application::info("{}: size: {}, sequence: {}", NS_FuncNameV, wsz, notify.sequence);

        serverRegion_.store(xcb_rectangle_t{0, 0, wsz.width, wsz.height});

        asio::co_spawn(xcb_strand(), [this, wsz, notify]() -> asio::awaitable<void> {
            co_await xcbShmInit(0, & wsz);

            displayResizeProcessed_ = false;
            serverDisplayResizedEvent(wsz);

            if(isClientSupportedEncoding(ENCODING_EXT_DESKTOP_SIZE)) {
                auto status = randrSequence == notify.sequence ?
                          RFB::DesktopResizeStatus::ClientSide : RFB::DesktopResizeStatus::ServerRuntime;

                co_await asio::post(rfb_strand(), asio::use_awaitable);

                if(status == RFB::DesktopResizeStatus::ServerRuntime) {
                    co_await sendEncodingDesktopResizeAwait(status, RFB::DesktopResizeError::NoError, wsz);
                    displayResizeEvent(wsz);
                } else if(displayResizeNegotiation_) {
                    // clientSide
                    co_await sendEncodingDesktopResizeAwait(status, RFB::DesktopResizeError::NoError, wsz);
                    displayResizeEvent(wsz);
                    displayResizeNegotiation_ = false;
                }
            }

            co_return;
	}, asio::detached);
    }

    void RFB::X11Server::rfbStop(void) {
        xcbDisableMessages(true);
        ServerEncoder::asioStop();

        rfb_cancel_.emit(asio::cancellation_type::terminal);
        xcb_cancel_.emit(asio::cancellation_type::terminal);
        srv_cancel_.emit(asio::cancellation_type::terminal);

        signals_.cancel();
        clipboard_ready_.cancel();
        force_update_.cancel();
    }

    asio::awaitable<void> RFB::X11Server::signalsHandler(void) {
        signals_.add(SIGTERM);
        signals_.add(SIGINT);
        signals_.add(SIGPIPE);

        try {
            for(;;) {
                int signal = co_await signals_.async_wait(asio::use_awaitable);
                if(signal == SIGPIPE) {
                    // ignore
                    continue;
                }
                if(signal == SIGTERM || signal == SIGINT) {
                    stop();
                    co_return;
                }
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            }
        }
    }

    asio::awaitable<void> RFB::X11Server::xcbEventsLoop(void) {
        try {
            auto ex = co_await asio::this_coro::executor;
            asio::posix::stream_descriptor sd{ex};

            for(;;) {
                if(! xcbAllowMessages()) {
                    asio::steady_timer xcb_check{ex, 50ms};
                    co_await xcb_check.async_wait(asio::use_awaitable);
                    continue;
                }

                if(auto err = XCB::RootDisplay::hasError()) {
                    Application::error("{}: xcb error, code: {}", NS_FuncNameV, err);
                    stop();
                    co_return;
                }

                sd.assign(XCB::RootDisplay::getFd());
                co_await sd.async_wait(asio::posix::stream_descriptor::wait_read, asio::use_awaitable);

                while(auto ev = XCB::RootDisplay::pollEvent()) {
                    if(auto extShm = XCB::RootDisplay::getExtension(XCB::Module::SHM)) {
                        uint16_t opcode = 0;

                        if(shm && extShm->isEventError(ev, & opcode)) {
                            Application::warning("{}: {} error: {:#06x}", NS_FuncNameV, "shm", opcode);
                            shm.reset();
                        }
                    } else if(auto extFixes = XCB::RootDisplay::getExtensionConst(XCB::Module::XFIXES)) {
                        uint16_t opcode = 0;

                        if(extFixes->isEventError(ev, & opcode)) {
                            Application::warning("{}: {} error: {:#06x}", NS_FuncNameV, "xfixes", opcode);
                        }
                    }
                }

                sd.release();
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                stop();
            }
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            stop();
        }

        co_return;
    }

    asio::awaitable<void> RFB::X11Server::rfbReceiveMessages(void) {
        try {
            Application::debug(DebugType::Rfb, "{}: wait remote messages...", NS_FuncNameV);
            for(;;) {
                co_await rfbMessageAwait();
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                stop();
            }
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            stop();
        }

        co_return;
    }

    asio::awaitable<void> RFB::X11Server::serverUpdateLoop(void) {
        try {
            for(;;) {
                co_await serverUpdateProcess();
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                stop();
            }
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            stop();
        }

        co_return;
    }

    asio::awaitable<void> RFB::X11Server::serverUpdateProcess(void) {
        if(!fullscreenUpdateReq_ && (isEncoderFFmpeg() || xcbNoDamageOption())) {
            fullscreenUpdateReq_ = true;
        }

        auto ex = co_await asio::this_coro::executor;
        XCB::Region clientRect = XCB::Rectangle(clientRegion_.load());

        if(displayResizeProcessed_ ||
            displayResizeNegotiation_ || clientRect.isEmpty()) {
                // perhaps next time...
                asio::steady_timer timer_update{ex, 50ms};
                co_await timer_update.async_wait(asio::use_awaitable);
                co_return;
        }

        if(! fpsMinAction_) {
            // video and nodamage strong calc fps from fpsMin
            if(isEncoderFFmpeg() || xcbNoDamageOption()) {
                asio::steady_timer timer_update{ex, 1ms};
                co_await timer_update.async_wait(asio::use_awaitable);
                co_return;
            }

            // max fps: wait timeout
            const std::chrono::microseconds frameDuration(1000000 / fpsMax_);
            const auto delayDuration = (frameTimePoint_ + frameDuration) - std::chrono::steady_clock::now();

            if(0 < delayDuration.count()) {
                asio::steady_timer timer_update{ex, delayDuration};
                co_await timer_update.async_wait(asio::use_awaitable);
            }

            // continue iteration
            const bool notUpdateNeed = !fullscreenUpdateReq_ && damagePool_.empty();
            if(notUpdateNeed) {
                // perhaps next time...
                asio::steady_timer timer_update{ex, 10ms};
                co_await timer_update.async_wait(asio::use_awaitable);
                co_return;
            }
        }

        // set frame point
        auto now = std::chrono::steady_clock::now();
        Application::trace(DebugType::X11Srv, "{}: sleep delay: {}ms",
            NS_FuncNameV, std::chrono::duration_cast<std::chrono::milliseconds>(now - frameTimePoint_).count());

        frameTimePoint_ = std::move(now);
        fpsMinAction_ = false;

        const auto serverRect = XCB::Rectangle(serverRegion_.load());
        XCB::Region damageRect = joinAllDamages();

        if(fullscreenUpdateReq_) {
            damageRect = serverRect;
            fullscreenUpdateReq_ = false;
        } else {
            // fix out of screen
            damageRect = serverRect.intersected(damageRect.align(4));

            if(damageRect.isEmpty()) {
                co_return;
            }
        }

        if(clientRect != serverRect) {
            damageRect = clientRect.intersected(damageRect);
        }

        if(xcbAllowMessages()) {
            co_await sendUpdateScreenAwait(damageRect);

            if(clientUpdateCursor_) {
                asio::co_spawn(xcb_strand(), sendUpdateCursorAwait(), asio::detached);
                clientUpdateCursor_ = false;
            }
        }

        co_return;
    }

    asio::awaitable<uint16_t> RFB::X11Server::xcbDisplayDepth(void) const {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);
        co_return XCB::RootDisplay::depth();
    }

    asio::awaitable<XCB::Size> RFB::X11Server::xcbDisplaySize(void) const {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);
        co_return XCB::RootDisplay::size();
    }

    bool RFB::X11Server::isDisplaySize(const XCB::Size& sz) const {
        return XCB::RootDisplay::size() == sz;
    }

    void RFB::X11Server::serverScreenUpdateRequest(void) {
        fullscreenUpdateReq_ = true;
    }

    void RFB::X11Server::serverScreenUpdateRequest(const XCB::Region & reg) {
        if(xcbAllowMessages() && ! xcbNoDamageOption()) {
            asio::dispatch(xcb_strand(), [this, reg]() {
                XCB::RootDisplay::rootDamageAddRegion(reg);
            });
        }
    }

    asio::awaitable<void> RFB::X11Server::rfbStart(void) {
        // vnc session not activated trigger
        asio::steady_timer timerNotActivated(ioc_, 30s);
        timerNotActivated.async_wait([this](const system::error_code & ec) {
            if(!ec) {
                this->stop();
            }
        });

        // RFB 6.1.1 version
        int protover = co_await serverHandshakeVersionAwait();

        if(protover == 0) {
            rfbStartingCode_ = EXIT_FAILURE;
            co_return;
        }

        serverHandshakeVersionEvent();

        // RFB 6.1.2 security
        const auto secInfo = rfbSecurityInfo();
        if(bool valid = co_await serverSecurityInitAwait(protover, secInfo); !valid) {
            rfbStartingCode_ = EXIT_FAILURE;
            co_return;
        }

        serverSecurityInitEvent();

        // RFB 6.3.1 client init
        const auto displaySize = co_await xcbDisplaySize();
        const auto displayDepth = co_await xcbDisplayDepth();

        serverRegion_.store(xcb_rectangle_t{0, 0, displaySize.width, displaySize.height});
        frameTimePoint_ = std::chrono::steady_clock::now();

        co_await serverClientInitAwait("X11 Remote Desktop", displaySize, displayDepth, serverFormat());
        timerNotActivated.cancel();

        co_await xcbShmInit();

        serverConnectedEvent();
        Application::info("{}: wait RFB messages, fps: {}", NS_FuncNameV, frameRateOption());

        // xcb on
        xcbDisableMessages(false);

        asio::co_spawn(ioc_, signalsHandler(), asio::detached);

        asio::co_spawn(rfb_strand(), rfbReceiveMessages(),
            asio::bind_cancellation_slot(rfb_cancel_.slot(), asio::detached));

        asio::co_spawn(xcb_strand(), xcbEventsLoop(),
            asio::bind_cancellation_slot(xcb_cancel_.slot(), asio::detached));

        asio::co_spawn(rfb_strand(), serverUpdateLoop(),
            asio::bind_cancellation_slot(srv_cancel_.slot(), asio::detached));

        force_update_.expires_after(100ms);
        force_update_.async_wait(std::bind(&X11Server::minFpsHandler, this, std::placeholders::_1));

        co_return;
    }

    void RFB::X11Server::minFpsHandler(const system::error_code& ec) {
        if(ec) {
            return;
        }

        if(auto fpsMin = frameRateOption()) {
            fpsMinAction_ = true;
            force_update_.expires_after(std::chrono::microseconds(1000000 / fpsMin));
        } else {
            force_update_.expires_after(200ms);
        }
        force_update_.async_wait(std::bind(&X11Server::minFpsHandler, this, std::placeholders::_1));
    }

    /* Connector::X11Server */
    asio::awaitable<int> RFB::X11Server::rfbCommunicationAwait(void) {
        serverSelectEncodings();
        rfbStartingCode_ = EXIT_SUCCESS;
        co_await rfbStart();
        co_return rfbStartingCode_;
    }

    void RFB::X11Server::serverRecvPixelFormatEvent(const PixelFormat &, bool bigEndian) {
        if(! clientFormat().compare(serverFormat(), true)) {
            Application::warning("{}: client/server format not optimal", NS_FuncNameV);
        }
    }

    void RFB::X11Server::serverRecvSetEncodingsEvent(const std::vector<int> & recvEncodings) {
        serverSelectEncodings();
        serverEncodingsEvent();

        if(isClientSupportedEncoding(ENCODING_EXT_DESKTOP_SIZE) && rfbDesktopResizeEnabled()) {
            asio::co_spawn(xcb_strand(), [this]() -> asio::awaitable<void> {
                const auto dsz = co_await xcbDisplaySize();
                co_await sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ServerRuntime,
							RFB::DesktopResizeError::NoError, dsz);
		co_return;
            }, asio::detached);
        }
    }

    void RFB::X11Server::serverRecvKeyEvent(bool pressed, uint32_t keycode, uint16_t scancode) {
        if(! xcbAllowMessages()) {
            return;
        }

        auto keysym = keycode;

        if(isClientLtsmKeyboard()) {
            auto x11sym = SDL::Window::convertScanCodeToKeySym(static_cast<SDL_Scancode>(scancode));

            if(x11sym) {
                keysym = static_cast<uint32_t>(x11sym);
            }

            if(auto xkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtension(XCB::Module::XKB))) {
                int group = xkb->getLayoutGroup();
                auto keycodeGroup = keysymToKeycodeGroup(x11sym);

                if(group != keycodeGroup.second) {
                    keysym = keycodeGroupToKeysym(keycodeGroup.first, group);
                }
            }
        }

        if(auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtensionConst(XCB::Module::TEST))) {
            auto keycode = rfbUserKeycode(keysym);

            if(! keycode) {
                keycode = XCB::RootDisplay::keysymToKeycodeAuto(keysym);
            }

            if(keycode) {
                test->screenInputKeycode(keycode, pressed);
            }
        }
    }

    void RFB::X11Server::serverRecvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy) {
        if(xcbAllowMessages()) {
            auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtensionConst(XCB::Module::TEST));

            if(! test) {
                return;
            }

            // pressed mask:
            //  left 0x01, middle 0x02, right 0x04, scrollUp: 0x08,
            //  scrollDn: 0x10, scrollLf: 0x20, scrollRt: 0x40, back: 0x80
            if(pressedMask ^ mask) {
                for(int num = 0; num < 8; ++num) {
                    int bit = 1 << num;

                    if(bit & mask) {
                        Application::debug(DebugType::X11Srv, "{}: xfb fake input pressed: {}", NS_FuncNameV, num + 1);

                        test->screenInputButton(num + 1, XCB::Point(posx, posy), true);
                        pressedMask |= bit;
                    } else if(bit & pressedMask) {
                        Application::debug(DebugType::X11Srv, "{}: xfb fake input released: {}", NS_FuncNameV, num + 1);

                        test->screenInputButton(num + 1, XCB::Point(posx, posy), false);
                        pressedMask &= ~bit;
                    }
                }
            } else {
                const XCB::Point pos(posx, posy);
                Application::debug(DebugType::X11Srv, "{}: xfb fake input move, pos: {}", NS_FuncNameV, pos);
                test->screenInputMove(pos);
            }
        }
    }

    void RFB::X11Server::extClipboardSendBuf(std::vector<uint8_t>&& buf) const {
        asio::co_spawn(rfb_strand(), [this, buf=std::move(buf)]() -> asio::awaitable<void> {
            co_await sendCutTextAwait(buf, true);
            co_return;
        }, asio::detached);
    }

    uint16_t RFB::X11Server::extClipboardLocalTypes(void) const {
        return clipLocalTypes_;
    }

    asio::awaitable<clipboard_buf> RFB::X11Server::extClipboardLocalDataAwait(uint16_t type) {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);

        if(0 == extClipboardRemoteCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        if(auto copy = static_cast<XCB::ModuleCopySelection*>(getExtension(XCB::Module::SELECTION_COPY))) {
            for(const auto & atom : ExtClip::typesToX11Atoms(type, *this)) {
                clientClipboard_.clear();
                clipboard_ready_.expires_after(3000ms);

                // this is an initiator. we launch from the background.
                asio::post(xcb_strand(), [this,copy,atom]() {
                    copy->convertSelection(atom, *this);
                });

                // wait clipboard
                try {
                    co_await clipboard_ready_.async_wait(asio::use_awaitable);
                } catch(const system::system_error& err) {
                    // clipboard_ready_.cancel() -> data ready
                    if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                        Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                        co_return clipboard_buf{};
                    }
                }

                if(clientClipboard_.size()) {
                    co_return clientClipboard_;
                }
            }
        }

        co_return clipboard_buf{};
    }

    asio::awaitable<void> RFB::X11Server::extClipboardRemoteTypesAwait(uint16_t types) {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);

        if(! extClipboardRemoteCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        clipRemoteTypes_ = types;

        if(auto paste = static_cast<XCB::ModulePasteSelection*>(XCB::RootDisplay::getExtension(XCB::Module::SELECTION_PASTE))) {
            paste->setSelectionOwner(*this);
        }
    
        co_return;
    }

    asio::awaitable<void> RFB::X11Server::extClipboardRemoteDataAwait(uint16_t type, std::vector<uint8_t> buf) {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);

        if(! extClipboardRemoteCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        clientClipboard_.swap(buf);
        clipboard_ready_.cancel();

        co_return;
    }

    void RFB::X11Server::selectionReceiveData(xcb_atom_t atom, std::vector<uint8_t>&& buf) const {
        // xcb context
        if(extClipboardRemoteCaps()) {
            if(auto ptr = const_cast<RFB::X11Server*>(this)) {
                ptr->clientClipboard_.swap(buf);
                ptr->clipboard_ready_.cancel();
            }
        } else {
            asio::co_spawn(rfb_strand(), [this, buf=std::move(buf)]() -> asio::awaitable<void> {
                co_await sendCutTextAwait(buf, false /* ext mode */);
                co_return;
            }, asio::detached);
        }
    }

    void RFB::X11Server::selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) {
        // xcb context
        clipLocalTypes_ = 0;

        if(extClipboardRemoteCaps()) {
            // calc types
            std::for_each(beg, end, [&](auto & atom) {
                clipLocalTypes_ |= ExtClip::x11AtomToType(atom);
            });

            asio::dispatch(xcb_strand(), std::bind(&X11Server::sendExtClipboardNotify, this, clipLocalTypes_));
        } else {
            if(auto copy = static_cast<XCB::ModuleCopySelection*>(getExtension(XCB::Module::SELECTION_COPY))) {
                for(const auto & atom : selectionSourceTargets()) {
                    const bool found = std::ranges::any_of(beg, end, [&](auto & trgt) { return atom == trgt; });
                    if(found) {
                        return copy->convertSelection(atom, *this);
                    }
                }
            }
        }
    }

    void RFB::X11Server::selectionChangedEvent(void) const {
        auto ptr = const_cast<RFB::X11Server*>(this);
        if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY))) {
            copy->convertSelection(getAtom("TARGETS"), *this);
        }
    }

    std::vector<xcb_atom_t> RFB::X11Server::selectionSourceTargets(void) const {
        return ExtClip::typesToX11Atoms(extClipboardRemoteCaps() ?
                                        clipRemoteTypes_ : ExtClipCaps::TypeText, *this);
    }

    asio::awaitable<bool> RFB::X11Server::extClipboardSourceReadyAwait(xcb_atom_t atom) {
        uint16_t requestType = ExtClip::x11AtomToType(atom);

        clientClipboard_.clear();
        clipboard_ready_.expires_after(3000ms);

        // this is an initiator. we launch from the background.
        asio::post(xcb_strand(), std::bind(&X11Server::sendExtClipboardRequest, this, requestType));

        // wait clipboard
        try {
            co_await clipboard_ready_.async_wait(asio::use_awaitable);
        } catch(const system::system_error& err) {
            // clipboard_ready_.cancel() -> data ready
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                co_return false;
            }
        }

        co_return true;
    }

    bool RFB::X11Server::selectionSourceReady(xcb_atom_t atom) const {
        // xcb context

        auto targets = selectionSourceTargets();
        if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
            return false;
        }

        if(extClipboardRemoteCaps()) {
            // FIXME const
            // auto ptr = const_cast<RFB::X11Server*>(this);
            // co_await extClipboardSourceReadyAwait(atom);
        } else {
            // basic mode
            return clientClipboard_.size();
        }

        return false;
    }

    size_t RFB::X11Server::selectionSourceSize(xcb_atom_t atom) const {
        // xcb context
        auto targets = selectionSourceTargets();

        if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
            return 0;
        }

        return clientClipboard_.size();
    }

    std::vector<uint8_t> RFB::X11Server::selectionSourceData(xcb_atom_t atom, size_t offset, uint32_t length) const {
        // xcb context
        auto targets = selectionSourceTargets();

        if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
            return {};
        }

        if(offset + length <= clientClipboard_.size()) {
            auto beg = clientClipboard_.begin() + offset;
            return std::vector<uint8_t>(beg, beg + length);
        } else {
            Application::error("{}: invalid length: {}, offset: {}", NS_FuncNameV, length, offset);
        }

        return {};
    }

    void RFB::X11Server::serverRecvCutTextEvent(std::vector<uint8_t> && buf) {
        // xcb context
        if(rfbClipboardEnable()) {
            clientClipboard_.swap(buf);
            clipboard_ready_.cancel();

            if(xcbAllowMessages()) {
                if(auto paste = static_cast<XCB::ModulePasteSelection*>(XCB::RootDisplay::getExtension(XCB::Module::SELECTION_PASTE))) {
                    paste->setSelectionOwner(*this);
                }
            }
        }
    }

    void RFB::X11Server::serverRecvFBUpdateEvent(bool incremental, const XCB::Region & reg) {
        // rfb context
        if(! xcbAllowMessages()) {
            fullscreenUpdateReq_ = true;
            return;
        }

        if(incremental && isContinueUpdatesProcessed()) {
            // skipped FramebufferUpdateRequest
            // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#enablecontinuousupdates
            clientRegion_.store(xcb_rectangle_t{});
        } else {
            clientRegion_.store(xcb_rectangle_t{reg.x, reg.y, reg.width, reg.height});
        }

        if(! incremental) {
            fullscreenUpdateReq_ = true;
        }
    }

    void RFB::X11Server::serverRecvDesktopSizeEvent(std::vector<RFB::ScreenInfo>&& screens) {
        XCB::Region desktop(0, 0, 0, 0);

        for(const auto & info : screens) {
            Application::info("{}: screen id: {}, region: {}, flags: {:#010x}",
                              NS_FuncNameV, info.id, info.pos(), info.flags);
            desktop.join(info.pos());
        }

        const auto dsz = XCB::RootDisplay::size();

        if(desktop.x != 0 && desktop.y != 0) {
            Application::error("{}: incorrect desktop size: {}", NS_FuncNameV, desktop);
	    asio::co_spawn(rfb_strand(), 
        	sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::InvalidScreenLayout, std::move(dsz)),
	        asio::detached);
        } else if(! xcbAllowMessages()) {
            Application::error("{}: xcb disabled", NS_FuncNameV);
	    asio::co_spawn(rfb_strand(),
        	sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources, XCB::Size{0, 0}),
	        asio::detached);
        } else if(dsz == desktop.toSize()) {
	    asio::co_spawn(rfb_strand(),
        	sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::NoError, std::move(dsz)),
	        asio::detached);
        } else {
            displayResizeNegotiation_ = true;
            uint16_t sequence = 0;
            if(XCB::RootDisplay::setRandrScreenSize(desktop.toSize(), & sequence)) {
                randrSequence = sequence;
            } else {
	        asio::co_spawn(rfb_strand(),
                    sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources, std::move(dsz)),
                    asio::detached);
                displayResizeNegotiation_ = false;
                displayResizeProcessed_ = false;
                randrSequence = 0;
            }
        }
    }

    asio::awaitable<void> RFB::X11Server::sendUpdateCursorAwait(void) {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);
        if(auto fixes = static_cast<const XCB::ModuleWindowFixes*>(XCB::RootDisplay::getExtensionConst(XCB::Module::WINFIXES))) {
            XCB::CursorImage replyCursor = fixes->getCursorImage();
            const auto & reply = replyCursor.reply();

            if(auto ptr = reinterpret_cast<uint8_t*>(replyCursor.data())) {
                const size_t argbSize = reply->width * reply->height;
                const size_t dataSize = replyCursor.size();

                Application::debug(X11Srv, "{}: data lenth: {}", NS_FuncNameV, dataSize);

                if(dataSize == argbSize) {
                    const auto cursorHot = XCB::Point(reply->xhot, reply->yhot);
                    const auto cursorSize = XCB::Size(reply->width, reply->height);
                    // priority LTSM cursors
                    if(isClientSupportedEncoding(RFB::ENCODING_LTSM_CURSOR)) {
                        co_await sendEncodingLtsmCursorAwait(cursorHot, cursorSize, std::span{ptr, dataSize * sizeof(uint32_t)});
                    } else {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                        auto cursorFB = FrameBuffer(ptr, XCB::Region(cursorSize), BGRA32);
#else
                        auto cursorFB = FrameBuffer(ptr, XCB::Region(cursorSize), ARGB32);
#endif
                        co_await sendEncodingRichCursorAwait(cursorFB, cursorHot);
                    }
                } else {
                    Application::warning("{}: size mismatch, data: {}, argb: {}", NS_FuncNameV, dataSize, argbSize);
                }
            }
        }
    }

    void RFB::X11Server::serverSendFBUpdateEvent(const XCB::Region & reg) const {
        if(! xcbNoDamageOption()) {
            asio::dispatch(xcb_strand(), [this, reg]() {
                XCB::RootDisplay::rootDamageSubtrack(reg);
            });
        }
    }

    asio::awaitable<void> RFB::X11Server::xcbShmInit(uid_t uid, const XCB::Size* psz) {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);

        if(auto ext = static_cast<const XCB::ModuleShm*>(XCB::RootDisplay::getExtension(XCB::Module::SHM))) {
            auto dsz = XCB::RootDisplay::size();

            if(psz && dsz < *psz) {
                Application::warning("{}: display size: {}, select size: {}", NS_FuncNameV, dsz, *psz);
                dsz = *psz;
            }

            auto bpp = XCB::RootDisplay::bitsPerPixel() >> 3;
            size_t shmsz = dsz.width * dsz.height * bpp;

            if(! shm || shm->owner != uid || shm->size < shmsz) {
                Application::info("{}: size: {}", NS_FuncNameV, shmsz);
                shm = ext->createShm(shmsz, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, false, uid);
            }
        }

        co_return;
    }

    XcbFrameBuffer RFB::X11Server::serverFrameBuffer(const XCB::Region & reg) const {
        Application::debug(DebugType::X11Srv, "{}: region: {}", NS_FuncNameV, reg);
        auto pixmapReply = XCB::RootDisplay::copyRootImageRegion(reg, shm);

        if(! pixmapReply) {
            Application::error("{}: {}", NS_FuncNameV, "xcb copy region empty");
            throw rfb_error(NS_FuncNameS);
        }

        Application::trace(DebugType::X11Srv, "{}: request size: {}, reply: length: {}, bits per pixel: {}, red: {:#010x}, green: {:#010x}, blue: {:#010x}",
                           NS_FuncNameV, reg.toSize(), pixmapReply->size(), pixmapReply->bitsPerPixel(), pixmapReply->rmask,
                           pixmapReply->gmask, pixmapReply->bmask);

        // fix align
        if(pixmapReply->size() != reg.width * reg.height * pixmapReply->bytePerPixel()) {
            Application::error("{}: region not aligned, reply size: {}, reg size: {}, byte per pixel: {}",
                               NS_FuncNameV, pixmapReply->size(), reg.toSize(), pixmapReply->bytePerPixel());
            throw rfb_error(NS_FuncNameS);
        }

        FrameBuffer fb(pixmapReply->data(), reg, serverFormat());
        serverFrameBufferModifyEvent(fb);

        return XcbFrameBuffer{std::move(pixmapReply), std::move(fb)};
    }

    void RFB::X11Server::serverRecvSetContinuousUpdatesEvent(bool enable, const XCB::Region & reg) {
        clientRegion_.store(xcb_rectangle_t{reg.x, reg.y, reg.width, reg.height});
        if(enable) {
            serverScreenUpdateRequest(reg);
        }
    }
}
