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

#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mount.h>

#include <cctype>
#include <chrono>
#include <atomic>
#include <thread>
#include <cstring>
#include <numeric>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <unordered_set>

#ifdef LTSM_WITH_SYSTEMD
#include <systemd/sd-login.h>
#include <systemd/sd-daemon.h>
#endif

#include "ltsm_fuse.h"
#include "ltsm_zlib.h"
#include "ltsm_pcsc.h"
#include "ltsm_audio.h"
#include "ltsm_tools.h"
#include "ltsm_pkcs11.h"
#include "ltsm_global.h"
#include "ltsm_sockets.h"
#include "ltsm_service.h"
#include "ltsm_channels.h"
#include "ltsm_byte_stream.h"
#include "ltsm_sdbus_proxy.h"
#include "ltsm_xcb_wrapper.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM::DisplaySession {
    // SessionAudio
    class SessionAudio : public SDBus::SessionProxy {
      public:
        SessionAudio(const std::string & dbusAddress)
            : SDBus::SessionProxy(dbusAddress, dbus_session_audio_name, dbus_session_audio_path, dbus_session_audio_ifce) {}
      
        int32_t getVersion(void) const {
            return CallProxyMethod<int32_t>("getVersion");
        }

        bool connectChannel(const std::string & socketPath) const {
            return CallProxyMethod<bool>("connectChannel", socketPath);
        }

        void disconnectChannel(const std::string & socketPath) const {
            CallProxyMethodNoResult("disconnectChannel", socketPath);
        }
    };

    // SessionPcsc
    class SessionPcsc : public SDBus::SessionProxy {
      public:
        SessionPcsc(const std::string & dbusAddress)
            : SDBus::SessionProxy(dbusAddress, dbus_session_pcsc_name, dbus_session_pcsc_path, dbus_session_pcsc_ifce) {}
      
        int32_t getVersion(void) const {
            return CallProxyMethod<int32_t>("getVersion");
        }

        bool connectChannel(const std::string & socketPath) const {
            return CallProxyMethod<bool>("connectChannel", socketPath);
        }

        void disconnectChannel(const std::string & socketPath) const {
            CallProxyMethodNoResult("disconnectChannel", socketPath);
        }
    };

    // SessionFuse
    class SessionFuse : public SDBus::SessionProxy {
      public:
        SessionFuse(const std::string & dbusAddress)
            : SDBus::SessionProxy(dbusAddress, dbus_session_fuse_name, dbus_session_fuse_path, dbus_session_fuse_ifce) {}

        int32_t getVersion(void) const {
            return CallProxyMethod<int32_t>("getVersion");
        }
            
        bool mountPoint(const std::string& localPoint, const std::string& remotePoint, const std::string& fuseSocket) const {
            return CallProxyMethod<bool>("mountPoint", localPoint, remotePoint, fuseSocket);
        }
            
        void umountPoint(const std::string& localPoint) const {
            CallProxyMethodNoResult("umountPoint", localPoint);
        }
    };
}

namespace LTSM::Manager {
    //
    void runSystemScript(XvfbSessionPtr, const std::string & cmd);
    void runSessionScript(XvfbSessionPtr, const std::string & cmd);

    bool switchToUser(const UserSession &);

    namespace ChildProcess {
        int pidNext = 0;

        void signalHandler(int sig) {
            if(sig == SIGTERM && 0 < pidNext) {
                kill(pidNext, SIGTERM);
            }
        }

        void pamOpenDisplaySession(XvfbSessionPtr sess, const ApplicationJsonConfig & json) {
            // child1 thread
            signal(SIGTERM, ChildProcess::signalHandler);

            const auto & userInfo = sess->userInfo;
            auto pam = std::make_unique<PamSession>(json.configGetString("pam:service"),
                            userInfo->user(), userInfo->password());

            if(! pam->pamStart(userInfo->user())) {
                return;
            }

            if(! userInfo->password().empty()) {
                if(! pam->authenticate()) {
                    return;
                }
            }

            if(! pam->validateAccount()) {
                return;
            }

            if(! std::filesystem::is_directory(userInfo->home())) {
                Application::warning("{}: HOME not found: `{}'", __FUNCTION__, userInfo->home());
            }

            if(0 != initgroups(userInfo->user().c_str(), userInfo->gid())) {
                Application::error("{}: {} failed, user: {}, gid: {}, error: {}",
                                   __FUNCTION__, "initgroups", userInfo->user(), userInfo->gid(), strerror(errno));
                return;
            }

            if(! pam->openSession()) {
                Application::error("{}: {}, display: {}, user: {}",
                                   __FUNCTION__, "PAM open session failed", sess->displayNum, userInfo->user());
                return;
            }

            Application::debug(DebugType::App, "{}: child mode, type: {}, pid: {}, uid: {}",
                               __FUNCTION__, "pam session", getpid(), getuid());

            if(pid_t pid = ForkMode::forkStart(); 0 != pid) {
                pidNext = pid;
                const bool notSysUser = std::string_view(ltsm_user_conn) != sess->userInfo->user();
                std::future<void> detach;

                // logon
                if(notSysUser && json.configHasKey("system:logon")) {
                    detach = std::async(std::launch::async, &runSystemScript, sess, json.configGetString("system:logon"));
                }

                // main2 thread
                ForkMode::waitPid(pid);
                // close pam session
                pam.reset();

                // logoff
                if(notSysUser && json.configHasKey("system:logoff")) {
                    runSystemScript(sess, json.configGetString("system:logoff"));
                }

                return;
            }

            // child2 thread
            Application::debug(DebugType::App, "{}: child mode, type: {}, pid: {}, uid: {}",
                               __FUNCTION__, "display session", getpid(), getuid());

            auto sessionBin = json.configGetString("starter:path", "/usr/libexec/ltsm/ltsm_session_display");

            if(std::filesystem::exists(sessionBin)) {
                if(switchToUser(*sess->userInfo)) {
                    // set environments
                    for(const auto & [key, val] : sess->getEnvironments(pam->getEnvList())) {
                        Application::debug(DebugType::App, "{}: setenv[ {} ] = `{}'", __FUNCTION__, key, val);
                        setenv(key.c_str(), val.c_str(), 1);
                    }

                    std::vector<const char*> argv;
                    argv.reserve(6);
                    argv.push_back(sessionBin.c_str());
                    argv.push_back("--display");
                    argv.push_back(sess->displayAddr.c_str());
                    argv.push_back("--xauth");
                    argv.push_back(sess->xauthfile.c_str());
                    argv.push_back(nullptr);

                    if(int res = execv(sessionBin.c_str(), (char* const*) argv.data()); res < 0) {
                        Application::error("{}: {} failed, error: {}, code: {}, path: `{}'",
                               __FUNCTION__, "execv", strerror(errno), errno, sessionBin);
                    }
                    // exit
                    exit(0);
                }
            } else {
                Application::error("{}: path not found: `{}'", __FUNCTION__, sessionBin);
            }
        }
    }

    SessionPolicy sessionPolicy(const std::string & name) {
        if(name == "authlock") {
            return SessionPolicy::AuthLock;
        }

        if(name == "authtake") {
            return SessionPolicy::AuthTake;
        }

        if(name == "authshare") {
            return SessionPolicy::AuthShare;
        }

        return SessionPolicy::AuthTake;
    }

    /* PamService */
    PamService::~PamService() {
        if(pamh) {
            pam_end(pamh, status);
        }
    }

    std::string PamService::error(void) const {
        return pamh ? std::string(pam_strerror(pamh, status)) : "unknown";
    }

    pam_handle_t* PamService::get(void) {
        return pamh;
    }

    void PamService::setItem(int type, const std::string & str) {
        if(pamh) {
            pam_set_item(pamh, type, str.c_str());
        }
    }

    bool PamService::pamStart(const std::string & username) {
        status = pam_start(service.c_str(), username.c_str(), pamConv(), & pamh);

        if(PAM_SUCCESS != status) {
            if(pamh) {
                Application::error("{}: {} failed, error: {}, code: {}",
                                   __FUNCTION__, "pam_start", pam_strerror(pamh, status), status);
            } else {
                Application::error("{}: {} failed", __FUNCTION__, "pam_start");
            }

            return false;
        }

        return true;
    }

    /* PamAuthenticate */
    int PamAuthenticate::pam_conv_func(int num_msg, const struct pam_message** msg, struct pam_response** resp,
                                       void* appdata) {
        if(! appdata) {
            Application::error("{}: pam error: {}", __FUNCTION__, "empty data");
            return PAM_CONV_ERR;
        }

        if(! msg || ! resp) {
            Application::error("{}: pam error: {}", __FUNCTION__, "empty params");
            return PAM_CONV_ERR;
        }

        if(! *resp) {
            *resp = (struct pam_response*) calloc(num_msg, sizeof(struct pam_response));

            if(! *resp) {
                Application::error("{}: pam error: {}", __FUNCTION__, "buf error");
                return PAM_BUF_ERR;
            }
        }

        auto pamAuthenticate = static_cast<const PamAuthenticate*>(appdata);

        for(int ii = 0 ; ii < num_msg; ++ii) {
            auto pm = msg[ii];
            auto pr = resp[ii];

            if(pr->resp) {
                free(pr->resp);
            }

            pr->resp = pamAuthenticate->onPamPrompt(pm->msg_style, pm->msg);
            pr->resp_retcode = PAM_SUCCESS;
        }

        return PAM_SUCCESS;
    }

    char* PamAuthenticate::onPamPrompt(int style, const char* msg) const {
        switch(style) {
            case PAM_ERROR_MSG:
                Application::info("{}: style: `{}', msg: `{}'", __FUNCTION__, "PAM_ERROR_MSG", msg);
                break;

            case PAM_TEXT_INFO:
                Application::info("{}: style: `{}', msg: `{}'", __FUNCTION__, "PAM_TEXT_INFO", msg);
                break;

            case PAM_PROMPT_ECHO_ON:
                Application::info("{}: style: `{}', msg: `{}'", __FUNCTION__, "PAM_PROMPT_ECHO_ON", msg);

                //if(0 == strncasecmp(msg, "login:", 6));
                return strdup(login.c_str());

                break;

            case PAM_PROMPT_ECHO_OFF:
                Application::info("{}: style: `{}', msg: `{}'", __FUNCTION__, "PAM_PROMPT_ECHO_OFF", msg);

                //if(0 == strncasecmp(msg, "password:", 9));
                return strdup(password.c_str());

                break;

            default:
                break;
        }

        return nullptr;
    }

    bool PamAuthenticate::isLogin(std::string_view name) const {
        return login == name;
    }

    struct pam_conv* PamAuthenticate::pamConv(void) {
        return & pamc;
    }

