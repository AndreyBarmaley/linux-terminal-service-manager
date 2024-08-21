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

#include <memory>
#include <stdexcept>

#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_openssl_wrapper.h"

namespace LTSM
{
    OpenSSL::Certificate::Certificate(X509* ptr)
    {
        x509.reset(ptr);
    }

    std::string OpenSSL::Certificate::subjectName(void) const 
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new(BIO_s_mem()), BIO_free_all };
        if(! bio)
        {
            Application::error("%s: %s failed", __FUNCTION__, "BIO_new");
            return "";
        }

        if(! X509_NAME_print(bio.get(), X509_get_subject_name(x509.get()), 0))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "X509_get_subject_name", ERR_error_string(err_code, nullptr));
            return "";
        }

        char* ptr = nullptr;
        size_t len = BIO_get_mem_data(bio.get(), & ptr);

        return std::string(ptr, len);
    }

    std::string OpenSSL::Certificate::issuerName(void) const
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new(BIO_s_mem()), BIO_free_all };
        if(! bio)
        {
            Application::error("%s: %s failed", __FUNCTION__, "BIO_new");
            return "";
        }

        if(! X509_NAME_print(bio.get(), X509_get_issuer_name(x509.get()), 0))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "X509_get_issuer_name", ERR_error_string(err_code, nullptr));
            return "";
        }

        char* ptr = nullptr;
        size_t len = BIO_get_mem_data(bio.get(), & ptr);

        return std::string(ptr, len);
    }

    std::string OpenSSL::Certificate::notBeforeTime(void) const
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new(BIO_s_mem()), BIO_free_all };
        if(! bio)
        {
            Application::error("%s: %s failed", __FUNCTION__, "BIO_new");
            return "";
        }

        if(! ASN1_TIME_print(bio.get(), X509_get_notBefore(x509.get())))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "X509_get_notBefore", ERR_error_string(err_code, nullptr));
            return "";
        }

        char* ptr = nullptr;
        size_t len = BIO_get_mem_data(bio.get(), & ptr);

        return std::string(ptr, len);
    }

    std::string OpenSSL::Certificate::notAfterTime(void) const
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new(BIO_s_mem()), BIO_free_all };
        if(! bio)
        {
            Application::error("%s: %s failed", __FUNCTION__, "BIO_new");
            return "";
        }

        if(! ASN1_TIME_print(bio.get(), X509_get_notAfter(x509.get())))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "X509_get_notAfter", ERR_error_string(err_code, nullptr));
            return "";
        }

        char* ptr = nullptr;
        size_t len = BIO_get_mem_data(bio.get(), & ptr);

        return std::string(ptr, len);
    }

    std::string OpenSSL::Certificate::serialNumber(void) const
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new(BIO_s_mem()), BIO_free_all };
        if(! bio)
        {
            Application::error("%s: %s failed", __FUNCTION__, "BIO_new");
            return "";
        }

        std::unique_ptr<BIGNUM, void(*)(BIGNUM*)> bn{ ASN1_INTEGER_to_BN(X509_get_serialNumber(x509.get()), NULL), BN_free };
        if(! bn)
        {
            Application::error("%s: %s failed", __FUNCTION__, "X509_get_serialNumber");
            return "";
        }

        size_t len = BN_num_bytes(bn.get());
        std::vector<uint8_t> buf(len, 0);

        BN_bn2bin(bn.get(), buf.data());
        return Tools::buffer2hexstring<uint8_t>(buf.data(), buf.size(), 2, ":", false);
    }

    OpenSSL::PublicKey OpenSSL::Certificate::publicKey(void) const
    {
        return PublicKey(x509.get());
    }

    OpenSSL::CertificatePem::CertificatePem(std::string_view str)
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new_mem_buf(str.data(), str.length()), BIO_free_all };
        if(! bio)
        {
            Application::error("%s: %s failed", __FUNCTION__, "BIO_new_mem_buf");
            throw openssl_error(NS_FuncName);
        }

        x509.reset(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));

        if(! x509)
        {
            Application::error("%s: %s failed", __FUNCTION__, "PEM_read_bio_X509");
            throw openssl_error(NS_FuncName);
        }
    }

    OpenSSL::CertificateDer::CertificateDer(const void* data, size_t length)
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new_mem_buf(data, length), BIO_free_all };
        if(! bio)
        {
            Application::error("%s: %s failed", __FUNCTION__, "BIO_new_mem_buf");
            throw openssl_error(NS_FuncName);
        }

        X509* ptr = nullptr;
        if(! d2i_X509_bio(bio.get(), & ptr))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "d2i_X509", ERR_error_string(err_code, nullptr));
            throw openssl_error(NS_FuncName);
        }

        x509.reset(ptr);
    }

    OpenSSL::CertificateDer::CertificateDer(const std::vector<uint8_t> & buf)
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new_mem_buf(buf.data(), buf.size()), BIO_free_all };
        if(! bio)
        {
            Application::error("%s: %s failed", __FUNCTION__, "BIO_new_mem_buf");
            throw openssl_error(NS_FuncName);
        }

        X509* ptr = nullptr;
        if(! d2i_X509_bio(bio.get(), & ptr))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "d2i_X509", ERR_error_string(err_code, nullptr));
            throw openssl_error(NS_FuncName);
        }

        x509.reset(ptr);
    }

    OpenSSL::PublicKey::PublicKey(X509* x509)
    {
        evp.reset(X509_get_pubkey(x509));

        if(! evp)
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "X509_get_pubkey", ERR_error_string(err_code, nullptr));
            throw openssl_error(NS_FuncName);
        }

        ctx.reset(EVP_PKEY_CTX_new(evp.get(), nullptr));
        if(! ctx)
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "EVP_PKEY_CTX_new", ERR_error_string(err_code, nullptr));
            throw openssl_error(NS_FuncName);
        }
    }

    std::vector<uint8_t> OpenSSL::PublicKey::encryptData(const void* data, size_t length) const
    {
        if(1 != EVP_PKEY_encrypt_init(ctx.get()))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "EVP_PKEY_encrypt_init", ERR_error_string(err_code, nullptr));
            return {};
        }

        size_t enclen = 0;
        if(1 != EVP_PKEY_encrypt(ctx.get(), nullptr, & enclen, (const unsigned char*) data, length))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "EVP_PKEY_encrypt", ERR_error_string(err_code, nullptr));
            return {};
        }

        std::vector<uint8_t> encbuf(enclen);

        if(1 != EVP_PKEY_encrypt(ctx.get(), encbuf.data(), & enclen, (const unsigned char*) data, length))
        {
            unsigned long err_code = ERR_get_error();
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "EVP_PKEY_encrypt", ERR_error_string(err_code, nullptr));
            return {};
        }

        if(enclen < encbuf.size())
            encbuf.resize(enclen);

        return encbuf;
    }
}
