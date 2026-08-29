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

#ifndef _LTSM_CONNECTOR_
#define _LTSM_CONNECTOR_

#include <list>
#include <chrono>
#include <atomic>
#include <string>

#include <boost/asio.hpp>

#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_service_proxy.h"
#include "ltsm_render_primitives.h"

namespace LTSM::Connector {
    using RenderPrimitivePtr = std::unique_ptr<RenderPrimitive>;

    std::string homeRuntime(void);

    enum class ConnectorType { VNC, LTSM, RDP };

    /// AuditService
#ifdef LTSM_WITH_AUDIT
    class AuditConnector : public AuditLog {
      public:
        AuditConnector() = default;
        ~AuditConnector() = default;

        void auditRemoteConnected(const std::string & ipaddr) const;
        void auditRemoteDisconnected(const std::string & ipaddr) const;
    };
#endif

    class BoostContext {
        const uint16_t concurency_ = 1;
        boost::asio::io_context ioc_;
        
      protected:
        inline boost::asio::io_context & ioc(void) { return ioc_; }
        inline uint16_t concurency(void) const { return concurency_; }
        boost::asio::any_io_executor get_executor(void) { return ioc_.get_executor(); }

      public:
        explicit BoostContext(uint16_t concurency);
        ~BoostContext() = default;

        void run(void);
    };

    class DBusProxy : public ApplicationJsonConfig, public BoostContext, public sdbus::ProxyInterfaces<Manager::Service_proxy> {
        boost::asio::steady_timer timer_idle_session_;

        std::list<RenderPrimitivePtr> renderPrimitives_;
        std::string connType_;
        std::string remoteAddr_;

        std::atomic<int> xcbDisplayNum_{0};
        std::atomic<bool> xcbDisable_{true};
        std::atomic<bool> idleSessionActive_{false};

        uint32_t idleTimeoutSec_{0};

#ifdef LTSM_WITH_AUDIT
        std::unique_ptr<AuditConnector> auditLog_;
#endif
      private:
        void checkIdleTimeoutCb(const boost::system::error_code &);

        // dbus virtual signals
        void onLoginFailure(const int32_t & display, const std::string & msg) override {}

        void onHelperSetLoginPassword(const int32_t & display, const std::string & login,
                                      const std::string & pass, const bool & autologin) override {}

        void onHelperSetTimezone(const int32_t & display, const std::string &) override {}

        void onHelperPkcs11ListennerStarted(const int32_t & display, const int32_t & connectorId) override {}

        void onSessionReconnect(const std::string & removeAddr, const std::string & connType) override {}

        void onDisplayRemoved(const int32_t & display) override {}

        void onCreateChannel(const int32_t & display, const std::string &, const std::string &,
                             const std::string &, const std::string &, const std::string &) override {}

        void onDestroyChannel(const int32_t & display, const uint8_t & channel) override {};

        void onCreateListener(const int32_t & display, const std::string &, const std::string &,
                              const std::string &, const std::string &, const std::string &, const uint8_t &, const uint32_t &) override {}

        void onDestroyListener(const int32_t & display, const std::string &,
                               const std::string &) override {}

        void onTransferAllow(const int32_t & display, const std::string & filepath,
                             const std::string & tmpfile, const std::string & dstdir) override {}

        void onDebugChannel(const int32_t & display, const uint8_t & channel,
                            const bool & debug) override {}

        void onSessionOnline(const int32_t & display, const std::string & userName) override {}

        void onSessionOffline(const int32_t & display, const std::string & userName) override {}

        void onSessionIdleTimeout(const int32_t & display, const std::string & userName) override {}

    protected:
        void asioStop(void);
        void setIdleTimeoutSec(uint32_t);
        void idleSessionReset(void);

        // dbus virtual signals
        void onPingConnector(const int32_t & display) override;
        void onClearRenderPrimitives(const int32_t & display) override;
        void onAddRenderRect(const int32_t & display,
                             const TupleRegion & rect,
                             const TupleColor & color, const bool & fill) override;
        void onAddRenderText(const int32_t & display, const std::string & text,
                             const TuplePosition & pos, const TupleColor & color) override;

        void renderPrimitivesToFB(FrameBuffer &) const;

        virtual void serverScreenUpdateRequest(const XCB::Region &) = 0;

        int displayNum(void) const;
        bool xcbConnect(int screen, XCB::RootDisplay &);

        boost::asio::awaitable<void> xcbConnectAwait(int screen, const std::string & xauthFile, XCB::RootDisplay &);

      public:
        DBusProxy(const ConnectorType &, const std::filesystem::path & confile, bool debug);
        virtual ~DBusProxy();

        virtual int start(void) = 0;
        void xcbDisableMessages(bool f);
        bool xcbAllowMessages(void) const;

        std::string checkFileOption(const std::string &) const;
        const std::string & connectorType(void) const;
        const std::string & remoteAddress(void) const;
    };

    /* Connector::startService */
    int startService(int argc, const char** argv);
}

#endif // _LTSM_CONNECTOR_
