// Google Gemini AI

#include <memory>
#include <vector>
#include <string>
#include <span>

#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_framebuffer.h"

using namespace LTSM;

TEST(ColorTest, DefaultAndParametricConstructors) {
    Color c1;
    EXPECT_EQ(c1.r, 0);
    EXPECT_EQ(c1.g, 0);
    EXPECT_EQ(c1.b, 0);
    EXPECT_EQ(c1.x, 0);

    Color c2(255, 128, 64, 200);
    EXPECT_EQ(c2.r, 255);
    EXPECT_EQ(c2.g, 128);
    EXPECT_EQ(c2.b, 64);
    EXPECT_EQ(c2.x, 200);
}

TEST(ColorTest, ConvertToARGB) {
    Color c(0x11, 0x22, 0x33, 0x44); // r=11, g=22, b=33, x=44
    // int: 0x44112233
    EXPECT_EQ(c.toARGB(), 0x44112233);
}

TEST(ColorTest, OperatorsAndHasher) {
    Color c1(10, 20, 30, 40);
    Color c2(10, 20, 30, 40);
    Color c3(10, 20, 30, 99);

    EXPECT_TRUE(c1 == c2);
    EXPECT_TRUE(c1 != c3);

    HasherColor hasher;
    EXPECT_EQ(hasher(c1), hasher(c2));
    EXPECT_NE(hasher(c1), hasher(c3));

    ColorMap map;
    EXPECT_NO_THROW(map.insert(c1)); 
}

TEST(PixelMapsTest, PaletteFindColorIndex) {
    PixelMapPalette palette;

    // empty
    EXPECT_EQ(palette.findColorIndex(0xFF00AA), static_cast<uint32_t>(-1));

    palette[0xFF0000] = 10; // Red -> Index 10
    palette[0x00FF00] = 20; // Green -> Index 20
    palette[0x0000FF] = 30; // Blue -> Index 30

    EXPECT_EQ(palette.findColorIndex(0x00FF00), 20);
    EXPECT_EQ(palette.findColorIndex(0x0000FF), 30);

    EXPECT_EQ(palette.findColorIndex(0xABCDEF), static_cast<uint32_t>(-1));
}

TEST(PixelMapsTest, WeightMaxWeightPixel) {
    PixelMapWeight weights;

    // empty
    EXPECT_EQ(weights.maxWeightPixel(), 0);

    weights[0x111111] = 5;   // Pixel 1, Weight 5
    weights[0x222222] = 100; // Pixel 2, Weight 100
    weights[0x333333] = 42;  // Pixel 3, Weight 42

    EXPECT_EQ(weights.maxWeightPixel(), 0x222222);

    weights[0x111111] = 500;
    EXPECT_EQ(weights.maxWeightPixel(), 0x111111);
}

TEST(PixelFormatTest, ConstructorsAndMaskParsing) {
    // (32bit ARGB8888)
    // A=0xFF000000, R=0x00FF0000, G=0x0000FF00, B=0x000000FF
    PixelFormat formatARGB(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);

    EXPECT_EQ(formatARGB.bitsPerPixel(), 32);
    EXPECT_EQ(formatARGB.rmask(), 0x00FF0000);
    EXPECT_EQ(formatARGB.gmask(), 0x0000FF00);
    EXPECT_EQ(formatARGB.bmask(), 0x000000FF);
    EXPECT_EQ(formatARGB.amask(), 0xFF000000);

    // (RGB565)
    // R: max=31 (5bit), shift=11; G: max=63 (6bit), shift=5; B: max=31 (5bit), shift=0
    PixelFormat formatRGB565(16, 31, 63, 31, 0, 11, 5, 0, 0);

    EXPECT_EQ(formatRGB565.bitsPerPixel(), 16);
    EXPECT_EQ(formatRGB565.rmax(), 31);
    EXPECT_EQ(formatRGB565.gmax(), 63);
    EXPECT_EQ(formatRGB565.bmax(), 31);
    EXPECT_EQ(formatRGB565.amax(), 0);
    EXPECT_EQ(formatRGB565.rshift(), 11);
    EXPECT_EQ(formatRGB565.gshift(), 5);
    EXPECT_EQ(formatRGB565.bshift(), 0);
}

