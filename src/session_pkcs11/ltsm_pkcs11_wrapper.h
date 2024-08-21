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

#ifndef _PKCS11_WRAPPER_
#define _PKCS11_WRAPPER_

#include <p11-kit/pkcs11.h>

#include <list>
#include <vector>
#include <memory>
#include <string>
#include <climits>
#include <stdexcept>
#include <forward_list>
#include <initializer_list>

namespace LTSM
{
    struct pkcs11_error : public std::runtime_error
    {
        explicit pkcs11_error(const std::string & what) : std::runtime_error(what) {}
        explicit pkcs11_error(const char* what) : std::runtime_error(what) {}
    };

    namespace PKCS11
    {
        const char* rvString(const CK_RV &);
        const char* mechString(const CK_MECHANISM_TYPE &);
        std::string mechStringEx(const CK_MECHANISM_TYPE &);

        /// MechInfo
        struct MechInfo : CK_MECHANISM_INFO
        {
            /*
            CK_MECHANISM_INFO
            {
                CK_ULONG   ulMinKeySize;
                CK_ULONG   ulMaxKeySize;
                CK_FLAGS   flags;
            }
            */

            inline bool isHardware(void) const { return flags & CKF_HW; }
            inline bool isEncrypt(void) const { return flags & CKF_ENCRYPT; }
            inline bool isDecrypt(void) const { return flags & CKF_DECRYPT; }
            inline bool isDigest(void) const { return flags & CKF_DIGEST; }
            inline bool isSign(void) const { return flags & CKF_SIGN; }
            inline bool isVerify(void) const { return flags & CKF_VERIFY; }
            inline bool isWrap(void) const { return flags & CKF_WRAP; }
            inline bool isUnwrap(void) const { return flags & CKF_UNWRAP; }
            inline bool isGenerate(void) const { return flags & CKF_GENERATE; }
            inline bool isDerive(void) const { return flags & CKF_DERIVE; }

            inline size_t getMinKeySize(void) const { return ulMinKeySize; }
            inline size_t getMaxKeySize(void) const { return ulMaxKeySize; }
        };

        typedef CK_MECHANISM_TYPE MechType;
        typedef std::vector<MechType> MechList;
        typedef std::unique_ptr<MechInfo> MechInfoPtr;

        /// SlotInfo
        struct SlotInfo : CK_SLOT_INFO
        {
            /*
            CK_SLOT_INFO
            {
                CK_UTF8CHAR   slotDescription[64];
                CK_UTF8CHAR   manufacturerID[32];
                CK_FLAGS      flags;
                CK_VERSION    hardwareVersion;
                CK_VERSION    firmwareVersion;
            }
            */

            std::string getManufacturerID(void) const;
            std::string getDescription(void) const;

            bool flagTokenPresent(void) const;
            bool flagRemovableDevice(void) const;
        };

        typedef CK_SLOT_ID SlotId;
        typedef std::unique_ptr<SlotInfo> SlotInfoPtr;

        /// TokenInfo
        struct TokenInfo : CK_TOKEN_INFO
        {
            /*
            CK_TOKEN_INFO
            {
                CK_UTF8CHAR   label[32];
                CK_UTF8CHAR   manufacturerID[32];
                CK_UTF8CHAR   model[16];
                CK_CHAR       serialNumber[16];
                CK_FLAGS      flags;
                CK_ULONG      ulMaxSessionCount;
                CK_ULONG      ulSessionCount;
                CK_ULONG      ulMaxRwSessionCount;
                CK_ULONG      ulRwSessionCount;
                CK_ULONG      ulMaxPinLen;
                CK_ULONG      ulMinPinLen;
                CK_ULONG      ulTotalPublicMemory;
                CK_ULONG      ulFreePublicMemory;
                CK_ULONG      ulTotalPrivateMemory;
                CK_ULONG      ulFreePrivateMemory;
                CK_VERSION    hardwareVersion;
                CK_VERSION    firmwareVersion;
                CK_CHAR       utcTime[16];
            }
            */

