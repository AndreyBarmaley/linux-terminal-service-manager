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
#include "librfb_winclient.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM {
    RFB::WinClient::WinClient(const asio::any_io_executor& ctx) : ClientDecoder(ctx) {
    }

    void RFB::WinClient::extClipboardSendBuf(std::vector<uint8_t>&& buf) const {
        Application::debug(DebugType::X11Cli, "{}, length: {}", NS_FuncNameV, buf.size());
        asio::co_spawn(rfb_strand(), [this, buf=std::move(buf)]() -> asio::awaitable<void> {
            co_await sendCutTextAwait(buf, true);
            co_return;
        }, asio::detached);
    }

    uint16_t RFB::WinClient::extClipboardLocalTypes(void) const {
        return clipLocalTypes;
    }

    asio::awaitable<clipboard_buf> RFB::WinClient::extClipboardLocalDataAwait(uint16_t type) {
        if(0 == extClipboardLocalCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        Application::debug(DebugType::WinCli, "{}", NS_FuncNameV);

        /*
                auto ptr = const_cast<RFB::WinClient*>(this);
                if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY)))
                {
                    for(const auto & atom: ExtClip::typesToX11Atoms(type, *this))
                    {
                        // see X11Client::extClipboardLocalDataAwait
                    }
                }
        */
        co_return clipboard_buf{};
    }

    asio::awaitable<void> RFB::WinClient::extClipboardRemoteTypesAwait(uint16_t types) {
        Application::debug(DebugType::WinCli, "{}, types: {:#06x}", NS_FuncNameV, types);

        if(! extClipboardRemoteCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        clipRemoteTypes = types;

        //if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE)))
        //        paste->setSelectionOwner(*this);
        co_return;
    }

    asio::awaitable<void> RFB::WinClient::extClipboardRemoteDataAwait(uint16_t type, std::vector<uint8_t> buf) {
        // xcb context
        Application::debug(DebugType::WinCli, "{}, type: {:#06x}, length: {}", NS_FuncNameV, type, buf.size());

        if(! extClipboardRemoteCaps()) {
            Application::error("{}: unsupported encoding: {}", NS_FuncNameV, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncNameS);
        }

        clientClipboard.swap(buf);
        co_return;
    }

    void RFB::WinClient::clientRecvCutTextEvent(std::vector<uint8_t> && buf) {
        // xcb context
        Application::debug(DebugType::WinCli, "{}: data length: {}", NS_FuncNameV, buf.size());
        clientClipboard.swap(buf);

        //if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE)))
        //    paste->setSelectionOwner(*this);
    }
}
