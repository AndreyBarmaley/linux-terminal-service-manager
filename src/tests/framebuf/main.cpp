//#include <ctime>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <algorithm>

#include "ltsm_framebuffer.h"

using namespace LTSM;

int rand2(int min, int max)
{
    if(min > max) std::swap(min, max);
    return min + std::rand() / (RAND_MAX + 1.0) * (max - min + 1);
}

struct RegionPixel : std::pair<XCB::Region, uint32_t>
{
    RegionPixel(const XCB::Region & reg, uint32_t pixel) : std::pair<XCB::Region, uint32_t>(reg, pixel) {}
    RegionPixel() {}

    const uint32_t &    pixel(void) const { return second; }
    const XCB::Region & region(void) const { return first; }
};

RegionPixel regionPixelRandom(const XCB::Size & wsz, const PixelFormat & pixelFormat)
{
    uint8_t cr = rand2(0, 255);
    uint8_t cg = rand2(0, 255);
    uint8_t cb = rand2(0, 255);

    auto col = Color(cr, cg, cb, 0xFF);
    auto reg = XCB::Region(rand2(0, wsz.width - 1), rand2(0, wsz.height - 1), 32, 32);

    return RegionPixel(reg, pixelFormat.pixel(col));
}

FrameBuffer generate(const PixelFormat & pf)
{
    FrameBuffer back(XCB::Size(640, 480), pf);
    back.fillColor(back.region(), Color());

    std::cout << "generate 1000 regions" << std::endl;
    for(int it = 0; it < 1000; ++it)
    {
        auto rp = regionPixelRandom(back.region(), back.pixelFormat());
        back.fillPixel(rp.region(), rp.pixel());
    }

    back.drawRect(XCB::Region(160, 120, 320, 240), Color(0xFF, 0xFF, 0));
    return back;
}

int main(int argc, char** argv)
{
    auto today = std::chrono::system_clock::now();
    auto dtn = today.time_since_epoch();
    std::srand(dtn.count());

    auto formats = { RGB565, BGR565, RGB24, BGR24, RGBA32, BGRA32, ARGB32, ABGR32 };
    FrameBuffer back(XCB::Size(640, 480), RGB24);
    int index = 0;

    for(auto & pf : formats)
    {
        std::cout << "test framebuffer: " << index++ << std::endl;
        auto tmp = generate(pf);
        back.blitRegion(tmp, tmp.region(), XCB::Point(0, 0));
    }

    return 0;
}
