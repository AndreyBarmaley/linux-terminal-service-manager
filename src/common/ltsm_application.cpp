/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <unistd.h>

#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_application.h"

#ifdef __MINGW64__
#define LOG_USER        (1<<3)  /* random user-level messages */
#define LOG_LOCAL0      (16<<3) /* reserved for local use */
#define LOG_LOCAL1      (17<<3) /* reserved for local use */
#define LOG_LOCAL2      (18<<3) /* reserved for local use */
#define LOG_LOCAL3      (19<<3) /* reserved for local use */
#define LOG_LOCAL4      (20<<3) /* reserved for local use */
#define LOG_LOCAL5      (21<<3) /* reserved for local use */
#define LOG_LOCAL6      (22<<3) /* reserved for local use */
#define LOG_LOCAL7      (23<<3) /* reserved for local use */
#endif

namespace LTSM
{
    std::string ident{"application"};
    int facility = LOG_USER;

    // Application
    FILE* Application::fdlog = stderr;
    DebugTarget Application::target = DebugTarget::Console;
    DebugLevel Application::level = DebugLevel::Info;
    uint32_t Application::types = DebugType::All;
    std::mutex Application::logging;

    void Application::setDebug(const DebugTarget & tgt, const DebugLevel & lvl)
    {
        setDebugTarget(tgt);

        level = lvl;
    }

    void Application::setDebugTypes(uint32_t val)
    {
        types = val;
    }

    void Application::setDebugTarget(const DebugTarget & tgt)
    {
#ifdef __LINUX__
        if(target != DebugTarget::Syslog && tgt == DebugTarget::Syslog)
        {
            openlog(ident.c_str(), 0, facility);
        }
        else if(target == DebugTarget::Syslog && tgt != DebugTarget::Syslog)
        {
            closelog();
        }
#endif
        target = tgt;
    }

    bool Application::isDebugTarget(const DebugTarget & tgt)
    {
        return target == tgt;
    }

    void Application::setDebugTarget(std::string_view tgt)
    {
        if(tgt == "console")
        {
            setDebugTarget(DebugTarget::Console);
        }
#ifdef __LINUX__
        else if(tgt == "syslog")
        {
            setDebugTarget(DebugTarget::Syslog);
        }
#endif
        else
        {
            setDebugTarget(DebugTarget::Quiet);
        }
    }

    bool Application::isDebugLevel(const DebugLevel & lvl)
    {
        return level == lvl;
    }

    void Application::setDebugLevel(const DebugLevel & lvl)
    {
        level = lvl;
    }

    void Application::setDebugLevel(std::string_view lvl)
    {
        if(lvl == "info")
        {
            setDebugLevel(DebugLevel::Info);
        }
        else if(lvl == "debug")
        {
            setDebugLevel(DebugLevel::Debug);
        }
        else if(lvl == "trace")
        {
            setDebugLevel(DebugLevel::Trace);
        }
        else
        {
            setDebugLevel(DebugLevel::None);
        }
    }

    Application::Application(std::string_view sid)
    {
        ident.assign(sid.begin(), sid.end());
    }

    Application::~Application()
    {
#ifdef __LINUX__
        if(isDebugTarget(DebugTarget::Syslog))
        {
            closelog();
        }
#endif
    }

    void Application::redirectSyslogFile(const char* file)
    {
#ifdef __LINUX__
        if(isDebugTarget(DebugTarget::Syslog))
        {
            closelog();

            if(file)
            {
                fdlog = fopen(file, "a");

                if(! fdlog)
                {
                    fdlog = stderr;
                }
            }

            // child: switch syslog to stderr
            Application::target = DebugTarget::Console;
        }
#endif
    }

#ifdef WITH_JSON
    ApplicationJsonConfig::ApplicationJsonConfig(std::string_view ident, const char* fconf)
        : Application(ident)
    {
        if(fconf)
        {
            readConfig(fconf);
        }
        else
        {
            std::list<std::filesystem::path> files;

            auto env = std::getenv("LTSM_CONFIG");

            if(env)
            {
                files.emplace_back(env);
            }

            files.emplace_back(std::filesystem::current_path() / "config.json");
            files.emplace_back("/etc/ltsm/config.json");

            for(auto & path : files)
            {
                auto st = std::filesystem::status(path);

                if(std::filesystem::file_type::not_found != st.type() &&
                        (st.permissions() & std::filesystem::perms::owner_read) != std::filesystem::perms::none)
                {
                    readConfig(path);
                    break;
                }
            }
        }
    }

    void ApplicationJsonConfig::readConfig(const std::filesystem::path & file)
    {
        std::error_code err;

        if(! std::filesystem::exists(file, err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), file.c_str(), getuid());
            throw std::invalid_argument(__FUNCTION__);
        }

        if((std::filesystem::status(file, err).permissions() & std::filesystem::perms::owner_read)
                == std::filesystem::perms::none)
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "permission failed"), file.c_str(), getuid());
            throw std::invalid_argument(__FUNCTION__);
        }


        Application::info("%s: path: `%s', uid: %d", __FUNCTION__, file.c_str(), getuid());
        JsonContentFile jsonFile(file);

        if(! jsonFile.isValid() || ! jsonFile.isObject())
        {
            Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "json parse", file.c_str());
            throw std::invalid_argument(__FUNCTION__);
        }

        json = jsonFile.toObject();
        json.addString("config:path", file.native());

        std::string str = json.getString("logging:facility");
        facility = LOG_USER;

        if(6 == str.size() && 0 == str.compare(0, 5, "local"))
        {
            switch(str[5])
            {
                case '0':
                    facility = LOG_LOCAL0;
                    break;

                case '1':
                    facility = LOG_LOCAL1;
                    break;

                case '2':
                    facility = LOG_LOCAL2;
                    break;

                case '3':
                    facility = LOG_LOCAL3;
                    break;

                case '4':
                    facility = LOG_LOCAL4;
                    break;

                case '5':
                    facility = LOG_LOCAL5;
                    break;

                case '6':
                    facility = LOG_LOCAL6;
                    break;

                case '7':
                    facility = LOG_LOCAL7;
                    break;

                default:
                    break;
            }
        }

#ifdef __LINUX__
        if(0 < facility)
        {
            closelog();
            ::openlog(ident.c_str(), 0, facility);
        }
#endif
    }

    void ApplicationJsonConfig::configSetInteger(const std::string & key, int val)
    {
        json.addInteger(key, val);
    }

    void ApplicationJsonConfig::configSetBoolean(const std::string & key, bool val)
    {
        json.addBoolean(key, val);
    }

    void ApplicationJsonConfig::configSetString(const std::string & key, std::string_view val)
    {
        json.addString(key, val);
    }

    void ApplicationJsonConfig::configSetDouble(const std::string & key, double val)
    {
        json.addDouble(key, val);
    }

    int ApplicationJsonConfig::configGetInteger(std::string_view key, int def) const
    {
        return json.getInteger(key, def);
    }

    bool ApplicationJsonConfig::configGetBoolean(std::string_view key, bool def) const
    {
        return json.getBoolean(key, def);
    }

    std::string ApplicationJsonConfig::configGetString(std::string_view key, std::string_view def) const
    {
        return json.getString(key, def);
    }

    double ApplicationJsonConfig::configGetDouble(std::string_view key, double def) const
    {
        return json.getDouble(key, def);
    }

    const JsonObject & ApplicationJsonConfig::config(void) const
    {
        return json;
    }

    void ApplicationJsonConfig::configSet(JsonObject && jo) noexcept
    {
        json.swap(jo);
    }

#endif
}
