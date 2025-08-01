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

#ifndef _LIBRFB_X11CLI_
#define _LIBRFB_X11CLI_

#include <mutex>
#include <vector>

#include "librfb_client.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    namespace RFB
    {
        class X11Client : public XCB::RootDisplay, public ClientDecoder, public XCB::SelectionSource, public XCB::SelectionRecipient
        {
            std::vector<uint8_t> clientClipboard;

            mutable std::mutex clientLock;

            mutable uint16_t clipLocalTypes = 0;
            uint16_t clipRemoteTypes = 0;

        protected:

            // selection source
            std::vector<xcb_atom_t> selectionSourceTargets(void) const override;
            bool selectionSourceReady(xcb_atom_t) const override;
            size_t selectionSourceSize(xcb_atom_t) const override;
            std::vector<uint8_t> selectionSourceData(xcb_atom_t, size_t offset, uint32_t length) const override;

            // selection recipient
            void selectionReceiveData(xcb_atom_t, const uint8_t* ptr, uint32_t len) const override;
            void selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) const override;
            void selectionChangedEvent(void) const override;

            // ext clipboard
            uint16_t extClipboardLocalTypes(void) const override;
            std::vector<uint8_t> extClipboardLocalData(uint16_t type) const override;
            void extClipboardRemoteTypesEvent(uint16_t type) override;
            void extClipboardRemoteDataEvent(uint16_t type, std::vector<uint8_t> &&) override;
            void extClipboardSendEvent(const std::vector<uint8_t> &) override;

            void clientRecvCutTextEvent(std::vector<uint8_t> &&) override;

            // x11 event
            void xcbDisplayConnectedEvent(void) override;

        public:
            X11Client();
        };
    }
}

#endif // _LIBRFB_X11CLI_
