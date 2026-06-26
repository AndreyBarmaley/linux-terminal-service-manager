// Google Gemini AI

#include <vector>
#include <chrono>
#include <atomic>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_async_mutex.h"

using namespace LTSM;

class AsyncMutexTest : public ::testing::Test {
protected:
    boost::asio::io_context ctx;
};

TEST_F(AsyncMutexTest, SingleLockUnlock) {
    bool executed = false;

    boost::asio::co_spawn(ctx, [&]() -> boost::asio::awaitable<void> {
        async_mutex mutex(ctx.get_executor());

        co_await mutex.async_lock();
        executed = true;
        mutex.unlock();

        co_return;
    }, boost::asio::detached);

    ctx.run();
    EXPECT_TRUE(executed);
}

TEST_F(AsyncMutexTest, MutualExclusion) {
    async_mutex mutex(ctx.get_executor());
    int shared_resource = 0;
    std::vector<int> execution_order;

    auto worker = [&](int id) -> boost::asio::awaitable<void> {
        co_await mutex.async_lock();
        
        execution_order.push_back(id);
        shared_resource++;
        
        boost::asio::steady_timer transient_timer(ctx.get_executor(), std::chrono::milliseconds(10));
        co_await transient_timer.async_wait(boost::asio::use_awaitable);
        
        shared_resource--;
        execution_order.push_back(id);
        
        mutex.unlock();
        co_return;
    };

    boost::asio::co_spawn(ctx, worker(1), boost::asio::detached);
    boost::asio::co_spawn(ctx, worker(2), boost::asio::detached);

    ctx.run();

    EXPECT_EQ(shared_resource, 0);
    
    ASSERT_EQ(execution_order.size(), 4);
    EXPECT_EQ(execution_order[0], execution_order[1]);
    EXPECT_EQ(execution_order[2], execution_order[3]);
}

TEST_F(AsyncMutexTest, FifoOrdering) {
    async_mutex mutex(ctx.get_executor());
    std::vector<int> order;

    boost::asio::co_spawn(ctx, [&]() -> boost::asio::awaitable<void> {
        co_await mutex.async_lock();
        
        for (int i = 2; i <= 4; ++i) {
            boost::asio::co_spawn(ctx, [&, id = i]() -> boost::asio::awaitable<void> {
                co_await mutex.async_lock();
                order.push_back(id);
                mutex.unlock();
                co_return;
            }, boost::asio::detached);
            
            boost::asio::steady_timer t(ctx.get_executor(), std::chrono::milliseconds(5));
            co_await t.async_wait(boost::asio::use_awaitable);
        }

        order.push_back(1);
        mutex.unlock();
        co_return;
    }, boost::asio::detached);

    ctx.run();

    std::vector<int> expected = {1, 2, 3, 4};
    EXPECT_EQ(order, expected);
}

TEST_F(AsyncMutexTest, CancelAbortsWaitingCoroutines) {
    async_mutex mutex(ctx.get_executor());
    std::atomic<int> aborted_count{0};
    std::atomic<bool> first_locked{false};

    boost::asio::co_spawn(ctx, [&]() -> boost::asio::awaitable<void> {
        co_await mutex.async_lock();
        first_locked = true;
        co_return;
    }, boost::asio::detached);

    auto waiting_worker = [&]() -> boost::asio::awaitable<void> {
        co_await mutex.async_lock();
        aborted_count--; 
        mutex.unlock();
        co_return;
    };

    boost::asio::co_spawn(ctx, waiting_worker(), boost::asio::detached);
    boost::asio::co_spawn(ctx, waiting_worker(), boost::asio::detached);

    boost::asio::co_spawn(ctx, [&]() -> boost::asio::awaitable<void> {
        while (!first_locked) {
            boost::asio::steady_timer t(ctx.get_executor(), std::chrono::milliseconds(1));
            co_await t.async_wait(boost::asio::use_awaitable);
        }
        
        mutex.cancel();
        aborted_count += 2;
        co_return;
    }, boost::asio::detached);

    ctx.run();
    EXPECT_EQ(aborted_count.load(), 2);
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_asyncmutex.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
