// Google Gemini AI

#include <random>
#include <memory>
#include <vector>
#include <string>
#include <ranges>
#include <algorithm>

#include <gtest/gtest.h>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "librfb_encodings.h"
#include "librfb_decodings.h"

using namespace LTSM;

class TestEncoderStream : public RFB::EncoderStream {
  protected:
    const PixelFormat & pf_;
    const XCB::Size xsz_;

    BinaryBuf & buf_;

    bool hasInput(void) const override {
        throw network_error(NS_FuncNameS);
    }

    size_t hasData(void) const override {
        throw network_error(NS_FuncNameS);
    }

    void recvRaw(void* ptr, size_t len) const override {
        throw network_error(NS_FuncNameS);
    }

  public:
    TestEncoderStream(const XCB::Size& sz, const PixelFormat &pf, BinaryBuf & buf) : pf_(pf), xsz_{sz}, buf_{buf} {}

    inline BinaryBuf & buf() {
        return buf_;
    }

    void sendRaw(const void* ptr, size_t len) override {
        if(ptr && len) {
            buf_.append(static_cast<const uint8_t*>(ptr), len);
        }
    }

    const PixelFormat & serverFormat(void) const override {
        return pf_;
    }

    const PixelFormat & clientFormat(void) const override {
        return pf_;
    }

    bool clientIsBigEndian(void) const override {
        return false;
    }

    XCB::Size displaySize(void) const override {
        return xsz_;
    }
};

class TestDecoderStream: public RFB::DecoderStream {
    StreamBufRef & sb_;
  public:
    TestDecoderStream(StreamBufRef & sb) : sb_{sb} {}

    void recvRaw(void* ptr, size_t len) const override {
        sb_.readTo(ptr, len);
    }
};

class TestDecoderRender: public RFB::DecoderRender {
    FrameBuffer & fb_;

  public:
    TestDecoderRender(FrameBuffer & fb) : fb_{fb} {}

    const PixelFormat & serverFormat(void) const override {
        return fb_.pixelFormat();
    }
    const PixelFormat & clientFormat(void) const override {
        return fb_.pixelFormat();
    }

    void setPixel(const XCB::Point & pt, uint32_t pixel) const override {
        fb_.setPixel(pt, pixel);
    }
    void fillPixel(const XCB::Region & rg, uint32_t pixel) const override {
        fb_.fillPixel(rg, pixel);
    }

    void updateRawPixels(const XCB::Region & reg, std::vector<uint8_t>&& buf, uint32_t pitch, const PixelFormat & pf) const override {
        FrameBuffer fb(buf.data(), XCB::Region{0, 0, reg.width, reg.height}, pf, pitch);
        fb_.blitRegion(fb, fb.region(), reg.topLeft());
    }

    XCB::Size clientSize(void) const override {
        return fb_.region().toSize();
    }

    void postDecoderJob(RFB::PostDecoderJobCb && func, std::vector<uint8_t> && buf1, const XCB::Region & reg, uint32_t pitch, const PixelFormat & pf) const override {
        auto buf2 = func(buf1, reg, pitch, pf);
        assert(buf2.size() == static_cast<size_t>(pitch) * reg.height);
        updateRawPixels(reg, std::move(buf2), pitch, pf);
    }
};

class TestFrameBuffer: public FrameBuffer {
    void fillRandomRegions(size_t numRegions = 64) {
        std::mt19937 rng(42);

        std::uniform_int_distribution<uint16_t> distX(0, width() - 1);
        std::uniform_int_distribution<uint16_t> distY(0, height() - 1);
        std::uniform_int_distribution<uint16_t> distColor(0, 255);

        for(size_t i = 0; i < numRegions; ++i) {
            const uint16_t x1 = distX(rng);
            const uint16_t y1 = distY(rng);
            const uint16_t x2 = distX(rng);
            const uint16_t y2 = distY(rng);

            const uint16_t topLeftX = std::min(x1, x2);
            const uint16_t topLeftY = std::min(y1, y2);
            const uint16_t width = std::max(x1, x2) - topLeftX + 1;
            const uint16_t height = std::max(y1, y2) - topLeftY + 1;

            const XCB::Region randomRegion(topLeftX, topLeftY, width, height);
            const Color randomColor(distColor(rng), distColor(rng), distColor(rng));

            fillColor(randomRegion, randomColor);
        }
    }

  public:
    TestFrameBuffer(const XCB::Size & sz, const PixelFormat & pf) : FrameBuffer(sz, pf) {
        const uint32_t testPixel = 0xAA55CCFF;
        fillPixel(region(), testPixel);
        fillRandomRegions(64);
    }
};

template <typename Encoder, typename Decoder>
struct CodecPair {
    using EncoderType = Encoder;
    using DecoderType = Decoder;
};

// fixed options here
template <typename T>
class CodecTypedTest : public ::testing::Test {
  protected:
    XCB::Size displaySize{320, 240};
    PixelFormat pixelFormat{RGBA32};
};

template <typename T>
class CodecTypedTest1 : public CodecTypedTest<T> {
};

template <typename T>
class CodecTypedTest2 : public CodecTypedTest<T> {
};