    bool PamAuthenticate::authenticate(void) {
        status = pam_authenticate(pamh, 0);

        if(PAM_SUCCESS != status) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               __FUNCTION__, "pam_authenticate", pam_strerror(pamh, status), status);
            return false;
        }

        return true;
    }

    /* PamSession */
    PamSession::~PamSession() {
        if(sessionOpenned) {
            pam_close_session(pamh, 0);
        }

        pam_setcred(pamh, PAM_DELETE_CRED);
    }

    bool PamSession::validateAccount(void) {
        status = pam_acct_mgmt(pamh, 0);

        if(status == PAM_NEW_AUTHTOK_REQD) {
            status = pam_chauthtok(pamh, PAM_CHANGE_EXPIRED_AUTHTOK);

            if(PAM_SUCCESS != status) {
                Application::error("{}: {} failed, error: {}, code: {}",
                                   __FUNCTION__, "pam_chauthtok", pam_strerror(pamh, status), status);
                return false;
            }
        } else if(PAM_SUCCESS != status) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               __FUNCTION__, "pam_acct_mgmt", pam_strerror(pamh, status), status);
            return false;
        }

        return true;
    }

    bool PamSession::refreshCreds(void) {
        status = pam_setcred(pamh, PAM_REFRESH_CRED);

        if(PAM_SUCCESS != status) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               __FUNCTION__, "pam_setcred", pam_strerror(pamh, status), status);
            return false;
        }

        return true;
    }

    bool PamSession::openSession(void) {
        status = pam_setcred(pamh, PAM_ESTABLISH_CRED);

        if(PAM_SUCCESS != status) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               __FUNCTION__, "pam_setcred", pam_strerror(pamh, status), status);
            return false;
        }

        status = pam_open_session(pamh, 0);

        if(PAM_SUCCESS != status) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               __FUNCTION__, "pam_open_session", pam_strerror(pamh, status), status);
            return false;
        }

        sessionOpenned = true;
        return true;
    }

    bool PamSession::setCreds(const Cred & cred) {
        // PAM_ESTABLISH_CRED, PAM_REFRESH_CRED, PAM_REINITIALIZE_CRED
        status = pam_setcred(pamh, cred);

        if(PAM_SUCCESS != status) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               __FUNCTION__, "pam_setcred", pam_strerror(pamh, status), status);
            return false;
        }

        return true;
    }

    EnvList PamSession::getEnvList(void) {
        EnvList list;

        if(auto envlist = pam_getenvlist(pamh)) {
            for(auto env = envlist; *env; ++env) {
                list.emplace_back(*env);
                free(*env);
            }

            free(envlist);
        }

        return list;
    }

    /* XvfbSession */
    XvfbSession::~XvfbSession() {
        if(0 < pid1) {
            // kill session
            Application::debug(DebugType::App, "{}: display: {}, pid: {}", "destroySession", displayNum, pid1);
            kill(pid1, SIGTERM);
        }

        try {
            // remove xautfile
            std::filesystem::remove(xauthfile);
        } catch(const std::filesystem::filesystem_error &) {
        }
    }

    std::filesystem::path XvfbSession::dbusSessionPath(void) const noexcept {
        if(0 < displayNum && userInfo) {
            // path generated from /etc/ltsm/xclients
            // format userRuntimeDir / ltsm / dbus_session_%{display}
            return userInfo->xdgRuntimeDir() / "ltsm" / fmt::format("dbus_session_{}", displayNum);
        }
        return {};
    }

    int32_t XvfbSession::dbusGetVersion(void) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                auto proxy = std::make_unique<DisplaySessionProxy>(dbusAddress, displayNum);
                return proxy->getVersion();
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
        return 0;
    }

    StatusStdout XvfbSession::dbusRunSessionZenity(const std::vector<std::string>& args) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                auto proxy = std::make_unique<DisplaySessionProxy>(dbusAddress, displayNum);
                return proxy->runSessionZenity(args);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
        return StatusStdout{0, {}};
    }

    int32_t XvfbSession::dbusRunSessionCommandAsync(const std::string& cmd, const std::vector<std::string>& args, const std::vector<std::string>& envs) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                auto proxy = std::make_unique<DisplaySessionProxy>(dbusAddress, displayNum);
                return proxy->runSessionCommandAsync(cmd, args, envs);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
        return -1;
    }

    void XvfbSession::dbusSetSessionKeyboardLayout(void) const noexcept {
        if(0 < displayNum && dbusAddress.size() && layout.size()) {
            try {
                auto proxy = std::make_unique<DisplaySessionProxy>(dbusAddress, displayNum);
                return proxy->setSessionKeyboardLayout(layout);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
    }

    void XvfbSession::dbusNotifyWarning(const std::string & summary, const std::string & body) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                auto proxy = std::make_unique<DisplaySessionProxy>(dbusAddress, displayNum);
                proxy->notifyWarning(summary, body);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
    }

    void XvfbSession::dbusNotifyError(const std::string & summary, const std::string & body) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                auto proxy = std::make_unique<DisplaySessionProxy>(dbusAddress, displayNum);
                proxy->notifyError(summary, body);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
    }

    void XvfbSession::dbusNotifyInfo(const std::string & summary, const std::string & body) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                auto proxy = std::make_unique<DisplaySessionProxy>(dbusAddress, displayNum);
                proxy->notifyInfo(summary, body);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
    }

    bool XvfbSession::dbusAudioChannelConnect(const std::string& socketPath) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                return DisplaySession::SessionAudio(dbusAddress).connectChannel(socketPath);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
        return false;
    }

    void XvfbSession::dbusAudioChannelDisconnect(const std::string& socketPath) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                DisplaySession::SessionAudio(dbusAddress).disconnectChannel(socketPath);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
    }

    bool XvfbSession::dbusPcscChannelConnect(const std::string& socketPath) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                return DisplaySession::SessionPcsc(dbusAddress).connectChannel(socketPath);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
        return false;
    }

    void XvfbSession::dbusPcscChannelDisconnect(const std::string& socketPath) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                DisplaySession::SessionPcsc(dbusAddress).disconnectChannel(socketPath);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
    }

    bool XvfbSession::dbusFuseMountPoint(const std::string& localPoint, const std::string& remotePoint, const std::string& fuseSocket) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                return DisplaySession::SessionFuse(dbusAddress).mountPoint(localPoint, remotePoint, fuseSocket);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
        return false;
    }

    void XvfbSession::dbusFuseUmountPoint(const std::string& localPoint) const noexcept {
        if(0 < displayNum && dbusAddress.size()) {
            try {
                DisplaySession::SessionFuse(dbusAddress).umountPoint(localPoint);
            } catch(const std::exception & err) {
                Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }
    }
    
    std::unordered_map<std::string, std::string> XvfbSession::getEnvironments(const EnvList & envs) const {
        auto res = environments;

        res.insert_or_assign("XAUTHORITY", xauthfile);
        res.insert_or_assign("DISPLAY", displayAddr);
        res.insert_or_assign("LTSM_REMOTEADDR", remoteAddr);
        res.insert_or_assign("LTSM_TYPECONN", conntype);
        res.insert_or_assign("XDG_RUNTIME_DIR", userInfo->xdgRuntimeDir().string());
        res.insert_or_assign("XDG_SESSION_TYPE", "x11");

        if(mode == SessionMode::Login) {
            res.insert_or_assign("LTSM_LOGIN_MODE", "OK");
        }

        for(const auto & env : envs) {
            if(auto pos = env.find("="); pos != std::string_view::npos) {
                auto it = env.begin() + pos;
                res.insert_or_assign(std::string{env.begin(), it}, std::string{std::next(it), env.end()});
            }
        }

        return res;
    }

    std::string XvfbSession::toJsonString(void) const {
        JsonObjectStream jos;
        jos.push("displaynum", displayNum);
        jos.push("pid1", pid1);
        jos.push("width", width);
        jos.push("height", height);
        jos.push("uid", userInfo->uid());
        jos.push("gid", userInfo->gid());
        jos.push("lifetime:limit", lifeTimeLimitSec.load());
        jos.push("online:limit", onlineTimeLimitSec.load());
        jos.push("offline:limit", offlineTimeLimitSec.load());
        jos.push("status:flags", statusFlags.load());
        jos.push("sesion:mode", static_cast<int>(mode.load()));
        jos.push("connect:policy", static_cast<int>(policy));
        jos.push("user", userInfo->user());
        jos.push("xauthfile", xauthfile);
        jos.push("remoteaddr", remoteAddr);
        jos.push("conntype", conntype);
        jos.push("encryption", encryption);
        jos.push("started:sec", sessionStartedSec().count());

        if(mode == SessionMode::Connected) {
            jos.push("onlined:sec", sessionOnlinedSec().count());
        } else if(mode == SessionMode::Disconnected) {
            jos.push("offlined:sec", sessionOfflinedSec().count());
        }

        jos.push("session:lifetime:timeout", static_cast<uint32_t>(lifeTimeLimitSec));
        jos.push("session:online:timeout", static_cast<uint32_t>(onlineTimeLimitSec));
        jos.push("session:offline:timeout", static_cast<uint32_t>(offlineTimeLimitSec));
        jos.push("session:idle:timeout", static_cast<uint32_t>(idleTimeLimitSec));
        jos.push("session:idle:disconnect", static_cast<bool>(idleDisconnect));

        return jos.flush();
    }

    /* XvfbSessions */
    XvfbSessions::XvfbSessions(size_t displays) {
        sessions.resize(displays);
    }

    std::forward_list<XvfbSessionPtr> XvfbSessions::findUserSessions(const std::string & username) const {
        std::forward_list<XvfbSessionPtr> res;

        std::scoped_lock guard{ lockSessions };
        std::ranges::copy_if(sessions, std::front_inserter(res), [&](const auto & ptr) {
            return ptr && ptr->mode != SessionMode::Shutdown && username == ptr->userInfo->user();
        });

        return res;
    }

    XvfbSessionPtr XvfbSessions::findUserSession(const std::string & username) const {
        std::scoped_lock guard{ lockSessions };
        auto it = std::ranges::find_if(sessions, [&](const auto & ptr) {
            return ptr &&
                   (ptr->mode == SessionMode::Started || ptr->mode == SessionMode::Connected || ptr->mode == SessionMode::Disconnected) &&
                   username == ptr->userInfo->user();
        });

        return it != sessions.end() ? *it : nullptr;
    }

    XvfbSessionPtr XvfbSessions::findPidSession(int pid) const {
        std::scoped_lock guard{ lockSessions };
        auto it = std::ranges::find_if(sessions, [&pid](const auto & ptr) {
            return ptr && ptr->pid1 == pid;
        });

        return it != sessions.end() ? *it : nullptr;
    }

    XvfbSessionPtr XvfbSessions::findDisplaySession(int screen) const {
        std::scoped_lock guard{ lockSessions };
        auto it = std::ranges::find_if(sessions, [&screen](const auto & ptr) {
            return ptr && ptr->displayNum == screen;
        });

        return it != sessions.end() ? *it : nullptr;
    }

    std::forward_list<XvfbSessionPtr> XvfbSessions::findTimepointLimitSessions(void) const {
        std::forward_list<XvfbSessionPtr> res;
        std::scoped_lock guard{ lockSessions };

        for(const auto & ptr : sessions) {
            if(ptr && (0 < ptr->lifeTimeLimitSec || 0 < ptr->onlineTimeLimitSec || 0 < ptr->offlineTimeLimitSec)) {
                res.push_front(ptr);
            }
        }

        return res;
    }

    std::forward_list<XvfbSessionPtr> XvfbSessions::getOnlineSessions(void) const {
        std::forward_list<XvfbSessionPtr> res;
        std::scoped_lock guard{ lockSessions };

        for(const auto & ptr : sessions) {
            if(ptr && ptr->mode == SessionMode::Connected) {
                res.push_front(ptr);
            }
        }

        return res;
    }

    void XvfbSessions::removeDisplaySession(int screen) {
        std::scoped_lock guard{ lockSessions };
        auto it = std::ranges::find_if(sessions, [&screen](auto & ptr) {
            return ptr && ptr->displayNum == screen;
        });

        if(it != sessions.end()) {
            (*it).reset();
        }
    }

    XvfbSessionPtr XvfbSessions::registryNewSession(int min, int max) {
        if(max < min) {
            std::swap(max, min);
        }

        std::scoped_lock guard{ lockSessions };
        auto freeDisplay = min;

        for(; freeDisplay <= max; ++freeDisplay) {
            if(std::ranges::none_of(sessions, [&](auto & ptr) { return ptr && ptr->displayNum == freeDisplay; })) {
                break;
            }
        }

        if(freeDisplay <= max) {
            auto it = std::ranges::find_if(sessions, [](auto & ptr) {
                return ! ptr;
            });

            if(it != sessions.end()) {
                (*it) = std::make_shared<XvfbSession>();
                (*it)->displayNum = freeDisplay;
            }

            return *it;
        }

        return nullptr;
    }

    std::string XvfbSessions::toJsonString(void) const {
        JsonArrayStream jas;
        std::scoped_lock guard{ lockSessions };

        for(const auto & ptr : sessions) {
            if(ptr) {
                jas.push(ptr->toJsonString());
            }
        }

        return jas.flush();
    }

    void runSystemScript(XvfbSessionPtr xvfb, const std::string & cmd) {
        if(cmd.empty()) {
            return;
        }

        std::error_code err;
        auto path = cmd.substr(0, cmd.find(0x20));

        if(! std::filesystem::exists(path, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                                __FUNCTION__, "exists", err.value(), err.message());
            return;
        }

        auto str = Tools::replace(cmd, "%{display}", xvfb->displayNum);
        str = Tools::replace(str, "%{user}", xvfb->userInfo->user());

        int ret = std::system(str.c_str());
        Application::debug(DebugType::App, "{}: command: `{}', return code: {}, display: {}",
                               "runSystemScript", str, ret, xvfb->displayNum);
    }

    void runSessionScript(XvfbSessionPtr xvfb, const std::string & str) {
        if(str.empty()) {
            return;
        }

        auto args = Tools::split(Tools::replace(
                                         Tools::replace(str, "%{display}", xvfb->displayNum), "%{user}", xvfb->userInfo->user()), 0x20);
        assertm(! args.empty(), "empty args list");
        xvfb->dbusRunSessionCommandAsync(args.front(), { std::next(args.begin()), args.end() }, {});
    }

    bool switchToUser(const UserSession & userInfo) {
        Application::debug(DebugType::App, "{}: pid: {}, uid: {}, gid: {}, home: `{}', shell: `{}'",
                           __FUNCTION__, getpid(), userInfo.uid(), userInfo.gid(), userInfo.home(), userInfo.shell());

        auto xdgRuntimeDir = userInfo.xdgRuntimeDir();
        auto xdgLtsm = xdgRuntimeDir / "ltsm";
        std::error_code err;

        if(! std::filesystem::is_directory(xdgLtsm, err)) {
            std::filesystem::create_directories(xdgLtsm, err);
        }

        if(! std::filesystem::is_directory(xdgLtsm, err)) {
            Application::error("{}: {} failed, path: `{}'", __FUNCTION__, "mkdir", xdgLtsm);
            return false;
        }

        // fix owner, perms
        Tools::setFileOwner(xdgLtsm, userInfo.uid(), userInfo.gid(), 0750);
        Tools::setFileOwner(xdgRuntimeDir, userInfo.uid(), userInfo.gid(), 0700);

        // set groups
        auto gids = userInfo.groups();

        if(! gids.empty()) {
            setgroups(gids.size(), gids.data());
        }

        if(0 != setgid(userInfo.gid())) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "setgid", strerror(errno), errno);
            return false;
        }

        if(0 != setuid(userInfo.uid())) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "setuid", strerror(errno), errno);
            return false;
        }

        if(0 != chdir(userInfo.home().c_str())) {
            Application::warning("{}: {} failed, error: {}, code: {}, path: `{}'",
                                 __FUNCTION__, "chdir", strerror(errno), errno, userInfo.home());
        }

        setenv("USER", userInfo.user().c_str(), 1);
        setenv("LOGNAME", userInfo.user().c_str(), 1);
        setenv("HOME", userInfo.home().c_str(), 1);
        setenv("SHELL", userInfo.shell().c_str(), 1);
        setenv("TERM", "linux", 1);

        if(Application::isDebugLevel(DebugLevel::Debug)) {
            auto cwd = std::filesystem::current_path();
            auto sgroups = Tools::join(gids, ",");
            Application::debug(DebugType::App, "{}: groups: ({}), current dir: `{}'", __FUNCTION__, sgroups, cwd);
        }

        return true;
    }

