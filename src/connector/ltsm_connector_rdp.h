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
#include <atomic>
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
        PixelFormat serverPf_;
        XCB::Region damageRegion_;

        std::once_flag stopFlag_;
        std::atomic<uint16_t> update_jobs_{0};

        uint32_t frameRate_{16};
        bool x11NoDamage_{false};

        std::unique_ptr<JsonObject> keymap_;

        boost::asio::cancellation_signal rdp_events_cancel_;
        boost::asio::cancellation_signal xcb_events_cancel_;
        boost::asio::strand<boost::asio::any_io_executor> xcb_strand_;
        boost::asio::strand<boost::asio::any_io_executor> rdp_strand_;
        boost::asio::steady_timer tm_not_activated_;

      protected:
        void notActivatedCb(const boost::system::error_code &);

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

        void xcbUpdateDisplay(void);
        void xcbKeyboardEvent(uint16_t flags, uint16_t code);
        void xcbMouseEvent(uint16_t flags, uint16_t posx, uint16_t posy);

        bool rdpUpdateBitmapPlanar(const XCB::Region &, const XCB::PixmapInfoReply &);
        bool rdpUpdateBitmapInterleaved(const XCB::Region &, const XCB::PixmapInfoReply &);
        void rdpDesktopResizeEvent(const XCB::Size &);

        bool rdpChannelsInit(void);
        void rdpChannelsFree(void);

      public:
        ConnectorRdp(const std::filesystem::path & confile, bool debug);
        ~ConnectorRdp();

        void stop(void) noexcept;
        int start(void) final;

        uint32_t frameRateOption(void) const;
        bool xcbNoDamageOption(void) const;

        boost::asio::awaitable<void> xcbEventsAwait(void);
        boost::asio::awaitable<void> createX11SessionAwait(void);

        void setEncryptionInfo(const std::string &);
        void setAutoLogin(const std::string &, const std::string &);

        bool serverCapabilitiesEvent(rdpSettings*);
        bool clientCapabilitiesEvent(const rdpSettings*);
        bool serverActivateEvent(const rdpSettings*);
        bool serverAdjustMonitorsEvent(const rdpSettings*);
        bool serverPostConnectEvent(const rdpSettings*);
        bool serverKeyboardEvent(uint16_t flags, uint16_t code);
        bool serverMouseEvent(uint16_t flags, uint16_t posx, uint16_t posy);
        bool serverRefreshEvent(uint8_t counts, const RECTANGLE_16*);
        bool serverSuppressEvent(bool allow);
        void serverDisconnectEvent(void);
        bool serverCloseEvent(void);
    };
}

#endif // _LTSM_CONNECTOR_RDP_
