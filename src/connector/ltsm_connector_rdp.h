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

#ifndef _LTSM_CONNECTOR_RDP_
#define _LTSM_CONNECTOR_RDP_

#include "ltsm_connector.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    namespace Connector
    {
        /* Connector::RDP */
        class RDP : public BaseStream, public SignalProxy
        {
        protected:
            // dbus virtual signals
            void                        onShutdownConnector(const int32_t & display) override {}
            void                        onHelperWidgetStarted(const int32_t & display) override {}
            void                        onSendBellSignal(const int32_t & display) override {}

        public:
            RDP(sdbus::IConnection* conn, const JsonObject & jo)
                : SignalProxy(conn, jo, "rdp")
            {
                registerProxy();
            }

            ~RDP()
            {
                unregisterProxy();
            }

            int		                communication(void) override;
        };
    }
}

#endif // _LTSM_CONNECTOR_RDP_
