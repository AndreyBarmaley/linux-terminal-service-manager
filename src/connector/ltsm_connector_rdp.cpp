/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

#include <string>
#include <thread>
#include <iostream>

#include "ltsm_tools.h"
#include "ltsm_connector_rdp.h"

using namespace std::chrono_literals;

namespace LTSM
{
    /* Connector::RDP */
    int Connector::RDP::communication(void)
    {
	if(0 >= busGetServiceVersion())
        {
            Application::error("%s", "bus service failure");
            return EXIT_FAILURE;
        }

        const std::string remoteaddr = Tools::getenv("REMOTE_ADDR", "local");
        Application::debug("under construction, remoteaddr: %s\n", remoteaddr.c_str());
        return EXIT_SUCCESS;
    }
}
