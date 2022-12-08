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

#ifndef _LTSM_CONNECTOR_VNC_
#define _LTSM_CONNECTOR_VNC_

#include <list>
#include <array>
#include <memory>
#include <atomic>
#include <exception>
#include <unordered_map>

#include "librfb_x11server.h"
#include "ltsm_connector.h"
#include "ltsm_fuse_proxy.h"

namespace LTSM
{
    struct vnc_error : public std::runtime_error
    {
        explicit vnc_error(const std::string & what) : std::runtime_error(what){}
        explicit vnc_error(const char* what) : std::runtime_error(what){}
    };

    /// FuseSessionProxy
    class FuseSessionProxy : public sdbus::ProxyInterfaces<Session::FUSE_proxy>
    {
        ChannelClient* sender = nullptr;

        protected:
            void onRequestOpen(const std::string& path, const uint32_t& cookie, const int32_t& flags) override;
            void onRequestRead(const std::string& path, const uint32_t& cookie, const uint32_t& size, const int32_t& offset) override;
            void onRequestReadDir(const std::string& path, const uint32_t& cookie) override;
            void onRequestGetAttr(const std::string& path, const uint32_t& cookie) override;

        public:
            FuseSessionProxy(const std::string& address, ChannelClient &);
            virtual ~FuseSessionProxy();
    };

    namespace Connector
    {
        /* Connector::VNC */
        class VNC : public SignalProxy, protected RFB::X11Server
        {
            PixelFormat         serverPf;

            std::unordered_map<uint32_t, int>
                                keymap;

            std::list<std::pair<std::string, size_t>> transfer;
            std::mutex          lockTransfer;

            std::chrono::time_point<std::chrono::steady_clock>
                                idleSession;
            size_t              idleTimeoutSec = 0;

            std::atomic<bool>   loginWidgetStarted{false};
            std::unique_ptr<FuseSessionProxy> fuse;
            std::atomic<bool>   userSession{false};
            uid_t               shmUid = 0;

        protected:
	    // rfb server encoding
            const PixelFormat & serverFormat(void) const override;
            void                xcbFrameBufferModify(FrameBuffer &) const override;
            std::list<std::string> serverDisabledEncodings(void) const override;
            std::list<std::string> serverPrefferedEncodings(void) const override;
 
            // x11server
            bool                xcbNoDamageOption(void) const override;
            void                xcbDisableMessages(bool) override;
            bool                xcbAllowMessages(void) const override;

            bool                rfbClipboardEnable(void) const override;
            bool                rfbDesktopResizeEnabled(void) const override;
            RFB::SecurityInfo   rfbSecurityInfo(void) const override;
            int                 rfbUserKeycode(uint32_t) const override;

            void                recvKeyEvent(bool pressed, uint32_t keysym) override;
            void                recvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy) override;

            // dbus virtual signals
            void                onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t& userUid) override;
            void                onShutdownConnector(const int32_t & display) override;
            void                onHelperWidgetStarted(const int32_t & display) override;
            void                onSendBellSignal(const int32_t & display) override;

            // connector
            void                xcbAddDamage(const XCB::Region &) override;

            void                onLoginFailure(const int32_t & display, const std::string & msg) override;
            void                onCreateChannel(const int32_t & display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode, const std::string& speed) override;
            void                onDestroyChannel(const int32_t& display, const uint8_t& channel) override;
            void                onTransferAllow(const int32_t& display, const std::string& filepath, const std::string& tmpfile, const std::string& dstdir) override;
            void                onCreateListener(const int32_t& display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode, const std::string& speed, const uint8_t& limit) override;
            void                onDestroyListener(const int32_t& display, const std::string& client, const std::string& server) override;
            void                onDebugChannel(const int32_t& display, const uint8_t& channel, const bool& debug) override;
            void                onFuseSessionStart(const int32_t& display, const std::string& addresses, const std::string& mount) override;
            void                onTokenAuthCheckPkcs7(const int32_t& display, const std::string& serial, const std::string& pin, const uint32_t& cert, const std::vector<uint8_t>& pkcs7) override;

            void                serverHandshakeVersionEvent(void) override;
            void                serverSelectEncodingsEvent(void) override;
            void                serverSecurityInitEvent(void) override;
            void                serverConnectedEvent(void) override;
            void                serverMainLoopEvent(void) override;
            void                serverDisplayResizedEvent(const XCB::Size &) override;
            void                serverEncodingsEvent(void) override;

            // rfb channel client
            bool                isUserSession(void) const override;
            void                systemChannelError(const JsonObject &) override;
            void                systemTransferFiles(const JsonObject &) override;
            void                systemClientVariables(const JsonObject &) override;
            void                systemKeyboardChange(const JsonObject &) override;
            void                systemFuseProxy(const JsonObject &) override;
            void                systemTokenAuth(const JsonObject &) override;

        public:
            VNC(const JsonObject & jo) : SignalProxy(jo, "vnc") {}
            ~VNC();

            int		        communication(void) override;
        };
    }
}

#endif // _LTSM_CONNECTOR_VNC_
