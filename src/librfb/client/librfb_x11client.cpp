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

#include <chrono>

#include "ltsm_application.h"
#include "ltsm_tools.h"

#include "ltsm_librfb.h"
#include "librfb_x11client.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM {
    RFB::X11Client::X11Client(const asio::any_io_executor& ctx) : ClientDecoder(ctx), clipboard_ready_{ctx} {
        if(! displayConnect(-1,
                            XCB::InitModules::Xkb | XCB::InitModules::SelCopy | XCB::InitModules::SelPaste, nullptr)) {
            throw xcb_error(NS_FuncNameS);
        }
    }

    asio::awaitable<void> RFB::X11Client::extClipboardSendAwait(std::span<const uint8_t> buf) const {
        Application::debug(DebugType::X11Cli, "{}, length: {}", NS_FuncNameV, buf.size());
        co_await sendCutTextAwait(buf, true);
        co_return;
    }

    uint16_t RFB::X11Client::extClipboardLocalTypes(void) const {
        return clipLocalTypes;
    }

    asio::awaitable<clipboard_buf> RFB::X11Client::extClipboardLocalDataAwait(uint16_t type) {
        // xcb context
        if(0 == extClipboardLocalCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        Application::debug(DebugType::X11Cli, "{}", NS_FuncNameV);

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

    asio::awaitable<void> RFB::X11Client::extClipboardRemoteTypesAwait(uint16_t types) {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);

        if(! extClipboardRemoteCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        clipRemoteTypes = types;
        if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE))) {
            paste->setSelectionOwner(*this);
        }

        co_return;
    }

    asio::awaitable<void> RFB::X11Client::extClipboardRemoteDataAwait(uint16_t type, std::vector<uint8_t> buf) {
        co_await asio::dispatch(xcb_strand(), asio::use_awaitable);
        Application::debug(DebugType::X11Cli, "{}, type: {:#06x}, length: {}", NS_FuncNameV, type, buf.size());

        if(! extClipboardRemoteCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }
        
        clientClipboard_.swap(buf);
        clipboard_ready_.cancel();
    
        co_return;
    }

    void RFB::X11Client::selectionReceiveData(xcb_atom_t atom, std::vector<uint8_t>&& buf) const {
        // xcb context
        Application::debug(DebugType::X11Cli, "{}, atom: {:#010x}, length: {}", NS_FuncNameV, atom, buf.size());

        if(auto ptr = const_cast<RFB::X11Client*>(this)) {
            if(extClipboardRemoteCaps()) {
                ptr->clientClipboard_.swap(buf);
                ptr->clipboard_ready_.cancel();
            } else {
                asio::co_spawn(rfb_strand(), [this, buf = std::move(buf)]() -> asio::awaitable<void> {
                    co_await sendCutTextAwait(buf, false /* ext mode */);
                    co_return;
                }, asio::detached);
            }
        }
    }

    void RFB::X11Client::selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) {
        Application::debug(DebugType::X11Cli, "{}", NS_FuncNameV);
        clipLocalTypes = 0;

        if(extClipboardRemoteCaps()) {
            // calc types
            std::for_each(beg, end, [&](auto & atom) {
                clipLocalTypes |= ExtClip::x11AtomToType(atom);
            });

            // FIXME await
            // co_await sendExtClipboardNotifyAwait(clipLocalTypes);
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

    void RFB::X11Client::selectionChangedEvent(void) const {
        Application::debug(DebugType::X11Cli, "{}", NS_FuncNameV);
        auto ptr = const_cast<RFB::X11Client*>(this);

        if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY))) {
            copy->convertSelection(getAtom("TARGETS"), *this);
        }
    }

    std::vector<xcb_atom_t> RFB::X11Client::selectionSourceTargets(void) const {
        Application::debug(DebugType::X11Cli, "{}", NS_FuncNameV);
        return ExtClip::typesToX11Atoms(extClipboardRemoteCaps() ?
                                        clipRemoteTypes : ExtClipCaps::TypeText, *this);
    }

    asio::awaitable<bool> RFB::X11Client::extClipboardSourceReadyAwait(xcb_atom_t atom) {
        uint16_t requestType = ExtClip::x11AtomToType(atom);
        clientClipboard_.clear();

        co_await sendExtClipboardRequestAwait(requestType);
        clipboard_ready_.expires_after(3000ms);

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

    bool RFB::X11Client::selectionSourceReady(xcb_atom_t atom) const {
        // xcb context
        Application::debug(DebugType::X11Cli, "{}, atom: {:#010x}", NS_FuncNameV, atom);
        auto targets = selectionSourceTargets();

        if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
            return false;
        }

        if(extClipboardRemoteCaps()) {
            // FIXME const
            // auto ptr = const_cast<RFB::X11Clent*>(this);
            // co_await extClipboardSourceReadyAwait(atom);
        } else {
            // basic mode
            return clientClipboard_.size();
        }

        return false;
    }

    size_t RFB::X11Client::selectionSourceSize(xcb_atom_t atom) const {
        // xcb context
        Application::debug(DebugType::X11Cli, "{}, atom: {:#010x}", NS_FuncNameV, atom);
        auto targets = selectionSourceTargets();

        if(std::ranges::none_of(targets, [&](auto & trgt) { return atom == trgt; })) {
            return 0;
        }

        return clientClipboard_.size();
    }

    std::vector<uint8_t> RFB::X11Client::selectionSourceData(xcb_atom_t atom, size_t offset, uint32_t length) const {
        // xcb context
        Application::debug(DebugType::X11Cli, "{}, atom: {:#010x}, offset: {}, length: {}", NS_FuncNameV, atom, offset, length);

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

    void RFB::X11Client::clientRecvCutTextEvent(std::vector<uint8_t> && buf) {
        // xcb context
        Application::debug(DebugType::X11Cli, "{}: data length: {}", NS_FuncNameV, buf.size());
        clientClipboard_.swap(buf);
        clipboard_ready_.cancel();

        if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE))) {
            paste->setSelectionOwner(*this);
        }
    }

    void RFB::X11Client::xcbDisplayConnectedEvent(void) {
        Application::debug(DebugType::X11Cli, "{}", NS_FuncNameV);
        ExtClip::x11AtomsUpdate(*this);

        // init selection copy
        selectionChangedEvent();
    }
}
