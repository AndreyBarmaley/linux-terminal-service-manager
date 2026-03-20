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
struct has_data_size<T, std::void_t<
decltype(std::declval<T>().data()),
decltype(std::declval<T>().size())>> : std::true_type {};

template <typename T>
inline constexpr bool has_data_size_v = has_data_size<T>::value;

namespace LTSM {

    template<typename T>
    auto value_to_buffer(T & val) {
        using DecayedT = std::decay_t<T>;

        if constexpr(std::is_integral_v<DecayedT>) {
            return boost::asio::buffer(&val, sizeof(val));
        } else if constexpr(std::is_same_v<DecayedT, boost::asio::buffer>) {
                return val;
        } else if constexpr(has_data_size_v<DecayedT>) {
            if constexpr(sizeof(typename DecayedT::value_type) == 1) {
                return boost::asio::buffer(val.data(), val.size());
            } else {
                static_assert(always_false_v<T>, "invalid value type for has_data_size");
            }
        } else {
            static_assert(always_false_v<T>, "invalid type for asio::const_buffer");
        }
    }

    template<typename T>
    auto value_to_const_buffer(const T & val) {
        using DecayedT = std::decay_t<T>;

        if constexpr(std::is_integral_v<DecayedT>) {
            return boost::asio::const_buffer(&val, sizeof(val));
        } else if constexpr(std::is_same_v<DecayedT, boost::asio::const_buffer>) {
                return val;
        } else if constexpr(has_data_size_v<DecayedT>) {
            if constexpr(sizeof(typename DecayedT::value_type) == 1) {
                return boost::asio::const_buffer(val.data(), val.size());
            } else {
                static_assert(always_false_v<T>, "invalid value type for has_data_size");
            }
        } else {
            static_assert(always_false_v<T>, "invalid type for asio::const_buffer");
        }
    }

    template<typename Socket>
    class AsyncSocket {
        mutable Socket sock_;

      protected:
        template<typename T>
        [[nodiscard]] boost::asio::awaitable<T> async_recv(void) const {
            T val = 0;
            co_await boost::asio::async_read(sock_, boost::asio::buffer(&val, sizeof(T)),
                                             boost::asio::transfer_exactly(sizeof(T)), boost::asio::use_awaitable);
            co_return val;
        }

        template<typename T>
        [[nodiscard]] boost::asio::awaitable<void> async_send(const T & val) const {
            co_await boost::asio::async_write(sock_, boost::asio::const_buffer(&val, sizeof(T)),
                                              boost::asio::transfer_all(), boost::asio::use_awaitable);
        }

      public:
        explicit AsyncSocket(const boost::asio::any_io_executor & ex) : sock_{ex} {}
        explicit AsyncSocket(Socket && sock) : sock_{std::forward<Socket>(sock)} {}

        // RECV
        template<typename Ptr>
        [[nodiscard]] boost::asio::awaitable<void> async_recv_buf(Ptr ptr, size_t len) const {
            if(len) {
                co_await boost::asio::async_read(sock_, boost::asio::buffer(ptr, len),
                                                 boost::asio::transfer_exactly(len), boost::asio::use_awaitable);
            }

            co_return;
        }

        template<typename Buffer>
        [[nodiscard]] boost::asio::awaitable<Buffer> async_recv_buf(size_t len) const {
            Buffer buf;

            if(len) {
                co_await boost::asio::async_read(sock_, boost::asio::dynamic_buffer(buf),
                                                 boost::asio::transfer_exactly(len), boost::asio::use_awaitable);
            }

            co_return buf;
        }

        // const auto & [ val64, val16, str ] = co_await async_recv_values<uint64_t, uint16_t, std::string>({16});
        template <typename... Values>
        [[nodiscard]] boost::asio::awaitable<std::tuple<Values...>> async_recv_values(std::initializer_list<size_t> sizes) const {
            auto tuple = std::tuple<Values...>{};
            auto itsz = sizes.begin();
            std::apply([&](auto&... val) {
                ([&]() {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr(has_data_size_v<T>) {
//                        if(itsz != sizes.end()) {
//                            val.resize(*itsz++);
//                        }
                    }
                }, ...);
            }, tuple);
        
            boost::container::small_vector<boost::asio::mutable_buffer, sizeof...(Values)> buffers;
            std::apply([&](auto&... val) {
                (buffers.emplace_back(value_to_buffer(val)), ...);
            }, tuple);

/*

        if constexpr(std::is_integral_v<DecayedT>) {
            return boost::asio::buffer(&val, sizeof(val));
        } else if constexpr(std::is_same_v<DecayedT, boost::asio::buffer>) {
                return val;
        } else if constexpr(has_data_size_v<DecayedT>) {
            if constexpr(sizeof(typename DecayedT::value_type) == 1) {
                return boost::asio::buffer(val.data(), val.size());
            } else {
                static_assert(always_false_v<T>, "invalid value type for has_data_size");
            }
        } else {
            static_assert(always_false_v<T>, "invalid type for asio::const_buffer");
        }
*/

            co_await boost::asio::async_read(sock_, buffers,
                                              boost::asio::transfer_all(), boost::asio::use_awaitable);
            co_return tuple;
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
        template<typename Buffer>
        [[nodiscard]] boost::asio::awaitable<void> async_send_buf(const Buffer& buf) const {
            co_await boost::asio::async_write(sock_, buf,
                                              boost::asio::transfer_all(), boost::asio::use_awaitable);
        }

        template<typename Buffer>
        [[nodiscard]] boost::asio::awaitable<void> async_send_buf(Buffer&& buf) const {
            co_await boost::asio::async_write(sock_, std::forward<Buffer>(buf),
                                              boost::asio::transfer_all(), boost::asio::use_awaitable);
        }

        // async_send_values(val1, val2, ... valX)
        template <typename... Values>
        [[nodiscard]] boost::asio::awaitable<void> async_send_values(const Values&... vals) const {
            auto list = { value_to_const_buffer(vals)... };
            co_await boost::asio::async_write(sock_, list,
                                              boost::asio::transfer_all(), boost::asio::use_awaitable);
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

        const Socket & socket(void) const {
            return sock_;
        }

        Socket & socket(void) {
            return sock_;
        }
    };
}

#endif // _LTSM_ASYNC_SOCKET_
