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

#include <list>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

#include "ltsm_compat.h"

namespace LTSM
{
    struct ldap_error : public std::runtime_error
    {
        explicit ldap_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };

    struct LdapResult
    {
        std::shared_ptr<char[]> _dn;
        std::unique_ptr<char[], void(*)(void*)> _attr;
        std::unique_ptr<berval*, void(*)(berval**)> _vals;

        const char* dn(void) const;
        const char* attr(void) const;

        int valuesCount(void) const;
        std::string_view valueString(void) const;
        std::list<std::string_view> valueListString(void) const;
        
        bool hasValue(const char* ptr, size_t len) const;
    };

    class LdapWrapper
    {
        LDAP* ldap = nullptr;

    public:
        LdapWrapper();
        ~LdapWrapper();

        std::list<LdapResult> search(int scope, std::vector<const char*> attrs, const char* filter = nullptr, const char* basedn = nullptr);
    };
}

#endif
