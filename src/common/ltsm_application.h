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
#include <string_view>

#include <systemd/sd-journal.h>

#include <syslog.h>
#include <stdio.h>

#include "ltsm_json_wrapper.h"

namespace LTSM
{
    enum class DebugTarget { Quiet, Console, Syslog, SystemD };
    enum class DebugLevel { None, Info, Debug, Trace };

    class Application
    {
    protected:
        static std::mutex logging;
        static FILE* fdlog;
        static DebugTarget target;
        static DebugLevel level;

    public:
        explicit Application(std::string_view ident);
        virtual ~Application();

        Application(Application &) = delete;
        Application & operator= (const Application &) = delete;

        template<typename... Values>
        static void info(const char* format, Values && ... vals)
        {
	    if(level != DebugLevel::None)
	    {
                if(target == DebugTarget::Console)
	        {
                    const std::scoped_lock guard{ logging };

		    fprintf(fdlog, "[info] ");
		    fprintf(fdlog, format, vals...);
		    fprintf(fdlog, "\n");
	        }
	        else
                if(target == DebugTarget::Syslog)
                {
                    syslog(LOG_INFO, format, vals...);
                }
                else
                if(target == DebugTarget::SystemD)
                {
                    sd_journal_print(LOG_INFO, format, vals...);
                }
            }
        }

        template<typename... Values>
        static void notice(const char* format, Values && ... vals)
        {
            if(target == DebugTarget::Console)
	    {
                const std::scoped_lock guard{ logging };

		fprintf(fdlog, "[notice] ");
		fprintf(fdlog, format, vals...);
	    	fprintf(fdlog, "\n");
	    }
	    else
            if(target == DebugTarget::Syslog)
            {
                syslog(LOG_NOTICE, format, vals...);
            }
            else
            if(target == DebugTarget::SystemD)
            {
                sd_journal_print(LOG_NOTICE, format, vals...);
            }
        }

        template<typename... Values>
        static void warning(const char* format, Values && ... vals)
        {
	    if(level != DebugLevel::None)
	    {
                if(target == DebugTarget::Console)
	        {
                    const std::scoped_lock guard{ logging };

		    fprintf(fdlog, "[warning] ");
		    fprintf(fdlog, format, vals...);
		    fprintf(fdlog, "\n");
	        }
	        else
                if(target == DebugTarget::Syslog)
                {
        	    syslog(LOG_WARNING, format, vals...);
                }
                else
                if(target == DebugTarget::SystemD)
                {
                    sd_journal_print(LOG_WARNING, format, vals...);
                }
            }
        }

        template<typename... Values>
        static void error(const char* format, Values && ... vals)
        {
            if(target == DebugTarget::Console)
	    {
                const std::scoped_lock guard{ logging };

		fprintf(fdlog, "[error] ");
		fprintf(fdlog, format, vals...);
		fprintf(fdlog, "\n");
	    }
	    else
            if(target == DebugTarget::Syslog)
            {
        	syslog(LOG_ERR, format, vals...);
            }
            else
            if(target == DebugTarget::SystemD)
            {
                sd_journal_print(LOG_ERR, format, vals...);
            }
        }

        template<typename... Values>
        static void debug(const char* format, Values && ... vals)
        {
	    if(level == DebugLevel::Debug || level == DebugLevel::Trace)
	    {
                if(target == DebugTarget::Console)
	        {
                    const std::scoped_lock guard{ logging };

		    fprintf(fdlog, "[debug] ");
		    fprintf(fdlog, format, vals...);
		    fprintf(fdlog, "\n");
	        }
	        else
                if(target == DebugTarget::Syslog)
                {
                    syslog(LOG_DEBUG, format, vals...);
                }
                else
                if(target == DebugTarget::SystemD)
                {
                    sd_journal_print(LOG_DEBUG, format, vals...);
                }
            }
        }

        template<typename... Values>
        static void trace(const char* format, Values && ... vals)
        {
	    if(level == DebugLevel::Trace)
	    {
                if(target == DebugTarget::Console)
	        {
                    const std::scoped_lock guard{ logging };

		    fprintf(fdlog, "[trace] ");
		    fprintf(fdlog, format, vals...);
		    fprintf(fdlog, "\n");
	        }
	        else
                if(target == DebugTarget::Syslog)
                {
                    syslog(LOG_DEBUG, format, vals...);
                }
                else
                if(target == DebugTarget::SystemD)
                {
                    sd_journal_print(LOG_DEBUG, format, vals...);
                }
            }
        }

        static void openSyslog(void);
        static void closeSyslog(void);

        static void openChildSyslog(const char* file = nullptr);

        static void setDebug(const DebugTarget &, const DebugLevel &);

        static void setDebugTarget(const DebugTarget &);
        static void setDebugTarget(std::string_view target);
        static bool isDebugTarget(const DebugTarget &);

        static void setDebugLevel(const DebugLevel &);
        static void setDebugLevel(std::string_view level);
        static bool isDebugLevel(const DebugLevel &);

        virtual int start(void) = 0;
    };

    class ApplicationJsonConfig : public Application
    {
        JsonObject	   json;

    protected:
        void               configSet(JsonObject &&) noexcept;

    public:
        ApplicationJsonConfig(std::string_view ident, const char* fconf = nullptr);

        void               readConfig(const std::filesystem::path &);

        void               configSetInteger(const std::string &, int);
        void               configSetBoolean(const std::string &, bool);
        void               configSetString(const std::string &, std::string_view);
        void               configSetDouble(const std::string &, double);

        int                configGetInteger(std::string_view, int = 0) const;
        bool               configGetBoolean(std::string_view, bool = false) const;
        std::string        configGetString(std::string_view, std::string_view = "") const;
        double             configGetDouble(std::string_view, double = 0) const;

        const JsonObject & config(void) const;
    };
}

#endif // _LTSM_APPLICATION_
