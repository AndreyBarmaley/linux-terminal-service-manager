/***************************************************************************
 *   Copyright © 2025 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef _LTSM_PARALLELS_JOBS_
#define _LTSM_PARALLELS_JOBS_

#include <list>
#include <mutex>
#include <thread>
#include <chrono>
#include <future>

using namespace std::chrono_literals;

namespace LTSM {
    template <typename ReturnType>
    class AsyncJob {
        std::future<ReturnType> future_;

      public:
        template <typename Callable, typename... Args>
        explicit AsyncJob(Callable&& job, Args&&... args) {
            std::promise<ReturnType> promise;
            future_ = promise.get_future();

            std::thread thr(
            [p = std::move(promise)](auto&& func, auto&&... bound_args) mutable {
                try {
                    if constexpr(std::is_void_v<ReturnType>) {
                        std::invoke(std::forward<decltype(func)>(func), std::forward<decltype(bound_args)>(bound_args)...);
                        p.set_value();
                    } else {
                        p.set_value(std::invoke(std::forward<decltype(func)>(func), std::forward<decltype(bound_args)>(bound_args)...));
                    }
                } catch(...) {
                    p.set_exception(std::current_exception());
                }
            },
            std::forward<Callable>(job),
            std::forward<Args>(args)...
            );

            thr.detach();
        }

        ReturnType get(void) {
            return future_.get();
        }

        bool isReady(void) const {
            return future_.wait_for(1us) == std::future_status::ready;
        }

        ~AsyncJob() {
            if(future_.valid())
                future_.wait();
        }

        AsyncJob(const AsyncJob &) = delete;
        AsyncJob & operator=(const AsyncJob &) = delete;

        AsyncJob(AsyncJob &&) noexcept = default;
        AsyncJob & operator=(AsyncJob &&) noexcept = default;
    };

    template <typename Callable, typename... Args>
    auto make_async_job(Callable&& job, Args&&... args) {
        using ReturnType = std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>;
        return AsyncJob<ReturnType>(std::forward<Callable>(job), std::forward<Args>(args)...);
    }

    template<typename JobResult>
    class ParallelsJobs {
        using JobFuture = AsyncJob<JobResult>; //std::future<JobResult>;
        using JobList = std::list<JobFuture>;

        std::mutex mutex_;
        JobList jobs_;
        const int tnum_;

      public:
        explicit ParallelsJobs(int num = std::thread::hardware_concurrency()) : tnum_(num) {
        }

        ~ParallelsJobs() {
            clear();
        }

        void addJob(JobFuture && job) {
            std::unique_lock<std::mutex> lock{mutex_};

            while(! jobs_.empty()) {
                auto busy = std::ranges::count_if(jobs_, [](auto & job) {
                    return ! job.isReady();
                });

                if(busy < tnum_) {
                    break;
                }

                lock.unlock();
                std::this_thread::yield();
                lock.lock();
            }

            jobs_.emplace_back(std::move(job));
        }

        JobList & jobList(void) {
            return jobs_;
        }

        void clear(void) noexcept {
            std::unique_lock<std::mutex> lock{mutex_};
            jobs_.clear();
        }
    };
}

#endif
