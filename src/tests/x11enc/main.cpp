#include <list>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <memory>
#include <cstring>
#include <iostream>
#include <exception>
#include <filesystem>

#include <signal.h>

#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "ltsm_xcb_wrapper.h"
#include "librfb_encodings.h"
#include "librfb_ffmpeg.h"
#include "ltsm_tools.h"

using namespace LTSM;

namespace Run
{
    std::atomic<bool> process{false};
}

void signalHandler(int sig)
{
    if(sig == SIGTERM || sig == SIGINT)
    {
        Run::process = false;
    }
    else
    {
        Application::warning("%s: receive signal: %d", __FUNCTION__, sig);
    }
}

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

    // NetworkStream interface
    void sendRaw(const void* ptr, size_t len) override { write += len; }

private:
    // NetworkStream interface
    bool hasInput(void) const override { throw std::runtime_error("unsupported"); }
    size_t hasData(void) const override { throw std::runtime_error("unsupported"); }
    uint8_t peekInt8(void) const override { throw std::runtime_error("unsupported"); }
    void recvRaw(void* ptr, size_t len) const override { throw std::runtime_error("unsupported"); }
};

struct EncodingTime
{
    std::unique_ptr<RFB::EncodingBase> enc;
    std::unique_ptr<FakeStream> stream;

    size_t iteration = 0;
    size_t workms = 0;

    void setThreads(int threads)
    {
        if(enc)
            enc->setThreads(threads);
    }

    void encodeTime(uint8_t* p, const XCB::Region & reg)
    {
        auto tp = std::chrono::steady_clock::now();

        const FrameBuffer fb(p, reg, stream->serverFormat());
        enc->sendFrameBuffer(stream.get(), fb);

        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp);
        workms += dt.count();
        iteration++;
    }

    void dumpResult(void)
    {
        switch(enc->getType())
        {
            default: break;
        }

        std::cout << enc->getTypeName() << ": - iteration: " << iteration << ", time: " << workms / iteration << "ms" << ", bandwith: " << stream->writeBytes() / iteration << "bytes" << std::endl;
    }
};

class EncodingTest : public Application
{
    std::unique_ptr<XCB::RootDisplay> xcb;

    const int frameRate;
    const int countLoop;
    const int threadsCount;

public:
    EncodingTest(int fps, int loop, int threads) : Application("x11enc"), frameRate(fps), countLoop(loop), threadsCount(threads)
    {
        xcb = std::make_unique<XCB::RootDisplay>(-1);

        if(!xcb)
        {
            Application::warning("xcb failed");
            throw std::runtime_error(NS_FuncName);
        }

        xcb->damageDisable();
        Application::info("%s: xcb max request: %u", __FUNCTION__, xcb->getMaxRequest());
    }

