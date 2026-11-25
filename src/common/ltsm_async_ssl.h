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

#ifndef _LTSM_ASYNC_SSL_
#define _LTSM_ASYNC_SSL_

#ifdef __WIN32__
#include <windows.h>
#endif

#include <stdexcept>
#include <openssl/ssl.h>

#include <boost/asio/ssl/stream.hpp>
#include "ltsm_async_stream.h"

namespace LTSM {
//

using SslContext = boost::asio::ssl::context;
using SslStream = boost::asio::ssl::stream<boost::asio::generic::stream_protocol::socket>;

// ssl_ctx_ create  before AsyncStream<SslStream>
// and destroy after close sock in ~AsyncStream.
struct AsyncSslContextHolder {
    SslContext ssl_ctx_;

    explicit AsyncSslContextHolder(SslContext::method method)
        : ssl_ctx_{method} {}
};

class AsyncSslStream : private AsyncSslContextHolder, public AsyncStream<SslStream> {
    bool ssl_connected_ = false;

  public:
    enum class HandshakeType { Client, Server };

    AsyncSslStream(const boost::asio::any_io_executor& ex, SslContext::method method)
        : AsyncSslContextHolder{method},
          AsyncStream<SslStream>{boost::asio::generic::stream_protocol::socket{ex}, ssl_ctx_} {}

    AsyncSslStream(boost::asio::generic::stream_protocol::socket&& sock, SslContext::method method)
        : AsyncSslContextHolder{method},
          AsyncStream<SslStream>{std::move(sock), ssl_ctx_} {}

    AsyncSslStream(AsyncSslStream&&) = delete;
    AsyncSslStream& operator=(AsyncSslStream&&) = delete;

    ~AsyncSslStream() override {
        close();
    }

    [[nodiscard]] std::size_t sync_recv_available() const {
        return ssl_stream().lowest_layer().available();
    }

    [[nodiscard]] bool isOpen() const override {
        return ssl_stream().lowest_layer().is_open();
    }

    [[nodiscard]] SslContext& ssl_context() noexcept { return ssl_ctx_; }
    [[nodiscard]] const SslContext& ssl_context() const noexcept { return ssl_ctx_; }

    [[nodiscard]] SslStream& ssl_stream() noexcept { return stream(); }
    [[nodiscard]] const SslStream& ssl_stream() const noexcept { return stream(); }

    void sslHandshake(HandshakeType type) {
        ssl_stream().handshake(
            type == HandshakeType::Client ? boost::asio::ssl::stream_base::client
                                          : boost::asio::ssl::stream_base::server
        );
        ssl_connected_ = true;
    }

    [[nodiscard]] boost::asio::awaitable<void> asyncSslHandshake(HandshakeType type) {
        co_await ssl_stream().async_handshake(
            type == HandshakeType::Client ? boost::asio::ssl::stream_base::client
                                          : boost::asio::ssl::stream_base::server,
            boost::asio::use_awaitable
        );
        ssl_connected_ = true;
    }

    void close() override {
        if (auto& sock = ssl_stream().lowest_layer(); sock.is_open()) {
            boost::system::error_code ec;
            sock.cancel(ec);

            if (ssl_connected_) {
                ssl_connected_ = false;
                ssl_stream().shutdown(ec);
            }

            sock.close(ec);
        }
    }

    void setCipherSuite(const char* list) {
        if (list != nullptr) {
            auto* ssl = ssl_stream().native_handle();
            SSL_set_cipher_list(ssl, list);
        }
    }
};
} // namespace LTSM

#endif // _LTSM_ASYNC_SSL_
