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

#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <chrono>
#include <cstdarg>
#include <clocale>
#include <cstring>
#include <iostream>
#include <filesystem>

#ifdef LTSM_WITH_SYSTEMD
#include <systemd/sd-journal.h>
#endif

#ifdef __UNIX__
#include <signal.h>
#include <syslog.h>
#include <sys/inotify.h>
#endif

#include <cstdio>

#include "ltsm_tools.h"
#include "ltsm_compat.h"
#include "ltsm_application.h"

#if defined(__WIN32__) || defined(__APPLE__)
#define LOG_EMERG       0       /* system is unusable */
#define LOG_ALERT       1       /* action must be taken immediately */
#define LOG_CRIT        2       /* critical conditions */
#define LOG_ERR         3       /* error conditions */
#define LOG_WARNING     4       /* warning conditions */
#define LOG_NOTICE      5       /* normal but significant condition */
#define LOG_INFO        6       /* informational */
#define LOG_DEBUG       7       /* debug-level messages */

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

using namespace std::chrono_literals;

namespace LTSM
{
    // local application
    std::unique_ptr<FILE, int(*)(FILE*)> appLoggingFd{ nullptr, fclose };
    DebugTarget appDebugTarget = DebugTarget::Console;
    DebugLevel appDebugLevel = DebugLevel::Info;
    uint32_t appDebugTypes = DebugType::All;

    std::mutex appLoggingLock;

    std::string ident{"application"};
    int facility = LOG_USER;
    bool forceSyslog = false;

    uint32_t debugListToTypes(const std::list<std::string> & typesList)
    {
        uint32_t types = 0;

        for(const auto & val : typesList)
        {
            auto slower = Tools::lower(val);

            if(slower == "xcb")
            {
                types |= DebugType::Xcb;
            }
            else if(slower == "rfb")
            {
                types |= DebugType::Rfb;
            }
            else if(slower == "clip")
            {
                types |= DebugType::Clip;
            }
            else if(slower == "sock")
            {
                types |= DebugType::Sock;
            }
            else if(slower == "tls")
            {
                types |= DebugType::Tls;
            }
            else if(slower == "chnl")
            {
                types |= DebugType::Channels;
            }
            else if(slower == "dbus")
            {
                types |= DebugType::Dbus;
            }
            else if(slower == "enc")
            {
                types |= DebugType::Enc;
            }
            else if(slower == "x11srv")
            {
                types |= DebugType::X11Srv;
            }
            else if(slower == "x11cli")
            {
                types |= DebugType::X11Cli;
            }
            else if(slower == "audio")
            {
                types |= DebugType::Audio;
            }
            else if(slower == "fuse")
            {
                types |= DebugType::Fuse;
            }
            else if(slower == "pcsc")
            {
                types |= DebugType::Pcsc;
            }
            else if(slower == "pkcs11")
            {
                types |= DebugType::Pkcs11;
            }
            else if(slower == "sdl")
            {
                types |= DebugType::Sdl;
            }
            else if(slower == "app")
            {
                types |= DebugType::App;
            }
            else if(slower == "ldap")
            {
                types |= DebugType::Ldap;
            }
            else if(slower == "gss")
            {
                types |= DebugType::Gss;
            }
            else if(slower == "all")
            {
                types |= DebugType::All;
            }
            else
            {
                Application::warning("%s: unknown debug marker: `%s'", __FUNCTION__, slower.c_str());
            }
        }

        return types;
    }

