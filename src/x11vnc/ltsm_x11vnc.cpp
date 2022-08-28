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
        std::cout << "version: " << LTSM_X11VNC_VERSION << std::endl;
        std::cout << "usage: " << prog << " [--display :0] --authfile <file> --passwdfile <file> [--keymapfile <file>] [--debug <error|info|debug>] [--inetd] [--noauth] [--notls] [--threads 2] [--port 5900] [--syslog] [--background] [--nodamage]" << std::endl;
    }

    /* Connector::Service */
    Connector::Service::Service(int argc, const char** argv)
        : Application("ltsm_x11vnc", argc, argv)
    {
        _config.addString("display", ":0");
        _config.addInteger("port", 5900);
        _config.addInteger("threads", 2);
        _config.addBoolean("inetd", false);
        _config.addBoolean("syslog", false);
        _config.addBoolean("background", false);
        _config.addBoolean("noauth", false);
        _config.addBoolean("notls", false);
        _config.addBoolean("DesktopResized", false);

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                connectorHelp(argv[0]);
                throw 0;
            }
            else if(0 == std::strcmp(argv[it], "--display") && it + 1 < argc)
            {
                _config.addString("display", argv[it + 1]);
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
                _config.addInteger("threads", std::stoi(argv[it + 1]));
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
                _config.addBoolean("xcb:nodamage", true);
            else if(0 == std::strcmp(argv[it], "+DesktopResized"))
                _config.addBoolean("DesktopResized", true);
        }

        bool error = false;

        if(_config.getBoolean("inetd"))
            _config.addBoolean("syslog", true);

        LTSM::Application::setDebugLevel(LTSM::DebugLevel::Console);

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

    int Connector::Service::startSocket(int port) const
    {
        int fd = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

        if(0 > fd)
        {
            Application::error("%s: socket error: %s", __FUNCTION__, strerror(errno));
            return -1;
        }

        struct sockaddr_in sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_in));

        sockaddr.sin_family = AF_INET;

        sockaddr.sin_port = htons(port);

        sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);

        if(0 != bind(fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_in)))
        {
            Application::error("%s: bind error: %s, port: %d", __FUNCTION__, strerror(errno), port);
            return -1;
        }

        if(0 != listen(fd, 5))
        {
            Application::error("%s: listen error: %s", __FUNCTION__, strerror(errno));
            return -1;
        }

        Application::info("listen inet port: %d", port);
        signal(SIGCHLD, SIG_IGN);

        while(int sock = accept(fd, nullptr, nullptr))
        {
            if(0 > sock)
                Application::error("%s: accept error: %s", __FUNCTION__, strerror(errno));
            else
                Application::debug("accept inet sock: %d", sock);

            // child
            if(0 == fork())
            {
                close(fd);
                int res = EXIT_FAILURE;

                try
                {
                    auto connector = std::make_unique<Connector::VNC>(sock, _config);
                    res = connector->communication();
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

    int Connector::Service::startInetd(void) const
    {
        int res = EXIT_FAILURE;

        try
        {
            auto connector = std::make_unique<Connector::VNC>(-1, _config);
            res = connector->communication();
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

    int Connector::Service::start(void)
    {
        Application::info("x11vnc version: %d", LTSM::service_version);

        if(_config.getBoolean("background") && fork())
            return 0;

        return _config.getBoolean("inetd") ?
               startInetd() : startSocket(_config.getInteger("port"));
    }

    /* Connector::DisplayProxy */
    Connector::DisplayProxy::DisplayProxy(const JsonObject & jo)
        : _config(& jo), _xcbDisableMessages(true)
    {
        _remoteaddr.assign("local");

        if(auto env = std::getenv("REMOTE_ADDR"))
            _remoteaddr.assign(env);
    }

    bool Connector::DisplayProxy::xcbConnect(void)
    {
        // FIXM XAUTH
        std::string xauthFile = _config->getString("authfile");
        std::string xcbDisplayAddr = _config->getString("display");
        Application::debug("%s: display addr: `%s'", __FUNCTION__, xcbDisplayAddr.c_str());
        Application::debug("%s: xauthfile: `%s'", __FUNCTION__, xauthFile.c_str());
        // Xvfb: wait display starting
        setenv("XAUTHORITY", xauthFile.c_str(), 1);

        try
        {
            _xcbDisplay.reset(new XCB::RootDisplayExt(xcbDisplayAddr));
        }
        catch(const std::exception & err)
        {
            Application::error("exception: %s", err.what());
            return false;
        }

        _xcbDisplay->resetInputs();
        Application::info("%s: display info, size: [%d,%d], depth: %d", __FUNCTION__, _xcbDisplay->width(), _xcbDisplay->height(), _xcbDisplay->depth());
        return true;
    }

    bool Connector::DisplayProxy::isAllowXcbMessages(void) const
    {
        return ! _xcbDisableMessages;
    }

    void Connector::DisplayProxy::setEnableXcbMessages(bool f)
    {
        _xcbDisableMessages = ! f;
    }
}

int main(int argc, const char** argv)
{
    int res = 0;

    try
    {
        LTSM::Connector::Service app(argc, argv);
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
