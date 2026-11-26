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
#include <optional>

#include <boost/asio/experimental/concurrent_channel.hpp>
#include "ltsm_async_stream.h"

#if BOOST_VERSION >= 108700
#include <boost/process/process.hpp>
#include <boost/process/stdio.hpp>
namespace bp_v2 = boost::process;
#else
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
namespace bp_v2 = boost::process::v2;
#endif

namespace LTSM {
    using AsyncStreamFileReadable = AsyncStream<boost::asio::stream_file, StreamMode::IRead>;
    using AsyncStreamFileWritable = AsyncStream<boost::asio::stream_file, StreamMode::IWrite>;
    using AsyncReadablePipe       = AsyncStream<boost::asio::readable_pipe, StreamMode::IRead>;
    using AsyncWritablePipe       = AsyncStream<boost::asio::writable_pipe, StreamMode::IWrite>;

    class BasicProcessExec {
        bp_v2::process proc_;

      protected:
        void stopping();

      public:
        BasicProcessExec(boost::asio::any_io_executor ex,
                         const std::string& cmd,
                         const std::vector<std::string> & args,
                         bp_v2::process_stdio&& redirect)
            : proc_{std::move(ex), cmd, args, std::move(redirect)} {}

        BasicProcessExec(BasicProcessExec &&) noexcept = default;
        BasicProcessExec & operator=(BasicProcessExec &&) noexcept = default;

        ~BasicProcessExec() {
            stopping();
        }
    };

    class AsyncReadablePipeCmd : public AsyncReadablePipe, public BasicProcessExec {
      public:
        AsyncReadablePipeCmd(boost::asio::any_io_executor ex, const std::string& cmd, const std::vector<std::string> & args)
            : AsyncReadablePipe(boost::asio::readable_pipe{ex}),
              BasicProcessExec(ex, cmd, args, bp_v2::process_stdio{nullptr, AsyncReadablePipe::stream(), nullptr}) {}
    };

    class AsyncWritablePipeCmd : public AsyncWritablePipe, public BasicProcessExec {
      public:
        AsyncWritablePipeCmd(boost::asio::any_io_executor ex, const std::string& cmd, const std::vector<std::string> & args)
            : AsyncWritablePipe(boost::asio::writable_pipe{ex}),
              BasicProcessExec(ex, cmd, args, bp_v2::process_stdio{AsyncWritablePipe::stream(), nullptr, nullptr}) {}
    };

    /// IStreamHandler - basic stream handler interface (common base for Reader/Writer)
    template <typename Stream>
    class IStreamHandler : public std::enable_shared_from_this<IStreamHandler<Stream>> {
      protected:
        [[nodiscard]] virtual Stream & stream() = 0;

      public:
        IStreamHandler() = default;
        virtual ~IStreamHandler() = default;
    };

    /// IReaderStream - asynchronous read loop from the stream
    template <typename Stream>
    class IReaderStream : public virtual IStreamHandler<Stream> {
      private:
        boost::asio::cancellation_signal read_cancel_;
        static constexpr std::size_t ReadChunkSize = 4096;

      protected:
        [[nodiscard]] static boost::asio::awaitable<void> readLoopAwait(std::shared_ptr<IReaderStream> self) {
            std::array<uint8_t, ReadChunkSize> scratch{};

            for(;;) {
                auto len = co_await self->stream().async_read_some(boost::asio::buffer(scratch));

                if(len == 0) {
                    continue;
                }

                std::vector<uint8_t> payload(scratch.begin(), scratch.begin() + len);
                co_await self->readStreamHandler(std::move(payload));
            }
        }
        
      public:
        ~IReaderStream() override = default;

        [[nodiscard]] virtual boost::asio::awaitable<void> readStreamHandler(std::vector<uint8_t>) = 0;
        virtual void readErrorHandler(std::exception_ptr) = 0;

        void stopReaderLoop(bool close_stream = false) {
            auto ex = this->stream().get_executor();
            auto self = std::shared_ptr<IReaderStream>(this->shared_from_this(), this);

            boost::asio::dispatch(ex, [self = std::move(self), close_stream]() {
                self->read_cancel_.emit(boost::asio::cancellation_type::terminal);
                if(close_stream) {
                    self->stream().close();
                }
            });
        }

