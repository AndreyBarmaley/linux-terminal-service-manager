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

#include <signal.h>

#include <thread>
#include <chrono>

#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_x11vnc.h"
#include "ltsm_connector_x11vnc.h"

namespace LTSM
{
    //
    void connectorHelp(const char* prog)
    {
        std::cout << "version: " << LTSM_X11VNC_VERSION << std::endl;
        std::cout << "usage: " << prog <<
                  " [--display :0] --authfile <file> --passwdfile <file> [--keymapfile <file>] [--debug <info|debug>] [--inetd] [--noauth] [--notls] [--threads 2] [--port 5900] [--syslog] [--background] [--nodamage] [+DesktopResized] [+ClipBoard]"
                  << std::endl;
    }

    /* X11Vnc */
    X11Vnc::X11Vnc(int argc, const char** argv)
        : ApplicationJsonConfig("ltsm_x11vnc")
    {
        configSetInteger("display", 0);
        configSetInteger("port", 5900);
        configSetInteger("threads", 2);
        configSetBoolean("inetd", false);
        configSetBoolean("syslog", false);
        configSetBoolean("background", false);
        configSetBoolean("noauth", false);
        configSetBoolean("notls", false);
        configSetBoolean("nodamage", false);
        configSetBoolean("DesktopResized", false);
        configSetBoolean("ClipBoard", false);

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                connectorHelp(argv[0]);
                throw 0;
            }
            else if(0 == std::strcmp(argv[it], "--display") && it + 1 < argc)
            {
                auto str = argv[it + 1];

                if(str[0] == ':')
                {
                    configSetInteger("display", std::stoi(str + 1));
                }
                else
                {
                    configSetInteger("display", std::stoi(str));
                }

                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--authfile") && it + 1 < argc)
            {
                configSetString("authfile", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--passwdfile") && it + 1 < argc)
            {
                configSetString("passwdfile", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--keymapfile") && it + 1 < argc)
            {
                configSetString("keymapfile", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--debug") && it + 1 < argc)
            {
                configSetString("debug", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--threads") && it + 1 < argc)
            {
                auto str = argv[it + 1];
                configSetInteger("threads", std::stoi(str[0] == ':' ? str + 1 : str));
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--port") && it + 1 < argc)
            {
                configSetInteger("port", std::stoi(argv[it + 1]));
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--noauth"))
            {
                configSetBoolean("noauth", true);
            }
            else if(0 == std::strcmp(argv[it], "--inetd"))
            {
                configSetBoolean("inetd", true);
            }
            else if(0 == std::strcmp(argv[it], "--notls"))
            {
                configSetBoolean("notls", true);
            }
            else if(0 == std::strcmp(argv[it], "--syslog"))
            {
                configSetBoolean("syslog", true);
            }
            else if(0 == std::strcmp(argv[it], "--background"))
            {
                configSetBoolean("background", true);
            }
            else if(0 == std::strcmp(argv[it], "--nodamage"))
            {
                configSetBoolean("nodamage", true);
            }
            else if(0 == std::strcmp(argv[it], "+DesktopResized"))
            {
                configSetBoolean("DesktopResized", true);
            }
            else if(0 == std::strcmp(argv[it], "+ClipBoard"))
            {
                configSetBoolean("ClipBoard", true);
            }
        }

        bool error = false;

        if(configGetBoolean("inetd"))
        {
            configSetBoolean("syslog", true);
        }

        Application::setDebug(LTSM::DebugTarget::Console, LTSM::DebugLevel::None);

        if(configGetBoolean("syslog"))
        {
            Application::setDebugTarget(LTSM::DebugTarget::Syslog);
            auto debug = configGetString("debug");

            if(! debug.empty())
            {
                Application::setDebugLevel(debug);
            }
        }

        if(1)
        {
            std::string file = configGetString("authfile");

            if(! file.empty() && ! std::filesystem::exists(file))
            {
                Application::warning("authfile not found: `%s'", file.c_str());
            }
        }

        if(! configGetBoolean("noauth"))
        {
            std::string file = configGetString("passwdfile");

            if(file.empty())
            {
                Application::error("error: %s", "passwdfile not defined");
                error = true;
            }
            else if(! std::filesystem::exists(file))
            {
                Application::error("passwdfile not found: `%s'", file.c_str());
                error = true;
            }
        }

        if(error)
        {
            std::cout << std::endl;
            connectorHelp(argv[0]);
            throw 0;
        }
    }

    int X11Vnc::startSocket(int port) const
    {
        int fd = TCPSocket::listen(port);

        if(fd < 0)
        {
            return -1;
        }

        Application::info("listen inet port: %d", port);
        signal(SIGCHLD, SIG_IGN);

        while(int sock = TCPSocket::accept(fd))
        {
            if(0 > sock)
            {
                return -1;
            }

            // child
            if(0 == fork())
            {
                openChildSyslog();
                close(fd);
                int res = EXIT_FAILURE;

                try
                {
                    auto connector = std::make_unique<Connector::X11VNC>(sock, config());
                    res = connector->rfbCommunication();
                }
                catch(const std::exception & err)
                {
                    Application::error("%s: exception: %s", __FUNCTION__, err.what());
                }

                close(sock);
                // exit child
                return res;
            }

            close(sock);
        }

        close(fd);
        return 0;
    }

    int X11Vnc::startInetd(void) const
    {
        int res = EXIT_FAILURE;

        try
        {
            auto connector = std::make_unique<Connector::X11VNC>(-1, config());
            res = connector->rfbCommunication();
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }

        return res;
    }

    int X11Vnc::start(void)
    {
        Application::info("x11vnc version: %d", LTSM_X11VNC_VERSION);

        if(configGetBoolean("background") && fork())
        {
            return 0;
        }

        return configGetBoolean("inetd") ?
               startInetd() : startSocket(configGetInteger("port"));
    }
}

int main(int argc, const char** argv)
{
    int res = 0;

    try
    {
        LTSM::X11Vnc app(argc, argv);
        res = app.start();
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());
        LTSM::Application::info("program: %s", "terminate...");
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
