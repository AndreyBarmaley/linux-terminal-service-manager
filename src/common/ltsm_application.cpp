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
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <cstdlib>
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

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_sinks.h"

#ifdef __UNIX__
#include "spdlog/sinks/syslog_sink.h"
#ifdef LTSM_WITH_SYSTEMD
#include "spdlog/sinks/systemd_sink.h"
#endif
#endif

using namespace std::chrono_literals;

namespace LTSM {
    // local application
    std::unique_ptr<FILE, int(*)(FILE*)> appLoggingFd{ nullptr, fclose };
    std::string appLoggingFile;
    DebugTarget appDebugTarget = DebugTarget::Console;
    DebugLevel appDebugLevel = DebugLevel::Info;
    uint32_t appDebugTypes = DebugType::All;
    bool appLogSync = false;

    std::mutex appLoggingLock;

    std::string ident{"application"};
    bool forceSyslog = false;

    const char* debugTypeToName(const DebugType & type) {
        switch(type) {
            case DebugType::All: return "all";
            case DebugType::Xcb: return "xcb";
            case DebugType::Rfb: return "rfb";
            case DebugType::Clip: return "clip";
            case DebugType::Sock: return "sock";
            case DebugType::Tls: return "tls";
            case DebugType::Channels: return "chan";
            case DebugType::Dbus: return "dbus";
            case DebugType::Enc: return "enc";
            case DebugType::X11Srv: return "x11srv";
            case DebugType::X11Cli: return "x11cli";
            case DebugType::Audio: return "audio";
            case DebugType::Fuse: return "fuse";
            case DebugType::Pcsc: return "pcsc";
            case DebugType::Pkcs11: return "pkcs11";
            case DebugType::Sdl: return "sdl";
            case DebugType::App: return "app";
            case DebugType::Ldap: return "ldap";
            case DebugType::Gss: return "gss";
            case DebugType::Fork: return "fork";
            case DebugType::Common: return "common";
            case DebugType::Default: return "default";
        }
        return "default";
    }

    uint32_t debugListToTypes(const std::list<std::string> & typesList) {
        uint32_t types = 0;

        for(const auto & val : typesList) {
            auto slower = Tools::lower(val);

            if(slower == "xcb") {
                types |= DebugType::Xcb;
            } else if(slower == "rfb") {
                types |= DebugType::Rfb;
            } else if(slower == "clip") {
                types |= DebugType::Clip;
            } else if(slower == "sock") {
                types |= DebugType::Sock;
            } else if(slower == "tls") {
                types |= DebugType::Tls;
            } else if(slower == "chnl" || startsWith(slower, "chan")) {
                types |= DebugType::Channels;
            } else if(slower == "dbus") {
                types |= DebugType::Dbus;
            } else if(slower == "enc") {
                types |= DebugType::Enc;
            } else if(slower == "x11srv") {
                types |= DebugType::X11Srv;
            } else if(slower == "x11cli") {
                types |= DebugType::X11Cli;
            } else if(slower == "wincli") {
                types |= DebugType::WinCli;
            } else if(slower == "audio") {
                types |= DebugType::Audio;
            } else if(slower == "fuse") {
                types |= DebugType::Fuse;
            } else if(slower == "pcsc") {
                types |= DebugType::Pcsc;
            } else if(slower == "pkcs11") {
                types |= DebugType::Pkcs11;
            } else if(slower == "sdl") {
                types |= DebugType::Sdl;
            } else if(slower == "app") {
                types |= DebugType::App;
            } else if(slower == "ldap") {
                types |= DebugType::Ldap;
            } else if(slower == "gss") {
                types |= DebugType::Gss;
            } else if(slower == "fork") {
                types |= DebugType::Fork;
            } else if(slower == "common") {
                types |= DebugType::Common;
            } else if(slower == "default") {
                types |= DebugType::Default;
            } else if(slower == "all") {
                types |= DebugType::All;
            } else {
                Application::warning("%s: unknown debug marker: `%s'", __FUNCTION__, slower.c_str());
            }
        }

        return types;
    }

