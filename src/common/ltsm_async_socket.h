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

#include <tuple>
#include <string>
#include <vector>
#include <cinttypes>
#include <type_traits>
#include <initializer_list>

#include <boost/asio.hpp>
#include <boost/endian.hpp>
#include <boost/container/small_vector.hpp>

template <typename T>
inline constexpr bool always_false_v = false;

template <typename T, typename = void>
struct has_data_size : std::false_type {};

template <typename T>
struct has_data_size<T, std::void_t<decltype(std::declval<T>().data()), decltype(std::declval<T>().size())>>
    : std::true_type {};

template <typename T>
inline constexpr bool has_data_size_v = has_data_size<T>::value;

template <typename T>
inline constexpr bool is_mutable_buffer_v = boost::asio::is_mutable_buffer_sequence<T>::value;

template <typename T>
inline constexpr bool is_const_buffer_v = boost::asio::is_const_buffer_sequence<T>::value;

namespace LTSM {

template <typename T>
boost::asio::mutable_buffer
value_to_buffer(T&& val) {
    using DecayedT = std::decay_t<T>;

    if constexpr (std::is_integral_v<DecayedT>) {
        return boost::asio::buffer(&val, sizeof(val));
    } else if constexpr (is_mutable_buffer_v<DecayedT>) {
        return std::forward<T>(val);
    } else if constexpr (has_data_size_v<DecayedT>) {
        if constexpr (sizeof(typename DecayedT::value_type) == 1) {
            return boost::asio::buffer(val.data(), val.size());
        } else {
            static_assert(always_false_v<T>, "invalid value type for has_data_size");
        }
    } else {
        static_assert(always_false_v<T>, "invalid type for asio::const_buffer");
    }
}

template <typename T>
boost::asio::const_buffer
value_to_const_buffer(const T& val) {
    using DecayedT = std::decay_t<T>;

    if constexpr (std::is_integral_v<DecayedT>) {
        return boost::asio::const_buffer(&val, sizeof(val));
    } else if constexpr (is_const_buffer_v<DecayedT>) {
        return val;
    } else if constexpr (has_data_size_v<DecayedT>) {
        if constexpr (sizeof(typename DecayedT::value_type) == 1) {
            return boost::asio::const_buffer(val.data(), val.size());
        } else {
            static_assert(always_false_v<T>, "invalid value type for has_data_size");
        }
    } else {
        static_assert(always_false_v<T>, "invalid type for asio::const_buffer");
    }
}

class AsyncSocketBase {
  protected:
    template <typename T>
    [[nodiscard]] boost::asio::awaitable<T> async_recv(void) const {
        T val = 0;
        co_await async_recv_buf(&val, sizeof(T));
        co_return val;
    }

    template <typename T>
    [[nodiscard]] boost::asio::awaitable<void> async_send(const T& val) const {
        co_await async_send_buf(boost::asio::const_buffer(&val, sizeof(T)));
    }

  public:
    AsyncSocketBase() = default;
    virtual ~AsyncSocketBase() = default;

    virtual void closeSocket(void) = 0;

    virtual void sync_recv_buf(void* ptr, size_t len) const = 0;
    virtual void sync_send_buf(const void* ptr, size_t len) const = 0;

    // RECV
    virtual boost::asio::awaitable<void> async_recv_buf(void* ptr, size_t len) const = 0;
    virtual boost::asio::awaitable<void> async_recv_buffers(
        std::initializer_list<boost::asio::mutable_buffer>) const = 0;

    boost::asio::awaitable<std::string> async_recv_string(size_t len) const {
        std::string res(len, 0);
        co_await async_recv_buf(res.data(), res.size());
        co_return res;
    }

    using binary_buf = std::vector<uint8_t>;

    boost::asio::awaitable<binary_buf> async_recv_buffer(size_t len) const {
        binary_buf res(len, 0);
        co_await async_recv_buf(res.data(), res.size());
        co_return res;
    }

    // async_read_values(val1&, val2&, ... valX&)
    template <typename... Values>
    [[nodiscard]] boost::asio::awaitable<void> async_recv_values(Values&&... vals) const {
        auto list = {value_to_buffer(std::forward<Values>(vals))...};
        co_await async_recv_buffers(list);
    }

    [[nodiscard]] boost::asio::awaitable<uint8_t> async_recv_byte(void) const {
        co_return co_await async_recv<uint8_t>();
    }

    [[nodiscard]] boost::asio::awaitable<uint16_t> async_recv_le16(void) const {
        auto val = co_await async_recv<uint16_t>();
        co_return boost::endian::little_to_native(val);
    }

    [[nodiscard]] boost::asio::awaitable<uint32_t> async_recv_le32(void) const {
        auto val = co_await async_recv<uint32_t>();
        co_return boost::endian::little_to_native(val);
    }

    [[nodiscard]] boost::asio::awaitable<uint64_t> async_recv_le64(void) const {
        auto val = co_await async_recv<uint64_t>();
        co_return boost::endian::little_to_native(val);
    }

