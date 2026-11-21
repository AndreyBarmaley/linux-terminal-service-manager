/***********************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_ASYNC_SOCKET_
#define _LTSM_ASYNC_SOCKET_

#include "ltsm_async_stream.h"

namespace LTSM {

using AsioTcpSocket = boost::asio::ip::tcp::socket;

class AsyncTcpStream : public AsyncStream<AsioTcpSocket> {
    AsioTcpSocket sock_;

  public:
    explicit AsyncTcpStream(const boost::asio::any_io_executor& ex) : sock_{ex} {}
    explicit AsyncTcpStream(AsioTcpSocket&& sock) : sock_{std::move(sock)} {}
    explicit AsyncTcpStream(AsyncTcpStream&&) noexcept = default;

    ~AsyncTcpStream() {
        close();
    }

    AsioTcpSocket& stream(void) final { return sock_; }

    size_t sync_recv_available(void) const {
        return sock_.available();
    }

    bool isOpen(void) const final {
        return sock_.is_open();
    }

    void close(void) final {
        if (sock_.is_open()) {
            boost::system::error_code ec;
            sock_.cancel(ec);
            sock_.close(ec);
        }
    }
};

#if defined(__APPLE__) || defined(__UNIX__)
using AsioLocalSocket = boost::asio::local::stream_protocol::socket;

class AsyncLocalStream : public AsyncStream<AsioLocalSocket> {
    AsioLocalSocket sock_;

  public:
    explicit AsyncLocalStream(const boost::asio::any_io_executor& ex) : sock_{ex} {}
    explicit AsyncLocalStream(AsioLocalSocket&& sock) : sock_{std::move(sock)} {}
    explicit AsyncLocalStream(AsyncLocalStream&&) noexcept = default;

    ~AsyncLocalStream(){
        close();
    }

    AsioLocalSocket& stream(void) final { return sock_; }

    size_t sync_recv_available(void) const {
        return sock_.available();
    }

    bool isOpen(void) const final {
        return sock_.is_open();
    }

    void close(void) final {
        if (sock_.is_open()) {
            boost::system::error_code ec;
            sock_.cancel(ec);
            sock_.close(ec);
        }
    }
};
#endif
} // namespace LTSM

#endif // _LTSM_ASYNC_SOCKET_
