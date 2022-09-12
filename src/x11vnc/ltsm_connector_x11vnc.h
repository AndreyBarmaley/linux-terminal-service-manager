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

#ifndef _LTSM_CONNECTOR_X11VNC_
#define _LTSM_CONNECTOR_X11VNC_

#include <memory>
#include <atomic>
#include <functional>

#include "librfb_server.h"
#include "ltsm_x11vnc.h"

namespace LTSM
{
    namespace Connector
    {
        /* Connector::VNC */
        class X11VNC : public DisplayProxy, protected RFB::ServerEncoder
        {
            std::unique_ptr<JsonObject> keymap;

            std::atomic<bool>   loopMessage{true};
	    std::atomic<bool>	sendBellFlag{false};
            XCB::Region         clientRegion;

        protected:
            // rfb server encoding
            XCB::RootDisplayExt* xcbDisplay(void) const override;
            bool                serviceAlive(void) const override;
            void                serviceStop(void) override;

        public:
            X11VNC(int fd, const JsonObject & jo);
            ~X11VNC() {}

            int		        communication(void) override;
        };
    }
}

#endif // _LTSM_CONNECTOR_X11VNC_