    void setDebugSyslogFacility(std::string_view name)
    {
        if(startsWith(name, "local"))
        {
            switch(name[5])
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

    void Application::setDebugTypes(const std::list<std::string> & list)
    {
        appDebugTypes = debugListToTypes(list);
    }

    void Application::setDebugTarget(const DebugTarget & tgt)
    {
        if(appDebugTarget == DebugTarget::SyslogFile && tgt != DebugTarget::SyslogFile)
        {
            appLoggingFd.reset();
        }

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
            appLoggingFd.reset(fopen(tmp.c_str(), "a"));
#else
            appLoggingFd.reset(fopen(file.c_str(), "a"));
#endif
            setDebugTarget(appLoggingFd ? DebugTarget::SyslogFile : DebugTarget::Console);
        }
    }

    bool Application::isDebugLevel(const DebugLevel & lvl)
    {
        if(appDebugLevel == DebugLevel::Trace)
        {
            return true;
        }

        if(appDebugLevel == DebugLevel::Debug && lvl == DebugLevel::Info)
        {
            return true;
        }

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
        std::setlocale(LC_ALL, "ru_RU.utf8");
        std::setlocale(LC_NUMERIC, "C");

        ident.assign(sid.begin(), sid.end());
#ifdef LTSM_WITH_SYSTEMD
        // check systemd or docker
        auto res = Tools::runcmd("systemctl is-system-running");

        // systemd not running switch to syslog
        if(res.empty() || res == "offline")
        {
            forceSyslog = true;
        }

#endif
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


#ifdef __UNIX__
    int Application::forkMode(void)
    {
        pid_t pid = fork();

        if(pid < 0)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fork", strerror(errno), errno);
            throw std::runtime_error(NS_FuncName);
        }

        // parent mode
        if(0 < pid)
        {
            return pid;
        }

        // child mode
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        // skip closelog, glibc dead lock
        if(isDebugTarget(DebugTarget::Syslog))
        {
            appDebugTarget = DebugTarget::Quiet;
        }

        return pid;
    }
#endif

    void toPlatformSyslog(int priority, const char* format, va_list args)
    {
#ifdef __UNIX__
#ifdef LTSM_WITH_SYSTEMD

        if(! forceSyslog)
        {
            sd_journal_printv(priority, format, args);
            return;
        }

#endif
        vsyslog(priority, format, args);
#else
        vfprintf(appLoggingFd ? appLoggingFd.get() : stderr, format, args);
#endif
    }

    void Application::info(const char* format, ...)
    {
        if(appDebugLevel != DebugLevel::None)
        {
            va_list args;
            va_start(args, format);

            if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile)
            {
                const std::scoped_lock guard{ appLoggingLock };

                FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
                fprintf(fd, "[info] ");
                vfprintf(fd, format, args);
                fprintf(fd, "\n");
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

        if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile)
        {
            const std::scoped_lock guard{ appLoggingLock };

            FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
            fprintf(fd, "[notice] ");
            vfprintf(fd, format, args);
            fprintf(fd, "\n");
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

            if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile)
            {
                const std::scoped_lock guard{ appLoggingLock };

                FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
                fprintf(fd, "[warning] ");
                vfprintf(fd, format, args);
                fprintf(fd, "\n");
                fflush(fd);
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

        if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile)
        {
            const std::scoped_lock guard{ appLoggingLock };

            FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
            fprintf(fd, "[error] ");
            vfprintf(fd, format, args);
            fprintf(fd, "\n");
            fflush(fd);
        }
        else if(appDebugTarget == DebugTarget::Syslog)
        {
            toPlatformSyslog(LOG_ERR, format, args);
        }

        va_end(args);
    }

    void Application::vdebug(uint32_t subsys, const char* format, va_list args)
    {
        if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile)
        {
            const std::scoped_lock guard{ appLoggingLock };

            FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
            fprintf(fd, "[debug] ");
            vfprintf(fd, format, args);
            fprintf(fd, "\n");
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
        if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile)
        {
            const std::scoped_lock guard{ appLoggingLock };

            FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
            fprintf(fd, "[trace] ");
            vfprintf(fd, format, args);
            fprintf(fd, "\n");
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

#ifdef LTSM_WITH_JSON
    // ApplicationLog
    ApplicationLog::ApplicationLog(std::string_view sid)
        : Application(sid)
    {
        const char* applog = getenv("LTSM_APPLOG");

        if(! applog)
        {
            applog = "/etc/ltsm/applog.json";
        }

        if(auto jc = JsonContentFile(applog); jc.isObject())
        {
            if(auto jo = jc.toObject(); jo.isObject(ident))
            {
                setAppLog(jo.getObject(ident));
            }
        }
    }

    void ApplicationLog::setAppLog(const JsonObject* jo)
    {
        setDebugTarget(jo->getString("debug:target", "console"));
        setDebugLevel(jo->getString("debug:level", "info"));

        if(isDebugTarget(DebugTarget::Syslog))
        {
            auto facility = jo->getString("debug:syslog", "user");
            setDebugSyslogFacility(facility);
        }
        else if(isDebugTarget(DebugTarget::SyslogFile))
        {
            if(auto file = jo->getString("debug:file"); ! file.empty())
            {
                setDebugTargetFile(file);
            }
            else
            {
                setDebugTarget(DebugTarget::Console);
            }
        }

        if(auto types = jo->getArray("debug:types"))
        {
            setDebugTypes(types->toStdList<std::string>());
        }
    }

    // WatchModification
    WatchModification::~WatchModification()
    {
        inotifyWatchStop();
    }

#ifdef __UNIX__
    void inotifyWatchCb(int fd, std::string filename, WatchModification* owner)
    {
        std::array<char, 256> buf = {};

        while(true)
        {
            auto len = read(fd, buf.data(), buf.size());

            if(len < 0)
            {
                if(errno == EAGAIN)
                    continue;

                if(errno != EBADF)
                {
                    Application::error("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                        __FUNCTION__, "inotify read", strerror(errno), errno, filename.c_str());
                }

                break;
            }

            if(len < sizeof(struct inotify_event))
            {
                Application::error("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                    __FUNCTION__, "inotify read", strerror(errno), errno, filename.c_str());
                break;
            }

            auto beg = buf.begin();
            auto end = buf.begin() + len;

            while(beg < end)
            {
                auto st = (const struct inotify_event*) std::addressof(*beg);

                if(st->mask == IN_CLOSE_WRITE)
                {
                    if(st->len && owner->inotifyWatchTarget(st->name))
                        owner->closeWriteEvent(filename);
                }

                beg += sizeof(struct inotify_event) + st->len;
            }
        }
    }

    bool WatchModification::inotifyWatchTarget(std::string_view name) const
    {
        return _fileName == name;
    }

    bool WatchModification::inotifyWatchStart(const std::filesystem::path & file)
    {
        if(! std::filesystem::is_regular_file(file))
        {
            Application::error("%s: path not found: `%s'", __FUNCTION__, file.c_str());
            return false;
        }

        _inotifyFd = inotify_init();

        if(0 > _inotifyFd)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                __FUNCTION__, "inotify_init", strerror(errno), errno);
            return false;
        }

        _fileName = file.filename();
        _inotifyWd = inotify_add_watch(_inotifyFd, file.parent_path().c_str(), IN_CLOSE_WRITE);

        if(0 > _inotifyWd)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                __FUNCTION__, "inotify_add_watch", strerror(errno), errno, file.c_str());

            inotifyWatchStop();
            return false;
        }

        _inotifyJob = std::thread(& inotifyWatchCb, _inotifyFd, file.native(), this );

        Application::debug(DebugType::App, "%s: path: `%s'", __FUNCTION__, file.c_str());
        return true;
    }

