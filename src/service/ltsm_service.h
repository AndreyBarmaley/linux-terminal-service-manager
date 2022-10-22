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

#include <chrono>
#include <future>
#include <stdexcept>
#include <functional>
#include <filesystem>
#include <string_view>

#include <security/pam_appl.h>
#include "alexab_safe_ptr.h"

#include "ltsm_global.h"
#include "ltsm_dbus_proxy.h"
#include "ltsm_application.h"
#include "ltsm_dbus_adaptor.h"
#include "ltsm_json_wrapper.h"

namespace LTSM
{
    struct service_error : public std::runtime_error
    {
        explicit service_error(const std::string & what) : std::runtime_error(what){}
        explicit service_error(const char* what) : std::runtime_error(what){}
    };

    namespace Manager
    {
        class Object;
    }

    enum class XvfbMode { SessionLogin, SessionOnline, SessionSleep };
    enum class SessionPolicy { AuthLock, AuthTake, AuthShare };

    struct XvfbSession
    {
        int                             pid1 = 0; // xvfb pid
        int                             pid2 = 0; // session pid
        int                             width = 0;
        int                             height = 0;
        uid_t                           uid = 0;
        gid_t                           gid = 0;
        int                             durationlimit = 0;
        int                             loginFailures = 0;
        bool				shutdown = false;
	bool				checkconn = false;

        std::string                     user;
        std::string                     display;
        std::string                     xauthfile;
        std::string                     remoteaddr;
        std::string                     conntype;
        std::string                     encryption;

        std::unordered_map<std::string, std::string>
                                        environments;

        std::unordered_map<std::string, std::string>
                                        options;

        std::shared_future<int>         idleActionRunning;

        XvfbMode                        mode = XvfbMode::SessionLogin;
	SessionPolicy			policy = SessionPolicy::AuthLock;
        bool                            allowTransfer = true;

        std::chrono::system_clock::time_point tpstart;

        pam_handle_t*                   pamh = nullptr;

        XvfbSession() = default;

	void				destroy(void);
    };

    // display, pid1, pid2, width, height, uid, gid, durationLimit, mode, policy, user, authfile, remoteaddr, conntype, encryption
    typedef sdbus::Struct<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, std::string, std::string, std::string, std::string, std::string>
            xvfb2tuple;

    typedef std::pair<int, std::vector<uint8_t>> StatusStdout;

    typedef std::pair<pid_t, std::shared_future<int>> PidStatus;
    typedef std::pair<pid_t, std::future<StatusStdout>> PidStatusStdout;

    class XvfbSessions
    {
    protected:
        sf::safe_ptr< std::map<int, XvfbSession> > _xvfb;

    public:
        XvfbSessions() = default;
        virtual ~XvfbSessions();

        XvfbSession*                    getXvfbInfo(int display);
        std::pair<int, XvfbSession*>    findUserSession(const std::string & username);
        XvfbSession*                    registryXvfbSession(int display, XvfbSession &&);
        void                            removeXvfbDisplay(int display);

        std::vector<xvfb2tuple>         toSessionsList(void);
    };

    namespace Manager
    {
        std::tuple<uid_t, gid_t, std::filesystem::path, std::string>
                                        getUserInfo(std::string_view);
        uid_t                           getUserUid(std::string_view);
        gid_t                           getUserGid(std::string_view);
        std::filesystem::path           getUserHome(std::string_view);
        gid_t                           getGroupGid(std::string_view);
        std::list<std::string>          getGroupMembers(std::string_view);
        std::list<std::string>          getSystemUsersRange(int uidMin, int uidMax);
        std::list<std::string>          getSessionDbusAddresses(std::string_view);
        void                            closefds(void);
        bool                            checkFileReadable(const std::filesystem::path &);
	void			        setFileOwner(const std::filesystem::path & file, uid_t uid, gid_t gid);
	bool			        runSystemScript(int dysplay, const std::string & user, const std::string & cmd);
        bool	                        switchToUser(const std::string &);
        std::string                     quotedString(std::string_view);

