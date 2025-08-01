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
#include "librfb_winclient.h"

using namespace std::chrono_literals;

namespace LTSM
{
    RFB::WinClient::WinClient()
    {
    }

    void RFB::WinClient::extClipboardSendEvent(const std::vector<uint8_t> & buf)
    {
        Application::debug(DebugType::WinCli, "%s, length: %" PRIu32, __FUNCTION__, buf.size());
        sendCutTextEvent(buf.data(), buf.size(), true);
    }

    uint16_t RFB::WinClient::extClipboardLocalTypes(void) const
    {
        return clipLocalTypes;
    }

    std::vector<uint8_t> RFB::WinClient::extClipboardLocalData(uint16_t type) const
    {
        if(0 == extClipboardLocalCaps())
        {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }

        Application::debug(DebugType::WinCli, "%s", __FUNCTION__);

/*
        auto ptr = const_cast<RFB::WinClient*>(this);
        if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY)))
        {
            for(const auto & atom: ExtClip::typesToX11Atoms(type, *this))
            {
                ptr->clientClipboard.clear();
                copy->convertSelection(atom, *this);

                // wait data from selectionReceiveData
                Tools::Timeout waitCb(100ms);

                while(true)
                {
                    std::this_thread::sleep_for(3ms);
                    
                    if(waitCb.check())
                        break;

                    const std::scoped_lock guard{ clientLock };

                    if(clientClipboard.size())
                        return clientClipboard;
                }
            }
        }
*/
        return {};
    }

    void RFB::WinClient::extClipboardRemoteTypesEvent(uint16_t types)
    {
        Application::debug(DebugType::WinCli, "%s, types: 0x%04" PRIx16, __FUNCTION__, types);
        if(extClipboardRemoteCaps())
        {
            clipRemoteTypes = types;

            //if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE)))
            //        paste->setSelectionOwner(*this);
        }
        else
        {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::WinClient::extClipboardRemoteDataEvent(uint16_t type, std::vector<uint8_t> && buf)
    {
        Application::debug(DebugType::WinCli, "%s, type: 0x%04" PRIx16 ", length: %" PRIu32, __FUNCTION__, type, buf.size());
        if(extClipboardRemoteCaps())
        {
            const std::scoped_lock guard{ clientLock };
            clientClipboard.swap(buf);
        }
        else
        {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::WinClient::clientRecvCutTextEvent(std::vector<uint8_t> && buf)
    {
        Application::debug(DebugType::WinCli, "%s: data length: %" PRIu32, __FUNCTION__, buf.size());

        const std::scoped_lock guard{ clientLock };
        clientClipboard.swap(buf);

        //if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE)))
        //    paste->setSelectionOwner(*this);
    }
}
