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

#include <mutex>
#include <string>
#include <cstdarg>
#include <filesystem>
#include <string_view>

#ifdef WITH_JSON
#include "ltsm_json_wrapper.h"
#endif

namespace LTSM
{
    enum class DebugTarget { Quiet, Console, Syslog };
    enum class DebugLevel { None, Info, Debug, Trace };

    enum DebugType
    {
        All = 0xFFFFFFFF,
        Xcb = 1 << 31,
        Rfb = 1 << 30,
        Clip = 1 << 29,
        Socket = 1 << 28,
        Tls = 1 << 27,
        Channels = 1 << 26,
        Conn = 1 << 25,
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
        Mgr = 1 << 15,
        Ldap = 1 << 14,
        Gss = 1 << 13,
        Helper = 1 << 14
    };

    class Application
    {
    public:
        explicit Application(std::string_view ident);
        virtual ~Application();

        Application(Application &) = delete;
        Application & operator= (const Application &) = delete;

        static void info(const char* format, ...);
        static void notice(const char* format, ...);
        static void warning(const char* format, ...);
        static void error(const char* format, ...);
        static void vdebug(uint32_t subsys, const char* format, va_list args);
        static void debug(uint32_t subsys, const char* format, ...);
        static void vtrace(uint32_t subsys, const char* format, va_list args);
        static void trace(uint32_t subsys, const char* format, ...);

        static void redirectSyslogFile(const char* file = nullptr);
        static void setDebug(const DebugTarget &, const DebugLevel &);

        static void setDebugTarget(const DebugTarget &);
        static void setDebugTarget(std::string_view target);
        static void setDebugTargetFile(const std::filesystem::path & file);
        static bool isDebugTarget(const DebugTarget &);

        static void setDebugLevel(const DebugLevel &);
        static void setDebugLevel(std::string_view level);
        static bool isDebugLevel(const DebugLevel &);

        static void setDebugTypes(uint32_t);
        static bool isDebugTypes(uint32_t vals);

        virtual int start(void) = 0;
    };

#ifdef WITH_JSON
    class ApplicationJsonConfig : public Application
    {
        JsonObject json;

    protected:
        void configSet(JsonObject &&) noexcept;

    public:
        ApplicationJsonConfig(std::string_view ident, const char* fconf = nullptr);

        void readConfig(const std::filesystem::path &);

        void configSetInteger(const std::string &, int);
        void configSetBoolean(const std::string &, bool);
        void configSetString(const std::string &, std::string_view);
        void configSetDouble(const std::string &, double);

        int configGetInteger(std::string_view, int = 0) const;
        bool configGetBoolean(std::string_view, bool = false) const;
        std::string configGetString(std::string_view, std::string_view = "") const;
        double configGetDouble(std::string_view, double = 0) const;

        const JsonObject & config(void) const;
    };

#endif
}

#endif // _LTSM_APPLICATION_
