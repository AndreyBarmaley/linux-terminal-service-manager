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

// shm access flags
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <poll.h>
#include <unistd.h>
#include <signal.h>

#include <cstdio>
#include <thread>
#include <chrono>
#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_x11vnc.h"
#include "ltsm_connector_vnc.h"

using namespace std::chrono_literals;

namespace LTSM
{
    //
    void connectorHelp(const char* prog)
    {
        std::cout << "version: " << LTSM_VNC2IMAGE_VERSION << std::endl;
        std::cout << "usage: " << prog << " --host <localhost> [--port 5900] [--password <pass>] [--timeout 0 (ms)] --image <screenshot.png> [--notls]" << std::endl;
    }

    Vnc2Image::Vnc2Image(int argc, const char** argv)
        : Application("ltsm_vnc2image", argc, argv)
    {
        _config.addString("host", "localhost");
        _config.addString("image", "screenshot.png");
        _config.addInteger("port", 5900);
        _config.addInteger("timeout", 0);
        _config.addBoolean("notls", false);

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                connectorHelp(argv[0]);
                throw 0;
            }
            else if(0 == std::strcmp(argv[it], "--host") && it + 1 < argc)
            {
                _config.addString("host", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--image") && it + 1 < argc)
            {
                _config.addString("image", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--password") && it + 1 < argc)
            {
                _config.addString("password", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--port") && it + 1 < argc)
            {
                _config.addInteger("port", std::stoi(argv[it + 1]));
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--timeout") && it + 1 < argc)
            {
                _config.addInteger("timeout", std::stoi(argv[it + 1]));
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--notls"))
                _config.addBoolean("notls", true);
        }

        LTSM::Application::setDebugLevel(LTSM::DebugLevel::Console);
    }

    int Vnc2Image::start(void)
    {
        Application::info("vnc2image version: %d", LTSM_VNC2IMAGE_VERSION);
        startSocket(_config.getInteger("port"));
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
