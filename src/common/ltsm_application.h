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

#include <string>

#include <stdio.h>
#include <syslog.h>

#include "ltsm_json_wrapper.h"

namespace LTSM
{
    class Application
    {
    protected:
        static int      _debug;
        int		_argc;
        const char**	_argv;
        const char*     _ident;
	int		_facility;

    public:
        Application(const char* ident, int argc, const char** argv);
        virtual ~Application();

        template<typename... Values>
        static void info(const char* format, Values && ... vals)
        {
            if(0 < _debug)
                syslog(LOG_INFO, format, (vals)...);
        }

        template<typename... Values>
        static void error(const char* format, Values && ... vals)
        {
            syslog(LOG_ERR, format, (vals)...);
        }

        template<typename... Values>
        static void debug(const char* format, Values && ... vals)
        {
            if(1 < _debug)
                syslog(LOG_DEBUG, format, (vals)...);
        }

        void openlog(void) const
        {
            ::openlog(_ident, 0, _facility);
        }

        static void busSetDebugLevel(const std::string & level)
        {
            if(level == "info")
                _debug = 1;
            else if(level == "debug")
                _debug = 2;
            else
                _debug = 0;
        }

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
