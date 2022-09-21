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

#include <sys/types.h>
#include <pwd.h>

#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <thread>
#include <chrono>
#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_font_psf.h"
#include "ltsm_connector.h"

#include "ltsm_connector_vnc.h"
#ifdef LTSM_WITH_RDP
#include "ltsm_connector_rdp.h"
#endif
#ifdef LTSM_WITH_SPICE
#include "ltsm_connector_spice.h"
#endif

using namespace std::chrono_literals;

namespace LTSM
{
    //
    void connectorHelp(const char* prog)
    {
        std::list<std::string> proto = { "VNC" };
#ifdef LTSM_WITH_RDP
        proto.emplace_back("RDP");
#endif
#ifdef LTSM_WITH_SPICE
        proto.emplace_back("SPICE");
#endif

        if(1 < proto.size())
            proto.emplace_back("AUTO");

        std::cout << "usage: " << prog << " --config <path> --type <" << Tools::join(proto, "|") << ">" << std::endl;
    }

    /* Connector::Service */
    Connector::Service::Service(int argc, const char** argv)
        : ApplicationJsonConfig("ltsm_connector", argc, argv), _type("auto")
    {
        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                connectorHelp(argv[0]);
                throw 0;
            }
            else if(0 == std::strcmp(argv[it], "--type") && it + 1 < argc)
            {
                _type = Tools::lower(argv[it + 1]);
                it = it + 1;
            }
        }
    }

    int autoDetectType(void)
    {
        auto fd = fileno(stdin);
        struct pollfd fds = {0};
        fds.fd = fd;
        fds.events = POLLIN;

        // has input
        if(0 < poll(& fds, 1, 1))
        {
            int val = std::fgetc(stdin);
            std::ungetc(val, stdin);
            return val;
        }

        return -1;
    }

    std::string Connector::homeRuntime(void)
    {
        std::string home("/tmp");

        if(struct passwd* st = getpwuid(getuid()))
            home.assign(st->pw_dir);

        return home;
    }

    int Connector::Service::start(void)
    {
        auto conn = sdbus::createSystemBusConnection();

        if(! conn)
        {
            Application::error("%s: %s", "Service::start", "dbus create connection failed");
            return EXIT_FAILURE;
        }

        std::unique_ptr<SignalProxy> connector;
        Application::setDebugLevel(_config.getString("connector:debug"));
        auto uid = getuid();
        Application::info("connector version: %d", LTSM::service_version);
        //if(0 < uid)
        {
            auto home = Connector::homeRuntime();
            Application::debug("uid: %d, gid: %d, working dir: %s", uid, getgid(), home.c_str());

            if(0 != chdir(home.c_str()))
                Application::warning("chdir failed, dir: %s, error: %s", home.c_str(), strerror(errno));
        }

        // protocol up
        if(_type == "auto")
        {
            _type = "vnc";
            int first = autoDetectType();
#ifdef LTSM_WITH_RDP

            if(first  == 0x03)
                _type = "rdp";

#endif
#ifdef LTSM_WITH_SPICE

            if(first == 0x52)
                _type = "spice";

#endif
        }

#ifdef LTSM_WITH_RDP

        if(_type == "rdp")
            connector.reset(new Connector::RDP(conn.get(), _config));

#endif
#ifdef LTSM_WITH_SPICE

        if(_type == "spice")
            connector.reset(new Connector::SPICE(conn.get(), _config));

#endif

        if(! connector)
            connector.reset(new Connector::VNC(conn.get(), _config));

        int res = 0;

        try
        {
            res = connector->communication();
        }
        catch(const std::exception & err)
        {
            Application::error("connector exception: %s", err.what());
            // terminated connection: exit normal
            res = EXIT_SUCCESS;
        }
        catch(...)
        {
            Application::error("connector exception: %s", "unknown");
            res = EXIT_FAILURE;
        }

        return res;
    }

    /* Connector::SignalProxy */
    Connector::SignalProxy::SignalProxy(sdbus::IConnection* conn, const JsonObject & jo, const char* type)
        : ProxyInterfaces(*conn, LTSM::dbus_service_name, LTSM::dbus_object_path), _conn(conn), _config(& jo), _display(0),
          _conntype(type), _xcbDisableMessages(true)
    {
        _remoteaddr.assign("local");

        if(auto env = std::getenv("REMOTE_ADDR"))
            _remoteaddr.assign(env);
    }

    std::string Connector::SignalProxy::checkFileOption(const std::string & param) const
    {
        auto fileName = _config->getString(param);

        if(! fileName.empty() && ! std::filesystem::exists(fileName))
        {
            Application::error("file not found: `%s'", fileName.c_str());
            fileName.clear();
        }

        return fileName;
    }

    bool Connector::SignalProxy::isAllowXcbMessages(void) const
    {
        return ! _xcbDisableMessages;
    }

    void Connector::SignalProxy::setEnableXcbMessages(bool f)
    {
        _xcbDisableMessages = ! f;
    }

    bool Connector::SignalProxy::xcbConnect(int screen)
    {
        std::string xauthFile = busCreateAuthFile(screen);
        Application::debug("%s: uid: %d, euid: %d, gid: %d, egid: %d", __FUNCTION__, getuid(), geteuid(), getgid(), getegid());
        Application::debug("%s: xauthfile request: `%s'", __FUNCTION__, xauthFile.c_str());
        // Xvfb: wait display starting
        setenv("XAUTHORITY", xauthFile.c_str(), 1);
        std::string socketFormat = _config->getString("xvfb:socket");
        std::filesystem::path socketPath = Tools::replace(socketFormat, "%{display}", screen);
        int width = _config->getInteger("default:width");
        int height = _config->getInteger("default:height");

        if(! Tools::waitCallable<std::chrono::milliseconds>(5000, 100, [&](){ return ! Tools::checkUnixSocket(socketPath); }))
            Application::error("SignalProxy::xcbConnect: checkUnixSocket failed, `%s'", socketPath.c_str());

        try
        {
            _xcbDisplay.reset(new XCB::RootDisplayExt(Tools::StringFormat(":%1").arg(screen)));
        }
        catch(const std::exception & err)
        {
            Application::error("xcb exception: %s", err.what());
            return false;
        }

        Application::info("xcb display info, size: [%d,%d], depth: %d", _xcbDisplay->width(), _xcbDisplay->height(), _xcbDisplay->depth());
        int color = _config->getInteger("display:solid", 0x4e7db7);

        if(0 != color)
            _xcbDisplay->fillBackground((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

        if(0 < width && 0 < height && _xcbDisplay->size() != XCB::Size(width, height))
            _xcbDisplay->setRandrScreenSize(width, height);

        _display = screen;
        return true;
    }

    void Connector::SignalProxy::onLoginSuccess(const int32_t & display, const std::string & userName)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: login success, display: %d, username: %s", display, userName.c_str());
            // disable message loop
            bool extDisable = _xcbDisableMessages;
            _xcbDisableMessages = true;
            _xcbDisplay->resetInputs();
            int oldDisplay = _display;
            int newDisplay = busStartUserSession(oldDisplay, userName, _remoteaddr, _conntype);

            if(newDisplay < 0)
                throw std::runtime_error("user session request failure");

            if(newDisplay != oldDisplay)
            {
                // wait xcb old operations ended
                std::this_thread::sleep_for(100ms);

                if(! xcbConnect(newDisplay))
                    throw std::runtime_error("xcb connect failed");

                busConnectorSwitched(oldDisplay, newDisplay);
                _display = newDisplay;
            }

            //
            if(! extDisable)
                _xcbDisableMessages = false;
        }
    }

    void Connector::SignalProxy::onClearRenderPrimitives(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::debug("dbus signal: clear render primitives, display: %d", display);

            for(auto & ptr : _renderPrimitives)
            {
                if(ptr->type == RenderType::RenderRect)
                {
                    if(auto prim = static_cast<RenderRect*>(ptr.get()))
                        onAddDamage(prim->toRegion());
                }
                else if(ptr->type == RenderType::RenderText)
                {
                    if(auto prim = static_cast<RenderText*>(ptr.get()))
                        onAddDamage(prim->toRegion());
                }
            }

            _renderPrimitives.clear();
        }
    }

    void Connector::SignalProxy::onAddRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill)
    {
        if(0 < _display && display == _display)
        {
            Application::debug("dbus signal: add fill rect, display: %d", display);
            _renderPrimitives.emplace_back(std::make_unique<RenderRect>(rect, color, fill));
            const int16_t rx = std::get<0>(rect);
            const int16_t ry = std::get<1>(rect);
            const uint16_t rw = std::get<2>(rect);
            const uint16_t rh = std::get<3>(rect);
            onAddDamage({rx, ry, rw, rh});
        }
    }

    void Connector::SignalProxy::onAddRenderText(const int32_t & display, const std::string & text, const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color)
    {
        if(0 < _display && display == _display)
        {
            Application::debug("dbus signal: add render text, display: %d", display);
            const int16_t rx = std::get<0>(pos);
            const int16_t ry = std::get<1>(pos);
            const uint16_t rw = _systemfont.width * text.size();
            const uint16_t rh = _systemfont.height;
            const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> rt{rx, ry, rw, rh};
            _renderPrimitives.emplace_back(std::make_unique<RenderText>(text, rt, color));
            onAddDamage({rx, ry, rw, rh});
        }
    }

    void Connector::SignalProxy::onPingConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            std::thread([=]()
            {
                this->busConnectorAlive(display);
            }).detach();
        }
    }

    void Connector::SignalProxy::onDebugLevel(const std::string & level)
    {
        Application::info("dbus signal: debug level: %s", level.c_str());
        Application::setDebugLevel(level);
    }

    void Connector::SignalProxy::onAddDamage(const XCB::Region & reg)
    {
        if(isAllowXcbMessages())
            _xcbDisplay->damageAdd(reg);
    }

    void Connector::SignalProxy::renderPrimitivesToFB(FrameBuffer & fb) const
    {
        for(auto & ptr : _renderPrimitives)
        {
            switch(ptr->type)
            {
                case RenderType::RenderRect:
                    if(auto prim = static_cast<RenderRect*>(ptr.get()))
                    {
                        XCB::Region section;

                        if(XCB::Region::intersection(fb.region(), prim->toRegion(), & section))
                        {
                            if(prim->fill)
                                fb.fillColor(section - fb.region().topLeft(), Color(prim->color));
                            else
                                fb.drawRect(section - fb.region().topLeft(), Color(prim->color));
                        }
                    }

                    break;

                case RenderType::RenderText:
                    if(auto prim = static_cast<RenderText*>(ptr.get()))
                    {
                        const XCB::Region reg = prim->toRegion();

                        if(XCB::Region::intersection(fb.region(), reg, nullptr))
                            fb.renderText(prim->text, Color(prim->color), reg.topLeft() - fb.region().topLeft());
                    }

                    break;

                default:
                    break;
            }
        }
    }
}

int main(int argc, const char** argv)
{
    LTSM::Application::setDebugLevel(LTSM::DebugLevel::SyslogInfo);
    int res = 0;

    try
    {
        LTSM::Connector::Service app(argc, argv);
        res = app.start();
    }
    catch(const sdbus::Error & err)
    {
        LTSM::Application::error("sdbus exception: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
        LTSM::Application::info("program: %s", "terminate...");
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("other exception: %s", err.what());
        LTSM::Application::info("program: %s", "terminate...");
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
