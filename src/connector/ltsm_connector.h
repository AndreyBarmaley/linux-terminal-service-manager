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

#include "ltsm_global.h"
#include "ltsm_dbus_proxy.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    enum class RenderType { RenderRect, RenderText };

    struct RenderPrimitive
    {
        const RenderType type;

        RenderPrimitive(const RenderType & val) : type(val) {}
        virtual ~RenderPrimitive() {}
    };

    struct RenderRect : RenderPrimitive
    {
        sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> region;
        sdbus::Struct<uint8_t, uint8_t, uint8_t>            color;
        bool                                                fill;

        RenderRect(const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rt, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & col, bool v)
            : RenderPrimitive(RenderType::RenderRect), region(rt), color(col), fill(v) {}

        XCB::Region toRegion(void) const { return XCB::Region(std::get<0>(region), std::get<1>(region), std::get<2>(region), std::get<3>(region)); }
    };

    struct RenderText : RenderPrimitive
    {
        std::string	                                    text;
        sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> region;
        sdbus::Struct<uint8_t, uint8_t, uint8_t>            color;

        RenderText(const std::string & str, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rt, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & col)
            : RenderPrimitive(RenderType::RenderText), text(str), region(rt), color(col) {}

        XCB::Region toRegion(void) const { return XCB::Region(std::get<0>(region), std::get<1>(region), std::get<2>(region), std::get<3>(region)); }
    };

    namespace Connector
    {
        ///@brief codec exception
        struct CodecFailed
        {
            std::string err;
            CodecFailed(const std::string & str) : err(str) {}
        };

        /* Connector::SignalProxy */
        class SignalProxy : public sdbus::ProxyInterfaces<Manager::Service_proxy>
        {
        protected:
            sdbus::IConnection*         _conn;
            const JsonObject*           _config;
            int                         _display;
            std::string		        _conntype;
            std::string			_remoteaddr;

            std::list<std::unique_ptr<RenderPrimitive>>
                                        _renderPrimitives;

            std::atomic<bool>           _xcbDisableMessages;
            XCB::SharedDisplay          _xcbDisplay;

        private:
            // dbus virtual signals
            void                        onLoginFailure(const int32_t & display, const std::string & msg) override {}
            void                        onHelperSetLoginPassword(const int32_t& display, const std::string& login, const std::string& pass, const bool& autologin) override {}
	    void			onHelperWidgetCentered(const int32_t& display) override {}
            void                        onSessionReconnect(const std::string & removeAddr, const std::string & connType) override {}
	    void			onSessionChanged(const int32_t& display) override {}
	    void			onDisplayRemoved(const int32_t& display) override {}

        protected:
            // dbus virtual signals
            void                        onLoginSuccess(const int32_t & display, const std::string & userName) override;
            void                        onDebugLevel(const std::string & level) override;
	    void			onPingConnector(const int32_t & display) override;
            void			onClearRenderPrimitives(const int32_t & display) override;
            void			onAddRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill) override;
            void			onAddRenderText(const int32_t & display, const std::string & text, const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color) override;
	    virtual void		onAddDamage(const XCB::Region &) {}

            bool                        xcbConnect(int display);

        public:
            SignalProxy(sdbus::IConnection*, const JsonObject &, const char* conntype);
            virtual ~SignalProxy() {}

    	    virtual int	                communication(void) = 0;
	    std::string         	checkFileOption(const std::string &) const;

	    bool			isAllowXcbMessages(void) const;
	    void			setEnableXcbMessages(bool f);
        };

        /* Connector::Service */
        class Service : public ApplicationJsonConfig
        {
            std::string                 _type;

        public:
            Service(int argc, const char** argv);

            int    		        start(void);
        };
    }
}

#endif // _LTSM_CONNECTOR_