#ifdef LTSM_WITH_AUDIT
    void AuditService::auditServiceStart(void) const {
        auditUserMessage(AUDIT_SERVICE_START, "service started", nullptr, nullptr, nullptr, 1);
    }

    void AuditService::auditServiceStop(void) const {
        auditUserMessage(AUDIT_SERVICE_STOP, "service stopped", nullptr, nullptr, nullptr, 1);
    }

    void AuditService::auditSessionStart(bool success) const {
        auditUserMessage(AUDIT_USER_START, "session started", nullptr, nullptr, nullptr, static_cast<int>(success));
    }

    void AuditService::auditSessionStop(bool success) const {
        auditUserMessage(AUDIT_USER_END, "session stopped", nullptr, nullptr, nullptr, static_cast<int>(success));
    }

    void AuditService::auditUserConnected(const std::string & tty) const {
        auditUserMessage(AUDIT_USER_LOGIN, "user connected", nullptr, nullptr, tty.c_str(), 1);
    }

    void AuditService::auditUserDisconnected(const std::string & tty) const {
        auditUserMessage(AUDIT_USER_LOGOUT, "user disconnected", nullptr, nullptr, tty.c_str(), 1);
    }
#endif

    DisplaySessionProxy::DisplaySessionProxy(const std::string & addr, int display) :
#ifdef SDBUS_2_0_API
        ProxyInterfaces(sdbus::createSessionBusConnectionWithAddress(addr), sdbus::ServiceName {LTSM::dbus_session_display_name}, sdbus::ObjectPath {dbus_session_display_path}),
#else
        ProxyInterfaces(sdbus::createSessionBusConnectionWithAddress(addr), LTSM::dbus_session_display_name, dbus_session_display_path),
#endif
        displayNum(display) {
        registerProxy();
        Application::debug(DebugType::App, "{}: create for display: {}", __FUNCTION__, displayNum);
    }

    DisplaySessionProxy::~DisplaySessionProxy() {
        unregisterProxy();
    }

    void DisplaySessionProxy::onRunSessionCommandAsyncComplete(const int32_t & pid, const bool& success, const int32_t & wstatus, const std::vector<uint8_t> & stdout) {
    }

    /* DBusAdaptor */
    DBusAdaptor::DBusAdaptor(asio::io_context& ctx, DBusConnectionPtr conn, const std::filesystem::path & confile)
        : ApplicationJsonConfig("ltsm_service", confile)
#ifdef SDBUS_2_0_API
        , AdaptorInterfaces(*conn, sdbus::ObjectPath {LTSM::dbus_manager_service_path})
#else
        , AdaptorInterfaces(*conn, LTSM::dbus_manager_service_path)
