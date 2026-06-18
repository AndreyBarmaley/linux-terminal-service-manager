// Google Gemini AI

#include <sstream>
#include <vector>
#include <string>

#include <gtest/gtest.h>
#include <boost/asio.hpp>

#include "ltsm_application.h"
#include "ltsm_byte_stream.h"
#include "ltsm_byte_streambuf.h"

using namespace LTSM;

class ByteOstreamTest : public ::testing::Test {
protected:
    std::stringstream ss;
};

TEST_F(ByteOstreamTest, WriteByte) {
    byte::ostream bos(ss);
    bos.write_byte(0x41); // 'A'

    std::string result = ss.str();
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0], 0x41);
}

TEST_F(ByteOstreamTest, Write16BitEndianness) {
    byte::ostream bos(ss);
    
    bos.write_be16(0x1234);
    bos.write_le16(0x5678);

    std::string result = ss.str();
    ASSERT_EQ(result.size(), 4);
    
    EXPECT_EQ(static_cast<uint8_t>(result[0]), 0x12);
    EXPECT_EQ(static_cast<uint8_t>(result[1]), 0x34);
    
    EXPECT_EQ(static_cast<uint8_t>(result[2]), 0x78);
    EXPECT_EQ(static_cast<uint8_t>(result[3]), 0x56);
}

TEST_F(ByteOstreamTest, Write32And64Bit) {
    byte::ostream bos(ss);
    
    bos.write_be32(0x11223344);
    bos.write_le64(0x1122334455667788ULL);

    std::string result = ss.str();
    ASSERT_EQ(result.size(), 12); // 4 + 8

    EXPECT_EQ(static_cast<uint8_t>(result[0]), 0x11);
    EXPECT_EQ(static_cast<uint8_t>(result[3]), 0x44);

    EXPECT_EQ(static_cast<uint8_t>(result[4]), 0x88);
    EXPECT_EQ(static_cast<uint8_t>(result[11]), 0x11);
}

TEST_F(ByteOstreamTest, WriteZeroAndBytesAndString) {
    byte::ostream bos(ss);
    
    bos.write_zero(3);
    bos.write_bytes(std::vector<uint8_t>{0xAA, 0xBB});
    bos.write_string("Hello");

    std::string result = ss.str();
    ASSERT_EQ(result.size(), 10);

    std::string expected = std::string("\x00\x00\x00\xAA\xBB", 5) + "Hello";
    EXPECT_EQ(result, expected);
}

class ByteIstreamTest : public ::testing::Test {
protected:
    std::stringstream ss;
};

TEST_F(ByteIstreamTest, ReadByte) {
    ss.write("\x7F", 1);
    byte::istream bis(ss);

    EXPECT_EQ(bis.read_byte(), 0x7F);
}

TEST_F(ByteIstreamTest, Read16BitEndianness) {
    ss.write("\xAA\xBB", 2); 
    ss.write("\xDD\xCC", 2); 

    byte::istream bis(ss);

    EXPECT_EQ(bis.read_be16(), 0xAABB);
    EXPECT_EQ(bis.read_le16(), 0xCCDD);
}

TEST_F(ByteIstreamTest, Read32And64Bit) {
    ss.write("\x10\x20\x30\x40", 4);
    ss.write("\x88\x77\x66\x55\x44\x33\x22\x11", 8);

    byte::istream bis(ss);

    EXPECT_EQ(bis.read_be32(), 0x10203040);
    EXPECT_EQ(bis.read_le64(), 0x1122334455667788ULL);
}

TEST_F(ByteIstreamTest, ReadBytesAndStringAndSkip) {
    ss.write("\x01\x02\x03\x04\x05TestString", 15);
    byte::istream bis(ss);

    std::vector<uint8_t> expected_bytes = {0x01, 0x02};
    EXPECT_EQ(bis.read_bytes(2), expected_bytes);

    bis.skip_bytes(3);

    EXPECT_EQ(bis.read_string(10), "TestString");
}

TEST(ByteStreamIntegration, WriteAndReadBack) {
    std::stringstream shared_stream;
    
    {
        byte::ostream bos(shared_stream);
        bos.write_be32(0xDEADBEEF)
           .write_le16(0x1337)
           .write_byte(0x2A)
           .write_string("CMake");
    }

    {
        byte::istream bis(shared_stream);
        EXPECT_EQ(bis.read_be32(), 0xDEADBEEF);
        EXPECT_EQ(bis.read_le16(), 0x1337);
        EXPECT_EQ(bis.read_byte(), 0x2A);
        EXPECT_EQ(bis.read_string(5), "CMake");
    }
}

class ByteStreambufTest : public ::testing::Test {
protected:
    boost::asio::streambuf sb;

    std::vector<uint8_t> get_buffer_bytes() {
        auto bufs = sb.data();
        return std::vector<uint8_t>(
            boost::asio::buffers_begin(bufs),
            boost::asio::buffers_end(bufs)
        );
    }
};

TEST_F(ByteStreambufTest, Write16BitEndianness) {
    byte::streambuf bsb(sb);

    bsb.write_be16(0x1234);
    bsb.write_le16(0x5678);

    auto bytes = get_buffer_bytes();
    ASSERT_EQ(bytes.size(), 4);

    EXPECT_EQ(bytes[0], 0x12);
    EXPECT_EQ(bytes[1], 0x34);

    EXPECT_EQ(bytes[2], 0x78);
    EXPECT_EQ(bytes[3], 0x56);
}