        void startReaderLoop() {
            auto ex = this->stream().get_executor();
            auto self = std::shared_ptr<IReaderStream>(this->shared_from_this(), this);

            boost::asio::dispatch(ex, [self = std::move(self), this, ex]() {
                if(this->read_cancel_.slot().has_handler()) {
                    return;
                }

                boost::asio::co_spawn(ex, readLoopAwait(self),
                                      boost::asio::bind_cancellation_slot(this->read_cancel_.slot(),
                [self](std::exception_ptr ep) {
                    self->readErrorHandler(ep);
                }));
            });
        }
    };

    using StdBuffer = std::vector<uint8_t>;
    using TransferChannel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code, StdBuffer)>;

    // Buffer for 64 packets (~256Kb with 4Kb chunks) to smooth out spikes without spawning detached coroutines
    constexpr std::size_t TransferChannelSize = 64;

    /// IWriterStream - Asynchronous write loop to the stream via a bounded queue
    template <typename Stream>
    class IWriterStream : public virtual IStreamHandler<Stream> {
      private:
        boost::asio::cancellation_signal write_cancel_;
        std::atomic<uint32_t> write_jobs_{0};

      protected:
        [[nodiscard]] virtual TransferChannel & transferChannel() = 0;

        [[nodiscard]] static boost::asio::awaitable<void> writeLoopAwait(std::shared_ptr<IWriterStream> self) {
            for(;;) {
                auto buf = co_await self->transferChannel().async_receive(boost::asio::use_awaitable);
                co_await self->writeStreamHandler(std::move(buf));
            }
        }

      public:
        ~IWriterStream() override = default;

        [[nodiscard]] virtual boost::asio::awaitable<void> writeStreamHandler(std::vector<uint8_t>) = 0;
        virtual void writeErrorHandler(std::exception_ptr) = 0;
        virtual void writeQueueOverflow(uint32_t /*max jobs*/) {}

        void stopWriterLoop(bool close_stream = false) {
            auto ex = this->stream().get_executor();
            auto self = std::shared_ptr<IWriterStream>(this->shared_from_this(), this);

            boost::asio::dispatch(ex, [self = std::move(self), close_stream]() {
                self->write_cancel_.emit(boost::asio::cancellation_type::terminal);
                self->transferChannel().cancel();
                self->transferChannel().close();
                if(close_stream) {
                    self->stream().close();
                }
            });
        }

        void startWriterLoop() {
            auto ex = this->stream().get_executor();
            auto self = std::shared_ptr<IWriterStream>(this->shared_from_this(), this);

            boost::asio::dispatch(ex, [self = std::move(self), this, ex]() {
                if(this->write_cancel_.slot().has_handler()) {
                    return;
                }

                boost::asio::co_spawn(ex, writeLoopAwait(self),
                                      boost::asio::bind_cancellation_slot(this->write_cancel_.slot(),
                [self](std::exception_ptr ep) {
                    self->writeErrorHandler(ep);
                }));
            });
        }

        bool pushWriterData(std::vector<uint8_t> && buf) {
            auto& ch = this->transferChannel();

            if(! ch.is_open()) {
                return false;
            }

            if(this->write_jobs_.load() == 0 &&
                ch.try_send(boost::system::error_code{}, std::move(buf))) {
                return true;
            }

            this->write_jobs_.fetch_add(1);
            // Inside the stream, we may have a read operation (a check is required)
            // we can’t block — we switch to a coroutine.
            auto self = std::shared_ptr<IWriterStream>(this->shared_from_this(), this);
            boost::asio::co_spawn(this->stream().get_executor(),
                [self = std::move(self), data = std::move(buf)]() mutable -> boost::asio::awaitable<void> {
                    [[maybe_unused]] auto [ec] =
                        co_await self->transferChannel().async_send(boost::system::error_code{}, std::move(data),
                                                            boost::asio::as_tuple(boost::asio::use_awaitable));
                    self->write_jobs_.fetch_sub(1);
                }, boost::asio::detached
            );
            writeQueueOverflow(this->write_jobs_.load());

            return true;
        }
    };
} // LTSM

#endif // _CHANNEL_STREAM_
