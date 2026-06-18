// Google Gemini AI

#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "ltsm_zlib.h"
#include "ltsm_application.h"

using namespace LTSM;
using namespace LTSM::ZLib;

TEST(ZLibToolsTest, RoundTripCompression) {
    std::string original_str = "Hello, GitHub Actions and CMake! Hello, World!";
    std::span<const uint8_t> input(reinterpret_cast<const uint8_t*>(original_str.data()), original_str.size());

    auto compressed = Tools::zlibCompress(input);
    ASSERT_FALSE(compressed.empty());

    auto uncompressed = Tools::zlibUncompress(compressed, original_str.size());
    std::string result_str(reinterpret_cast<char*>(uncompressed.data()), uncompressed.size());
    
    EXPECT_EQ(original_str, result_str);
}

TEST(ZLibToolsTest, EmptyInput) {
    std::span<const uint8_t> empty_input;
    
    auto compressed = Tools::zlibCompress(empty_input);
    auto uncompressed = Tools::zlibUncompress(compressed, 0);
    
    EXPECT_TRUE(uncompressed.empty());
}

TEST(ZLibToolsTest, BadDecompressionThrows) {
    std::array<uint8_t, 5> invalid_data = {1, 2, 3, 4, 5};
    EXPECT_THROW(Tools::zlibUncompress(invalid_data), LTSM::zlib_error);
}

TEST(ZLibStreamTest, DeflateBaseStream) {
    DeflateBase compressor(Z_DEFAULT_COMPRESSION);
    std::string part1 = "Data chunk number one. ";
    std::string part2 = "Data chunk number two.";

    std::span<const uint8_t> span1(reinterpret_cast<const uint8_t*>(part1.data()), part1.size());
    std::span<const uint8_t> span2(reinterpret_cast<const uint8_t*>(part2.data()), part2.size());

    auto comp1 = compressor.deflateData(span1, Z_NO_FLUSH);
    auto comp2 = compressor.deflateData(span2, Z_FINISH);

    std::vector<uint8_t> total_compressed;
    total_compressed.insert(total_compressed.end(), comp1.begin(), comp1.end());
    total_compressed.insert(total_compressed.end(), comp2.begin(), comp2.end());

    auto uncompressed = inflate(total_compressed);
    std::string result_str(reinterpret_cast<char*>(uncompressed.data()), uncompressed.size());

    EXPECT_EQ(part1 + part2, result_str);
}

TEST(ZLibStreamTest, DeflateReset) {
    DeflateBase compressor;
    std::string data = "Some reusable data string";
    std::span<const uint8_t> span(reinterpret_cast<const uint8_t*>(data.data()), data.size());

    auto comp1 = compressor.deflateData(span, Z_FINISH);
    
    EXPECT_NO_THROW(compressor.reset());
    
    auto comp2 = compressor.deflateData(span, Z_FINISH);
    EXPECT_EQ(comp1, comp2);
}

TEST(ZLibStreamTest, InflateBaseStream) {
    std::string original = "Streaming deflation and inflation coverage test.";
    auto compressed = deflate(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(original.data()), original.size()));

    InflateBase decompressor;
    
    std::vector<uint8_t> total_uncompressed;
    size_t chunk_size = 5;
    
    for (size_t i = 0; i < compressed.size(); i += chunk_size) {
        size_t current_chunk = std::min(chunk_size, compressed.size() - i);
        std::span<const uint8_t> chunk(&compressed[i], current_chunk);
        
        int flush = (i + current_chunk == compressed.size()) ? Z_FINISH : Z_SYNC_FLUSH;
        auto decomp_part = decompressor.inflateData(chunk, flush);
        
        total_uncompressed.insert(total_uncompressed.end(), decomp_part.begin(), decomp_part.end());
    }

    std::string result_str(reinterpret_cast<char*>(total_uncompressed.data()), total_uncompressed.size());
    EXPECT_EQ(original, result_str);
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_zlib.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
