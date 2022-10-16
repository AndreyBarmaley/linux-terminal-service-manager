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
        std::cout << "usage: " << prog << " [--display :0] --authfile <file> --passwdfile <file> [--keymapfile <file>] [--debug <error|info|debug>] [--inetd] [--noauth] [--notls] [--threads 2] [--port 5900] [--syslog] [--background] [--nodamage] [+DesktopResized] [+ClipBoard]" << std::endl;
    }

    /* X11Vnc */
    X11Vnc::X11Vnc(int argc, const char** argv)
        : Application("ltsm_x11vnc", argc, argv)
    {
        _config.addInteger("display", 0);
        _config.addInteger("port", 5900);
        _config.addInteger("threads", 2);
        _config.addBoolean("inetd", false);
        _config.addBoolean("syslog", false);
        _config.addBoolean("background", false);
        _config.addBoolean("noauth", false);
        _config.addBoolean("notls", false);
        _config.addBoolean("nodamage", false);
        _config.addBoolean("DesktopResized", false);
        _config.addBoolean("ClipBoard", false);

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                connectorHelp(argv[0]);
                throw 0;
            }
            else if(0 == std::strcmp(argv[it], "--display") && it + 1 < argc)
            {
                _config.addInteger("display", std::stoi(argv[it + 1]));
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--authfile") && it + 1 < argc)
            {
                _config.addString("authfile", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--passwdfile") && it + 1 < argc)
            {
                _config.addString("passwdfile", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--keymapfile") && it + 1 < argc)
            {
                _config.addString("keymapfile", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--debug") && it + 1 < argc)
            {
                _config.addString("debug", argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--threads") && it + 1 < argc)
            {
                auto str = argv[it + 1];
                _config.addInteger("threads", std::stoi(str[0] == ':' ? str + 1 : str));
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--port") && it + 1 < argc)
            {
                _config.addInteger("port", std::stoi(argv[it + 1]));
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--noauth"))
                _config.addBoolean("noauth", true);
            else if(0 == std::strcmp(argv[it], "--inetd"))
                _config.addBoolean("inetd", true);
            else if(0 == std::strcmp(argv[it], "--notls"))
                _config.addBoolean("notls", true);
            else if(0 == std::strcmp(argv[it], "--syslog"))
                _config.addBoolean("syslog", true);
            else if(0 == std::strcmp(argv[it], "--background"))
                _config.addBoolean("background", true);
            else if(0 == std::strcmp(argv[it], "--nodamage"))
                _config.addBoolean("nodamage", true);
            else if(0 == std::strcmp(argv[it], "+DesktopResized"))
                _config.addBoolean("DesktopResized", true);
            else if(0 == std::strcmp(argv[it], "+ClipBoard"))
                _config.addBoolean("ClipBoard", true);
        }

        bool error = false;

        if(_config.getBoolean("inetd"))
            _config.addBoolean("syslog", true);

        LTSM::Application::setDebugLevel(LTSM::DebugLevel::ConsoleError);

        if(_config.getBoolean("syslog"))
        {
            Application::setDebugLevel(LTSM::DebugLevel::SyslogInfo);
            auto debug = _config.getString("debug");

            if(! debug.empty() && debug != "console")
                Application::setDebugLevel(debug);
        }

        if(1)
        {
            std::string file = _config.getString("authfile");

            if(! file.empty() && ! std::filesystem::exists(file))
                Application::warning("authfile not found: `%s'", file.c_str());
        }

        if(! _config.getBoolean("noauth"))
        {
            std::string file = _config.getString("passwdfile");

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
            return -1;

        Application::info("listen inet port: %d", port);
        signal(SIGCHLD, SIG_IGN);

        while(int sock = TCPSocket::accept(fd))
        {
            if(0 > sock)
                return -1;

            // child
            if(0 == fork())
            {
                close(fd);
                int res = EXIT_FAILURE;

                try
                {
                    auto connector = std::make_unique<Connector::X11VNC>(sock, _config);
                    res = connector->rfbCommunication();
                }
                catch(const std::exception & err)
                {
                    Application::error("exception: %s", err.what());
                }
                catch(...)
                {
                    Application::error("exception: %s", "unknown");
                }

                close(sock);
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
            auto connector = std::make_unique<Connector::X11VNC>(-1, _config);
            res = connector->rfbCommunication();
        }
        catch(const std::exception & err)
        {
            Application::error("exception: %s", err.what());
        }
        catch(...)
        {
            Application::error("exception: %s", "unknown");
        }

        return res;
    }

    int X11Vnc::start(void)
    {
        Application::info("x11vnc version: %d", LTSM_X11VNC_VERSION);

        if(_config.getBoolean("background") && fork())
            return 0;

        return _config.getBoolean("inetd") ?
               startInetd() : startSocket(_config.getInteger("port"));
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
        LTSM::Application::error("local exception: %s", err.what());
        LTSM::Application::info("program: %s", "terminate...");
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
