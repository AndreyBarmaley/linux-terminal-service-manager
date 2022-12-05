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
        LTSM::Application::setDebugLevel(LTSM::DebugLevel::Console);

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
        : ApplicationJsonConfig("ltsm_connector"), _type("auto")
    {
        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--type") && it + 1 < argc)
            {
                _type = Tools::lower(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--config") && it + 1 < argc)
            {
                readConfig(argv[it + 1]);
                it = it + 1;
            }
            else
            {
                connectorHelp(argv[0]);
                throw 0;
            }
        }

        if(! config().isValid())
        {
            LTSM::Application::setDebugLevel(LTSM::DebugLevel::Console);

            Application::error("%s: %s failed", "Connector", "config");
            throw std::invalid_argument("Connector");
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

        Application::setDebugLevel(configGetString("connector:debug"));
        auto uid = getuid();
        Application::info("%s: runtime version: %d", __FUNCTION__, LTSM::service_version);
        //if(0 < uid)
        {
            auto home = Connector::homeRuntime();
            Application::debug("%s: uid: %d, gid: %d, working dir: %s", __FUNCTION__, uid, getgid(), home.c_str());

            if(0 != chdir(home.c_str()))
                Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "chdir", strerror(errno), errno);
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

        std::unique_ptr<SignalProxy> connector;

#ifdef LTSM_WITH_RDP

        if(_type == "rdp")
            connector.reset(new Connector::RDP(config()));

#endif
#ifdef LTSM_WITH_SPICE

        if(_type == "spice")
            connector.reset(new Connector::SPICE(config()));

#endif

        if(! connector)
            connector.reset(new Connector::VNC(config()));

        int res = 0;

        try
        {
            res = connector->communication();
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
            // terminated connection: exit normal
            res = EXIT_SUCCESS;
        }

        return res;
    }

    /* Connector::SignalProxy */
    Connector::SignalProxy::SignalProxy(const JsonObject & jo, const char* type)
        : ProxyInterfaces(sdbus::createSystemBusConnection(), LTSM::dbus_manager_service_name, LTSM::dbus_manager_service_path), _conntype(type), _config(& jo)
    {
        _remoteaddr.assign("local");

        if(auto env = std::getenv("REMOTE_ADDR"))
            _remoteaddr.assign(env);

        registerProxy();
    }

    Connector::SignalProxy::~SignalProxy()
    {
        unregisterProxy();
    }

    bool Connector::SignalProxy::xcbConnect(int screen, XCB::RootDisplay & xcbDisplay)
    {
        Application::info("%s: display: %d", __FUNCTION__, screen);
        std::string xauthFile = busCreateAuthFile(screen);
        
        Application::info("%s: display: %d, xauthfile: %s, uid: %d, gid: %d", __FUNCTION__, screen, xauthFile.c_str(), getuid(), getgid());
        
        setenv("XAUTHORITY", xauthFile.c_str(), 1);
        std::string socketFormat = _config->getString("xvfb:socket", "/tmp/.X11-unix/X%{display}");
        std::filesystem::path socketPath = Tools::replace(socketFormat, "%{display}", screen);
            
        // wait display starting
        if(! Tools::waitCallable<std::chrono::milliseconds>(5000, 100, [&](){ return ! Tools::checkUnixSocket(socketPath); }))
            Application::error("%s: checkUnixSocket failed, `%s'", "xcbConnect", socketPath.c_str());

        try
        {
            xcbDisplay.reconnect(screen);
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
            return false;
        }
        
        xcbDisplay.resetInputs();

        auto defaultSz = XCB::Size(_config->getInteger("default:width", 0),
                                    _config->getInteger("default:height", 0));
        auto displaySz = xcbDisplay.size();
        int color = _config->getInteger("display:solid", 0x4e7db7);

        Application::debug("%s: display: %d, size: [%d,%d], depth: %d", __FUNCTION__, screen, displaySz.width, displaySz.height, xcbDisplay.depth());

        if(0 != color)
            xcbDisplay.fillBackground((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

        if(! defaultSz.isEmpty() && displaySz != defaultSz)
            xcbDisplay.setRandrScreenSize(defaultSz);

        _xcbDisplayNum = screen;
        return true;
    }

    int Connector::SignalProxy::displayNum(void) const
    {
        return _xcbDisplayNum;
    }

    void Connector::SignalProxy::xcbDisableMessages(bool f)
    {
        _xcbDisable = f;
    }
        
    bool Connector::SignalProxy::xcbAllowMessages(void) const
    {
        return ! _xcbDisable;
    }

    std::string Connector::SignalProxy::checkFileOption(const std::string & param) const
    {
        auto fileName = _config->getString(param);
        std::error_code err;

        if(! fileName.empty() && ! std::filesystem::exists(fileName, err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), fileName.c_str(), getuid());
            fileName.clear();
        }

        return fileName;
    }

    void Connector::SignalProxy::onClearRenderPrimitives(const int32_t & display)
    {
        if(display == displayNum())
        {
            Application::debug("%s: display: %d", __FUNCTION__, display);

            for(auto & ptr : _renderPrimitives)
            {
                if(ptr->type == RenderType::RenderRect)
                {
                    if(auto prim = static_cast<RenderRect*>(ptr.get()))
                        xcbAddDamage(prim->toRegion());
                }
                else
                if(ptr->type == RenderType::RenderText)
                {
                    if(auto prim = static_cast<RenderText*>(ptr.get()))
                        xcbAddDamage(prim->toRegion());
                }
            }

            _renderPrimitives.clear();
        }
    }

    void Connector::SignalProxy::onAddRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill)
    {
        if(display == displayNum())
        {
            Application::debug("%s: display: %d", __FUNCTION__, display);
            _renderPrimitives.emplace_back(std::make_unique<RenderRect>(rect, color, fill));
            const int16_t rx = std::get<0>(rect);
            const int16_t ry = std::get<1>(rect);
            const uint16_t rw = std::get<2>(rect);
            const uint16_t rh = std::get<3>(rect);
            xcbAddDamage({rx, ry, rw, rh});
        }
    }

    void Connector::SignalProxy::onAddRenderText(const int32_t & display, const std::string & text, const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color)
    {
        if(display == displayNum())
        {
            Application::debug("%s: display: %d", __FUNCTION__, display);
            const int16_t rx = std::get<0>(pos);
            const int16_t ry = std::get<1>(pos);
            const uint16_t rw = _systemfont.width * text.size();
            const uint16_t rh = _systemfont.height;
            const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> rt{rx, ry, rw, rh};
            _renderPrimitives.emplace_back(std::make_unique<RenderText>(text, rt, color));
            xcbAddDamage({rx, ry, rw, rh});
        }
    }

    void Connector::SignalProxy::onPingConnector(const int32_t & display)
    {
        if(display == displayNum())
        {
            std::thread([=]()
            {
                this->busConnectorAlive(display);
            }).detach();
        }
    }

    void Connector::SignalProxy::onDebugLevel(const int32_t & display, const std::string & level)
    {
        if(display == displayNum())
        {
            Application::info("%s: display: %d, level: %s", __FUNCTION__, display, level.c_str());
            Application::setDebugLevel(level);
        }
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
    int res = 0;
    LTSM::Application::setDebugLevel(LTSM::DebugLevel::SyslogInfo);

    try
    {
        LTSM::Connector::Service app(argc, argv);
        res = app.start();
    }
    catch(const sdbus::Error & err)
    {
        LTSM::Application::error("sdbus exception: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
