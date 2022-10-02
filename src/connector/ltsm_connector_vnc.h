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

namespace LTSM
{
    struct vnc_error : public std::runtime_error
    {
        explicit vnc_error(const std::string & what) : std::runtime_error(what){}
        explicit vnc_error(const char* what) : std::runtime_error(what){}
    };

    namespace Connector
    {
        /* Connector::VNC */
        class VNC : public SignalProxy, protected RFB::X11Server
        {
            PixelFormat         format;

            std::unordered_map<uint32_t, int>
                                keymap;

            std::list<std::pair<std::string, size_t>> transfer;
            std::mutex          lockTransfer;

            std::atomic<bool>   loginWidgetStarted{false};
#ifdef LTSM_CHANNELS
            std::atomic<bool>   userSession{false};
#endif

        protected:
	    // rfb server encoding
            const PixelFormat & serverFormat(void) const override;
            void                xcbFrameBufferModify(FrameBuffer &) const override;

            // x11server
            XCB::RootDisplayExt*       xcbDisplay(void) override;
            const XCB::RootDisplayExt* xcbDisplay(void) const override;
            bool                xcbNoDamage(void) const override;
            bool                xcbAllow(void) const override;
            void                setXcbAllow(bool) override;

            bool                rfbClipboardEnable(void) const override;
            bool                rfbDesktopResizeEnabled(void) const override;
            RFB::SecurityInfo   rfbSecurityInfo(void) const override;
            int                 rfbUserKeycode(uint32_t) const override;

            // dbus virtual signals
            void                onLoginSuccess(const int32_t & display, const std::string & userName) override;
            void                onShutdownConnector(const int32_t & display) override;
            void                onHelperWidgetStarted(const int32_t & display) override;
            void                onSendBellSignal(const int32_t & display) override;
#ifdef LTSM_CHANNELS
            void                onCreateChannel(const int32_t & display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode) override;
            void                onDestroyChannel(const int32_t& display, const uint8_t& channel) override;
            void                onTransferAllow(const int32_t& display, const std::string& filepath, const std::string& tmpfile, const std::string& dstdir) override;
            void                onCreateListenner(const int32_t& display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode) override;
            void                onDestroyListenner(const int32_t& display, const std::string& client, const std::string& server) override;
#endif

            void                serverHandshakeVersionEvent(void) override;
            void                serverSelectEncodingsEvent(void) override;
            void                serverSecurityInitEvent(void) override;
            void                serverConnectedEvent(void) override;
            void                serverMainLoopEvent(void) override;
            void                serverDisplayResizedEvent(const XCB::Size &) override;
            void                serverEncodingsEvent(void) override;

#ifdef LTSM_CHANNELS
            // rfb channel client
            bool                isUserSession(void) const override;
            void                systemTransferFiles(const JsonObject &) override;
            void                systemClientVariables(const JsonObject &) override;
            void                systemKeyboardChange(const JsonObject &) override;
#endif

        public:
            VNC(sdbus::IConnection* conn, const JsonObject & jo);
            ~VNC();

            int		        communication(void) override;
        };
    }
}

#endif // _LTSM_CONNECTOR_VNC_
