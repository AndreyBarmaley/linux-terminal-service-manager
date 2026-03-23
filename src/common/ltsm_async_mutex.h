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

#ifndef _LTSM_ASYNC_MUTEX_
#define _LTSM_ASYNC_MUTEX_

#include <boost/asio.hpp>
#include <atomic>

namespace LTSM {

    class async_mutex {
        boost::asio::steady_timer timer_;
        std::atomic<uint32_t> queue_{0};

      public:
        explicit async_mutex(const boost::asio::any_io_executor & ex) : timer_(ex) {
            timer_.expires_at(boost::asio::steady_timer::time_point::max());
        }

        boost::asio::awaitable<void> async_lock(void) {
            if(0 < queue_.fetch_add(1, std::memory_order_acquire)) {
                //  we are not the first here, we need to wait on the timer...
                try {
                    co_await timer_.async_wait(boost::asio::use_awaitable);
                } catch(const boost::system::system_error& err) {
                    if(err.code() != boost::asio::error::operation_aborted) {
                        throw;
                    }
                }
            }
        }

        void unlock(void) {
            if(1 < queue_.fetch_sub(1, std::memory_order_release)) {
                // there's someone else waiting, let's skip one
                timer_.cancel_one();
            }
        }
    };
}

#endif //LTSM_ASYNC_MUTEX_