//  codecs (block data) for tests
using CodecTypes1 = ::testing::Types <
                      CodecPair<RFB::EncodingRaw, RFB::DecodingRaw>,
                      CodecPair<RFB::EncodingZlib, RFB::DecodingZlib>
                      >;

TYPED_TEST_SUITE(CodecTypedTest1, CodecTypes1);

TYPED_TEST(CodecTypedTest1, LoopbackEncodeDecode) {
    using EncoderT = typename TypeParam::EncoderType;
    using DecoderT = typename TypeParam::DecoderType;

    auto encoder = std::make_unique<EncoderT>();
    auto decoder = std::make_unique<DecoderT>();

    TestFrameBuffer srcFb(this->displaySize, this->pixelFormat);
    FrameBuffer dstFb(this->displaySize, this->pixelFormat);

    ASSERT_EQ(dstFb.width(), srcFb.width());
    ASSERT_EQ(dstFb.height(), srcFb.height());
    ASSERT_EQ(dstFb.pixelFormat(), srcFb.pixelFormat());

    BinaryBuf buf1;
    buf1.reserve(1024 * 1024);

    TestEncoderStream encoderStream(this->displaySize, this->pixelFormat, buf1);
    TestDecoderRender testDecoder(dstFb);

    // encoder process
    ASSERT_NO_THROW(encoder->sendFrameBuffer(&encoderStream, srcFb));

    // unpack header(count16,region16,type32,length32,data)
    StreamBufRef sb(encoderStream.buf().data(), encoderStream.buf().size());
    const int count = sb.readIntBE16();
    ASSERT_EQ(count, 1);

    const int16_t rx = sb.readIntBE16();
    const int16_t ry = sb.readIntBE16();
    const uint16_t rw = sb.readIntBE16();
    const uint16_t rh = sb.readIntBE16();
    ASSERT_EQ(XCB::Region(rx, ry, rw, rh), srcFb.region());

    const int type = sb.readIntBE32();
    ASSERT_EQ(type, encoder->getType());

    uint32_t length = 0;

    if(type == RFB::ENCODING_RAW) {
        length = sb.last();
    } else {
        length = sb.readIntBE32();
    }

    ASSERT_TRUE(0 < length);
    auto buf2 = sb.read(length);

    // decoder process
    ASSERT_NO_THROW(decoder->updateRegionBuf(std::move(buf2), testDecoder, srcFb.region()));

    // testing
    EXPECT_TRUE(std::ranges::equal(dstFb.span(), srcFb.span()));
}

//  codecs (stream data) for tests
using CodecTypes2 = ::testing::Types <
                         CodecPair<RFB::EncodingRRE, RFB::DecodingRRE>,
                         CodecPair<RFB::EncodingHexTile, RFB::DecodingHexTile>,
                         CodecPair<RFB::EncodingTRLE, RFB::DecodingTRLE>
                         >;

/*
#include <iostream>

TYPED_TEST_SUITE(CodecTypedTest2, CodecTypes2);

TYPED_TEST(CodecTypedTest2, LoopbackEncodeDecode) {
    using EncoderT = typename TypeParam::EncoderType;
    using DecoderT = typename TypeParam::DecoderType;

    auto encoder = std::make_unique<EncoderT>();
    auto decoder = std::make_unique<DecoderT>();

    TestFrameBuffer srcFb(this->displaySize, this->pixelFormat);
    FrameBuffer dstFb(this->displaySize, this->pixelFormat);

    ASSERT_EQ(dstFb.width(), srcFb.width());
    ASSERT_EQ(dstFb.height(), srcFb.height());
    ASSERT_EQ(dstFb.pixelFormat(), srcFb.pixelFormat());

    BinaryBuf buf1;
    buf1.reserve(1024 * 1024);

    TestEncoderStream encoderStream(this->displaySize, this->pixelFormat, buf1);
    TestDecoderRender testDecoder(dstFb);

    // encoder process
    ASSERT_NO_THROW(encoder->sendFrameBuffer(&encoderStream, srcFb));

    // unpack header(count16,region16,type32,data)
    StreamBufRef sb(encoderStream.buf().data(), encoderStream.buf().size());
    const int count = sb.readIntBE16();
    ASSERT_TRUE(0 < count);

    for(int it = 0; it < count; ++it) {
        const int16_t rx = sb.readIntBE16();
        const int16_t ry = sb.readIntBE16();
        const uint16_t rw = sb.readIntBE16();
        const uint16_t rh = sb.readIntBE16();

        std::cerr << rx << "," << ry << "," << rw << "," << rh << std::endl;
        ASSERT_TRUE(srcFb.region().contains(XCB::Region(rx, ry, rw, rh)));

        const int type = sb.readIntBE32();
        ASSERT_EQ(type, encoder->getType());

        // decoder process
        ASSERT_NO_THROW(decoder->updateRegionStream(TestDecoderStream(sb), testDecoder, srcFb.region()));
    }

    // testing
    EXPECT_TRUE(std::ranges::equal(dstFb.span(), srcFb.span()));
}
*/

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_codecs.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