#endif
        , XvfbSessions(300), ioc_{ctx}, signals_{ioc_},
            work_guard_{asio::make_work_guard(ioc_)}, childs_guard_{asio::make_strand(ioc_)},
            timer_limit_{ioc_}, timer_ended_{childs_guard_}, timer_alive_{ioc_}, dbus_conn_{std::move(conn)} {
        //
        checkConfigPathes();
        createRuntimeDir();

        // set pool sessions
        sessions.resize(std::abs(configGetInteger("limit:sessions", 20)));

        //
        saneRuntimeFmt = std::filesystem::path(ltsmRuntimeDir / "sane" / "%{user}").string();
        audioRuntimeFmt = std::filesystem::path(ltsmRuntimeDir / "audio" / "%{user}").string();
        pcscRuntimeFmt = std::filesystem::path(ltsmRuntimeDir / "pcsc" / "%{user}").string();
        fuseRuntimeFmt = std::filesystem::path(ltsmRuntimeDir / "fuse" / "%{user}").string();
        cupsRuntimeFmt = std::filesystem::path(ltsmRuntimeDir / "cups" / "printer_%{user}").string();
        // sync with ltsm_pkcs11_session
        pkcs11RuntimeFmt = std::filesystem::path(ltsmRuntimeDir / "pkcs11" / "%{display}").string();

        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        signals_.async_wait([this](const system::error_code& ec, int signal)
        {
            // skip canceled
            if(ec != asio::error::operation_aborted && (signal == SIGTERM || signal == SIGINT))
            {
                this->stop();
            }
        });

        // check sessions timepoint limit
        timer_limit_.expires_after(dur_limit_);
        timer_limit_.async_wait(std::bind(&DBusAdaptor::timerSessionsTimeLimitAction, this, std::placeholders::_1));
        // check sessions killed
        timer_ended_.expires_after(dur_ended_);
        timer_ended_.async_wait(std::bind(&DBusAdaptor::timerSessionsEndedAction, this, std::placeholders::_1));
        // check sessions alive
        timer_alive_.expires_after(dur_alive_);
        timer_alive_.async_wait(std::bind(&DBusAdaptor::timerSessionsCheckConnectedAction, this, std::placeholders::_1));

        inotifyWatchStart(ctx);

#ifdef LTSM_WITH_AUDIT
        auditLog = std::make_unique<AuditService>();
        auditLog->auditServiceStart();
#endif

        // registry
        registerAdaptor();
    }

    DBusAdaptor::~DBusAdaptor() {
#ifdef LTSM_WITH_AUDIT
        auditLog->auditServiceStop();
#endif
        unregisterAdaptor();
        stop();
    }

    void DBusAdaptor::stop(void) {

        // terminate connectors
        for(const auto & ptr : sessions) {
            if(ptr) {
                displayShutdown(ptr, true);
            }
        }

        auto isValidSession = [](const XvfbSessionPtr & ptr) {
            return !! ptr;
        };

        // wait sessions
        while(auto sessionsAlive = std::ranges::count_if(sessions, isValidSession)) {
            Application::debug(DebugType::App, "{}: wait sessions: {}", __FUNCTION__, sessionsAlive);
            std::this_thread::sleep_for(50ms);
        }

        Application::notice("{}: {}, pid: {}", __FUNCTION__, "complete", getpid());

        dbus_conn_->leaveEventLoop();

        signals_.cancel();
        timer_limit_.cancel();
        timer_ended_.cancel();
        timer_alive_.cancel();

        for(const auto & pid: childs_) {
            kill(pid, SIGTERM);
        }
        for(const auto & pid: childs_) {
            waitpid(pid, nullptr, 0);
        }
        childs_.clear();

        work_guard_.reset();
    }

    void DBusAdaptor::createRuntimeDir(void) const {
        // create
        if(! std::filesystem::is_directory(ltsmRuntimeDir)) {
            std::error_code fserr;
            std::filesystem::create_directories(ltsmRuntimeDir, fserr);
        }

        if(std::filesystem::is_directory(ltsmRuntimeDir)) {
            // find group id
            gid_t setgid = Tools::getGroupGid(ltsm_group_auth);
            // fix mode 0755
            Tools::setFileOwner(ltsmRuntimeDir, 0, setgid, 0755);
        }
    }

    void DBusAdaptor::checkConfigPathes(void) const {
        // check present executable files
        for(const auto & key : config().keys()) {
            // only for path
            if(startsWith(key, "#") || ! endsWith(key, ":path")) {
                continue;
            }

            // skip comment
            if(0 == std::ispunct(key.front())) {
                continue;
            }

            if(auto value = configGetString(key);
               ! std::filesystem::exists(value)) {
                Application::warning("{}: path not found: `{}'", "CheckProgram", value);
            }
        }
    }

    void DBusAdaptor::configReloadedEvent(void) {
        checkConfigPathes();

        // check pool sessions
        size_t poolsz = std::abs(configGetInteger("limit:sessions", 20));

        if(poolsz > sessions.size()) {
            std::scoped_lock guard{ lockSessions };
            sessions.resize(poolsz);
        }

        Application::notice("{}: success", __FUNCTION__);
    }

    void DBusAdaptor::timerSessionsTimeLimitAction(const system::error_code& ec) {
        if(ec) {
            return;
        }

        for(auto & ptr : findTimepointLimitSessions()) {
            uint32_t lastSecStarted = UINT32_MAX;
            uint32_t lastSecOnlined = UINT32_MAX;

            // check started timepoint
            if(0 < ptr->lifeTimeLimitSec) {
                auto startedSec = ptr->sessionStartedSec();

                if(startedSec.count() > ptr->lifeTimeLimitSec) {
                    Application::notice("{}: {} limit, display: {}, limit: {}sec, session alive: {}sec",
                                        __FUNCTION__, "started", ptr->displayNum, static_cast<uint32_t>(ptr->lifeTimeLimitSec), startedSec.count());
                    displayShutdownAsync(std::move(ptr), true);
                    continue;
                }

                if(ptr->mode != SessionMode::Login) {
                    lastSecStarted = ptr->lifeTimeLimitSec - startedSec.count();
                }
            }

            // check online timepoint
            if(ptr->mode == SessionMode::Connected && 0 < ptr->onlineTimeLimitSec) {
                auto onlinedSec = ptr->sessionOnlinedSec();

                if(onlinedSec.count() > ptr->onlineTimeLimitSec) {
                    Application::notice("{}: {} limit, display: {}, limit: {}sec, session alive: {}sec",
                                        __FUNCTION__, "online", ptr->displayNum, static_cast<uint32_t>(ptr->onlineTimeLimitSec), onlinedSec.count());
                    emitShutdownConnector(ptr->displayNum);
                    continue;
                }

                lastSecOnlined = ptr->onlineTimeLimitSec - onlinedSec.count();
            }

            // check offline timepoint
            if(ptr->mode == SessionMode::Disconnected && 0 < ptr->offlineTimeLimitSec) {
                auto offlinedSec = ptr->sessionOfflinedSec();

                if(offlinedSec.count() > ptr->offlineTimeLimitSec) {
                    Application::notice("{}: {} limit, display: {}, limit: {}sec, session alive: {}sec",
                                        __FUNCTION__, "offline", ptr->displayNum, static_cast<uint32_t>(ptr->offlineTimeLimitSec), offlinedSec.count());
                    displayShutdownAsync(std::move(ptr), true);
                    continue;
                }
            }

            if(auto lastsec = std::min(lastSecStarted, lastSecOnlined); lastsec < UINT32_MAX) {
                // inform alert
                if(100 > lastsec) {
                    emitClearRenderPrimitives(ptr->displayNum);
                    // send render rect
                    const uint16_t fw = ptr->width;
                    const uint16_t fh = 24;
                    emitAddRenderRect(ptr->displayNum, {0, 0, fw, fh}, {0x10, 0x17, 0x80}, true);
                    // send render text
                    auto text = fmt::format("{} limit - time left: {}sec",
                                (lastSecStarted < lastSecOnlined ? "Session" : "Online"), lastsec);
                    const int16_t px = (fw - text.size() * 8) / 2;
                    const int16_t py = (fh - 16) / 2;
                    emitAddRenderText(ptr->displayNum, text, {px, py}, {0xFF, 0xFF, 0});
                }

                // inform beep
                if(10 > lastsec) {
                    emitSendBellSignal(ptr->displayNum);
                }
            }
        }

        timer_limit_.expires_after(dur_limit_);
        timer_limit_.async_wait(std::bind(&DBusAdaptor::timerSessionsTimeLimitAction, this, std::placeholders::_1));
    }

    void DBusAdaptor::timerSessionsEndedAction(const system::error_code& ec) {
        if(ec) {
            return;
        }

        std::erase_if(childs_, [this](auto & pid)
        {
            int status;
            int ret = waitpid(pid, &status, WNOHANG);
            if(0 > ret) {
                Application::error("{}: {} failed, error: {}, code: {}",
                        "removeChildsEnded", "waitpid", strerror(errno), errno);
                return false;
            }

            if(0 == ret) {
                // wnohang - is running
                return false;
            }

            if(WIFSIGNALED(status)) {
                Application::warning("{}: process {}, pid: {}, signal: {}",
                        "removeChildsEnded", "killed", pid, WTERMSIG(status));
            } else if(WIFEXITED(status)) {
                Application::info("{}: process {}, pid: {}, return: {}",
                        "removeChildsEnded", "exited", pid, WEXITSTATUS(status));
            } else {
                Application::info("{}: process {}, pid: {}, wstatus: {:#010x}",
                        "removeChildsEnded", "ended", pid, status);
            }

            if(auto ptr = findPidSession(pid)) {
                Application::notice("{}: session ended, pid: {}", "removeChildsEnded", pid);
                ptr->pid1 = 0;
                this->displayShutdownAsync(std::move(ptr), true);
            }

            return true;
        });

        timer_ended_.expires_after(dur_ended_);
        timer_ended_.async_wait(std::bind(&DBusAdaptor::timerSessionsEndedAction, this, std::placeholders::_1));
    }

    void DBusAdaptor::timerSessionsCheckConnectedAction(const system::error_code& ec) {
        if(ec) {
            return;
        }

        for(const auto & ptr : getOnlineSessions()) {
            // check alive connectors
            if(! ptr->checkStatus(Flags::SessionStatus::CheckConnection)) {
                ptr->setStatus(Flags::SessionStatus::CheckConnection);
                emitPingConnector(ptr->displayNum);
            } else {
                ptr->connectorFailures++;
                // not reply
                Application::warning("connector not reply, display: {}, connector id: {}", ptr->displayNum, ptr->connectorId);

                if(3 < ptr->connectorFailures) {
                    busConnectorTerminated(ptr->displayNum, -1);
                } else {
                    // reset error
                    ptr->resetStatus(Flags::SessionStatus::CheckConnection);
                }
            }
        }

        timer_alive_.expires_after(dur_alive_);
        timer_alive_.async_wait(std::bind(&DBusAdaptor::timerSessionsCheckConnectedAction, this, std::placeholders::_1));
    }

    void DBusAdaptor::displayShutdownAsync(XvfbSessionPtr xvfb, bool emitSignal) {
        asio::post(ioc_, std::bind(&DBusAdaptor::displayShutdown, this, std::move(xvfb), emitSignal));
    }

    bool DBusAdaptor::displayShutdown(XvfbSessionPtr xvfb, bool emitSignal) {
        if(! xvfb || xvfb->mode == SessionMode::Shutdown) {
            return false;
        }

        if(xvfb->connectorId && xvfb->mode != SessionMode::Login) {
            kill(xvfb->connectorId, SIGTERM);
            xvfb->connectorId = 0;
        }

#ifdef LTSM_WITH_AUDIT

        if(xvfb->mode == SessionMode::Connected) {
            auditLog->auditUserDisconnected(xvfb->displayAddr);
        }

#endif
        // mode shutdown
        xvfb->mode = SessionMode::Shutdown;

        if(emitSignal) {
            emitShutdownConnector(xvfb->displayNum);
        }

        Application::notice("{}: display: {}", __FUNCTION__, xvfb->displayNum);
        const bool notSysUser = std::string_view(ltsm_user_conn) != xvfb->userInfo->user();
        
        if(notSysUser) {
            runSessionScript(xvfb, configGetString("session:disconnect"));
            runSystemScript(xvfb, configGetString("system:disconnect"));
        }

        // scripts
        removeDisplaySession(xvfb->displayNum);
        emitDisplayRemoved(xvfb->displayNum);

        if(notSysUser) {
#ifdef LTSM_WITH_AUDIT
            auditLog->auditSessionStop();
#endif
        }

        return true;
    }

    bool DBusAdaptor::checkDisplaySessionAlive(int display) {
        return 0 < display && Tools::checkUnixSocket(Tools::x11UnixPath(display));
    }

    bool DBusAdaptor::checkDisplaySessionStarted(XvfbSessionPtr sess) {
        try {
            auto dbusPath = sess->dbusSessionPath();
            if(! std::filesystem::is_regular_file(dbusPath)) {
                return false;
            }
            auto addr = Tools::fileToString(dbusPath);
            auto dbus = std::make_unique<DisplaySessionProxy>(addr, sess->displayNum);
            if(0 < dbus->getVersion()) {
                // set valid session dbus address
                sess->dbusAddress = std::move(addr);
                return true;
            }
        } catch(...) {}
        return false;
    }

    template <typename WaitFunc>
    bool waitAsioCallable(asio::io_context & ioc, uint32_t total, uint32_t pause, const WaitFunc & waitFunc) {
        asio::steady_timer timer{ioc};

        while(true) {
            timer.expires_after(std::chrono::milliseconds(pause));
            auto wait = timer.async_wait(asio::use_future);

            // ioc in thread pool
            wait.get();

            if(waitFunc()) {
                return true;
            }

            if(total < pause) {
                return false;
            }

            total -= pause;
        }

        return false;
    }

    std::filesystem::path DBusAdaptor::createXauthFile(int displayNum, const std::vector<uint8_t> & mcookie) const {
        // format ltsmRuntimeDir / auth_%{display}
        auto xauthFilePath = ltsmRuntimeDir / "auth_";
        xauthFilePath += std::to_string(displayNum);

        Application::debug(DebugType::App, "{}: path: `{}'", __FUNCTION__, xauthFilePath);
        std::ofstream ofs(xauthFilePath, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);

        if(ofs) {
            byte::ostream bs(ofs);

            // create xautfile
            auto host = Tools::getHostname();
            auto display = std::to_string(displayNum);
            std::string_view magic{"MIT-MAGIC-COOKIE-1"};
            // format: 01 00 [ <host len:be16> [ host ]] [ <display len:be16> [ display ]] [ <magic len:be16> [ magic ]] [ <cookie len:be16> [ cookie ]]
            bs.write_byte(1);
            bs.write_byte(0);
            bs.write_be16(host.size());
            bs.write_string(host);
            bs.write_be16(display.size());
            bs.write_string(display);
            bs.write_be16(magic.size());
            bs.write_string(magic);
            bs.write_be16(mcookie.size());
            bs.write_bytes(mcookie);
            ofs.close();
        } else {
            Application::error("{}: create xauthfile failed, path: `{}'", __FUNCTION__, xauthFilePath);
            return "";
        }

        return xauthFilePath;
    }

    XvfbSessionPtr DBusAdaptor::runNewDisplaySession(const std::string & username,
                const std::string & pass, EnvironmentsMap && envs, OptionsMap && opts) {

        auto userInfo = Tools::getUserInfo(username);

        if(! userInfo) {
            Application::error("{}: user not found: `{}'", __FUNCTION__, username);
            return nullptr;
        }

        if(userInfo->uid() == 0) {
            Application::error("{}: deny for root", __FUNCTION__);
            return nullptr;
        }

        const bool loginMode = username == ltsm_user_conn;

        std::scoped_lock guard{ lockSessions };
        
        // check limit:sessions
        auto freeSlot = std::ranges::find_if(sessions, [](auto & ptr) {
            return ! ptr;
        });

        if(freeSlot == sessions.end()) {
            Application::error("{}: limit:sessions overload", __FUNCTION__);
            return nullptr;
        }

        // check limit:users
        if(int usersLimitMax = configGetInteger("limit:users", 0); 0 < usersLimitMax) {
            std::unordered_set<uid_t> users;
            users.reserve(sessions.size());
            for(const auto & ptr: sessions) {
                if(ptr) {
                    users.emplace(ptr->userInfo->uid());
                }
            }
            if(usersLimitMax <= users.size()) {
                Application::error("{}: limit:users overload", __FUNCTION__);
                return nullptr;
            }
        }

        int freeDisplay = configGetInteger("display:init", 101);
        const int freeDisplayMax = freeDisplay + sessions.size();
    
        for(; freeDisplay <= freeDisplayMax; ++freeDisplay) {
            if(std::ranges::none_of(sessions, [&](auto & ptr) { return ptr && ptr->displayNum == freeDisplay; })) {
                break;
            }
        }

        if(auto x11SocketDir = std::filesystem::path(Tools::x11UnixPath(freeDisplay)).parent_path();
                                    ! std::filesystem::is_directory(x11SocketDir)) {
            std::error_code fserr;
            std::filesystem::create_directories(x11SocketDir, fserr);
            // default permision: 1777
            std::filesystem::permissions(x11SocketDir,
                                         std::filesystem::perms::sticky_bit | std::filesystem::perms::owner_all |
                                         std::filesystem::perms::group_all | std::filesystem::perms::others_all,
                                         std::filesystem::perm_options::replace, fserr);
        }

        auto sess = std::make_shared<XvfbSession>();
        sess->userInfo = std::make_shared<UserSession>(std::move(userInfo), pass);

        if(envs.size()) {
            sess->environments = std::move(envs);
            for(auto & [key, val] : sess->environments) {
                if(std::string::npos != val.find("%{user}")) {
                    val = Tools::replace(val, "%{user}", sess->userInfo->user());
                } else if(std::string::npos != val.find("%{runtime_dir}")) {
                    val = Tools::replace(val, "%{runtime_dir}", sess->userInfo->xdgRuntimeDir().string());
                }
            }
        }

        if(opts.size()) {
            sess->options = std::move(opts);
            JsonObjectStream jos;
            for(const auto & [key, val]: sess->options) {
                jos.push(key, val);
            }
            try {
                auto json = jos.flush();
                auto base64 = Tools::base64Encode(Tools::zlibCompress(json));
                sess->environments.emplace("LTSM_CLIENT_OPTS", std::move(base64));
            } catch(const std::exception & err) {
                Application::error("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }

        sess->mode = loginMode ? SessionMode::Login : SessionMode::Started;
        sess->displayNum = freeDisplay;
        sess->tpStart = std::chrono::system_clock::now();
        sess->displayAddr = fmt::format(":{}", sess->displayNum);
        sess->lifeTimeLimitSec = configGetInteger("session:lifetime:timeout", 0);

        // session xauthfile
        sess->mcookie = Tools::randomBytes(128);
        sess->xauthfile = createXauthFile(freeDisplay, sess->mcookie);

        if(sess->xauthfile.empty()) {
            return nullptr;
        }

        // set permissons user,auth, 0440
        Tools::setFileOwner(sess->xauthfile, sess->userInfo->uid(), Tools::getGroupGid(ltsm_group_auth), 0440);

        // the io_context is not used in the child process, so we skip it...
        // ioc_.notify_fork(asio::execution_context::fork_prepare);

        try {
            sess->pid1 = ForkMode::forkStart();

            // child process
            if(0 == sess->pid1) {
                ChildProcess::pamOpenDisplaySession(std::move(sess), *this);
                // ended
                ForkMode::runChildExit();
            }
        } catch(const std::exception &) {
            return nullptr;
        }

        // main thread
        // ioc_.notify_fork(asio::execution_context::fork_parent);

        Application::debug(DebugType::App, "{}: started, pid: {}, display: {}",
                           __FUNCTION__, sess->pid1, sess->displayNum);

        auto sessionStartTimeout = configGetDouble("session:start:timeout", 3.f);

        // wait display session starting
        if(waitAsioCallable(ioc_, sessionStartTimeout * 1000 /* ms */, 300,
                            std::bind(&DBusAdaptor::checkDisplaySessionStarted, sess))) {
            try {
                // fix X11 socket pemissions 0660
                Tools::setFileOwner(Tools::x11UnixPath(sess->displayNum),
                            sess->userInfo->uid(), Tools::getGroupGid(ltsm_group_auth), 0660);

                (*freeSlot) = std::move(sess);
                return *freeSlot;
            } catch(const std::exception & err) {
                Application::error("{}: {}", __FUNCTION__, "permission", err.what());
            }
        } else {
            Application::error("{}: display session not started", __FUNCTION__);
        }

        ForkMode::waitPid(sess->pid1);
        return nullptr;
    }

    int32_t DBusAdaptor::busStartLoginSession(const int32_t & connectorId, const uint8_t & depth,
            const std::string & remoteAddr, const std::string & connType) {
        Application::debug(DebugType::Dbus, "{}: login request, remote: {}, type: {}",
                           __FUNCTION__, remoteAddr, connType);

        auto sess = runNewDisplaySession(ltsm_user_conn, "", {}, {});

        if(sess) {
            // registered xvfb job
            asio::post(childs_guard_, [this, pid = sess->pid1](){
                childs_.emplace_back(pid);
            });
        } else {
            return -1;
        }

        // update screen
        sess->remoteAddr = remoteAddr;
        sess->conntype = connType;
        sess->connectorId = connectorId;

        startLoginChannels(sess);

        return sess->displayNum;
    }

    int32_t DBusAdaptor::busStartUserSession(const int32_t & oldScreen, const int32_t & connectorId,
            const std::string & userName, const std::string & remoteAddr, const std::string & connType) {
        Application::debug(DebugType::Dbus, "{}: session request, user: {}, remote: {}, display: {}",
                           __FUNCTION__, userName, remoteAddr, oldScreen);

        auto loginSess = findDisplaySession(oldScreen);

        if(! loginSess) {
            Application::error("{}: display not found: {}", __FUNCTION__, oldScreen);
            return -1;
        }

        if(auto userSessions = findUserSessions(userName); !userSessions.empty()) {
            auto oldSess = userSessions.front();

            Application::info("{}: {}, display: {}, user: {}, pid: {}",
                    __FUNCTION__, "connect to session", oldSess->displayNum, oldSess->userInfo->user(), oldSess->pid1);

            if(! checkDisplaySessionAlive(oldSess->displayNum)) {
                Application::error("{}: {} failed, display: {}",
                    __FUNCTION__, "checkDisplaySessionAlive", oldSess->displayNum);
                return -1;
            }

            // parent continue
            oldSess->remoteAddr = remoteAddr;
            oldSess->conntype = connType;
            oldSess->connectorId = connectorId;
            oldSess->encryption = std::move(loginSess->encryption);
            oldSess->layout = std::move(loginSess->layout);
            // FIXME not working
            //oldSess->environments = std::move(loginSess->environments);
            //oldSess->options = std::move(loginSess->options);

            /*
            FIXME
                        // reinit pam session
                        if(! oldSess->pam || ! oldSess->pam->refreshCreds()) {
                            Application::error("{}: {}, display: {}, user: {}",
                                               __FUNCTION__, "PAM failed", oldSess->displayNum, oldSess->userInfo->user());
                            displayShutdownAsync(oldSess, true);
                            return -1;
                        }
            */

            emitSessionReconnect(remoteAddr, connType);

            if(configGetBoolean("session:kill:stop", false)) {
                auto cmd = std::string("/usr/bin/killall -s SIGCONT -u ").append(oldSess->userInfo->user());
                int ret = std::system(cmd.c_str());
                Application::debug(DebugType::App, "{}: command: `{}', return code: {}, display: {}",
                                   __FUNCTION__, cmd, ret, oldSess->displayNum);
            }

            oldSess->dbusSetSessionKeyboardLayout();
            runSessionScript(oldSess, configGetString("session:connect"));

            int res = oldSess->displayNum;
            asio::post(ioc_, std::bind(&DBusAdaptor::startSessionChannels, this, std::move(oldSess)));

            return res;
        }

        // get owner screen
        auto newSess = runNewDisplaySession(userName, loginSess->userInfo->password(),
                std::move(loginSess->environments), std::move(loginSess->options));

        if(newSess) {
            // registered xvfb job
            asio::post(ioc_, [this, pid = newSess->pid1](){
                childs_.emplace_back(pid);
            });
        } else {        
            return -1;
        }

        // parent continue
        Application::info("{}: {}, display: {}, user: {}, pid: {}",
                    __FUNCTION__, "session started", newSess->displayNum, newSess->userInfo->user(), newSess->pid1);

        std::error_code err;
        if(! std::filesystem::is_directory(newSess->userInfo->home(), err)) {
            Application::warning("{}: path not found: `{}'", __FUNCTION__, newSess->userInfo->home());
        }

        // update screen
        newSess->encryption = std::move(loginSess->encryption);
        newSess->layout = std::move(loginSess->layout);
        newSess->remoteAddr = remoteAddr;
        newSess->conntype = connType;
        newSess->connectorId = connectorId;
        newSess->policy = sessionPolicy(Tools::lower(configGetString("session:policy")));
        newSess->mode = SessionMode::Started;
        newSess->tpStart = std::chrono::system_clock::now();
        newSess->lifeTimeLimitSec = configGetInteger("session:lifetime:timeout", 0);
        newSess->idleTimeLimitSec = configGetInteger("session:idle:timeout", 120);
        newSess->idleDisconnect = configGetBoolean("session:idle:disconnect", false);

        if(! configGetBoolean("transfer:file:disabled", false)) {
            newSess->setStatus(Flags::AllowChannel:: TransferFiles);
        }

        if(! configGetBoolean("channel:printer:disabled", false)) {
            newSess->setStatus(Flags::AllowChannel::RedirectPrinter);
        }

        if(! configGetBoolean("channel:audio:disabled", false)) {
            newSess->setStatus(Flags::AllowChannel::RedirectAudio);
        }

        if(! configGetBoolean("channel:pcsc:disabled", false)) {
            newSess->setStatus(Flags::AllowChannel::RedirectPcsc);
        }

        if(! configGetBoolean("channel:sane:disabled", false)) {
            newSess->setStatus(Flags::AllowChannel::RedirectScanner);
        }

        if(! configGetBoolean("channel:fuse:disabled", false)) {
            newSess->setStatus(Flags::AllowChannel::RemoteFilesUse);
        }

        newSess->dbusSetSessionKeyboardLayout();
        if(true) {
            asio::post(ioc_, [ptr = newSess, system = configGetString("system:connect"), session = configGetString("session:connect")](){
                    runSystemScript(ptr, system);
                    runSessionScript(ptr, session);
                }
            );
        }

#ifdef LTSM_WITH_AUDIT
        auditLog->auditSessionStart();
#endif

        int res = newSess->displayNum;
        asio::post(ioc_, std::bind(&DBusAdaptor::startSessionChannels, this, std::move(newSess)));

        return res;
    }

    int32_t DBusAdaptor::busGetServiceVersion(void) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);
        return LTSM::service_version;
    }

    std::string DBusAdaptor::busDisplayAuthFile(const int32_t & display) {
        Application::debug(DebugType::Dbus, "{}: display: {}", __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display)) {
            return xvfb->xauthfile;
        }

        Application::warning("{}: display not found: {}", __FUNCTION__, display);
        return "";
    }

    void DBusAdaptor::busShutdownDisplay(const int32_t & display) {
        Application::debug(DebugType::Dbus, "{}: display: {}", __FUNCTION__, display);

        if(auto ptr = findDisplaySession(display)) {
            displayShutdownAsync(std::move(ptr), true);
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busShutdownConnector(const int32_t & display) {
        Application::debug(DebugType::Dbus, "{}: display: {}", __FUNCTION__, display);
        asio::post(ioc_, [this, display]() {
            this->emitShutdownConnector(display);
        });
    }

    void DBusAdaptor::busShutdownService(void) {
        Application::debug(DebugType::Dbus, "{}: {}, pid: {}", __FUNCTION__, "starting", getpid());

        asio::post(ioc_, [this]() {
            this->stop();
        });
    }

    void DBusAdaptor::busSendMessage(const int32_t & display, const std::string & message) {
        Application::debug(DebugType::Dbus, "{}: display: {}, message: `{}'", __FUNCTION__, display, message);

        if(auto xvfb = findDisplaySession(display)) {
            if(xvfb->mode == SessionMode::Connected ||
               xvfb->mode == SessionMode::Disconnected) {
                // zenity info
                std::ignore = xvfb->dbusRunSessionZenity({ "--info", "--no-wrap", "--text", Tools::quotedString(message) });
                return;
            }

            Application::warning("{}: {} failed, display: {}", __FUNCTION__, "session mode", display);
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSessionIdleTimeout(const int32_t & display) {
        Application::debug(DebugType::Dbus, "{}: display: {}", __FUNCTION__, display);

        if(auto ptr = findDisplaySession(display)) {
            emitSessionIdleTimeout(ptr->displayNum, ptr->userInfo->user());

            if(ptr->idleDisconnect) {
                asio::post(ioc_, [this, xvfb = std::move(ptr)]() {
                    this->emitShutdownConnector(xvfb->displayNum);
                });
            }
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busConnectorAlive(const int32_t & display) {
        Application::debug(DebugType::Dbus, "{}: display: {}", __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display)) {
            xvfb->resetStatus(Flags::SessionStatus::CheckConnection);
            xvfb->connectorFailures = 0;
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetLoginsDisable(const bool & action) {
        Application::debug(DebugType::Dbus, "{}: action: {}", __FUNCTION__, (action ? "true" : "false"));
        loginsDisable = action;
    }

#ifdef LTSM_BUILD_COVERAGE_TESTS
    bool skipLoginShutdown(int display) {
        try {
            if(auto env = getenv("LTSM_SESSION_TEST")) {
                if(auto sid = std::stoi(env); sid == display) {
                    return true;
                }
            }
        } catch(...) {
        }

        return false;
    }
#endif

    void DBusAdaptor::busConnectorConnected(const int32_t & display, const int32_t & connectorId) {
        Application::debug(DebugType::Dbus, "{}: display: {}", __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display)) {
            xvfb->connectorId = connectorId;
            xvfb->tpOnline = std::chrono::system_clock::now();
            xvfb->onlineTimeLimitSec = configGetInteger("session:online:timeout", 0);
            xvfb->mode = SessionMode::Connected;

#ifdef LTSM_WITH_AUDIT
            auditLog->auditUserConnected(xvfb->displayAddr);
#endif

            emitSessionOnline(xvfb->displayNum, xvfb->userInfo->user());
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busConnectorTerminated(const int32_t & display, const int32_t & connectorId) {
        Application::debug(DebugType::Dbus, "{}: display: {}", __FUNCTION__, display);

        auto ptr = findDisplaySession(display);

        if(! ptr) {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
            return;
        }

        if(ptr->mode == SessionMode::Login) {
#ifdef LTSM_BUILD_COVERAGE_TESTS

            if(skipLoginShutdown(display)) {
                return;
            }

#endif
            stopLoginChannels(ptr);
            displayShutdownAsync(std::move(ptr), false);
        } else if(ptr->mode == SessionMode::Connected) {
            ptr->resetStatus(Flags::SessionStatus::CheckConnection);
            ptr->remoteAddr.clear();
            ptr->conntype.clear();
            ptr->encryption.clear();
            ptr->connectorId = 0;
            ptr->tpOffline = std::chrono::system_clock::now();
            ptr->offlineTimeLimitSec = configGetInteger("session:offline:timeout", 0);
            ptr->mode = SessionMode::Disconnected;

#ifdef LTSM_WITH_AUDIT
            auditLog->auditUserDisconnected(ptr->displayAddr);
#endif

            // stop user process
            if(configGetBoolean("session:kill:stop", false)) {
                auto cmd = std::string("/usr/bin/killall -s SIGSTOP -u ").append(ptr->userInfo->user());
                int ret = std::system(cmd.c_str());
                Application::debug(DebugType::App, "{}: command: `{}', return code: {}, display: {}",
                                   __FUNCTION__, cmd, ret, ptr->displayNum);
            }

            emitSessionOffline(ptr->displayNum, ptr->userInfo->user());
            stopSessionChannels(std::move(ptr));
        }
    }

    bool DBusAdaptor::transferFileCopyAllow(XvfbSessionPtr xvfb, const std::filesystem::path & dstdir, const std::filesystem::path & tmpname, const FileNameSize & info) {
        Application::debug(DebugType::App, "{}: transfer file request, display: {}, select dir: `{}', tmp name: `{}'",
                               __FUNCTION__, xvfb->displayNum, dstdir, tmpname);
        auto filepath = std::filesystem::path(std::get<0>(info));
        auto filesize = std::get<1>(info);
        std::error_code fserr;

        // check disk space limited
        if(auto spaceInfo = std::filesystem::space(dstdir, fserr); spaceInfo.available < filesize) {
            sendNotifyCallAsync(xvfb, "Transfer Rejected", "not enough disk space", NotifyParams::Error);
            Application::error("{}: no space available", __FUNCTION__);
            throw service_error(NS_FuncNameS);
        }

        // check dstdir writeable / filename present
        auto dstfile = dstdir / filepath.filename();

        if(std::filesystem::exists(dstfile, fserr)) {
            Application::error("{}: file present and skipping, path: `{}'", __FUNCTION__, dstfile);
            sendNotifyCallAsync(xvfb, "Transfer Skipping", fmt::format("such a file exists: {}", dstfile.string()), NotifyParams::Warning);
            return false;
        }

        emitTransferAllow(xvfb->displayNum, filepath, tmpname, dstdir);
        return true;
    }

    void DBusAdaptor::transferFilesRequestCommunication(XvfbSessionPtr xvfb, std::vector<FileNameSize> files, std::string msg) {

        auto emitTransferReject = [this, display = xvfb->displayNum, &files]() {
            for(const auto & info : files) {
                // empty dst/file erase job
                this->emitTransferAllow(display, std::get<0>(info), "", "");
            }
        };

        // wait zenity question
        auto statusQuestion = xvfb->dbusRunSessionZenity({ "--question", "--default-cancel", "--text", msg });

        // yes = 0, no: 256
        if(256 == std::get<0>(statusQuestion)) {
            emitTransferReject();
            return;
        }

        // zenity select directory, wait file selection
        auto statusSelectDir = xvfb->dbusRunSessionZenity( {
            "--file-selection", "--directory",
            "--title", "Select directory",
            "--width", "640", "--height", "480" });

        // status: ok = 0, cancel: 256
        if(256 == std::get<0>(statusSelectDir)) {
            emitTransferReject();
            return;
        }

        // get dstdir
        const auto & buf = std::get<1>(statusSelectDir);
        auto end = buf.back() == 0x0a ? std::prev(buf.end()) : buf.end();
        std::filesystem::path dstdir(std::string(buf.begin(), end));

        std::error_code fserr;
        if(! std::filesystem::is_directory(dstdir, fserr)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                                __FUNCTION__, "is_directory", fserr.value(), fserr.message(), dstdir.string());
            emitTransferReject();
            return;
        }

        // copy all files to (Connector) user home, after success move to real user
        auto connectorHome = Tools::getUserHome(ltsm_user_conn);

        for(const auto & info: files) {
            auto tmpname = std::filesystem::path(connectorHome) / "transfer_";
            tmpname += Tools::randomHexString(8);

            try {
                transferFileCopyAllow(xvfb, dstdir, tmpname, info);
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", __FUNCTION__, err.what());
                return;
            }
        }
    }

    asio::awaitable<void> DBusAdaptor::transferFileComplete(XvfbSessionPtr xvfb, std::string tmpfile, uint32_t filesz) const {
        asio::steady_timer timer{ioc_};

        while(true) {
            // check fill data complete
            if(std::filesystem::exists(tmpfile) &&
               std::filesystem::file_size(tmpfile) >= filesz) {
                break;
            }

            // check lost conn
            if(xvfb->mode != SessionMode::Connected) {
                sendNotifyCallAsync(xvfb, "Transfer Error", "transfer connection is lost", NotifyParams::Error);
                std::filesystem::remove(tmpfile);
                Application::warning("{}: session disconnected, display: {}", __FUNCTION__, xvfb->displayNum);
                throw service_error(NS_FuncNameS);
            }

            timer.expires_after(250ms);
            co_await timer.async_wait(asio::use_awaitable);
        }

        co_return;
    }

    asio::awaitable<void> DBusAdaptor::transferFileStartBackground(XvfbSessionPtr xvfb, std::string tmpfile, std::string dstfile, uint32_t filesz) const {
        try {
            co_await transferFileComplete(xvfb, tmpfile, filesz);
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", __FUNCTION__, err.what());
            co_return;
        }

        std::error_code fserr;
        // move tmpfile to dstfile
        std::filesystem::rename(tmpfile, dstfile, fserr);

        if(fserr) {
            // rename failed
            if(fserr == std::errc::cross_device_link) {
                std::filesystem::copy_file(tmpfile, dstfile, fserr);
            } else {
                Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                        __FUNCTION__, "rename", fserr.value(), fserr.message(), dstfile);
                co_return;
            }

            std::filesystem::remove(tmpfile, fserr);
        }

        Tools::setFileOwner(dstfile, xvfb->userInfo->uid(), xvfb->userInfo->gid());
        sendNotifyCallAsync(xvfb, "Transfer Complete",
                          fmt::format("new file added: <a href='file://{}'>{}</a>",
                                     dstfile, std::filesystem::path(dstfile).filename().string()),
                          NotifyParams::Information);
        co_return;
    }

    bool DBusAdaptor::busTransferFilesRequest(const int32_t & display, const std::vector<FileNameSize> & files) {
        Application::debug(DebugType::Dbus, "{}: display: {}, count: {}", __FUNCTION__, display, files.size());
        auto xvfb = findDisplaySession(display);

        if(! xvfb) {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::TransferFiles)) {
            Application::warning("{}: display {}, transfer reject", __FUNCTION__, display);
            sendNotifyCallAsync(xvfb, "Transfer Restricted", "transfer is blocked, contact the administrator",
                          NotifyParams::IconType::Warning);
            return false;
        }

        if(configHasKey("transfer:group:only")) {
            if(auto groupInfo = Tools::getGroupInfo(configGetString("transfer:group:only"))) {
                auto gids = xvfb->userInfo->groups();

                if(std::ranges::none_of(gids, [&](auto & gid) { return gid == groupInfo->gid(); })) {
                    Application::warning("{}: display {}, transfer reject", __FUNCTION__, display);
                    sendNotifyCallAsync(xvfb, "Transfer Restricted", "transfer is blocked, contact the administrator",
                                  NotifyParams::IconType::Warning);
                    return false;
                }
            }
        }

        //run background
        asio::post(ioc_, std::bind(&DBusAdaptor::transferFilesRequestCommunication, this, std::move(xvfb),
                    files, fmt::format("Can you receive remote files? ({})", files.size())));

        return true;
    }

    bool DBusAdaptor::busTransferFileStarted(const int32_t & display, const std::string & tmpfile,
            const uint32_t & filesz, const std::string & dstfile) {
        Application::debug(DebugType::Dbus, "{}: display: {}, tmp file: `{}', dst file: `{}'",
                           __FUNCTION__, display, tmpfile, dstfile);

        if(auto xvfb = findDisplaySession(display)) {
            //run background
            asio::co_spawn(ioc_, std::bind(&DBusAdaptor::transferFileStartBackground, this,
                    std::move(xvfb), tmpfile, dstfile, filesz), asio::detached);
            return true;
        }

        Application::warning("{}: display not found: {}", __FUNCTION__, display);
        return false;
    }

    asio::awaitable<void> DBusAdaptor::sendNotifyCall(XvfbSessionPtr xvfb, std::string summary, std::string body, uint8_t icontype) const {

        // wait new session started
        if(xvfb->sessionOnlinedSec() < std::chrono::seconds(2)) {
            asio::steady_timer timer{ioc_, 2s};
            co_await timer.async_wait(asio::use_awaitable);
        }

        switch(icontype) {
            case NotifyParams::IconType::Warning:
                xvfb->dbusNotifyWarning(summary, body);
                break;

            case NotifyParams::IconType::Error:
                xvfb->dbusNotifyError(summary, body);
                break;

            default:
                xvfb->dbusNotifyInfo(summary, body);
                break;
        }

        co_return;
    }

    void DBusAdaptor::sendNotifyCallAsync(XvfbSessionPtr xvfb, std::string summary, std::string body, uint8_t icontype) const {
        asio::co_spawn(ioc_, std::bind(&DBusAdaptor::sendNotifyCall, this, std::move(xvfb), std::move(summary), std::move(body), icontype), asio::detached);
    }

    void DBusAdaptor::busSendNotify(const int32_t & display, const std::string & summary, const std::string & body,
                                    const uint8_t & icontype, const uint8_t & urgency) {
        Application::debug(DebugType::Dbus, "{}: display: {}, summary: `{}', body: `{}'",
                           __FUNCTION__, display, summary, body);

        // urgency:  NotifyParams::UrgencyLevel { Low, Normal, Critical }
        // icontype: NotifyParams::IconType { Information, Warning, Error, Question }
        if(auto xvfb = findDisplaySession(display)) {
            if(xvfb->mode == SessionMode::Connected ||
               xvfb->mode == SessionMode::Disconnected) {
                // thread mode
                sendNotifyCallAsync(xvfb, summary, body, icontype /*, urgency2 = urgency */);
                return;
            }

            Application::warning("{}: {} failed, display: {}", __FUNCTION__, "session mode", display);
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    std::forward_list<std::string> DBusAdaptor::getAllowLogins(void) const {
        // uids names: "access:uid:min", "access:uid:max"
        int minUidRange = configGetInteger("access:uid:min", 0);
        int maxUidRange = configGetInteger("access:uid:max", INT32_MAX);
        auto accessUidNames = Tools::getSystemUsers(minUidRange, maxUidRange);
        // access list: "access:users"
        auto accessUsersNames = config().getStdListForward<std::string>("access:users");

        // append list: "access:groups"
        for(const auto & group : config().getStdListForward<std::string>("access:groups")) {
            try {
                accessUsersNames.splice_after(accessUsersNames.begin(), GroupInfo(group).members());
            } catch(const std::exception &) {
            }
        }

        if(accessUsersNames.empty()) {
            return accessUidNames;
        }

        accessUsersNames.sort();
        accessUsersNames.unique();

        if(accessUidNames.empty()) {
            return accessUsersNames;
        }

        accessUidNames.sort();
        accessUidNames.unique();
        std::forward_list<std::string> allowNames;
        std::set_intersection(accessUsersNames.begin(), accessUsersNames.end(), accessUidNames.begin(), accessUidNames.end(),
                              std::front_inserter(allowNames));
        return allowNames;
    }

    std::vector<std::string> DBusAdaptor::helperGetUsersList(const int32_t & display) {
        auto allowLogins = getAllowLogins();
        return std::vector<std::string>(allowLogins.begin(), allowLogins.end());
    }

    bool DBusAdaptor::busSetAuthenticateToken(const int32_t & display, const std::string & login) {
        Application::debug(DebugType::Dbus, "{}: display: {}, user: {}",
                           __FUNCTION__, display, login);

        if(auto xvfb = this->findDisplaySession(display)) {
            asio::post(ioc_, std::bind(&DBusAdaptor::pamAuthenticate, this, std::move(xvfb), login, "******", true));
            return true;
        }

        Application::warning("{}: display not found: {}", __FUNCTION__, display);
        return true;
    }

    bool DBusAdaptor::busSetAuthenticateLoginPass(const int32_t & display, const std::string & login,
            const std::string & password) {
        Application::debug(DebugType::Dbus, "{}: display: {}, user: {}",
                           __FUNCTION__, display, login);

        if(auto xvfb = this->findDisplaySession(display)) {
            asio::post(ioc_, std::bind(&DBusAdaptor::pamAuthenticate, this, std::move(xvfb), login, password, false));
            return true;
        }

        Application::warning("{}: display not found: {}", __FUNCTION__, display);
        return false;
    }

    bool DBusAdaptor::pamAuthenticate(XvfbSessionPtr xvfb, const std::string & login, const std::string & password,
                                      bool token) {
        Application::info("{}: display: {}, user: {}", __FUNCTION__, xvfb->displayNum, login);
        auto users = getAllowLogins();

        if(users.empty()) {
            Application::error("{}: {}, display: {}, user: {}",
                               __FUNCTION__, "login disabled", xvfb->displayNum, login);
            emitLoginFailure(xvfb->displayNum, "login disabled");
            return false;
        }

        if(std::ranges::none_of(users, [&](auto & val) { return val == login; })) {
            Application::error("{}: {}, display: {}, user: {}",
                               __FUNCTION__, "login not found", xvfb->displayNum, login);
            emitLoginFailure(xvfb->displayNum, "login not found");
            return false;
        }

        if(loginsDisable) {
            Application::info("{}: {}, display: {}", __FUNCTION__, "logins disabled", xvfb->displayNum);
            emitLoginFailure(xvfb->displayNum, "logins disabled by the administrator");
            return false;
        }

        int loginFailuresConf = configGetInteger("login:failures_count", 0);

        if(0 > loginFailuresConf) {
            loginFailuresConf = 0;
        }

        // open PAM
        auto pam = std::make_unique<PamSession>(configGetString("pam:service"), login, password);

        if(! pam->pamStart(login)) {
            emitLoginFailure(xvfb->displayNum, "pam error");
            return false;
        }

        if(! token) {
            // check user/pass
            if(! pam->authenticate()) {
                emitLoginFailure(xvfb->displayNum, pam->error());
                xvfb->loginFailures += 1;

                if(loginFailuresConf < xvfb->loginFailures) {
                    Application::error("{}: login failures limit, display: {}", __FUNCTION__, xvfb->displayNum);
                    emitLoginFailure(xvfb->displayNum, "failures limit");
                    displayShutdownAsync(xvfb, true);
                }

                return false;
            }

            xvfb->userInfo->setPassword(password);
            pam->setItem(PAM_XDISPLAY, xvfb->displayAddr.c_str());
            pam->setItem(PAM_TTY, std::string("X11:").append(xvfb->displayAddr).c_str());
            pam->setItem(PAM_RHOST, xvfb->remoteAddr.empty() ? "127.0.0.1" : xvfb->remoteAddr.c_str());

            if(! pam->validateAccount()) {
                Application::error("{}: {} failed", __FUNCTION__, "validate account");
                for(auto & sess: findUserSessions(login)) {
                    busShutdownDisplay(sess->displayNum);
                }
                return false;
            }
        }

        // auth success
        if(0 < loginFailuresConf) {
            xvfb->loginFailures = 0;
        }

        // check connection policy
        auto userSess = findUserSession(login);

        if(userSess && 0 < userSess->displayNum &&
           userSess->mode == SessionMode::Connected) {
            if(userSess->policy == SessionPolicy::AuthLock) {
                Application::error("{}: session busy, policy: {}, user: {}, session display: {}, from: {}, display: {}",
                                   __FUNCTION__, "authlock", login, userSess->displayNum, userSess->remoteAddr, xvfb->displayNum);
                // informer login display
                emitLoginFailure(xvfb->displayNum, fmt::format("session busy, from: {}", userSess->remoteAddr));
                return false;
            } else if(userSess->policy == SessionPolicy::AuthTake) {
                // shutdown prev connect
                emitShutdownConnector(userSess->displayNum);
                // wait session: changes connected
                waitAsioCallable(ioc_, 2000, 50, [userSess](){ return userSess->mode == SessionMode::Disconnected; });
            }
        }

        Application::notice("{}: success, display: {}, user: {}, token: {}",
                            __FUNCTION__, xvfb->displayNum, login, (token ? "true" : "false"));

        emitLoginSuccess(xvfb->displayNum, login, Tools::getUserUid(login));
        return true;
    }

    void DBusAdaptor::busSetSessionKeyboardLayouts(const int32_t & display, const std::vector<std::string> & layouts) {
        Application::debug(DebugType::Dbus, "{}: display: {}, layouts: [{}]",
                           __FUNCTION__, display, Tools::join(layouts, ","));

        if(auto xvfb = findDisplaySession(display)) {
            if(layouts.empty()) {
                return;
            }

            std::ostringstream os;

            for(auto it = layouts.begin(); it != layouts.end(); ++it) {
                auto id = Tools::lower((*it).substr(0, 2));

                if(id == "en") {
                    id = "us";
                }

                os << id;

                if(std::next(it) != layouts.end()) {
                    os << ",";
                }
            }

            xvfb->layout = Tools::quotedString(os.str());
            xvfb->dbusSetSessionKeyboardLayout();
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionEnvironments(const int32_t & display, const std::map<std::string, std::string> & map) {
        Application::debug(DebugType::Dbus, "{}: display: {}, env counts: {}",
                           __FUNCTION__, display, map.size());
        auto xvfb = findDisplaySession(display);

        if(! xvfb) {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
            return;
        }

        xvfb->environments.clear();

        for(const auto & [key, val] : map) {
            Application::info("{}: {} = `{}'", __FUNCTION__, key, val);
            xvfb->environments.emplace(key, val);

            if(key == "TZ") {
                emitHelperSetTimezone(display, val);
            }
        }
    }

    void DBusAdaptor::busSetSessionEncodings(const int32_t& display, const std::vector<int32_t>& encs) {
        Application::debug(DebugType::Dbus, "{}: display: {}, encodings counts: {}",
                           __FUNCTION__, display, encs.size());

        auto xvfb = findDisplaySession(display);

        if(! xvfb) {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
            return;
        }

        xvfb->encodings = encs;
    }

    void DBusAdaptor::busSetSessionOptions(const int32_t & display, const std::map<std::string, std::string> & map) {
        Application::debug(DebugType::Dbus, "{}: display: {}, opts counts: {}",
                           __FUNCTION__, display, map.size());

        auto xvfb = findDisplaySession(display);

        if(! xvfb) {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
            return;
        }

        xvfb->options.clear();
        std::string login, pass;

        for(const auto & [key, val] : map) {
            Application::info("{}: {} = `{}'", __FUNCTION__, key, (key != "password" ? val : "HIDDEN"));

            if(key == "redirect:cups") {
                if(configGetBoolean("channel:printer:disabled", false)) {
                    continue;
                }
            } else if(key == "redirect:fuse") {
                if(configGetBoolean("channel:fuse:disabled", false)) {
                    continue;
                }
            } else if(key == "redirect:audio") {
                if(configGetBoolean("channel:audio:disabled", false)) {
                    continue;
                }
            } else if(key == "redirect:pcsc") {
                if(configGetBoolean("channel:pcsc:disabled", false)) {
                    continue;
                }

                xvfb->environments.emplace("PCSCLITE_CSOCK_NAME", "%{runtime_dir}/pcsc2ltsm");
            } else if(key == "redirect:sane") {
                if(configGetBoolean("channel:sane:disabled", false)) {
                    continue;
                }

                xvfb->environments.emplace("SANE_UNIX_PATH", saneRuntimeFmt);
            } else if(key == "username") {
                login = val;
            } else if(key == "password") {
                pass = val;
            } else if(key == "pkcs11:auth") {
                startPkcs11Listener(xvfb, "");
                emitHelperPkcs11ListennerStarted(display, xvfb->connectorId);
            }

            xvfb->options.emplace(key, val);
        }

        if(! login.empty()) {
            emitHelperSetLoginPassword(display, login, pass, ! pass.empty());
        }
    }

    bool waitFileSetPermission(asio::io_context & ioc, std::filesystem::path path, uid_t uid, gid_t gid, mode_t mode) {
        auto fileExists = [&path]() {
            std::error_code fserr;
            return std::filesystem::exists(path, fserr);
        };

        if(waitAsioCallable(ioc, 3500, 300, fileExists)) {
            Tools::setFileOwner(path, uid, gid, mode);
            return true;
        }

        return false;
    }

    void DBusAdaptor::startSessionChannels(XvfbSessionPtr xvfb) {

        auto printer = xvfb->options.find("redirect:cups");
        auto sane = xvfb->options.find("redirect:sane");
        auto audio = xvfb->options.find("redirect:audio");
        auto pcsc = xvfb->options.find("redirect:pcsc");
        auto fuse = xvfb->options.find("redirect:fuse");

        // wait new session started
        if(xvfb->sessionOnlinedSec() < 2s) {
            waitAsioCallable(ioc_, 2000, 500, [xvfb](){ return 2s <= xvfb->sessionOnlinedSec(); });
        }

        try {
            if(xvfb->options.end() != printer) {
                std::bind(&DBusAdaptor::startPrinterListener, this, xvfb, printer->second);
            }

            if(xvfb->options.end() != sane) {
                startSaneListener(xvfb, sane->second);
            }

            if(xvfb->options.end() != audio) {
                startAudioListener(xvfb, audio->second);
            }

            if(xvfb->options.end() != pcsc) {
                startPcscListener(xvfb, pcsc->second);
            }

            if(xvfb->options.end() != fuse && ! fuse->second.empty()) {
                for(const auto & share : JsonContentString(Tools::unescaped(fuse->second)).toArray().toStdList<std::string>()) {
                    startFuseListener(xvfb, share);
                }
            }
        } catch(const std::exception & err) {
            Application::warning("{}: exception: `{}'", __FUNCTION__, err.what());
        }
    }

    void DBusAdaptor::stopSessionChannels(XvfbSessionPtr xvfb) {
        if(0 < xvfb->connectorId) {
            auto fuse = xvfb->options.find("redirect:fuse");

            if(xvfb->options.end() != fuse && ! fuse->second.empty()) {
                for(const auto & share : JsonContentString(Tools::unescaped(fuse->second)).toArray().toStdList<std::string>()) {
                    stopFuseListener(xvfb, share);
                }
            }

            auto audio = xvfb->options.find("redirect:audio");

            if(xvfb->options.end() != audio) {
                stopAudioListener(xvfb, audio->second);
            }

            auto pcsc = xvfb->options.find("redirect:pcsc");

            if(xvfb->options.end() != pcsc) {
                stopPcscListener(xvfb, pcsc->second);
            }
        }
    }

    void DBusAdaptor::startLoginChannels(XvfbSessionPtr xvfb) {
    }

    void DBusAdaptor::stopLoginChannels(XvfbSessionPtr xvfb) {
        if(0 < xvfb->connectorId) {
            auto pkcs11 = xvfb->options.find("pkcs11:auth");

            if(xvfb->options.end() != pkcs11) {
                stopPkcs11Listener(xvfb, pkcs11->second);
            }
        }
    }

    bool DBusAdaptor::startPrinterListener(XvfbSessionPtr xvfb, const std::string & clientUrl) {
        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectPrinter)) {
            Application::warning("{}: display {}, redirect disabled: {}", __FUNCTION__, xvfb->displayNum, "printer");
            sendNotifyCallAsync(xvfb, "Channel Disabled", "redirect " "printer" " is blocked, contact the administrator",
                          NotifyParams::IconType::Warning);
            return false;
        }

        Application::info("{}: url: {}", __FUNCTION__, clientUrl);
        auto[clientType, clientAddress] = Channel::parseUrl(clientUrl);

        if(clientType == Channel::ConnectorType::Unknown) {
            Application::error("{}: {}, unknown client url: {}", __FUNCTION__, "printer", clientUrl);
            return false;
        }

        auto socketFolder = std::filesystem::path(Tools::replace(cupsRuntimeFmt, "%{user}", xvfb->userInfo->user())).parent_path();
        auto lp = Tools::getGroupGid("lp");
        std::error_code err;

        if(! std::filesystem::is_directory(socketFolder, err) &&
           ! std::filesystem::create_directories(socketFolder, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                            __FUNCTION__, "create_directories", err.value(), err.message(), socketFolder.string());
            return false;
        }

        // fix owner xvfb.lp, mode 0750
        Tools::setFileOwner(socketFolder, Tools::getUserUid(ltsm_user_conn), lp, 0750);
        auto printerSocket = socketFolder / std::to_string(xvfb->connectorId);
        printerSocket += ".sock";

        if(std::filesystem::is_socket(printerSocket, err)) {
            std::filesystem::remove(printerSocket, err);
        }

        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, printerSocket.string());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::WriteOnly),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadOnly), "medium", 5,
                           static_cast<uint32_t>(Channel::OptsFlags::ZLibCompression));
        // fix permissions job
        return waitFileSetPermission(ioc_, printerSocket, xvfb->userInfo->uid(), lp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    }

    bool DBusAdaptor::startAudioListener(XvfbSessionPtr xvfb, const std::string & param) {
        if(xvfb->mode == SessionMode::Login) {
            Application::error("{}: login session skipped, display: {}", __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectAudio)) {
            Application::warning("{}: display {}, redirect disabled: {}", __FUNCTION__, xvfb->displayNum, "audio");
            sendNotifyCallAsync(xvfb, "Channel Disabled", "redirect " "audio" " is blocked, contact the administrator",
                          NotifyParams::IconType::Warning);
            return false;
        }

        Application::info("{}: param: `{}'", __FUNCTION__, param);
        auto audioFolder = std::filesystem::path(Tools::replace(audioRuntimeFmt, "%{user}", xvfb->userInfo->user()));
        std::error_code err;

        if(! std::filesystem::is_directory(audioFolder, err) &&
           ! std::filesystem::create_directories(audioFolder, err)) {
            Application::error("{}: {} failed, code: {}, error: {} path: `{}'",
                            __FUNCTION__, "create_directories", err.value(), err.message(), audioFolder.string());
            return false;
        }

        // fix owner xvfb.user, mode 0750
        Tools::setFileOwner(audioFolder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid(), 0750);
        auto audioSocket = std::filesystem::path(audioFolder) / std::to_string(xvfb->connectorId);
        audioSocket += ".sock";

        if(std::filesystem::is_socket(audioSocket, err)) {
            std::filesystem::remove(audioSocket, err);
        }

        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Audio, "");
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, audioSocket.string());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "ultra", 5, 0);

        // fix permissions job
        if(waitFileSetPermission(ioc_, audioSocket, xvfb->userInfo->uid(), xvfb->userInfo->gid(), S_IRUSR | S_IWUSR)){
            Application::info("{}: display: {}, user: {}, socket: `{}'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), audioSocket);

            if(xvfb->dbusAudioChannelConnect(audioSocket)) {
                return true;
            }

            // destroy channel
            auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, audioSocket.string());
            auto clientUrl = Channel::createUrl(Channel::ConnectorType::Audio, "");
            emitDestroyListener(xvfb->displayNum, clientUrl, serverUrl);
            return false;
        }
        return false;
    }

    void DBusAdaptor::stopAudioListener(XvfbSessionPtr xvfb, const std::string & param) {
        Application::info("{}: param: `{}'", __FUNCTION__, param);
        auto audioFolder = std::filesystem::path(Tools::replace(audioRuntimeFmt, "%{user}", xvfb->userInfo->user()));
        auto audioSocket = std::filesystem::path(audioFolder) / std::to_string(xvfb->connectorId);
        audioSocket += ".sock";

        Application::info("{}: display: {}, user: {}, socket: `{}'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), audioSocket);

        xvfb->dbusAudioChannelDisconnect(audioSocket);
    }

    bool DBusAdaptor::startSaneListener(XvfbSessionPtr xvfb, const std::string & clientUrl) {
        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectScanner)) {
            Application::warning("{}: display {}, redirect disabled: {}", __FUNCTION__, xvfb->displayNum, "scanner");
            sendNotifyCallAsync(xvfb, "Channel Disabled", "redirect " "scanner" " is blocked, contact the administrator",
                          NotifyParams::IconType::Warning);
            return false;
        }

        Application::info("{}: url: {}", __FUNCTION__, clientUrl);
        auto[clientType, clientAddress] = Channel::parseUrl(clientUrl);

        if(clientType == Channel::ConnectorType::Unknown) {
            Application::error("{}: {}, unknown client url: {}", __FUNCTION__, "sane", clientUrl);
            return false;
        }

        auto socketFolder = std::filesystem::path{Tools::replace(saneRuntimeFmt, "%{user}", xvfb->userInfo->user())};
        std::error_code err;

        if(! std::filesystem::is_directory(socketFolder, err) &&
           ! std::filesystem::create_directories(socketFolder, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                            __FUNCTION__, "create_directories", err.value(), err.message(), socketFolder);
            return false;
        }

        // fix owner xvfb.user, mode 0750
        Tools::setFileOwner(socketFolder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid(), 0750);
        auto saneSocket = socketFolder / std::to_string(xvfb->connectorId);
        saneSocket += ".sock";

        if(std::filesystem::is_socket(saneSocket, err)) {
            std::filesystem::remove(saneSocket, err);
        }

        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, saneSocket.string());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "medium", 5,
                           static_cast<uint32_t>(Channel::OptsFlags::ZLibCompression));
        // fix permissions job
        return waitFileSetPermission(ioc_, saneSocket, xvfb->userInfo->uid(), xvfb->userInfo->gid(),
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    }

    bool DBusAdaptor::startPcscListener(XvfbSessionPtr xvfb, const std::string & param) {
        if(xvfb->mode == SessionMode::Login) {
            Application::error("{}: login session skipped, display: {}", __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectPcsc)) {
            Application::warning("{}: display {}, redirect disabled: {}", __FUNCTION__, xvfb->displayNum, "pcsc");
            sendNotifyCallAsync(xvfb, "Channel Disabled", "redirect " "smartcard" " is blocked, contact the administrator",
                          NotifyParams::IconType::Warning);
            return false;
        }

        Application::info("{}: param: `{}'", __FUNCTION__, param);
        auto pcscFolder = std::filesystem::path(Tools::replace(pcscRuntimeFmt, "%{user}", xvfb->userInfo->user()));
        std::error_code err;

        if(! std::filesystem::is_directory(pcscFolder, err) &&
           ! std::filesystem::create_directories(pcscFolder, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                            __FUNCTION__, "create_directories", err.value(), err.message(), pcscFolder.string());
            return false;
        }

        // fix owner xvfb.user, mode 0750
        Tools::setFileOwner(pcscFolder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid(), 0750);
        auto pcscSocket = std::filesystem::path(pcscFolder) / std::to_string(xvfb->connectorId);
        pcscSocket += ".sock";

        if(std::filesystem::is_socket(pcscSocket, err)) {
            std::filesystem::remove(pcscSocket, err);
        }

        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Pcsc, "");
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, pcscSocket.string());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "fast", 5, 0);

        // fix permissions job
        if(waitFileSetPermission(ioc_, pcscSocket, xvfb->userInfo->uid(), xvfb->userInfo->gid(), S_IRUSR | S_IWUSR)) {
            Application::info("{}: display: {}, user: {}, socket: `{}'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), pcscSocket);

            if(xvfb->dbusPcscChannelConnect(pcscSocket)) {
                return true;
            }

            // destroy channel
            auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, pcscSocket.string());
            auto clientUrl = Channel::createUrl(Channel::ConnectorType::Pcsc, "");
            emitDestroyListener(xvfb->displayNum, clientUrl, serverUrl);
            return false;
        }
        return false;
    }

    void DBusAdaptor::stopPcscListener(XvfbSessionPtr xvfb, const std::string & param) {
        Application::info("{}: param: `{}'", __FUNCTION__, param);
        auto pcscFolder = std::filesystem::path(Tools::replace(pcscRuntimeFmt, "%{user}", xvfb->userInfo->user()));
        auto pcscSocket = std::filesystem::path(pcscFolder) / "sock";

        Application::info("{}: display: {}, user: {}, socket: `{}'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), pcscSocket);

        xvfb->dbusPcscChannelDisconnect(pcscSocket);
    }

    bool DBusAdaptor::startPkcs11Listener(XvfbSessionPtr xvfb, const std::string & param) {
        if(xvfb->mode != SessionMode::Login) {
            Application::warning("{}: login session only, display: {}", __FUNCTION__, xvfb->displayNum);
            return false;
        }

        Application::info("{}: param: `{}'", __FUNCTION__, param);
            
        auto pkcs11Folder = std::filesystem::path{Tools::replace(pkcs11RuntimeFmt, "%{display}", xvfb->displayNum)};
        std::error_code err;

        if(! std::filesystem::is_directory(pkcs11Folder, err) &&
           ! std::filesystem::create_directories(pkcs11Folder, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                            __FUNCTION__, "create_directories", err.value(), err.message(), pkcs11Folder.string());
            return false;
        }

        // fix owner xvfb.user, mode 0750
        Tools::setFileOwner(pkcs11Folder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid(), 0750);
        // fixme path to ltsm_session_pkcs11
        auto pkcs11Socket = std::filesystem::path(pkcs11Folder) / "sock";

        if(std::filesystem::is_socket(pkcs11Socket, err)) {
            std::filesystem::remove(pkcs11Socket, err);
        }

        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Pkcs11, "");
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, pkcs11Socket.string());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "slow", 5,
                           static_cast<uint32_t>(Channel::OptsFlags::AllowLoginSession));
        // fix permissions job
        return waitFileSetPermission(ioc_, pkcs11Socket, xvfb->userInfo->uid(), xvfb->userInfo->gid(), S_IRUSR | S_IWUSR);
    }

    void DBusAdaptor::stopPkcs11Listener(XvfbSessionPtr xvfb, const std::string & param) {
        Application::info("{}: param: `{}'", __FUNCTION__, param);
    }

    bool startFuseSessionJob(DBusAdaptor* owner, XvfbSessionPtr xvfb, std::string localPoint, std::string remotePoint,
                             std::string fuseSocket) {
        Application::info("{}: display: {}, user: {}, local: `{}', remote: `{}', socket: `{}'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), localPoint, remotePoint, fuseSocket);

        if(! xvfb->dbusFuseMountPoint(localPoint, remotePoint, fuseSocket)) {
            auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, fuseSocket);
            auto clientUrl = Channel::createUrl(Channel::ConnectorType::Fuse, "");
            owner->emitDestroyListener(xvfb->displayNum, clientUrl, serverUrl);
            return false;
        }

        return true;
    }

    bool DBusAdaptor::startFuseListener(XvfbSessionPtr xvfb, const std::string & remotePoint) {
        if(xvfb->mode == SessionMode::Login) {
            Application::error("{}: login session skipped, display: {}", __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RemoteFilesUse)) {
            Application::warning("{}: display {}, redirect disabled: {}", __FUNCTION__, xvfb->displayNum, "fuse");
            sendNotifyCallAsync(xvfb, "Channel Disabled", "redirect " "drivers" " is blocked, contact the administrator",
                          NotifyParams::IconType::Warning);
            return false;
        }

        Application::info("{}: remote point: {}", __FUNCTION__, remotePoint);
        auto userShareFolder = std::filesystem::path{Tools::replace(fuseRuntimeFmt, "%{user}", xvfb->userInfo->user())};
        auto fusePointName = std::filesystem::path(remotePoint).filename();
        auto fusePointFolder = userShareFolder / fusePointName;
        std::error_code err;

        if(! std::filesystem::is_directory(fusePointFolder, err) &&
           ! std::filesystem::create_directories(fusePointFolder, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                            __FUNCTION__, "create_directories", err.value(), err.message(), fusePointFolder.string());
            return false;
        }

        // fix owner xvfb.user, mode 0750
        Tools::setFileOwner(userShareFolder.string(), Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid(), 0750);
        // fix owner user.user, mode 0700
        Tools::setFileOwner(fusePointFolder, xvfb->userInfo->uid(), xvfb->userInfo->gid(), 0700);

        auto fuseSocket = userShareFolder / fusePointName;
        fuseSocket += ".sock";

        if(std::filesystem::is_socket(fuseSocket, err)) {
            std::filesystem::remove(fuseSocket, err);
        }

        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Fuse, "");
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, fuseSocket.string());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "fast", 5, 0);

        // fix permissions job
        if(waitFileSetPermission(ioc_, fuseSocket, xvfb->userInfo->uid(), xvfb->userInfo->gid(), S_IRUSR | S_IWUSR)) {
            const auto & localPoint = fusePointFolder.string();

            Application::info("{}: display: {}, user: {}, local: `{}', remote: `{}', socket: `{}'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), localPoint, remotePoint, fuseSocket);

            if(xvfb->dbusFuseMountPoint(localPoint, remotePoint, fuseSocket)) {
                return true;
            }
    
            // destroy channel
            auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, fuseSocket.string());
            auto clientUrl = Channel::createUrl(Channel::ConnectorType::Fuse, "");
            emitDestroyListener(xvfb->displayNum, clientUrl, serverUrl);
            return false;
        }

        return false;
    }

    void DBusAdaptor::stopFuseListener(XvfbSessionPtr xvfb, const std::string & remotePoint) {
        auto userShareFolder = Tools::replace(fuseRuntimeFmt, "%{user}", xvfb->userInfo->user());
        auto fusePointName = std::filesystem::path(remotePoint).filename();
        auto fusePointFolder = std::filesystem::path(userShareFolder) / fusePointName;

        Application::info("{}: display: {}, user: {}, local point: `{}'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), fusePointFolder);

        xvfb->dbusFuseUmountPoint(fusePointFolder.string());
    }

    void DBusAdaptor::busSetDebugLevel(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", __FUNCTION__, level);
        Application::setDebugLevel(level);
    }

    void DBusAdaptor::busSetChannelDebug(const int32_t & display, const uint8_t & channel, const bool & debug) {
        Application::debug(DebugType::Dbus, "{}: display: {}, channel: {}, debug: {}",
                           __FUNCTION__, display, channel, (debug ? "true" : "false"));
        emitDebugChannel(display, channel, debug);
    }

    std::string DBusAdaptor::busEncryptionInfo(const int32_t & display) {
        Application::debug(DebugType::Dbus, "{}: display: {}",
                           __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display)) {
            return xvfb->encryption;
        }

        Application::warning("{}: display not found: {}", __FUNCTION__, display);
        return "none";
    }

    void DBusAdaptor::busDisplayResized(const int32_t & display, const uint16_t & width, const uint16_t & height) {
        Application::debug(DebugType::Dbus, "{}: display: {}, width: {}, height: {}",
                           __FUNCTION__, display, width, height);

        if(auto xvfb = findDisplaySession(display)) {
            xvfb->width = width;
            xvfb->height = height;
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetEncryptionInfo(const int32_t & display, const std::string & info) {
        Application::debug(DebugType::Dbus, "{}: display: {}, encryption: {}", __FUNCTION__, display, info);

        if(auto xvfb = findDisplaySession(display)) {
            xvfb->encryption = info;
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionLifetimeLimitSec(const int32_t & display, const uint32_t & limit) {
        Application::debug(DebugType::Dbus, "{}: display: {}, limit: {}", __FUNCTION__, display, limit);

        if(auto xvfb = findDisplaySession(display)) {
            xvfb->lifeTimeLimitSec = limit;
            emitClearRenderPrimitives(display);
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionOnlineLimitSec(const int32_t & display, const uint32_t & limit) {
        Application::debug(DebugType::Dbus, "{}: display: {}, limit: {}", __FUNCTION__, display, limit);

        if(auto xvfb = findDisplaySession(display)) {
            xvfb->onlineTimeLimitSec = limit;
            emitClearRenderPrimitives(display);
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionOfflineLimitSec(const int32_t & display, const uint32_t & limit) {
        Application::debug(DebugType::Dbus, "{}: display: {}, limit: {}", __FUNCTION__, display, limit);

        if(auto xvfb = findDisplaySession(display)) {
            xvfb->offlineTimeLimitSec = limit;
            emitClearRenderPrimitives(display);
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionIdleLimitSec(const int32_t & display, const uint32_t & limit) {
        Application::debug(DebugType::Dbus, "{}: display: {}, limit: {}", __FUNCTION__, display, limit);

        if(auto xvfb = findDisplaySession(display)) {
            xvfb->idleTimeLimitSec = limit;
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionPolicy(const int32_t & display, const std::string & policy) {
        Application::debug(DebugType::Dbus, "{}: display: {}, policy: {}", __FUNCTION__, display, policy);

        if(auto xvfb = findDisplaySession(display)) {
            if(Tools::lower(policy) == "authlock") {
                xvfb->policy = SessionPolicy::AuthLock;
            } else if(Tools::lower(policy) == "authtake") {
                xvfb->policy = SessionPolicy::AuthTake;
            } else if(Tools::lower(policy) == "authshare") {
                xvfb->policy = SessionPolicy::AuthShare;
            } else {
                Application::warning("{}: {}, display: {}, policy: {}", __FUNCTION__, "unknown value", display, policy);
            }
        } else {
            Application::warning("{}: display not found: {}", __FUNCTION__, display);
        }
    }

    void DBusAdaptor::helperSetSessionLoginPassword(const int32_t & display, const std::string & login,
            const std::string & password, const bool & action) {
        Application::info("{}: display: {}, user: {}", __FUNCTION__, display, login);

        asio::post(ioc_, [this, display, login, password, action]() {
            this->emitHelperSetLoginPassword(display, login, password, action);
        });
    }

    std::string DBusAdaptor::busGetSessionJson(const int32_t & display) {
        Application::debug(DebugType::Dbus, "{}: display: {}", __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display)) {
            return xvfb->toJsonString();
        }

        Application::warning("{}: display not found: {}", __FUNCTION__, display);
        return "{}";
    }

    std::string DBusAdaptor::busGetSessionsJson(void) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);
        return XvfbSessions::toJsonString();
    }

    void DBusAdaptor::busRenderRect(const int32_t & display, const TupleRegion & rect, const TupleColor & color, const bool & fill) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);

        asio::post(ioc_, [this, display, rect, color, fill]() {
            this->emitAddRenderRect(display, rect, color, fill);
        });
    }

    void DBusAdaptor::busRenderText(const int32_t & display, const std::string & text, const TuplePosition & pos, const TupleColor & color) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);

        asio::post(ioc_, [this, display, text, pos, color]() {
            this->emitAddRenderText(display, text, pos, color);
        });
    }

    void DBusAdaptor::busRenderClear(const int32_t & display) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);

        asio::post(ioc_, [this, display]() {
            this->emitClearRenderPrimitives(display);
        });
    }

    bool DBusAdaptor::busCreateChannel(const int32_t & display, const std::string & client, const std::string & cmode,
                                       const std::string & server, const std::string & smode, const std::string & speed) {
        Application::debug(DebugType::Dbus, "{}:, display: {}, client: ({}, {}), server: ({}, {}), speed: {}",
                           __FUNCTION__, display, client, cmode, server, smode, speed);

        auto modes = { "ro", "rw", "wo" };

        if(std::ranges::none_of(modes, [&](auto & val) { return cmode == val; })) {
            Application::error("{}: incorrect {} mode: {}", __FUNCTION__, "client", cmode);
            return false;
        }

        if(std::ranges::none_of(modes, [&](auto & val) { return smode == val; })) {
            Application::error("{}: incorrect {} mode: {}", __FUNCTION__, "server", smode);
            return false;
        }

        emitCreateChannel(display, client, cmode, server, smode, speed);
        return true;
    }

    bool DBusAdaptor::busDestroyChannel(const int32_t & display, const uint8_t & channel) {
        Application::debug(DebugType::Dbus, "{}:, display: {}, channel: {:#04x}",
                           __FUNCTION__, display, channel);

        emitDestroyChannel(display, channel);
        return true;
    }

    /* Manager::startService */
    int startService(int argc, const char** argv) {
        bool isBackground = false;
        std::filesystem::path confile;

        for(int it = 1; it < argc; ++it) {
            if(0 == std::strcmp(argv[it], "--background")) {
                isBackground = true;
            } else if(0 == std::strcmp(argv[it], "--config") && it + 1 < argc) {
                confile.assign(argv[it + 1]);
                it = it + 1;
            } else {
                std::cout << "usage: " << argv[0] << " [--config file] [--background]" << std::endl;
                return EXIT_SUCCESS;
            }
        }

        if(0 < getuid()) {
            std::cerr << "need root privileges" << std::endl;
            return EXIT_FAILURE;
        }

        if(isBackground) {
            Application::setDebugTarget(DebugTarget::Syslog, "ltsm_service");
            Application::setDebugLevel(DebugLevel::Info);
        
            if(fork()) {
                return EXIT_SUCCESS;
            }
            
            setsid();
            chdir("/");
        }

#ifdef SDBUS_2_0_API
        auto conn = sdbus::createSystemBusConnection(sdbus::ServiceName {LTSM::dbus_manager_service_name});
#else
        auto conn = sdbus::createSystemBusConnection(LTSM::dbus_manager_service_name);
#endif

        if(auto connectorHome = Tools::getUserHome(ltsm_user_conn);
                                std::filesystem::is_directory(connectorHome)) {
            // remove old sockets
            for(auto const & dirEntry : std::filesystem::directory_iterator{connectorHome}) {
                if(dirEntry.is_socket()) {
                    std::filesystem::remove(dirEntry);
                }
            }
        }

        auto dbus_conn = conn.get();
        const size_t concurency = 4;

        asio::io_context ctx{concurency};
        asio::thread_pool pool{concurency};

        signal(SIGPIPE, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        auto serviceAdaptor = std::make_unique<DBusAdaptor>(ctx, std::move(conn), confile);
        Application::notice("{}: service started, uid: {}, gid: {}, pid: {}, version: {}",
                __FUNCTION__, getuid(), getgid(), getpid(), LTSM::service_version);

        // sdbus background
        dbus_conn->enterEventLoopAsync();

#ifdef LTSM_WITH_SYSTEMD
        sd_notify(0, "READY=1");
#endif

        for(auto it = 0; it < concurency; ++it) {
            asio::post(pool, [&ctx](){ ctx.run(); });
        }

        pool.join();
        dbus_conn->leaveEventLoop();

#ifdef LTSM_WITH_SYSTEMD
        sd_notify(0, "STOPPING=1");
#endif
        Application::notice("{}: service stopped", __FUNCTION__);
        serviceAdaptor.reset();

        return EXIT_SUCCESS;
    }
}

int main(int argc, const char** argv) {
    int res = 0;

    try {
        res = LTSM::Manager::startService(argc, argv);
    } catch(const sdbus::Error & err) {
        LTSM::Application::error("sdbus: [{}] {}", err.getName(), err.getMessage());
    } catch(const std::exception & err) {
        LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    return res;
}
