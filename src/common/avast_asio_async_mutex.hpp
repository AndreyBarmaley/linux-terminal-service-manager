// https://github.com/avast/asio-mutex
//
// SPDX-FileCopyrightText: 2023 Daniel Vrátil <daniel.vratil@gendigital.com>
// SPDX-FileCopyrightText: 2023 Martin Beran <martin.beran@gendigital.com>
//
// SPDX-License-Identifier: BSL-1.0

// adopted to boost-1.78

#pragma once

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <cassert>

#define ASIO_NS boost::asio

namespace avast::asio {

    class async_mutex_lock;
    class async_mutex;

    namespace detail {

        struct locked_waiter {
            explicit locked_waiter(locked_waiter *next_waiter) : next(next_waiter) {}
            virtual ~locked_waiter() = default;

            // Запрет копирования
            locked_waiter(const locked_waiter &) = delete;
            locked_waiter & operator=(const locked_waiter &) = delete;

            virtual void completion() = 0;
            locked_waiter* next = nullptr;
        };

        template <typename Handler>
        struct async_locked_waiter final : public locked_waiter {
            async_locked_waiter(async_mutex*, locked_waiter *next_waiter, Handler&& handler)
                : locked_waiter(next_waiter), m_handler(std::move(handler)) {}

            void completion() override {
                auto executor = ASIO_NS::get_associated_executor(m_handler);
                ASIO_NS::post(std::move(executor), [handler = std::move(m_handler)]() mutable {
                    handler();
                });
            }

          private:
            Handler m_handler;
        };

        template <typename Handler>
        struct scoped_async_locked_waiter final : public locked_waiter {
            scoped_async_locked_waiter(async_mutex *mutex, locked_waiter *next_waiter, Handler&& handler)
                : locked_waiter(next_waiter), m_mutex(mutex), m_handler(std::move(handler)) {}

            void completion() override;

          private:
            async_mutex* m_mutex;
            Handler m_handler;
        };

        template <template <typename> class WaiterType>
        class async_lock_initiator {
          public:
            explicit async_lock_initiator(async_mutex *mutex) : m_mutex(mutex) {}

            template <typename Handler>
            void operator()(Handler&& handler);

          private:
            async_mutex* m_mutex;
        };

    }

    class async_mutex {
      public:
        async_mutex() noexcept = default;
        ~async_mutex() {
            assert(m_state.load() == not_locked || m_state.load() == locked_no_waiters);
            assert(m_waiters == nullptr);
        }

        bool try_lock() noexcept {
            std::uintptr_t old_state = not_locked;
            return m_state.compare_exchange_strong(old_state, locked_no_waiters,
                                                   std::memory_order_acquire, std::memory_order_relaxed);
        }

        template <typename LockToken>
        auto async_lock(LockToken&& token) {
            return ASIO_NS::async_initiate<LockToken, void()>(
                       detail::async_lock_initiator<detail::async_locked_waiter>(this), token);
        }

        template <typename LockToken>
        auto async_scoped_lock(LockToken&& token) {
            return ASIO_NS::async_initiate<LockToken, void(async_mutex_lock)>(
                       detail::async_lock_initiator<detail::scoped_async_locked_waiter>(this), token);
        }

        void unlock() {
            auto* waiters_head = m_waiters;

            if(waiters_head == nullptr) {
                std::uintptr_t old_state = locked_no_waiters;

                if(m_state.compare_exchange_strong(old_state, not_locked,
                                                   std::memory_order_release, std::memory_order_relaxed)) {
                    return;
                }

                old_state = m_state.exchange(locked_no_waiters, std::memory_order_acquire);
                auto* next = reinterpret_cast<detail::locked_waiter*>(old_state);

                while(next != nullptr) {
                    auto* temp = next->next;
                    next->next = waiters_head;
                    waiters_head = next;
                    next = temp;
                }
            }

            if(waiters_head) {
                m_waiters = waiters_head->next;
                waiters_head->completion();
                delete waiters_head;
            }
        }

      private:
        template <template <typename> class> friend class detail::async_lock_initiator;

        static constexpr std::uintptr_t not_locked = 1;
        static constexpr std::uintptr_t locked_no_waiters = 0;
        std::atomic<std::uintptr_t> m_state{not_locked};
        detail::locked_waiter* m_waiters = nullptr;
    };

    class async_mutex_lock {
      public:
        explicit async_mutex_lock() noexcept = default;
        explicit async_mutex_lock(async_mutex& mutex, std::adopt_lock_t) noexcept : m_mutex(&mutex) {}

        async_mutex_lock(async_mutex_lock&& other) noexcept : m_mutex(std::exchange(other.m_mutex, nullptr)) {}
        async_mutex_lock & operator=(async_mutex_lock&& other) noexcept {
            if(m_mutex) {
                m_mutex->unlock();
            }

            m_mutex = std::exchange(other.m_mutex, nullptr);
            return *this;
        }

        ~async_mutex_lock() {
            if(m_mutex) {
                m_mutex->unlock();
            }
        }

        bool owns_lock() const noexcept {
            return m_mutex != nullptr;
        }

      private:
        async_mutex* m_mutex = nullptr;
    };

    namespace detail {

        template <typename Handler>
        void scoped_async_locked_waiter<Handler>::completion() {
            auto executor = ASIO_NS::get_associated_executor(m_handler);
            ASIO_NS::post(std::move(executor), [h = std::move(m_handler), m = m_mutex]() mutable {
                h(async_mutex_lock{*m, std::adopt_lock});
            });
        }

        template <template <typename> class WaiterType>
        template <typename Handler>
        void async_lock_initiator<WaiterType>::operator()(Handler&& handler) {
            auto old_state = m_mutex->m_state.load(std::memory_order_acquire);
            std::unique_ptr<WaiterType<Handler>> waiter;

            while(true) {
                if(old_state == async_mutex::not_locked) {
                    if(m_mutex->m_state.compare_exchange_weak(old_state, async_mutex::locked_no_waiters,
                            std::memory_order_acquire, std::memory_order_relaxed)) {
                        WaiterType<Handler>(m_mutex, nullptr, std::forward<Handler>(handler)).completion();
                        return;
                    }
                } else {
                    if(! waiter) {
                        waiter.reset(new WaiterType<Handler>(m_mutex, reinterpret_cast<locked_waiter*>(old_state), std::forward<Handler>(handler)));
                    } else {
                        waiter->next = reinterpret_cast<locked_waiter*>(old_state);
                    }

                    if(m_mutex->m_state.compare_exchange_weak(old_state, reinterpret_cast<std::uintptr_t>(waiter.get()),
                            std::memory_order_release, std::memory_order_relaxed)) {
                        waiter.release();
                        return;
                    }
                }
            }
        }

    } // namespace detail
} // namespace avast::asio
