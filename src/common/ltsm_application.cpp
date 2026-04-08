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
#include <clocale>
#include <cstring>
#include <iostream>
#include <filesystem>

#ifdef __UNIX__
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <cstdlib>
#endif

#include <cstdio>

#include "ltsm_tools.h"
#include "ltsm_compat.h"
#include "ltsm_application.h"

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_sinks.h"

#ifdef __UNIX__
#include "systemd/sd-daemon.h"
#include "spdlog/sinks/syslog_sink.h"
#ifdef LTSM_WITH_SYSTEMD
#include "spdlog/sinks/systemd_sink.h"
#endif
#endif

using namespace std::chrono_literals;

namespace LTSM {
    // local application
    std::string appLoggingFile;
    DebugTarget appDebugTarget = DebugTarget::Console;
    DebugLevel appDebugLevel = DebugLevel::Info;
    uint32_t appDebugTypes = DebugType::All;

    std::string appIdent{"application"};

    bool appLogSync = false;

    const char* debugTypeToName(const DebugType & type) {
        switch(type) {
            case DebugType::All:
                return "all";

            case DebugType::Xcb:
                return "xcb";

            case DebugType::Rfb:
                return "rfb";

            case DebugType::Clip:
                return "clip";

            case DebugType::Sock:
                return "sock";

            case DebugType::Tls:
                return "tls";

            case DebugType::Channels:
                return "chan";

            case DebugType::Dbus:
                return "dbus";

            case DebugType::Enc:
                return "enc";

            case DebugType::X11Srv:
                return "x11srv";

            case DebugType::X11Cli:
                return "x11cli";

            case DebugType::Audio:
                return "audio";

            case DebugType::Fuse:
                return "fuse";

            case DebugType::Pcsc:
                return "pcsc";

            case DebugType::Pkcs11:
                return "pkcs11";

            case DebugType::Sdl:
                return "sdl";

            case DebugType::App:
                return "app";

            case DebugType::Ldap:
                return "ldap";

            case DebugType::Gss:
                return "gss";

            case DebugType::Fork:
                return "fork";

            case DebugType::Common:
                return "common";

            case DebugType::Pam:
                return "pam";

            case DebugType::Default:
                return "default";
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
            } else if(slower == "pam") {
                types |= DebugType::Pam;
            } else if(slower == "default") {
                types |= DebugType::Default;
            } else if(slower == "all") {
                types |= DebugType::All;
            } else {
                Application::warning("{}: unknown debug marker: `{}'", NS_FuncNameV, slower);
            }
        }

