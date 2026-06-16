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

#ifndef _LTSM_APPLICATION_
#define _LTSM_APPLICATION_

#include <list>
#include <mutex>
#include <vector>
#include <thread>
#include <string>
#include <functional>
#include <filesystem>

#ifdef LTSM_WITH_AUDIT
#include <libaudit.h>
#endif

#ifdef __UNIX__
#ifdef LTSM_WITH_BOOST
#include <boost/asio.hpp>
#endif
#endif

#ifdef LTSM_WITH_JSON
#include "ltsm_json_wrapper.h"
#endif

#include "spdlog/spdlog.h"

namespace LTSM {
    enum class DebugTarget { Console, Syslog, SyslogFile };

    enum class DebugLevel {
        Trace,
        Debug,
        Info,
        Warn,
        Error,
        Crit,
        Quiet
    };

    enum DebugType {
        All = 0xFFFFFFFF,
        Xcb = 1 << 31,
        Rfb = 1 << 30,
        Clip = 1 << 29,
        Sock = 1 << 28,
        Tls = 1 << 27,
        Channels = 1 << 26,
        Dbus = 1 << 25,
        Enc = 1 << 24,
        X11Srv = 1 << 23,
        X11Cli = 1 << 22,
        WinCli = X11Cli,
        Audio = 1 << 21,
        Fuse = 1 << 20,
        Pcsc = 1 << 19,
        Pkcs11 = 1 << 18,
        Sdl = 1 << 17,
        App = 1 << 16,
        Ldap = 1 << 14,
        Gss = 1 << 13,
        Fork = 1 << 12,
        Common = 1 << 11,
        Pam = 1 << 10,
        Default = 1 << 9
    };

    using Logger = std::shared_ptr<spdlog::logger>;

    class Application {
      public:
        explicit Application(std::string_view ident);
        virtual ~Application() = default;

        Application(Application &) = delete;
        Application & operator= (const Application &) = delete;

        static Logger logger(const DebugType & type = DebugType::Default);

        template<typename... Args>
        static void error(std::string_view fmt, Args&& ... args) noexcept {
            try {
                spdlog::error(fmt::runtime(fmt), args...);
            } catch(...) {
            }
        }

        template<typename... Args>
        static void warning(std::string_view fmt, Args&& ... args) noexcept {
            try {
                spdlog::warn(fmt::runtime(fmt), args...);
            } catch(...) {
            }
        }

        template<typename... Args>
        static void info(std::string_view fmt, Args&& ... args) noexcept {
            if(isDebugLevel(DebugLevel::Info)) {
                try {
                    spdlog::info(fmt::runtime(fmt), args...);
                } catch(...) {
                }
            }
        }

        template<typename... Args>
        static void notice(std::string_view fmt, Args&& ... args) {
            spdlog::warn(fmt::runtime(fmt), args...);
        }

        template<typename... Args>
        static void debug(const DebugType & type, std::string_view fmt, Args&& ... args) noexcept {
            if(isDebugLevel(DebugLevel::Debug) && isDebugTypes(type)) {
                try {
                    if(auto log = logger(type)) {
                        log->debug(fmt::runtime(fmt), args...);
                    }
                } catch(...) {
                }
            }
        }

        template<typename... Args>
        static void trace(const DebugType & type, std::string_view fmt, Args&& ... args) noexcept {
            if(isDebugLevel(DebugLevel::Trace) && isDebugTypes(type)) {
                try {
                    if(auto log = logger(type)) {
                        log->debug(fmt::runtime(fmt), args...);
                    }
                } catch(...) {
                }
            }
        }
        
        static void setDebugTarget(const DebugTarget &, std::string_view = "");
        static void setDebugTarget(std::string_view target, std::string_view = "");
        static bool isDebugTarget(const DebugTarget &);

        static void setDebugLevel(const DebugLevel &);
        static void setDebugLevel(std::string_view level);
        static bool isDebugLevel(const DebugLevel &);

