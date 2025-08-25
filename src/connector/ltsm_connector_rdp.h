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

#include <exception>

#include "ltsm_sockets.h"
#include "ltsm_connector.h"

#include "freerdp/freerdp.h"
#include "freerdp/listener.h"

namespace LTSM::Connector
{
    struct FreeRdpCallback;

    struct rdp_error : public std::runtime_error
    {
        explicit rdp_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };

    class ConnectorRdp : public DBusProxy, public XCB::RootDisplay, protected ProxySocket
    {
        std::atomic<bool> helperStartedFlag{false};
        std::atomic<bool> loopShutdownFlag{false};
        std::atomic<bool> updatePartFlag{true};
        std::unique_ptr<FreeRdpCallback> freeRdp;
        PixelFormat serverFormat;
        XCB::Region damageRegion;

    protected:
        // dbus virtual signals
        void onLoginSuccess(const int32_t & display, const std::string & userName,
                            const uint32_t & userUid) override;
        void onSendBellSignal(const int32_t & display) override;
        void onShutdownConnector(const int32_t & display) override;
        void onHelperWidgetStarted(const int32_t & display) override;

        // connector
        void serverScreenUpdateRequest(const XCB::Region & ) override;

        // root display
        void xcbDamageNotifyEvent(const xcb_rectangle_t &) override;
        void xcbRandrScreenChangedEvent(const XCB::Size &, const xcb_randr_notify_event_t &) override;
        void xcbXkbGroupChangedEvent(int) override;

        bool updateEvent(const XCB::Region & );
        bool updateBitmapPlanar(const XCB::Region &, const XCB::PixmapInfoReply & );
        bool updateBitmapInterleaved(const XCB::Region &, const XCB::PixmapInfoReply & );
        void desktopResizeEvent(freerdp_peer &, uint16_t, uint16_t);
        void disconnectedEvent(void);

        bool xcbEventLoopAsync(bool nodamage);

        bool channelsInit(void);
        void channelsFree(void);

    public:
        ConnectorRdp(const JsonObject & );
        ~ConnectorRdp();

        int communication(void) override;
        bool createX11Session(uint8_t depth);
        void setEncryptionInfo(const std::string & );
        void setAutoLogin(const std::string &, const std::string & );

        // freerdp callback func
        static BOOL cbServerPostConnect(freerdp_peer * client);
        static BOOL cbServerActivate(freerdp_peer * client);
        static BOOL cbServerAuthenticate(freerdp_peer * client, const char** user, const char** domain,
                                         const char** password);
        static BOOL cbServerSynchronizeEvent(rdpInput * input, UINT32 flags);
        static BOOL cbServerKeyboardEvent(rdpInput * input, UINT16 flags, UINT16 code);
        static BOOL cbServerMouseEvent(rdpInput * input, UINT16 flags, UINT16 x, UINT16 y);
        static BOOL cbServerRefreshRect(rdpContext * context, BYTE count, const RECTANGLE_16 * areas);
        static BOOL cbServerSuppressOutput(rdpContext * context, BYTE allow, const RECTANGLE_16 * area);
        static BOOL cbServerRefreshRequest(freerdp_peer * client);

        static BOOL cbServerClose(freerdp_peer * client);
        static void cbServerDisconnect(freerdp_peer * client);
        static BOOL cbServerCapabilities(freerdp_peer * client);
        static BOOL cbServerAdjustMonitorsLayout(freerdp_peer * client);
        static BOOL cbServerClientCapabilities(freerdp_peer * client);
    };
}

#endif // _LTSM_CONNECTOR_RDP_
