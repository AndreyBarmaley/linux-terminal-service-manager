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

#ifdef __WIN32__
#include <windows.h>
#endif

#include <stdexcept>
#include <openssl/ssl.h>

#include <boost/asio/ssl/stream.hpp>

#include "ltsm_async_socket.h"

namespace LTSM {
//
using AsioSslContext = boost::asio::ssl::context;
using AsioSslStream = boost::asio::ssl::stream<AsioTcpSocket>;

namespace AsioTls {
enum class HandshakeType { Client, Srrver };

class AsyncStream : public AsyncSocket<AsioSslStream> {
    AsioSslContext ssl_ctx_;
    AsioSslStream ssl_sock_;
    bool ssl_connected_ = false;

  protected:
    AsioSslStream& socket(void) override { return ssl_sock_; }

  public:
    explicit AsyncStream(const boost::asio::any_io_executor& ex, const AsioSslContext::method& method)
        : ssl_ctx_{method}, ssl_sock_{AsioTcpSocket{ex}, ssl_ctx_} {}

    explicit AsyncStream(AsioTcpSocket&& sock, const AsioSslContext::method& method)
        : ssl_ctx_{method}, ssl_sock_{std::move(sock), ssl_ctx_} {}

    inline AsioSslContext& ssl_context(void) { return ssl_ctx_; }

    inline const AsioSslContext& ssl_context(void) const { return ssl_ctx_; }

    inline AsioSslStream& ssl_stream(void) { return ssl_sock_; }

    inline const AsioSslStream& ssl_stream(void) const { return ssl_sock_; }

    void sslHandshake(const HandshakeType& type) {
        ssl_stream().handshake(type == HandshakeType::Client ? boost::asio::ssl::stream_base::client
                                                             : boost::asio::ssl::stream_base::server);
        ssl_connected_ = true;
    }

    void closeSocket(void) override {
        if (auto& sock = ssl_stream().lowest_layer(); sock.is_open()) {
            boost::system::error_code ec;
            sock.cancel(ec);

            if (ssl_connected_) {
                ssl_stream().shutdown(ec);
            }

            sock.close(ec);
        }
    }

    void setCipherSuite(const char* list) {
        if (list) {
            auto ssl = ssl_stream().native_handle();
            SSL_set_cipher_list(ssl, list);
        }
    }
};
} // namespace AsioTls
} // namespace LTSM

#endif // _LTSM_BOOST_SOCKETS_
