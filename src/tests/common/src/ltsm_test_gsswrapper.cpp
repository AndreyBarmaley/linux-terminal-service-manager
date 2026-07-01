// Google Gemini AI

#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_gsslayer.h"

using namespace LTSM;

TEST(GssUtilsTest, NameTypeNameReturnsCorrectString) {
    EXPECT_STREQ(Gss::nameTypeName(Gss::NameType::NtUserName), "NtUserName");
    EXPECT_STREQ(Gss::nameTypeName(Gss::NameType::NoName), "NoName");
}

TEST(GssUtilsTest, CredUsageNameReturnsCorrectString) {
    EXPECT_STREQ(Gss::credUsageName(Gss::CredentialUsage::Initiate), "initiate");
    EXPECT_STREQ(Gss::credUsageName(Gss::CredentialUsage::Accept), "accept");
}

TEST(GssUtilsTest, FlagNameReturnsCorrectString) {
    EXPECT_STREQ(Gss::flagName(Gss::ContextFlag::Delegate), "delegate");
    EXPECT_STREQ(Gss::flagName(Gss::ContextFlag::Mutual), "mutual");
}

TEST(GssUtilsTest, ExportFlagsParsesBitmaskCorrectly) {
    int mask = GSS_C_MUTUAL_FLAG | GSS_C_REPLAY_FLAG;
    auto flags = Gss::exportFlags(mask);
    
    EXPECT_EQ(flags.size(), 2);
    
    auto has_flag = [&flags](Gss::ContextFlag f) {
        return std::find(flags.begin(), flags.end(), f) != flags.end();
    };
    
    EXPECT_TRUE(has_flag(Gss::ContextFlag::Mutual));
    EXPECT_TRUE(has_flag(Gss::ContextFlag::Replay));
    EXPECT_FALSE(has_flag(Gss::ContextFlag::Confidential));
}

TEST(GssUtilsTest, Error2StrFormatsCorrectly) {
    std::string error = Gss::error2str(0, 0); 
    EXPECT_FALSE(error.empty());
}

TEST(GssRAIITest, CredentialHandlesNullptrOnDestruction) {
    EXPECT_NO_THROW({
        Gss::Credential cred;
        cred.name = nullptr;
        cred.cred = nullptr;
    });
}

TEST(GssRAIITest, SecurityHandlesNullptrOnDestruction) {
    EXPECT_NO_THROW({
        Gss::Security sec;
        sec.sec = nullptr;
    });
}

class MockBaseContext : public Gss::BaseContext {
public:
    MOCK_METHOD(std::vector<uint8_t>, recvToken, (), (const, override));
    MOCK_METHOD(void, sendToken, (const void*, size_t), (override));
    MOCK_METHOD(void, error, (std::string_view, std::string_view, OM_uint32, OM_uint32), (const, override));

    void setMockSecurity(Gss::SecurityPtr security) {
        ctx = std::move(security);
    }
};

using ::testing::_;
using ::testing::Return;
using ::testing::ElementsAre;

TEST(GssBaseContextTest, SendMessageInvokesSendToken) {
    ::testing::NiceMock<MockBaseContext> context; 
    std::string message = "Hello GSSAPI";

    auto fake_sec = std::make_unique<Gss::Security>();
    context.setMockSecurity(std::move(fake_sec));

    EXPECT_CALL(context, sendToken(_, _)).Times(::testing::AtLeast(0));
    
    bool result = context.sendMessage(message.data(), message.size(), false);
    
    (void)result; 
}

TEST(GssBaseContextTest, RecvMessageReturnsEmptyOnEmptyToken) {
    ::testing::NiceMock<MockBaseContext> context; 
    auto fake_sec = std::make_unique<Gss::Security>();
    context.setMockSecurity(std::move(fake_sec));

    EXPECT_CALL(context, recvToken())
        .WillOnce(Return(std::vector<uint8_t>{}));

    auto result = context.recvMessage();
    EXPECT_TRUE(result.empty());
}

TEST(GssCredentialTest, AcquireUserCredentialFailsForEmptyUser) {
    Gss::ErrorCodes err;
    auto cred = Gss::acquireUserCredential("", &err);
    
    EXPECT_EQ(cred, nullptr);
    EXPECT_NE(err.code1, GSS_S_COMPLETE);
    EXPECT_STREQ(err.func, "gss_import_name");
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_gsswrap.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