    Logger Application::logger(const DebugType & type) {

        auto name = debugTypeToName(type);

        if(auto log = spdlog::get(name)) {
            return log;
        }

        spdlog::set_automatic_registration(true);
        Logger log;

        switch(appDebugTarget) {
            case DebugTarget::Console:
                log = spdlog::stderr_logger_mt(name);
                break;
            case DebugTarget::Syslog:
#ifdef __UNIX__
#ifdef LTSM_WITH_SYSTEMD
                log = spdlog::systemd_logger_mt(name, ident);
#else
                log = spdlog::syslog_logger_mt(name, ident);
#endif
#endif
                break;
            case DebugTarget::SyslogFile:
                log = spdlog::basic_logger_st(name, appLoggingFile);
                break;
        }

        log->set_level(static_cast<spdlog::level::level_enum>(appDebugLevel));
        return log;
    }

    int syslogFacility(std::string_view name) {
        int facility = LOG_USER;

        if(startsWith(name, "local")) {
            switch(name[5]) {
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

        return facility;
    }

    bool Application::isDebugTarget(const DebugTarget & tgt) {
        return appDebugTarget == tgt;
    }

    bool Application::isDebugTypes(uint32_t vals) {
        return appDebugTypes & vals;
    }

    void Application::setDebugTypes(const std::list<std::string> & list) {
        appDebugTypes = debugListToTypes(list);
    }

    void Application::setDebugTarget(const DebugTarget & tgt, std::string_view ext) {

        switch(tgt) {
            case DebugTarget::Console:
                if(appDebugTarget == DebugTarget::SyslogFile) {
                    appLoggingFd.reset(fopen(appLoggingFile.c_str(), "a"));
                    appLoggingFile.clear();
                }
#ifdef __UNIX__
                if(appDebugTarget == DebugTarget::Syslog) {
                    closelog();
                }
#endif
                appDebugTarget = tgt;
                break;

            case DebugTarget::Syslog:
                if(appDebugTarget == DebugTarget::SyslogFile) {
                    appLoggingFd.reset(fopen(appLoggingFile.c_str(), "a"));
                    appLoggingFile.clear();
                }
#ifdef __UNIX__
                if(appDebugTarget == DebugTarget::Syslog) {
                    closelog();
                }

                openlog(ident.c_str(), 0, syslogFacility(ext));
#endif
                appDebugTarget = tgt;
                break;

            case DebugTarget::SyslogFile:
#ifdef __UNIX__
                if(appDebugTarget == DebugTarget::Syslog) {
                    closelog();
                }
#endif
                if(!ext.empty()) {
                    appLoggingFile = view2string(ext);
                    appLoggingFd.reset(fopen(appLoggingFile.c_str(), "a"));
                }
                appDebugTarget = tgt;
                break;
        }
    }

    void Application::setDebugTarget(std::string_view tgt, std::string_view ext) {
        if(tgt == "console") {
            setDebugTarget(DebugTarget::Console);
        }

#ifdef __UNIX__
        else if(tgt == "syslog") {
            setDebugTarget(DebugTarget::Syslog);
        }

#endif
        else if(tgt == "file") {
            setDebugTarget(DebugTarget::SyslogFile, ext);
        } else {
            setDebugTarget(DebugTarget::Console);
        }
    }

    bool Application::isDebugLevel(const DebugLevel & lvl) {
        if(appDebugLevel == DebugLevel::Trace) {
            return true;
        }

        if(appDebugLevel == DebugLevel::Debug && lvl == DebugLevel::Info) {
            return true;
        }

        return appDebugLevel == lvl;
    }

    void Application::setDebugLevel(const DebugLevel & lvl) {
        appDebugLevel = lvl;
    }

    void Application::setDebugLevel(std::string_view lvl) {
        if(lvl == "info") {
            setDebugLevel(DebugLevel::Info);
        } else if(lvl == "debug") {
            setDebugLevel(DebugLevel::Debug);
        } else if(lvl == "trace") {
            setDebugLevel(DebugLevel::Trace);
        } else if(startsWith(lvl, "warn")) {
            setDebugLevel(DebugLevel::Warn);
        } else if(startsWith(lvl, "err")) {
            setDebugLevel(DebugLevel::Error);
        } else if(startsWith(lvl, "crit")) {
            setDebugLevel(DebugLevel::Crit);
        } else {
            setDebugLevel(DebugLevel::Quiet);
        }
    }
 
    Application::Application(std::string_view sid) {
        std::setlocale(LC_ALL, "ru_RU.utf8");
        std::setlocale(LC_NUMERIC, "C");

        ident.assign(sid.begin(), sid.end());
#ifdef LTSM_WITH_SYSTEMD
        // check systemd or docker
        auto res = Tools::runcmd("systemctl is-system-running");

        // systemd not running switch to syslog
        if(res.empty() || res == "offline") {
            forceSyslog = true;
        }

#endif
    }

    Application::~Application() {
#ifdef __UNIX__

        if(isDebugTarget(DebugTarget::Syslog)) {
            closelog();
        }

#endif
    }

    void toPlatformSyslog(int priority, const char* format, va_list args) {
#ifdef __UNIX__
#ifdef LTSM_WITH_SYSTEMD

        if(! forceSyslog) {
            sd_journal_printv(priority, format, args);
            return;
        }

#endif
        vsyslog(priority, format, args);
#else
        vfprintf(appLoggingFd ? appLoggingFd.get() : stderr, format, args);
#endif
    }

    void Application::info(const char* format, ...) {
        if(appDebugLevel != DebugLevel::Quiet) {
            va_list args;
            va_start(args, format);

            if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile) {
                const std::scoped_lock guard{ appLoggingLock };

                FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
                fprintf(fd, "[info] ");
                vfprintf(fd, format, args);
                fprintf(fd, "\n");

                if(appLogSync) {
                    fflush(fd);
                }
            } else if(appDebugTarget == DebugTarget::Syslog) {
                toPlatformSyslog(LOG_INFO, format, args);
            }

            va_end(args);
        }
    }

    void Application::notice(const char* format, ...) {
        va_list args;
        va_start(args, format);

        if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile) {
            const std::scoped_lock guard{ appLoggingLock };

            FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
            fprintf(fd, "[notice] ");
            vfprintf(fd, format, args);
            fprintf(fd, "\n");

            if(appLogSync) {
                fflush(fd);
            }
        } else if(appDebugTarget == DebugTarget::Syslog) {
            toPlatformSyslog(LOG_NOTICE, format, args);
        }

        va_end(args);
    }

