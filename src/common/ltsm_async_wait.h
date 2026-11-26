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

#ifndef _LTSM_ASYNC_WAIT_
#define _LTSM_ASYNC_WAIT_

#include <boost/asio.hpp>
#include <boost/system/system_error.hpp>
#include <atomic>

namespace LTSM {
    class AsyncWait {
        struct State {
            boost::asio::steady_timer timer;
            bool is_set{false};
            bool is_cancelled{false};

            explicit State(const boost::asio::any_io_executor& ex)
                : timer(ex, boost::asio::steady_timer::time_point::max()) {}
        };

        std::shared_ptr<State> state_;

        static boost::asio::awaitable<void> wait_in_strand(std::shared_ptr<State> st) {
            while(st->is_set && ! st->is_cancelled) {
                auto [ec] = co_await st->timer.async_wait(
                                boost::asio::as_tuple(boost::asio::use_awaitable)
                            );

                if(st->is_cancelled) {
                    throw boost::system::system_error(boost::asio::error::operation_aborted);
                }

                if(ec && ec != boost::asio::error::operation_aborted) {
                    throw boost::system::system_error(ec);
                }
            }

            if(st->is_cancelled) {
                throw boost::system::system_error(boost::asio::error::operation_aborted);
            }
        }

      public:
        explicit AsyncWait(const boost::asio::strand<boost::asio::any_io_executor>& ex)
            : state_(std::make_shared<State>(ex)) {
        }

        void set(void) {
            boost::asio::dispatch(state_->timer.get_executor(), [st = state_]() {
                if(! st->is_cancelled) {
                    st->is_set = true;
                }
            });
        }

        void clear(void) {
            boost::asio::dispatch(state_->timer.get_executor(), [st = state_]() {
                st->is_set = false;
                st->timer.cancel();
            });
        }

        void cancel(void) {
            boost::asio::dispatch(state_->timer.get_executor(), [st = state_]() {
                st->is_cancelled = true;
                st->is_set = false;
                st->timer.cancel();
            });
        }

        [[nodiscard]] boost::asio::awaitable<void> wait(void) {
            auto caller_ex = co_await boost::asio::this_coro::executor;
            auto timer_ex = state_->timer.get_executor();

            if(caller_ex != timer_ex) {
                co_await boost::asio::co_spawn(
                    timer_ex,
                    wait_in_strand(state_),
                    boost::asio::use_awaitable);
            } else {
                co_await wait_in_strand(state_);
            }

            co_return;
        }
    };
} // namespace LTSM

#endif // _LTSM_ASYNC_WAIT_
