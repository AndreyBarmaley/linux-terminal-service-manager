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

#ifndef _LTSM_SERVICE_
#define _LTSM_SERVICE_

#include <chrono>
#include <future>
#include <stdexcept>
#include <functional>
#include <filesystem>
#include <string_view>
#include <forward_list>

#include <security/pam_appl.h>
#include <security/pam_misc.h>

#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_service_adaptor.h"
#include "ltsm_json_wrapper.h"

namespace LTSM
{
    struct service_error : public std::runtime_error
    {
        explicit service_error(std::string_view what) : std::runtime_error(what.data()) {}
    };

    /// PamService
    class PamService
    {
    protected:
        std::string service;
        pam_handle_t* pamh = nullptr;
        int status = PAM_SUCCESS;

        virtual struct pam_conv* pamConv(void) = 0;

    public:
        PamService(const std::string & name) : service(name) {}

        virtual ~PamService();

        bool pamStart(const std::string & username);
        void setItem(int type, const std::string & item);

        std::string error(void) const;
        pam_handle_t* get(void);
    };

    /// PamAuthenticate
    class PamAuthenticate : public PamService
    {
        static int pam_conv_func(int num_msg, const struct pam_message** msg, struct pam_response** resp, void* appdata);

        std::string login;
        std::string password;
        struct pam_conv pamc { pam_conv_func, this };
        bool authenticateSuccess = false;

    protected:
        struct pam_conv* pamConv(void) override;
        virtual char* onPamPrompt(int, const char*) const;

    public:
        PamAuthenticate(const std::string & service, const std::string & user, const std::string & pass)
            : PamService(service), login(user), password(pass) {}

        bool authenticate(void);

        bool isAuthenticated(void) const;
        bool isLogin(std::string_view name) const;
    };

    /// PamSession
    class PamSession : public PamAuthenticate
    {
        bool sessionOpenned = false;

    protected:

    public:
        PamSession(const std::string & service, const std::string & user, const std::string & pass) : PamAuthenticate(service,
                    user, pass) {}

        ~PamSession();

        enum Cred { Establish = PAM_ESTABLISH_CRED, Refresh = PAM_REFRESH_CRED, Reinit = PAM_REINITIALIZE_CRED };

        bool validateAccount(void);
        bool openSession(void);
        bool refreshCreds(void);
        bool setCreds(const Cred &);

        std::forward_list<std::string> getEnvList(void);
    };

    /// Manager
    namespace Manager
    {
        class Object;
    }

    enum class XvfbMode { SessionLogin, SessionOnline, SessionSleep, SessionShutdown };
    enum class SessionPolicy { AuthLock, AuthTake, AuthShare };

    /// Flags
    namespace Flags
    {
        enum AllowChannel : size_t
        {
            TransferFiles = (1 << 1),
            RedirectPrinter = (1 << 2),
            RedirectAudio = (1 << 3),
            RedirectScanner = (1 << 4),
            RedirectPcsc = (1 << 5),
            RemoteFilesUse = (1 << 6)
        };

        enum SessionStatus : size_t
        {
            CheckConnection = (1 << 24)
        };
    }

    SessionPolicy sessionPolicy(const std::string &);

    /// XvfbSession
    struct XvfbSession
    {
        std::unordered_map<std::string, std::string> environments;
        std::unordered_map<std::string, std::string> options;

        std::filesystem::path xauthfile;

        UserInfoPtr userInfo;
        GroupInfoPtr groupInfo;

        std::string displayAddr;
        std::string remoteAddr;
        std::string conntype;
        std::string encryption;
        std::string layout;
        std::vector<uint8_t> mcookie;

        std::chrono::system_clock::time_point tpstart;

        int displayNum = -1;

        int pid1 = 0; // xvfb pid
        int pid2 = 0; // session pid
        int connectorId = 0; // connector pid

        std::atomic<size_t> durationLimit{0};
        std::atomic<size_t> statusFlags{0};
        int loginFailures = 0;

        uint16_t width = 0;
        uint16_t height = 0;
        uint8_t depth = 0;

        std::shared_future<int> idleActionRunning;
        std::unique_ptr<PamSession> pam;

        std::atomic<XvfbMode> mode{XvfbMode::SessionLogin};
        SessionPolicy policy = SessionPolicy::AuthTake;

        bool checkStatus(size_t st) const
        {
            return statusFlags & st;
        }

        void setStatus(size_t st)
        {
            statusFlags |= st;
        }

        void resetStatus(size_t st)
        {
            statusFlags &= ~st;
        }

        std::chrono::seconds aliveSec(void) const
        {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - tpstart);
        }

        XvfbSession() = default;
        ~XvfbSession();

