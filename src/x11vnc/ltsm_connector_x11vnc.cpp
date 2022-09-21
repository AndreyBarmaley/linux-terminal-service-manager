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

namespace LTSM
{
    Connector::X11VNC::X11VNC(int fd, const JsonObject & jo) : RFB::X11Server(fd)
    {
        _config = & jo;
        _remoteaddr.assign("local");
    
        if(auto env = std::getenv("REMOTE_ADDR"))
            _remoteaddr.assign(env);

        loadKeymap();
    }

    bool Connector::X11VNC::loadKeymap(void)
    {
        if(! _config->hasKey("keymapfile"))
            return false;

        auto jc = JsonContentFile(_config->getString("keymapfile"));

        if(! jc.isObject())
        {
            Application::error("%s: invalid keymap file", __FUNCTION__);
            return false;
        }

        auto jo = jc.toObject();
        for(auto & skey : jo.keys())
        {
            try
            {
                keymap.emplace(std::stoi(skey, nullptr, 0), jo.getInteger(skey));
            }
            catch(const std::exception &)
            {
            }
        }

        return keymap.size();
    }

    bool Connector::X11VNC::rfbClipboardEnable(void) const
    {
        return _config->getBoolean("ClipBoard");
    }

    bool Connector::X11VNC::rfbDesktopResizeEnabled(void) const
    {
        return _config->getBoolean("DesktopResized");
    }

    XCB::RootDisplayExt* Connector::X11VNC::xcbDisplay(void)
    {
        return _xcbDisplay.get();
    }

    const XCB::RootDisplayExt* Connector::X11VNC::xcbDisplay(void) const
    {
        return _xcbDisplay.get();
    }

    bool Connector::X11VNC::xcbNoDamage(void) const
    {
        return _config->getBoolean("nodamage", false);
    }

    bool Connector::X11VNC::xcbAllow(void) const
    {
        return ! _xcbDisableMessages;
    }

    void Connector::X11VNC::setXcbAllow(bool f)
    {
        _xcbDisableMessages = !f;
    }

    int Connector::X11VNC::rfbUserKeycode(uint32_t keysym) const
    {
        auto it = keymap.find(keysym);
        return it != keymap.end() ? it->second : 0;
    }

    const PixelFormat & Connector::X11VNC::serverFormat(void) const
    {
        return _format;
    }

    RFB::SecurityInfo Connector::X11VNC::rfbSecurityInfo(void) const
    {
        RFB::SecurityInfo secInfo;
        secInfo.authNone = _config->getBoolean("noauth", false);
        secInfo.authVnc = _config->hasKey("passwdfile");
        secInfo.passwdFile = _config->getString("passwdfile");
        secInfo.authVenCrypt = ! _config->getBoolean("notls", false);
        secInfo.tlsPriority = "NORMAL:+ANON-ECDH:+ANON-DH";
        secInfo.tlsAnonMode = true;
        secInfo.tlsDebug = 0;

        if(Application::isDebugLevel(DebugLevel::SyslogDebug))
            secInfo.tlsDebug = 1;
        else
        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
            secInfo.tlsDebug = 3;

        return secInfo;
    }

    bool Connector::X11VNC::xcbConnect(void)
    {
        // FIXM XAUTH
        std::string xauthFile = _config->getString("authfile");
        std::string displayAddr = _config->getString("display");
        Application::debug("%s: display addr: `%s'", __FUNCTION__, displayAddr.c_str());
        Application::debug("%s: xauthfile: `%s'", __FUNCTION__, xauthFile.c_str());
        // Xvfb: wait display starting
        setenv("XAUTHORITY", xauthFile.c_str(), 1);

        try
        {
            _xcbDisplay.reset(new XCB::RootDisplayExt(displayAddr));
        }
        catch(const std::exception & err)
        {
            Application::error("exception: %s", err.what());
            return false;
        }

        _xcbDisplay->resetInputs();

        Application::info("%s: display info, size: [%d,%d], depth: %d", __FUNCTION__, _xcbDisplay->width(), _xcbDisplay->height(), _xcbDisplay->depth());
        Application::debug("%s: xcb max request: %d", __FUNCTION__, _xcbDisplay->getMaxRequest());

        const xcb_visualtype_t* visual = _xcbDisplay->visual();
        if(! visual)
        {
            Application::error("%s: xcb visual empty", __FUNCTION__);
            return false;
        }

        // init server format
        _format = PixelFormat(_xcbDisplay->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);

        return true;
    }

    void Connector::X11VNC::serverHandshakeVersionEvent(void)
    {
        if(! xcbConnect())
        {
            Application::error("%s: xcb connect: failed", __FUNCTION__);
            throw std::runtime_error("serverHandshakeVersionEvent");
        }
    }
}