    void Application::warning(const char* format, ...) {
        if(appDebugLevel != DebugLevel::Quiet) {
            va_list args;
            va_start(args, format);

            if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile) {
                const std::scoped_lock guard{ appLoggingLock };

                FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
                fprintf(fd, "[warning] ");
                vfprintf(fd, format, args);
                fprintf(fd, "\n");

                if(appLogSync) {
                    fflush(fd);
                }
            } else if(appDebugTarget == DebugTarget::Syslog) {
                toPlatformSyslog(LOG_WARNING, format, args);
            }

            va_end(args);
        }
    }

    void Application::error(const char* format, ...) {
        va_list args;
        va_start(args, format);

        if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile) {
            const std::scoped_lock guard{ appLoggingLock };

            FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
            fprintf(fd, "[error] ");
            vfprintf(fd, format, args);
            fprintf(fd, "\n");

            if(appLogSync) {
                fflush(fd);
            }
        } else if(appDebugTarget == DebugTarget::Syslog) {
            toPlatformSyslog(LOG_ERR, format, args);
        }

        va_end(args);
    }

    void Application::vdebug(const DebugType & subsys, const char* format, va_list args) {
        if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile) {
            const std::scoped_lock guard{ appLoggingLock };

            FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
            fprintf(fd, "[debug] ");
            vfprintf(fd, format, args);
            fprintf(fd, "\n");

            if(appLogSync) {
                fflush(fd);
            }
        } else if(appDebugTarget == DebugTarget::Syslog) {
            toPlatformSyslog(LOG_DEBUG, format, args);
        }
    }

    void Application::debug(const DebugType & subsys, const char* format, ...) {
        if((subsys & appDebugTypes) &&
           (appDebugLevel == DebugLevel::Debug || appDebugLevel == DebugLevel::Trace)) {
            va_list args;
            va_start(args, format);

            vdebug(subsys, format, args);

            va_end(args);
        }
    }

    void Application::vtrace(const DebugType & subsys, const char* format, va_list args) {
        if(appDebugTarget == DebugTarget::Console || appDebugTarget == DebugTarget::SyslogFile) {
            const std::scoped_lock guard{ appLoggingLock };

            FILE* fd = appLoggingFd ? appLoggingFd.get() : stderr;
            fprintf(fd, "[trace] ");
            vfprintf(fd, format, args);
            fprintf(fd, "\n");

            if(appLogSync) {
                fflush(fd);
            }
        } else if(appDebugTarget == DebugTarget::Syslog) {
            toPlatformSyslog(LOG_DEBUG, format, args);
        }
    }

    void Application::trace(const DebugType & subsys, const char* format, ...) {
        if((subsys & appDebugTypes) &&
           (appDebugLevel == DebugLevel::Trace)) {
            va_list args;
            va_start(args, format);

            vtrace(subsys, format, args);

            va_end(args);
        }
    }

