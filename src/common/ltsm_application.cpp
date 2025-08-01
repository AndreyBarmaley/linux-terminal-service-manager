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

#ifdef LTSM_WITH_SYSTEMD
#include <systemd/sd-journal.h>
#endif

#ifdef __UNIX__
#include <syslog.h>
#endif

#include <cstdio>

#include "ltsm_tools.h"
#include "ltsm_application.h"

#if defined(__WIN32__) || defined(__APPLE__)
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
    // local application
    FILE* appLoggingFd = nullptr;
    DebugTarget appDebugTarget = DebugTarget::Console;
    DebugLevel appDebugLevel = DebugLevel::Info;
    uint32_t appDebugTypes = DebugType::All;

    std::mutex appLoggingLock;

    std::string ident{"application"};
    int facility = LOG_USER;

    bool Application::isDebugTarget(const DebugTarget & tgt)
    {
        return appDebugTarget == tgt;
    }

    bool Application::isDebugTypes(uint32_t vals)
    {
        return appDebugTypes & vals;
    }

    void Application::setDebug(const DebugTarget & tgt, const DebugLevel & lvl)
    {
        setDebugTarget(tgt);

        appDebugLevel = lvl;
    }

    void Application::setDebugTypes(uint32_t val)
    {
        appDebugTypes = val;
    }

    void Application::setDebugTarget(const DebugTarget & tgt)
    {
#ifdef __UNIX__
        if(appDebugTarget != DebugTarget::Syslog && tgt == DebugTarget::Syslog)
        {
            openlog(ident.c_str(), 0, facility);
        }
        else if(appDebugTarget == DebugTarget::Syslog && tgt != DebugTarget::Syslog)
        {
            closelog();
        }
#endif
        appDebugTarget = tgt;
    }

    void Application::setDebugTarget(std::string_view tgt)
    {
        if(tgt == "console")
        {
            setDebugTarget(DebugTarget::Console);
        }
#ifdef __UNIX__
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

    void Application::setDebugTargetFile(const std::filesystem::path & file)
    {
        if(! file.empty())
        {
#ifdef __WIN32__
            auto tmp = Tools::wstring2string(file.native());
            appLoggingFd = fopen(tmp.c_str(), "a");
#else
            appLoggingFd = fopen(file.c_str(), "a");
#endif
            if(! appLoggingFd)
            {
                appLoggingFd = stderr;
            }
        }
    }

    bool Application::isDebugLevel(const DebugLevel & lvl)
    {
        if(appDebugLevel == DebugLevel::Trace)
            return true;

        if(appDebugLevel == DebugLevel::Debug && lvl == DebugLevel::Info)
            return true;

        return appDebugLevel == lvl;
    }

    void Application::setDebugLevel(const DebugLevel & lvl)
    {
        appDebugLevel = lvl;
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
#ifdef __UNIX__
        if(isDebugTarget(DebugTarget::Syslog))
        {
            closelog();
        }
#endif
    }

    void Application::redirectSyslogFile(const char* file)
    {
#ifdef __UNIX__
        if(isDebugTarget(DebugTarget::Syslog))
        {
            closelog();

            if(file)
            {
                appLoggingFd = fopen(file, "a");

                if(! appLoggingFd)
                {
                    appLoggingFd = stderr;
                }
            }

            // child: switch syslog to stderr
            appDebugTarget = DebugTarget::Console;
        }
#endif
    }

    void toPlatformSyslog(int priority, const char* format, va_list args)
    {
#ifdef __UNIX__
 #if defined(LTSM_FORCE_SYSLOG)
        vsyslog(priority, format, args);
 #elif defined(LTSM_WITH_SYSTEMD)
        sd_journal_printv(priority, format, args);
 #else
        vsyslog(priority, format, args);
 #endif
#else
        vfprintf(format, args);
#endif
    }

    void Application::info(const char* format, ...)
    {
        if(appDebugLevel != DebugLevel::None)
        {
            va_list args;
            va_start(args, format);

            if(appDebugTarget == DebugTarget::Console)
            {
                const std::scoped_lock guard{ appLoggingLock };

                if(! appLoggingFd)
                {
                    appLoggingFd = stderr;
                }

                fprintf(appLoggingFd, "[info] ");
                vfprintf(appLoggingFd, format, args);
                fprintf(appLoggingFd, "\n");
            }
            else if(appDebugTarget == DebugTarget::Syslog)
            {
                toPlatformSyslog(LOG_INFO, format, args);
            }

            va_end(args);
        }
    }

    void Application::notice(const char* format, ...)
    {
        va_list args;
        va_start(args, format);

        if(appDebugTarget == DebugTarget::Console)
        {
            const std::scoped_lock guard{ appLoggingLock };

            if(! appLoggingFd)
            {
                appLoggingFd = stderr;
            }

            fprintf(appLoggingFd, "[notice] ");
            vfprintf(appLoggingFd, format, args);
            fprintf(appLoggingFd, "\n");
        }
        else if(appDebugTarget == DebugTarget::Syslog)
        {
            toPlatformSyslog(LOG_NOTICE, format, args);
        }

        va_end(args);
    }

    void Application::warning(const char* format, ...)
    {
        if(appDebugLevel != DebugLevel::None)
        {
            va_list args;
            va_start(args, format);

            if(appDebugTarget == DebugTarget::Console)
            {
                const std::scoped_lock guard{ appLoggingLock };

                if(! appLoggingFd)
                {
                    appLoggingFd = stderr;
                }

                fprintf(appLoggingFd, "[warning] ");
                vfprintf(appLoggingFd, format, args);
                fprintf(appLoggingFd, "\n");
                fflush(appLoggingFd);
            }
            else if(appDebugTarget == DebugTarget::Syslog)
            {
                toPlatformSyslog(LOG_WARNING, format, args);
            }

            va_end(args);
        }
    }

    void Application::error(const char* format, ...)
    {
        va_list args;
        va_start(args, format);

        if(appDebugTarget == DebugTarget::Console)
        {
            const std::scoped_lock guard{ appLoggingLock };

            if(! appLoggingFd)
            {
                appLoggingFd = stderr;
            }

            fprintf(appLoggingFd, "[error] ");
            vfprintf(appLoggingFd, format, args);
            fprintf(appLoggingFd, "\n");
            fflush(appLoggingFd);
        }
        else if(appDebugTarget == DebugTarget::Syslog)
        {
            toPlatformSyslog(LOG_ERR, format, args);
        }

        va_end(args);
    }

    void Application::vdebug(uint32_t subsys, const char* format, va_list args)
    {
        if(appDebugTarget == DebugTarget::Console)
        {
            const std::scoped_lock guard{ appLoggingLock };

            if(! appLoggingFd)
            {
                appLoggingFd = stderr;
            }

            fprintf(appLoggingFd, "[debug] ");
            vfprintf(appLoggingFd, format, args);
            fprintf(appLoggingFd, "\n");
        }
        else if(appDebugTarget == DebugTarget::Syslog)
        {
            toPlatformSyslog(LOG_DEBUG, format, args);
        }
    }

    void Application::debug(uint32_t subsys, const char* format, ...)
    {
        if((subsys & appDebugTypes) &&
            (appDebugLevel == DebugLevel::Debug || appDebugLevel == DebugLevel::Trace))
        {
            va_list args;
            va_start(args, format);

            vdebug(subsys, format, args);

            va_end(args);
        }
    }

    void Application::vtrace(uint32_t subsys, const char* format, va_list args)
    {
        if(appDebugTarget == DebugTarget::Console)
        {
            const std::scoped_lock guard{ appLoggingLock };

            if(! appLoggingFd)
            {
                appLoggingFd = stderr;
            }

            fprintf(appLoggingFd, "[trace] ");
            vfprintf(appLoggingFd, format, args);
            fprintf(appLoggingFd, "\n");
        }
        else if(appDebugTarget == DebugTarget::Syslog)
        {
            toPlatformSyslog(LOG_DEBUG, format, args);
        }
    }

    void Application::trace(uint32_t subsys, const char* format, ...)
    {
        if((subsys & appDebugTypes) &&
            (appDebugLevel == DebugLevel::Trace))
        {
            va_list args;
            va_start(args, format);

            vtrace(subsys, format, args);

            va_end(args);
        }
    }


    // ApplicationJsonConfig
#ifdef LTSM_WITH_JSON
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

            for(const auto & path : files)
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

#ifdef __UNIX__
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
