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
    const char* LdapResult::dn(void) const
    {
        return _dn.get();
    }

    const char* LdapResult::attr(void) const
    {
        return _attr.get();
    }

    int LdapResult::valuesCount(void) const
    {
        return _vals ? ldap_count_values_len(_vals.get()) : 0;
    }

    std::string_view LdapResult::valueString(void) const
    {
        if(0 < valuesCount())
        {
            auto arr = _vals.get();
            return string2view(arr[0]->bv_val, arr[0]->bv_val + arr[0]->bv_len);
        }
        
        return {};
    }

    std::list<std::string_view> LdapResult::valueListString(void) const
    {
        int count = valuesCount();
        auto arr = _vals.get();
        std::list<std::string_view> res;

        for(int it = 0; it < count; ++it)
        {
            res.emplace_back(string2view(arr[0]->bv_val, arr[0]->bv_val + arr[0]->bv_len));
        }

        return res;
    }

    bool LdapResult::hasValue(const char* ptr, size_t len) const
    {
        int count = valuesCount();
        auto arr = _vals.get();

        for(int it = 0; it < count; ++it)
        {
            if(len == arr[it]->bv_len && std::equal(ptr, ptr + len, arr[it]->bv_val))
                return true;
        }
        
        return false;
    }

    LdapWrapper::LdapWrapper()
    {
        if(int ret = ldap_initialize(& ldap, nullptr); ret != LDAP_SUCCESS)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "ldap_initialize", ldap_err2string(ret), ret);
            throw ldap_error(NS_FuncName);
        }

        int protover = 3;

        if(int ret = ldap_set_option(ldap, LDAP_OPT_PROTOCOL_VERSION, & protover); ret != LDAP_SUCCESS)
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

    std::list<LdapResult> LdapWrapper::search(int scope, std::vector<const char*> attrs, const char* filter, const char* basedn)
    {
        if(attrs.size() && attrs.back())
            attrs.push_back(nullptr);

        LDAPMessage* msg = NULL;
        int ret = ldap_search_ext_s(ldap, basedn, scope, filter, 
                    attrs.size() ? (char**) attrs.data() : nullptr, 0, nullptr, nullptr, nullptr, 0, & msg);
                                    
        std::unique_ptr<LDAPMessage, int(*)(LDAPMessage*)> guard_msg{ msg, ldap_msgfree };

        if(ret != LDAP_SUCCESS)
        {
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "ldap_search", ldap_err2string(ret), ret);
            return {};
        }

        Application::debug(DebugType::Ldap, "%s: found entries: %d", __FUNCTION__, ldap_count_entries(ldap, msg));
        std::list<LdapResult> res;

        for(LDAPMessage* entry = ldap_first_entry(ldap, msg); entry; entry = ldap_next_entry(ldap, entry))
        {
            std::shared_ptr<char[]> dn{ ldap_get_dn(ldap, entry), ldap_memfree };
            Application::debug(DebugType::Ldap, "%s: dn: `%s'", __FUNCTION__, dn.get());

            BerElement* ber = nullptr;

            for(char* attr = ldap_first_attribute(ldap, entry, & ber); attr; attr = ldap_next_attribute(ldap, entry, ber))
            {
                std::unique_ptr<char[], void(*)(void*)> guard_attr{ attr, ldap_memfree };
                std::unique_ptr<berval*, void(*)(berval**)> vals{ ldap_get_values_len(ldap, entry, attr), ldap_value_free_len };
                
                Application::debug(DebugType::Ldap, "%s: attr: `%s'", __FUNCTION__, attr);
                res.emplace_back(LdapResult{ dn, std::move(guard_attr), std::move(vals) });
            }

            if(ber)
            {
                ber_free(ber, 0);
            }
        }
        
        return res;
    }

    std::string LdapWrapper::findLoginFromDn(const std::string & dn)
    {
        if(auto res = search(LDAP_SCOPE_BASE, { "uid" }, nullptr, dn.c_str()); res.size())
            return view2string(res.front().valueString());

        return "";
    }

    std::string LdapWrapper::findDnFromCertificate(const uint8_t* derform, size_t length)
    {
        if(auto res = search(LDAP_SCOPE_SUBTREE, { "userCertificate" }, "userCertificate;binary=*"); res.size())
        {
            if(auto it = std::find_if(res.begin(), res.end(), [=](auto & st)
                { return st.hasValue((const char*) derform, length); }); it != res.end())
                return it->dn();
        }

        return "";
    }
}
