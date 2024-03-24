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

        static void info(const char* format, ...)
        {
	    if(level != DebugLevel::None)
	    {
                va_list args;
                va_start(args, format);

                if(target == DebugTarget::Console)
	        {
                    const std::scoped_lock guard{ logging };

		    fprintf(fdlog, "[info] ");
		    vfprintf(fdlog, format, args);
		    fprintf(fdlog, "\n");
	        }
	        else
                if(target == DebugTarget::Syslog)
                {
                    vsyslog(LOG_INFO, format, args);
                }
                else
                if(target == DebugTarget::SystemD)
                {
                    sd_journal_printv(LOG_INFO, format, args);
                }

                va_end(args);
            }
        }

        static void notice(const char* format, ...)
        {
            va_list args;
            va_start(args, format);

            if(target == DebugTarget::Console)
	    {
                const std::scoped_lock guard{ logging };

		fprintf(fdlog, "[notice] ");
		vfprintf(fdlog, format, args);
	    	fprintf(fdlog, "\n");
	    }
	    else
            if(target == DebugTarget::Syslog)
            {
                vsyslog(LOG_NOTICE, format, args);
            }
            else
            if(target == DebugTarget::SystemD)
            {
                sd_journal_printv(LOG_NOTICE, format, args);
            }

            va_end(args);
        }

        static void warning(const char* format, ...)
        {
	    if(level != DebugLevel::None)
	    {
                va_list args;
                va_start(args, format);

                if(target == DebugTarget::Console)
	        {
                    const std::scoped_lock guard{ logging };

		    fprintf(fdlog, "[warning] ");
		    vfprintf(fdlog, format, args);
		    fprintf(fdlog, "\n");
	        }
	        else
                if(target == DebugTarget::Syslog)
                {
        	    vsyslog(LOG_WARNING, format, args);
                }
                else
                if(target == DebugTarget::SystemD)
                {
                    sd_journal_printv(LOG_WARNING, format, args);
                }

                va_end(args);
            }
        }

        static void error(const char* format, ...)
        {
            va_list args;
            va_start(args, format);

            if(target == DebugTarget::Console)
	    {
                const std::scoped_lock guard{ logging };

		fprintf(fdlog, "[error] ");
		vfprintf(fdlog, format, args);
		fprintf(fdlog, "\n");
	    }
	    else
            if(target == DebugTarget::Syslog)
            {
        	vsyslog(LOG_ERR, format, args);
            }
            else
            if(target == DebugTarget::SystemD)
            {
                sd_journal_printv(LOG_ERR, format, args);
            }

            va_end(args);
        }

        static void debug(const char* format, ...)
        {
	    if(level == DebugLevel::Debug || level == DebugLevel::Trace)
	    {
                va_list args;
                va_start(args, format);

                if(target == DebugTarget::Console)
	        {
                    const std::scoped_lock guard{ logging };

		    fprintf(fdlog, "[debug] ");
		    vfprintf(fdlog, format, args);
		    fprintf(fdlog, "\n");
	        }
	        else
                if(target == DebugTarget::Syslog)
                {
                    vsyslog(LOG_DEBUG, format, args);
                }
                else
                if(target == DebugTarget::SystemD)
                {
                    sd_journal_printv(LOG_DEBUG, format, args);
                }

                va_end(args);
            }
        }

        static void trace(const char* format, ...)
        {
	    if(level == DebugLevel::Trace)
	    {
                va_list args;
                va_start(args, format);

                if(target == DebugTarget::Console)
	        {
                    const std::scoped_lock guard{ logging };

		    fprintf(fdlog, "[trace] ");
		    vfprintf(fdlog, format, args);
		    fprintf(fdlog, "\n");
	        }
	        else
                if(target == DebugTarget::Syslog)
                {
                    vsyslog(LOG_DEBUG, format, args);
                }
                else
                if(target == DebugTarget::SystemD)
                {
                    sd_journal_printv(LOG_DEBUG, format, args);
                }

                va_end(args);
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
