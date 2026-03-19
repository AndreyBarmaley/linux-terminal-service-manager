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

#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <boost/endian.hpp>

namespace LTSM {

    template<typename Socket>
    class AsyncSocket {
        Socket sock_;

      protected:
        template<typename T>
        [[nodiscard]] boost::asio::awaitable<T> async_recv_le(void) {
            T val = 0;
            co_await boost::asio::async_read(sock_, boost::asio::buffer(&val, sizeof(T)),
                                             boost::asio::transfer_exactly(sizeof(T)), boost::asio::use_awaitable);
            co_return boost::endian::little_to_native(val);
        }

        template<typename T>
        [[nodiscard]] boost::asio::awaitable<T> async_recv_be(void) {
            T val = 0;
            co_await boost::asio::async_read(sock_, boost::asio::buffer(&val, sizeof(T)),
                                             boost::asio::transfer_exactly(sizeof(T)), boost::asio::use_awaitable);
            co_return boost::endian::big_to_native(val);
        }

        template<typename T>
        [[nodiscard]] boost::asio::awaitable<void> async_send_le(T val) {
            boost::endian::native_to_little_inplace(val);
            co_await boost::asio::async_write(sock_, boost::asio::buffer(&val, sizeof(T)),
                                              boost::asio::transfer_all(), boost::asio::use_awaitable);
        }

        template<typename T>
        [[nodiscard]] boost::asio::awaitable<void> async_send_be(T val) {
            boost::endian::native_to_big_inplace(val);
            co_await boost::asio::async_write(sock_, boost::asio::buffer(&val, sizeof(T)),
                                              boost::asio::transfer_all(), boost::asio::use_awaitable);
        }

      public:
        AsyncSocket(const boost::asio::any_io_executor & ex) : sock_{ex} {}
        AsyncSocket(Socket && sock) : sock_{std::forward<Socket>(sock)} {}

        template<typename Ptr>
        [[nodiscard]] boost::asio::awaitable<void> async_recv_buf(Ptr ptr, size_t len) {
            if(len) {
                co_await boost::asio::async_read(sock_, boost::asio::buffer(ptr, len),
                                                 boost::asio::transfer_exactly(len), boost::asio::use_awaitable);
            }

            co_return;
        }

        template<typename Buffer>
        [[nodiscard]] boost::asio::awaitable<Buffer> async_recv_buf(size_t len) {
            Buffer buf;

            if(len) {
                co_await boost::asio::async_read(sock_, boost::asio::dynamic_buffer(buf),
                                                 boost::asio::transfer_exactly(len), boost::asio::use_awaitable);
            }

            co_return buf;
        }

        template<typename Buffer>
        [[nodiscard]] boost::asio::awaitable<void> async_send_buf(Buffer&& buf) {
            co_await boost::asio::async_write(sock_, std::forward<Buffer>(buf),
                                                  boost::asio::transfer_all(), boost::asio::use_awaitable);
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_byte(uint8_t val) {
            co_await boost::asio::async_write(sock_, boost::asio::buffer(&val, 1),
                                              boost::asio::transfer_all(), boost::asio::use_awaitable);
        }

        [[nodiscard]] boost::asio::awaitable<uint8_t> async_recv_byte(void) {
            uint8_t val;
            co_await boost::asio::async_read(sock_, boost::asio::buffer(&val, 1),
                                             boost::asio::transfer_exactly(1), boost::asio::use_awaitable);
            co_return val;
        }

        [[nodiscard]] boost::asio::awaitable<uint16_t> async_recv_le16(void) {
            co_return co_await async_recv_le<uint16_t>();
        }

        [[nodiscard]] boost::asio::awaitable<uint32_t> async_recv_le32(void) {
            co_return co_await async_recv_le<uint32_t>();
        }

        [[nodiscard]] boost::asio::awaitable<uint64_t> async_recv_le64(void) {
            co_return co_await async_recv_le<uint64_t>();
        }

        [[nodiscard]] boost::asio::awaitable<uint16_t> async_recv_be16(void) {
            co_return co_await async_recv_be<uint16_t>();
        }

        [[nodiscard]] boost::asio::awaitable<uint32_t> async_recv_be32(void) {
            co_return co_await async_recv_be<uint32_t>();
        }

        [[nodiscard]] boost::asio::awaitable<uint64_t> async_recv_be64(void) {
            co_return co_await async_recv_be<uint64_t>();
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_le16(uint16_t val) {
            co_await async_send_le<uint16_t>(val);
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_le32(uint32_t val) {
            co_await async_send_le<uint32_t>(val);
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_le64(uint64_t val) {
            co_await async_send_le<uint64_t>(val);
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_be16(uint16_t val) {
            co_await async_send_be<uint16_t>(val);
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_be32(uint32_t val) {
            co_await async_send_be<uint32_t>(val);
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_be64(uint64_t val) {
            co_await async_send_be<uint64_t>(val);
        }

        const Socket & socket(void) const {
            return sock_;
        }

        Socket & socket(void) {
            return sock_;
        }
    };
}

#endif // _LTSM_ASYNC_SOCKET_