        class Object : public XvfbSessions, public sdbus::AdaptorInterfaces<Service_adaptor>
        {
	    std::list<PidStatus>	_childsRunning;
            std::mutex                  _lockRunning;

            std::list<std::string>      _allowTransfer;
            std::mutex                  _lockTransfer;

            std::unique_ptr<Tools::BaseTimer> timer1, timer2, timer3;

	    const Application*		_app = nullptr;
            const JsonObject*		_config = nullptr;
            std::atomic<bool>           _running = false;
            bool                        _loginsDisable = false;

            pid_t                       runSessionCommandSafe(const XvfbSession*, const std::filesystem::path &, std::list<std::string>);
            void                        waitPidBackgroundSafe(pid_t pid);

            bool                        sessionRunZenity(const XvfbSession*, std::initializer_list<std::string>);

#ifdef LTSM_CHANNELS
            static void                 transferFileStartBackground(Object* owner, const XvfbSession* xvfb, int display,
                                            std::string tmpfile, std::string dstfile, uint32_t filesz);
            static void                 transferFilesRequestCommunication(Object* owner, const XvfbSession* xvfb,
                                            int display, std::filesystem::path zenity, std::vector<sdbus::Struct<std::string, uint32_t>> files,
                                            std::function<void(int, const std::vector<sdbus::Struct<std::string, uint32_t>> &)> emitTransferReject, std::shared_future<int>);
#endif

        protected:
	    void			openlog(void) const;
            int		                getFreeDisplay(void) const;
	    void			closeSystemSession(int display, XvfbSession &);
            std::filesystem::path	createXauthFile(int display, const std::vector<uint8_t> & mcookie, const std::string & username, const std::string & remoteaddr);
            std::filesystem::path       createSessionConnInfo(const std::filesystem::path & home, const XvfbSession*);
            pid_t                       runXvfbDisplay(int display, uint8_t depth, uint16_t width, uint16_t height, const std::filesystem::path & xauthFile, const std::string & userLogin);
            int				runUserSession(int display, const std::filesystem::path & sessionBin, const XvfbSession &);
	    void			runSessionScript(int dysplay, const std::string & user, const std::string & cmd);
            bool			waitXvfbStarting(int display, uint32_t waitms) const;
            bool			checkXvfbSocket(int display) const;
            bool			checkXvfbLocking(int display) const;
	    void			removeXvfbSocket(int display) const;
            bool			displayShutdown(int display, bool emitSignal, XvfbSession*);
            bool                        pamAuthenticate(const int32_t & display, const std::string & login, const std::string & password);
            std::list<std::string>      getAllowLogins(void) const;

	    void			sessionsTimeLimitAction(void);
	    void			sessionsEndedAction(void);
	    void			sessionsCheckAliveAction(void);

            void                        childEndedEvent(void);

        public:
            Object(sdbus::IConnection &, const JsonObject &, const Application &);
            ~Object();

            bool                        isRunning(void) const;
            void                        shutdown(void);

