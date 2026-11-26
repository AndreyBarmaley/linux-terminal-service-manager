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

/*
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
*/

namespace LTSM {

    template <typename T>
    concept IntegralValue = std::integral<std::remove_cvref_t<T>>;

    template <typename T>
    concept ByteContainer = requires(T c) {
        {
            c.data()
        };
        {
            c.size()
        }
        -> std::convertible_to<std::size_t>;
        requires sizeof(typename std::remove_cvref_t<T>::value_type) == 1;
    };

    template <typename T>
    concept MutableAsioBuffer = boost::asio::is_mutable_buffer_sequence<std::remove_cvref_t<T>>::value;

    template <typename T>
    concept ConstAsioBuffer = boost::asio::is_const_buffer_sequence<std::remove_cvref_t<T>>::value;

    // value_to_buffer overloads: we strictly accept lvalues to avoid dangling pointers
    template <IntegralValue T>
    [[nodiscard]] boost::asio::mutable_buffer value_to_buffer(T& val) noexcept {
        static_assert(! std::is_const_v<T>, "Cannot read into const integral value");
        return boost::asio::buffer(&val, sizeof(val));
    }

    template <MutableAsioBuffer T>
    [[nodiscard]] boost::asio::mutable_buffer value_to_buffer(T val) noexcept {
        return val;
    }

    template <ByteContainer T>
    requires(! MutableAsioBuffer<T>)
    [[nodiscard]] boost::asio::mutable_buffer value_to_buffer(T& val) noexcept {
        static_assert(! std::is_const_v<T>, "Cannot read into const container");
        return boost::asio::buffer(val.data(), val.size());
    }

    // prohibit passing rvalues (temporary objects)
    template <typename T>
    void value_to_buffer(const T &&) = delete;

    template <IntegralValue T>
    [[nodiscard]] boost::asio::const_buffer value_to_const_buffer(const T& val) noexcept {
        return boost::asio::const_buffer(&val, sizeof(val));
    }

    template <ConstAsioBuffer T>
    [[nodiscard]] boost::asio::const_buffer value_to_const_buffer(T val) noexcept {
        return val;
    }

    template <ByteContainer T>
    requires(! ConstAsioBuffer<T>)
    [[nodiscard]] boost::asio::const_buffer value_to_const_buffer(const T& val) noexcept {
        return boost::asio::const_buffer(val.data(), val.size());
    }

    // prohibit passing temporary values for sending (otherwise the buffer will die before co_await)
    template <IntegralValue T>
    void value_to_const_buffer(const T &&) = delete;

    template <ByteContainer T>
    requires(! ConstAsioBuffer<T> && ! std::is_same_v<std::remove_cvref_t<T>, std::string_view>)
    void value_to_const_buffer(const T &&) = delete;

    class AsyncStreamBase {
      protected:
        template <IntegralValue T>
        [[nodiscard]] boost::asio::awaitable<T> async_recv() {
            T val{};
            co_await async_recv_buf(&val, sizeof(T));
            co_return val;
        }

        template <IntegralValue T>
        [[nodiscard]] boost::asio::awaitable<void> async_send(T val) {
            co_await async_send_buf(boost::asio::const_buffer(&val, sizeof(T)));
        }

      public:
        using binary_buf = std::vector<uint8_t>;

        AsyncStreamBase() = default;
        virtual ~AsyncStreamBase() = default;

        AsyncStreamBase(const AsyncStreamBase &) = delete;
        AsyncStreamBase & operator=(const AsyncStreamBase &) = delete;
        AsyncStreamBase(AsyncStreamBase &&) noexcept = default;
        AsyncStreamBase & operator=(AsyncStreamBase &&) noexcept = default;

        [[nodiscard]] virtual bool isOpen() const = 0;
        virtual void close() = 0;
        [[nodiscard]] virtual boost::asio::any_io_executor get_executor() = 0;

        virtual void sync_recv_buf(void* ptr, std::size_t len) = 0;
        virtual void sync_send_buf(const void* ptr, std::size_t len) = 0;
        virtual void sync_send_buffers(std::span<const boost::asio::const_buffer> buffers) = 0;

        inline void sync_send_buf(const boost::asio::const_buffer& buf) {
            sync_send_buf(buf.data(), buf.size());
        }

        [[nodiscard]] virtual boost::asio::awaitable<void> async_recv_buf(void* ptr, std::size_t len) = 0;
        [[nodiscard]] virtual boost::asio::awaitable<std::size_t> async_read_some(boost::asio::mutable_buffer buffer) = 0;
        [[nodiscard]] virtual boost::asio::awaitable<void> async_recv_buffers(std::span<boost::asio::mutable_buffer> buffers) = 0;

        [[nodiscard]] boost::asio::awaitable<std::string> async_recv_string(std::size_t len) {
            std::string res(len, '\0');
            co_await async_recv_buf(res.data(), res.size());
            co_return res;
        }

