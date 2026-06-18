// Google Gemini AI

#include <string_view>
#include <vector>
#include <string>

#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_streambuf.h"

using namespace LTSM;

TEST(BinaryBufTest, ConstructorsAndAssignment) {
    std::vector<uint8_t> init_data = {0x01, 0x02, 0x03};

    BinaryBuf buf1(init_data);
    EXPECT_EQ(buf1.size(), 3);
    EXPECT_EQ(buf1.data()[0], 0x01);

    BinaryBuf buf2(std::move(init_data));
    EXPECT_EQ(buf2.size(), 3);
    EXPECT_TRUE(init_data.empty());

    std::vector<uint8_t> more_data = {0xA, 0xB};
    buf1 = more_data;
    EXPECT_EQ(buf1.size(), 2);
    EXPECT_EQ(buf1.data()[1], 0xB);

    buf1 = std::move(more_data);
    EXPECT_EQ(buf1.size(), 2);
}

TEST(BinaryBufTest, SizeAndDataAccess) {
    BinaryBuf buf({0xAA, 0xBB, 0xCC});
    
    EXPECT_EQ(buf.size(), 3);
    ASSERT_NE(buf.data(), nullptr);
    EXPECT_EQ(buf.data()[0], 0xAA);

    buf.data()[1] = 0xFF;
    
    const ByteArray& base = buf;
    EXPECT_EQ(base.size(), 3);
    ASSERT_NE(base.data(), nullptr);
    EXPECT_EQ(base.data()[1], 0xFF);
}

TEST(BinaryBufTest, AppendMethods) {
    BinaryBuf buf;

    uint8_t raw_data[] = {0x01, 0x02};
    buf.append(raw_data, 2);
    EXPECT_EQ(buf.size(), 2);

    std::vector<uint8_t> vec_data = {0x03, 0x04};
    buf.append(vec_data);
    EXPECT_EQ(buf.size(), 4);

    std::string_view str_data = "\x05\x06";
    buf.append(str_data);
    EXPECT_EQ(buf.size(), 6);

    buf.append({0x07}).append({0x08});
    EXPECT_EQ(buf.size(), 8);

    uint8_t expected[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    for (size_t i = 0; i < buf.size(); ++i) {
        EXPECT_EQ(buf.data()[i], expected[i]);
    }
}

TEST(BinaryBufTest, CopyMethod) {
    BinaryBuf original({0x11, 0x22});
    BinaryBuf cloned = original.copy();

    EXPECT_EQ(cloned.size(), original.size());
    EXPECT_EQ(cloned.data()[0], original.data()[0]);
    
    cloned.data()[0] = 0x99;
    EXPECT_EQ(original.data()[0], 0x11);
    EXPECT_EQ(cloned.data()[0], 0x99);
}

TEST(BinaryBufTest, PolymorphicAssignment) {
    BinaryBuf src({0x01, 0x02, 0x03});
    const ByteArray& base_ref = src;

    BinaryBuf dest;
    dest = base_ref;

    EXPECT_EQ(dest.size(), 3);
    EXPECT_EQ(dest.data()[2], 0x03);
}

TEST(ByteArrayTest, ComparisonOperators) {
    BinaryBuf buf1({0x01, 0x02});
    BinaryBuf buf2({0x01, 0x02});
    BinaryBuf buf3({0x01, 0x03});
    BinaryBuf buf4({0x01, 0x02, 0x03});

    const ByteArray& base1 = buf1;
    const ByteArray& base2 = buf2;
    const ByteArray& base3 = buf3;
    const ByteArray& base4 = buf4;

    EXPECT_TRUE(base1 == base2);
    EXPECT_FALSE(base1 != base2);

    EXPECT_FALSE(base1 == base3);
    EXPECT_TRUE(base1 != base3);

    EXPECT_FALSE(base1 == base4);
    EXPECT_TRUE(base1 != base4);
}

TEST(ByteArrayTest, ToStringConversion) {
    BinaryBuf buf({0x48, 0x65, 0x6C, 0x6C, 0x6F}); 
    const ByteArray& base = buf;

    EXPECT_EQ(base.toString(), "Hello");

    BinaryBuf empty_buf;
    EXPECT_EQ(empty_buf.toString(), "");
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_binarybuf.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
