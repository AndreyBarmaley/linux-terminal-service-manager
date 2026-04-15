/***********************************************************************
 *   Copyright © 2026 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

#ifndef _LTSM_BOOST_SOCKETS_
#define _LTSM_BOOST_SOCKETS_

#include "ltsm_sockets.h"

#include <stdexcept>
#include <openssl/ssl.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl/stream.hpp>

namespace LTSM {
    template <typename Stream>
    class BoostStream : public NetworkStream {
      protected:
        mutable Stream stream_;

      public:
        explicit BoostStream(Stream && st) : stream_{std::move(st)} {}

        Stream & native(void) {
            return stream_;
        }

        bool hasInput(void) const override {
            return 0 < stream_.available(); //NetworkStream::hasInput(stream_.native_handle());
        }

        size_t hasData(void) const override {
            return stream_.available();
        }

#ifdef LTSM_WITH_GNUTLS
        void setupTLS(gnutls::session* sess) const override {
            sess->set_transport_ptr(reinterpret_cast<gnutls_transport_ptr_t>(stream_.native_handle()));
        }
#endif

        void sendRaw(const void* ptr, size_t len) override {
            boost::asio::write(stream_, boost::asio::const_buffer(ptr, len), boost::asio::transfer_all());
        }

        void recvRaw(void* ptr, size_t len) const override {
            boost::asio::read(stream_, boost::asio::buffer(ptr, len), boost::asio::transfer_all());
        }
    };

    template <typename Stream>
    class BoostSslStream : public NetworkStream {
      protected:
        boost::asio::ssl::context ssl_ctx_;
        mutable boost::asio::ssl::stream<Stream> stream_;

      public:
        explicit BoostSslStream(Stream && st, const boost::asio::ssl::context::method & method)
            : ssl_ctx_{method}, stream_{std::move(st), ssl_ctx_} {}

        boost::asio::ssl::context & context(void) {
            return ssl_ctx_;
        }

        boost::asio::ssl::stream<Stream> & native(void) {
            return stream_;
        }

        void setCipherSuite(const char* list) {
            if(list) {
                auto ssl = stream_.native_handle();
                SSL_set_cipher_list(ssl, list);
            }
        }

        bool hasInput(void) const override {
            auto ssl = stream_.native_handle();
            return 0 < SSL_pending(ssl) ||
                0 < stream_.lowest_layer().available();
        }

        size_t hasData(void) const override {
            auto ssl = stream_.native_handle();
            return SSL_pending(ssl);
        }

        void sendRaw(const void* ptr, size_t len) override {
            boost::asio::write(stream_, boost::asio::const_buffer(ptr, len), boost::asio::transfer_all());
        }

        void recvRaw(void* ptr, size_t len) const override {
            boost::asio::read(stream_, boost::asio::buffer(ptr, len), boost::asio::transfer_all());
        }
    };
} // LTSM

#endif // _LTSM_BOOST_SOCKETS_
