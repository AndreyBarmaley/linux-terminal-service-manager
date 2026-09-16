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

#include <boost/asio.hpp>
#include <signal.h>

#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "ltsm_xcb_wrapper.h"
#include "librfb_encodings.h"
#include "ltsm_tools.h"

#ifdef LTSM_WITH_FFMPEG
#include "librfb_ffmpeg.h"
#endif

using namespace LTSM;

namespace Run {
    std::atomic<bool> process{false};
}

void signalHandler(int sig) {
    if(sig == SIGTERM || sig == SIGINT) {
        Run::process = false;
    } else {
        Application::warning("{}: receive signal: {}", NS_FuncNameV, sig);
    }
}

class FakeStream : public RFB::EncoderStream {
    PixelFormat pf;
    size_t threads = 1;
    boost::asio::thread_pool thread_pool;

  public:
    FakeStream(const XCB::RootDisplay* xcb, int thr) : threads(thr), thread_pool{threads} {
        if(! xcb) {
            Application::error("{}: xcb failed", NS_FuncNameV);
            throw std::runtime_error(NS_FuncNameS);
        }

        auto visual = xcb->visual();

        if(! visual) {
            Application::error("{}: xcb visual failed", NS_FuncNameV);
            throw std::runtime_error(NS_FuncNameS);
        }

        pf = PixelFormat(xcb->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);
    }

    virtual ~FakeStream() = default;

    uint16_t encodingThreads(void) const override {
        return threads;
    }

    std::future<BinaryBuf> postEncoderJob(RFB::PostEncoderJobCb && func, XCB::Region reg) const override {
        std::promise<BinaryBuf> prom;
        auto ret = prom.get_future();
        if(1 < threads) {
            // job background
            boost::asio::post(const_cast<FakeStream&>(*this).thread_pool.get_executor(), [promise=std::move(prom), job=std::move(func), reg]() mutable {
                promise.set_value(job(reg));
            });
        } else {
            std::promise<BinaryBuf> promise;
            auto ret = promise.get_future();
            promise.set_value(func(reg));
            return ret;
        }
        return ret;

    }

    // RFB::EncoderStream interface
    const PixelFormat & serverFormat(void) const override {
        return pf;
    }
    const PixelFormat & clientFormat(void) const override {
        return pf;
    }
    bool clientIsBigEndian(void) const override {
        return false;
    }
    bool isDisplaySize(const XCB::Size& sz) const override {
        return true;
    }
};

struct EncodingTime {
    std::string_view id;
    std::unique_ptr<RFB::EncodingBase> enc;
    std::unique_ptr<FakeStream> stream;

    size_t iteration = 0;
    size_t workms = 0;
    size_t write = 0;

    void encodeTime(uint8_t* p, const XCB::Region & reg) {

        const FrameBuffer fb(p, reg, stream->serverFormat());

        auto tp = std::chrono::steady_clock::now();
        for(auto& pkt: enc->getFrameBufferPackets(stream.get(), fb)) {
            write += pkt.size();
        }

        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp);
        workms += dt.count();
        iteration++;
    }

    void dumpResult(void) {
        switch(enc->getType()) {
            default:
                break;
        }

        std::cout << enc->getTypeName();

        if(id.size()) {
            std::cout << "(" << id << ")";
        }

        std::cout << ": - iteration: " << iteration << ", time: " << workms / iteration << " ms" << ", bandwith: " << write / iteration << " bytes" << std::endl;
    }
};

namespace LTSM::RFB {
    std::list<int> supportedEncodings(void) {
        return {
            ENCODING_ZRLE, ENCODING_TRLE, ENCODING_HEXTILE,
            ENCODING_ZLIB, ENCODING_CORRE, ENCODING_RRE,

            ENCODING_LTSM_LZ4,
            ENCODING_LTSM_QOI,
            ENCODING_LTSM_ZQOI,
            ENCODING_LTSM_TJPG,

#ifdef LTSM_WITH_FFMPEG
            ENCODING_LTSM_H264,
            ENCODING_LTSM_MPEG4,
#endif
            ENCODING_RAW };
    }
}

class EncodingTest : public Application {
    std::unique_ptr<XCB::RootDisplay> xcb;

    const int frameRate;
    const int countLoop;
    const int threadsCount;
    const std::list<std::string> encodings;

