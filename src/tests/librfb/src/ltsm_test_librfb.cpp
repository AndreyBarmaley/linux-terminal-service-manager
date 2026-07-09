// Google Gemini AI

#include <string>
#include <stdexcept>

#include <gtest/gtest.h>

#include "ltsm_librfb.h"
#include "ltsm_application.h"

using namespace LTSM;
using namespace LTSM::RFB;

TEST(RfbErrorTest, ExceptionMessageIsCorrect) {
    const std::string_view expected_msg = "RFB Protocol Violation";
    
    try {
        throw rfb_error(expected_msg);
    } catch (const rfb_error& e) {
        EXPECT_STREQ(e.what(), "RFB Protocol Violation");
    } catch (...) {
        FAIL() << "Expected LTSM::rfb_error to be caught.";
    }
}

TEST(RfbErrorTest, InheritsFromStdRuntimeError) {
    rfb_error err("Test Error");
    EXPECT_TRUE((std::is_base_of_v<std::runtime_error, rfb_error>));
}

TEST(EncodingNameTest, ReturnsCorrectNameForStandardEncodings) {
    EXPECT_STRCASEEQ(encodingName(ENCODING_RAW), "RAW");
    EXPECT_STRCASEEQ(encodingName(ENCODING_COPYRECT), "COPYRECT");
    EXPECT_STRCASEEQ(encodingName(ENCODING_TIGHT), "TIGHT");
}

TEST(EncodingNameTest, ReturnsCorrectNameForPseudoEncodings) {
    EXPECT_STRCASEEQ(encodingName(ENCODING_DESKTOP_SIZE), "DESKTOPSIZE");
    EXPECT_STRCASEEQ(encodingName(ENCODING_LAST_RECT), "EXTENDEDLASTRECT");
}

TEST(EncodingNameTest, ReturnsCorrectNameForLtsmEncodings) {
    EXPECT_STRCASEEQ(encodingName(ENCODING_LTSM_MPEG4), "FFMPEG_MPEG4");
    EXPECT_STRCASEEQ(encodingName(ENCODING_LTSM_H264), "FFMPEG_H264");
}

TEST(EncodingNameTest, HandlesUnknownEncodingType) {
    EXPECT_STRCASEEQ(encodingName(-999), "UNKNOWN"); 
}

TEST(EncodingTypeTest, ReturnsCorrectTypeForStandardNames) {
    EXPECT_EQ(encodingType("RAW"), ENCODING_RAW);
    EXPECT_EQ(encodingType("COPYRECT"), ENCODING_COPYRECT);
    EXPECT_EQ(encodingType("TIGHT"), ENCODING_TIGHT);
}

TEST(EncodingTypeTest, ReturnsCorrectTypeForLtsmNames) {
    EXPECT_EQ(encodingType("FFMPEG_MPEG4"), ENCODING_LTSM_MPEG4);
    EXPECT_EQ(encodingType("LTSM_QOI"), ENCODING_LTSM_QOI);
}

TEST(EncodingTypeTest, HandlesUnknownEncodingNames) {
    EXPECT_EQ(encodingType("INVALID_ENCODING_NAME"), ENCODING_UNKNOWN);
}

TEST(EncodingTypeTest, HandlesEmptyString) {
    EXPECT_EQ(encodingType(""), ENCODING_UNKNOWN);
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_librfb.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
