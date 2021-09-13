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

#include "ltsm_sockets.h"
#include "ltsm_connector.h"

#include "freerdp/freerdp.h"
#include "freerdp/listener.h"

namespace LTSM
{
    struct FreeRdpCallback;

    namespace Connector
    {
        /* Connector::RDP */
        class RDP : public SignalProxy, protected ProxySocket
        {
            std::atomic<bool>           helperStartedFlag;
            std::atomic<bool>           loopShutdownFlag;
            std::atomic<bool>           clientUpdatePartFlag;
	    std::unique_ptr<FreeRdpCallback> freeRdp;

        protected:
            // dbus virtual signals
	    void                	onLoginSuccess(const int32_t & display, const std::string & userName) override;
            void                        onSendBellSignal(const int32_t & display) override {}
            void                        onShutdownConnector(const int32_t & display) override;
            void                        onHelperWidgetStarted(const int32_t & display) override;

	    bool			clientUpdateEvent(freerdp_peer &, const XCB::Region &, const XCB::PixmapInfoReply &);
	    bool			clientUpdateRfx(freerdp_peer &, const XCB::Region &, const XCB::PixmapInfoReply &);
	    bool			clientUpdateBitmapPlanar(freerdp_peer &, const XCB::Region &, const XCB::PixmapInfoReply &);
	    bool			clientUpdateBitmapInterleaved(freerdp_peer &, const XCB::Region &, const XCB::PixmapInfoReply &);
            void                        clientDesktopResizeEvent(freerdp_peer &, uint16_t, uint16_t);
	    void                	clientDisconnectedEvent(void);

        public:
            RDP(sdbus::IConnection* conn, const JsonObject & jo);
            ~RDP();

            int		                communication(void) override;
	    bool			createX11Session(void);
	    void			setEncryptionInfo(const std::string &);

	    // freerdp callback func
	    static BOOL			cbClientPostConnect(freerdp_peer* client);
	    static BOOL			cbClientActivate(freerdp_peer* client);
	    static BOOL			cbClientAuthenticate(freerdp_peer* client, const char** user, const char** domain, const char** password);
	    static BOOL			cbClientSynchronizeEvent(rdpInput* input, UINT32 flags);
	    static BOOL			cbClientKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code);
	    static BOOL			cbClientMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
	    static BOOL			cbClientRefreshRect(rdpContext* context, BYTE count, const RECTANGLE_16* areas);
	    static BOOL			cbClientSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* area);
	    static BOOL			cbClientRefreshRequest(freerdp_peer* client);
	};
    }
}

#endif // _LTSM_CONNECTOR_RDP_
