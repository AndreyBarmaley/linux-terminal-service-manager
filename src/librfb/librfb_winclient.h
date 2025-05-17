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

#ifndef _LIBRFB_WINCLI_
#define _LIBRFB_WINCLI_

#include <mutex>
#include <vector>

#include "librfb_client.h"

namespace LTSM
{
    namespace RFB
    {
        class WinClient : public ClientDecoder
        {
            std::vector<uint8_t> clientClipboard;

            mutable std::mutex clientLock;

            mutable uint16_t clipLocalTypes = 0;
            uint16_t clipRemoteTypes = 0;

        protected:

            // ext clipboard
            uint16_t extClipboardLocalTypes(void) const override;
            std::vector<uint8_t> extClipboardLocalData(uint16_t type) const override;
            void extClipboardRemoteTypesEvent(uint16_t type) override;
            void extClipboardRemoteDataEvent(uint16_t type, std::vector<uint8_t> &&) override;
            void extClipboardSendEvent(const std::vector<uint8_t> &) override;

            void clientRecvCutTextEvent(std::vector<uint8_t> &&) override;

        public:
            WinClient();
        };
    }
}

#endif // _LIBRFB_WINCLI_
