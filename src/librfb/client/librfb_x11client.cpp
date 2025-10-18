/***********************************************************************
 *   Copyright © 2025 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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
#include <thread>

#include "ltsm_application.h"
#include "ltsm_tools.h"

#include "ltsm_librfb.h"
#include "librfb_x11client.h"

using namespace std::chrono_literals;

namespace LTSM {
    RFB::X11Client::X11Client() {
        if(! displayConnect(-1,
                            XCB::InitModules::Xkb | XCB::InitModules::SelCopy | XCB::InitModules::SelPaste, nullptr)) {
            throw xcb_error(NS_FuncName);
        }
    }

    void RFB::X11Client::extClipboardSendEvent(const std::vector<uint8_t> & buf) {
        Application::debug(DebugType::X11Cli, "%s, length: %lu", __FUNCTION__, buf.size());
        sendCutTextEvent(buf.data(), buf.size(), true);
    }

    uint16_t RFB::X11Client::extClipboardLocalTypes(void) const {
        return clipLocalTypes;
    }

    std::vector<uint8_t> RFB::X11Client::extClipboardLocalData(uint16_t type) const {
        if(0 == extClipboardLocalCaps()) {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }

        Application::debug(DebugType::X11Cli, "%s", __FUNCTION__);

        auto ptr = const_cast<RFB::X11Client*>(this);

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

                    const std::scoped_lock guard{ clientLock };

                    if(clientClipboard.size()) {
                        return clientClipboard;
                    }
                }
            }
        }

        return {};
    }

    void RFB::X11Client::extClipboardRemoteTypesEvent(uint16_t types) {
        Application::debug(DebugType::X11Cli, "%s, types: 0x%04" PRIx16, __FUNCTION__, types);

        if(extClipboardRemoteCaps()) {
            clipRemoteTypes = types;

            if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE))) {
                paste->setSelectionOwner(*this);
            }
        } else {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::X11Client::extClipboardRemoteDataEvent(uint16_t type, std::vector<uint8_t> && buf) {
        Application::debug(DebugType::X11Cli, "%s, type: 0x%04" PRIx16 ", length: %lu", __FUNCTION__, type, buf.size());

        if(extClipboardRemoteCaps()) {
            const std::scoped_lock guard{ clientLock };
            clientClipboard.swap(buf);
        } else {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::X11Client::selectionReceiveData(xcb_atom_t atom, const uint8_t* buf, uint32_t len) const {
        Application::debug(DebugType::X11Cli, "%s, atom: 0x%08" PRIx32 ", length: %" PRIu32, __FUNCTION__, atom, len);

        if(auto ptr = const_cast<RFB::X11Client*>(this)) {
            if(extClipboardRemoteCaps()) {
                const std::scoped_lock guard{ clientLock };
                ptr->clientClipboard.assign(buf, buf + len);
            } else {
                ptr->sendCutTextEvent(buf, len, false);
            }
        }
    }

    void RFB::X11Client::selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) const {
        Application::debug(DebugType::X11Cli, "%s", __FUNCTION__);
        clipLocalTypes = 0;

        if(extClipboardRemoteCaps()) {
            // calc types
            std::for_each(beg, end, [&](auto & atom) {
                clipLocalTypes |= ExtClip::x11AtomToType(atom);
            });

            if(auto owner = const_cast<X11Client*>(this)) {
                owner->sendExtClipboardNotify(clipLocalTypes);
            }
        } else {
            auto ptr = const_cast<RFB::X11Client*>(this);

            if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY))) {
                for(const auto & atom : selectionSourceTargets()) {
                    if(std::any_of(beg, end, [&](auto & trgt) {
                    return atom == trgt;
                })) {
                        return copy->convertSelection(atom, *this);
                    }
                }
            }
        }
    }

    void RFB::X11Client::selectionChangedEvent(void) const {
        Application::debug(DebugType::X11Cli, "%s", __FUNCTION__);
        auto ptr = const_cast<RFB::X11Client*>(this);

        if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY))) {
            copy->convertSelection(getAtom("TARGETS"), *this);
        }
    }

    std::vector<xcb_atom_t> RFB::X11Client::selectionSourceTargets(void) const {
        Application::debug(DebugType::X11Cli, "%s", __FUNCTION__);
        return ExtClip::typesToX11Atoms(extClipboardRemoteCaps() ?
                                        clipRemoteTypes : ExtClipCaps::TypeText, *this);
    }

    bool RFB::X11Client::selectionSourceReady(xcb_atom_t atom) const {
        Application::debug(DebugType::X11Cli, "%s, atom: 0x%08" PRIx32, __FUNCTION__, atom);
        auto targets = selectionSourceTargets();

        if(std::none_of(targets.begin(), targets.end(), [&](auto & trgt) {
        return atom == trgt;
    }))
        return false;

        if(extClipboardRemoteCaps()) {
            uint16_t requestType = ExtClip::x11AtomToType(atom);
            auto ptr = const_cast<RFB::X11Client*>(this);

            ptr->clientClipboard.clear();
            ptr->sendExtClipboardRequest(requestType);

            // wait data from extClipboardRemoteDataEvent
            Tools::Timeout waitCb(3000ms);

            while(true) {
                std::this_thread::sleep_for(3ms);

                if(waitCb.check()) {
                    break;
                }

                const std::scoped_lock guard{ clientLock };

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

    size_t RFB::X11Client::selectionSourceSize(xcb_atom_t atom) const {
        Application::debug(DebugType::X11Cli, "%s, atom: 0x%08" PRIx32, __FUNCTION__, atom);
        auto targets = selectionSourceTargets();

        if(std::none_of(targets.begin(), targets.end(), [&](auto & trgt) {
        return atom == trgt;
    }))
        return 0;

        const std::scoped_lock guard{ clientLock };
        return clientClipboard.size();
    }

    std::vector<uint8_t> RFB::X11Client::selectionSourceData(xcb_atom_t atom, size_t offset, uint32_t length) const {
        Application::debug(DebugType::X11Cli, "%s, atom: 0x%08" PRIx32 ", offset: %lu, length: %" PRIu32, __FUNCTION__, atom, offset, length);

        auto targets = selectionSourceTargets();

        if(std::none_of(targets.begin(), targets.end(), [&](auto & trgt) {
        return atom == trgt;
    }))
        return {};

        const std::scoped_lock guard{ clientLock };

        if(offset + length <= clientClipboard.size()) {
            auto beg = clientClipboard.begin() + offset;
            return std::vector<uint8_t>(beg, beg + length);
        } else {
            Application::error("%s: invalid length: %" PRIu32 ", offset: %lu", __FUNCTION__, length, offset);
        }

        return {};
    }

    void RFB::X11Client::clientRecvCutTextEvent(std::vector<uint8_t> && buf) {
        Application::debug(DebugType::X11Cli, "%s: data length: %lu", __FUNCTION__, buf.size());

        const std::scoped_lock guard{ clientLock };
        clientClipboard.swap(buf);

        if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE))) {
            paste->setSelectionOwner(*this);
        }
    }

    void RFB::X11Client::xcbDisplayConnectedEvent(void) {
        Application::debug(DebugType::X11Cli, "%s", __FUNCTION__);
        ExtClip::x11AtomsUpdate(*this);

        // init selection copy
        selectionChangedEvent();
    }
}
