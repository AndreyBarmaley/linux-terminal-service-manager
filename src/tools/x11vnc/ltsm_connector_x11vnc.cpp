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

#include <exception>

#include "ltsm_application.h"
#include "ltsm_connector_x11vnc.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM {
    Connector::X11VNC::X11VNC(asio::io_context & ctx, const JsonObject & jo) : RFB::X11Server(ctx), ioc_{ctx} {
        config_ = & jo;
        remoteaddr_.assign("local");

        if(auto env = std::getenv("REMOTE_ADDR")) {
            remoteaddr_.assign(env);
        }

        loadKeymap();
    }

    bool Connector::X11VNC::loadKeymap(void) {
        if(! config_->hasKey("keymapfile")) {
            return false;
        }

        auto jc = JsonContentFile(config_->getString("keymapfile"));

        if(! jc.isObject()) {
            Application::error("{}: invalid keymap file", NS_FuncNameV);
            return false;
        }

        auto jo = jc.toObject();

        for(const auto & skey : jo.keys()) {
            try {
                keymap_.emplace(std::stoi(skey, nullptr, 0), jo.getInteger(skey));
            } catch(const std::exception &) {
            }
        }

        return keymap_.size();
    }

    bool Connector::X11VNC::rfbClipboardEnable(void) const {
        return config_->getBoolean("ClipBoard");
    }

    bool Connector::X11VNC::rfbDesktopResizeEnabled(void) const {
        return config_->getBoolean("DesktopResized");
    }

    bool Connector::X11VNC::xcbNoDamageOption(void) const {
        return config_->getBoolean("nodamage", false);
    }

    bool Connector::X11VNC::xcbAllowMessages(void) const {
        return ! xcb_disable_;
    }

    void Connector::X11VNC::xcbDisableMessages(bool f) {
        xcb_disable_ = f;
    }

    int Connector::X11VNC::rfbUserKeycode(uint32_t keysym) const {
        auto it = keymap_.find(keysym);
        return it != keymap_.end() ? it->second : 0;
    }

    const PixelFormat & Connector::X11VNC::serverFormat(void) const {
        return pf_;
    }

    std::forward_list<std::string> Connector::X11VNC::serverDisabledEncodings(void) const {
        return {};
    }

    RFB::SecurityInfo Connector::X11VNC::rfbSecurityInfo(void) const {
        RFB::SecurityInfo secInfo;
        secInfo.authNone = config_->getBoolean("noauth", false);
        secInfo.authVnc = config_->hasKey("passwdfile");
        secInfo.passwdFile = config_->getString("passwdfile");
        secInfo.authVenCrypt = ! config_->getBoolean("notls", false);
        secInfo.tlsPriority = "NORMAL:+ANON-ECDH:+ANON-DH";
        secInfo.tlsAnonMode = true;
        secInfo.tlsDebug = 0;

        if(Application::isDebugLevel(DebugLevel::Debug)) {
            secInfo.tlsDebug = 1;
        } else if(Application::isDebugLevel(DebugLevel::Trace)) {
            secInfo.tlsDebug = 3;
        }

        return secInfo;
    }

    asio::awaitable<bool> Connector::X11VNC::xcbConnect(void) {
        // FIXM XAUTH
        std::string xauthFile = config_->getString("authfile");
        Application::debug(DebugType::App, "{}: xauthfile: `{}'", NS_FuncNameV, xauthFile);
        // Xvfb: wait display starting
        setenv("XAUTHORITY", xauthFile.c_str(), 1);
        size_t screen = config_->getInteger("display", 0);

        try {
            xcbDisplay()->displayReconnect(screen);
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            co_return false;
        }

        Application::info("{}: display: {}, size: {}, depth: {}",
                NS_FuncNameV, screen, xcbDisplay()->size(), xcbDisplay()->depth());
        Application::debug(DebugType::App, "{}: xcb max request: {}",
                NS_FuncNameV, xcbDisplay()->getMaxRequest());
        const xcb_visualtype_t* visual = xcbDisplay()->visual();

        if(! visual) {
            Application::error("{}: xcb visual empty", NS_FuncNameV);
            co_return false;
        }

        co_await xcbShmInit();

        // init server format
        pf_ = PixelFormat(xcbDisplay()->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);
        co_return true;
    }

    asio::awaitable<void> Connector::X11VNC::connectorHandshakeVersionAwait(void) {
        bool success = co_await xcbConnect();
        if(! success) {
            Application::error("{}: {}", NS_FuncNameV, "xcb connect failed");
            stop();
        }
    }

    void Connector::X11VNC::stop(void) noexcept {
        X11Server::rfbStop();
    }

    uint16_t Connector::X11VNC::encodingThreads(void) const {
        return 1;
    }

    std::future<BinaryBuf> Connector::X11VNC::postEncoderJob(RFB::PostEncoderJobCb && func, XCB::Region reg) const {
        std::promise<BinaryBuf> prom;
        auto ret = prom.get_future();
        try {
            prom.set_value(func(reg));
        } catch(const std::exception&) {
            prom.set_exception_at_thread_exit(std::current_exception());
        }
        return ret;
    }
}
