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

#ifndef _LTSM_CONNECTOR_PROTO_
#define _LTSM_CONNECTOR_PROTO_

#include <list>
#include <array>
#include <memory>
#include <atomic>
#include <exception>
#include <unordered_map>

#include "librfb_x11server.h"
#include "ltsm_connector.h"

namespace LTSM::Connector
{
    struct proto_error : public std::runtime_error
    {
        explicit proto_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };

    using TupleFileSize = sdbus::Struct<std::string, uint32_t>;

    class ConnectorLtsm : public DBusProxy, protected RFB::X11Server
    {
        PixelFormat _serverPf;
        std::unordered_map<uint32_t, int> _keymap;

        std::list<TupleFileSize> _transferPlanned;
        std::mutex _lockTransfer;

        std::atomic<uint32_t> _frameRate{16};
        std::atomic<bool> _loginWidgetStarted{false};
        std::atomic<bool> _userSession{false};
        std::atomic<bool> _x11NoDamage{false};

        std::chrono::time_point<std::chrono::steady_clock> _idleSession;
        uint32_t _idleTimeoutSec = 0;

        uid_t _shmUid = 0;
        int _ltsmClientVersion = 0;

    protected:
        // rfb server encoding
        const PixelFormat & serverFormat(void) const override;
        void serverFrameBufferModifyEvent(FrameBuffer & ) const override;
        std::forward_list<std::string> serverDisabledEncodings(void) const override;

        // x11server
        bool xcbNoDamageOption(void) const override;
        void xcbDisableMessages(bool) override;
        bool xcbAllowMessages(void) const override;
        size_t frameRateOption(void) const override;

        bool rfbClipboardEnable(void) const override;
        bool rfbDesktopResizeEnabled(void) const override;
        RFB::SecurityInfo rfbSecurityInfo(void) const override;
        int rfbUserKeycode(uint32_t) const override;

        void serverRecvKeyEvent(bool pressed, uint32_t keysym) override;
        void serverRecvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy) override;

        // dbus virtual signals
        void onLoginSuccess(const int32_t & display, const std::string & userName,
                            const uint32_t & userUid) override;
        void onShutdownConnector(const int32_t & display) override;
        void onHelperWidgetStarted(const int32_t & display) override;
        void onSendBellSignal(const int32_t & display) override;

        // connector
        void serverScreenUpdateRequest(const XCB::Region & ) override;

        void onLoginFailure(const int32_t & display, const std::string & msg) override;
        void onCreateChannel(const int32_t & display, const std::string & client, const std::string & cmode,
                             const std::string & server, const std::string & smode, const std::string & speed) override;
        void onDestroyChannel(const int32_t & display, const uint8_t & channel) override;
        void onTransferAllow(const int32_t & display, const std::string & filepath, const std::string & tmpfile,
                             const std::string & dstdir) override;
        void onCreateListener(const int32_t & display, const std::string & client, const std::string & cmode,
                              const std::string & server, const std::string & smode, const std::string & speed, const uint8_t & limit,
                              const uint32_t & flags) override;
        void onDestroyListener(const int32_t & display, const std::string & client,
                               const std::string & server) override;
        void onDebugChannel(const int32_t & display, const uint8_t & channel, const bool & debug) override;

        void serverHandshakeVersionEvent(void) override;
        void serverEncodingSelectedEvent(void) override;
        void serverSecurityInitEvent(void) override;
        void serverConnectedEvent(void) override;
        void serverMainLoopEvent(void) override;
        void serverDisplayResizedEvent(const XCB::Size & ) override;
        void serverEncodingsEvent(void) override;

        // rfb channel client
        bool isUserSession(void) const override;
        void systemChannelError(const JsonObject & ) override;
        void systemTransferFiles(const JsonObject & ) override;
        void systemClientVariables(const JsonObject & ) override;
        void systemKeyboardChange(const JsonObject & ) override;
        void systemKeyboardEvent(const JsonObject & ) override;
        void systemCursorFailed(const JsonObject & jo) override;

        int remoteClientVersion(void) const override;

    protected:
        void loadKeymap(const std::string & file);
        void transferFilesPartial(std::list<TupleFileSize> files);

    public:
        ConnectorLtsm(const JsonObject & jo) : DBusProxy(jo, ConnectorType::LTSM) {}
        ~ConnectorLtsm();

        int communication(void) override;
    };
}

#endif // _LTSM_CONNECTOR_VNC_