        [[nodiscard]] boost::asio::awaitable<binary_buf> async_recv_buffer(std::size_t len) {
            binary_buf res(len);
            co_await async_recv_buf(res.data(), res.size());
            co_return res;
        }

        template <typename... Values>
        requires(sizeof...(Values) > 0)
        [[nodiscard]] boost::asio::awaitable<void> async_recv_values(Values&... vals) {
            std::array<boost::asio::mutable_buffer, sizeof...(Values)> list{value_to_buffer(vals)...};
            co_await async_recv_buffers(list);
        }

        [[nodiscard]] boost::asio::awaitable<uint8_t> async_recv_byte() {
            co_return co_await async_recv<uint8_t>();
        }

        [[nodiscard]] boost::asio::awaitable<uint16_t> async_recv_le16() {
            co_return boost::endian::little_to_native(co_await async_recv<uint16_t>());
        }

        [[nodiscard]] boost::asio::awaitable<uint32_t> async_recv_le32() {
            co_return boost::endian::little_to_native(co_await async_recv<uint32_t>());
        }

        [[nodiscard]] boost::asio::awaitable<uint64_t> async_recv_le64() {
            co_return boost::endian::little_to_native(co_await async_recv<uint64_t>());
        }

        [[nodiscard]] boost::asio::awaitable<uint16_t> async_recv_be16() {
            co_return boost::endian::big_to_native(co_await async_recv<uint16_t>());
        }

        [[nodiscard]] boost::asio::awaitable<uint32_t> async_recv_be32() {
            co_return boost::endian::big_to_native(co_await async_recv<uint32_t>());
        }

        [[nodiscard]] boost::asio::awaitable<uint64_t> async_recv_be64() {
            co_return boost::endian::big_to_native(co_await async_recv<uint64_t>());
        }

        [[nodiscard]] virtual boost::asio::awaitable<void> async_send_buf(const boost::asio::const_buffer& buf) = 0;
        [[nodiscard]] virtual boost::asio::awaitable<void> async_send_buffers(std::span<const boost::asio::const_buffer> buffers) = 0;

        template <typename... Values>
        requires(sizeof...(Values) > 0)
        [[nodiscard]] boost::asio::awaitable<void> async_send_values(const Values&... vals) {
            std::array<boost::asio::const_buffer, sizeof...(Values)> list{value_to_const_buffer(vals)...};
            co_await async_send_buffers(list);
        }

