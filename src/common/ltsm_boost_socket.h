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

#include <boost/utility/base_from_member.hpp>
#include <boost/asio/ssl/stream.hpp>

#include "ltsm_sockets.h"
#include "ltsm_async_socket.h"

namespace LTSM {
    //
    using AsioSocket = boost::asio::ip::tcp::socket;
    using AsioSslStream = boost::asio::ssl::stream<AsioSocket>;
    using AsioSslContext = boost::asio::ssl::context;

    namespace AsioTls {
        enum class HandshakeType { Client, Srrver };

        class AsyncStream : protected boost::base_from_member<AsioSslContext>, public AsyncSocket<AsioSslStream>, public NetworkStream {
            bool ssl_connected = false;

          public:
            explicit AsyncStream(const boost::asio::any_io_executor & ex, const AsioSslContext::method & method)
                : boost::base_from_member<AsioSslContext>(AsioSslContext{method})
                , AsyncSocket<AsioSslStream>(AsioSslStream{AsioSocket{ex}, boost::base_from_member<AsioSslContext>::member}) {}

            explicit AsyncStream(AsioSocket && sock, const AsioSslContext::method & method)
                : boost::base_from_member<AsioSslContext>(AsioSslContext{method})
                , AsyncSocket<AsioSslStream>(AsioSslStream{std::move(sock), boost::base_from_member<AsioSslContext>::member}) {}

            inline AsioSslContext & ssl_context(void) {
                return boost::base_from_member<boost::asio::ssl::context>::member;
            }

            inline const AsioSslContext & ssl_context(void) const {
                return boost::base_from_member<boost::asio::ssl::context>::member;
            }

            inline AsioSslStream & ssl_stream(void) {
                return AsyncSocket<AsioSslStream>::socket();
            }

            inline const AsioSslStream & ssl_stream(void) const {
                return AsyncSocket<AsioSslStream>::socket();
            }

            void sslHandshake(const HandshakeType & type) {
                ssl_stream().handshake(type == HandshakeType::Client ?
                                       boost::asio::ssl::stream_base::client : boost::asio::ssl::stream_base::server);
                ssl_connected = true;
            }

            void closeSocket(void) {
                boost::system::error_code ec;

                if(ssl_connected) {
                    ssl_stream().shutdown(ec);
                }

                ssl_stream().lowest_layer().close(ec);
            }

            void setCipherSuite(const char* list) {
                if(list) {
                    auto ssl = ssl_stream().native_handle();
                    SSL_set_cipher_list(ssl, list);
                }
            }

            bool hasInput(void) const override {
                if(ssl_connected) {
                    if(auto ssl = const_cast<AsioSslStream &>(ssl_stream()).native_handle()) {
                        return 0 < SSL_pending(ssl) ||
                               0 < ssl_stream().lowest_layer().available();
                    }

                    return false;
                }

                return 0 < ssl_stream().lowest_layer().available();
            }

            size_t hasData(void) const override {
                if(ssl_connected) {
                    auto ssl = const_cast<AsioSslStream &>(ssl_stream()).native_handle();
                    return ssl ? SSL_pending(ssl) : 0;
                }

                return ssl_stream().lowest_layer().available();
            }

            void sendRaw(const void* ptr, size_t len) override {
                if(ssl_connected) {
                    boost::asio::write(ssl_stream(), boost::asio::const_buffer(ptr, len), boost::asio::transfer_all());
                } else {
                    boost::asio::write(ssl_stream().next_layer(), boost::asio::const_buffer(ptr, len), boost::asio::transfer_all());
                }

                NetworkStream::bytesOut += len;
            }

            void recvRaw(void* ptr, size_t len) const override {
                if(ssl_connected) {
                    boost::asio::read(const_cast<AsioSslStream &>(ssl_stream()), boost::asio::buffer(ptr, len), boost::asio::transfer_all());
                } else {
                    boost::asio::read(const_cast<AsioSslStream &>(ssl_stream()).next_layer(), boost::asio::buffer(ptr, len), boost::asio::transfer_all());
                }

                NetworkStream::bytesIn += len;
            }
        };
    } // AsioTls
} // LTSM

#endif // _LTSM_BOOST_SOCKETS_
