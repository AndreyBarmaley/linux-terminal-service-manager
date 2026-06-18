// Google Gemini AI

#include <future>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>

#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_parallels_jobs.h"

using namespace LTSM;

class ParallelsJobsTest : public ::testing::Test {
protected:
    ParallelsJobs<int> pool{2};
};

TEST_F(ParallelsJobsTest, AddAndGetResults) {
    auto f1 = std::async(std::launch::async, []() { return 10; });
    auto f2 = std::async(std::launch::async, []() { return 20; });

    pool.addJob(std::move(f1));
    pool.addJob(std::move(f2));

    auto& results = pool.waitAll();
    EXPECT_EQ(results.size(), 2);

    std::vector<int> values;
    for (auto& job : results) {
        values.push_back(job.get());
    }

    EXPECT_EQ(values.size(), 2);
    EXPECT_EQ(values[0], 10);
    EXPECT_EQ(values[1], 20);
}

TEST_F(ParallelsJobsTest, ThrottlingLimitsConcurrentJobs) {
    auto long_worker = []() {
        std::this_thread::sleep_for(50ms);
        return 0;
    };

    pool.addJob(std::async(std::launch::async, long_worker));
    pool.addJob(std::async(std::launch::async, long_worker));

    std::this_thread::sleep_for(5ms);

    auto busy_before = std::ranges::count_if(pool.jobList(), [](auto & job) {
        return job.wait_for(1us) != std::future_status::ready;
    });
    EXPECT_EQ(busy_before, 2);

    pool.addJob(std::async(std::launch::async, long_worker));
    pool.addJob(std::async(std::launch::async, long_worker));

    pool.waitAll();
    EXPECT_EQ(pool.jobList().size(), 4);
}

TEST_F(ParallelsJobsTest, ClearEmptiesTheList) {
    pool.addJob(std::async(std::launch::async, []() { return 1; }));
    pool.addJob(std::async(std::launch::async, []() { return 2; }));

    EXPECT_EQ(pool.jobList().size(), 2);
    pool.clear();

    EXPECT_TRUE(pool.jobList().empty());
}

TEST(ParallelsJobsDestructorTest, DestructorWaitsForRunningJobs) {
    std::atomic<bool> job_finished{false};

    {
        ParallelsJobs<void> void_pool{1};
        
        auto long_job = std::async(std::launch::async, [&]() {
            std::this_thread::sleep_for(30ms);
            job_finished = true;
        });

        void_pool.addJob(std::move(long_job));
    }

    EXPECT_TRUE(job_finished.load());
}

TEST_F(ParallelsJobsTest, HandlesExceptionsInFutures) {
    auto exception_job = std::async(std::launch::async, []() -> int {
        throw std::runtime_error("Job failed successfully");
    });

    pool.addJob(std::move(exception_job));
    pool.waitAll();

    auto& list = pool.jobList();
    ASSERT_EQ(list.size(), 1);

    EXPECT_THROW({
        list.front().get();
    }, std::runtime_error);
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_parallels.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