        return types;
    }

    Logger Application::logger(const DebugType & type) {

        auto name = debugTypeToName(type);

        if(auto log = spdlog::get(name)) {
            return log;
        }

        Logger log;

        switch(appDebugTarget) {
            case DebugTarget::Console:
                log = spdlog::stderr_logger_mt(name);
                break;

            case DebugTarget::Syslog:
#ifdef __UNIX__
#ifdef LTSM_WITH_SYSTEMD
                if(sd_booted()) {
 #if 11000 <= SPDLOG_VERSION
                    log = spdlog::systemd_logger_mt(name, appIdent);
 #else
                    log = spdlog::systemd_logger_mt(name);
 #endif
                }
#endif
                if(! log) {
                    log = spdlog::syslog_logger_mt(name, appIdent);
                }
#endif
                break;

            case DebugTarget::SyslogFile:
                log = spdlog::basic_logger_st(name, appLoggingFile);
                if(appLogSync) {
                    log->flush_on(spdlog::level::info);
                    log->flush_on(spdlog::level::debug);
                }
                break;
        }

        switch(appDebugLevel) {
            case DebugLevel::Trace: log->set_level(spdlog::level::trace); break;
            case DebugLevel::Debug: log->set_level(spdlog::level::debug); break;
            case DebugLevel::Info: log->set_level(spdlog::level::info); break;
            case DebugLevel::Warn: log->set_level(spdlog::level::warn); break;
            case DebugLevel::Error: log->set_level(spdlog::level::err); break;
            case DebugLevel::Crit: log->set_level(spdlog::level::critical); break;
            case DebugLevel::Quiet: log->set_level(spdlog::level::off); break;
        }
    
#if 11503 <= SPDLOG_VERSION
        spdlog::register_or_replace(log);
#else
        spdlog::drop(name);
        spdlog::register_logger(log);
#endif
        return log;
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
                appDebugTarget = tgt;
                break;

            case DebugTarget::Syslog:
                appDebugTarget = tgt;
                if(ext.size()) {
                    appIdent.assign(ext.begin(), ext.end());
                }
                break;

            case DebugTarget::SyslogFile:
                appLoggingFile = view2string(ext);
                appDebugTarget = tgt;
                break;
        }

        spdlog::drop("default");
        auto log = logger(DebugType::Default);
        spdlog::set_default_logger(log);
    }

    void Application::setDebugTarget(std::string_view tgt, std::string_view ext) {
        if(tgt == "console") {
            setDebugTarget(DebugTarget::Console);
        }

#ifdef __UNIX__
        else if(tgt == "syslog") {
            setDebugTarget(DebugTarget::Syslog, ext);
        }

#endif
        else if(tgt == "file") {
            setDebugTarget(DebugTarget::SyslogFile, ext);
        } else {
            setDebugTarget(DebugTarget::Console);
        }
    }

    bool Application::isDebugLevel(const DebugLevel & lvl) {
        if(appDebugLevel == lvl) {
            return true;
        }

        if(appDebugLevel == DebugLevel::Trace) {
            return true;
        }

        if(appDebugLevel == DebugLevel::Debug && lvl == DebugLevel::Info) {
            return true;
        }

        return false;
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

        appIdent.assign(sid.begin(), sid.end());

        spdlog::set_automatic_registration(false);
        auto log = logger(DebugType::Default);
        spdlog::set_default_logger(log);
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
            if(auto jo = jc.toObject(); jo.isObject(appIdent)) {
                setAppLog(jo.getObject(appIdent));
            }
        }
    }

    void ApplicationLog::setAppLog(const JsonObject* jo) {
        auto target = jo->getString("debug:target", "console");

        if(target == "syslog") {
            setDebugTarget(DebugTarget::Syslog);
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
#ifdef LTSM_WITH_BOOST
    boost::asio::awaitable<void> WatchModification::inotifyWatchCb(void) {
        auto len = co_await _inotifyStream.async_read_some(boost::asio::buffer(_inotifyBuf), boost::asio::use_awaitable);

        auto beg = _inotifyBuf.begin();
        auto end = beg + len;

        while(beg < end) {
            auto st = (const struct inotify_event*) std::addressof(*beg);

            if(st->mask == IN_CLOSE_WRITE) {
                if(st->len && _filePath.filename() == st->name && closeWriteCb) {
                    boost::asio::post(_inotifyStream.get_executor(), std::bind(closeWriteCb, _filePath.string()));
                }
            }

            beg += sizeof(struct inotify_event) + st->len;
        }

        co_return;
    }
#else
    void WatchModification::inotifyWatchCb(void) const {
        std::array<char, 256> buf = {};

        while(true) {
            auto len = read(_inotifyFd, buf.data(), buf.size());

            if(len < 0) {
                if(errno == EAGAIN) {
                    continue;
                }

                if(errno != EBADF) {
                    Application::error("{}: {} failed, error: {}, code: {}, path: `{}'",
                                       NS_FuncNameV, "inotify read", strerror(errno), errno, _filePath.string());
                }

                break;
            }

            if(len < sizeof(struct inotify_event)) {
                Application::error("{}: {} failed, error: {}, code: {}, path: `{}'",
                                   NS_FuncNameV, "inotify read", strerror(errno), errno, _filePath.string());
                break;
            }

            auto beg = buf.begin();
            auto end = buf.begin() + len;

            while(beg < end) {
                auto st = (const struct inotify_event*) std::addressof(*beg);

                if(st->mask == IN_CLOSE_WRITE) {
                    if(st->len && _filePath.filename() == st->name && closeWriteCb) {
                        closeWriteCb(_filePath.string());
                    }
                }

                beg += sizeof(struct inotify_event) + st->len;
            }
        }
    }
#endif

    bool WatchModification::inotifyWatchStart(const std::filesystem::path & file) {
        if(! std::filesystem::is_regular_file(file)) {
            Application::error("{}: path not found: `{}'", NS_FuncNameV, file);
            return false;
        }

#ifdef LTSM_WITH_BOOST
        _inotifyFd = inotify_init1(IN_NONBLOCK);
#else
        _inotifyFd = inotify_init();
#endif

        if(0 > _inotifyFd) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               NS_FuncNameV, "inotify_init", strerror(errno), errno);
            return false;
        }

        _filePath = file;
        _inotifyWd = inotify_add_watch(_inotifyFd, file.parent_path().c_str(), IN_CLOSE_WRITE);

        if(0 > _inotifyWd) {
            Application::error("{}: {} failed, error: {}, code: {}, path: `{}'",
                               NS_FuncNameV, "inotify_add_watch", strerror(errno), errno, file);

            inotifyWatchStop();
            return false;
        }

#ifdef LTSM_WITH_BOOST
        _inotifyStream.assign(_inotifyFd);

        boost::asio::co_spawn(_inotifyStream.get_executor(),
            [this]() -> boost::asio::awaitable<void> {
                try {
                    for(;;) {
                        co_await this->inotifyWatchCb();
                    }
                } catch (const boost::system::system_error& err) {
                    auto ec = err.code();
                    if(ec != boost::asio::error::operation_aborted) {
                        Application::error("{}: {} failed, code: {}, error: {}",
                            "inotifyWatchStart", "inotfyWatchCb", ec.value(), ec.message());
                    }
                    co_return;
                }
            }, boost::asio::bind_cancellation_slot(_inotifyStop.slot(), boost::asio::detached));
#else
        _inotifyJob = std::thread(& WatchModification::inotifyWatchCb, this);
#endif

        Application::debug(DebugType::App, "{}: path: `{}'", NS_FuncNameV, file);
        return true;
    }

    void WatchModification::inotifyWatchStop(void) {
        if(0 <= _inotifyWd) {
            inotify_rm_watch(_inotifyFd, _inotifyWd);
        }

#ifdef LTSM_WITH_BOOST
        _inotifyStop.emit(boost::asio::cancellation_type::terminal);
#else
        if(0 <= _inotifyFd) {
            close(_inotifyFd);
        }

        if(_inotifyJob.joinable()) {
            _inotifyJob.join();
        }
#endif

        _inotifyFd = -1;
        _inotifyWd = -1;
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

        auto ident_json = std::string(appIdent).append(".json");
        files.emplace_back(std::filesystem::current_path() / ident_json);

        auto ident_conf = std::filesystem::path("/etc/ltsm") / ident_json;
        files.emplace_back(std::move(ident_conf));

        files.emplace_back(std::filesystem::current_path() / "config.json");

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
        std::error_code err;

        if(! std::filesystem::exists(file, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                     NS_FuncNameV, "exists", err.value(), err.message(), file.string());
            return false;
        }

        if((std::filesystem::status(file, err).permissions() &
            std::filesystem::perms::owner_read) == std::filesystem::perms::none) {
            if(err) {
                Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                     NS_FuncNameV, "status", err.value(), err.message(), file.string());
            } else {
                Application::error("{}: {}, path: `{}', uid: {}", NS_FuncNameV, "permission failed", file, getuid());
            }
            return false;
        }


        Application::info("{}: path: `{}', uid: {}", NS_FuncNameV, file, getuid());
        JsonContentFile jsonFile(file);

        if(! jsonFile.isValid() || ! jsonFile.isObject()) {
            Application::error("{}: {} failed, path: `{}'", NS_FuncNameV, "json object", file);
            return false;
        }

        json.swap(jsonFile.toObject());
        json.addString("config:path", file.native());

        return true;
    }

