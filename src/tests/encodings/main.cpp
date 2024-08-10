#include <chrono>
#include <cstdio>
#include <memory>
#include <cstring>
#include <iostream>
#include <exception>
#include <filesystem>

#include <SDL2/SDL_image.h>

#include "ltsm_application.h"
#include "ltsm_framebuffer.h"
#include "librfb_server2.h"
#include "ltsm_tools.h"

using namespace LTSM;

struct Image
{
    std::unique_ptr<FrameBuffer> fb;

    Image(std::string_view file)
    {
        std::unique_ptr<SDL_Surface, decltype(SDL_FreeSurface)*> sf{IMG_Load(file.data()), SDL_FreeSurface};

        if(sf)
        {
            SDL_PixelFormat* pf = sf->format;
            auto fb24 = FrameBuffer((uint8_t*) sf->pixels, XCB::Region(0, 0, sf->w, sf->h), PixelFormat(pf->BitsPerPixel, pf->Rmask, pf->Gmask, pf->Bmask, pf->Amask), sf->pitch);
            fb = std::make_unique<FrameBuffer>(XCB::Size(fb24.width(), fb24.height()), BGRA32);
            fb->blitRegion(fb24, XCB::Region(0, 0, fb->width(), fb->height()), XCB::Point(0, 0));
        }

        Application::info("%s: loading: %s", __FUNCTION__, file.data());
    }
};

class EncodingTest : public Application
{
    std::unique_ptr<RFB::ServerEncoderBuf> srv;
    std::list<Image> images;
    std::string imagesPath;
    int useThreads = 1;

public:
    EncodingTest(const std::string & folder, int threadNum) : Application("encoding-test"), imagesPath(folder), useThreads(threadNum)
    {
    }

    int start(void)
    {
        std::list<std::thread> jobs;

        if(std::filesystem::is_directory(imagesPath))
        {
            for(auto const & dirEntry : std::filesystem::directory_iterator{imagesPath})
            {
                jobs.emplace_back(std::thread([this, file = dirEntry.path().native()]()
                {
                    images.emplace_back(file);
                }));
            }
        }

        for(auto & job: jobs)
            if(job.joinable()) job.join();

        if(! images.empty())
        {
            auto encodings = { RFB::ENCODING_RRE, RFB::ENCODING_CORRE, RFB::ENCODING_HEXTILE, RFB::ENCODING_ZLIB, RFB::ENCODING_TRLE, RFB::ENCODING_ZRLE };
            auto & pf = images.front().fb->pixelFormat();

            srv = std::make_unique<RFB::ServerEncoderBuf>(pf);

            Application::info("%s: pixel format, bpp: %d, rmask: 0x%08x, gmask: 0x%08x, bmask: 0x%08x, amask: 0x%08x",
                __FUNCTION__, (int) pf.bitsPerPixel(), pf.rmask(), pf.gmask(), pf.bmask(), pf.amask());


            for(auto type: encodings)
            {
                srv->serverSetClientEncoding(type);
                srv->setEncodingDebug(0);
                srv->setEncodingThreads(useThreads);

                auto tp = std::chrono::steady_clock::now();

                for(auto & img: images)
                    srv->sendFrameBufferUpdate(*img.fb);

                auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp);
                Application::info("%s: encoding: %s, time: %dms, stream sz: %dMb", __FUNCTION__, RFB::encodingName(type), dt.count(), srv->getBuffer().size()/(1024*1024));

                srv->resetBuffer();
            }
        }

        return 0;
    }
};

int main(int argc, char** argv)
{
    auto hardwareThreads = std::thread::hardware_concurrency();
    int threadNum = hardwareThreads;
    std::string folder = "images";

    for(int it = 1; it < argc; ++it)
    {
        if(0 == std::strcmp(argv[it], "--thread") && it + 1 < argc)
        {
            try
            {
                threadNum = std::stoi(argv[it + 1]);
            }
            catch(const std::invalid_argument &)
            {
                std::cerr << "incorrect threads number" << std::endl;
            }
            it = it + 1;
        }
        else
        if(0 == std::strcmp(argv[it], "--images") && it + 1 < argc)
        {
            folder.assign(argv[it + 1]);
            it = it + 1;
        }
        else
        {
            std::cout << "usage: " << argv[0] << " --thread <num>" << " --images <folder>" << std::endl;
            return 0;
        }
    }

    if(threadNum < 0 || hardwareThreads < threadNum)
        threadNum = hardwareThreads;

    int res = 0;
    SDL_Init(SDL_INIT_TIMER);

    try
    {
        res = EncodingTest(folder, threadNum).start();
    }
    catch(const std::exception & err)
    {
        Application::error("exception: %s", err.what());
    }

    SDL_Quit();
    return res;
}
