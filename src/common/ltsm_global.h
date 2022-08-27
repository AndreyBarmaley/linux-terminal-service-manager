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

#ifndef _LTSM_GLOBALS_
#define _LTSM_GLOBALS_

namespace LTSM
{
    enum class XvfbMode { SessionLogin, SessionOnline, SessionSleep };
    enum class SessionPolicy { AuthLock, AuthTake, AuthShare };

    inline static const char* dbus_service_name = "ltsm.manager.service";
    inline static const char* dbus_object_path = "/ltsm/manager/service";

    inline static const int service_version = 20220827;

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
    inline static const bool big_endian = false;
#else
    inline static const bool big_endian = true;
#endif
}

#ifdef LTSM_BUILD_STD_MAP
#include <unordered_map>
#include <unordered_set>
#define INTMAP std::unordered_map
#define INTSET std::unordered_set
#else
#include "flat_hash_map/unordered_map.hpp"
#define INTMAP ska::unordered_map
#define INTSET ska::unordered_set
#endif

#endif // _LTSM_GLOBALS_