  public:
    EncodingTest(int fps, int loop, int threads, const std::list<std::string> & list)
        : Application("x11enc"), frameRate(fps), countLoop(loop), threadsCount(threads), encodings(list) {
        xcb = std::make_unique<XCB::RootDisplay>(-1);

        if(! xcb) {
            Application::warning("xcb failed");
            throw std::runtime_error(NS_FuncNameS);
        }

        xcb->extensionDisable(XCB::Module::DAMAGE);
    }

    int start(void) {
        auto tp = std::chrono::steady_clock::now();

        auto win = xcb->root();
        auto dsz = xcb->size();
        auto reg = XCB::Region{{0, 0}, dsz};
        auto bpp = xcb->bitsPerPixel() >> 3;
        auto pitch = Tools::alignUp(dsz.width * bpp, 8);

        Application::info("{}: settings - fps: {}, threads: {}, iterations: {}", NS_FuncNameV, frameRate, threadsCount, countLoop);
        Application::info("{}: xcb - width: {}, height: {}, bpp: {}, pitch: {}, max request: {}", NS_FuncNameV, dsz.width, dsz.height, bpp, pitch, xcb->getMaxRequest());

        auto shm = static_cast<const XCB::ModuleShm*>(xcb->getExtension(XCB::Module::SHM));
        auto shmId = shm ? shm->createShm(pitch * dsz.height, 0600, false) : nullptr;

        const int frameDelayMs = 1000 / frameRate;
        std::list<EncodingTime> pool;

        // test all encodings
        if(encodings.empty()) {
            // RFB::ENCODING_RRE
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingRRE>(false), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
            // RFB::ENCODING_CORRE
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingRRE>(true), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
#ifndef LTSM_BUILD_COVERAGE_TESTS
            // RFB::ENCODING_HEXTILE
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingHexTile>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
#endif
            // RFB::ENCODING_TRLE
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingTRLE>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
            // RFB::ENCODING_ZRLE
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingZRLE>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
#ifdef LTSM_WITH_FFMPEG
            // RFB::ENCODING_LTSM_H264
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingFFmpeg>(RFB::ENCODING_LTSM_H264), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
            // RFB::ENCODING_LTSM_MPEG4
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingFFmpeg>(RFB::ENCODING_LTSM_MPEG4), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
#endif
            // RFB::ENCODING_LTSM_LZ4
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingLZ4>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
            // RFB::ENCODING_LTSM_TJPG
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingTJPG>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
            // RFB::ENCODING_LTSM_QOI
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingQOI>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
            // RFB::ENCODING_LTSM_ZQOI
            pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingZQOI>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
        } else {
            for(const auto & name : encodings) {
                // test preffered encodings
                switch(RFB::encodingType(name)) {
                    case RFB::ENCODING_RRE:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingRRE>(false), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    case RFB::ENCODING_CORRE:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingRRE>(true), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    case RFB::ENCODING_HEXTILE:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingHexTile>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    case RFB::ENCODING_TRLE:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingTRLE>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    case RFB::ENCODING_ZRLE:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingZRLE>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

#ifdef LTSM_WITH_FFMPEG
                    case RFB::ENCODING_LTSM_H264:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingFFmpeg>(RFB::ENCODING_LTSM_H264), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    case RFB::ENCODING_LTSM_MPEG4:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingFFmpeg>(RFB::ENCODING_LTSM_MPEG4), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;
#endif
                    case RFB::ENCODING_LTSM_LZ4:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingLZ4>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    case RFB::ENCODING_LTSM_ZQOI:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingZQOI>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    case RFB::ENCODING_LTSM_QOI:
                        pool.emplace_back(EncodingTime{ .enc = std::make_unique<RFB::EncodingQOI>(), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    case RFB::ENCODING_LTSM_TJPG:
                        pool.emplace_back(EncodingTime{ .id = "SAMP_444", .enc = std::make_unique<RFB::EncodingTJPG>(85, TJSAMP_444), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        pool.emplace_back(EncodingTime{ .id = "SAMP_422", .enc = std::make_unique<RFB::EncodingTJPG>(85, TJSAMP_422), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        pool.emplace_back(EncodingTime{ .id = "SAMP_420", .enc = std::make_unique<RFB::EncodingTJPG>(85, TJSAMP_420), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        pool.emplace_back(EncodingTime{ .id = "SAMP_GRAY", .enc = std::make_unique<RFB::EncodingTJPG>(85, TJSAMP_GRAY), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        pool.emplace_back(EncodingTime{ .id = "SAMP_440", .enc = std::make_unique<RFB::EncodingTJPG>(85, TJSAMP_440), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        pool.emplace_back(EncodingTime{ .id = "SAMP_411", .enc = std::make_unique<RFB::EncodingTJPG>(85, TJSAMP_411), .stream = std::make_unique<FakeStream>(xcb.get(), threadsCount) });
                        break;

                    default:
                        Application::error("encoding not found: {}", name.data());
                        break;;
                }
            }
        }

        if(pool.empty()) {
            Application::error("test skipped, pool empty");
            return -1;
        }

        size_t loopIter = countLoop ? countLoop : 0xFFFFFFFF;

        while(Run::process) {
            if(! loopIter--) {
                break;
            }

            if(int err = xcb->hasError()) {
                Application::error("xcb error: {}", err);
                return err;
            }

            tp = std::chrono::steady_clock::now();

            if(auto pixmapReply = xcb->copyRootImageRegion(reg, shmId)) {
                auto dtCopy = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp);

                for(auto & enc : pool) {
                    enc.encodeTime(pixmapReply->data(), reg);
                }
            } else {
                Application::error("{}: {}", NS_FuncNameV, "xcb copy region failed");
                throw std::runtime_error(NS_FuncNameS);
            }

            auto dtFrame = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp);

            if(dtFrame.count() < frameDelayMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(frameDelayMs - dtFrame.count()));
            }
        }

        for(auto & enc : pool) {
            enc.dumpResult();
        }

        return 0;
    }
};

int main(int argc, char** argv) {
    Run::process = true;
    int frameRate = 16;
    int countLoop = 10;
    int useThreads = 4;
    std::list<std::string> encodings;

    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);

    for(int it = 1; it < argc; ++it) {
        if(0 == std::strcmp(argv[it], "--fps") && it + 1 < argc) {
            try {
                frameRate = std::stoi(argv[it + 1]);
            } catch(const std::invalid_argument &) {
                std::cerr << "incorrect fps number" << std::endl;
            }

            it = it + 1;
        } else if(0 == std::strcmp(argv[it], "--count") && it + 1 < argc) {
            try {
                countLoop = std::stoi(argv[it + 1]);
            } catch(const std::invalid_argument &) {
                std::cerr << "incorrect count number" << std::endl;
            }

            it = it + 1;
        } else if(0 == std::strcmp(argv[it], "--threads") && it + 1 < argc) {
            try {
                useThreads = std::stoi(argv[it + 1]);
            } catch(const std::invalid_argument &) {
                std::cerr << "incorrect threads number" << std::endl;
            }

            it = it + 1;
        } else if(0 == std::strcmp(argv[it], "--encoding") && it + 1 < argc) {
            encodings.push_back(argv[it + 1]);
            it = it + 1;
        } else if(0 == std::strcmp(argv[it], "--encodings") && it + 1 < argc) {
            while(it + 1 < argc) {
                if(0 == std::strncmp(argv[it + 1], "--", 2)) {
                    break;
                }

                encodings.push_back(argv[it + 1]);
                it = it + 1;
            }
        } else {
            std::cout << "usage: " << argv[0] << " [--fps 16] [--count 10] [--threads 4] [--encoding xxx] [--encodings xxx yyy zzz]" << std::endl;
            std::cout << std::endl << "supported encodings: " << std::endl <<
                                      "    ";

            for(auto enc : RFB::supportedEncodings()) {
                std::cout << Tools::lower(RFB::encodingName(enc)) << " ";
            }

            std::cout << std::endl;
            return 0;
        }
    }

    if(0 >= frameRate || 0 >= countLoop || 0 >= useThreads) {
        std::cerr << "invalid params" << std::endl;
        return 1;
    }

    int res = 0;

    try {
        res = EncodingTest(frameRate, countLoop, useThreads, encodings).start();
    } catch(const std::exception & err) {
        Application::error("exception: {}", err.what());
    }

    return res;
}
