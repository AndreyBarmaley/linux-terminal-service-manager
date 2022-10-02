/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

#include <chrono>
#include <cstring>
#include <iostream>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "librfb_client.h"
#include "ltsm_vnc2image.h"

using namespace std::chrono_literals;

namespace LTSM
{
    //
    void connectorHelp(const char* prog)
    {
        std::cout << "version: " << LTSM_VNC2IMAGE_VERSION << std::endl;
        std::cout << "usage: " << prog << " --host <localhost> [--port 5900] [--password <pass>] [--timeout 100 (ms)] --image <screenshot.png> [--notls] [--debug] [--priority <string>]" << std::endl;
    }

    Vnc2Image::Vnc2Image(int argc, const char** argv)
        : Application("ltsm_vnc2image", argc, argv)
    {
        Application::setDebugLevel(DebugLevel::ConsoleError);

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                connectorHelp(argv[0]);
                throw 0;
            }
            else if(0 == std::strcmp(argv[it], "--host") && it + 1 < argc)
            {
                host.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--priority") && it + 1 < argc)
            {
                priority.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--image") && it + 1 < argc)
            {
                filename.assign("image", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--password") && it + 1 < argc)
            {
                password.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--port") && it + 1 < argc)
            {
                port = std::stoi(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--timeout") && it + 1 < argc)
            {
                timeout = std::stoi(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--notls"))
                notls = true;
            else if(0 == std::strcmp(argv[it], "--debug"))
                Application::setDebugLevel(DebugLevel::Console);
        }
    }

    int Vnc2Image::start(void)
    {
        auto ipaddr = TCPSocket::resolvHostname(host);
        int sockfd = TCPSocket::connect(ipaddr, port);

        if(0 == sockfd)
            return -1;

        RFB::ClientDecoder::setSocketStreamMode(sockfd);

        // process rfb message background
        try
        {
            if(rfbHandshake(! notls, priority, password))
            {
                tp = std::chrono::steady_clock::now();
                rfbMessagesLoop();
            }
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }
        catch(...)
        {
            Application::error("%s: unknown exception", __FUNCTION__);
        }

        return 0;
    }

    void Vnc2Image::fbUpdateEvent(void)
    {
        if(0 < timeout &&
            std::chrono::milliseconds(timeout) > std::chrono::steady_clock::now() - tp)
            return;

        if(! filename.empty() && fbPtr)
            PNG::save(*fbPtr, filename);

        RFB::ClientDecoder::rfbMessagesShutdown();
    }

    XCB::Size Vnc2Image::clientSize(void) const
    {
        return fbPtr->region().toSize();
    }

    void Vnc2Image::pixelFormatEvent(const PixelFormat & pf, uint16_t width, uint16_t height)
    {
        // receive server pixel format
        auto format = PixelFormat(pf.bitsPerPixel, pf.rmask(), pf.gmask(), pf.bmask(), 0);
        fbPtr.reset(new FrameBuffer(XCB::Region(0, 0, width, height), format));
    }

    void Vnc2Image::setPixel(const XCB::Point & dst, uint32_t pixel)
    {
        fbPtr->setPixel(dst, pixel);
    }

    void Vnc2Image::fillPixel(const XCB::Region & dst, uint32_t pixel)
    {
        fbPtr->fillPixel(dst, pixel);
    }

    const PixelFormat & Vnc2Image::clientPixelFormat(void) const
    {
        return fbPtr->pixelFormat();
    }
}

int main(int argc, const char** argv)
{
    int res = 0;

    try
    {
        LTSM::Vnc2Image app(argc, argv);
        res = app.start();
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("local exception: %s", err.what());
        LTSM::Application::info("program: %s", "terminate...");
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
