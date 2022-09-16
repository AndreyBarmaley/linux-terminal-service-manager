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

#ifndef _LTSM_VNC2IMAGE_
#define _LTSM_VNC2IMAGE_

#include <string>

#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"

#define LTSM_VNC2IMAGE_VERSION 20220829

namespace LTSM
{
    class Vnc2Image : public Application
    {
        std::string             host{"localhost"};
        std::string             password;
        std::string             filename{"screenshot.png"};
        std::string             priority;
        int                     port = 5900;
        int                     timeout = 0;
        bool                    notls = false;

        int    		        startSocket(std::string_view host, int port) const;

    public:
        Vnc2Image(int argc, const char** argv);

        int    		        start(void);
    };
}

#endif // _LTSM_CONNECTOR_
