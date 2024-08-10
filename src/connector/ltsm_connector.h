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
#include "ltsm_service_proxy.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"

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
        std::string                     homeRuntime(void);

        /* Connector::SignalProxy */
        class SignalProxy : public sdbus::ProxyInterfaces<Manager::Service_proxy>
        {
        protected:
            std::list<std::unique_ptr<RenderPrimitive>>
                                        _renderPrimitives;

            std::string		        _conntype;
            std::string			_remoteaddr;

            const JsonObject*           _config = nullptr;

            std::atomic<int>            _xcbDisplayNum{0};
            std::atomic<bool>           _xcbDisable{true};

        private:
            // dbus virtual signals
            void                        onLoginFailure(const int32_t & display, const std::string & msg) override {}
            void                        onHelperSetLoginPassword(const int32_t& display, const std::string& login, const std::string& pass, const bool& autologin) override {}
	    void			onHelperWidgetCentered(const int32_t& display) override {}
            void                        onHelperWidgetTimezone(const int32_t& display, const std::string&) override {}
            void                        onSessionReconnect(const std::string & removeAddr, const std::string & connType) override {}
	    void			onSessionChanged(const int32_t& display) override {}
	    void			onDisplayRemoved(const int32_t& display) override {}
            void                        onCreateChannel(const int32_t & display, const std::string&, const std::string&, const std::string&, const std::string&, const std::string&) override {}
            void                        onDestroyChannel(const int32_t& display, const uint8_t& channel) override {};
            void                        onCreateListener(const int32_t& display, const std::string&, const std::string&, const std::string&, const std::string&, const std::string&, const uint8_t&, const uint32_t&) override {}
            void                        onDestroyListener(const int32_t& display, const std::string&, const std::string&) override {}
            void                        onTransferAllow(const int32_t& display, const std::string& filepath, const std::string& tmpfile,  const std::string& dstdir) override {}
            void                        onDebugChannel(const int32_t& display, const uint8_t& channel, const bool& debug) override {}
            void                        onTokenAuthAttached(const int32_t& display, const std::string& serial, const std::string& description, const std::vector<std::string>& certs) override {}
            void                        onTokenAuthDetached(const int32_t& display, const std::string& serial) override {}
            void                        onTokenAuthCheckPkcs7(const int32_t& display, const std::string& serial, const std::string& pin, const uint32_t& cert, const std::vector<uint8_t>& pkcs7) override {}
            void                        onTokenAuthReplyCheck(const int32_t& display, const std::string& serial, const uint32_t& cert, const std::string& decrypt) override {}

        protected:
            // dbus virtual signals
            void                        onDebugLevel(const int32_t& display, const std::string & level) override;
	    void			onPingConnector(const int32_t & display) override;
            void			onClearRenderPrimitives(const int32_t & display) override;
            void			onAddRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill) override;
            void			onAddRenderText(const int32_t & display, const std::string & text, const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color) override;

            void                        renderPrimitivesToFB(FrameBuffer &) const;

            virtual void                xcbAddDamage(const XCB::Region &) = 0;

    	    int	                        displayNum(void) const;
            bool                        xcbConnect(int screen, XCB::RootDisplay &);
	    void                        xcbDisableMessages(bool f);
	    bool                        xcbAllowMessages(void) const;

        public:
            SignalProxy(const JsonObject &, const char* conntype);
            virtual ~SignalProxy();

    	    virtual int	                communication(void) = 0;

	    std::string         	checkFileOption(const std::string &) const;

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
