#include <list>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <memory>
#include <cstring>
#include <iostream>
#include <exception>
#include <filesystem>

#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "ltsm_xcb_wrapper.h"
#include "librfb_encodings.h"
#include "librfb_ffmpeg.h"
#include "ltsm_tools.h"

using namespace LTSM;

class FakeStream : public RFB::EncoderStream
{
    PixelFormat pf;
    size_t write = 0;

public:
    FakeStream(const XCB::RootDisplay* xcb)
    {
        if(! xcb)
        {
            Application::error("%s: xcb failed", __FUNCTION__);
            throw std::runtime_error(NS_FuncName);
        }

        auto visual = xcb->visual();
        
        if(! visual)
        {
            Application::error("%s: xcb visual failed", __FUNCTION__);
            throw std::runtime_error(NS_FuncName);
        }

        pf = PixelFormat(xcb->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);
    }

    const size_t & writeBytes(void) const { return write; }

    // RFB::EncoderStream interface
    const PixelFormat & serverFormat(void) const override { return pf; }
    const PixelFormat & clientFormat(void) const override { return pf; }
    bool clientIsBigEndian(void) const override { return false; }
    XCB::Size displaySize(void) const override { return {0,0}; }

    // NetworkStream interface
    void sendRaw(const void* ptr, size_t len) override { write += len; }

private:
    // NetworkStream interface
    bool hasInput(void) const override { throw std::runtime_error("unsupported"); }
    size_t hasData(void) const override { throw std::runtime_error("unsupported"); }
    uint8_t peekInt8(void) const override { throw std::runtime_error("unsupported"); }
    void recvRaw(void* ptr, size_t len) const override { throw std::runtime_error("unsupported"); }
};

class EncodingTest : public Application
{
    std::unique_ptr<XCB::RootDisplay> xcb;

public:
    EncodingTest()
        : Application("x11enc")
    {
        xcb = std::make_unique<XCB::RootDisplay>(-1);

        if(!xcb)
        {
            Application::warning("xcb failed");
            throw std::runtime_error(NS_FuncName);
        }

        xcb->extensionDisable(XCB::Module::DAMAGE);
    }

    int start(void)
    {
        auto win = xcb->root();
        auto dsz = xcb->size();
        auto reg = XCB::Region{{0,0}, dsz};
        auto bpp = xcb->bitsPerPixel() >> 3;
        auto pitch = dsz.width * bpp;

        if(auto align8 = pitch % 8)
            pitch += 8 - align8;

        Application::info("%s: xcb - width: %u, height: %u, bpp: %u, pitch: %u, max request: %u", __FUNCTION__, dsz.width, dsz.height, bpp, pitch, xcb->getMaxRequest());

        auto shm = static_cast<const XCB::ModuleShm*>(xcb->getExtension(XCB::Module::SHM));
        auto shmId = shm ? shm->createShm(pitch * dsz.height, 0600, false) : nullptr;

        if(int err = xcb->hasError())
        {
            Application::error("xcb error: %d", err);
            return err;
        }

        auto stream = std::make_unique<FakeStream>(xcb.get());

        if(auto pixmapReply = xcb->copyRootImageRegion(reg, shmId))
        {
            const FrameBuffer fb(pixmapReply->data(), reg, stream->serverFormat());

            auto tp1 = std::chrono::steady_clock::now();
            auto map1 = fb.pixelMapPalette(reg);
            auto dt1 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp1);

            Application::info("%s: pixelMapPalette: %u", __FUNCTION__, dt1.count());

            auto tp2 = std::chrono::steady_clock::now();
            auto map2 = fb.pixelMapWeight(reg);
            auto dt2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp2);

            Application::info("%s: pixelMapWeight: %u", __FUNCTION__, dt2.count());

            auto tp3 = std::chrono::steady_clock::now();
            auto map3 = fb.toRLE(reg);
            auto dt3 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp3);

            Application::info("%s: toRLE: %u", __FUNCTION__, dt3.count());
        }

        return 0;
    }
};

/*
                               PARA/FLAT   PARA/UNOR   SINGLE/FLAT SINGLE/UNOR
[info] start: pixelMapPalette: 3           3           9           8
[info] start: pixelMapWeight:  2           3           8           8
[info] start: toRLE:           3           3           8           9
*/

int main(int argc, char** argv)
{
    int res = 0;

    try
    {
        res = EncodingTest().start();
    }
    catch(const std::exception & err)
    {
        Application::error("exception: %s", err.what());
    }

    return res;
}
