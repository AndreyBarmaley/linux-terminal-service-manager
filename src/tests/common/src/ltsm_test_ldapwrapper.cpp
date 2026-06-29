// Google Gemini AI

#include <list>
#include <string>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "ltsm_application.h"
#include "ltsm_ldap_wrapper.h"

using namespace LTSM;

class MockLdapWrapper : public ILdapWrapper {
public:
    MOCK_METHOD(std::list<LdapResult>, search, 
                (int scope, std::vector<const char*> attrs, const char* filter, const char* basedn), 
                (override));
};

class UserService {
    ILdapWrapper& _ldap;
public:
    UserService(ILdapWrapper& ldap) : _ldap(ldap) {}

    bool checkUserExists(const std::string& username) {
        auto results = _ldap.search(1, {"uid"}, username.c_str(), "ou=users");
        return !results.empty();
    }
};

TEST(UserServiceTest, ReturnsTrueIfUserFoundInLdap) {
    MockLdapWrapper mockLdap;
    UserService service(mockLdap);

    LdapResult fakeResult;
    std::list<LdapResult> expectedList { fakeResult };

    EXPECT_CALL(mockLdap, search(testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(expectedList));

    EXPECT_TRUE(service.checkUserExists("john_doe"));
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_ldap.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
