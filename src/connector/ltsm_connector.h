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

#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_service_proxy.h"
#include "ltsm_render_primitives.h"

namespace LTSM::Connector {
    using RenderPrimitivePtr = std::unique_ptr<RenderPrimitive>;

    std::string homeRuntime(void);

    enum class ConnectorType { VNC, LTSM, RDP };

    class DBusProxy : public sdbus::ProxyInterfaces<Manager::Service_proxy> {
      protected:
        std::list<RenderPrimitivePtr> _renderPrimitives;

        std::string _conntype;
        std::string _remoteaddr;

        const JsonObject & _config;

        std::atomic<int> _xcbDisplayNum{0};
        std::atomic<bool> _xcbDisable{true};

        std::chrono::time_point<std::chrono::steady_clock> _idleSessionTp;
        uint32_t _idleTimeoutSec = 0;

      private:
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
        void xcbDisableMessages(bool f);
        bool xcbAllowMessages(void) const;

      public:
        DBusProxy(const JsonObject &, const ConnectorType &);
        virtual ~DBusProxy();

        virtual int communication(void) = 0;

        std::string checkFileOption(const std::string &) const;
        const std::string & connectorType(void) const;

        void checkIdleTimeout(void);
    };

    /* Connector::Service */
    class Service : public ApplicationJsonConfig {
        std::string _type;

      public:
        Service(int argc, const char** argv);

        int start(void);
    };
}

#endif // _LTSM_CONNECTOR_
