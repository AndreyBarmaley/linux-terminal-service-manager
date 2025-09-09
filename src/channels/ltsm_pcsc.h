/***********************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_PCSC_
#define _LTSM_PCSC_

#define LTSM_SESSION_PCSC_VERSION 20250905

#include <cstdint>
#include <stdexcept>

#include "ltsm_compat.h"

namespace PcscLite
{
    enum
    {
        EstablishContext = 0x01,
        ReleaseContext = 0x02,
        ListReaders = 0x03,
        Connect = 0x04,
        Reconnect = 0x05,
        Disconnect = 0x06,
        BeginTransaction = 0x07,
        EndTransaction = 0x08,
        Transmit = 0x09,
        Control = 0x0A,
        Status = 0x0B,
        GetStatusChange = 0x0C,
        Cancel = 0x0D,
        CancelTransaction = 0x0E,
        GetAttrib = 0x0F,
        SetAttrib = 0x10,
        GetVersion = 0x11,
        GetReaderState = 0x12,
        WaitReaderStateChangeStart = 0x13,
        WaitReaderStateChangeStop = 0x14
    };
}

namespace LTSM
{
    namespace PcscOp
    {
        enum
        {
            Init = 0xFD01
        };
    }

    struct pcsc_error : public std::runtime_error
    {
        explicit pcsc_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };
}

#endif // _LTSM_PCSC_
