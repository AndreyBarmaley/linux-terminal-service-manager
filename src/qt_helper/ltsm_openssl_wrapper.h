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

#ifndef _OPENSSL_WRAPPER_
#define _OPENSSL_WRAPPER_

#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

#include <openssl/x509.h>

namespace LTSM
{
    struct openssl_error : public std::runtime_error
    {
        explicit openssl_error(const std::string & what) : std::runtime_error(what) {}
        explicit openssl_error(const char* what) : std::runtime_error(what) {}
    };

    namespace OpenSSL
    {
        class PublicKey
        {
            std::unique_ptr<EVP_PKEY, void(*)(EVP_PKEY*)> evp{ nullptr, EVP_PKEY_free };
            std::unique_ptr<EVP_PKEY_CTX, void(*)(EVP_PKEY_CTX*)> ctx{ nullptr, EVP_PKEY_CTX_free };

        public:
            PublicKey(X509*);

            std::vector<uint8_t> encryptData(const void*, size_t) const;
        };

        class Certificate
        {
        protected:
            std::unique_ptr<X509, void(*)(X509*)> x509{ nullptr, X509_free };
            Certificate() = default;

        public:
            Certificate(X509*);
            virtual ~Certificate() = default;

            std::string subjectName(void) const;
            std::string issuerName(void) const;
            std::string notBeforeTime(void) const;
            std::string notAfterTime(void) const;
            std::string serialNumber(void) const;

            PublicKey publicKey(void) const;
        };

        class CertificatePem : public Certificate
        {
        public:
            CertificatePem(std::string_view);
        };

        class CertificateDer : public Certificate
        {
        public:
            CertificateDer(const std::vector<uint8_t> &);
            CertificateDer(const void*, size_t);
        };
    }
}

#endif
