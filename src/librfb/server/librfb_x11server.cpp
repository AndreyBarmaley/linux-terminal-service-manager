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
#include <thread>

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

    const XCB::Region & RFB::X11Server::getClientRegion(void) const {
        return clientRegion;
    }

    void RFB::X11Server::xcbFixesCursorChangedEvent(void) {
        clientUpdateCursor = isClientSupportedEncoding(ENCODING_RICH_CURSOR);
    }

    void RFB::X11Server::xcbDamageNotifyEvent(const xcb_rectangle_t & rt, uint8_t level) {
        const std::scoped_lock guard{ serverLock };
        damageRegion.join(rt.x, rt.y, rt.width, rt.height);
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
        displayResizeProcessed = true;
    }

    void RFB::X11Server::xcbRandrScreenChangedEvent(const XCB::Size & wsz, const xcb_randr_notify_event_t & notify) {
        Application::info("{}: size: {}, sequence: {}", NS_FuncNameV, wsz, notify.sequence);
        xcbShmInit(0, & wsz);
        displayResizeProcessed = false;
        serverDisplayResizedEvent(wsz);

        if(isClientSupportedEncoding(ENCODING_EXT_DESKTOP_SIZE)) {
            auto status = randrSequence == notify.sequence ?
                          RFB::DesktopResizeStatus::ClientSide : RFB::DesktopResizeStatus::ServerRuntime;

	    asio::co_spawn(rfb_strand(), [this, status, wsz]() -> asio::awaitable<void> {
        	if(status == RFB::DesktopResizeStatus::ServerRuntime) {
            	    co_await sendEncodingDesktopResizeAwait(status, RFB::DesktopResizeError::NoError, wsz);
            	    displayResizeEvent(wsz);
        	} else if(this->displayResizeNegotiation) {
                    // clientSide
                    co_await sendEncodingDesktopResizeAwait(status, RFB::DesktopResizeError::NoError, wsz);
                    displayResizeEvent(wsz);
                    displayResizeNegotiation = false;
                }
		co_return;
	    }, asio::detached);
        }
    }

    void RFB::X11Server::stop(void) {
        xcbDisableMessages(true);
        rfbMessagesShutdown();

        rfb_cancel_.emit(asio::cancellation_type::terminal);
        xcb_cancel_.emit(asio::cancellation_type::terminal);
        srv_cancel_.emit(asio::cancellation_type::terminal);

        signals_.cancel();
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
                    asio::post(xcb_strand(), std::bind(&X11Server::stop, this));
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
        auto ex = co_await asio::this_coro::executor;
        asio::posix::stream_descriptor sd{ex, XCB::RootDisplay::getFd()};

        try {
            for(;;) {
                if(auto err = XCB::RootDisplay::hasError()) {
                    Application::error("{}: xcb error, code: {}", NS_FuncNameV, err);
                    asio::post(ioc_, std::bind(&X11Server::stop, this));
                    throw system::system_error(asio::error::operation_aborted);
                }

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
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                asio::post(ioc_, std::bind(&X11Server::stop, this));
            }
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            asio::post(ioc_, std::bind(&X11Server::stop, this));
        }

        sd.release();
        co_return;
    }

    asio::awaitable<void> RFB::X11Server::rfbReceiveMessages(void) {
        try {
            Application::debug(DebugType::Rfb, "{}: wait remote messages...", NS_FuncNameV);
            for(;;) {
                co_await ServerEncoder::rfbWaitMessage();
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                asio::post(ioc_, std::bind(&X11Server::stop, this));
            }
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            asio::post(ioc_, std::bind(&X11Server::stop, this));
        }

        co_return;
    }

    asio::awaitable<void> RFB::X11Server::serverUpdateLoop(void) {
        try {
            frameTimePoint = std::chrono::steady_clock::now();

            for(;;) {
                co_await serverUpdateProcess();
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
                asio::post(ioc_, std::bind(&X11Server::stop, this));
            }
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            asio::post(ioc_, std::bind(&X11Server::stop, this));
        }

        co_return;
    }

    asio::awaitable<void> RFB::X11Server::serverUpdateProcess(void) {
        serverMainLoopEvent();

        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer{ex};

        // check timepoint frame
        if(auto frameRate = frameRateOption()) {
            int delayTimeout = 1000 / frameRate;

            if(isEncoderFFmpeg()) {
                // ffmpeg encoding: fixed fps
                fullscreenUpdateReq = true;
            } else if(xcbNoDamageOption()) {
                // no damage: fixed fps
            } else if(! damageRegion.isEmpty()) {
                // damage present - 16 fps
                delayTimeout = 65;
            }

            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - frameTimePoint);
            int last = delayTimeout - static_cast<int>(dt.count());
            Application::debug(DebugType::X11Srv, "{}: sleep ms: {}", NS_FuncNameV, last);

            // large timepoint
            if(30 < last) {
                timer.expires_after(30ms);
                co_await timer.async_wait(asio::use_awaitable);
                co_return;
            }

            // small timepoint
            if(0 < last) {
                timer.expires_after(std::chrono::milliseconds(last));
                co_await timer.async_wait(asio::use_awaitable);
            }
        }

        // processed frame update
        if(fullscreenUpdateReq || ! damageRegion.isEmpty()) {
            // wait condition
            if(! xcbAllowMessages() || displayResizeProcessed ||
               displayResizeNegotiation || clientRegion.isEmpty()) {
                timer.expires_after(10ms);
                co_await timer.async_wait(asio::use_awaitable);
                co_return;
            }

            frameTimePoint = std::chrono::steady_clock::now();
            auto serverRegion = XCB::RootDisplay::region();

            //const std::scoped_lock guard{ serverLock };

            if(fullscreenUpdateReq) {
                damageRegion = serverRegion;
                fullscreenUpdateReq = false;
            } else {
                // fix out of screen
                damageRegion = serverRegion.intersected(damageRegion.align(4));
            }

            if(clientRegion != serverRegion) {
                damageRegion = clientRegion.intersected(damageRegion);
            }

            co_await sendUpdateScreenAwait(damageRegion);

            if(clientUpdateCursor) {
                co_await sendUpdateRichCursorAwait();
                clientUpdateCursor = false;
            }

            damageRegion.reset();
        }

        co_return;
    }

    XCB::Size RFB::X11Server::displaySize(void) const {
        return XCB::RootDisplay::size();
    }

    void RFB::X11Server::serverScreenUpdateRequest(void) {
        fullscreenUpdateReq = true;
    }

    void RFB::X11Server::serverScreenUpdateRequest(const XCB::Region & reg) {
        const std::scoped_lock guard{ serverLock };
        damageRegion.join(reg);
    }

    asio::awaitable<void> RFB::X11Server::rfbStart(void) {
        // vnc session not activated trigger
        boost::asio::steady_timer timerNotActivated(ioc_, 30s);
        timerNotActivated.async_wait([this](const boost::system::error_code & ec) {
            if(!ec) {
                this->stop();
            }
        });

        // RFB 6.1.1 version
        int protover = co_await serverHandshakeVersion();

        if(protover == 0) {
            rfbStartingCode_ = EXIT_FAILURE;
            co_return;
        }

        serverHandshakeVersionEvent();

        // RFB 6.1.2 security
        if(bool valid = co_await serverSecurityInit(protover, rfbSecurityInfo()); !valid) {
            rfbStartingCode_ = EXIT_FAILURE;
            co_return;
        }

        serverSecurityInitEvent();

        // RFB 6.3.1 client init
        co_await serverClientInit("X11 Remote Desktop", XCB::RootDisplay::size(), XCB::RootDisplay::depth(), serverFormat());
        timerNotActivated.cancel();

        xcbShmInit();

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

        co_return;
    }

    /* Connector::X11Server */
    int RFB::X11Server::rfbCommunication(void) {
        serverSelectEncodings();

        rfbStartingCode_ = EXIT_SUCCESS;

        asio::co_spawn(ioc_, rfbStart(), asio::detached);
        ioc_.run();

        return rfbStartingCode_;
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
            asio::co_spawn(rfb_strand(), [this]() -> asio::awaitable<void> {
                co_await sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ServerRuntime,
							RFB::DesktopResizeError::NoError, XCB::RootDisplay::size());
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

    void RFB::X11Server::extClipboardSendEvent(std::vector<uint8_t>&& buf) {
        asio::co_spawn(ioc_, [this, buf=std::move(buf)]() -> asio::awaitable<void> {
            co_await sendCutTextEventAwait(buf, true);
            co_return;
        }, asio::detached);
    }

    uint16_t RFB::X11Server::extClipboardLocalTypes(void) const {
        return clipLocalTypes;
    }

    std::vector<uint8_t> RFB::X11Server::extClipboardLocalData(uint16_t type) const {
        if(0 == extClipboardRemoteCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        auto ptr = const_cast<RFB::X11Server*>(this);

        if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY))) {
            for(const auto & atom : ExtClip::typesToX11Atoms(type, *this)) {
                ptr->clientClipboard.clear();
                copy->convertSelection(atom, *this);

                // wait data from selectionReceiveData
                Tools::Timeout waitCb(100ms);

                while(true) {
                    std::this_thread::sleep_for(3ms);

                    if(waitCb.check()) {
                        break;
                    }

                    const std::scoped_lock guard{ serverLock };

                    if(clientClipboard.size()) {
                        return clientClipboard;
                    }
                }
            }
        }

        return {};
    }

    void RFB::X11Server::extClipboardRemoteTypesEvent(uint16_t types) {
        if(extClipboardRemoteCaps()) {
            clipRemoteTypes = types;

            if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE))) {
                paste->setSelectionOwner(*this);
            }
        } else {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }
    }

    void RFB::X11Server::extClipboardRemoteDataEvent(uint16_t type, std::vector<uint8_t> && buf) {
        if(extClipboardRemoteCaps()) {
            const std::scoped_lock guard{ serverLock };
            clientClipboard.swap(buf);
        } else {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }
    }

    void RFB::X11Server::selectionReceiveData(xcb_atom_t atom, std::vector<uint8_t>&& buf) const {
        if(auto ptr = const_cast<RFB::X11Server*>(this)) {
            if(extClipboardRemoteCaps()) {
                const std::scoped_lock guard{ serverLock };
                ptr->clientClipboard.swap(buf);
            } else {
                asio::co_spawn(ioc_, [this, buf=std::move(buf)]() -> asio::awaitable<void> {
                    co_await sendCutTextEventAwait(buf, false);
                    co_return;
                }, asio::detached);
            }
        }
    }

    void RFB::X11Server::selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) {
        clipLocalTypes = 0;

        if(extClipboardRemoteCaps()) {
            // calc types
            std::for_each(beg, end, [&](auto & atom) {
                clipLocalTypes |= ExtClip::x11AtomToType(atom);
            });

            if(auto owner = const_cast<X11Server*>(this)) {
                owner->sendExtClipboardNotify(clipLocalTypes);
            }
        } else {
            auto ptr = const_cast<RFB::X11Server*>(this);

            if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY))) {
                for(const auto & atom : selectionSourceTargets()) {
                    if(std::ranges::any_of(beg, end, [&](auto & trgt) { return atom == trgt; })) {
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
                                        clipRemoteTypes : ExtClipCaps::TypeText, *this);
    }

    bool RFB::X11Server::selectionSourceReady(xcb_atom_t atom) const {
        auto targets = selectionSourceTargets();

        if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
            return false;
        }

        if(extClipboardRemoteCaps()) {
            uint16_t requestType = ExtClip::x11AtomToType(atom);
            auto ptr = const_cast<RFB::X11Server*>(this);

            ptr->clientClipboard.clear();
            ptr->sendExtClipboardRequest(requestType);

            // wait data from extClipboardRemoteDataEvent
            Tools::Timeout waitCb(3000ms);

            while(true) {
                std::this_thread::sleep_for(3ms);

                if(waitCb.check()) {
                    break;
                }

                const std::scoped_lock guard{ serverLock };

                if(clientClipboard.size()) {
                    return true;
                }
            }
        } else {
            // basic mode
            return clientClipboard.size();
        }

        return false;
    }

    size_t RFB::X11Server::selectionSourceSize(xcb_atom_t atom) const {
        auto targets = selectionSourceTargets();

        if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
            return 0;
        }

        const std::scoped_lock guard{ serverLock };
        return clientClipboard.size();
    }

    std::vector<uint8_t> RFB::X11Server::selectionSourceData(xcb_atom_t atom, size_t offset, uint32_t length) const {
        auto targets = selectionSourceTargets();

        if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
            return {};
        }

        const std::scoped_lock guard{ serverLock };

        if(offset + length <= clientClipboard.size()) {
            auto beg = clientClipboard.begin() + offset;
            return std::vector<uint8_t>(beg, beg + length);
        } else {
            Application::error("{}: invalid length: {}, offset: {}", NS_FuncNameV, length, offset);
        }

        return {};
    }

    void RFB::X11Server::serverRecvCutTextEvent(std::vector<uint8_t> && buf) {
        if(rfbClipboardEnable()) {
            const std::scoped_lock guard{ serverLock };
            clientClipboard.swap(buf);

            if(xcbAllowMessages()) {
                if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE))) {
                    paste->setSelectionOwner(*this);
                }
            }
        }
    }

    void RFB::X11Server::serverRecvFBUpdateEvent(bool incremental, const XCB::Region & region) {
        if(! xcbAllowMessages()) {
            fullscreenUpdateReq = true;
            return;
        }

        const std::scoped_lock guard{ serverLock };
        clientRegion = region;

        if(! incremental) {
            fullscreenUpdateReq = true;
        } else if(isContinueUpdatesProcessed()) {
            // skipped FramebufferUpdateRequest
            // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#enablecontinuousupdates
            clientRegion.reset();
        }
    }

    void RFB::X11Server::serverRecvDesktopSizeEvent(const std::vector<RFB::ScreenInfo> & screens) {
        XCB::Region desktop(0, 0, 0, 0);

        for(const auto & info : screens) {
            Application::info("{}: screen id: {}, region: {}, flags: {:#010x}",
                              NS_FuncNameV, info.id, info.pos(), info.flags);
            desktop.join(info.pos());
        }

        if(desktop.x != 0 && desktop.y != 0) {
            Application::error("{}: incorrect desktop size: {}", NS_FuncNameV, desktop);
	    asio::co_spawn(rfb_strand(), [this, dsz=XCB::RootDisplay::size()]() -> asio::awaitable<void> {
        	co_await sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::InvalidScreenLayout, dsz);
		co_return;
	    }, asio::detached);
        } else if(! xcbAllowMessages()) {
            Application::error("{}: xcb disabled", NS_FuncNameV);
	    asio::co_spawn(rfb_strand(), [this]() -> asio::awaitable<void> {
        	co_await sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources, XCB::Size{0, 0});
		co_return;
	    }, asio::detached);
        } else if(XCB::RootDisplay::size() == desktop.toSize()) {
	    asio::co_spawn(rfb_strand(), [this, dsz=XCB::RootDisplay::size()]() -> asio::awaitable<void> {
        	co_await sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::NoError, dsz);
		co_return;
	    }, asio::detached);
        } else {
            displayResizeNegotiation = true;
	    asio::co_spawn(xcb_strand(), [this, dsz=desktop.toSize()]() -> asio::awaitable<void> {
                uint16_t sequence = 0;

                if(XCB::RootDisplay::setRandrScreenSize(dsz, & sequence)) {
                    randrSequence = sequence;
                } else {
                    co_await sendEncodingDesktopResizeAwait(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources,
                                              XCB::RootDisplay::size());
                    displayResizeNegotiation = false;
                    displayResizeProcessed = false;
                    randrSequence = 0;
                }
            }, asio::detached);
        }
    }

    asio::awaitable<void> RFB::X11Server::sendUpdateRichCursorAwait(void) {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);
        if(auto fixes = static_cast<const XCB::ModuleWindowFixes*>(XCB::RootDisplay::getExtensionConst(XCB::Module::WINFIXES))) {
            XCB::CursorImage replyCursor = fixes->getCursorImage();
            const auto & reply = replyCursor.reply();

            if(auto ptr = replyCursor.data()) {
                size_t argbSize = reply->width * reply->height;
                size_t dataSize = replyCursor.size();

                Application::debug(X11Srv, "{}: data lenth: {}", NS_FuncNameV, dataSize);

                if(dataSize == argbSize) {
                    auto cursorRegion = XCB::Region(reply->x, reply->y, reply->width, reply->height);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                    auto cursorFB = FrameBuffer(reinterpret_cast<uint8_t*>(ptr), cursorRegion, BGRA32);
#else
                    auto cursorFB = FrameBuffer(reinterpret_cast<uint8_t*>(ptr), cursorRegion, ARGB32);
#endif
                    co_await asio::dispatch(rfb_strand(), asio::use_awaitable);
                    co_await sendEncodingRichCursorAwait(cursorFB, reply->xhot, reply->yhot);
                } else {
                    Application::warning("{}: size mismatch, data: {}, argb: {}", NS_FuncNameV, dataSize, argbSize);
                }
            }
        }
    }

    void RFB::X11Server::serverSendFBUpdateEvent(const XCB::Region & reg) {
        if(! xcbNoDamageOption()) {
            XCB::RootDisplay::rootDamageSubtrack(reg);
        }
    }

    void RFB::X11Server::xcbShmInit(uid_t uid, const XCB::Size* psz) {
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
        const std::scoped_lock guard{ serverLock };
        clientRegion = reg;

        // serverScreenUpdateRequest
        if(enable) {
            damageRegion.join(reg);
        }
    }
}
