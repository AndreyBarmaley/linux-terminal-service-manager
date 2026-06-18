// Google Gemini AI

#include <string_view>
#include <vector>
#include <string>

#include <gtest/gtest.h>

#include "spdlog/sinks/ostream_sink.h"
#include "ltsm_application.h"

using namespace LTSM;

class ApplicationLoggerTest : public ::testing::Test {
  protected:
    std::shared_ptr<std::ostringstream> oss;
    std::shared_ptr<spdlog::sinks::ostream_sink_mt> test_sink;

    void SetUp() override {
        oss = std::make_shared<std::ostringstream>();
        test_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*oss);

        auto default_logger = std::make_shared<spdlog::logger>("global_test", test_sink);
        default_logger->set_level(spdlog::level::trace);
        default_logger->set_pattern("%v");
        spdlog::set_default_logger(default_logger);

        auto xcb_logger = std::make_shared<spdlog::logger>("xcb", test_sink);
        xcb_logger->set_level(spdlog::level::trace);
        xcb_logger->set_pattern("%v");
        spdlog::register_logger(xcb_logger);
    }

    void TearDown() override {
        spdlog::shutdown();
    }
};

TEST_F(ApplicationLoggerTest, DebugLevelConfiguration) {
    Application::setDebugLevel(DebugLevel::Info);
    EXPECT_TRUE(Application::isDebugLevel(DebugLevel::Info));
    EXPECT_TRUE(Application::isDebugLevel(DebugLevel::Warn));
    EXPECT_FALSE(Application::isDebugLevel(DebugLevel::Debug));

    Application::setDebugLevel("trace");
    EXPECT_TRUE(Application::isDebugLevel(DebugLevel::Trace));
    EXPECT_TRUE(Application::isDebugLevel(DebugLevel::Crit));
}

TEST_F(ApplicationLoggerTest, DebugTargetAndTypes) {
    Application::setDebugTarget(DebugTarget::Console);
    EXPECT_TRUE(Application::isDebugTarget(DebugTarget::Console));
    EXPECT_FALSE(Application::isDebugTarget(DebugTarget::Syslog));

    std::list<std::string> enabled_types = {"Xcb", "Common"};
    Application::setDebugTypes(enabled_types);

    EXPECT_TRUE(Application::isDebugTypes(DebugType::Xcb));
    EXPECT_TRUE(Application::isDebugTypes(DebugType::Common));
    EXPECT_FALSE(Application::isDebugTypes(DebugType::Audio));
}

TEST_F(ApplicationLoggerTest, GlobalLoggingMethods) {
    oss->str("");

    Application::error("Failed to open connection: status={}", 500);
    EXPECT_EQ(oss->str(), "Failed to open connection: status=500\n");

    oss->str("");
    Application::warning("Deprecated feature used");
    EXPECT_EQ(oss->str(), "Deprecated feature used\n");

    oss->str("");
    Application::notice("System is rebooting");
    EXPECT_EQ(oss->str(), "System is rebooting\n");

    Application::setDebugLevel(DebugLevel::Info);
    oss->str("");
    Application::info("User {} logged in", "admin");
    EXPECT_EQ(oss->str(), "User admin logged in\n");

    Application::setDebugLevel(DebugLevel::Warn);
    oss->str("");
    Application::info("This should not be logged");
    EXPECT_TRUE(oss->str().empty());
}

TEST_F(ApplicationLoggerTest, SubsystemLoggingDebugAndTrace) {
    Application::setDebugLevel(DebugLevel::Debug);
    Application::setDebugTypes({"Xcb"});

    oss->str("");
    Application::debug(DebugType::Xcb, "XCB window initialized: ID={}", 0x123);
    
    EXPECT_NO_THROW(Application::debug(DebugType::Xcb, "test"));

    oss->str("");
    Application::debug(DebugType::Audio, "Audio track started");
    EXPECT_TRUE(oss->str().empty());

    Application::setDebugLevel(DebugLevel::Quiet);
    oss->str("");
    Application::trace(DebugType::Xcb, "Trace verbose data dump");
    EXPECT_TRUE(oss->str().empty());
}

TEST_F(ApplicationLoggerTest, FormattingErrorResilience) {
    Application::setDebugLevel(DebugLevel::Info);
    
    EXPECT_NO_THROW({
        Application::error("Missing argument {} and {}", "only_one");
        Application::info("Too many arguments {}", 1, 2, 3);
    });
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_application.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