        template <std::input_iterator Iterator, std::sentinel_for<Iterator> Sentinel>
        [[nodiscard]] boost::asio::awaitable<void> async_send_sequence(Iterator beg, Sentinel end) {
            std::vector<boost::asio::const_buffer> buffers;

            if constexpr(std::sized_sentinel_for<Sentinel, Iterator>) {
                buffers.reserve(static_cast<std::size_t>(std::distance(beg, end)));
            }

            for(auto it = beg; it != end; ++it) {
                buffers.push_back(value_to_const_buffer(*it));
            }

            co_await async_send_buffers(buffers);
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_byte(uint8_t val) {
            co_await async_send<uint8_t>(val);
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_le16(uint16_t val) {
            co_await async_send<uint16_t>(boost::endian::native_to_little(val));
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_le32(uint32_t val) {
            co_await async_send<uint32_t>(boost::endian::native_to_little(val));
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_le64(uint64_t val) {
            co_await async_send<uint64_t>(boost::endian::native_to_little(val));
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_be16(uint16_t val) {
            co_await async_send<uint16_t>(boost::endian::native_to_big(val));
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_be32(uint32_t val) {
            co_await async_send<uint32_t>(boost::endian::native_to_big(val));
        }

        [[nodiscard]] boost::asio::awaitable<void> async_send_be64(uint64_t val) {
            co_await async_send<uint64_t>(boost::endian::native_to_big(val));
        }
    };
    enum class StreamMode : uint8_t {
        IRead = 1 << 0,
        IWrite = 1 << 1,
        IReadWrite = IRead | IWrite
    };
    constexpr bool is_read_mode(StreamMode mode) noexcept {
        return (static_cast<uint8_t>(mode) & static_cast<uint8_t>(StreamMode::IRead)) != 0;
    }
    constexpr bool is_write_mode(StreamMode mode) noexcept {
        return (static_cast<uint8_t>(mode) & static_cast<uint8_t>(StreamMode::IWrite)) != 0;
    }

    // A single template wrapper over any Asio stream/socket without an extra inheritance layer
    template <typename Stream, StreamMode Mode = StreamMode::IReadWrite>
    class AsyncStream : public AsyncStreamBase {
        Stream stream_;
      public:
        template <typename... Args>
        requires std::constructible_from<Stream, Args...>
        explicit AsyncStream(Args&&... args) : stream_{std::forward<Args>(args)...} {}
        ~AsyncStream() override {
            close();
        }
        AsyncStream(AsyncStream &&) noexcept = default;
        AsyncStream & operator=(AsyncStream &&) noexcept = default;
        [[nodiscard]] Stream & stream() noexcept {
            return stream_;
        }
        [[nodiscard]] const Stream & stream() const noexcept {
            return stream_;
        }
        [[nodiscard]] bool isOpen() const override {
            if constexpr(requires { stream_.is_open(); }) {
                return stream_.is_open();
            } else {
                return stream_.lowest_layer().is_open();
            }
        }
        void close() override {
            boost::system::error_code ec;

            if constexpr(requires { stream_.is_open(); }) {
                stream_.cancel(ec);
                stream_.close(ec);
            } else {
                stream_.lowest_layer().cancel(ec);
                stream_.lowest_layer().close(ec);
            }
        }
        [[nodiscard]] boost::asio::any_io_executor get_executor() override {
            return stream_.get_executor();
        }
        void sync_recv_buf(void* ptr, std::size_t len) override {
            if constexpr(! is_read_mode(Mode)) {
                throw std::logic_error("AsyncStream: read operation on write-only stream");
            } else if(len > 0) {
                boost::asio::read(stream_, boost::asio::buffer(ptr, len), boost::asio::transfer_all());
            }
        }
        void sync_send_buf(const void* ptr, std::size_t len) override {
            if constexpr(! is_write_mode(Mode)) {
                throw std::logic_error("AsyncStream: write operation on read-only stream");
            } else if(len > 0) {
                boost::asio::write(stream_, boost::asio::buffer(ptr, len), boost::asio::transfer_all());
            }
        }
        void sync_send_buffers(std::span<const boost::asio::const_buffer> buffers) override {
            if constexpr(! is_write_mode(Mode)) {
                throw std::logic_error("AsyncStream: sync_send_buffers on read-only stream");
            } else if(! buffers.empty()) {
                boost::asio::write(stream_, buffers, boost::asio::transfer_all());
            }
        }
        [[nodiscard]] boost::asio::awaitable<std::size_t> async_read_some(boost::asio::mutable_buffer buffer) override {
            if constexpr(! is_read_mode(Mode)) {
                throw std::logic_error("AsyncStream: async_read_some on write-only stream");
            } else {
                co_return co_await stream_.async_read_some(buffer, boost::asio::use_awaitable);
            }
        }
        [[nodiscard]] boost::asio::awaitable<void> async_recv_buf(void* ptr, std::size_t len) override {
            if constexpr(! is_read_mode(Mode)) {
                throw std::logic_error("AsyncStream: async_recv_buf on write-only stream");
            } else if(len > 0) {
                co_await boost::asio::async_read(stream_, boost::asio::buffer(ptr, len),
                                                 boost::asio::transfer_exactly(len), boost::asio::use_awaitable);
            }

            co_return;
        }
        [[nodiscard]] boost::asio::awaitable<void> async_recv_buffers(std::span<boost::asio::mutable_buffer> buffers) override {
            if constexpr(! is_read_mode(Mode)) {
                throw std::logic_error("AsyncStream: async_recv_buffers on write-only stream");
            } else if(! buffers.empty()) {
                co_await boost::asio::async_read(stream_, buffers, boost::asio::transfer_all(), boost::asio::use_awaitable);
            }

            co_return;
        }
        [[nodiscard]] boost::asio::awaitable<void> async_send_buf(const boost::asio::const_buffer& buf) override {
            if constexpr(! is_write_mode(Mode)) {
                throw std::logic_error("AsyncStream: async_send_buf on read-only stream");
            } else if(buf.size() > 0) {
                co_await boost::asio::async_write(stream_, buf, boost::asio::transfer_all(), boost::asio::use_awaitable);
            }

            co_return;
        }
        [[nodiscard]] boost::asio::awaitable<void> async_send_buffers(std::span<const boost::asio::const_buffer> buffers) override {
            if constexpr(! is_write_mode(Mode)) {
                throw std::logic_error("AsyncStream: async_send_buffers on read-only stream");
            } else if(! buffers.empty()) {
                co_await boost::asio::async_write(stream_, buffers, boost::asio::transfer_all(), boost::asio::use_awaitable);
            }

            co_return;
        }
    };

#ifdef __UNIX__
    using AsyncStreamDescriptor   = AsyncStream<boost::asio::posix::stream_descriptor>;
#else
    using AsyncStreamDescriptor   = AsyncStream<boost::asio::windows::stream_handle>;
#endif

    using AsyncTcpStream          = AsyncStream<boost::asio::ip::tcp::socket>;
    using AsyncLocalStream        = AsyncStream<boost::asio::local::stream_protocol::socket>;
    using AsyncGenericSocket      = AsyncStream<boost::asio::generic::stream_protocol::socket>;
} // namespace LTSM

#endif // _LTSM_ASYNC_STREAM_
