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

#ifndef _LTSM_ASYNC_STREAM_
#define _LTSM_ASYNC_STREAM_

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

class AsyncStreamBase {
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
    AsyncStreamBase() = default;
    virtual ~AsyncStreamBase() = default;

    virtual bool isOpen(void) const = 0;
    virtual void close(void) = 0;

    virtual boost::asio::any_io_executor get_executor(void) = 0;
    virtual void sync_recv_buf(void* ptr, size_t len) const = 0;
    virtual void sync_send_buf(const void* ptr, size_t len) const = 0;

    // RECV
    virtual boost::asio::awaitable<void> async_recv_buf(void* ptr, size_t len) const = 0;
    virtual boost::asio::awaitable<size_t> async_read_some(boost::asio::mutable_buffer buffer) const = 0;

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
    virtual boost::asio::awaitable<void> async_send_buffers(std::vector<boost::asio::const_buffer>&&) const = 0;

    inline void sync_send_buf(const boost::asio::const_buffer& buf) const {
        sync_send_buf(buf.data(), buf.size());
    }

    // async_send_values(val1, val2, ... valX)
    template <typename... Values>
    [[nodiscard]] boost::asio::awaitable<void> async_send_values(const Values&... vals) const {
        auto list = {value_to_const_buffer(vals)...};
        co_await async_send_buffers(list);
    }

    template <typename Iterator>
    [[nodiscard]] boost::asio::awaitable<void> async_send_sequence(Iterator beg, Iterator end) const {
        std::vector<boost::asio::const_buffer> buffers;
    
        for (auto it = beg; it != end; ++it) {
            buffers.push_back(value_to_const_buffer(*it));
        }
    
        co_await async_send_buffers(std::move(buffers));
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

enum class StreamMode { IRead  = 1, IWrite = 2, IReadWrite = 3 };

constexpr bool is_read_mode(StreamMode mode) {
    return static_cast<int>(mode) & static_cast<int>(StreamMode::IRead);
}

constexpr bool is_write_mode(StreamMode mode) {
    return static_cast<int>(mode) & static_cast<int>(StreamMode::IWrite);
}

template <typename Stream, StreamMode Mode = StreamMode::IReadWrite>
class AsyncStream : public AsyncStreamBase {
  public:
    AsyncStream() = default;
    virtual ~AsyncStream() = default;

    boost::asio::any_io_executor get_executor(void) final {
        return stream().get_executor();
    }

    void sync_recv_buf(void* ptr, size_t len) const final {
        if constexpr (is_read_mode(Mode)) {
            if (len) {
                boost::asio::read(const_cast<AsyncStream&>(*this).stream(), boost::asio::buffer(ptr, len),
                              boost::asio::transfer_all());
            }
        }
    }

    void sync_send_buf(const void* ptr, size_t len) const final {
        if constexpr (is_write_mode(Mode)) {
            if (len) {
                boost::asio::write(const_cast<AsyncStream&>(*this).stream(), boost::asio::buffer(ptr, len),
                              boost::asio::transfer_all());
            }
        }
    }

    template <typename Buffers>
    void sync_send_buffers(const Buffers& bufs) const {
        if constexpr (is_write_mode(Mode)) {
            boost::asio::write(const_cast<AsyncStream&>(*this).stream(), bufs,
                            boost::asio::transfer_all());
        }
    }

    [[nodiscard]] boost::asio::awaitable<size_t> async_read_some(boost::asio::mutable_buffer buffer) const final {
        if constexpr (is_read_mode(Mode)) {
            auto readsz = co_await const_cast<AsyncStream&>(*this).stream().async_read_some(buffer,
                                             boost::asio::use_awaitable);
            co_return readsz;
        } else {
            co_return 0;
        }
    }

    [[nodiscard]] boost::asio::awaitable<void> async_recv_buf(void* ptr, size_t len) const final {
        if constexpr (is_read_mode(Mode)) {
            if (len) {
                co_await boost::asio::async_read(const_cast<AsyncStream&>(*this).stream(), boost::asio::buffer(ptr, len),
                                             boost::asio::transfer_exactly(len), boost::asio::use_awaitable);
            }
        }
        co_return;
    }

    [[nodiscard]] boost::asio::awaitable<void> async_recv_buffers(
        std::initializer_list<boost::asio::mutable_buffer> list) const final {
        if constexpr (is_read_mode(Mode)) {
            co_await boost::asio::async_read(const_cast<AsyncStream&>(*this).stream(), list, boost::asio::transfer_all(), boost::asio::use_awaitable);
        }
        co_return;
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_buf(const boost::asio::const_buffer& buf) const final {
        if constexpr (is_write_mode(Mode)) {
            co_await boost::asio::async_write(const_cast<AsyncStream&>(*this).stream(), buf, boost::asio::transfer_all(), boost::asio::use_awaitable);
        }
        co_return;
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_buffers(
        std::vector<boost::asio::const_buffer>&& buffers) const final {
        if constexpr (is_write_mode(Mode)) {
            co_await boost::asio::async_write(const_cast<AsyncStream&>(*this).stream(), std::move(buffers), boost::asio::transfer_all(), boost::asio::use_awaitable);
        }
        co_return;
    }

    [[nodiscard]] boost::asio::awaitable<void> async_send_buffers(
        std::initializer_list<boost::asio::const_buffer> list) const final {
        if constexpr (is_write_mode(Mode)) {
            co_await boost::asio::async_write(const_cast<AsyncStream&>(*this).stream(), list, boost::asio::transfer_all(), boost::asio::use_awaitable);
        }
        co_return;
    }

    template <typename Buffers>
    [[nodiscard]] boost::asio::awaitable<void> async_send_buffers(const Buffers& bufs) const {
        if constexpr (is_write_mode(Mode)) {
            co_await boost::asio::async_write(const_cast<AsyncStream&>(*this).stream(), bufs, boost::asio::transfer_all(), boost::asio::use_awaitable);
        }
        co_return;
    }

    virtual Stream& stream(void) = 0;
};
} // namespace LTSM

#endif // _LTSM_ASYNC_STREAM_