    void WatchModification::inotifyWatchStop(void)
    {
        if(0 <= _inotifyWd)
            inotify_rm_watch(_inotifyFd, _inotifyWd);

        if(0 <= _inotifyFd)
            close(_inotifyFd);

        _inotifyFd = -1;
        _inotifyWd = -1;

        if(_inotifyJob.joinable())
            _inotifyJob.join();
    }
#else
    bool WatchModification::inotifyWatchStart(const std::filesystem::path & file)
    {
        return false;
    }

    void WatchModification::inotifyWatchStop(void)
    {
    }
#endif

    // ApplicationJsonConfig
    ApplicationJsonConfig::ApplicationJsonConfig(std::string_view ident, const std::filesystem::path & fconf)
        : ApplicationLog(ident)
    {
        if(fconf.empty() || ! std::filesystem::exists(fconf))
        {
            readDefaultConfig();
        }
        else
        {
            readConfig(fconf);
        }
    }

    void ApplicationJsonConfig::readDefaultConfig(void)
    {
        std::list<std::filesystem::path> files;

        if(auto env = std::getenv("LTSM_CONFIG"))
        {
            files.emplace_back(env);
        }

        auto ident_json = Tools::joinToString(ident, ".json");
        files.emplace_back(std::filesystem::current_path() / ident_json);

        auto ident_conf = std::filesystem::path("/etc/ltsm") / ident_json;
        files.emplace_back(std::move(ident_conf));

        files.emplace_back(std::filesystem::current_path() / "config.json");
        files.emplace_back("/etc/ltsm/config.json");

        for(const auto & path : files)
        {
            auto st = std::filesystem::status(path);

            if(std::filesystem::file_type::not_found == st.type())
            {
                continue;
            }

            if(readConfig(path))
            {
                break;
            }
        }
    }

    bool ApplicationJsonConfig::readConfig(const std::filesystem::path & file)
    {
        std::error_code err;

        if(! std::filesystem::exists(file, err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), file.c_str(), getuid());
            return false;
        }

        if((std::filesystem::status(file, err).permissions() & std::filesystem::perms::owner_read)
                == std::filesystem::perms::none)
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "permission failed"), file.c_str(), getuid());
            return false;
        }


        Application::info("%s: path: `%s', uid: %d", __FUNCTION__, file.c_str(), getuid());
        JsonContentFile jsonFile(file);

        if(! jsonFile.isValid() || ! jsonFile.isObject())
        {
            Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "json object", file.c_str());
            return false;
        }

        json.swap(jsonFile.toObject());
        json.addString("config:path", file.native());

        return true;
    }

    bool ApplicationJsonConfig::inotifyWatchStart(void)
    {
        return WatchModification::inotifyWatchStart(configGetString("config:path"));
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

    const JsonObject & ApplicationJsonConfig::config(void) const
    {
        return json;
    }

    void ApplicationJsonConfig::closeWriteEvent(const std::string & file)
    {
        if(readConfig(file))
            configReloadedEvent();
    }
#endif
}
