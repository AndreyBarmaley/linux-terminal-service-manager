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

#ifndef _CHANNEL_STREAM_
#define _CHANNEL_STREAM_

#include <atomic>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include "ltsm_async_stream.h"

namespace LTSM {
    template<typename AsioGeneric, StreamMode Mode = StreamMode::IReadWrite>
    class AsyncAsioGeneric : public AsyncStream<AsioGeneric, Mode> {
        AsioGeneric ao_;

      public:
        explicit AsyncAsioGeneric(const boost::asio::any_io_executor& ex) : ao_{ex} {}
        explicit AsyncAsioGeneric(AsioGeneric&& sf) : ao_{std::move(sf)} {}
        explicit AsyncAsioGeneric(AsyncAsioGeneric&&) noexcept = default;

        ~AsyncAsioGeneric() {
            close();
        }

        AsioGeneric& stream(void) final { return ao_; }

        bool isOpen(void) const final {
            return ao_.is_open();
        }

        void close(void) final {
            if(ao_.is_open()) {
                boost::system::error_code ec;
                ao_.cancel(ec);
                ao_.close(ec);
            }
        }
    };

#ifdef __UNIX__
    using AsyncStreamDescriptor = AsyncAsioGeneric<boost::asio::posix::stream_descriptor>;
#else
    using AsyncStreamDescriptor = AsyncAsioGeneric<boost::asio::windows::stream_handle>;
#endif
    using AsyncGenericSocket = AsyncAsioGeneric<boost::asio::generic::stream_protocol::socket>;
    using AsyncStreamFileReadable = AsyncAsioGeneric<boost::asio::stream_file, StreamMode::IRead>;
    using AsyncStreamFileWritable = AsyncAsioGeneric<boost::asio::stream_file, StreamMode::IWrite>;
    using AsyncReadablePipe = AsyncAsioGeneric<boost::asio::readable_pipe, StreamMode::IRead>;
    using AsyncWritablePipe = AsyncAsioGeneric<boost::asio::writable_pipe, StreamMode::IWrite>;

    template<typename Stream>
    class IStreamHandler : public std::enable_shared_from_this<IStreamHandler<Stream>> {
      protected:
        virtual Stream & stream(void) = 0;

      public:
        IStreamHandler() = default;
        virtual ~IStreamHandler() = default;
    };

    template<typename Stream>
    class IReaderStream : public virtual IStreamHandler<Stream> {
      private:
        boost::asio::cancellation_signal read_cancel_;

      protected:
        boost::asio::awaitable<void> readLoopAwait(void) {
            for(;;) {
                std::vector<uint8_t> buf(4096);
                auto len = co_await this->stream().async_read_some(boost::asio::buffer(buf));
                buf.resize(len);
                co_await this->readStreamHandler(std::move(buf));
            }
        }

      public:
        ~IReaderStream() override = default;

        virtual boost::asio::awaitable<void> readStreamHandler(std::vector<uint8_t>) = 0;
        virtual void readErrorHandler(std::exception_ptr) = 0;

        void stopReaderLoop(void) {
            boost::asio::dispatch(this->stream().get_executor(), [self = this->shared_from_this(), this]() {
                this->read_cancel_.emit(boost::asio::cancellation_type::terminal);
            });
        }

        void startReaderLoop(void) {
            auto ex = this->stream().get_executor();
            boost::asio::dispatch(ex, [self = this->shared_from_this(), this, ex]() {
                if(this->read_cancel_.slot().has_handler()) {
                    return;
                }

                boost::asio::co_spawn(ex, readLoopAwait(),
                    boost::asio::bind_cancellation_slot(this->read_cancel_.slot(),
                        [self, this](std::exception_ptr ep) {
                            this->readErrorHandler(ep);
                        }
                    )
                );
            });
        }
    };

    using StdBuffer = std::vector<uint8_t>;
    using TransferChannel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code, StdBuffer)>;
    constexpr int TransferChannelSize = 16;

    template<typename Stream>
    class IWriterStream : public virtual IStreamHandler<Stream> {
      private:
        boost::asio::cancellation_signal write_cancel_;
        std::atomic<uint32_t> write_jobs_{0};

      protected:
        virtual TransferChannel & transferChannel(void) = 0;

        boost::asio::awaitable<void> writeLoopAwait(void) {
            for(;;) {
                auto buf = co_await this->transferChannel().async_receive(boost::asio::use_awaitable);
                co_await this->writeStreamHandler(std::move(buf));
            }

            co_return;
        }

      public:
        ~IWriterStream() override = default;

        virtual boost::asio::awaitable<void> writeStreamHandler(std::vector<uint8_t>) = 0;
        virtual void writeErrorHandler(std::exception_ptr) = 0;
        virtual void writeJobsOverhead(uint32_t) {}

        void stopWriterLoop(void) {
            boost::asio::dispatch(this->stream().get_executor(), [self = this->shared_from_this(), this]() {
                this->write_cancel_.emit(boost::asio::cancellation_type::terminal);
            });
        }

        void startWriterLoop(void) {
            auto ex = this->stream().get_executor();
            boost::asio::dispatch(ex, [self = this->shared_from_this(), this, ex]() {
                if(this->write_cancel_.slot().has_handler()) {
                    return;
                }

                boost::asio::co_spawn(ex, writeLoopAwait(),
                    boost::asio::bind_cancellation_slot(this->write_cancel_.slot(),
                        [self, this](std::exception_ptr ep) {
                            this->writeErrorHandler(ep);
                        }
                    )
                );
            });
        }

        bool pushWriterData(std::vector<uint8_t> && buf) {
            if(! this->transferChannel().is_open()) {
                return false;
            }

            if(this->transferChannel().try_send(boost::system::error_code{}, std::move(buf))) {
                return true;
            }

            this->write_jobs_.fetch_add(1);
            // Inside the stream, we may have a read operation (a check is required)
            // we can’t block — we switch to a coroutine.
            boost::asio::co_spawn(this->stream().get_executor(),
                [self = this->shared_from_this(), this, data = std::move(buf)]() mutable -> boost::asio::awaitable<void> {
                    [[maybe_unused]] auto [ec] =
                        co_await this->transferChannel().async_send(boost::system::error_code{}, std::move(data),
                                                            boost::asio::as_tuple(boost::asio::use_awaitable));
                    this->write_jobs_.fetch_sub(1);
                }, boost::asio::detached
            );
            writeJobsOverhead(this->write_jobs_.load());

            return true;
        }
    };
} // LTSM

#endif // _CHANNEL_STREAM_
