// Google Gemini AI

#include <filesystem>
#include <vector>
#include <string>
#include <list>
#include <span>
#include <numeric>

#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_tools.h"

using namespace LTSM;
using namespace LTSM::Tools;

TEST(ToolsTest, FileOperationsAndIO) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "ltsm_test_file.bin";
    
    std::filesystem::remove(test_file);

    std::string test_data = "Hello LTSM Tools!";
    
    // binaryToFile
    bool write_ok = binaryToFile(test_data.data(), test_data.size(), test_file, false);
    ASSERT_TRUE(write_ok);
    EXPECT_TRUE(std::filesystem::exists(test_file));

    // fileToString
    std::string read_str = fileToString(test_file);
    EXPECT_EQ(read_str, test_data);

    // fileToBinaryBuf
    std::vector<uint8_t> read_bin = fileToBinaryBuf(test_file);
    std::string bin_str(read_bin.begin(), read_bin.end());
    EXPECT_EQ(bin_str, test_data);

    // binaryToFile(append = true)
    std::string append_data = " Appended.";
    binaryToFile(append_data.data(), append_data.size(), test_file, true);
    EXPECT_EQ(fileToString(test_file), "Hello LTSM Tools! Appended.");

    std::filesystem::remove(test_file);
}

TEST(ToolsTest, DirectoryAndSymlinks) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "ltsm_test_dir";
    std::filesystem::create_directories(temp_dir);

    std::filesystem::path file1 = temp_dir / "a.txt";
    std::filesystem::path file2 = temp_dir / "b.txt";
    binaryToFile("1", 1, file1);
    binaryToFile("2", 1, file2);

    // readDir (no recursion)
    std::list<std::string> files = readDir(temp_dir, false);
    EXPECT_EQ(files.size(), 2);

    // resolveSymLink
    std::filesystem::path link_path = temp_dir / "link_to_a.txt";
    std::filesystem::remove(link_path);
    
    try {
        std::filesystem::create_symlink(file1, link_path);
        std::filesystem::path resolved = resolveSymLink(link_path);
        EXPECT_EQ(resolved, std::filesystem::canonical(file1));
    } catch (...) {}

    std::filesystem::remove_all(temp_dir);
}

TEST(ToolsTest, SystemPaths) {
    // :0 -> /tmp/.X11-unix/X0
    std::filesystem::path x11_path = x11UnixPath(0);
    EXPECT_FALSE(x11_path.empty());
    
    std::string tz = getTimeZone();
    EXPECT_FALSE(tz.empty());
}

TEST(ToolsTest, StringSplittingAndJoining) {
    std::string_view csv = "apple,banana,cherry";
    
    std::list<std::string> parts_char = split(csv, ',');
    ASSERT_EQ(parts_char.size(), 3);
    EXPECT_EQ(parts_char.front(), "apple");

    std::string_view custom_sep = " - ";
    std::string_view text = "one - two - three";
    std::list<std::string> parts_str = split(text, custom_sep);
    ASSERT_EQ(parts_str.size(), 3);
    EXPECT_EQ(parts_str.back(), "three");

    std::list<std::string> words = {"C++", "Google", "Test"};
    std::string joined = join(words, " ");
    EXPECT_EQ(joined, "C++ Google Test");
    
    std::string r_joined = rangeJoin(words.begin(), words.end(), "|");
    EXPECT_EQ(r_joined, "C++|Google|Test");
}

TEST(ToolsTest, StringTransformations) {
    EXPECT_EQ(lower("HeLLo WoRLd!"), "hello world!");

    std::string target = "I like Java. Java is good.";
    std::string updated = replace(target, "Java", "C++");
    EXPECT_EQ(updated, "I like C++. C++ is good.");

    std::string err_template = "Error code: {code}";
    std::string err_msg = replace(err_template, "{code}", 404);
    EXPECT_EQ(err_msg, "Error code: 404");

    EXPECT_EQ(quotedString("hello"), "\"hello\"");
}

TEST(ToolsTest, Escaping) {
    std::string raw = "Hello\n\tWorld";
    
    std::string esc = escaped(raw, false);
    EXPECT_NE(esc, raw);

    std::string unesc = unescaped(esc);
    EXPECT_NO_THROW(unescaped(esc)); 
}

TEST(ToolsTest, HexAndStringConversions) {
    EXPECT_EQ(hex(255, 4), "0x00ff");

    std::string orig = "Hello";
    std::wstring wstr = string2wstring(orig);
    std::string back = wstring2string(wstr);
    EXPECT_EQ(orig, back);
}

TEST(ToolsTest, Base64Coding) {
    std::vector<uint8_t> raw_bytes = {0x4D, 0x61, 0x6E}; // "Man"
    
    std::string encoded = base64Encode(raw_bytes);
    EXPECT_EQ(encoded, "TWFu");

    std::vector<uint8_t> decoded = base64Decode("TWFu");
    EXPECT_EQ(decoded, raw_bytes);
}

TEST(ToolsTest, RandomGeneration) {
    std::vector<uint8_t> bytes = randomBytes(16);
    EXPECT_EQ(bytes.size(), 16);

    std::string hex_str = randomHexString(10);
    EXPECT_EQ(hex_str.size(), 20);
    
    for (char c : hex_str) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
    }
}

TEST(ToolsTest, ChecksumsAndCrc32) {
    std::string text = "123456789";
    std::span<const uint8_t> data_span(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    
    // CRC32b "123456789" (magic 0xEDB88320) -> 0xCBF43926
    uint32_t computed_crc = crc32b(data_span);
    EXPECT_EQ(computed_crc, 0xCBF43926);
}

TEST(ToolsTest, RangeHexFormatting) {
    std::vector<uint8_t> data = {0x0A, 0x1B, 0xFF};
    
    std::string formatted = hexString(data, 2, ",", true);
    // Ожидаемый вид: "0x0A,0x1B,0xFF"
    EXPECT_EQ(formatted, "0x0A,0x1B,0xFF");
}

TEST(ToolsTest, MemoryAlignment) {
    EXPECT_EQ(alignUp(5, 4), 8);
    EXPECT_EQ(alignUp(8, 4), 8);
    EXPECT_EQ(alignUp(0, 4), 0);
    EXPECT_EQ(alignUp(5, 3), 0);

    EXPECT_EQ(alignDown(7, 4), 4);
    EXPECT_EQ(alignDown(4, 4), 4);
    EXPECT_EQ(alignDown(5, 3), 5);
}

TEST(ToolsTest, RangeNextTo) {
    std::list<int> my_list = {10, 20, 30, 40, 50};
    
    auto it = my_list.begin();
    auto advanced_it = nextTo(my_list, it, 2);
    EXPECT_EQ(*advanced_it, 30);

    auto safe_end_it = nextTo(my_list, it, 10);
    EXPECT_EQ(safe_end_it, my_list.end());
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_tools.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
