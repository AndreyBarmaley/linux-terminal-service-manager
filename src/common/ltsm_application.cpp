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

#include <sys/types.h>

#include <libgen.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include <iostream>

#include "ltsm_tools.h"
#include "ltsm_application.h"


namespace LTSM
{
    int Application::_debug = 2;

    ApplicationJsonConfig::ApplicationJsonConfig(const char* ident, int argc, const char** argv)
        : Application(ident, argc, argv)
    {
        std::string confPath;

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--config") && it + 1 < argc)
            {
                confPath.assign(argv[it + 1]);
                it = it + 1;
            }
        }

        if(confPath.empty())
        {
            const char* env = std::getenv("LTSM_CONFIG");

            if(env) confPath.assign(env);
        }

        if(confPath.empty())
        {
            for(auto path :
                { "config.json", "/etc/ltsm/config.json" })
            {
                if(0 == access(path, R_OK))
                {
                    confPath.assign(path);
                    break;
                }
            }

            if(confPath.empty())
            {
                const std::string local = Tools::dirname(argv[0]).append("/").append("config.json");

                if(0 == access(local.c_str(), R_OK))
                    confPath.assign(local);
            }
        }

        if(confPath.empty())
            throw std::string("config.json not found");

        info("used config: %s, running uid: %d", confPath.c_str(), getuid());
        JsonContentFile jsonFile(confPath);

        if(! jsonFile.isValid() || ! jsonFile.isObject())
            throw std::string("json parse error: ").append(confPath);

        _config = jsonFile.toObject();
        std::string str = _config.getString("logging:facility");
	int facility = 0;

        if(6 == str.size() && 0 == str.compare(0, 5, "local"))
        {
            switch(str[5])
            {
                case '0': facility = LOG_LOCAL0; break;
                case '1': facility = LOG_LOCAL1; break;
                case '2': facility = LOG_LOCAL2; break;
                case '3': facility = LOG_LOCAL3; break;
                case '4': facility = LOG_LOCAL4; break;
                case '5': facility = LOG_LOCAL5; break;
                case '6': facility = LOG_LOCAL6; break;
                case '7': facility = LOG_LOCAL7; break;
                default: break;
            }
        }

	if(0 < facility)
        {
	    closelog();
    	    ::openlog(_ident, 0, facility);
	    _facility = facility;
	}
    }
}