            std::string getManufacturerID(void) const;
            std::string getLabel(void) const;
            std::string getModel(void) const;
            std::string getSerialNumber(void) const;
            std::string getUtcTime(void) const;

            bool flagWriteProtected(void) const;
            bool flagLoginRequired(void) const;
            bool flagTokenInitialized(void) const;
        };

        typedef std::unique_ptr<TokenInfo> TokenInfoPtr;

        enum SessionState
        {
            PublicRO = CKS_RO_PUBLIC_SESSION,
            PublicRW = CKS_RW_PUBLIC_SESSION,
            UserRO = CKS_RO_USER_FUNCTIONS,
            UserRW = CKS_RW_USER_FUNCTIONS,
            FunctionsRW = CKS_RW_SO_FUNCTIONS
        };

        /// SessionInfo
        struct SessionInfo : CK_SESSION_INFO
        {
            /*
            CK_SESSION_INFO
            {
                CK_SLOT_ID   slotID;
                CK_STATE     state;
                CK_FLAGS     flags;
                CK_ULONG     ulDeviceError;
            }
            */

            bool flagRwSession(void) const;
            bool flagSerialSession(void) const;
        };

        typedef std::unique_ptr<SessionInfo> SessionInfoPtr;

        /// LibraryInfo
        struct LibraryInfo : CK_INFO
        {
            /*
            CK_INFO
            {
                CK_VERSION    cryptokiVersion;
                CK_UTF8CHAR   manufacturerID[32];
                CK_FLAGS      flags;
                CK_UTF8CHAR   libraryDescription[32];
                CK_VERSION    libraryVersion;
            }
            */

            std::string getManufacturerID(void) const;
            std::string getDescription(void) const;
        };

        typedef std::unique_ptr<LibraryInfo> LibraryInfoPtr;

        class Slot;
        typedef std::list<Slot> SlotList;

        class Session;
        typedef std::unique_ptr<Session> SessionPtr;

        class Library;
        typedef std::shared_ptr<Library> LibraryPtr;

        typedef CK_OBJECT_CLASS ObjectClass;
        typedef CK_OBJECT_HANDLE ObjectHandle;
        typedef std::vector<ObjectHandle> ObjectList;
        typedef std::vector<uint8_t> RawData;

        struct RawDataRef : std::pair<const uint8_t*, size_t>
        {
            RawDataRef() : std::pair<const uint8_t*, size_t>(nullptr, 0) {}
            RawDataRef(const uint8_t* ptr, size_t len) : std::pair<const uint8_t*, size_t>(ptr, len) {}
            RawDataRef(const std::vector<uint8_t> & v) : std::pair<const uint8_t*, size_t>(v.data(), v.size()) {}

            inline const uint8_t* data(void) const { return first; }
            inline size_t         size(void) const { return second; }

            std::string         toString(void) const;
            std::string         toHexString(std::string_view sep = ",", bool pref = true) const;
            RawData             copy(void) const { return RawData(first, first + second); }

            bool                operator==(const RawDataRef &) const;
        };

        typedef RawDataRef ObjectIdRef;

        class Date
        {
            /*
            CK_DATE
            {
                char year[4];
                char month[2];
                char day[2];
            }
            */

            uint16_t year = 0;
            uint8_t month = 0;
            uint8_t day = 0;

        public:
            Date() = default;
            Date(const RawDataRef &);

            int                 getYear(void) const { return year; }
            int                 getMonth(void) const { return month; }
            int                 getDay(void) const { return day; }

            std::string         toString(std::string_view format = "%Y%m%d" /*strftime format*/) const;
        };

        /// BaseObject
        class ObjectInfo
        {
        protected:
            friend class Session;

            std::vector<CK_ATTRIBUTE>  attrs;
            std::unique_ptr<uint8_t[]> ptr;
            ObjectHandle               handle = 0;