        std::string toJsonString(void) const;
    };

    typedef std::shared_ptr<XvfbSession> XvfbSessionPtr;
    typedef std::pair<int, std::vector<uint8_t>> StatusStdout;

    typedef std::pair<pid_t, std::shared_future<int>> PidStatus;
    typedef std::pair<pid_t, std::future<StatusStdout>> PidStatusStdout;

    class XvfbSessions
    {
    protected:
        std::vector<XvfbSessionPtr> sessions;
        mutable std::mutex lockSessions;

    public:
        XvfbSessions(size_t);
        virtual ~XvfbSessions() = default;

        XvfbSessionPtr findDisplaySession(int display);
        XvfbSessionPtr findUserSession(const std::string & username);
        XvfbSessionPtr registryNewSession(int min, int max);
        void removeDisplaySession(int display);
        std::forward_list<XvfbSessionPtr> findTimepointLimitSessions(void);
        std::forward_list<XvfbSessionPtr> getOnlineSessions(void);

        std::string toJsonString(void) const;
    };

    using FileNameSize = sdbus::Struct<std::string, uint32_t>;

    namespace Manager
    {
        std::forward_list<std::string> getSessionDBusAddresses(const UserInfo &, int displayNum);
        void redirectStdoutStderrTo(bool out, bool err, const std::filesystem::path &);
        void closefds(std::initializer_list<int> exclude);
        bool checkFileReadable(const std::filesystem::path &);
        void setFileOwner(const std::filesystem::path & file, uid_t uid, gid_t gid);
        bool runSystemScript(XvfbSessionPtr, const std::string & cmd);
        bool switchToUser(const UserInfo &);
        std::string quotedString(std::string_view);

        class Object : public sdbus::AdaptorInterfaces<Service_adaptor>, public XvfbSessions
        {
            std::forward_list<PidStatus> childsRunning;
            std::mutex lockRunning;

            std::forward_list<std::string> allowTransfer;
            std::mutex lockTransfer;

            std::unique_ptr<Tools::BaseTimer> timer1, timer2, timer3;

            const Application* _app = nullptr;
            const JsonObject* _config = nullptr;
            std::atomic<bool> loginsDisable = false;

            pid_t runSessionCommandSafe(XvfbSessionPtr, const std::filesystem::path &,
                    std::list<std::string>);
            void waitPidBackgroundSafe(pid_t pid);

            bool sessionRunZenity(XvfbSessionPtr, std::initializer_list<std::string>);
            void sessionRunSetxkbmapLayout(XvfbSessionPtr);

            static void transferFileStartBackground(Object* owner, XvfbSessionPtr,
                    std::string tmpfile, std::string dstfile, uint32_t filesz);
            static void transferFilesRequestCommunication(Object* owner, XvfbSessionPtr,
                    std::filesystem::path zenity, std::vector<FileNameSize> files,
                    std::function<void(int, const std::vector<FileNameSize> &)> emitTransferReject,
                    std::shared_future<int>);
        protected:
            void closeSystemSession(XvfbSessionPtr);
            std::filesystem::path createXauthFile(int display, const std::vector<uint8_t> & mcookie);
            bool createSessionConnInfo(XvfbSessionPtr, bool destroy = false);
            XvfbSessionPtr runXvfbDisplayNewSession(uint8_t depth, uint16_t width, uint16_t height,
                    UserInfoPtr userInfo);
            int runUserSession(XvfbSessionPtr, const std::filesystem::path &, PamSession*);
            void runSessionScript(XvfbSessionPtr, const std::string & cmd);
            bool waitXvfbStarting(int display, const std::vector<uint8_t> &, uint32_t waitms) const;
            bool checkXvfbSocket(int display) const;
            void removeXvfbSocket(int display) const;
            bool displayShutdown(XvfbSessionPtr, bool emitSignal);
            bool pamAuthenticate(XvfbSessionPtr, const std::string & login, const std::string & password,
                    bool token);

            std::forward_list<std::string> getAllowLogins(void) const;

            void sessionsTimeLimitAction(void);
            void sessionsEndedAction(void);
            void sessionsCheckAliveAction(void);

            void childEndedEvent(void);

        public:
            Object(sdbus::IConnection &, const JsonObject &, size_t displays, const Application &);
            ~Object();

            void shutdownService(void);
            void configReloadedEvent(void);

        private: /* virtual dbus methods */
            int32_t busGetServiceVersion(void) override;
            void busShutdownService(void) override;
            int32_t busStartLoginSession(const int32_t & connectorId, const uint8_t & depth,
                    const std::string & remoteAddr, const std::string & connType) override;
            int32_t busStartUserSession(const int32_t & oldDisplay, const int32_t & connectorId,
                    const std::string & userName, const std::string & remoteAddr, const std::string & connType) override;
            std::string busCreateAuthFile(const int32_t & display) override;
            std::string busEncryptionInfo(const int32_t & display) override;
            bool busShutdownDisplay(const int32_t & display) override;
            bool busShutdownConnector(const int32_t & display) override;
            bool busConnectorTerminated(const int32_t & display, const int32_t & connectorId) override;
            bool busConnectorAlive(const int32_t & display) override;
            bool busIdleTimeoutAction(const int32_t & display) override;
            bool busSetLoginsDisable(const bool & action) override;
            void busSetDebugLevel(const std::string & level) override;
            void busSetChannelDebug(const int32_t & display, const uint8_t & channel,
                    const bool & debug) override;
            void busSetConnectorDebugLevel(const int32_t & display, const std::string & level) override;
            bool busSetEncryptionInfo(const int32_t & display, const std::string & info) override;
            bool busSetSessionDurationSec(const int32_t & display, const uint32_t & duration) override;
            bool busSetSessionPolicy(const int32_t & display, const std::string & policy) override;
            bool busSetSessionEnvironments(const int32_t & display,
                    const std::map<std::string, std::string> & map) override;
            bool busSetSessionOptions(const int32_t & display,
                    const std::map<std::string, std::string> & map) override;
            bool busSetSessionKeyboardLayouts(const int32_t & display,
                    const std::vector<std::string> & layouts) override;
            bool busSendMessage(const int32_t & display, const std::string & message) override;
            bool busSendNotify(const int32_t & display, const std::string & summary,
                    const std::string & body, const uint8_t & icontype, const uint8_t & urgency) override;
            bool busDisplayResized(const int32_t & display, const uint16_t & width, const uint16_t & height) override;
            bool busCreateChannel(const int32_t & display, const std::string & client,
                    const std::string & cmode, const std::string & server, const std::string & smode, const std::string & speed) override;
            bool busDestroyChannel(const int32_t & display, const uint8_t & channel) override;
            bool busTransferFilesRequest(const int32_t & display,
                    const std::vector<FileNameSize> & files) override;
            bool busTransferFileStarted(const int32_t & display, const std::string & tmpfile,
                    const uint32_t & filesz, const std::string & dstfile) override;

            bool helperWidgetStartedAction(const int32_t & display) override;
            std::vector<std::string> helperGetUsersList(const int32_t & display) override;
            bool helperIsAutoComplete(const int32_t & display) override;
            std::string helperGetTitle(const int32_t & display) override;
            std::string helperGetDateFormat(const int32_t & display) override;
            bool helperSetSessionLoginPassword(const int32_t & display, const std::string & login,
                    const std::string & password, const bool & action) override;

            bool busSetAuthenticateLoginPass(const int32_t & display, const std::string & login,
                    const std::string & password) override;
            bool busSetAuthenticateToken(const int32_t & display, const std::string & login) override;

            std::string busGetSessionJson(const int32_t & display) override;
            std::string busGetSessionsJson(void) override;

            bool busRenderRect(const int32_t & display,
                    const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect,
                    const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill) override;
            bool busRenderText(const int32_t & display, const std::string & text,
                    const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color) override;
            bool busRenderClear(const int32_t & display) override;

            void startSessionChannels(XvfbSessionPtr);
            void stopSessionChannels(XvfbSessionPtr);

            void startLoginChannels(XvfbSessionPtr);
            void stopLoginChannels(XvfbSessionPtr);

            bool startPrinterListener(XvfbSessionPtr, const std::string & clientUrl);
            bool startAudioListener(XvfbSessionPtr, const std::string & clientUrl);
            bool startFuseListener(XvfbSessionPtr, const std::string & clientUrl);
            bool startPcscListener(XvfbSessionPtr, const std::string & clientUrl);
            bool startPkcs11Listener(XvfbSessionPtr, const std::string & clientUrl);
            bool startSaneListener(XvfbSessionPtr, const std::string & clientUrl);

            void stopAudioListener(XvfbSessionPtr, const std::string & clientUrl);
            void stopFuseListener(XvfbSessionPtr, const std::string & clientUrl);
            void stopPcscListener(XvfbSessionPtr, const std::string & clientUrl);
            void stopPkcs11Listener(XvfbSessionPtr, const std::string & clientUrl);
        };

        class Service : public ApplicationJsonConfig
        {
            std::unique_ptr<Tools::BaseTimer> timerInotifyWatchConfig;
            bool isBackground = false;

        protected:
            bool createXauthDir(void);
            bool inotifyWatchConfigStart(void);

        public:
            Service(int argc, const char** argv);

            int start(void);
            static void signalHandler(int);
        };
    }
}

#endif // _LTSM_SERVICE_
