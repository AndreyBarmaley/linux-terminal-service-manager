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

#ifndef _LTSM_X11VNC_
#define _LTSM_X11VNC_

#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_framebuffer.h"

#define LTSM_X11VNC_VERSION 20220826

namespace LTSM
{
    namespace Connector
    {
        ///@brief codec exception
        struct CodecFailed
        {
            std::string err;
            CodecFailed(const std::string & str) : err(str) {}
        };

        /* Connector::DisplayProxy */
        class DisplayProxy
        {
        protected:
            const JsonObject*           _config;
            std::string			_remoteaddr;

            std::atomic<bool>           _xcbDisableMessages;
            XCB::SharedDisplay          _xcbDisplay;

        protected:
            bool                        xcbConnect(void);

        public:
            DisplayProxy(const JsonObject &);
            virtual ~DisplayProxy() {}

    	    virtual int	                communication(void) = 0;

	    bool			isAllowXcbMessages(void) const;
	    void			setEnableXcbMessages(bool f);
        };

        /* Connector::Service */
        class Service : public Application
        {
            JsonObject                  _config;

            int    		        startInetd(void);
            int    		        startSocket(int port);

        public:
            Service(int argc, const char** argv);

            int    		        start(void);
        };
    }
}

#endif // _LTSM_CONNECTOR_
