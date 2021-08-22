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

#include "freerdp/freerdp.h"
#include "freerdp/listener.h"

namespace LTSM
{
    namespace Connector
    {
	class ProxySocket : private BaseStream
	{
	    std::atomic<bool>		loopTransmission;
	    std::thread			loopThread;
	    int				bridgeSock;
	    int				clientSock;
	    std::string			socketPath;
	    std::vector<uint8_t>	buf;

	protected:
            bool                        enterEventLoopAsync(void);

	public:
	    ProxySocket() : loopTransmission(false), bridgeSock(-1), clientSock(-1) {}
	    ~ProxySocket();

	    int				clientSocket(void) const;
            bool                        initUnixSockets(const std::string &);

	    void			startEventLoopBackground(void);
	    void			stopEventLoop(void);

	    static int			connectUnixSocket(const char* path);
	    static int			listenUnixSocket(const char* path);
	};

        /* Connector::RDP */
        class RDP : public ProxySocket, public SignalProxy
        {
            std::atomic<bool>           loopMessage;

        protected:
            // dbus virtual signals
            void                        onSendBellSignal(const int32_t & display) override {}
            void                        onShutdownConnector(const int32_t & display) override;
            void                        onHelperWidgetStarted(const int32_t & display) override;

        public:
            RDP(sdbus::IConnection* conn, const JsonObject & jo)
                : SignalProxy(conn, jo, "rdp"), loopMessage(false)
            {
                registerProxy();
            }

            ~RDP()
	    {
    		unregisterProxy();
	    }

            int		                communication(void) override;

	    static BOOL			clientPostConnect(freerdp_peer* client);
	    static BOOL			clientActivate(freerdp_peer* client);
	    static BOOL			clientSynchronizeEvent(rdpInput* input, UINT32 flags);
	    static BOOL			clientKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code);
	    static BOOL			clientUnicodeKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code);
	    static BOOL			clientMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
	    static BOOL			clientExtendedMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
	    static BOOL			clientRefreshRect(rdpContext* context, BYTE count, const RECTANGLE_16* areas);
	    static BOOL			clientSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* area);
	};
    }
}

#endif // _LTSM_CONNECTOR_RDP_
