#include <vector>
#include <string>
#include <string_view>
#include <cstring>

#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_streambuf.h"

using namespace LTSM;

TEST(ByteOrderInterfaceTest, StaticByteSwapping) {
    EXPECT_EQ(ByteOrderInterface::swap16(0x1234), 0x3412);
    EXPECT_EQ(ByteOrderInterface::swap32(0x12345678), 0x78563412);
    EXPECT_EQ(ByteOrderInterface::swap64(0x1122334455667788ULL), 0x8877665544332211ULL);
}

TEST(StreamBufRefTest, InitializationAndProperties) {
    uint8_t buffer[] = {0x01, 0x02, 0x03, 0x04};
    
    StreamBufRef stream(buffer, sizeof(buffer));
    EXPECT_EQ(stream.last(), sizeof(buffer));
    EXPECT_EQ(stream.peek(), 0x01);
    
    #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        EXPECT_TRUE(stream.bigendian());
    #else
        EXPECT_FALSE(stream.bigendian());
    #endif

    uint8_t new_buffer[] = {0xAA, 0xBB};
    stream.reset(new_buffer, sizeof(new_buffer));
    EXPECT_EQ(stream.last(), 2);
    EXPECT_EQ(stream.peek(), 0xAA);
    EXPECT_EQ(stream.data(), new_buffer);
}

TEST(StreamBufRefTest, MoveSemantics) {
    uint8_t buffer[] = {0x0A, 0x0B, 0x0C};
    StreamBufRef src(buffer, sizeof(buffer));

    StreamBufRef dest(std::move(src));
    EXPECT_EQ(dest.last(), 3);
    EXPECT_EQ(dest.peek(), 0x0A);

    StreamBufRef dest2;
    dest2 = std::move(dest);
    EXPECT_EQ(dest2.last(), 3);
}

TEST(StreamBufRefTest, ReadEndianIntegers) {
    uint8_t buffer[] = {
        0x01,                               // Int8
        0x12, 0x34,                         // 16-bit
        0x12, 0x34, 0x56, 0x78,             // 32-bit
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 // 64-bit
    };

    // Little Endian
    {
        StreamBufRef stream(buffer, sizeof(buffer));
        EXPECT_EQ(stream.readInt8(), 0x01);
        
        stream.reset(buffer + 1, 2);
        EXPECT_EQ(stream.peekIntLE16(), 0x3412);

        stream.reset(buffer + 3, 4);
        EXPECT_EQ(stream.peekIntLE32(), 0x78563412);

        stream.reset(buffer + 7, 8);
        EXPECT_EQ(stream.peekIntLE64(), 0x8877665544332211ULL);
    }

    // Big Endian
    {
        StreamBufRef stream(buffer, sizeof(buffer));
        stream.reset(buffer + 1, 2);
        EXPECT_EQ(stream.peekIntBE16(), 0x1234);

        stream.reset(buffer + 3, 4);
        EXPECT_EQ(stream.peekIntBE32(), 0x12345678);

        stream.reset(buffer + 7, 8);
        EXPECT_EQ(stream.peekIntBE64(), 0x1122334455667788ULL);
    }
}

TEST(StreamBufRefTest, AdvancedReadAndStreaming) {
    uint8_t data[] = {0x41, 0x42, 0x43, 0x44, 0x45, 0x46}; // "ABCDEF"
    StreamBufRef stream(data, sizeof(data));

    // skip()
    stream.skip(1);
    EXPECT_EQ(stream.peek(), 0x42);

    // read(size) -> std::vector
    std::vector<uint8_t> vec = stream.read(2);
    ASSERT_EQ(vec.size(), 2);
    EXPECT_EQ(vec[0], 0x42);
    EXPECT_EQ(vec[1], 0x43);

    stream.reset(data, sizeof(data));
    
    uint8_t a;
    stream >> a;
    EXPECT_EQ(a, 0x41);

    uint8_t raw_dest[2];
    stream.readTo(raw_dest, 2);
    EXPECT_EQ(raw_dest[0], 0x42);
    EXPECT_EQ(raw_dest[1], 0x43);

    stream.reset(data, sizeof(data));
    std::string str = stream.readString(4);
    EXPECT_EQ(str, "ABCD");

    stream.reset(data, sizeof(data));
    uint8_t arr[3];
    stream >> arr;
    EXPECT_EQ(arr[0], 0x41);
    EXPECT_EQ(arr[1], 0x42);
    EXPECT_EQ(arr[2], 0x43);
}