        public:
            inline static const auto types = { CKA_ID, CKA_START_DATE, CKA_END_DATE, CKA_TOKEN, CKA_PRIVATE, CKA_MODIFIABLE, CKA_LABEL };

            ObjectInfo() = default;
            ObjectInfo(ObjectInfo &&) = default;
            virtual ~ObjectInfo() = default;

            RawDataRef          getRawData(const CK_ATTRIBUTE_TYPE &) const;
            bool                getBool(const CK_ATTRIBUTE_TYPE &) const;

            const ObjectHandle & getHandle(void) const { return handle; }

            ObjectIdRef         getId(void) const { return getRawData(CKA_ID); }
            std::string         getLabel(void) const;

            Date                getStartDate(void) const { return getRawData(CKA_START_DATE); }
            Date                getEndDate(void) const { return getRawData(CKA_END_DATE); }

            bool                isToken(void) const { return getBool(CKA_TOKEN); };
            bool                isPrivate(void) const { return getBool(CKA_PRIVATE); };
            bool                isModifiable(void) const { return getBool(CKA_MODIFIABLE); };
        };

        /// Certificate
        struct CertificateInfo : ObjectInfo
        {
            inline static const auto types = { CKA_SUBJECT, CKA_ISSUER, CKA_SERIAL_NUMBER, CKA_VALUE };

            RawDataRef          getRawValue(void) const { return getRawData(CKA_VALUE); }
            RawDataRef          getSubject(void) const { return getRawData(CKA_SUBJECT); }
            RawDataRef          getIssuer(void) const { return getRawData(CKA_ISSUER); }
            RawDataRef          getSerialNumber(void) const { return getRawData(CKA_SERIAL_NUMBER); }

            CertificateInfo() = default;
            CertificateInfo(ObjectInfo && obj) noexcept : ObjectInfo(std::move(obj)) {}
        };

        /// PublicKey
        struct PublicKeyInfo : ObjectInfo
        {
            inline static const auto types = { CKA_SUBJECT, CKA_ENCRYPT, CKA_VERIFY, CKA_WRAP,  };

            RawDataRef          getSubject(void) const { return getRawData(CKA_SUBJECT); }

            bool                isEncrypt(void) const{ return getBool(CKA_ENCRYPT); }
            bool                isVerify(void) const{ return getBool(CKA_VERIFY); }
            bool                isWrap(void) const{ return getBool(CKA_WRAP); }

            PublicKeyInfo() = default;
            PublicKeyInfo(ObjectInfo && obj) noexcept : ObjectInfo(std::move(obj)) {}
        };

        /// PrivateKey
        struct PrivateKeyInfo : ObjectInfo
        {
            inline static const auto types = { CKA_SUBJECT, CKA_DECRYPT, CKA_SIGN, CKA_UNWRAP, CKA_ALWAYS_AUTHENTICATE };

            RawDataRef         getSubject(void) const { return getRawData(CKA_SUBJECT); }

            bool               isDecrypt(void) const{ return getBool(CKA_DECRYPT); }
            bool               isSign(void) const{ return getBool(CKA_SIGN); }
            bool               isUnwrap(void) const{ return getBool(CKA_UNWRAP); }
            bool               isAlwaysAuthenticate(void) const{ return getBool(CKA_ALWAYS_AUTHENTICATE); }

            PrivateKeyInfo() = default;
            PrivateKeyInfo(ObjectInfo && obj) noexcept : ObjectInfo(std::move(obj)) {}
        };

        // API
        LibraryPtr              loadLibrary(std::string_view);
        SessionPtr              createSession(const SlotId &, bool rwmode, const LibraryPtr &);
        SlotList                getSlots(bool tokenPresentOnly, const LibraryPtr &);

        /// Library
        class Library
        {
            std::forward_list<CK_SESSION_HANDLE> sessions;

            std::unique_ptr<void, int(*)(void*)> dll;
            CK_FUNCTION_LIST_PTR pFunctionList = nullptr;

