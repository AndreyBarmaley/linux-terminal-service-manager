/***************************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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

#ifndef _LTSM_LDAP_WRAPPER_H_
#define _LTSM_LDAP_WRAPPER_H_

#include <ldap.h>

#include <string>
#include <vector>
#include <stdexcept>

namespace LTSM
{
    struct ldap_error : public std::runtime_error
    {
        explicit ldap_error(const std::string & what) : std::runtime_error(what) {}

        explicit ldap_error(const char* what) : std::runtime_error(what) {}
    };

    class LdapWrapper
    {
        LDAP* ldap = nullptr;

    public:
        LdapWrapper();
        ~LdapWrapper();

        std::string findLoginFromDn(std::string_view dn);
        std::string findDnFromCertificate(const uint8_t*, size_t);
    };
}

#endif
