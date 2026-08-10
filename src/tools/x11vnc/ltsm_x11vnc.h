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

#ifndef _LTSM_X11VNC_
#define _LTSM_X11VNC_

#include <boost/asio.hpp>
#include "ltsm_application.h"

#define LTSM_X11VNC_VERSION 20260810

namespace LTSM {
    class X11Vnc : public ApplicationJsonConfig {
        boost::asio::io_context ioc_;

        boost::asio::awaitable<void> startInetd(void);
        boost::asio::awaitable<void> startSocket(uint16_t port);

      public:
        X11Vnc(int argc, const char** argv);

        int start(void);
    };
}

#endif // _LTSM_CONNECTOR_