        static void setDebugTypes(const std::list<std::string> &);
        static bool isDebugTypes(uint32_t vals);
    };

#ifdef LTSM_WITH_JSON
    class ApplicationLog : public Application {
      protected:
        void setAppLog(const JsonObject*);

      public:
        ApplicationLog(std::string_view ident);
    };

#ifdef __UNIX__
    using WatchModificationCb = std::function<void(const std::string &)>;

    class WatchModification {
#ifdef LTSM_WITH_BOOST
        std::array<char, 1024> _inotifyBuf;
        boost::asio::posix::stream_descriptor _inotifyStream;
        boost::asio::cancellation_signal _inotifyStop;
#else
        std::thread _inotifyJob;
#endif
        std::filesystem::path _filePath;

        int _inotifyFd = -1;
        int _inotifyWd = -1;

        WatchModificationCb closeWriteCb;

      protected:
#ifdef LTSM_WITH_BOOST
        boost::asio::awaitable<void> inotifyWatchCb(void);
#else
        void inotifyWatchCb(void) const;
#endif

        WatchModification(const WatchModification &) = delete;
        WatchModification & operator=(const WatchModification &) = delete;

      public:
#ifdef LTSM_WITH_BOOST
        WatchModification(boost::asio::io_context& ctx, const WatchModificationCb & cb)
            : _inotifyStream{ctx}, closeWriteCb(cb) {};
#else
        WatchModification(const WatchModificationCb & cb) : closeWriteCb(cb) {};
#endif
        ~WatchModification();

        bool inotifyWatchStart(const std::filesystem::path &);
        void inotifyWatchStop(void);
    };

    class ApplicationJsonConfig : public ApplicationLog {
        JsonObject json;
        std::unique_ptr<WatchModification> watcher;

      protected:
        void closeWriteEvent(const std::string &);

#ifdef LTSM_WITH_BOOST
        bool inotifyWatchStart(boost::asio::io_context&);
#endif
        bool inotifyWatchStart(void);
        void inotifyWatchStop(void);
        void readDefaultConfig(void);

      public:
        explicit ApplicationJsonConfig(std::string_view ident);
        ApplicationJsonConfig(std::string_view ident, const std::filesystem::path & file);

        bool readConfig(const std::filesystem::path &);

        void configSetInteger(const std::string &, int);
        void configSetBoolean(const std::string &, bool);
        void configSetString(const std::string &, std::string_view);
        void configSetDouble(const std::string &, double);

        inline int configGetInteger(std::string_view key, int def = 0) const {
            return json.getInteger(key, def);
        }

        inline bool configGetBoolean(std::string_view key, bool def = false) const {
            return json.getBoolean(key, def);
        }

        inline std::string configGetString(std::string_view key, std::string_view def = "") const {
            return json.getString(key, def);
        }

        inline double configGetDouble(std::string_view key, double def = 0) const {
            return json.getDouble(key, def);
        }

        inline bool configHasKey(std::string_view key) const {
            return json.hasKey(key);
        }

        const JsonObject & config(void) const;
        virtual void configReloadedEvent(void) {}
    };

#endif // __UNIX__
#endif // LTSM_WITH_JSON

#ifdef LTSM_WITH_AUDIT
    class AuditLog {
        int fd = -1;

      public:
        AuditLog();
        virtual ~AuditLog();

        AuditLog(const AuditLog &) = delete;
        AuditLog & operator=(const AuditLog &) = delete;

        void auditUserMessage(int type, const char* msg, const char* hostname, const char* addr, const char* tty, bool success) const;
    };
#endif

#ifdef __UNIX__
    enum class RedirectLog { None, StdoutStderr, StdoutFd };

    namespace ForkMode {
        int forkStart(int redirectFd = -1);
        int waitPid(int pid);
        void runChildExit(int res = 0);
    }
#endif
}

#endif // _LTSM_APPLICATION_
