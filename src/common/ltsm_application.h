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

#include <stdio.h>
#include <syslog.h>

#include "ltsm_json_wrapper.h"

namespace LTSM
{
    enum class DebugLevel { Quiet, Console, ConsoleError, SyslogInfo, SyslogDebug, SyslogTrace };

    class Application
    {
        const char*     _ident = nullptr;
	int		_facility = LOG_USER;

    protected:
        static std::mutex _logging;
        static DebugLevel _debug;

        void            reopenSyslog(int facility);

    public:
        Application(const char* ident, int argc, const char** argv);
        virtual ~Application();

        Application(Application &) = delete;
        Application & operator= (const Application &) = delete;

        template<typename... Values>
        static void info(const char* format, Values && ... vals)
        {
	    if(_debug == DebugLevel::Console)
	    {
                const std::scoped_lock<std::mutex> lock(_logging);
		fprintf(stderr, "[info]\t");
		fprintf(stderr, format, vals...);
		fprintf(stderr, "\n");
	    }
	    else
	    if(_debug == DebugLevel::SyslogInfo ||  _debug == DebugLevel::SyslogDebug ||  _debug == DebugLevel::SyslogTrace)
                syslog(LOG_INFO, format, vals...);
        }

        template<typename... Values>
        static void notice(const char* format, Values && ... vals)
        {
	    if(_debug == DebugLevel::Console)
	    {
                const std::scoped_lock<std::mutex> lock(_logging);
		fprintf(stderr, "[notice]\t");
		fprintf(stderr, format, vals...);
		fprintf(stderr, "\n");
	    }
	    else
	    if(_debug == DebugLevel::SyslogInfo ||  _debug == DebugLevel::SyslogDebug ||  _debug == DebugLevel::SyslogTrace)
        	syslog(LOG_NOTICE, format, vals...);
        }

        template<typename... Values>
        static void warning(const char* format, Values && ... vals)
        {
	    if(_debug == DebugLevel::Console ||
	        _debug == DebugLevel::ConsoleError)
	    {
                const std::scoped_lock<std::mutex> lock(_logging);
		fprintf(stderr, "[warning]\t");
		fprintf(stderr, format, vals...);
		fprintf(stderr, "\n");
	    }
	    else
        	syslog(LOG_WARNING, format, vals...);
        }

        template<typename... Values>
        static void error(const char* format, Values && ... vals)
        {
	    if(_debug == DebugLevel::Console ||
	        _debug == DebugLevel::ConsoleError)
	    {
                const std::scoped_lock<std::mutex> lock(_logging);
		fprintf(stderr, "[error]\t");
		fprintf(stderr, format, vals...);
		fprintf(stderr, "\n");
	    }
	    else
        	syslog(LOG_ERR, format, vals...);
        }

        template<typename... Values>
        static void debug(const char* format, Values && ... vals)
        {
	    if(_debug == DebugLevel::Console)
	    {
                const std::scoped_lock<std::mutex> lock(_logging);
		fprintf(stderr, "[debug]\t");
		fprintf(stderr, format, vals...);
		fprintf(stderr, "\n");
	    }
	    else
	    if(_debug == DebugLevel::SyslogDebug ||  _debug == DebugLevel::SyslogTrace)
                syslog(LOG_DEBUG, format, vals...);
        }

        void openlog(void) const
        {
            ::openlog(_ident, 0, _facility);
        }

        static bool isDebugLevel(const DebugLevel &);
        static void setDebugLevel(const DebugLevel &);
        static void setDebugLevel(std::string_view level);

        virtual int start(void) = 0;
    };

    class ApplicationJsonConfig : public Application
    {
    protected:
        JsonObject	_config;

    public:
        ApplicationJsonConfig(const char* ident, int argc, const char** argv);

        const JsonObject & config(void) const
        {
            return _config;
        }
    };
}

#endif // _LTSM_APPLICATION_