    int start(void)
    {
        auto tp = std::chrono::steady_clock::now();
        auto win = xcb->root();
        auto dsz = xcb->size();
        auto reg = XCB::Region{{0,0}, dsz};
        auto bpp = xcb->bitsPerPixel() >> 3;

        auto shm = static_cast<const XCB::ModuleShm*>(xcb->getExtension(XCB::Module::SHM));
        auto shmId = shm ? shm->createShm(dsz.width * dsz.height * bpp, 0600, false) : nullptr;

        auto stream = std::make_unique<FakeStream>(xcb.get());

        const int frameDelayMs = 1000 / frameRate;
        std::list<EncodingTime> pool;

/*
        // RFB::ENCODING_RRE
        pool.emplace_back( EncodingTime{ .enc = std::make_unique<RFB::EncodingRRE>(false), .stream = std::make_unique<FakeStream>(xcb.get()) } );
        // RFB::ENCODING_CORRE
        pool.emplace_back( EncodingTime{ .enc = std::make_unique<RFB::EncodingRRE>(true), .stream = std::make_unique<FakeStream>(xcb.get()) } );
        // RFB::ENCODING_HEXTILE
        // pool.emplace_back( EncodingTime{ .enc = std::make_unique<RFB::EncodingHexTile>(), .stream = std::make_unique<FakeStream>(xcb.get()) } );
        // RFB::ENCODING_TRLE
        pool.emplace_back( EncodingTime{ .enc = std::make_unique<RFB::EncodingTRLE>(false), .stream = std::make_unique<FakeStream>(xcb.get()) } );
        // RFB::ENCODING_ZRLE
        pool.emplace_back( EncodingTime{ .enc = std::make_unique<RFB::EncodingTRLE>(true), .stream = std::make_unique<FakeStream>(xcb.get()) } );
        // RFB::ENCODING_LTSM_LZ4
        pool.emplace_back( EncodingTime{ .enc = std::make_unique<RFB::EncodingLZ4>(), .stream = std::make_unique<FakeStream>(xcb.get()) } );
        // RFB::ENCODING_FFMPEG_H264
        pool.emplace_back( EncodingTime{ .enc = std::make_unique<RFB::EncodingFFmpeg>(RFB::ENCODING_FFMPEG_H264), .stream = std::make_unique<FakeStream>(xcb.get()) } );
*/
        // RFB::ENCODING_LTSM_LZ4
        pool.emplace_back( EncodingTime{ .enc = std::make_unique<RFB::EncodingLZ4>(), .stream = std::make_unique<FakeStream>(xcb.get()) } );
    
        for(auto & enc: pool)
        {
            enc.setThreads(threadsCount);
        }
        
        size_t loopIter = countLoop ? countLoop : 0xFFFFFFFF;

        while(Run::process)
        {
            if(! loopIter--)
            {
                Application::warning("limit iteration: %d, fps: %d", countLoop, frameRate);
                break;
            }

            if(int err = xcb->hasError())
            {
                Application::error("xcb error: %d", err);
                return err;
            }

            tp = std::chrono::steady_clock::now();
            if(auto pixmapReply = xcb->copyRootImageRegion(reg, shmId))
            {
                for(auto & enc: pool)
                {
                    enc.encodeTime(pixmapReply->data(), reg);
                }
            }
            else
            {
                Application::error("%s: %s", __FUNCTION__, "xcb copy region failed");
                throw std::runtime_error(NS_FuncName);
            }

            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp);
            if(dt.count() < frameDelayMs)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(frameDelayMs - dt.count()));
            }
        }

        for(auto & enc: pool)
        {
            enc.dumpResult();
        }

        return 0;
    }
};

int main(int argc, char** argv)
{
    Run::process = true;
    int frameRate = 16;
    int countLoop = 0;
    int useThreads = 4;

    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);

    for(int it = 1; it < argc; ++it)
    {
        if(0 == std::strcmp(argv[it], "--fps") && it + 1 < argc)
        {
            try
            {
                frameRate = std::stoi(argv[it + 1]);
            }
            catch(const std::invalid_argument &)
            {
                std::cerr << "incorrect fps number" << std::endl;
            }
            it = it + 1;
        }
        else
        if(0 == std::strcmp(argv[it], "--count") && it + 1 < argc)
        {
            try
            {
                countLoop = std::stoi(argv[it + 1]);
            }
            catch(const std::invalid_argument &)
            {
                std::cerr << "incorrect count number" << std::endl;
            }
            it = it + 1;
        }
        else
        if(0 == std::strcmp(argv[it], "--threads") && it + 1 < argc)
        {
            try
            {
                useThreads = std::stoi(argv[it + 1]);
            }
            catch(const std::invalid_argument &)
            {
                std::cerr << "incorrect threads number" << std::endl;
            }
            it = it + 1;
        }
        else
        {
            std::cout << "usage: " << argv[0] << " --fps <num> --count <num> --threads <num>" << std::endl;
            return 0;
        }
    }

    int res = 0;

    try
    {
        res = EncodingTest(frameRate, countLoop, useThreads).start();
    }
    catch(const std::exception & err)
    {
        Application::error("exception: %s", err.what());
    }

    return res;
}