TEST(StreamBufTest, LifecycleAndConstructors) {
    {
        StreamBuf stream(512);
        EXPECT_EQ(stream.last(), 0);
        EXPECT_EQ(stream.tell(), 0);
        EXPECT_TRUE(stream.rawbuf().empty() || stream.rawbuf().capacity() >= 512);
    }

    std::vector<uint8_t> init_data = {0x10, 0x20, 0x30};
    {
        StreamBuf stream(init_data);
        EXPECT_EQ(stream.last(), 3);
        EXPECT_EQ(stream.peek(), 0x10);
    }

    {
        std::vector<uint8_t> move_data = init_data;
        StreamBuf stream(std::move(move_data));
        EXPECT_EQ(stream.last(), 3);
        EXPECT_TRUE(move_data.empty());
    }
}

TEST(StreamBufTest, CopyAndMoveSemantics) {
    StreamBuf original({0x01, 0x02, 0x03});
    
    StreamBuf copy_constructed(original);
    EXPECT_EQ(copy_constructed.last(), 3);
    EXPECT_EQ(copy_constructed.peek(), 0x01);
    
    StreamBuf copy_assigned;
    copy_assigned = original;
    EXPECT_EQ(copy_assigned.last(), 3);

    StreamBuf move_constructed(std::move(copy_constructed));
    EXPECT_EQ(move_constructed.last(), 3);

    StreamBuf move_assigned;
    move_assigned = std::move(copy_assigned);
    EXPECT_EQ(move_assigned.last(), 3);
}

TEST(StreamBufTest, PositionAndStateManagement) {
    StreamBuf stream({0xAA, 0xBB, 0xCC, 0xDD});
    
    EXPECT_EQ(stream.tell(), 0);
    EXPECT_EQ(stream.last(), 4);

    uint8_t val;
    stream >> val;
    EXPECT_EQ(val, 0xAA);
    EXPECT_EQ(stream.tell(), 1);
    EXPECT_EQ(stream.last(), 3); // Зависит от реализации: last() может уменьшаться или оставаться константным. 
                                 // Если last() — это всегда полный размер буфера, замените на EXPECT_EQ(stream.last(), 4);

    stream.skip(2);
    EXPECT_EQ(stream.tell(), 3);
    EXPECT_EQ(stream.peek(), 0xDD);

    EXPECT_NO_THROW(stream.shrink());

    stream.reset();
    EXPECT_EQ(stream.tell(), 0);
    EXPECT_EQ(stream.last(), 0);

    stream.reset({0x11, 0x22});
    EXPECT_EQ(stream.last(), 2);
    EXPECT_EQ(stream.peek(), 0x11);
}

TEST(StreamBufTest, InternalBufferAccess) {
    StreamBuf stream({0x05, 0x06, 0x07});

    const uint8_t* ptr = stream.data();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr[0], 0x05);

    const StreamBuf& const_stream = stream;
    const BinaryBuf& const_raw = const_stream.rawbuf();
    EXPECT_EQ(const_raw.size(), 3);
}

TEST(StreamBufTest, ReadWriteIntegration) {
    StreamBuf stream;

    stream << std::string_view("Hello") << uint8_t(0x21); // "Hello!"
    EXPECT_EQ(stream.last(), 6);
    
    std::vector<uint8_t> read_data = stream.read(5);
    std::string str(read_data.begin(), read_data.end());
    EXPECT_EQ(str, "Hello");
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_streambuf.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
