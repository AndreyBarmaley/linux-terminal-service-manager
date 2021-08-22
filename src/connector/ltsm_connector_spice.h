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

#ifndef _LTSM_CONNECTOR_SPICE_
#define _LTSM_CONNECTOR_SPICE_

#include <gnutls/abstract.h>

#include "spice/protocol.h"
#include "ltsm_connector.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    struct RedLinkMess : SpiceLinkMess
    {
	uint32_t		connectionId;
	uint8_t			channelType;
	uint8_t			channelId;
	std::vector<uint8_t>	commonCaps;
	std::vector<uint8_t>	channelCaps;

	RedLinkMess() : connectionId(0), channelType(0), channelId(0) {}
    };

    namespace Connector
    {
        /* Connector::SPICE */
        class SPICE : public BaseStream, public SignalProxy
        {
            gnutls_privkey_t    rsaPrivate;
            gnutls_pubkey_t     rsaPublic;
	    std::array<uint8_t, SPICE_TICKET_PUBKEY_BYTES> publicKey;

        protected:
            // dbus virtual signals
            void                        onShutdownConnector(const int32_t & display) override {}
            void                        onHelperWidgetStarted(const int32_t & display) override {}
            void                        onSendBellSignal(const int32_t & display) override {}

	    std::pair<RedLinkMess, bool> recvLinkMess(void);
	    void			linkReplyOk(void);
	    void			linkReplyError(int err);
	    void			linkReplyData(const std::vector<uint32_t> & commonCaps, const std::vector<uint32_t> & channelCaps);

        public:
            SPICE(sdbus::IConnection* conn, const JsonObject & jo)
                : SignalProxy(conn, jo, "spice")
            {
                registerProxy();
            }

            ~SPICE()
            {
                unregisterProxy();
                gnutls_privkey_deinit(rsaPrivate);
                gnutls_pubkey_deinit(rsaPublic);
            }

            int		                communication(void) override;
        };
    }
}

#endif // _LTSM_CONNECTOR_SPICE_