#ifdef LTSM_WITH_JSON
    // ApplicationLog
    ApplicationLog::ApplicationLog(std::string_view sid)
        : Application(sid) {
        const char* applog = getenv("LTSM_APPLOG");

        if(! applog) {
            applog = "/etc/ltsm/applog.json";
        }

        if(auto jc = JsonContentFile(applog); jc.isObject()) {
            if(auto jo = jc.toObject(); jo.isObject(ident)) {
                setAppLog(jo.getObject(ident));
            }
        }
    }

    void ApplicationLog::setAppLog(const JsonObject* jo) {
        auto target = jo->getString("debug:target", "console");

        if(target == "syslog") {
            auto facility = jo->getString("debug:syslog", "user");
            setDebugTarget(DebugTarget::Syslog, facility);
        } else if(target == "file") {
            auto file = jo->getString("debug:file");
            setDebugTarget(DebugTarget::SyslogFile, file);
        } else {
            setDebugTarget(DebugTarget::Console);
        }

        setDebugLevel(jo->getString("debug:level", "info"));

        if(auto types = jo->getArray("debug:types")) {
            setDebugTypes(types->toStdList<std::string>());
        }
    }

    // WatchModification
    WatchModification::~WatchModification() {
        inotifyWatchStop();
    }

#ifdef __UNIX__
    void WatchModification::inotifyWatchCb(void) const {
        std::array<char, 256> buf = {};
        auto logger = Application::logger(DebugType::App);

        while(true) {
            auto len = read(_inotifyFd, buf.data(), buf.size());

            if(len < 0) {
                if(errno == EAGAIN) {
                    continue;
                }

                if(errno != EBADF) {
                    logger->error("{}: {} failed, error: {}, code: {}, path: {}",
                                       NS_FuncNameV, "inotify read", strerror(errno), errno, _fileName);
                }

                break;
            }

            if(len < sizeof(struct inotify_event)) {
                logger->error("{}: {} failed, error: {}, code: {}, path: {}",
                                   NS_FuncNameV, "inotify read", strerror(errno), errno, _fileName);
                break;
            }

            auto beg = buf.begin();
            auto end = buf.begin() + len;

            while(beg < end) {
                auto st = (const struct inotify_event*) std::addressof(*beg);

                if(st->mask == IN_CLOSE_WRITE) {
                    if(st->len && _fileName == st->name &&closeWriteCb) {
                        closeWriteCb(_fileName);
                    }
                }

                beg += sizeof(struct inotify_event) + st->len;
            }
        }
    }

    bool WatchModification::inotifyWatchStart(const std::filesystem::path & file) {
        auto logger = Application::logger(DebugType::App);

        if(! std::filesystem::is_regular_file(file)) {
            logger->error("{}: path not found: {}", NS_FuncNameV, file.c_str());
            return false;
        }

        _inotifyFd = inotify_init();

        if(0 > _inotifyFd) {
            logger->error("{}: {} failed, error: {}, code: {}",
                        NS_FuncNameV, "inotify_init", strerror(errno), errno);
            return false;
        }

        _fileName = file.filename();
        _inotifyWd = inotify_add_watch(_inotifyFd, file.parent_path().c_str(), IN_CLOSE_WRITE);

        if(0 > _inotifyWd) {
            logger->error("{}: {} failed, error: {}, code: {}, path: {}",
                        NS_FuncNameV, "inotify_add_watch", strerror(errno), errno, file.c_str());

            inotifyWatchStop();
            return false;
        }

        _inotifyJob = std::thread(& WatchModification::inotifyWatchCb, this);

        logger->debug("{}: path: {}", NS_FuncNameV, file.c_str());
        return true;
    }

    void WatchModification::inotifyWatchStop(void) {
        if(0 <= _inotifyWd) {
            inotify_rm_watch(_inotifyFd, _inotifyWd);
        }

        if(0 <= _inotifyFd) {
            close(_inotifyFd);
        }

        _inotifyFd = -1;
        _inotifyWd = -1;

        if(_inotifyJob.joinable()) {
            _inotifyJob.join();
        }
    }
