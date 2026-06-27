// Google Gemini AI

#include <vector>
#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

#include "ltsm_tools.h"
#include "ltsm_librfb.h"
#include "ltsm_application.h"

using namespace LTSM;

TEST(StreamBitsTest, DefaultConstructorIsEmpty) {
    Tools::StreamBits stream;
    EXPECT_TRUE(stream.empty());
    EXPECT_EQ(stream.bitpos, 0);
    EXPECT_TRUE(stream.toVector().empty());
}

TEST(StreamBitsTest, MoveConstructorPopulatesVector) {
    std::vector<uint8_t> data = {0xAA, 0xBB};
    Tools::StreamBits stream(std::move(data));

    EXPECT_FALSE(stream.empty());
    EXPECT_EQ(stream.toVector().size(), 2);
    EXPECT_EQ(stream.toVector()[0], 0xAA);
}

TEST(StreamBitsPackTest, PushBitChangesState) {
    Tools::StreamBitsPack pack;

    pack.pushBit(true);
    EXPECT_NO_THROW(pack.pushBit(false));
}

TEST(StreamBitsPackTest, PushValueDifferentFields) {
    Tools::StreamBitsPack pack;

    EXPECT_NO_THROW(pack.pushValue(1, Tools::StreamBitsPack::Field::Val1));
    EXPECT_NO_THROW(pack.pushValue(3, Tools::StreamBitsPack::Field::Val2));
    EXPECT_NO_THROW(pack.pushValue(15, Tools::StreamBitsPack::Field::Val4));
}

TEST(StreamBitsPackTest, PushAlignAlignsToByte) {
    Tools::StreamBitsPack pack;
    pack.pushBit(true);
    pack.pushAlign();

    EXPECT_EQ(pack.toVector().size(), 1);
}

TEST(StreamBitsIntegrationTest, PackAndUnpackBits) {
    Tools::StreamBitsPack pack;
    pack.pushBit(true);
    pack.pushBit(false);
    pack.pushBit(true);
    pack.pushAlign();

    std::vector<uint8_t> buffer = pack.toVector();
    Tools::StreamBitsUnpack unpack(std::move(buffer), 3, 1);

    EXPECT_TRUE(unpack.popBit());
    EXPECT_FALSE(unpack.popBit());
    EXPECT_TRUE(unpack.popBit());
}

TEST(StreamBitsIntegrationTest, PackAndUnpackValues) {
    Tools::StreamBitsPack pack;
    pack.pushValue(5, Tools::StreamBitsPack::Field::Val4);
    pack.pushAlign();

    std::vector<uint8_t> buffer = pack.toVector();
    Tools::StreamBitsUnpack unpack(std::move(buffer), 1, 4);
    
    EXPECT_EQ(unpack.popValue(4), 5);
}

TEST(StreamBitsUnpackTest, UnpackFromEmptyBuffer) {
    std::vector<uint8_t> empty_vector;
    Tools::StreamBitsUnpack unpack(std::move(empty_vector), 0, 0);

    EXPECT_THROW(unpack.popBit(), std::invalid_argument); 
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_streambit.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