            Library(const Library &) = delete;
            Library & operator=(const Library &) = delete;

        protected:
            friend class Session;

            CK_SESSION_HANDLE   sessionOpen(const SlotId &, bool rwmode);
            void                sessionClose(CK_SESSION_HANDLE);

        public:
            Library();
            Library(std::string_view);

            Library(Library &&) noexcept;
            Library & operator=(Library &&) noexcept;

            ~Library();

            CK_FUNCTION_LIST_PTR func(void) { return pFunctionList; }

            LibraryInfoPtr      getLibraryInfo(void) const;
            bool                waitSlotEvent(bool async, SlotId & res) const;
        };

        /// Slot
        class Slot
        {
        protected:
            std::weak_ptr<Library> weak;
            SlotId              id = ULONG_MAX;

        public:
            Slot(const SlotId &, const LibraryPtr &);
            virtual ~Slot() {}

            const SlotId & slotId(void) const { return id; }

            MechList            getMechanisms(void) const;

            bool                getSlotInfo(SlotInfo &) const;
            bool                getTokenInfo(TokenInfo &) const;

            SlotInfoPtr         getSlotInfo(void) const;
            TokenInfoPtr        getTokenInfo(void) const;
            MechInfoPtr         getMechInfo(const MechType &) const;
        };

        /// Session
        class Session : public Slot
        {
            CK_SESSION_HANDLE   sid = CK_INVALID_HANDLE;
            bool                islogged = false;

        public:
            Session(const SlotId &, bool rwmode, const LibraryPtr &);
            ~Session();

            SessionInfoPtr      getInfo(void) const;
            RawData             generateRandom(size_t) const;

            RawData             digestData(const void* ptr, size_t len, const MechType &) const;

            RawData             digestMD5(const void* ptr, size_t len) const;
            RawData             digestSHA1(const void* ptr, size_t len) const;
            RawData             digestSHA256(const void* ptr, size_t len) const;

            bool                login(std::string_view pin, bool admin = false);
            void                logout(void);

            ObjectList          findTokenObjects(const ObjectClass &, size_t maxObjects = 32) const;
            ObjectList          findTokenObjects(size_t maxObjects, const CK_ATTRIBUTE*, size_t counts) const;

            ObjectHandle        findPublicKey(const ObjectIdRef &) const;
            ObjectHandle        findPrivateKey(const ObjectIdRef &) const;

            ObjectList          getCertificates(bool havePulicPrivateKeys = false) const;
            ObjectList          getPublicKeys(void) const { return findTokenObjects(CKO_PUBLIC_KEY); }
            ObjectList          getPrivateKeys(void) const { return findTokenObjects(CKO_PRIVATE_KEY); }

            ObjectInfo          getObjectInfo(const ObjectHandle &, std::initializer_list<CK_ATTRIBUTE_TYPE> = {}) const;

            CertificateInfo     getCertificateInfo(const ObjectHandle &) const;
            PublicKeyInfo       getPublicKeyInfo(const ObjectHandle &) const;
            PrivateKeyInfo      getPrivateKeyInfo(const ObjectHandle &) const;

            bool                getAttributes(const ObjectHandle &, const CK_ATTRIBUTE*, size_t counts) const;

            ssize_t             getAttribLength(const ObjectHandle &, const CK_ATTRIBUTE_TYPE &) const;
            RawData             getAttribData(const ObjectHandle &, const CK_ATTRIBUTE_TYPE &) const;

            RawData             signData(const ObjectIdRef & certId, const void* ptr, size_t len, const MechType & = CKM_RSA_PKCS) const;

            RawData             encryptData(const ObjectIdRef & certId, const void* ptr, size_t len, const MechType & = CKM_RSA_PKCS) const;
            RawData             decryptData(const ObjectIdRef & certId, const void* ptr, size_t len, const MechType & = CKM_RSA_PKCS) const;
        };

    } // PKCS11
} // LTSM

#endif