#else
    void WatchModification::inotifyWatchCb(void) const {
    }

    bool WatchModification::inotifyWatchStart(const std::filesystem::path & file) {
        return false;
    }

    void WatchModification::inotifyWatchStop(void) {
    }
#endif

    // ApplicationJsonConfig
    ApplicationJsonConfig::ApplicationJsonConfig(std::string_view ident)
        : ApplicationLog(ident) {
        readDefaultConfig();
    }

    ApplicationJsonConfig::ApplicationJsonConfig(std::string_view ident, const std::filesystem::path & fconf)
        : ApplicationLog(ident) {
        if(fconf.empty()) {
            readDefaultConfig();
        } else {
            readConfig(fconf);
        }
    }

    void ApplicationJsonConfig::readDefaultConfig(void) {
        std::list<std::filesystem::path> files;

        if(auto env = std::getenv("LTSM_CONFIG")) {
            files.emplace_back(env);
        }

        auto ident_json = Tools::joinToString(ident, ".json");
        files.emplace_back(std::filesystem::current_path() / ident_json);

        auto ident_conf = std::filesystem::path("/etc/ltsm") / ident_json;
        files.emplace_back(std::move(ident_conf));

        files.emplace_back(std::filesystem::current_path() / "config.json");
        files.emplace_back("/etc/ltsm/config.json");

        for(const auto & path : files) {
            auto st = std::filesystem::status(path);

            if(std::filesystem::file_type::not_found == st.type()) {
                continue;
            }

            if(readConfig(path)) {
                break;
            }
        }
    }

    bool ApplicationJsonConfig::readConfig(const std::filesystem::path & file) {
        auto logger = Application::logger(DebugType::App);
        std::error_code err;

        if(! std::filesystem::exists(file, err)) {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), file.c_str(), getuid());
            return false;
        }

        if((std::filesystem::status(file, err).permissions() &
            std::filesystem::perms::owner_read) == std::filesystem::perms::none) {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "permission failed"), file.c_str(), getuid());
            return false;
        }


        Application::info("%s: path: `%s', uid: %d", __FUNCTION__, file.c_str(), getuid());
        JsonContentFile jsonFile(file);

        if(! jsonFile.isValid() || ! jsonFile.isObject()) {
            Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "json object", file.c_str());
            return false;
        }

        json.swap(jsonFile.toObject());
        json.addString("config:path", file.native());

        return true;
    }

    bool ApplicationJsonConfig::inotifyWatchStart(void) {
        if(! watcher) {
            watcher = std::make_unique<WatchModification>(std::bind(& ApplicationJsonConfig::closeWriteEvent, this, std::placeholders::_1));
        }
        return watcher->inotifyWatchStart(configGetString("config:path"));
    }

    void ApplicationJsonConfig::inotifyWatchStop(void) {
        if(watcher) {
            watcher->inotifyWatchStart(configGetString("config:path"));
        }
    }

    void ApplicationJsonConfig::configSetInteger(const std::string & key, int val) {
        json.addInteger(key, val);
    }

    void ApplicationJsonConfig::configSetBoolean(const std::string & key, bool val) {
        json.addBoolean(key, val);
    }

    void ApplicationJsonConfig::configSetString(const std::string & key, std::string_view val) {
        json.addString(key, val);
    }

    void ApplicationJsonConfig::configSetDouble(const std::string & key, double val) {
        json.addDouble(key, val);
    }

    const JsonObject & ApplicationJsonConfig::config(void) const {
        return json;
    }

    void ApplicationJsonConfig::closeWriteEvent(const std::string & file) {
        if(readConfig(file)) {
            configReloadedEvent();
        }
    }
