/***********************************************************************
 *   Copyright © 2026 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_ASYNC_SDBUS_
#define _LTSM_ASYNC_SDBUS_

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <mutex>
#include <chrono>
#include <memory>

#include <sdbus-c++/sdbus-c++.h>

namespace LTSM::SDBus {
    using namespace boost;

    class AsioCoroConnector {
        std::unique_ptr<sdbus::IConnection> dbus_conn_;
        boost::asio::cancellation_signal sdbus_cancel_;

      protected:
        template<typename ExecutionContext>
        struct weak_stream_descriptor : asio::posix::stream_descriptor {
            weak_stream_descriptor(ExecutionContext & context, const asio::posix::stream_descriptor::native_handle_type & fd)
                : asio::posix::stream_descriptor(context, fd) {
            }

            ~weak_stream_descriptor() {
                release();
            }
        };

        [[nodiscard]] asio::awaitable<void> waitPollDataEventFd(const sdbus::IConnection::PollData & pollData) {
            auto ex = co_await asio::this_coro::executor;
            weak_stream_descriptor sd{ex, pollData.eventFd};

            co_await sd.async_wait(asio::posix::descriptor::wait_read,
                asio::use_awaitable);

            co_return;
        }

        [[nodiscard]] asio::awaitable<void> waitPollDataFd(const sdbus::IConnection::PollData & pollData) {
            auto ex = co_await asio::this_coro::executor;
            weak_stream_descriptor sd{ex, pollData.fd};

            if((pollData.events & POLLIN) && (pollData.events & POLLOUT)) {
                using namespace asio::experimental::awaitable_operators;
                co_await (
                    sd.async_wait(asio::posix::descriptor::wait_read,
                        asio::use_awaitable) ||
                    sd.async_wait(asio::posix::descriptor::wait_write,
                        asio::use_awaitable)
                );
            } else if(pollData.events & POLLIN) {
                co_await sd.async_wait(asio::posix::descriptor::wait_read,
                    asio::use_awaitable);
            } else if(pollData.events & POLLOUT) {
                co_await sd.async_wait(asio::posix::descriptor::wait_write,
                    asio::use_awaitable);
            }

            co_return;
        }

        [[nodiscard]] asio::awaitable<void> waitTimeout(uint32_t timeout_ms) {
            auto ex = co_await asio::this_coro::executor;
            asio::steady_timer tm{ex, std::chrono::milliseconds(timeout_ms)};
            co_await tm.async_wait(asio::use_awaitable);
            co_return;
        }

      public:
        AsioCoroConnector(std::unique_ptr<sdbus::IConnection> && ptr) : dbus_conn_{std::move(ptr)} {
        }

        ~AsioCoroConnector() {
            sdbus_cancel_.emit(asio::cancellation_type::terminal);
        }

        void sdbusLoopCancel(void) {
            sdbus_cancel_.emit(asio::cancellation_type::terminal);
        }

        [[nodiscard]] asio::awaitable<void> sdbusEventLoop(void) {
            try {
                auto ex = co_await asio::this_coro::executor;
                for(;;) {
                    co_await asio::co_spawn(ex, sdbusEventProcess(),
                        asio::bind_cancellation_slot(sdbus_cancel_.slot(), asio::use_awaitable));
                }
            } catch(const system::system_error& err) {
                if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                    throw err;
                }
            }

            co_return;
        }

        [[nodiscard]] asio::awaitable<void> sdbusEventProcess(void) {
            /*
                https://github.com/Kistler-Group/sdbus-cpp/blob/v2.2.0/docs/using-sdbus-c++.md#using-sdbus-c-in-external-event-loops
                -
                Before each invocation of the I/O polling call,
                    IConnection::getEventLoopPollData() function should be invoked.
                Returned PollData::fd file descriptor should be polled for the events indicated by PollData::events,
                    and the I/O call should block up to the returned PollData::timeout.
                Additionally, returned PollData::eventFd should be polled for POLLIN events.
                After each I/O polling call (for both PollData::fd and PollData::eventFd events),
                    the IConnection::processPendingEvent() method should be invoked.
                This enables the bus connection to process any incoming or outgoing D-Bus messages.
            */

            auto ex = co_await asio::this_coro::executor;
            auto pollData = dbus_conn_->getEventLoopPollData();

            using namespace asio::experimental::awaitable_operators;
            co_await (waitPollDataFd(pollData) || waitPollDataEventFd(pollData));

#ifdef SDBUS_2_0_API
            dbus_conn_->processPendingEvent();
#else
            dbus_conn_->processPendingRequest();
#endif
            if(auto timeout_ms = pollData.getPollTimeout(); 0 < timeout_ms) {
                co_await waitTimeout(timeout_ms);
            }

            co_return;
        }
    };
} // LTSM::SDbus

#endif // _LTSM_ASYNC_SDBUS_
