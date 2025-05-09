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
    inline static const char* dbus_manager_service_name = "ltsm.manager.service";
    inline static const char* dbus_manager_service_path = "/ltsm/manager/service";

    inline static const char* dbus_session_fuse_name = "ltsm.session.fuse";
    inline static const char* dbus_session_fuse_path = "/ltsm/session/fuse";

    inline static const char* dbus_session_audio_name = "ltsm.session.audio";
    inline static const char* dbus_session_audio_path = "/ltsm/session/audio";

    inline static const char* dbus_session_pcsc_name = "ltsm.session.pcsc";
    inline static const char* dbus_session_pcsc_path = "/ltsm/session/pcsc";

    inline static const int service_version = 20250509;

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
    inline static const bool BigEndian = false;
#else
    inline static const bool BigEndian = true;
#endif

    namespace NotifyParams
    {
        enum IconType { Information, Warning, Error, Question };
        enum UrgencyLevel { Low = 0, Normal = 1, Critical = 2 };
    };
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
