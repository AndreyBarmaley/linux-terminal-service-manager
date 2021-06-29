/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef _LTSM_SERVICE_
#define _LTSM_SERVICE_

#include <mutex>
#include <chrono>

#include <security/pam_appl.h>

#include "ltsm_global.h"
#include "ltsm_dbus_proxy.h"
#include "ltsm_application.h"
#include "ltsm_dbus_adaptor.h"
#include "ltsm_json_wrapper.h"

namespace LTSM
{
    namespace Manager
    {
        class Object;
    }

    struct XvfbSession
    {
        int                             pid1;
        int                             pid2;
        int                             width;
        int                             height;
        int                             uid;
        int                             gid;
        int                             durationlimit;
        int                             loginFailures;
        bool				shutdown;
	bool				checkconn;

        std::string                     user;
        std::string                     xauthfile;
        std::string                     mcookie;
        std::string                     remoteaddr;
        std::string                     conntype;
        std::string                     encryption;

        XvfbMode                        mode;
	SessionPolicy			policy;

        std::chrono::system_clock::time_point tpstart;

        pam_handle_t*                   pamh;

        XvfbSession() : pid1(0), pid2(0), width(0), height(0), uid(0), gid(0),
            durationlimit(0), loginFailures(0), shutdown(false), checkconn(false), mode(XvfbMode::SessionLogin), policy(SessionPolicy::AuthLock), pamh(nullptr) {}

        void            destroy(int killpid = 0);
    };

    // display, pid1, pid2, width, height, uid, gid, durationLimit, mode, policy, user, authfile, remoteaddr, conntype, encryption
    typedef sdbus::Struct<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, std::string, std::string, std::string, std::string, std::string>
            xvfb2tuple;

    class XvfbSessions
    {
    protected:
        INTMAP<int, XvfbSession>        _xvfb;
        std::mutex              	_mutex;

    public:
        ~XvfbSessions();

        XvfbSession*                    getXvfbInfo(int display);
        int                             findXvfbPIDs(int pid);
        int                             findUserSession(const std::string & username);
        XvfbSession*                    registryXvfbSession(int display, const XvfbSession &);
        void                            removeXvfbDisplay(int display);
        void                            unregistryChilds(int pid);
	void				confirmCheckConnection(int display);

        std::vector<xvfb2tuple>         toSessionsList(void);
    };

    namespace Manager
    {
        std::tuple<int, int, std::string, std::string>
                                        getUserInfo(const std::string &);
        int                             getGroupGid(const std::string &);
        std::list<std::string>          getGroupMembers(const std::string &);

        class Object : public XvfbSessions, public sdbus::AdaptorInterfaces<Service_adaptor>
        {
	    const Application*		_app;
            const JsonObject*		_config;
	    SessionPolicy		_sessionPolicy;
            int                         _loginFailuresConf;
            int                         _helperIdleTimeout;
            bool                        _helperAutoComplete;
            int                         _helperAutoCompeteMinUid;
            int                         _helperAutoCompeteMaxUid;
            std::string                 _helperTitle;
            std::string                 _helperDateFormat;
            std::vector<std::string>    _helperAccessUsersList;
    	    std::chrono::system_clock::time_point _tpsec3, _tpsec30;

        protected:
            void                        closefds(void) const;
	    void			openlog(void) const;
            int		                getFreeDisplay(void);
	    void			closeSystemSession(int display, XvfbSession &);
            std::string			createXauthFile(int display, const std::string & mcookie, const std::string & username, const std::string & remoteaddr);
            std::string                 createSessionConnInfo(const std::string & home, const XvfbSession*);
            int				runXvfbDisplay(int display, int width, int height, const std::string & xauthFile, const std::string & userLogin);
            int				runLoginHelper(int display, const std::string & xauthFile, const std::string & userLogin);
            int				runUserSession(int display, const std::string & sessionBin, const XvfbSession &);
	    void			setFileOwner(const std::string & file, int uid, int gid);
            bool			waitXvfbStarting(int display, uint32_t waitms);
            bool	                switchToUser(const std::string &);
            bool			checkXvfbSocket(int display);
            bool			checkXvfbLocking(int display);
            bool                        checkFileReadable(const std::string &);
	    void			removeXvfbSocket(int display);
            void			displayShutdown(int display, bool emitSignal);
            std::list<std::string>      getSystemUsersList(int uidMin, int uidMax) const;
            bool                        pamAuthenticate(const int32_t & display, const std::string & login, const std::string & password);

        public:
            Object(sdbus::IConnection &, const JsonObject &, const Application &);
            ~Object();

            void                        systemTick(void);
            void                        childEnded(int pid, int status);

        private:                        /* virtual dbus methods */
            int32_t                     busGetServiceVersion(void) override;
            int32_t                     busStartLoginSession(const std::string & remoteAddr, const std::string & connType) override;
            int32_t                     busStartUserSession(const int32_t & oldDisplay, const std::string & userName, const std::string & remoteAddr, const std::string & connType) override;
            std::string                 busCreateAuthFile(const int32_t & display) override;
            std::string                 busEncryptionInfo(const int32_t & display) override;
            bool                        busShutdownDisplay(const int32_t & display) override;
            bool                        busShutdownConnector(const int32_t & display) override;
            bool                        busConnectorTerminated(const int32_t & display) override;
            bool                        busConnectorSwitched(const int32_t & oldDisplay, const int32_t & newDisplay) override;
	    bool			busConnectorAlive(const int32_t & display) override;
            bool                        busSetDebugLevel(const std::string & level) override;
            bool                        busSetEncryptionInfo(const int32_t & display, const std::string & info) override;
	    bool			busSetSessionDurationSec(const int32_t & display, const uint32_t & duration) override;
	    bool			busSetSessionPolicy(const int32_t& display, const std::string& policy) override;
            bool                        busSendMessage(const int32_t& display, const std::string& message) override;

            bool                        helperIdleTimeoutAction(const int32_t & display) override;
            bool                        helperWidgetStartedAction(const int32_t & display) override;
            int32_t                     helperGetIdleTimeoutSec(const int32_t & display) override;
            std::vector<std::string>    helperGetUsersList(const int32_t & display) override;
            bool                        helperIsAutoComplete(const int32_t & display) override;
            std::string                 helperGetTitle(const int32_t & display) override;
            std::string                 helperGetDateFormat(const int32_t & display) override;

            bool                        busCheckAuthenticate(const int32_t & display, const std::string & login, const std::string & password) override;
            std::vector<xvfb2tuple>     busGetSessions(void) override;

            bool                        busRenderRect(const int32_t& display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>& rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color, const bool& fill) override;
            bool                        busRenderText(const int32_t& display, const std::string& text, const sdbus::Struct<int16_t, int16_t>& pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color) override;
            bool                        busRenderClear(const int32_t& display) override;
        };

        class Service : public ApplicationJsonConfig
        {
        protected:
            bool                        createXauthDir(void);

        public:
            Service(int argc, const char** argv);

            int                         start(void);
            static void                 signalHandler(int);
        };
    }
}

#endif // _LTSM_SERVICE_