#ifdef LTSM_WITH_BOOST
    bool ApplicationJsonConfig::inotifyWatchStart(boost::asio::io_context& ctx) {
        if(! watcher) {
            watcher = std::make_unique<WatchModification>(ctx, std::bind(& ApplicationJsonConfig::closeWriteEvent, this, std::placeholders::_1));
        }

        return watcher->inotifyWatchStart(configGetString("config:path"));
    }
#else
    bool ApplicationJsonConfig::inotifyWatchStart(void) {
        if(! watcher) {
            watcher = std::make_unique<WatchModification>(std::bind(& ApplicationJsonConfig::closeWriteEvent, this, std::placeholders::_1));
        }

        return watcher->inotifyWatchStart(configGetString("config:path"));
    }
#endif

    void ApplicationJsonConfig::inotifyWatchStop(void) {
        if(watcher) {
            watcher->inotifyWatchStop();
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
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "audit_open", strerror(errno), errno);
            throw std::runtime_error(NS_FuncNameS);
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
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "audit_log_user_message", strerror(errno), errno);
        }
    }
#endif

#ifdef __UNIX__
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
            Application::warning("{}: {}, path: `{}', uid: {}", NS_FuncNameV, "open failed", file, getuid());

            if(file != devnull) {
                redirectStdoutStderrTo(out, err, devnull);
            }
        }
    }

    int ForkMode::forkStart(int redirectFd) {
        const bool debug = Application::isDebugTypes(DebugType::Fork);
        pid_t pid = fork();

        if(pid < 0) {
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "fork", strerror(errno), errno);
            throw std::runtime_error(NS_FuncNameS);
        }

        spdlog::apply_all([&](auto log) { log->flush(); });

        // parent mode
        if(0 < pid) {
            Application::debug(DebugType::App, "{}: child pid: {}", NS_FuncNameV, pid);
            return pid;
        }

        // child mode
        spdlog::shutdown();

        if(Application::isDebugTarget(DebugTarget::Syslog)) {
#ifdef LTSM_WITH_SYSTEMD
            if(sd_booted()) {
                Application::setDebugTarget(DebugTarget::Console);
            }
#endif
        }

        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        // close parend fds: skip STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO
        for(int fd = 3; fd < 1024; ++fd) {
            if(fd == redirectFd) {
                continue;
            }

            close(fd);
        }

        // register log
        auto log = Application::logger(DebugType::Default);
        spdlog::set_default_logger(log);

        if(debug) {
            auto file = fmt::format("/var/tmp/.fork_{}_{}.log", appIdent, getpid());
            appDebugTypes = DebugType::All;
            appLogSync = true;
            Application::setDebugTarget(DebugTarget::SyslogFile, file);
            Application::debug(DebugType::App, "{}: log redirected", NS_FuncNameV);
        }

        return pid;
    }

    int ForkMode::waitPid(int pid) {
        Application::debug(DebugType::App, "{}: pid: {}", NS_FuncNameV, pid);
        // waitpid
        int status;
        int ret = waitpid(pid, &status, 0);

        if(0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               NS_FuncNameV, "waitpid", strerror(errno), errno);
            return ret;
        }

        if(WIFSIGNALED(status)) {
            Application::warning("{}: process {}, pid: {}, signal: {}",
                                 NS_FuncNameV, "killed", pid, WTERMSIG(status));
        } else if(WIFEXITED(status)) {
            Application::info("{}: process {}, pid: {}, return: {}",
                              NS_FuncNameV, "exited", pid, WEXITSTATUS(status));
        } else {
            Application::debug(DebugType::App, "{}: process {}, pid: {}, wstatus: {:#010x}",
                               NS_FuncNameV, "ended", pid, status);
        }

        return status;
    }

    void ForkMode::runChildExit(int res) {
        execl("/bin/true", "/bin/true", nullptr);
        std::exit(res);
    }
#endif // UNIX
}
