/***************************************************************************
 *   Copyright © 2023 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   https://github.com/AndreyBarmaley/gssapi-layer-cpp                    *
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

#include <sstream>
#include <iostream>

#include "ltsm_gsslayer.h"

namespace Gss
{
    int apiVersion(void)
    {
	return 20210328;
    }

    std::string error2str(OM_uint32 code1, OM_uint32 code2)
    {
        OM_uint32 ctx, stat;
        gss_buffer_desc msg1, msg2;

        ctx = 0;
        gss_display_status(& stat, code1, GSS_C_GSS_CODE, GSS_C_NULL_OID, & ctx, & msg1);
        ctx = 0;
        gss_display_status(& stat, code2, GSS_C_MECH_CODE, GSS_C_NULL_OID, & ctx, & msg2);

        std::ostringstream os;
        os << (const char*) msg1.value << ", (" << (const char*) msg2.value << ")" << ", codes: [" << code1 << ", " << code2 << "]";

        gss_release_buffer(& stat, & msg1);
        gss_release_buffer(& stat, & msg2);

        return os.str();
    }

    gss_name_t importName(std::string_view name, const NameType & type, ErrorCodes* err)
    {
        OM_uint32 stat;
        gss_OID oid = GSS_C_NO_OID;

        switch(type)
        {
            case NameType::NoName: oid = (gss_OID) GSS_C_NO_NAME; break;
            case NameType::NoOid: oid = GSS_C_NO_OID; break;
            case NameType::NtAnonymous: oid = GSS_C_NT_ANONYMOUS; break;
            case NameType::NtExportName: oid = GSS_C_NT_EXPORT_NAME; break;
            case NameType::NtHostService: oid = GSS_C_NT_HOSTBASED_SERVICE; break;
            case NameType::NtMachineUid: oid = GSS_C_NT_MACHINE_UID_NAME; break;
            case NameType::NtStringUid: oid = GSS_C_NT_STRING_UID_NAME; break;
            case NameType::NtUserName: oid = GSS_C_NT_USER_NAME; break;
        }
 
        gss_buffer_desc buf{ name.size(), (void*) name.data() };
        gss_name_t res = nullptr;

        auto ret = gss_import_name(& stat, & buf, oid, & res);

        if(ret == GSS_S_COMPLETE)
            return res;

        if(err)
        {
            err->func = "gss_import_name";
            err->code1 = ret;
            err->code2 = stat;
        }

        gss_release_name(& stat, & res);
        return nullptr;
    }

    std::string displayName(const gss_name_t & name, ErrorCodes* err)
    {
        OM_uint32 stat;
        gss_buffer_desc buf;
        std::string res;

        auto ret = gss_display_name(& stat, name, & buf, nullptr);

        if(ret == GSS_S_COMPLETE)
            res.assign((char*) buf.value, (char*) buf.value + buf.length);
        else
        if(err)
        {
            err->func = "gss_display_name";
            err->code1 = ret;
            err->code2 = stat;
        }

        gss_release_buffer(& stat, & buf);
        return res;
    }

    std::string canonicalizeName(const gss_name_t & name1, const gss_OID & mech, ErrorCodes* err)
    {
        OM_uint32 stat;
        gss_name_t name2 = nullptr;
        std::string res;

	auto ret = gss_canonicalize_name(& stat, name1, mech, & name2);

        if(ret == GSS_S_COMPLETE)
    	    res = displayName(name2, err);
        else
        if(err)
        {
            err->func = "gss_canonicalize_name";
            err->code1 = ret;
            err->code2 = stat;
        }

        gss_release_name(& stat, & name2);
        return res;
    }

    std::string oidName(const gss_OID & oid, ErrorCodes* err)
    {
        OM_uint32 stat;
        gss_buffer_desc buf;

        auto ret = gss_oid_to_str(& stat, oid, & buf);
        std::string res;

        if(ret == GSS_S_COMPLETE)
            res.assign((char*) buf.value, (char*) buf.value + buf.length);
        else
        if(err)
        {
            err->func = "gss_display_name";
            err->code1 = ret;
            err->code2 = stat;
        }

        gss_release_buffer(& stat, & buf);

/*
        "{ 1 2 840 113554 1 2 1 }" gssapi generic
        "{ 1 2 840 113554 1 2 2 }" gssapi krb5
        "{ 1 2 840 113554 1 2 3 }" gssapi krb5v2

        "{ 1 2 840 113554 1 2 1 1 }" gssapi generic user-name
        "{ 1 2 840 113554 1 2 1 2 }" gssapi generic machine-uid-name
        "{ 1 2 840 113554 1 2 1 3 }" gssapi generic string-uid-name
        "{ 1 2 840 113554 1 2 1 4 }" gssapi generic service-name

        "{ 1 2 840 113554 1 2 2 1 }" gssapi krb5 krb5-name
        "{ 1 2 840 113554 1 2 2 2 }" gssapi krb5 krb5-principal
        "{ 1 2 840 113554 1 2 2 3 }" gssapi krb5 user-to-user-mechanism
*/

        return res;
    }

    const char* flagName(const ContextFlag & flag)
    {
        switch(flag)
        {
            case ContextFlag::Delegate: return "delegate";
            case ContextFlag::Mutual:   return "mutual";
            case ContextFlag::Replay:   return "replay";
            case ContextFlag::Sequence: return "sequence";
            case ContextFlag::Confidential: return "confidential";
            case ContextFlag::Integrity: return "integrity";
            case ContextFlag::Anonymous: return "anonymous";
            case ContextFlag::Protection: return "protection";
            case ContextFlag::Transfer: return"transfer";
            default: break;
        }

        return "unknown";
    }

    std::list<ContextFlag> exportFlags(int flags)
    {
        auto all = { ContextFlag::Delegate, ContextFlag::Mutual, ContextFlag::Replay, ContextFlag::Sequence, ContextFlag::Confidential,
                ContextFlag::Integrity, ContextFlag::Anonymous, ContextFlag::Protection, ContextFlag::Transfer };

        std::list<ContextFlag> res;

        for(auto & v : all)
            if(flags & v) res.push_front(v);

        return res;
    }

    std::list<std::string> nameMechs(const gss_name_t & name, ErrorCodes* err)
    {
        std::list<std::string> res;

        OM_uint32 stat;
        gss_OID_set mech_types;

        auto ret = gss_inquire_mechs_for_name(& stat, name, & mech_types);
        if(ret == GSS_S_COMPLETE)
        {
            for(int it = 0; it < mech_types->count; ++it)
            {
                auto name = oidName(& mech_types->elements[it]);

                if(! name.empty())
                    res.emplace_back(std::move(name));
            }
        }
        else
        if(err)
        {
            err->func = "gss_inquire_mechs_for_name";
            err->code1 = ret;
            err->code2 = stat;
        }

        gss_release_oid_set(& stat, & mech_types);
        return res;
    }

    std::list<std::string> mechNames(const gss_OID & oid, ErrorCodes* err)
    {
        std::list<std::string> res;

        OM_uint32 stat;
        gss_OID_set mech_names;

        auto ret = gss_inquire_names_for_mech(& stat, oid, & mech_names);
        if(ret == GSS_S_COMPLETE)
        {
            for(int it = 0; it < mech_names->count; ++it)
            {
                auto name = oidName(& mech_names->elements[it]);

                if(! name.empty())
                    res.emplace_back(std::move(name));
            }
        }
        else
        if(err)
        {
            err->func = "gss_inquire_names_for_mech";
            err->code1 = ret;
            err->code2 = stat;
        }

        gss_release_oid_set(& stat, & mech_names);
        return res;
    }

    // Credential
    Credential::~Credential()
    {
        if(mechs)
        {
            OM_uint32 stat = 0;
            gss_release_oid_set(& stat, & mechs);
        }

        if(cred)
        {
            OM_uint32 stat = 0;
            gss_release_cred(& stat, & cred);
        }

        if(name)
        {
            OM_uint32 stat = 0;
            gss_release_name(& stat, & name);
        }
    }

    // Security
    Security::~Security()
    {
        if(sec)
        {
            OM_uint32 stat = 0;
            gss_delete_sec_context(& stat, & sec, GSS_C_NO_BUFFER);
        }

        if(name)
        {
            OM_uint32 stat = 0;
            gss_release_name(& stat, & name);
        }
    }

    CredentialPtr acquireCredential(std::string_view service, const NameType & type, const CredentialUsage & usage, ErrorCodes* err)
    {
        auto name = importName(service, type, err);

        if(! name)
            return nullptr;

        CredentialPtr res = std::make_unique<Credential>();
	res->name = name;

        OM_uint32 stat;
        auto ret = gss_acquire_cred(& stat, res->name, 0, GSS_C_NULL_OID_SET, usage, & res->cred, & res->mechs, & res->timerec);

        if(ret == GSS_S_COMPLETE)
            return res;

        if(err)
        {
            err->func = "gss_acquire_cred";
            err->code1 = ret;
            err->code2 = stat;
        }

        return nullptr;
    }

    CredentialPtr acquireUserPasswordCredential(std::string_view username, std::string_view password, ErrorCodes* err)
    {
        auto name = importName(username.data(), Gss::NameType::NtUserName, err);

        if(! name)
            return nullptr;

        gss_buffer_desc pass{ password.size(), (void*) password.data() };

        CredentialPtr res = std::make_unique<Credential>();
	res->name = name;

        OM_uint32 stat;
        auto ret = gss_acquire_cred_with_password(& stat, res->name, & pass, 0, GSS_C_NULL_OID_SET, Gss::CredentialUsage::Initiate, & res->cred, & res->mechs, & res->timerec);

        if(ret == GSS_S_COMPLETE)
            return res;

        if(err)
        {
            err->func = "gss_acquire_cred_with_password";
            err->code1 = ret;
            err->code2 = stat;
        }

        return nullptr;
    }

    CredentialPtr acquireUserCredential(std::string_view username, ErrorCodes* err)
    {
        return acquireCredential(username.data(), Gss::NameType::NtUserName, Gss::CredentialUsage::Initiate, err);
    }

    CredentialPtr acquireServiceCredential(std::string_view service, ErrorCodes* err)
    {
        return acquireCredential(service.data(), Gss::NameType::NtHostService, Gss::CredentialUsage::Accept, err);
    }

    // BaseContext
    std::vector<uint8_t> BaseContext::recvMessage(void)
    {
	if(! ctx)
	    throw std::invalid_argument("context is null");

        OM_uint32 stat;
        auto buf = recvToken();

        gss_buffer_desc in_buf{ buf.size(), (void*) buf.data() };
        gss_buffer_desc out_buf{ 0, nullptr, };

        auto ret = gss_unwrap(& stat, ctx->sec, & in_buf, & out_buf, nullptr, nullptr);
        std::vector<uint8_t> res;

        if(ret == GSS_S_COMPLETE)
        {
            res.assign((uint8_t*) out_buf.value, (uint8_t*) out_buf.value + out_buf.length);
        }
        else
        {
            error(__FUNCTION__, "gss_unwrap", ret, stat);
            res.clear();
        }
        
        gss_release_buffer(& stat, & out_buf);
        return res;
    }

    bool BaseContext::sendMessage(const void* buf, size_t len, bool encrypt)
    {
	if(! ctx)
	    throw std::invalid_argument("context is null");

        OM_uint32 stat;
        gss_buffer_desc in_buf{ len, (void*) buf };
        gss_buffer_desc out_buf{ 0, nullptr, };

        auto ret = gss_wrap(& stat, ctx->sec, encrypt, GSS_C_QOP_DEFAULT, & in_buf, nullptr, & out_buf);
        bool res = true;

        if(ret == GSS_S_COMPLETE)
        {
            sendToken(out_buf.value, out_buf.length);
        }
        else
        {
            error(__FUNCTION__, "gss_wrap", ret, stat);
            res = false;
        }
        
        gss_release_buffer(& stat, & out_buf);
        return res;
    }

    bool BaseContext::recvMIC(const void* msg, size_t msgsz)
    {
	if(! ctx)
	    throw std::invalid_argument("context is null");

        // recv token
        auto buf = recvToken();

        OM_uint32 stat;
        gss_buffer_desc in_buf{ msgsz, (void*) msg };
        gss_buffer_desc out_buf{ buf.size(), (void*) buf.data() };

        auto ret = gss_verify_mic(& stat, ctx->sec, & in_buf, & out_buf, nullptr);

        if(ret == GSS_S_COMPLETE)
            return true;

        error(__FUNCTION__, "gss_verify_mic", ret, stat);
        return false;
    }

    bool BaseContext::sendMIC(const void* msg, size_t msgsz)
    {
	if(! ctx)
	    throw std::invalid_argument("context is null");

        OM_uint32 stat;
        gss_buffer_desc in_buf{ msgsz, (void*) msg };
        gss_buffer_desc out_buf{ 0, nullptr };

        auto ret = gss_get_mic(& stat, ctx->sec, GSS_C_QOP_DEFAULT, & in_buf, & out_buf);
        bool res = true;

        if(ret == GSS_S_COMPLETE)
        {
            sendToken(out_buf.value, out_buf.length);
        }
        else
        {
            error(__FUNCTION__, "gss_get_mic", ret, stat);
            res = false;
        }

        gss_release_buffer(& stat, & out_buf);
        return res;
    }

    // ServiceContext
    bool ServiceContext::acceptClient(CredentialPtr ptr)
    {
        ctx = std::make_unique<Security>();

        OM_uint32 stat;
        OM_uint32 ret = GSS_S_CONTINUE_NEEDED;

        while(ret == GSS_S_CONTINUE_NEEDED)
        {
            // recv token
            auto buf = recvToken();

            gss_buffer_desc recv_tok{ buf.size(), (void*) buf.data() };
            gss_buffer_desc send_tok{ 0, nullptr };

            ret = gss_accept_sec_context(& stat, & ctx->sec, ptr ? ptr->cred : GSS_C_NO_CREDENTIAL, & recv_tok, GSS_C_NO_CHANNEL_BINDINGS,
                                     & ctx->name, & ctx->mech, & send_tok, & ctx->supported, & ctx->timerec, nullptr);

            if(0 < send_tok.length)
            {
                sendToken(send_tok.value, send_tok.length);
                gss_release_buffer(& stat, & send_tok);
            }
        }

        if(ret == GSS_S_COMPLETE)
	{
    	    if(ptr)
        	ctx->cred = std::move(ptr);

            return true;
	}

	ctx.reset();
	error(__FUNCTION__, "gss_accept_sec_context", ret, stat);

        return false;
    }

    // ClientContext
    bool ClientContext::connectService(std::string_view service, bool mutual, CredentialPtr ptr)
    {
	ErrorCodes err;
        auto name = importName(service, NameType::NtHostService, & err);

        if(! name)
	{
	    error(__FUNCTION__, err.func, err.code1, err.code2);
            return false;
	}

        ctx = std::make_unique<Security>();
	ctx->name = name;

        gss_channel_bindings_t input_chan_bindings = nullptr; // no channel bindings
        std::vector<uint8_t> buf;
        OM_uint32 stat;

        int flags = GSS_C_REPLAY_FLAG;

        if(mutual)
            flags |= GSS_C_MUTUAL_FLAG;

        gss_buffer_desc recv_tok{ 0, nullptr };
        gss_buffer_desc send_tok{ service.size(), (void*) service.data() };

        OM_uint32 ret = GSS_S_CONTINUE_NEEDED;
        while(ret == GSS_S_CONTINUE_NEEDED)
        {
            ret = gss_init_sec_context(& stat, ptr ? ptr->cred : GSS_C_NO_CREDENTIAL, & ctx->sec, ctx->name, GSS_C_NULL_OID, flags,
                                    0, input_chan_bindings, & recv_tok, & ctx->mech, & send_tok, & ctx->supported, & ctx->timerec);

            if(0 < send_tok.length)
            {
                sendToken(send_tok.value, send_tok.length);
                if(send_tok.value != service.data())
                    gss_release_buffer(& stat, & send_tok);
            }

            if(ret == GSS_S_CONTINUE_NEEDED)
            {
                buf = recvToken();
                recv_tok.length = buf.size();
                recv_tok.value = buf.data();
            }
        }

        if(ret == GSS_S_COMPLETE)
	{
    	    if(ptr)
        	ctx->cred = std::move(ptr);

            return true;
	}

	ctx.reset();
	error(__FUNCTION__, "gss_init_sec_context", ret, stat);

        return false;
    }
}