#endif

#ifdef LTSM_WITH_AUDIT
    AuditLog::AuditLog() {
        fd = audit_open();

        if(fd < 0) {
            Application::error("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "audit_open", strerror(errno), errno);
            throw std::runtime_error(NS_FuncName);
        }
    }

    AuditLog::~AuditLog() {
        if(0 <= fd) {
            audit_close(fd);
        }
    }

    void AuditLog::auditUserMessage(int type, const char* msg, const char* hostname, const char* addr, const char* tty, bool success) const {
        assert(msg);

        if(int seq = audit_log_user_message(fd, type, msg, hostname, addr, tty, static_cast<int>(success)); seq < 0) {
            Application::error("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "audit_log_user_message", strerror(errno), errno);
        }
    }
#endif

#ifdef __UNIX__
    void fileLogClose(void) {
        appLoggingFd.reset();
        appDebugTarget = DebugTarget::Console;
        appDebugLevel = DebugLevel::Quiet;
    }

    void redirectStdoutStderrTo(bool out, bool err, const std::filesystem::path & file) {
        auto dir = file.parent_path();
        std::error_code fserr;

        if(! std::filesystem::is_directory(dir, fserr)) {
            std::filesystem::create_directories(dir, fserr);
        }

        int fd = open(file.c_str(), O_RDWR | O_CREAT, 0640);

        if(0 <= fd) {
            if(out) {
                dup2(fd, STDOUT_FILENO);
            }

            if(err) {
                dup2(fd, STDERR_FILENO);
            }

            close(fd);
        } else {
            const char* devnull = "/dev/null";
            Application::warning("%s: %s, path: `%s', uid: %" PRId32, __FUNCTION__, "open failed", file.c_str(), getuid());

            if(file != devnull) {
                redirectStdoutStderrTo(out, err, devnull);
            }
        }
    }

    int ForkMode::forkStart(int redirectFd) {
        const bool debug = Application::isDebugTypes(DebugType::Fork);
        pid_t pid = fork();

        if(pid < 0) {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fork", strerror(errno), errno);
            throw std::runtime_error(NS_FuncName);
        }

        // parent mode
        if(0 < pid) {
            Application::debug(DebugType::App, "%s: child pid: %d", __FUNCTION__, pid);
            return pid;
        }

        // child mode
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        // skip closelog, glibc dead lock
        if(Application::isDebugTarget(DebugTarget::Syslog)) {
            Application::setDebugTarget(DebugTarget::Console);
            Application::setDebugLevel(DebugLevel::Quiet);
        }

        appLoggingFd.reset();

        // close parend fds: skip STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO
        for(int fd = 3; fd < 1024; ++fd) {
            if(fd == redirectFd) {
                continue;
            }

            close(fd);
        }


        if(debug) {
            auto file = Tools::joinToString("/var/tmp/.fork_", ident, "_", getpid(), ".log");
            Application::setDebugTarget(DebugTarget::SyslogFile, file);
            appDebugTypes = DebugType::All;
            appLogSync = true;
            Application::debug(DebugType::App, "%s: log redirected", __FUNCTION__);
        }

        return pid;
    }

    int ForkMode::waitPid(int pid) {
        Application::debug(DebugType::App, "%s: pid: %" PRId32, __FUNCTION__, pid);
        // waitpid
        int status;
        int ret = waitpid(pid, &status, 0);

        if(0 > ret) {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                               __FUNCTION__, "waitpid", strerror(errno), errno);
            return ret;
        }

        if(WIFSIGNALED(status)) {
            Application::warning("%s: process %s, pid: %" PRId32 ", signal: %" PRId32,
                                 __FUNCTION__, "killed", pid, WTERMSIG(status));
        } else if(WIFEXITED(status)) {
            Application::info("%s: process %s, pid: %" PRId32 ", return: %" PRId32,
                              __FUNCTION__, "exited", pid, WEXITSTATUS(status));
        } else {
            Application::debug(DebugType::App, "%s: process %s, pid: %" PRId32 ", wstatus: 0x%08" PRIx32,
                               __FUNCTION__, "ended", pid, status);
        }

        return status;
    }

    int ForkMode::runChildFailure(int res) {
        fileLogClose();
        execl("/bin/true", "/bin/true", nullptr);
        std::exit(res);
        return res;
    }

    void ForkMode::runChildSuccess(void) {
        runChildFailure(0);
    }

    void ForkMode::runChildProcess(const std::filesystem::path & cmd, const std::vector<std::string> & args,
                                   const std::vector<std::string> & envs, const RedirectLog & rmode, int redirectFd) {
        if(Application::isDebugLevel(DebugLevel::Debug)) {
            auto sargs = Tools::join(args.begin(), args.end(), " ");
            auto senvs = Tools::join(envs.begin(), envs.end(), ",");

            Application::info("%s: pid: %" PRId32 ", cmd: `%s', args: `%s', envs: [ %s ]",
                              __FUNCTION__, getpid(), cmd.c_str(), sargs.c_str(), senvs.c_str());
        } else {
            Application::info("%s: pid: %" PRId32 ", cmd: `%s'", __FUNCTION__, getpid(), cmd.c_str());
        }

        for(const auto & env : envs) {
            putenv(const_cast<char*>(env.c_str()));
        }

        // create argv[]
        std::vector<const char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(cmd.c_str());

        for(const auto & arg : args) {
            argv.push_back(arg.c_str());
        }

        argv.push_back(nullptr);

        auto procLogDir = std::filesystem::path{"/tmp"} / ".ltsm" / "log";

        if(auto home = getenv("HOME")) {
            procLogDir = std::filesystem::path{home} / ".ltsm" / "log";
        }

        if(! std::filesystem::is_directory(procLogDir)) {
            std::error_code fserr;
            std::filesystem::create_directories(procLogDir, fserr);
        }

        auto logFile = procLogDir / cmd.filename();
        logFile.replace_extension(".log");

        if(rmode == RedirectLog::StdoutFd) {
            // redirect stderr
            redirectStdoutStderrTo(false, true, logFile.native());

            if(0 <= redirectFd) {
                // redirect stdout
                if(0 > dup2(redirectFd, STDOUT_FILENO)) {
                    Application::warning("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "dup2", strerror(errno), errno);
                }

                close(redirectFd);
                redirectFd = -1;
            }
        } else if(rmode == RedirectLog::StdoutStderr) {
            // redirect stdout, stderr
            redirectStdoutStderrTo(true, true, logFile.native());
        }

        fileLogClose();

        if(int res = execv(cmd.c_str(), (char* const*) argv.data()); res < 0) {
            Application::error("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                               __FUNCTION__, "execv", strerror(errno), errno, cmd.c_str());
        }

        runChildSuccess();
    }
#endif // UNIX
}