TEST_F(ByteStreambufTest, Write32And64Bit) {
    byte::streambuf bsb(sb);

    bsb.write_be32(0x11223344);
    bsb.write_le64(0x0102030405060708ULL);

    auto bytes = get_buffer_bytes();
    ASSERT_EQ(bytes.size(), 12); // 4 + 8

    EXPECT_EQ(bytes[0], 0x11);
    EXPECT_EQ(bytes[3], 0x44);

    EXPECT_EQ(bytes[4], 0x08);
    EXPECT_EQ(bytes[11], 0x01);
}

TEST_F(ByteStreambufTest, WriteBytesAndString) {
    byte::streambuf bsb(sb);

    std::vector<uint8_t> raw_bytes = {0xAA, 0xBB};
    uint8_t raw_arr[] = {0xCC, 0xDD};

    bsb.write_bytes(raw_bytes);
    bsb.write_bytes(raw_arr, 2);
    bsb.write_string("Hello");

    auto bytes = get_buffer_bytes();
    ASSERT_EQ(bytes.size(), 9); // 2 + 2 + 5

    EXPECT_EQ(bytes[0], 0xAA);
    EXPECT_EQ(bytes[1], 0xBB);
    EXPECT_EQ(bytes[2], 0xCC);
    EXPECT_EQ(bytes[3], 0xDD);
    
    std::string str_part(bytes.begin() + 4, bytes.end());
    EXPECT_EQ(str_part, "Hello");
}

TEST_F(ByteStreambufTest, WriteEmptyData) {
    byte::streambuf bsb(sb);

    bsb.write_bytes(nullptr, 0);
    bsb.write_string("");
    bsb.write_bytes(std::vector<uint8_t>{});

    EXPECT_EQ(sb.size(), 0);
}

TEST_F(ByteStreambufTest, Read16BitEndianness) {
    {
        auto buf = sb.prepare(4);
        uint8_t* ptr = static_cast<uint8_t*>(buf.data());
        ptr[0] = 0x12; ptr[1] = 0x34; // BE 0x1234
        ptr[2] = 0x78; ptr[3] = 0x56; // LE 0x5678
        sb.commit(4);
    }

    byte::streambuf bsb(sb);
    EXPECT_EQ(bsb.read_be16(), 0x1234);
    EXPECT_EQ(bsb.read_le16(), 0x5678);
    EXPECT_EQ(sb.size(), 0);
}

TEST_F(ByteStreambufTest, Read32And64Bit) {
    {
        auto buf = sb.prepare(12);
        uint8_t* ptr = static_cast<uint8_t*>(buf.data());
        // BE 0x11223344
        ptr[0] = 0x11; ptr[1] = 0x22; ptr[2] = 0x33; ptr[3] = 0x44;
        // LE 0x0102030405060708
        ptr[4] = 0x08; ptr[5] = 0x07; ptr[6] = 0x06; ptr[7] = 0x05;
        ptr[8] = 0x04; ptr[9] = 0x03; ptr[10] = 0x02; ptr[11] = 0x01;
        sb.commit(12);
    }

    byte::streambuf bsb(sb);
    EXPECT_EQ(bsb.read_be32(), 0x11223344);
    EXPECT_EQ(bsb.read_le64(), 0x0102030405060708ULL);
}

TEST_F(ByteStreambufTest, ReadBuffersAndSkip) {
    {
        auto buf = sb.prepare(10);
        std::memcpy(buf.data(), "ABCDEFGHIJ", 10);
        sb.commit(10);
    }

    byte::streambuf bsb(sb);

    EXPECT_EQ(bsb.read_string(3), "ABC");

    bsb.skip_bytes(4);

    std::vector<uint8_t> expected = {'H', 'I', 'J'};
    EXPECT_EQ(bsb.read_bytes(3), expected);
    EXPECT_EQ(sb.size(), 0);
}

TEST_F(ByteStreambufTest, ReadEmptyBuffer) {
    byte::streambuf bsb(sb);

    EXPECT_TRUE(bsb.read_string(0).empty());
    EXPECT_TRUE(bsb.read_bytes(0).empty());
}

TEST(ByteStreambufIntegration, WriteAndReadBack) {
    boost::asio::streambuf shared_sb;

    {
        byte::streambuf bsb(shared_sb);
        bsb.write_be32(0xDEADBEEF)
           .write_le16(0xCAFE)
           .write_string("BoostAsio")
           .write_be64(0xFFFFFFFFFFFFFFFFULL);
    }

    {
        byte::streambuf bsb(shared_sb);
        EXPECT_EQ(bsb.read_be32(), 0xDEADBEEF);
        EXPECT_EQ(bsb.read_le16(), 0xCAFE);
        EXPECT_EQ(bsb.read_string(9), "BoostAsio");
        EXPECT_EQ(bsb.read_le64(), 0xFFFFFFFFFFFFFFFFULL);
        EXPECT_EQ(shared_sb.size(), 0);
    }
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_bytestream.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