        private:                        /* virtual dbus methods */
            int32_t                     busGetServiceVersion(void) override;
            bool                        busShutdownService(void) override;
            int32_t                     busStartLoginSession(const uint8_t& depth, const std::string & remoteAddr, const std::string & connType) override;
            int32_t                     busStartUserSession(const int32_t & oldDisplay, const std::string & userName, const std::string & remoteAddr, const std::string & connType) override;
            std::string                 busCreateAuthFile(const int32_t & display) override;
            std::string                 busEncryptionInfo(const int32_t & display) override;
            bool                        busShutdownDisplay(const int32_t & display) override;
            bool                        busShutdownConnector(const int32_t & display) override;
            bool                        busConnectorTerminated(const int32_t & display) override;
            bool                        busConnectorSwitched(const int32_t & oldDisplay, const int32_t & newDisplay) override;
	    bool			busConnectorAlive(const int32_t & display) override;
            bool                        busIdleTimeoutAction(const int32_t& display) override;
	    bool			busSetLoginsDisable(const bool & action) override;
            void                        busSetDebugLevel(const std::string & level) override;
            void                        busSetChannelDebug(const int32_t& display, const uint8_t& channel, const bool& debug) override;
            void                        busSetConnectorDebugLevel(const int32_t& display, const std::string& level) override;
            bool                        busSetEncryptionInfo(const int32_t & display, const std::string & info) override;
	    bool			busSetSessionDurationSec(const int32_t & display, const uint32_t & duration) override;
	    bool			busSetSessionPolicy(const int32_t& display, const std::string& policy) override;
            bool                        busSetSessionEnvironments(const int32_t& display, const std::map<std::string, std::string>& map) override;
            bool                        busSetSessionOptions(const int32_t& display, const std::map<std::string, std::string>& map) override;
            bool                        busSetSessionKeyboardLayouts(const int32_t& display, const std::vector<std::string>& layouts) override;
            bool                        busSendMessage(const int32_t& display, const std::string& message) override;
            bool                        busSendNotify(const int32_t& display, const std::string& summary, const std::string& body, const uint8_t& icontype, const uint8_t& urgency) override;
	    bool			busDisplayResized(const int32_t& display, const uint16_t& width, const uint16_t& height) override;
            bool                        busCreateChannel(const int32_t& display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode) override;
            bool                        busDestroyChannel(const int32_t& display, const uint8_t& channel) override;
            bool                        busTransferFilesRequest(const int32_t& display, const std::vector<sdbus::Struct<std::string, uint32_t>>& files) override;
            bool                        busTransferFileStarted(const int32_t& display, const std::string& tmpfile, const uint32_t& filesz, const std::string& dstfile) override;

            bool                        helperIdleTimeoutAction(const int32_t & display) override;
            bool                        helperWidgetStartedAction(const int32_t & display) override;
            int32_t                     helperGetIdleTimeoutSec(const int32_t & display) override;
            std::vector<std::string>    helperGetUsersList(const int32_t & display) override;
            bool                        helperIsAutoComplete(const int32_t & display) override;
            std::string                 helperGetTitle(const int32_t & display) override;
            std::string                 helperGetDateFormat(const int32_t & display) override;
            bool                        helperSetSessionLoginPassword(const int32_t& display, const std::string& login, const std::string& password, const bool& action) override;

            bool                        busSetAuthenticateInfo(const int32_t & display, const std::string & login, const std::string & password) override;
            std::vector<xvfb2tuple>     busGetSessions(void) override;

            bool                        busRenderRect(const int32_t& display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>& rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color, const bool& fill) override;
            bool                        busRenderText(const int32_t& display, const std::string& text, const sdbus::Struct<int16_t, int16_t>& pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color) override;
            bool                        busRenderClear(const int32_t& display) override;

#ifdef LTSM_CHANNELS
            void                        startSessionChannels(int display);
            bool                        startPrinterListener(int display, const XvfbSession &, const std::string & clientUrl);
            bool                        startPulseAudioListener(int display, const XvfbSession &, const std::string & clientUrl);
            bool                        startPcscdListener(int display, const XvfbSession &, const std::string & clientUrl);
            bool                        startSaneListener(int display, const XvfbSession &, const std::string & clientUrl);
#endif
        };

        class Service : public ApplicationJsonConfig
        {
            static std::unique_ptr<Object> objAdaptor;
            static std::atomic<bool>       isRunning;

            std::unique_ptr<Tools::BaseTimer> timerInotifyWatchConfig;
            bool                           isBackground = false;

        protected:
            bool                        createXauthDir(void);
            bool                        inotifyWatchConfigStart(void);

        public:
            Service(int argc, const char** argv);

            int                         start(void);
            static void                 signalHandler(int);
        };
    }
}

#endif // _LTSM_SERVICE_
