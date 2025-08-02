/***************************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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

#include <dlfcn.h>

#include <array>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <numeric>
#include <cinttypes>
#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_pkcs11_wrapper.h"

namespace LTSM
{
    template <typename Iterator>
    std::string trimString(Iterator first, Iterator last)
    {
        if(first == last)
        {
            return "";
        }

        while(first != last && std::isspace(*first))
        {
            first = std::next(first);
        }

        if(first == last)
        {
            return "";
        }

        last = std::prev(last);

        while(last != first && std::isspace(*last))
        {
            last = std::prev(last);
        }

        return std::string(first, last + 1);
    }

    // SlotInfo
    std::string PKCS11::SlotInfo::getDescription(void) const
    {
        return trimString(std::begin(slotDescription), std::end(slotDescription));
    }

    std::string PKCS11::SlotInfo::getManufacturerID(void) const
    {
        return trimString(std::begin(manufacturerID), std::end(manufacturerID));
    }

    bool PKCS11::SlotInfo::flagTokenPresent(void) const
    {
        return (flags & CKF_TOKEN_PRESENT);
    }

    bool PKCS11::SlotInfo::flagRemovableDevice(void) const
    {
        return (flags & CKF_REMOVABLE_DEVICE);
    }

    // TokenInfo
    std::string PKCS11::TokenInfo::getLabel(void) const
    {
        return trimString(std::begin(label), std::end(label));
    }

    std::string PKCS11::TokenInfo::getManufacturerID(void) const
    {
        return trimString(std::begin(manufacturerID), std::end(manufacturerID));
    }

    std::string PKCS11::TokenInfo::getModel(void) const
    {
        return trimString(std::begin(model), std::end(model));
    }

    std::string PKCS11::TokenInfo::getSerialNumber(void) const
    {
        return trimString(std::begin(serialNumber), std::end(serialNumber));
    }

    std::string PKCS11::TokenInfo::getUtcTime(void) const
    {
        return trimString(std::begin(utcTime), std::end(utcTime));
    }

    bool PKCS11::TokenInfo::flagWriteProtected(void) const
    {
        return (flags & CKF_WRITE_PROTECTED);
    }

    bool PKCS11::TokenInfo::flagLoginRequired(void) const
    {
        return (flags & CKF_LOGIN_REQUIRED);
    }

    bool PKCS11::TokenInfo::flagTokenInitialized(void) const
    {
        return (flags & CKF_TOKEN_INITIALIZED);
    }

    // SessionInfo
    bool PKCS11::SessionInfo::flagRwSession(void) const
    {
        return (flags & CKF_RW_SESSION);
    }

    bool PKCS11::SessionInfo::flagSerialSession(void) const
    {
        return (flags & CKF_SERIAL_SESSION);
    }

    // LibraryInfo
    std::string PKCS11::LibraryInfo::getDescription(void) const
    {
        return trimString(std::begin(libraryDescription), std::end(libraryDescription));
    }

    std::string PKCS11::LibraryInfo::getManufacturerID(void) const
    {
        return trimString(std::begin(manufacturerID), std::end(manufacturerID));
    }

    // Library
    PKCS11::Library::Library() :
        dll{ nullptr, dlclose }
    {
    }

    PKCS11::Library::Library(const std::string & name) :
        dll{ nullptr, dlclose }
    {
        if(name.empty())
        {
            Application::error("%s: failed, name empty", __FUNCTION__);
            throw pkcs11_error(NS_FuncName);
        }

        dll.reset(dlopen(name.c_str(), RTLD_LAZY));

        if(! dll)
        {
            Application::error("%s: %s failed, name: %s", __FUNCTION__, "dlopen", name.c_str());
            throw pkcs11_error(NS_FuncName);
        }

        auto pfGetFunctionList = (CK_C_GetFunctionList) dlsym(dll.get(), "C_GetFunctionList");

        if(! pfGetFunctionList)
        {
            Application::error("%s: %s symbol not found", __FUNCTION__, "C_GetFunctionList");
            throw pkcs11_error(NS_FuncName);
        }

        auto ret = pfGetFunctionList(& pFunctionList);

        if(ret != CKR_OK)
        {
            Application::error("%s: %s failed, code: 0x%" PRIx64 ", rv: `%s'", __FUNCTION__, "C_GetFunctionList", ret,
                               rvString(ret));
            throw pkcs11_error(NS_FuncName);
        }

        ret = pFunctionList->C_Initialize(nullptr);

        if(ret != CKR_OK)
        {
            Application::error("%s: %s failed, code: 0x%" PRIx64 ", rv: `%s'", __FUNCTION__, "C_Initialize", ret, rvString(ret));
            throw pkcs11_error(NS_FuncName);
        }
    }

    PKCS11::Library::Library(Library && lib) noexcept :
        dll
    {
        nullptr, dlclose
    }

    {
        sessions.swap(lib.sessions);
        dll.reset(lib.dll.release());
        pFunctionList = lib.pFunctionList;
        lib.pFunctionList = nullptr;
    }

    PKCS11::Library & PKCS11::Library::operator=(Library && lib) noexcept
    {
        sessions.swap(lib.sessions);
        dll.reset(lib.dll.release());
        pFunctionList = lib.pFunctionList;
        lib.pFunctionList = nullptr;
        return *this;
    }

    PKCS11::Library::~Library()
    {
        if(pFunctionList)
        {
            for(const auto & sid : sessions)
            {
                pFunctionList->C_CloseSession(sid);
            }

            pFunctionList->C_Finalize(nullptr);
        }
    }

    PKCS11::LibraryInfoPtr PKCS11::Library::getLibraryInfo(void) const
    {
        auto info = std::make_unique<LibraryInfo>();
        auto ret = pFunctionList->C_GetInfo(info.get());

        if(ret == CKR_OK)
        {
            return info;
        }

        Application::error("%s: %s failed, code: 0x%" PRIx64 ", rv: `%s'", __FUNCTION__, "C_GetInfo", ret, rvString(ret));
        return nullptr;
    }

    PKCS11::SlotList PKCS11::getSlots(bool tokenPresentOnly, const LibraryPtr & lib)
    {
        CK_ULONG pulCount = 0;

        if(auto ret = lib->func()->C_GetSlotList(tokenPresentOnly, nullptr, & pulCount); ret != CKR_OK)
        {
            Application::error("%s: %s failed, code: 0x%" PRIx64 ", rv: `%s'",
                               __FUNCTION__, "C_GetSlotList", ret, rvString(ret));
            return {};
        }

        if(0 == pulCount)
        {
            Application::debug(DebugType::Pkcs11, "%s: empty %s", __FUNCTION__, "slots");
            return {};
        }

        Application::debug(DebugType::Pkcs11, "%s: connected slots: %" PRIu64, __FUNCTION__, pulCount);
        std::vector<SlotId> slots(pulCount);

        if(auto ret = lib->func()->C_GetSlotList(tokenPresentOnly, slots.data(), & pulCount); ret != CKR_OK)
        {
            Application::error("%s: %s failed, code: 0x%" PRIx64 ", rv: `%s'",
                               __FUNCTION__, "C_GetSlotList", ret, rvString(ret));
            return {};
        }

        SlotList res;

        for(const auto & id : slots)
        {
            res.emplace_front(id, lib);
        }

        return res;
    }

    void PKCS11::Library::sessionClose(CK_SESSION_HANDLE sid)
    {
        Application::debug(DebugType::Pkcs11, "%s: session: %" PRIu64, __FUNCTION__, sid);
        auto ret = pFunctionList->C_CloseSession(sid);

        if(ret != CKR_OK)
        {
            Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                               __FUNCTION__, "C_CloseSession", sid, ret, rvString(ret));
        }

        sessions.remove(sid);
    }

    CK_SESSION_HANDLE PKCS11::Library::sessionOpen(const SlotId & id, bool rwmode)
    {
        Application::debug(DebugType::Pkcs11, "%s: slot: %" PRIu64, __FUNCTION__, id);
        CK_FLAGS flags = CKF_SERIAL_SESSION;

        if(rwmode)
        {
            flags |= CKF_RW_SESSION;
        }

        CK_SESSION_HANDLE sid = 0;
        auto ret = pFunctionList->C_OpenSession(id, flags, nullptr, nullptr, & sid);

        if(ret != CKR_OK)
        {
            Application::error("%s: %s failed, slot: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                               __FUNCTION__, "C_OpenSession", id, ret, rvString(ret));
            return CK_INVALID_HANDLE;
        }

        Application::debug(DebugType::Pkcs11, "%s: session: %" PRIu64, __FUNCTION__, sid);
        sessions.push_front(sid);
        return sid;
    }

    bool PKCS11::Library::waitSlotEvent(bool async, SlotId & res) const
    {
        return CKR_OK == pFunctionList->C_WaitForSlotEvent(async ? CKF_DONT_BLOCK : 0, & res, nullptr);
    }

    // Slot
    PKCS11::Slot::Slot(const SlotId & val, const LibraryPtr & lib) : weak(lib), id(val)
    {
        if(! lib)
        {
            Application::error("%s: lib failed", __FUNCTION__);
            throw pkcs11_error(NS_FuncName);
        }
    }

    bool PKCS11::Slot::getSlotInfo(SlotInfo & info) const
    {
        Application::debug(DebugType::Pkcs11, "%s: slot: %" PRIu64, __FUNCTION__, id);
        auto lib = weak.lock();

        if(! lib)
        {
            return false;
        }

        auto ret = lib->func()->C_GetSlotInfo(id, & info);

        if(ret == CKR_OK)
        {
            return true;
        }

        Application::error("%s: %s failed, slot: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                           __FUNCTION__, "C_GetSlotInfo", id, ret, rvString(ret));
        return false;
    }

    PKCS11::SlotInfoPtr PKCS11::Slot::getSlotInfo(void) const
    {
        auto info = std::make_unique<SlotInfo>();

        if(getSlotInfo(*info))
        {
            return info;
        }

        return nullptr;
    }

    bool PKCS11::Slot::getTokenInfo(TokenInfo & info) const
    {
        Application::debug(DebugType::Pkcs11, "%s: slot: %" PRIu64, __FUNCTION__, id);
        auto lib = weak.lock();

        if(! lib)
        {
            return false;
        }

        auto ret = lib->func()->C_GetTokenInfo(id, & info);

        if(ret == CKR_OK)
        {
            return true;
        }

        Application::error("%s: %s failed, slot: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                           __FUNCTION__, "C_GetTokenInfo", id, ret, rvString(ret));
        return false;
    }

    PKCS11::TokenInfoPtr PKCS11::Slot::getTokenInfo(void) const
    {
        auto info = std::make_unique<TokenInfo>();

        if(getTokenInfo(*info))
        {
            return info;
        }

        return nullptr;
    }

    PKCS11::MechList PKCS11::Slot::getMechanisms(void) const
    {
        Application::debug(DebugType::Pkcs11, "%s: slot: %" PRIu64, __FUNCTION__, id);
        auto lib = weak.lock();

        if(! lib)
            return {};

        CK_ULONG pulCount = 0;

        if(auto ret = lib->func()->C_GetMechanismList(id, nullptr, & pulCount); ret != CKR_OK)
        {
            Application::error("%s: %s failed, code: 0x%" PRIx64 ", rv: `%s'",
                               __FUNCTION__, "C_GetMechanismList", ret, rvString(ret));
            return {};
        }

        if(0 == pulCount)
        {
            Application::debug(DebugType::Pkcs11, "%s: empty %s", __FUNCTION__, "mechanisms");
            return {};
        }

        MechList mechs(pulCount);

        if(auto ret = lib->func()->C_GetMechanismList(id, mechs.data(), & pulCount); ret != CKR_OK)
        {
            Application::error("%s: %s failed, code: 0x%" PRIx64 ", rv: `%s'",
                               __FUNCTION__, "C_GetMechanismList", ret, rvString(ret));
            return {};
        }

        return mechs;
    }

    PKCS11::MechInfoPtr PKCS11::Slot::getMechInfo(const MechType & mech) const
    {
        Application::debug(DebugType::Pkcs11, "%s: slot: %" PRIu64 ", mech: %s", __FUNCTION__, id, mechString(mech));
        auto lib = weak.lock();

        if(! lib)
        {
            return nullptr;
        }

        auto info = std::make_unique<MechInfo>();
        auto ret = lib->func()->C_GetMechanismInfo(id, mech, info.get());

        if(ret == CKR_OK)
        {
            return info;
        }

        Application::error("%s: %s failed, mech: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                           __FUNCTION__, "C_GetMechanismInfo", mech, ret, rvString(ret));
        return nullptr;
    }

    // RawDataRef
    std::string PKCS11::RawDataRef::toString(void) const
    {
        if(size())
        {
            auto beg = data();
            auto end = std::find(beg, beg + size(), 0);
            return std::string(beg, end);
        }

        return "";
    }

    std::string PKCS11::RawDataRef::toHexString(std::string_view sep, bool pref) const
    {
        return Tools::buffer2hexstring(data(), data() + size(), 2, sep, pref);
    }

    bool PKCS11::RawDataRef::operator== (const RawDataRef & raw) const
    {
        if(size() != raw.size())
        {
            return false;
        }

        return std::equal(data(), data() + size(), raw.data());
    }

    // Date
    PKCS11::Date::Date(const RawDataRef & ref)
    {
        if(ref.data() && ref.size() == 8)
        {
            try
            {
                year = std::stoi(std::string(ref.data(), ref.data() + 4));
                month = std::stoi(std::string(ref.data() + 4, ref.data() + 6));
                day = std::stoi(std::string(ref.data() + 6, ref.data() + 8));
            }
            catch(const std::invalid_argument &)
            {
                Application::error("%s: invalid value `%.*s`", __FUNCTION__, 8, (const char*) ref.data());
            }
        }
        else
        {
            Application::error("%s: invalid size: %lu", __FUNCTION__, ref.size());
        }
    }

    std::string PKCS11::Date::toString(const std::string & format) const
    {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        auto in_time = std::mktime(& tm);
        std::stringstream ss;
        ss << std::put_time(localtime_r(&in_time, &tm), format.c_str());
        return ss.str();
    }

    // Session
    PKCS11::Session::Session(const SlotId & id, bool rwmode, const LibraryPtr & lib) : Slot(id, lib)
    {
        sid = lib->sessionOpen(id, rwmode);

        if(sid == CK_INVALID_HANDLE)
        {
            throw pkcs11_error(NS_FuncName);
        }
    }

    PKCS11::Session::~Session()
    {
        logout();

        if(auto lib = weak.lock())
        {
            lib->sessionClose(sid);
        }
    }

    PKCS11::SessionInfoPtr PKCS11::Session::getInfo(void) const
    {
        Application::debug(DebugType::Pkcs11, "%s: session: %" PRIu64, __FUNCTION__, sid);

        if(auto lib = weak.lock())
        {
            auto info = std::make_unique<SessionInfo>();
            auto ret = lib->func()->C_GetSessionInfo(sid, info.get());

            if(ret == CKR_OK)
            {
                return info;
            }

            Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                               __FUNCTION__, "C_GetSessionInfo", sid, ret, rvString(ret));
        }

        return nullptr;
    }

    PKCS11::RawData PKCS11::Session::generateRandom(size_t len) const
    {
        Application::debug(DebugType::Pkcs11, "%s: session: %" PRIu64, __FUNCTION__, sid);

        if(auto lib = weak.lock())
        {
            RawData res;
            res.reserve(len);
            std::array<CK_BYTE, 96> tmp;

            while(res.size() < len)
            {
                auto ret = lib->func()->C_GenerateRandom(sid, tmp.data(), tmp.size());

                if(ret != CKR_OK)
                {
                    Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                       __FUNCTION__, "C_GenerateRandom", sid, ret, rvString(ret));
                    return {};
                }

                res.insert(res.end(), tmp.begin(), tmp.end());
            }

            res.resize(len);
            return res;
        }

        return {};
    }

    bool PKCS11::Session::login(std::string_view pin, bool admin)
    {
        auto info = getTokenInfo();

        if(! info->flagLoginRequired())
        {
            Application::debug(DebugType::Pkcs11, "%s: not login required, session: %" PRIu64, __FUNCTION__, sid);
            return true;
        }

        if(auto lib = weak.lock())
        {
            auto ret = lib->func()->C_Login(sid, admin ? CKU_SO : CKU_USER, (CK_UTF8CHAR_PTR) pin.data(), pin.size());

            if(ret == CKR_OK)
            {
                islogged = true;
                return true;
            }

            Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                               __FUNCTION__, "C_Login", sid, ret, rvString(ret));
        }

        return false;
    }

    void PKCS11::Session::logout(void)
    {
        if(! islogged)
        {
            return;
        }

        if(auto lib = weak.lock())
        {
            auto ret = lib->func()->C_Logout(sid);

            if(ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Logout", sid, ret, rvString(ret));
            }
        }

        islogged = false;
    }

    PKCS11::ObjectList PKCS11::Session::findTokenObjects(size_t maxObjects, const CK_ATTRIBUTE* attrs, size_t counts) const
    {
        if(auto lib = weak.lock())
        {
            if(auto ret = lib->func()->C_FindObjectsInit(sid, const_cast<CK_ATTRIBUTE*>(attrs), counts); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_FindObjectsInit", sid, ret, rvString(ret));
                return {};
            }

            ObjectList res(maxObjects, CK_INVALID_HANDLE);
            CK_ULONG objectCount = 0;

            if(auto ret = lib->func()->C_FindObjects(sid, res.data(), res.size(), & objectCount); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_FindObjects", sid, ret, rvString(ret));
            }

            Application::debug(DebugType::Pkcs11, "%s: objects count: %" PRIu64, __FUNCTION__, objectCount);

            if(auto ret = lib->func()->C_FindObjectsFinal(sid); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_FindObjectsFinal", sid, ret, rvString(ret));
            }

            res.resize(objectCount);
            return res;
        }

        return {};
    }

    PKCS11::ObjectList PKCS11::Session::findTokenObjects(const ObjectClass & objectClass, size_t maxObjects /* 32 */) const
    {
        CK_BBOOL tokenStorage = CK_TRUE;
        CK_ATTRIBUTE attrs[] =
        {
            { CKA_CLASS, (void*) & objectClass, sizeof(objectClass) },
            { CKA_TOKEN, & tokenStorage, sizeof(tokenStorage) },
        };
        auto attrsCount = sizeof(attrs) / sizeof(CK_ATTRIBUTE);
        return findTokenObjects(maxObjects, attrs, attrsCount);
    }

    PKCS11::ObjectList PKCS11::Session::getCertificates(bool havePulicPrivateKeys) const
    {
        auto certs = findTokenObjects(CKO_CERTIFICATE);

        if(havePulicPrivateKeys)
        {
            auto publicKeys = getPublicKeys();
            auto itend = std::remove_if(certs.begin(), certs.end(), [&](auto & certId)
            {
                auto certInfo = getObjectInfo(certId);

                if(0 == findPublicKey(certInfo.getId()))
                {
                    return true;
                }

                if(islogged)
                {
                    if(0 == findPrivateKey(certInfo.getId()))
                    {
                        return true;
                    }
                }

                return false;
            });

            certs.erase(itend, certs.end());
        }

        return certs;
    }

    PKCS11::ObjectInfo PKCS11::Session::getObjectInfo(const ObjectHandle & handle,
            std::initializer_list<CK_ATTRIBUTE_TYPE> types) const
    {
        auto & defaults = ObjectInfo::types;
        ObjectInfo info;
        info.attrs.reserve(defaults.size() + types.size());

        for(const auto & type : defaults)
            info.attrs.emplace_back(CK_ATTRIBUTE{ type, nullptr, 0 });

        for(const auto & type : types)
            info.attrs.emplace_back(CK_ATTRIBUTE{ type, nullptr, 0 });

        // get size
        if(! getAttributes(handle, info.attrs.data(), info.attrs.size()))
            return {};

        size_t buflen = std::accumulate(info.attrs.begin(), info.attrs.end(), 0, [](size_t sum, auto & attr)
        {
            return sum += attr.ulValueLen;
        });
        info.ptr = std::make_unique<uint8_t[]>(buflen);
        size_t offset = 0;

        for(auto & attr : info.attrs)
        {
            attr.pValue = info.ptr.get() + offset;
            offset += attr.ulValueLen;
        }

        // get values
        if(! getAttributes(handle, info.attrs.data(), info.attrs.size()))
            return {};

        info.handle = handle;

        return info;
    }

    PKCS11::CertificateInfo PKCS11::Session::getCertificateInfo(const ObjectHandle & handle) const
    {
        return getObjectInfo(handle, CertificateInfo::types);
    }

    PKCS11::PublicKeyInfo PKCS11::Session::getPublicKeyInfo(const ObjectHandle & handle) const
    {
        return getObjectInfo(handle, PublicKeyInfo::types);
    }

    PKCS11::PrivateKeyInfo PKCS11::Session::getPrivateKeyInfo(const ObjectHandle & handle) const
    {
        return getObjectInfo(handle, PrivateKeyInfo::types);
    }

    PKCS11::RawDataRef PKCS11::ObjectInfo::getRawData(const CK_ATTRIBUTE_TYPE & type) const
    {
        auto it = std::find_if(attrs.begin(), attrs.end(), [&](auto & attr)
        {
            return attr.type == type;
        });

        if(it == attrs.end())
            return {};

        return { (const uint8_t*) it->pValue, it->ulValueLen };
    }

    std::string PKCS11::ObjectInfo::getLabel(void) const
    {
        auto ref = getRawData(CKA_LABEL);

        if(ref.data() && ref.size())
        {
            return std::string(ref.data(), ref.data() + ref.size());
        }

        return "";
    }

    bool PKCS11::ObjectInfo::getBool(const CK_ATTRIBUTE_TYPE & type) const
    {
        auto it = std::find_if(attrs.begin(), attrs.end(), [&](auto & attr)
        {
            return attr.type == type;
        });

        if(it == attrs.end())
        {
            return false;
        }

        if(sizeof(CK_BBOOL) != it->ulValueLen)
        {
            Application::error("%s: invalid bool, type: 0x%" PRIx64, __FUNCTION__, type);
            return false;
        }

        return *static_cast<CK_BBOOL*>(it->pValue);
    }

    bool PKCS11::Session::getAttributes(const ObjectHandle & handle, const CK_ATTRIBUTE* attribs, size_t counts) const
    {
        if(auto lib = weak.lock())
        {
            if(auto ret = lib->func()->C_GetAttributeValue(sid, handle, const_cast<CK_ATTRIBUTE*>(attribs), counts); ret != CKR_OK)
            {
                Application::error("%s: %s failed, code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_GetAttributeValue", ret, rvString(ret));
                return false;
            }

            return true;
        }

        return false;
    }

    ssize_t PKCS11::Session::getAttribLength(const ObjectHandle & handle, const CK_ATTRIBUTE_TYPE & attrType) const
    {
        if(auto lib = weak.lock())
        {
            CK_ATTRIBUTE attribs[] =
            {
                { attrType, nullptr, 0 }
            };
            const CK_ULONG countAttribs = sizeof(attribs) / sizeof(CK_ATTRIBUTE);

            if(auto ret = lib->func()->C_GetAttributeValue(sid, handle, attribs, countAttribs); ret != CKR_OK)
            {
                Application::error("%s: %s failed, type: 0x%" PRIx64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_GetAttributeValue", attrType, ret, rvString(ret));
                return -1;
            }

            return attribs[0].ulValueLen;
        }

        return -1;
    }

    PKCS11::RawData PKCS11::Session::getAttribData(const ObjectHandle & handle, const CK_ATTRIBUTE_TYPE & attrType) const
    {
        const ssize_t len = getAttribLength(handle, attrType);

        if(len < 0)
            return {};

        if(auto lib = weak.lock())
        {
            RawData res(len);
            CK_ATTRIBUTE attribs[] =
            {
                { attrType, res.data(), res.size() }
            };
            const CK_ULONG countAttribs = sizeof(attribs) / sizeof(CK_ATTRIBUTE);

            if(auto ret = lib->func()->C_GetAttributeValue(sid, handle, attribs, countAttribs); ret != CKR_OK)
            {
                Application::error("%s: %s failed, type: 0x%" PRIx64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_GetAttributeValue", attrType, ret, rvString(ret));
                return {};
            }

            return res;
        }

        return {};
    }

    PKCS11::RawData PKCS11::Session::digestData(const void* ptr, size_t len, const MechType & type) const
    {
        Application::debug(DebugType::Pkcs11, "%s: session: %" PRIu64 ", mech: %s", __FUNCTION__, sid, mechString(type));

        if(! ptr || len == 0)
        {
            Application::warning("%s: data empty", __FUNCTION__);
            return {};
        }

        if(auto lib = weak.lock())
        {
            CK_MECHANISM mech = { type, nullptr, 0 };

            if(auto ret = lib->func()->C_DigestInit(sid, & mech); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_DigestInit", sid, ret, rvString(ret));
                return {};
            }

            CK_ULONG hashLen = 0;

            if(auto ret = lib->func()->C_Digest(sid, (CK_BYTE_PTR) ptr, len, nullptr, & hashLen); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Digest", sid, ret, rvString(ret));
                return {};
            }

            RawData hash(hashLen);

            if(auto ret = lib->func()->C_Digest(sid, (CK_BYTE_PTR) ptr, len, hash.data(), & hashLen); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Digest", sid, ret, rvString(ret));
                return {};
            }

            return hash;
        }

        return {};
    }

    PKCS11::RawData PKCS11::Session::digestMD5(const void* ptr, size_t len) const
    {
        return digestData(ptr, len, CKM_MD5);
    }

    PKCS11::RawData PKCS11::Session::digestSHA1(const void* ptr, size_t len) const
    {
        return digestData(ptr, len, CKM_SHA_1);
    }

    PKCS11::RawData PKCS11::Session::digestSHA256(const void* ptr, size_t len) const
    {
        return digestData(ptr, len, CKM_SHA256);
    }

    PKCS11::ObjectHandle PKCS11::Session::findPublicKey(const ObjectIdRef & objId) const
    {
        CK_BBOOL tokenStorage = CK_TRUE;
        CK_OBJECT_CLASS classPublicKey = CKO_PUBLIC_KEY;
        CK_ATTRIBUTE attrs[] =
        {
            { CKA_CLASS, (void*) & classPublicKey, sizeof(classPublicKey) },
            { CKA_TOKEN, & tokenStorage, sizeof(tokenStorage) },
            { CKA_ID, (void*) objId.data(), objId.size() }
        };
        auto attrsCount = sizeof(attrs) / sizeof(CK_ATTRIBUTE);
        auto publicKeys = findTokenObjects(1, attrs, attrsCount);
        return publicKeys.empty() ? 0 : publicKeys.front();
    }

    PKCS11::ObjectHandle PKCS11::Session::findPrivateKey(const ObjectIdRef & objId) const
    {
        CK_BBOOL tokenStorage = CK_TRUE;
        CK_OBJECT_CLASS classPublicKey = CKO_PRIVATE_KEY;
        CK_ATTRIBUTE attrs[] =
        {
            { CKA_CLASS, (void*) & classPublicKey, sizeof(classPublicKey) },
            { CKA_TOKEN, & tokenStorage, sizeof(tokenStorage) },
            { CKA_ID, (void*) objId.data(), objId.size() }
        };
        auto attrsCount = sizeof(attrs) / sizeof(CK_ATTRIBUTE);
        auto publicKeys = findTokenObjects(1, attrs, attrsCount);
        return publicKeys.empty() ? 0 : publicKeys.front();
    }

    PKCS11::RawData PKCS11::Session::signData(const RawDataRef & certId, const void* data, size_t length,
            const MechType & mechType) const
    {
        if(! islogged)
        {
            Application::error("%s: not logged session", __FUNCTION__);
            return {};
        }

        auto mechInfo = getMechInfo(mechType);

        if(! mechType)
        {
            Application::error("%s: unknown mech type: 0x%" PRIx64, __FUNCTION__, mechType);
            return {};
        }

        CK_MECHANISM mech = { mechType, nullptr, 0 };
        CK_OBJECT_HANDLE privateHandle = findPrivateKey(certId);

        if(! privateHandle)
        {
            Application::error("%s: %s not found, id: `%s'", __FUNCTION__, "private key", certId.toHexString().c_str());
            return {};
        }

        if(auto lib = weak.lock())
        {
            if(auto ret = lib->func()->C_SignInit(sid, & mech, privateHandle); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_SignInit", sid, ret, rvString(ret));
                return {};
            }

            std::vector<uint8_t> buf;
            CK_ULONG bufLength = 0;

            // get result size
            if(auto ret = lib->func()->C_Sign(sid, (unsigned char*) data, length, nullptr, & bufLength); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Sign", sid, ret, rvString(ret));
                return {};
            }

            buf.resize(bufLength);

            if(auto ret = lib->func()->C_Sign(sid, (unsigned char*) data, length, buf.data(), & bufLength); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Sign", sid, ret, rvString(ret));
                return {};
            }

            if(bufLength < buf.size())
            {
                buf.resize(bufLength);
            }

            return buf;
        }

        return {};
    }

    /*
        bool PKCS11::Session::verifyData(const RawDataRef & certId, const void* data, size_t length, const MechType & mechType) const
        {
            auto mechInfo = getMechInfo(mechType);
            if(! mechType)
            {
                Application::error("%s: unknown mech type: 0x%" PRIx64, __FUNCTION__, mechType);
                return false;
            }

            CK_MECHANISM mech = { mechType, nullptr, 0 };
            CK_OBJECT_HANDLE publicHandle = findPublicKey(certId);

            if(! publicHandle)
            {
                Application::error("%s: %s not found, id: `%s'", __FUNCTION__, "public key", certId.toHexString().c_str());
                return false;
            }

            if(auto lib = weak.lock())
            {
                if(auto ret = lib->func()->C_VerifyInit(sid, & mech, publicHandle); ret != CKR_OK)
                {
                    Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                        __FUNCTION__, "C_VerifyInit", sid, ret, rvString(ret));
                    return false;
                }

                if(auto ret = lib->func()->C_Verify(sid, (unsigned char*) data, length, buf.data(), & bufLength); ret != CKR_OK)
                {
                    Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                        __FUNCTION__, "C_Verify", sid, ret, rvString(ret));
                    return false;
                }

                return true;
            }

            return false;
        }
    */
    PKCS11::RawData PKCS11::Session::encryptData(const RawDataRef & certId, const void* data, size_t length,
            const MechType & mechType) const
    {
        auto mechInfo = getMechInfo(mechType);

        if(! mechType)
        {
            Application::error("%s: unknown mech type: 0x%" PRIx64, __FUNCTION__, mechType);
            return {};
        }

        CK_MECHANISM mech = { mechType, nullptr, 0 };
        CK_OBJECT_HANDLE publicHandle = findPublicKey(certId);

        if(! publicHandle)
        {
            Application::error("%s: %s not found, id: `%s'", __FUNCTION__, "public key", certId.toHexString().c_str());
            return {};
        }

        if(auto lib = weak.lock())
        {
            if(auto ret = lib->func()->C_EncryptInit(sid, & mech, publicHandle); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_EncryptInit", sid, ret, rvString(ret));
                return {};
            }

            std::vector<uint8_t> buf;
            CK_ULONG bufLength = 0;

            // get result size
            if(auto ret = lib->func()->C_Encrypt(sid, (unsigned char*) data, length, nullptr, & bufLength); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Encrypt", sid, ret, rvString(ret));
                return {};
            }

            buf.resize(bufLength);

            if(auto ret = lib->func()->C_Encrypt(sid, (unsigned char*) data, length, buf.data(), & bufLength); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Encrypt", sid, ret, rvString(ret));
                return {};
            }

            if(bufLength < buf.size())
            {
                buf.resize(bufLength);
            }

            return buf;
        }

        return {};
    }

    PKCS11::RawData PKCS11::Session::decryptData(const RawDataRef & certId, const void* data, size_t length,
            const MechType & mechType) const
    {
        if(! islogged)
        {
            Application::error("%s: not logged session", __FUNCTION__);
            return {};
        }

        auto mechInfo = getMechInfo(mechType);

        if(! mechType)
        {
            Application::error("%s: unknown mech type: 0x%" PRIx64, __FUNCTION__, mechType);
            return {};
        }

        CK_MECHANISM mech = { mechType, nullptr, 0 };
        CK_OBJECT_HANDLE privateHandle = findPrivateKey(certId);

        if(! privateHandle)
        {
            Application::error("%s: %s not found, id: `%s'", __FUNCTION__, "private key", certId.toHexString().c_str());
            return {};
        }

        if(auto lib = weak.lock())
        {
            if(auto ret = lib->func()->C_DecryptInit(sid, & mech, privateHandle); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_DecryptInit", sid, ret, rvString(ret));
                return {};
            }

            std::vector<uint8_t> buf;
            CK_ULONG bufLength = 0;

            // get result size
            if(auto ret = lib->func()->C_Decrypt(sid, (unsigned char*) data, length, nullptr, & bufLength); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Decrypt", sid, ret, rvString(ret));
                return {};
            }

            buf.resize(bufLength);

            if(auto ret = lib->func()->C_Decrypt(sid, (unsigned char*) data, length, buf.data(), & bufLength); ret != CKR_OK)
            {
                Application::error("%s: %s failed, session: %" PRIu64 ", code: 0x%" PRIx64 ", rv: `%s'",
                                   __FUNCTION__, "C_Decrypt", sid, ret, rvString(ret));
                return {};
            }

            if(bufLength < buf.size())
            {
                buf.resize(bufLength);
            }

            return buf;
        }

        return {};
    }

    // pkcs11 API
    PKCS11::SessionPtr PKCS11::createSession(const SlotId & id, bool rwmode, const LibraryPtr & lib)
    {
        try
        {
            return std::make_unique<Session>(id, rwmode, lib);
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

        return nullptr;
    }

    PKCS11::LibraryPtr PKCS11::loadLibrary(const std::string & name)
    {
        return std::make_shared<Library>(name);
    }

    const char* PKCS11::mechString(const CK_MECHANISM_TYPE & type)
    {
        switch(type)
        {
            case CKM_RSA_PKCS_KEY_PAIR_GEN:
                return "RSA_PKCS_KEY_PAIR_GEN";

            case CKM_RSA_PKCS:
                return "RSA_PKCS";

            case CKM_RSA_9796:
                return "RSA_9796";

            case CKM_RSA_X_509:
                return "RSA_X_509";

            case CKM_MD2_RSA_PKCS:
                return "MD2_RSA_PKCS";

            case CKM_MD5_RSA_PKCS:
                return "MD5_RSA_PKCS";

            case CKM_SHA1_RSA_PKCS:
                return "SHA1_RSA_PKCS";

            case CKM_RIPEMD128_RSA_PKCS:
                return "RIPEMD128_RSA_PKCS";

            case CKM_RIPEMD160_RSA_PKCS:
                return "RIPEMD160_RSA_PKCS";

            case CKM_RSA_PKCS_OAEP:
                return "RSA_PKCS_OAEP";

            case CKM_RSA_X9_31_KEY_PAIR_GEN:
                return "RSA_X9_31_KEY_PAIR_GEN";

            case CKM_RSA_X9_31:
                return "RSA_X9_31";

            case CKM_SHA1_RSA_X9_31:
                return "SHA1_RSA_X9_31";

            case CKM_RSA_PKCS_PSS:
                return "RSA_PKCS_PSS";

            case CKM_SHA1_RSA_PKCS_PSS:
                return "SHA1_RSA_PKCS_PSS";

            case CKM_DSA_KEY_PAIR_GEN:
                return "DSA_KEY_PAIR_GEN";

            case CKM_DSA:
                return "DSA";

            case CKM_DSA_SHA1:
                return "DSA_SHA1";

#if defined(CKM_DSA_SHA224)
            case CKM_DSA_SHA224:
                return "DSA_SHA224";
#endif
#if defined(CKM_DSA_SHA256)
            case CKM_DSA_SHA256:
                return "DSA_SHA256";
#endif
#if defined(CKM_DSA_SHA384)
            case CKM_DSA_SHA384:
                return "DSA_SHA384";
#endif
#if defined(CKM_DSA_SHA512)
            case CKM_DSA_SHA512:
                return "DSA_SHA512";
#endif
            case CKM_DH_PKCS_KEY_PAIR_GEN:
                return "DH_PKCS_KEY_PAIR_GEN";

            case CKM_DH_PKCS_DERIVE:
                return "DH_PKCS_DERIVE";

            case CKM_X9_42_DH_KEY_PAIR_GEN:
                return "X9_42_DH_KEY_PAIR_GEN";

            case CKM_X9_42_DH_DERIVE:
                return "X9_42_DH_DERIVE";

            case CKM_X9_42_DH_HYBRID_DERIVE:
                return "X9_42_DH_HYBRID_DERIVE";

            case CKM_X9_42_MQV_DERIVE:
                return "X9_42_MQV_DERIVE";

            case CKM_SHA256_RSA_PKCS:
                return "SHA256_RSA_PKCS";

            case CKM_SHA384_RSA_PKCS:
                return "SHA384_RSA_PKCS";

            case CKM_SHA512_RSA_PKCS:
                return "SHA512_RSA_PKCS";

            case CKM_SHA256_RSA_PKCS_PSS:
                return "SHA256_RSA_PKCS_PSS";

            case CKM_SHA384_RSA_PKCS_PSS:
                return "SHA384_RSA_PKCS_PSS";

            case CKM_SHA512_RSA_PKCS_PSS:
                return "SHA512_RSA_PKCS_PSS";

#if defined(CKM_SHA512_224)
            case CKM_SHA512_224:
                return "SHA512_224";

            case CKM_SHA512_224_HMAC:
                return "SHA512_224_HMAC";

            case CKM_SHA512_224_HMAC_GENERAL:
                return "SHA512_224_HMAC_GENERAL";

            case CKM_SHA512_224_KEY_DERIVATION:
                return "SHA512_224_KEY_DERIVATION";
#endif
#if defined(CKM_SHA512_256)
            case CKM_SHA512_256:
                return "SHA512_256";

            case CKM_SHA512_256_HMAC:
                return "SHA512_256_HMAC";

            case CKM_SHA512_256_HMAC_GENERAL:
                return "SHA512_256_HMAC_GENERAL";

            case CKM_SHA512_256_KEY_DERIVATION:
                return "SHA512_256_KEY_DERIVATION";
#endif
#if defined(CKM_SHA512_T)
            case CKM_SHA512_T:
                return "SHA512_T";

            case CKM_SHA512_T_HMAC:
                return "SHA512_T_HMAC";

            case CKM_SHA512_T_HMAC_GENERAL:
                return "SHA512_T_HMAC_GENERAL";

            case CKM_SHA512_T_KEY_DERIVATION:
                return "SHA512_T_KEY_DERIVATION";
#endif
            case CKM_RC2_KEY_GEN:
                return "RC2_KEY_GEN";

            case CKM_RC2_ECB:
                return "RC2_ECB";

            case CKM_RC2_CBC:
                return "RC2_CBC";

            case CKM_RC2_MAC:
                return "RC2_MAC";

            case CKM_RC2_MAC_GENERAL:
                return "RC2_MAC_GENERAL";

            case CKM_RC2_CBC_PAD:
                return "RC2_CBC_PAD";

            case CKM_RC4_KEY_GEN:
                return "RC4_KEY_GEN";

            case CKM_RC4:
                return "RC4";

            case CKM_DES_KEY_GEN:
                return "DES_KEY_GEN";

            case CKM_DES_ECB:
                return "DES_ECB";

            case CKM_DES_CBC:
                return "DES_CBC";

            case CKM_DES_MAC:
                return "DES_MAC";

            case CKM_DES_MAC_GENERAL:
                return "DES_MAC_GENERAL";

            case CKM_DES_CBC_PAD:
                return "DES_CBC_PAD";

            case CKM_DES2_KEY_GEN:
                return "DES2_KEY_GEN";

#if defined(CKM_DES3_KEY_GEN)
            case CKM_DES3_KEY_GEN:
                return "DES3_KEY_GEN";

            case CKM_DES3_ECB:
                return "DES3_ECB";

            case CKM_DES3_CBC:
                return "DES3_CBC";

            case CKM_DES3_MAC:
                return "DES3_MAC";

            case CKM_DES3_MAC_GENERAL:
                return "DES3_MAC_GENERAL";

            case CKM_DES3_CBC_PAD:
                return "DES3_CBC_PAD";
#endif
#if defined(CKM_DES3_CMAC)
            case CKM_DES3_CMAC_GENERAL:
                return "DES3_CMAC_GENERAL";

            case CKM_DES3_CMAC:
                return "DES3_CMAC";
#endif
            case CKM_CDMF_KEY_GEN:
                return "CDMF_KEY_GEN";

            case CKM_CDMF_ECB:
                return "CDMF_ECB";

            case CKM_CDMF_CBC:
                return "CDMF_CBC";

            case CKM_CDMF_MAC:
                return "CDMF_MAC";

            case CKM_CDMF_MAC_GENERAL:
                return "CDMF_MAC_GENERAL";

            case CKM_CDMF_CBC_PAD:
                return "CDMF_CBC_PAD";

            case CKM_DES_OFB64:
                return "DES_OFB64";

            case CKM_DES_OFB8:
                return "DES_OFB8";

            case CKM_DES_CFB64:
                return "DES_CFB64";

            case CKM_DES_CFB8:
                return "DES_CFB8";

            case CKM_MD2:
                return "MD2";

            case CKM_MD2_HMAC:
                return "MD2_HMAC";

            case CKM_MD2_HMAC_GENERAL:
                return "MD2_HMAC_GENERAL";

            case CKM_MD5:
                return "MD5";

            case CKM_MD5_HMAC:
                return "MD5_HMAC";

            case CKM_MD5_HMAC_GENERAL:
                return "MD5_HMAC_GENERAL";

            case CKM_SHA_1:
                return "SHA_1";

            case CKM_SHA_1_HMAC:
                return "SHA_1_HMAC";

            case CKM_SHA_1_HMAC_GENERAL:
                return "SHA_1_HMAC_GENERAL";

            case CKM_RIPEMD128:
                return "RIPEMD128";

            case CKM_RIPEMD128_HMAC:
                return "RIPEMD128_HMAC";

            case CKM_RIPEMD128_HMAC_GENERAL:
                return "RIPEMD128_HMAC_GENERAL";

            case CKM_RIPEMD160:
                return "RIPEMD160";

            case CKM_RIPEMD160_HMAC:
                return "RIPEMD160_HMAC";

            case CKM_RIPEMD160_HMAC_GENERAL:
                return "RIPEMD160_HMAC_GENERAL";

            case CKM_SHA256:
                return "SHA256";

            case CKM_SHA256_HMAC:
                return "SHA256_HMAC";

            case CKM_SHA256_HMAC_GENERAL:
                return "SHA256_HMAC_GENERAL";

            case CKM_SHA384:
                return "SHA384";

            case CKM_SHA384_HMAC:
                return "SHA384_HMAC";

            case CKM_SHA384_HMAC_GENERAL:
                return "SHA384_HMAC_GENERAL";

            case CKM_SHA512:
                return "SHA512";

            case CKM_SHA512_HMAC:
                return "SHA512_HMAC";

            case CKM_SHA512_HMAC_GENERAL:
                return "SHA512_HMAC_GENERAL";

#if defined(CKM_SECURID)
            case CKM_SECURID_KEY_GEN:
                return "SECURID_KEY_GEN";

            case CKM_SECURID:
                return "SECURID";
#endif
#if defined(CKM_HOTP)
            case CKM_HOTP_KEY_GEN:
                return "HOTP_KEY_GEN";

            case CKM_HOTP:
                return "HOTP";
#endif
#if defined(CKM_ACTI)
            case CKM_ACTI:
                return "ACTI";

            case CKM_ACTI_KEY_GEN:
                return "ACTI_KEY_GEN";
#endif
            case CKM_CAST_KEY_GEN:
                return "CAST_KEY_GEN";

            case CKM_CAST_ECB:
                return "CAST_ECB";

            case CKM_CAST_CBC:
                return "CAST_CBC";

            case CKM_CAST_MAC:
                return "CAST_MAC";

            case CKM_CAST_MAC_GENERAL:
                return "CAST_MAC_GENERAL";

            case CKM_CAST_CBC_PAD:
                return "CAST_CBC_PAD";

            case CKM_CAST3_KEY_GEN:
                return "CAST3_KEY_GEN";

            case CKM_CAST3_ECB:
                return "CAST3_ECB";

            case CKM_CAST3_CBC:
                return "CAST3_CBC";

            case CKM_CAST3_MAC:
                return "CAST3_MAC";

            case CKM_CAST3_MAC_GENERAL:
                return "CAST3_MAC_GENERAL";

            case CKM_CAST3_CBC_PAD:
                return "CAST3_CBC_PAD";

            //case CKM_CAST5_KEY_GEN: return "CAST5_KEY_GEN";
            case CKM_CAST128_KEY_GEN:
                return "CAST128_KEY_GEN";

            //case CKM_CAST5_ECB: return "CAST5_ECB";
            case CKM_CAST128_ECB:
                return "CAST128_ECB";

            //case CKM_CAST5_CBC: return "CAST5_CBC";
            case CKM_CAST128_CBC:
                return "CAST128_CBC";

            //case CKM_CAST5_MAC: return "CAST5_MAC";
            case CKM_CAST128_MAC:
                return "CAST128_MAC";

            //case CKM_CAST5_MAC_GENERAL: return "CAST5_MAC_GENERAL";
            case CKM_CAST128_MAC_GENERAL:
                return "CAST128_MAC_GENERAL";

            //case CKM_CAST5_CBC_PAD: return "CAST5_CBC_PAD";
            case CKM_CAST128_CBC_PAD:
                return "CAST128_CBC_PAD";

            case CKM_RC5_KEY_GEN:
                return "RC5_KEY_GEN";

            case CKM_RC5_ECB:
                return "RC5_ECB";

            case CKM_RC5_CBC:
                return "RC5_CBC";

            case CKM_RC5_MAC:
                return "RC5_MAC";

            case CKM_RC5_MAC_GENERAL:
                return "RC5_MAC_GENERAL";

            case CKM_RC5_CBC_PAD:
                return "RC5_CBC_PAD";

            case CKM_IDEA_KEY_GEN:
                return "IDEA_KEY_GEN";

            case CKM_IDEA_ECB:
                return "IDEA_ECB";

            case CKM_IDEA_CBC:
                return "IDEA_CBC";

            case CKM_IDEA_MAC:
                return "IDEA_MAC";

            case CKM_IDEA_MAC_GENERAL:
                return "IDEA_MAC_GENERAL";

            case CKM_IDEA_CBC_PAD:
                return "IDEA_CBC_PAD";

            case CKM_GENERIC_SECRET_KEY_GEN:
                return "GENERIC_SECRET_KEY_GEN";

            case CKM_CONCATENATE_BASE_AND_KEY:
                return "CONCATENATE_BASE_AND_KEY";

            case CKM_CONCATENATE_BASE_AND_DATA:
                return "CONCATENATE_BASE_AND_DATA";

            case CKM_CONCATENATE_DATA_AND_BASE:
                return "CONCATENATE_DATA_AND_BASE";

            case CKM_XOR_BASE_AND_DATA:
                return "XOR_BASE_AND_DATA";

            case CKM_EXTRACT_KEY_FROM_KEY:
                return "EXTRACT_KEY_FROM_KEY";

            case CKM_SSL3_PRE_MASTER_KEY_GEN:
                return "SSL3_PRE_MASTER_KEY_GEN";

            case CKM_SSL3_MASTER_KEY_DERIVE:
                return "SSL3_MASTER_KEY_DERIVE";

            case CKM_SSL3_KEY_AND_MAC_DERIVE:
                return "SSL3_KEY_AND_MAC_DERIVE";

            case CKM_SSL3_MASTER_KEY_DERIVE_DH:
                return "SSL3_MASTER_KEY_DERIVE_DH";

            case CKM_TLS_PRE_MASTER_KEY_GEN:
                return "TLS_PRE_MASTER_KEY_GEN";

            case CKM_TLS_MASTER_KEY_DERIVE:
                return "TLS_MASTER_KEY_DERIVE";

            case CKM_TLS_KEY_AND_MAC_DERIVE:
                return "TLS_KEY_AND_MAC_DERIVE";

            case CKM_TLS_MASTER_KEY_DERIVE_DH:
                return "TLS_MASTER_KEY_DERIVE_DH";

            case CKM_TLS_PRF:
                return "TLS_PRF";

            case CKM_SSL3_MD5_MAC:
                return "SSL3_MD5_MAC";

            case CKM_SSL3_SHA1_MAC:
                return "SSL3_SHA1_MAC";

            case CKM_MD5_KEY_DERIVATION:
                return "MD5_KEY_DERIVATION";

            case CKM_MD2_KEY_DERIVATION:
                return "MD2_KEY_DERIVATION";

            case CKM_SHA1_KEY_DERIVATION:
                return "SHA1_KEY_DERIVATION";

            case CKM_SHA256_KEY_DERIVATION:
                return "SHA256_KEY_DERIVATION";

            case CKM_SHA384_KEY_DERIVATION:
                return "SHA384_KEY_DERIVATION";

            case CKM_SHA512_KEY_DERIVATION:
                return "SHA512_KEY_DERIVATION";

            case CKM_PBE_MD2_DES_CBC:
                return "PBE_MD2_DES_CBC";

            case CKM_PBE_MD5_DES_CBC:
                return "PBE_MD5_DES_CBC";

            case CKM_PBE_MD5_CAST_CBC:
                return "PBE_MD5_CAST_CBC";

            case CKM_PBE_MD5_CAST3_CBC:
                return "PBE_MD5_CAST3_CBC";

            //case CKM_PBE_MD5_CAST5_CBC: return "PBE_MD5_CAST5_CBC";
            case CKM_PBE_MD5_CAST128_CBC:
                return "PBE_MD5_CAST128_CBC";

            //case CKM_PBE_SHA1_CAST5_CBC: return "PBE_SHA1_CAST5_CBC";
            case CKM_PBE_SHA1_CAST128_CBC:
                return "PBE_SHA1_CAST128_CBC";

            case CKM_PBE_SHA1_RC4_128:
                return "PBE_SHA1_RC4_128";

            case CKM_PBE_SHA1_RC4_40:
                return "PBE_SHA1_RC4_40";

            case CKM_PBE_SHA1_DES3_EDE_CBC:
                return "PBE_SHA1_DES3_EDE_CBC";

            case CKM_PBE_SHA1_DES2_EDE_CBC:
                return "PBE_SHA1_DES2_EDE_CBC";

            case CKM_PBE_SHA1_RC2_128_CBC:
                return "PBE_SHA1_RC2_128_CBC";

            case CKM_PBE_SHA1_RC2_40_CBC:
                return "PBE_SHA1_RC2_40_CBC";

            case CKM_PKCS5_PBKD2:
                return "PKCS5_PBKD2";

            case CKM_PBA_SHA1_WITH_SHA1_HMAC:
                return "PBA_SHA1_WITH_SHA1_HMAC";

            case CKM_WTLS_PRE_MASTER_KEY_GEN:
                return "WTLS_PRE_MASTER_KEY_GEN";

            case CKM_WTLS_MASTER_KEY_DERIVE:
                return "WTLS_MASTER_KEY_DERIVE";

            case CKM_WTLS_MASTER_KEY_DERIVE_DH_ECC:
                return "WTLS_MASTER_KEY_DERIVE_DH_ECC";

            case CKM_WTLS_PRF:
                return "WTLS_PRF";

            case CKM_WTLS_SERVER_KEY_AND_MAC_DERIVE:
                return "WTLS_SERVER_KEY_AND_MAC_DERIVE";

            case CKM_WTLS_CLIENT_KEY_AND_MAC_DERIVE:
                return "WTLS_CLIENT_KEY_AND_MAC_DERIVE";

#if defined(CKM_TLS10_MAC_SERVER)
            case CKM_TLS10_MAC_SERVER:
                return "TLS10_MAC_SERVER";

            case CKM_TLS10_MAC_CLIENT:
                return "TLS10_MAC_CLIENT";
#endif
#if defined(CKM_TLS12_MAC)
            case CKM_TLS12_MAC:
                return "TLS12_MAC";

            case CKM_TLS12_KDF:
                return "TLS12_KDF";

            case CKM_TLS12_MASTER_KEY_DERIVE:
                return "TLS12_MASTER_KEY_DERIVE";

            case CKM_TLS12_KEY_AND_MAC_DERIVE:
                return "TLS12_KEY_AND_MAC_DERIVE";

            case CKM_TLS12_MASTER_KEY_DERIVE_DH:
                return "TLS12_MASTER_KEY_DERIVE_DH";

            case CKM_TLS12_KEY_SAFE_DERIVE:
                return "TLS12_KEY_SAFE_DERIVE";
#endif
#if defined(CKM_TLS_MAC)
            case CKM_TLS_MAC:
                return "TLS_MAC";
#endif
#if defined(CKM_TLS_KDF)
            case CKM_TLS_KDF:
                return "TLS_KDF";
#endif
            case CKM_KEY_WRAP_LYNKS:
                return "KEY_WRAP_LYNKS";

            case CKM_KEY_WRAP_SET_OAEP:
                return "KEY_WRAP_SET_OAEP";

            case CKM_CMS_SIG:
                return "CMS_SIG";

#if defined(CKM_KIP_MAC)
            case CKM_KIP_DERIVE:
                return "KIP_DERIVE";

            case CKM_KIP_WRAP:
                return "KIP_WRAP";

            case CKM_KIP_MAC:
                return "KIP_MAC";
#endif
#if defined(CKM_ARIA_KEY_GEN)
            case CKM_ARIA_KEY_GEN:
                return "ARIA_KEY_GEN";

            case CKM_ARIA_ECB:
                return "ARIA_ECB";

            case CKM_ARIA_CBC:
                return "ARIA_CBC";

            case CKM_ARIA_MAC:
                return "ARIA_MAC";

            case CKM_ARIA_MAC_GENERAL:
                return "ARIA_MAC_GENERAL";

            case CKM_ARIA_CBC_PAD:
                return "ARIA_CBC_PAD";

            case CKM_ARIA_ECB_ENCRYPT_DATA:
                return "ARIA_ECB_ENCRYPT_DATA";

            case CKM_ARIA_CBC_ENCRYPT_DATA:
                return "ARIA_CBC_ENCRYPT_DATA";
#endif
#if defined(CKM_SEED_KEY_GEN)
            case CKM_SEED_KEY_GEN:
                return "SEED_KEY_GEN";
#endif
#if defined(CKM_SEED_ECB)
            case CKM_SEED_ECB:
                return "SEED_ECB";
#endif
#if defined(CKM_SEED_CBC)
            case CKM_SEED_CBC:
                return "SEED_CBC";
#endif
#if defined(CKM_SEED_MAC)
            case CKM_SEED_MAC:
                return "SEED_MAC";

            case CKM_SEED_MAC_GENERAL:
                return "SEED_MAC_GENERAL";
#endif
#if defined(CKM_SEED_CBC_PAD)
            case CKM_SEED_CBC_PAD:
                return "SEED_CBC_PAD";
#endif
#if defined(CKM_SEED_ECB_ENCRYPT_DATA)
            case CKM_SEED_ECB_ENCRYPT_DATA:
                return "SEED_ECB_ENCRYPT_DATA";
#endif
#if defined(CKM_SEED_CBC_ENCRYPT_DATA)
            case CKM_SEED_CBC_ENCRYPT_DATA:
                return "SEED_CBC_ENCRYPT_DATA";
#endif
            case CKM_SKIPJACK_KEY_GEN:
                return "SKIPJACK_KEY_GEN";

            case CKM_SKIPJACK_ECB64:
                return "SKIPJACK_ECB64";

            case CKM_SKIPJACK_CBC64:
                return "SKIPJACK_CBC64";

            case CKM_SKIPJACK_OFB64:
                return "SKIPJACK_OFB64";

            case CKM_SKIPJACK_CFB64:
                return "SKIPJACK_CFB64";

            case CKM_SKIPJACK_CFB32:
                return "SKIPJACK_CFB32";

            case CKM_SKIPJACK_CFB16:
                return "SKIPJACK_CFB16";

            case CKM_SKIPJACK_CFB8:
                return "SKIPJACK_CFB8";

            case CKM_SKIPJACK_WRAP:
                return "SKIPJACK_WRAP";

            case CKM_SKIPJACK_PRIVATE_WRAP:
                return "SKIPJACK_PRIVATE_WRAP";

            case CKM_SKIPJACK_RELAYX:
                return "SKIPJACK_RELAYX";

            case CKM_KEA_KEY_PAIR_GEN:
                return "KEA_KEY_PAIR_GEN";

            case CKM_KEA_KEY_DERIVE:
                return "KEA_KEY_DERIVE";

            case CKM_FORTEZZA_TIMESTAMP:
                return "FORTEZZA_TIMESTAMP";

            case CKM_BATON_KEY_GEN:
                return "BATON_KEY_GEN";

            case CKM_BATON_ECB128:
                return "BATON_ECB128";

            case CKM_BATON_ECB96:
                return "BATON_ECB96";

            case CKM_BATON_CBC128:
                return "BATON_CBC128";

            case CKM_BATON_COUNTER:
                return "BATON_COUNTER";

            case CKM_BATON_SHUFFLE:
                return "BATON_SHUFFLE";

            case CKM_BATON_WRAP:
                return "BATON_WRAP";

            case CKM_ECDSA_KEY_PAIR_GEN:
                return "ECDSA_KEY_PAIR_GEN";

            //case CKM_EC_KEY_PAIR_GEN: return "EC_KEY_PAIR_GEN";
            case CKM_ECDSA:
                return "ECDSA";

            case CKM_ECDSA_SHA1:
                return "ECDSA_SHA1";

#if defined(CKM_ECDSA_SHA224)
            case CKM_ECDSA_SHA224:
                return "ECDSA_SHA224";
#endif

#if defined(CKM_ECDSA_SHA256)
            case CKM_ECDSA_SHA256:
                return "ECDSA_SHA256";
#endif

#if defined(CKM_ECDSA_SHA384)
            case CKM_ECDSA_SHA384:
                return "ECDSA_SHA384";
#endif

#if defined(CKM_ECDSA_SHA512)
            case CKM_ECDSA_SHA512:
                return "ECDSA_SHA512";
#endif
            case CKM_ECDH1_DERIVE:
                return "ECDH1_DERIVE";

            case CKM_ECDH1_COFACTOR_DERIVE:
                return "ECDH1_COFACTOR_DERIVE";

            case CKM_ECMQV_DERIVE:
                return "ECMQV_DERIVE";

#if defined(CKM_ECDH_AES_KEY_WRAP)
            case CKM_ECDH_AES_KEY_WRAP:
                return "ECDH_AES_KEY_WRAP";
#endif
#if defined(CKM_RSA_AES_KEY_WRAP)
            case CKM_RSA_AES_KEY_WRAP:
                return "RSA_AES_KEY_WRAP";
#endif
            case CKM_JUNIPER_KEY_GEN:
                return "JUNIPER_KEY_GEN";

            case CKM_JUNIPER_ECB128:
                return "JUNIPER_ECB128";

            case CKM_JUNIPER_CBC128:
                return "JUNIPER_CBC128";

            case CKM_JUNIPER_COUNTER:
                return "JUNIPER_COUNTER";

            case CKM_JUNIPER_SHUFFLE:
                return "JUNIPER_SHUFFLE";

            case CKM_JUNIPER_WRAP:
                return "JUNIPER_WRAP";

            case CKM_FASTHASH:
                return "FASTHASH";

            case CKM_AES_KEY_GEN:
                return "AES_KEY_GEN";

            case CKM_AES_ECB:
                return "AES_ECB";

#if defined(CKM_AES_CBC)
            case CKM_AES_CBC:
                return "AES_CBC";

            case CKM_AES_CBC_PAD:
                return "AES_CBC_PAD";
#endif
            case CKM_AES_MAC:
                return "AES_MAC";

            case CKM_AES_MAC_GENERAL:
                return "AES_MAC_GENERAL";

#if defined(CKM_AES_CTR)
            case CKM_AES_CTR:
                return "AES_CTR";
#endif
#if defined(CKM_AES_CGM)
            case CKM_AES_GCM:
                return "AES_GCM";
#endif

#if defined(CKM_AES_CCM)
            case CKM_AES_CCM:
                return "AES_CCM";
#endif

#if defined(CKM_AES_CTS)
            case CKM_AES_CTS:
                return "AES_CTS";
#endif
#if defined(CKM_AES_CMAC)
            case CKM_AES_CMAC:
                return "AES_CMAC";

            case CKM_AES_CMAC_GENERAL:
                return "AES_CMAC_GENERAL";
#endif
#if defined(CKM_AES_XCBC_MAC)
            case CKM_AES_XCBC_MAC:
                return "AES_XCBC_MAC";

            case CKM_AES_XCBC_MAC_96:
                return "AES_XCBC_MAC_96";
#endif
#if defined(CKM_AES_GMAC)
            case CKM_AES_GMAC:
                return "AES_GMAC";
#endif
#if defined(CKM_TWOFISH_CBC)
            case CKM_TWOFISH_KEY_GEN:
                return "TWOFISH_KEY_GEN";

            case CKM_TWOFISH_CBC:
                return "TWOFISH_CBC";
#endif
#if defined(CKM_TWOFISH_CBC_PAD)
            case CKM_TWOFISH_CBC_PAD:
                return "TWOFISH_CBC_PAD";
#endif
#if defined(CKM_BLOWFISH_CBC)
            case CKM_BLOWFISH_KEY_GEN:
                return "BLOWFISH_KEY_GEN";

            case CKM_BLOWFISH_CBC:
                return "BLOWFISH_CBC";
#endif

#if defined(CKM_BLOWFISH_CBC_PAD)
            case CKM_BLOWFISH_CBC_PAD:
                return "BLOWFISH_CBC_PAD";
#endif
            case CKM_DES_ECB_ENCRYPT_DATA:
                return "DES_ECB_ENCRYPT_DATA";

            case CKM_DES_CBC_ENCRYPT_DATA:
                return "DES_CBC_ENCRYPT_DATA";

            case CKM_DES3_ECB_ENCRYPT_DATA:
                return "DES3_ECB_ENCRYPT_DATA";

            case CKM_DES3_CBC_ENCRYPT_DATA:
                return "DES3_CBC_ENCRYPT_DATA";

            case CKM_AES_ECB_ENCRYPT_DATA:
                return "AES_ECB_ENCRYPT_DATA";

            case CKM_AES_CBC_ENCRYPT_DATA:
                return "AES_CBC_ENCRYPT_DATA";


#if defined(CKM_GOSTR3411)
            case CKM_GOSTR3410:
                return "GOSTR3410";

            case CKM_GOSTR3410_KEY_PAIR_GEN:
                return "GOSTR3410_KEY_PAIR_GEN";

            case CKM_GOSTR3410_WITH_GOSTR3411:
                return "GOSTR3410_WITH_GOSTR3411";

            case CKM_GOSTR3410_KEY_WRAP:
                return "GOSTR3410_KEY_WRAP";

            case CKM_GOSTR3410_DERIVE:
                return "GOSTR3410_DERIVE";
#endif
#if defined(CKM_GOSTR3411)
            case CKM_GOSTR3411:
                return "GOSTR3411";

            case CKM_GOSTR3411_HMAC:
                return "GOSTR3411_HMAC";
#endif
#if defined(CKM_GOST28147)
            case CKM_GOST28147:
                return "GOST28147";

            case CKM_GOST28147_KEY_GEN:
                return "GOST28147_KEY_GEN";

            case CKM_GOST28147_ECB:
                return "GOST28147_ECB";

            case CKM_GOST28147_MAC:
                return "GOST28147_MAC";

            case CKM_GOST28147_KEY_WRAP:
                return "GOST28147_KEY_WRAP";
#endif
#if defined (CKM_CHACHA20)

            case CKM_CHACHA20_KEY_GEN:
                return "CHACHA20_KEY_GEN";

            case CKM_CHACHA20:
                return "CHACHA20";
#endif
#if defined(CKM_POLY1305)

            case CKM_POLY1305_KEY_GEN:
                return "POLY1305_KEY_GEN";

            case CKM_POLY1305:
                return "POLY1305";
#endif

            case CKM_DSA_PARAMETER_GEN:
                return "DSA_PARAMETER_GEN";

            case CKM_DH_PKCS_PARAMETER_GEN:
                return "DH_PKCS_PARAMETER_GEN";

            case CKM_X9_42_DH_PARAMETER_GEN:
                return "X9_42_DH_PARAMETER_GEN";

#if defined(CKM_DSA_PROBABLISTIC_PARAMETER_GEN)
            case CKM_DSA_PROBABLISTIC_PARAMETER_GEN:
                return "DSA_PROBABLISTIC_PARAMETER_GEN";
#endif
#if defined(CKM_DSA_SHAWE_TAYLOR_PARAMETER_GEN)
            case CKM_DSA_SHAWE_TAYLOR_PARAMETER_GEN:
                return "DSA_SHAWE_TAYLOR_PARAMETER_GEN";
#endif
#if defined(CKM_AES_OFB)
            case CKM_AES_OFB:
                return "AES_OFB";
#endif
#if defined(CKM_AES_CFB64)
            case CKM_AES_CFB64:
                return "AES_CFB64";
#endif
#if defined(CKM_AES_CFB8)
            case CKM_AES_CFB8:
                return "AES_CFB8";
#endif
#if defined(CKM_AES_CFB128)
            case CKM_AES_CFB128:
                return "AES_CFB128";
#endif

#if defined(CKM_AES_CFB1)
            case CKM_AES_CFB1:
                return "AES_CFB1";
#endif
            case CKM_VENDOR_DEFINED:
                return "VENDOR_DEFINED";

            case CKM_SHA224:
                return "SHA224";

            case CKM_SHA224_HMAC:
                return "SHA224_HMAC";

            case CKM_SHA224_HMAC_GENERAL:
                return "SHA224_HMAC_GENERAL";

            case CKM_SHA224_RSA_PKCS:
                return "SHA224_RSA_PKCS";

            case CKM_SHA224_RSA_PKCS_PSS:
                return "SHA224_RSA_PKCS_PSS";

            case CKM_SHA224_KEY_DERIVATION:
                return "SHA224_KEY_DERIVATION";

            case CKM_CAMELLIA_KEY_GEN:
                return "CAMELLIA_KEY_GEN";

            case CKM_CAMELLIA_ECB:
                return "CAMELLIA_ECB";

            case CKM_CAMELLIA_CBC:
                return "CAMELLIA_CBC";

            case CKM_CAMELLIA_MAC:
                return "CAMELLIA_MAC";

            case CKM_CAMELLIA_MAC_GENERAL:
                return "CAMELLIA_MAC_GENERAL";

            case CKM_CAMELLIA_CBC_PAD:
                return "CAMELLIA_CBC_PAD";

            case CKM_CAMELLIA_ECB_ENCRYPT_DATA:
                return "CAMELLIA_ECB_ENCRYPT_DATA";

            case CKM_CAMELLIA_CBC_ENCRYPT_DATA:
                return "CAMELLIA_CBC_ENCRYPT_DATA";

#if defined(CKM_CAMELLIA_CTR)
            case CKM_CAMELLIA_CTR:
                return "CAMELLIA_CTR";
#endif
            case CKM_AES_KEY_WRAP:
                return "AES_KEY_WRAP";

            case CKM_AES_KEY_WRAP_PAD:
                return "AES_KEY_WRAP_PAD";

#if defined(CKM_RSA_PKCS_TPM_1_1)
            case CKM_RSA_PKCS_TPM_1_1:
                return "RSA_PKCS_TPM_1_1";
#endif
#if defined(CKM_RSA_PKCS_OAEP_TPM_1_1)
            case CKM_RSA_PKCS_OAEP_TPM_1_1:
                return "RSA_PKCS_OAEP_TPM_1_1";
#endif
#if defined(CKM_EC_EDWARDS_KEY_PAIR_GEN)
            case CKM_EC_EDWARDS_KEY_PAIR_GEN:
                return "EC_EDWARDS_KEY_PAIR_GEN";
#endif
#if defined(CKM_EC_MONTGOMERY_KEY_PAIR_GEN)

            case CKM_EC_MONTGOMERY_KEY_PAIR_GEN:
                return "EC_MONTGOMERY_KEY_PAIR_GEN";
#endif
#if defined(CKM_EDDSA)
            case CKM_EDDSA:
                return "EDDSA";
#endif
#if defined(CKM_XEDDSA)

            case CKM_XEDDSA:
                return "XEDDSA";
#endif
        }

        return nullptr;
    }

    std::string PKCS11::mechStringEx(const CK_MECHANISM_TYPE & type)
    {
        if(auto str = mechString(type))
        {
            return str;
        }

        std::ostringstream os;
        os << "UNKNOWN" << "_" << std::setw(8) << std::setfill('0') << std::uppercase << std::hex << type;
        return os.str();
    }

    const char* PKCS11::rvString(const CK_RV & rv)
    {
        switch(rv)
        {
            case CKR_CANCEL:
                return "CANCEL";

            case CKR_HOST_MEMORY:
                return "HOST_MEMORY";

            case CKR_SLOT_ID_INVALID:
                return "SLOT_ID_INVALID";

            case CKR_GENERAL_ERROR:
                return "GENERAL_ERROR";

            case CKR_FUNCTION_FAILED:
                return "FUNCTION_FAILED";

            case CKR_ARGUMENTS_BAD:
                return "ARGUMENTS_BAD";

            case CKR_NO_EVENT:
                return "NO_EVENT";

            case CKR_NEED_TO_CREATE_THREADS:
                return "NEED_TO_CREATE_THREADS";

            case CKR_CANT_LOCK:
                return "CANT_LOCK";

            case CKR_ATTRIBUTE_READ_ONLY:
                return "ATTRIBUTE_READ_ONLY";

            case CKR_ATTRIBUTE_SENSITIVE:
                return "ATTRIBUTE_SENSITIVE";

            case CKR_ATTRIBUTE_TYPE_INVALID:
                return "ATTRIBUTE_TYPE_INVALID";

            case CKR_ATTRIBUTE_VALUE_INVALID:
                return "ATTRIBUTE_VALUE_INVALID";

#if defined(CKR_ACTION_PROHIBITED)
            case CKR_ACTION_PROHIBITED:
                return "ACTION_PROHIBITED";
#endif
            case CKR_DATA_INVALID:
                return "DATA_INVALID";

            case CKR_DATA_LEN_RANGE:
                return "DATA_LEN_RANGE";

            case CKR_DEVICE_ERROR:
                return "DEVICE_ERROR";

            case CKR_DEVICE_MEMORY:
                return "DEVICE_MEMORY";

            case CKR_DEVICE_REMOVED:
                return "DEVICE_REMOVED";

            case CKR_ENCRYPTED_DATA_INVALID:
                return "ENCRYPTED_DATA_INVALID";

            case CKR_ENCRYPTED_DATA_LEN_RANGE:
                return "ENCRYPTED_DATA_LEN_RANGE";

            case CKR_FUNCTION_CANCELED:
                return "FUNCTION_CANCELED";

            case CKR_FUNCTION_NOT_PARALLEL:
                return "FUNCTION_NOT_PARALLEL";

            case CKR_FUNCTION_NOT_SUPPORTED:
                return "FUNCTION_NOT_SUPPORTED";

            case CKR_KEY_HANDLE_INVALID:
                return "KEY_HANDLE_INVALID";

            case CKR_KEY_SIZE_RANGE:
                return "KEY_SIZE_RANGE";

            case CKR_KEY_TYPE_INCONSISTENT:
                return "KEY_TYPE_INCONSISTENT";

            case CKR_KEY_NOT_NEEDED:
                return "KEY_NOT_NEEDED";

            case CKR_KEY_CHANGED:
                return "KEY_CHANGED";

            case CKR_KEY_NEEDED:
                return "KEY_NEEDED";

            case CKR_KEY_INDIGESTIBLE:
                return "KEY_INDIGESTIBLE";

            case CKR_KEY_FUNCTION_NOT_PERMITTED:
                return "KEY_FUNCTION_NOT_PERMITTED";

            case CKR_KEY_NOT_WRAPPABLE:
                return "KEY_NOT_WRAPPABLE";

            case CKR_KEY_UNEXTRACTABLE:
                return "KEY_UNEXTRACTABLE";

            case CKR_MECHANISM_INVALID:
                return "MECHANISM_INVALID";

            case CKR_MECHANISM_PARAM_INVALID:
                return "MECHANISM_PARAM_INVALID";

            case CKR_OBJECT_HANDLE_INVALID:
                return "OBJECT_HANDLE_INVALID";

            case CKR_OPERATION_ACTIVE:
                return "OPERATION_ACTIVE";

            case CKR_OPERATION_NOT_INITIALIZED:
                return "OPERATION_NOT_INITIALIZED";

            case CKR_PIN_INCORRECT:
                return "PIN_INCORRECT";

            case CKR_PIN_INVALID:
                return "PIN_INVALID";

            case CKR_PIN_LEN_RANGE:
                return "PIN_LEN_RANGE";

            case CKR_PIN_EXPIRED:
                return "PIN_EXPIRED";

            case CKR_PIN_LOCKED:
                return "PIN_LOCKED";

            case CKR_SESSION_CLOSED:
                return "SESSION_CLOSED";

            case CKR_SESSION_COUNT:
                return "SESSION_COUNT";

            case CKR_SESSION_HANDLE_INVALID:
                return "SESSION_HANDLE_INVALID";

            case CKR_SESSION_PARALLEL_NOT_SUPPORTED:
                return "SESSION_PARALLEL_NOT_SUPPORTED";

            case CKR_SESSION_READ_ONLY:
                return "SESSION_READ_ONLY";

            case CKR_SESSION_EXISTS:
                return "SESSION_EXISTS";

            case CKR_SESSION_READ_ONLY_EXISTS:
                return "SESSION_READ_ONLY_EXISTS";

            case CKR_SESSION_READ_WRITE_SO_EXISTS:
                return "SESSION_READ_WRITE_SO_EXISTS";

            case CKR_SIGNATURE_INVALID:
                return "SIGNATURE_INVALID";

            case CKR_SIGNATURE_LEN_RANGE:
                return "SIGNATURE_LEN_RANGE";

            case CKR_TEMPLATE_INCOMPLETE:
                return "TEMPLATE_INCOMPLETE";

            case CKR_TEMPLATE_INCONSISTENT:
                return "TEMPLATE_INCONSISTENT";

            case CKR_TOKEN_NOT_PRESENT:
                return "TOKEN_NOT_PRESENT";

            case CKR_TOKEN_NOT_RECOGNIZED:
                return "TOKEN_NOT_RECOGNIZED";

            case CKR_TOKEN_WRITE_PROTECTED:
                return "TOKEN_WRITE_PROTECTED";

            case CKR_UNWRAPPING_KEY_SIZE_RANGE:
                return "UNWRAPPING_KEY_SIZE_RANGE";

            case CKR_UNWRAPPING_KEY_TYPE_INCONSISTENT:
                return "UNWRAPPING_KEY_TYPE_INCONSISTENT";

            case CKR_USER_ALREADY_LOGGED_IN:
                return "USER_ALREADY_LOGGED_IN";

            case CKR_USER_NOT_LOGGED_IN:
                return "USER_NOT_LOGGED_IN";

            case CKR_USER_PIN_NOT_INITIALIZED:
                return "USER_PIN_NOT_INITIALIZED";

            case CKR_USER_TYPE_INVALID:
                return "USER_TYPE_INVALID";

            case CKR_USER_ANOTHER_ALREADY_LOGGED_IN:
                return "USER_ANOTHER_ALREADY_LOGGED_IN";

            case CKR_USER_TOO_MANY_TYPES:
                return "USER_TOO_MANY_TYPES";

            case CKR_WRAPPED_KEY_INVALID:
                return "WRAPPED_KEY_INVALID";

            case CKR_WRAPPED_KEY_LEN_RANGE:
                return "WRAPPED_KEY_LEN_RANGE";

            case CKR_WRAPPING_KEY_HANDLE_INVALID:
                return "WRAPPING_KEY_HANDLE_INVALID";

            case CKR_WRAPPING_KEY_SIZE_RANGE:
                return "WRAPPING_KEY_SIZE_RANGE";

            case CKR_WRAPPING_KEY_TYPE_INCONSISTENT:
                return "WRAPPING_KEY_TYPE_INCONSISTENT";

            case CKR_RANDOM_SEED_NOT_SUPPORTED:
                return "RANDOM_SEED_NOT_SUPPORTED";

            case CKR_RANDOM_NO_RNG:
                return "RANDOM_NO_RNG";

            case CKR_DOMAIN_PARAMS_INVALID:
                return "DOMAIN_PARAMS_INVALID";

#if defined(CKR_CURVE_NOT_SUPPORTED)
            case CKR_CURVE_NOT_SUPPORTED:
                return "CURVE_NOT_SUPPORTED";
#endif
            case CKR_BUFFER_TOO_SMALL:
                return "BUFFER_TOO_SMALL";

            case CKR_SAVED_STATE_INVALID:
                return "SAVED_STATE_INVALID";

            case CKR_INFORMATION_SENSITIVE:
                return "INFORMATION_SENSITIVE";

            case CKR_STATE_UNSAVEABLE:
                return "STATE_UNSAVEABLE";

            case CKR_CRYPTOKI_NOT_INITIALIZED:
                return "CRYPTOKI_NOT_INITIALIZED";

            case CKR_CRYPTOKI_ALREADY_INITIALIZED:
                return "CRYPTOKI_ALREADY_INITIALIZED";

            case CKR_MUTEX_BAD:
                return "MUTEX_BAD";

            case CKR_MUTEX_NOT_LOCKED:
                return "MUTEX_NOT_LOCKED";

#if defined(CKR_NEW_PIN_MODE)
            case CKR_NEW_PIN_MODE:
                return "NEW_PIN_MODE";
#endif
#if defined(CKR_NEXT_OTP)
            case CKR_NEXT_OTP:
                return "NEXT_OTP";
#endif
#if defined(CKR_EXCEEDED_MAX_ITERATIONS)
            case CKR_EXCEEDED_MAX_ITERATIONS:
                return "EXCEEDED_MAX_ITERATIONS";
#endif
#if defined(CKR_FIPS_SELF_TEST_FAILED)
            case CKR_FIPS_SELF_TEST_FAILED:
                return "FIPS_SELF_TEST_FAILED";
#endif
#if defined(CKR_LIBRARY_LOAD_FAILED)
            case CKR_LIBRARY_LOAD_FAILED:
                return "LIBRARY_LOAD_FAILED";
#endif
#if defined(CKR_PIN_TOO_WEAK)
            case CKR_PIN_TOO_WEAK:
                return "PIN_TOO_WEAK";
#endif
#if defined(CKR_PUBLIC_KEY_INVALID)
            case CKR_PUBLIC_KEY_INVALID:
                return "PUBLIC_KEY_INVALID";
#endif
            case CKR_FUNCTION_REJECTED:
                return "FUNCTION_REJECTED";
#if defined(CKR_OPERATION_CANCEL_FAILED)

            case CKR_OPERATION_CANCEL_FAILED:
                return "OPERATION_CANCEL_FAILED";
#endif

            case CKR_VENDOR_DEFINED:
                return "VENDOR_DEFINED";

            default:
                break;
        }

        return "UNKNOWN";
    }
}