TEST(PixelFormatTest, ComparisonsAndDepth) {
    PixelFormat fmt1(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    PixelFormat fmt2(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000); // no alpha

    EXPECT_TRUE(fmt1 != fmt2);

    EXPECT_TRUE(fmt1.compare(fmt2, true /* skipAlpha */));
    EXPECT_FALSE(fmt1.compare(fmt2, false /* skipAlpha */));

    EXPECT_GT(fmt1.depth(), 0);
    EXPECT_TRUE(fmt1.leastSignificant());
}

TEST(PixelFormatTest, ExtractChannelsAndColor) {
    // ARGB8888
    PixelFormat fmt(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    
    // A=0xAA, R=0xBB, G=0xCC, B=0xDD -> 0xAABBCCDD
    Color expected_color(0xBB, 0xCC, 0xDD, 0xAA);
    uint32_t pixel = expected_color.toARGB();

    EXPECT_EQ(fmt.alpha(pixel), expected_color.x);
    EXPECT_EQ(fmt.red(pixel), expected_color.r);
    EXPECT_EQ(fmt.green(pixel), expected_color.g);
    EXPECT_EQ(fmt.blue(pixel), expected_color.b);

    EXPECT_EQ(fmt.color(pixel), expected_color);
    EXPECT_EQ(fmt.pixel(expected_color), pixel);
}

TEST(PixelFormatTest, FormatConversion) {
    // ARGB8888
    PixelFormat fmt32(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    // RGB565 (R=5, G=6, B=5)
    PixelFormat fmt16(16, 31, 63, 31, 0, 11, 5, 0, 0);

    uint32_t pixel32 = 0xFFFFFFFF; // white 32bit

    uint32_t pixel16 = fmt32.convertTo(pixel32, fmt16);
    EXPECT_EQ(pixel16, 0xFFFF);

    uint32_t converted_back = fmt16.convertFrom(fmt32, pixel16);
    EXPECT_NO_THROW(fmt32.convertFrom(fmt16, pixel16));
}

TEST(PixelLengthTest, PropertiesAccess) {
    PixelLength pl(0xFF00AA, 12);
    EXPECT_EQ(pl.pixel(), 0xFF00AA);
    EXPECT_EQ(pl.length(), 12);
}

TEST(FBInfoTest, InternalAllocationAndExternalPointer) {
    XCB::Size size(100, 100);
    PixelFormat fmt = RGBA32;

    {
        fbinfo_t info(size, fmt);
        EXPECT_NE(info.buffer, nullptr);
        EXPECT_GT(info.pitch, 0);
        EXPECT_TRUE(info.allocated); 
    }

    std::vector<uint8_t> external_mem(400);
    {
        fbinfo_t info(external_mem.data(), size, fmt);
        EXPECT_EQ(info.buffer, external_mem.data());
        EXPECT_FALSE(info.allocated);
    }
}

TEST(FrameBufferTest, InitializationAndMetainfo) {
    XCB::Size size(16, 16);
    FrameBuffer fb(size, RGBA32);

    EXPECT_EQ(fb.region().width, 16);
    EXPECT_EQ(fb.region().height, 16);
    EXPECT_EQ(fb.bitsPerPixel(), 32);
    EXPECT_EQ(fb.bytePerPixel(), 4);
    EXPECT_GT(fb.pitchSize(), 0);

    auto it = fb.coordBegin();
    EXPECT_TRUE(it.isValid());

    auto fb_span = fb.span();
    EXPECT_FALSE(fb_span.empty());
}

TEST(FrameBufferTest, SubRegionConstruction) {
    XCB::Size main_size(100, 100);
    FrameBuffer main_fb(main_size, RGBA32);

    XCB::Region sub_reg(10, 20, 30, 40); // x=10, y=20, w=30, h=40
    FrameBuffer sub_fb(sub_reg, main_fb);

    EXPECT_EQ(sub_fb.region().width, 30);
    EXPECT_EQ(sub_fb.region().height, 40);
    
    EXPECT_EQ(sub_fb.pixelFormat().bitsPerPixel(), main_fb.pixelFormat().bitsPerPixel());
}

TEST(FrameBufferTest, PixelAndColorManipulation) {
    XCB::Size size(10, 10);
    FrameBuffer fb(size, RGBA32);
    XCB::Point pt(2, 3);
    uint32_t target_pixel = 0xFF112233;

    fb.setPixel(pt, target_pixel);
    EXPECT_EQ(fb.pixel(pt), target_pixel);

    XCB::Point row_start(0, 5);
    fb.setPixelRow(row_start, 0x00FF00FF, 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(fb.pixel(XCB::Point(i, 5)), 0x00FF00FF);
    }

    XCB::Point color_pt(4, 4);
    Color test_color(100, 150, 200, 255);
    fb.setColor(color_pt, test_color);
    
    EXPECT_EQ(fb.color(color_pt), test_color);
}

TEST(FrameBufferTest, RegionFillingAndDrawing) {
    XCB::Size size(20, 20);
    FrameBuffer fb(size, RGBA32);
    XCB::Region fill_reg(5, 5, 10, 10);
    uint32_t fill_pixel = 0xAA00BB00;

    fb.fillPixel(fill_reg, fill_pixel);
    
    EXPECT_EQ(fb.pixel(XCB::Point(7, 7)), fill_pixel);
    EXPECT_NE(fb.pixel(XCB::Point(2, 2)), fill_pixel);

    EXPECT_TRUE(fb.allOfPixel(fill_pixel, fill_reg));
    EXPECT_FALSE(fb.allOfPixel(fill_pixel, fb.region()));

    EXPECT_NO_THROW(fb.drawRect(fill_reg, Color(255, 0, 0)));
}

TEST(FrameBufferTest, TextRendering) {
    FrameBuffer fb(XCB::Size(200, 50), RGBA32);
    Color text_color(255, 255, 255);
    XCB::Point origin(10, 10);

    EXPECT_NO_THROW(fb.renderChar('A', text_color, origin));
    EXPECT_NO_THROW(fb.renderText("Hello", text_color, origin));
}

TEST(FrameBufferTest, BlitAndCopyOperations) {
    FrameBuffer src(XCB::Size(10, 10), RGBA32);
    FrameBuffer dest(XCB::Size(20, 20), RGBA32);

    src.fillPixel(src.region(), 0xFFFFFFFF);

    XCB::Region src_sub(0, 0, 5, 5);
    XCB::Point dest_pos(5, 5);
    
    EXPECT_NO_THROW(dest.blitRegion(src, src_sub, dest_pos));
    EXPECT_EQ(dest.pixel(XCB::Point(6, 6)), 0xFFFFFFFF);

    FrameBuffer cloned_reg = src.copyRegion(XCB::Region(0, 0, 2, 2));
    EXPECT_EQ(cloned_reg.region().width, 2);
    EXPECT_EQ(cloned_reg.pixel(XCB::Point(0, 0)), 0xFFFFFFFF);
}

TEST(FrameBufferTest, AnalysisAndSerialization) {
    FrameBuffer fb(XCB::Size(5, 5), RGBA32);
    
    fb.setPixel(XCB::Point(0, 0), 0x11111111);
    fb.setPixel(XCB::Point(0, 1), 0x22222222);
    fb.setPixel(XCB::Point(0, 2), 0x22222222);

    EXPECT_NO_THROW({
        ColorMap c_map = fb.colourMap();
        PixelMapPalette p_palette = fb.pixelMapPalette(fb.region());
        PixelMapWeight p_weight = fb.pixelMapWeight(fb.region());
    });

    EXPECT_NO_THROW({
        PixelLengthList rle = fb.toRLE(fb.region());
        EXPECT_FALSE(rle.empty());
    });
}

TEST(FrameBufferTest, RawPixelStaticDecoder) {
    uint32_t le_pixel = 0x12345678;

    uint32_t decoded = FrameBuffer::rawPixel(&le_pixel, 32, false /* Little Endian */);
    EXPECT_NO_THROW(FrameBuffer::rawPixel(&le_pixel, 32, true));
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_framebuf.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
