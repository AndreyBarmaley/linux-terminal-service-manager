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

#include <mutex>
#include <exception>

#include "ltsm_sockets.h"
#include "ltsm_connector.h"

#include "freerdp/freerdp.h"
#include "freerdp/listener.h"

namespace LTSM::Connector {
    struct FreeRdpEvents;

    struct rdp_error : public std::runtime_error {
        explicit rdp_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };

    class ConnectorRdp : public DBusProxy, public XCB::RootDisplay, protected InetStream {
        std::unique_ptr<FreeRdpEvents> rdpEvents_;
        PixelFormat serverFormat_;
        XCB::Region damageRegion_;
        std::once_flag stopFlag_;
        uint32_t frameRate_{16};

      protected:
        // dbus virtual signals
        void onLoginSuccess(const int32_t & display, const std::string & userName,
                            const uint32_t & userUid) override;
        void onSendBellSignal(const int32_t & display) override;
        void onShutdownConnector(const int32_t & display) override;

        // connector
        void serverScreenUpdateRequest(const XCB::Region &) override;

        // root display
        void xcbDamageNotifyEvent(const xcb_rectangle_t &, uint8_t level) override;
        void xcbRandrScreenChangedEvent(const XCB::Size &, const xcb_randr_notify_event_t &) override;
        void xcbXkbGroupChangedEvent(int) override;

        bool updateRegionEvent(const XCB::Region &);
        bool updateBitmapPlanar(const XCB::Region &, const XCB::PixmapInfoReply &);
        bool updateBitmapInterleaved(const XCB::Region &, const XCB::PixmapInfoReply &);
        void desktopResizeEvent(freerdp_peer &, uint16_t, uint16_t);
        void disconnectedEvent(void);

        bool channelsInit(void);
        void channelsFree(void);

      public:
        ConnectorRdp(const std::filesystem::path & confile, bool debug);
        ~ConnectorRdp();

        void stop(void) noexcept;
        int start(void) final;

        bool createX11Session(uint8_t depth);
        bool updateDisplayEvent(bool nodamage);
        uint32_t frameRateOption(void) const;

        void setEncryptionInfo(const std::string &);
        void setAutoLogin(const std::string &, const std::string &);

        // freerdp callback func
        static BOOL rdpServerPostConnect(freerdp_peer* client);
        static BOOL rdpServerActivate(freerdp_peer* client);
        static BOOL rdpServerAuthenticate(freerdp_peer* client, const char** user, const char** domain,
                                         const char** password);
        static BOOL rdpServerSynchronizeEvent(rdpInput* input, UINT32 flags);
        static BOOL rdpServerKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code);
        static BOOL rdpServerMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
        static BOOL rdpServerRefreshRect(rdpContext* context, BYTE count, const RECTANGLE_16* areas);
        static BOOL rdpServerSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* area);
        static BOOL rdpServerRefreshRequest(freerdp_peer* client);

        static BOOL rdpServerClose(freerdp_peer* client);
        static void rdpServerDisconnect(freerdp_peer* client);
        static BOOL rdpServerCapabilities(freerdp_peer* client);
        static BOOL rdpServerAdjustMonitorsLayout(freerdp_peer* client);
        static BOOL rdpServerClientCapabilities(freerdp_peer* client);
    };
}

#endif // _LTSM_CONNECTOR_RDP_
