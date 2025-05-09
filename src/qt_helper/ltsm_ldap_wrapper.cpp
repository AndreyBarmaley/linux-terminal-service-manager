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

#include <exception>
#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_ldap_wrapper.h"

namespace LTSM
{
    LdapWrapper::LdapWrapper()
    {
        if(int ret = ldap_initialize(& ldap, nullptr); ret != LDAP_SUCCESS)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "ldap_initialize", ldap_err2string(ret), ret);
            throw ldap_error(NS_FuncName);
        }

        int protover = 3;

        if(int ret = ldap_set_option(ldap, LDAP_OPT_PROTOCOL_VERSION, &protover); ret != LDAP_SUCCESS)
        {
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "ldap_set_option", ldap_err2string(ret), ret);
        }

        struct berval cred
        {
            0, nullptr
        };

        if(int ret = ldap_sasl_bind_s(ldap, nullptr, nullptr, & cred, nullptr, nullptr, nullptr); ret != LDAP_SUCCESS)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "ldap_sasl_bind", ldap_err2string(ret), ret);
            throw ldap_error(NS_FuncName);
        }

        Application::debug(DebugType::Ldap, "%s: bind success", __FUNCTION__);
    }

    LdapWrapper::~LdapWrapper()
    {
        if(ldap)
        {
            ldap_unbind_ext_s(ldap, NULL, NULL);
        }
    }

    std::string LdapWrapper::findLoginFromDn(const std::string & dn)
    {
        std::string res;
        const char* attrs[] = { "uid", nullptr };
        LDAPMessage* msg = NULL;
        int ret = ldap_search_ext_s(ldap, dn.c_str(), LDAP_SCOPE_BASE,
                                    nullptr, (char**) attrs, 0, nullptr, nullptr, nullptr, 0, & msg);

        if(ret == LDAP_SUCCESS)
        {
            Application::debug(DebugType::Ldap, "%s: dn: `%s', found entries: %d", __FUNCTION__, dn.c_str(), ldap_count_entries(ldap, msg));

            if(LDAPMessage* entry = ldap_first_entry(ldap, msg))
            {
                BerElement* ber = nullptr;

                if(char* attr = ldap_first_attribute(ldap, entry, & ber))
                {
                    Application::trace(DebugType::Ldap, "%s: found attribute: `%s'", __FUNCTION__, attr);

                    if(berval** vals = ldap_get_values_len(ldap, entry, attr))
                    {
                        if(0 < ldap_count_values_len(vals))
                        {
                            res.assign(vals[0]->bv_val, vals[0]->bv_len);
                        }

                        ldap_value_free_len(vals);
                    }

                    ldap_memfree(attr);
                }

                if(ber)
                {
                    ber_free(ber, 0);
                }
            }
        }
        else
        {
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "ldap_search", ldap_err2string(ret), ret);
        }

        if(msg)
        {
            ldap_msgfree(msg);
        }

        return res;
    }

    std::string LdapWrapper::findDnFromCertificate(const uint8_t* derform, size_t length)
    {
        std::string res;
        const char* attrs[] = { "userCertificate", nullptr };
        LDAPMessage* msg = NULL;
        int ret = ldap_search_ext_s(ldap, nullptr, LDAP_SCOPE_SUBTREE,
                                    "userCertificate;binary=*", (char**) attrs, 0, nullptr, nullptr, nullptr, 0, & msg);

        if(ret == LDAP_SUCCESS)
        {
            Application::debug(DebugType::Ldap, "%s: found entries: %d", __FUNCTION__, ldap_count_entries(ldap, msg));

            for(LDAPMessage* entry = ldap_first_entry(ldap, msg);
                    entry && res.empty(); entry = ldap_next_entry(ldap, entry))
            {
                char* dn = ldap_get_dn(ldap, entry);
                BerElement* ber = nullptr;

                for(char* attr = ldap_first_attribute(ldap, entry, & ber);
                        attr && res.empty(); attr = ldap_next_attribute(ldap, entry, ber))
                {
                    Application::trace(DebugType::Ldap, "%s: found attribute: `%s'", __FUNCTION__, attr);

                    if(berval** vals = ldap_get_values_len(ldap, entry, attr))
                    {
                        if(size_t count = ldap_count_values_len(vals))
                        {
                            for(size_t ii = 0; ii < count; ++ii)
                            {
                                if(length == vals[ii]->bv_len &&
                                        std::equal(derform, derform + length, (uint8_t*) vals[ii]->bv_val))
                                {
                                    res.assign(dn);
                                }
                            }
                        }

                        ldap_value_free_len(vals);
                    }

                    ldap_memfree(attr);
                }

                if(ber)
                {
                    ber_free(ber, 0);
                }

                if(dn)
                {
                    ldap_memfree(dn);
                }
            }
        }
        else
        {
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "ldap_search", ldap_err2string(ret), ret);
        }

        if(msg)
        {
            ldap_msgfree(msg);
        }

        return res;
    }
}