    [[nodiscard]] boost::asio::awaitable<uint16_t> async_recv_be16(void) const {
        auto val = co_await async_recv<uint16_t>();
        co_return boost::endian::big_to_native(val);
    }

    [[nodiscard]] boost::asio::awaitable<uint32_t> async_recv_be32(void) const {
        auto val = co_await async_recv<uint32_t>();
        co_return boost::endian::big_to_native(val);
    }

    [[nodiscard]] boost::asio::awaitable<uint64_t> async_recv_be64(void) const {
        auto val = co_await async_recv<uint64_t>();
        co_return boost::endian::big_to_native(val);
    }

    // SEND
    virtual boost::asio::awaitable<void> async_send_buf(const boost::asio::const_buffer& buf) const = 0;
    virtual boost::asio::awaitable<void> async_send_buffers(std::initializer_list<boost::asio::const_buffer>) const = 0;

    // async_send_values(val1, val2, ... valX)
    template <typename... Values>
    [[nodiscard]] boost::asio::awaitable<void> async_send_values(const Values&... vals) const {
        auto list = {value_to_const_buffer(vals)...};
        co_await async_send_buffers(list);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_byte(uint8_t val) const {
        co_await async_send<uint8_t>(val);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_le16(uint16_t val) const {
        boost::endian::native_to_little_inplace(val);
        co_await async_send<uint16_t>(val);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_le32(uint32_t val) const {
        boost::endian::native_to_little_inplace(val);
        co_await async_send<uint32_t>(val);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_le64(uint64_t val) const {
        boost::endian::native_to_little_inplace(val);
        co_await async_send<uint64_t>(val);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_be16(uint16_t val) const {
        boost::endian::native_to_big_inplace(val);
        co_await async_send<uint16_t>(val);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_be32(uint32_t val) const {
        boost::endian::native_to_big_inplace(val);
        co_await async_send<uint32_t>(val);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_be64(uint64_t val) const {
        boost::endian::native_to_big_inplace(val);
        co_await async_send<uint64_t>(val);
    }
};

template <typename Socket>
class AsyncSocket : public AsyncSocketBase {
  public:
    AsyncSocket() = default;
    virtual ~AsyncSocket() = default;

    void sync_recv_buf(void* ptr, size_t len) const final {
        if (len) {
            boost::asio::read(socket(), boost::asio::buffer(ptr, len),
                              boost::asio::transfer_all());
        }
    }

    void sync_send_buf(const void* ptr, size_t len) const final {
        if (len) {
            boost::asio::write(socket(), boost::asio::buffer(ptr, len),
                              boost::asio::transfer_all());
        }
    }

    [[nodiscard]] boost::asio::awaitable<void> async_recv_buf(void* ptr, size_t len) const final {
        if (len) {
            co_await boost::asio::async_read(socket(), boost::asio::buffer(ptr, len),
                                             boost::asio::transfer_exactly(len), boost::asio::use_awaitable);
        }
        co_return;
    }

    [[nodiscard]] boost::asio::awaitable<void> async_recv_buffers(
        std::initializer_list<boost::asio::mutable_buffer> list) const final {
        co_await boost::asio::async_read(socket(), list, boost::asio::transfer_all(), boost::asio::use_awaitable);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_buf(const boost::asio::const_buffer& buf) const final {
        co_await boost::asio::async_write(socket(), buf, boost::asio::transfer_all(), boost::asio::use_awaitable);
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_buffers(
        std::initializer_list<boost::asio::const_buffer> list) const final {
        co_await boost::asio::async_write(socket(), list, boost::asio::transfer_all(), boost::asio::use_awaitable);
    }

    template <typename Buffers>
    [[nodiscard]] boost::asio::awaitable<void> async_send_buffers(const Buffers& bufs) const {
        co_await boost::asio::async_write(socket(), bufs, boost::asio::transfer_all(), boost::asio::use_awaitable);
    }

    virtual Socket& socket(void) const = 0;
};

using AsioTcpSocket = boost::asio::ip::tcp::socket;

class AsyncTcpStream : public AsyncSocket<AsioTcpSocket> {
    mutable AsioTcpSocket sock_;

  public:
    explicit AsyncTcpStream(const boost::asio::any_io_executor& ex) : sock_{ex} {}
    explicit AsyncTcpStream(AsioTcpSocket&& sock) : sock_{std::move(sock)} {}

    AsioTcpSocket& socket(void) const override { return sock_; }

    void closeSocket(void) override {
        if (sock_.is_open()) {
            boost::system::error_code ec;
            sock_.cancel(ec);
            sock_.close(ec);
        }
    }
};

#if defined(__APPLE__) || defined(__UNIX__)
using AsioLocalSocket = boost::asio::local::stream_protocol::socket;

class AsyncLocalStream : public AsyncSocket<AsioLocalSocket> {
    mutable AsioLocalSocket sock_;

  public:
    explicit AsyncLocalStream(const boost::asio::any_io_executor& ex) : sock_{ex} {}
    explicit AsyncLocalStream(AsioLocalSocket&& sock) : sock_{std::move(sock)} {}

    AsioLocalSocket& socket(void) const override { return sock_; }

    void closeSocket(void) override {
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
