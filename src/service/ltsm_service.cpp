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

#ifdef WITH_SYSTEMD
#include <systemd/sd-login.h>
#include <systemd/sd-daemon.h>
#endif

#include "ltsm_fuse.h"
#include "ltsm_pcsc.h"
#include "ltsm_audio.h"
#include "ltsm_tools.h"
#include "ltsm_pkcs11.h"
#include "ltsm_global.h"
#include "ltsm_sockets.h"
#include "ltsm_service.h"
#include "ltsm_channels.h"
#include "ltsm_xcb_wrapper.h"

using namespace std::chrono_literals;

namespace LTSM::Manager
{
    std::unique_ptr<sdbus::IConnection> serviceConn;
    std::unique_ptr<DBusAdaptor> serviceAdaptor;

    //
    std::forward_list<std::string> getSessionDBusAddresses(const UserInfo &, int displayNum);
    void redirectStdoutStderrTo(bool out, bool err, const std::filesystem::path &);
    void closefds(std::initializer_list<int> exclude);
    bool runSystemScript(XvfbSessionPtr, const std::string & cmd);
    bool switchToUser(const UserInfo &);

    void signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
        {
            if(serviceConn)
            {
                serviceConn->leaveEventLoop();
            }
        }
    }

    SessionPolicy sessionPolicy(const std::string & name)
    {
        if(name == "authlock")
        {
            return SessionPolicy::AuthLock;
        }

        if(name == "authtake")
        {
            return SessionPolicy::AuthTake;
        }

        if(name == "authshare")
        {
            return SessionPolicy::AuthShare;
        }

        return SessionPolicy::AuthTake;
    }

    /* PamService */
    PamService::~PamService()
    {
        if(pamh)
        {
            pam_end(pamh, status);
        }
    }

    std::string PamService::error(void) const
    {
        return pamh ? std::string(pam_strerror(pamh, status)) : "unknown";
    }

    pam_handle_t* PamService::get(void)
    {
        return pamh;
    }

    void PamService::setItem(int type, const std::string & str)
    {
        if(pamh)
        {
            pam_set_item(pamh, type, str.c_str());
        }
    }

    bool PamService::pamStart(const std::string & username)
    {
        status = pam_start(service.c_str(), username.c_str(), pamConv(), & pamh);

        if(PAM_SUCCESS != status)
        {
            if(pamh)
            {
                Application::error("%s: %s failed, error: %s, code: %" PRId32,
                                   __FUNCTION__, "pam_start", pam_strerror(pamh, status), status);
            }
            else
            {
                Application::error("%s: %s failed", __FUNCTION__, "pam_start");
            }

            return false;
        }

        return true;
    }

    /* PamAuthenticate */
    int PamAuthenticate::pam_conv_func(int num_msg, const struct pam_message** msg, struct pam_response** resp,
                                       void* appdata)
    {
        if(! appdata)
        {
            Application::error("%s: pam error: %s", __FUNCTION__, "empty data");
            return PAM_CONV_ERR;
        }

        if(! msg || ! resp)
        {
            Application::error("%s: pam error: %s", __FUNCTION__, "empty params");
            return PAM_CONV_ERR;
        }

        if(! *resp)
        {
            *resp = (struct pam_response*) calloc(num_msg, sizeof(struct pam_response));

            if(! *resp)
            {
                Application::error("%s: pam error: %s", __FUNCTION__, "buf error");
                return PAM_BUF_ERR;
            }
        }

        auto pamAuthenticate = static_cast<const PamAuthenticate*>(appdata);

        for(int ii = 0 ; ii < num_msg; ++ii)
        {
            auto pm = msg[ii];
            auto pr = resp[ii];

            if(pr->resp)
            {
                free(pr->resp);
            }

            pr->resp = pamAuthenticate->onPamPrompt(pm->msg_style, pm->msg);
            pr->resp_retcode = PAM_SUCCESS;
        }

        return PAM_SUCCESS;
    }

    char* PamAuthenticate::onPamPrompt(int style, const char* msg) const
    {
        switch(style)
        {
            case PAM_ERROR_MSG:
                Application::info("%s: style: `%s', msg: `%s'", __FUNCTION__, "PAM_ERROR_MSG", msg);
                break;

            case PAM_TEXT_INFO:
                Application::info("%s: style: `%s', msg: `%s'", __FUNCTION__, "PAM_TEXT_INFO", msg);
                break;

            case PAM_PROMPT_ECHO_ON:
                Application::info("%s: style: `%s', msg: `%s'", __FUNCTION__, "PAM_PROMPT_ECHO_ON", msg);

                //if(0 == strncasecmp(msg, "login:", 6));
                return strdup(login.c_str());

                break;

            case PAM_PROMPT_ECHO_OFF:
                Application::info("%s: style: `%s', msg: `%s'", __FUNCTION__, "PAM_PROMPT_ECHO_OFF", msg);

                //if(0 == strncasecmp(msg, "password:", 9));
                return strdup(password.c_str());

                break;

            default:
                break;
        }

        return nullptr;
    }

    bool PamAuthenticate::isAuthenticated(void) const
    {
        return authenticateSuccess;
    }

    bool PamAuthenticate::isLogin(std::string_view name) const
    {
        return login == name;
    }

    struct pam_conv* PamAuthenticate::pamConv(void)
    {
        return & pamc;
    }

    bool PamAuthenticate::authenticate(void)
    {
        status = pam_authenticate(pamh, 0);

        if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                               __FUNCTION__, "pam_authenticate", pam_strerror(pamh, status), status);
            return false;
        }

        authenticateSuccess = true;
        return true;
    }

    /* PamSession */
    PamSession::~PamSession()
    {
        if(sessionOpenned)
        {
            pam_close_session(pamh, 0);
        }

        pam_setcred(pamh, PAM_DELETE_CRED);
    }

    bool PamSession::validateAccount(void)
    {
        status = pam_acct_mgmt(pamh, 0);

        if(status == PAM_NEW_AUTHTOK_REQD)
        {
            status = pam_chauthtok(pamh, PAM_CHANGE_EXPIRED_AUTHTOK);

            if(PAM_SUCCESS != status)
            {
                Application::error("%s: %s failed, error: %s, code: %" PRId32,
                                   __FUNCTION__, "pam_chauthtok", pam_strerror(pamh, status), status);
                return false;
            }
        }
        else if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                               __FUNCTION__, "pam_acct_mgmt", pam_strerror(pamh, status), status);
            return false;
        }

        return true;
    }

    bool PamSession::refreshCreds(void)
    {
        status = pam_setcred(pamh, PAM_REFRESH_CRED);

        if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                               __FUNCTION__, "pam_setcred", pam_strerror(pamh, status), status);
            return false;
        }

        return true;
    }

    bool PamSession::openSession(void)
    {
        status = pam_setcred(pamh, PAM_ESTABLISH_CRED);

        if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                               __FUNCTION__, "pam_setcred", pam_strerror(pamh, status), status);
            return false;
        }

        status = pam_open_session(pamh, 0);

        if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                               __FUNCTION__, "pam_open_session", pam_strerror(pamh, status), status);
            return false;
        }

        sessionOpenned = true;
        return true;
    }

    bool PamSession::setCreds(const Cred & cred)
    {
        // PAM_ESTABLISH_CRED, PAM_REFRESH_CRED, PAM_REINITIALIZE_CRED
        status = pam_setcred(pamh, cred);

        if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                               __FUNCTION__, "pam_setcred", pam_strerror(pamh, status), status);
            return false;
        }

        return true;
    }

    std::forward_list<std::string> PamSession::getEnvList(void)
    {
        std::forward_list<std::string> list;

        if(auto envlist = pam_getenvlist(pamh))
        {
            for(auto env = envlist; *env; ++env)
            {
                list.emplace_front(*env);
                free(*env);
            }

            free(envlist);
        }

        return list;
    }

    /* XvfbSession */
    XvfbSession::~XvfbSession()
    {
        if(0 < pid2)
        {
            int status;
            // kill session
            Application::debug(DebugType::App, "%s: kill %s, pid: %" PRId32, "destroySession", "helper", pid2);
            kill(pid2, SIGTERM);
        }

        if(0 < pid1)
        {
            int status;
            // kill xvfb
            Application::debug(DebugType::App, "%s: kill %s, pid: %" PRId32, "destroySession", "xvfb", pid1);
            kill(pid1, SIGTERM);
        }

        try
        {
            // remove xautfile
            std::filesystem::remove(xauthfile);
        }
        catch(const std::filesystem::filesystem_error &)
        {
        }
    }

    std::string XvfbSession::toJsonString(void) const
    {
        JsonObjectStream jos;
        jos.push("displaynum", displayNum);
        jos.push("pid1", pid1);
        jos.push("pid2", pid2);
        jos.push("width", width);
        jos.push("height", height);
        jos.push("uid", userInfo->uid());
        jos.push("gid", userInfo->gid());
        jos.push("start:limit", startTimeLimitSec.load());
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

        if(mode == SessionMode::Connected)
        {
            jos.push("onlined:sec", sessionOnlinedSec().count());
        }
        else if(mode == SessionMode::Disconnected)
        {
            jos.push("offlined:sec", sessionOfflinedSec().count());
        }

        return jos.flush();
    }

    /* XvfbSessions */
    XvfbSessions::XvfbSessions(size_t displays)
    {
        sessions.resize(displays);
    }

    XvfbSessionPtr XvfbSessions::findUserSession(const std::string & username)
    {
        std::scoped_lock guard{ lockSessions };
        auto it = std::find_if(sessions.begin(), sessions.end(), [&](auto & ptr)
        {
            return ptr &&
                   (ptr->mode == SessionMode::Started || ptr->mode == SessionMode::Connected || ptr->mode == SessionMode::Disconnected) &&
                   username == ptr->userInfo->user();
        });

        return it != sessions.end() ? *it : nullptr;
    }

    XvfbSessionPtr XvfbSessions::findDisplaySession(int screen)
    {
        std::scoped_lock guard{ lockSessions };
        auto it = std::find_if(sessions.begin(), sessions.end(), [& screen](auto & ptr)
        {
            return ptr && ptr->displayNum == screen;
        });

        return it != sessions.end() ? *it : nullptr;
    }

    std::forward_list<XvfbSessionPtr> XvfbSessions::findTimepointLimitSessions(void)
    {
        std::forward_list<XvfbSessionPtr> res;
        std::scoped_lock guard{ lockSessions };

        for(const auto & ptr : sessions)
        {
            if(ptr && (0 < ptr->startTimeLimitSec || 0 < ptr->onlineTimeLimitSec || 0 < ptr->offlineTimeLimitSec))
            {
                res.push_front(ptr);
            }
        }

        return res;
    }

    std::forward_list<XvfbSessionPtr> XvfbSessions::getOnlineSessions(void)
    {
        std::forward_list<XvfbSessionPtr> res;
        std::scoped_lock guard{ lockSessions };

        for(const auto & ptr : sessions)
        {
            if(ptr && ptr->mode == SessionMode::Connected)
            {
                res.push_front(ptr);
            }
        }

        return res;
    }

    void XvfbSessions::removeDisplaySession(int screen)
    {
        std::scoped_lock guard{ lockSessions };
        auto it = std::find_if(sessions.begin(), sessions.end(), [& screen](auto & ptr)
        {
            return ptr && ptr->displayNum == screen;
        });

        if(it != sessions.end())
        {
            (*it).reset();
        }
    }

    XvfbSessionPtr XvfbSessions::registryNewSession(int min, int max)
    {
        if(max < min)
        {
            std::swap(max, min);
        }

        std::scoped_lock guard{ lockSessions };
        auto freeDisplay = min;

        for(; freeDisplay <= max; ++freeDisplay)
        {
            if(std::none_of(sessions.begin(), sessions.end(), [&](auto & ptr) { return ptr && ptr->displayNum == freeDisplay; }))
            {
                break;
            }
        }

        if(freeDisplay <= max)
        {
            auto it = std::find_if(sessions.begin(), sessions.end(),
                                   [](auto & ptr)
            {
                return ! ptr;
            });

            if(it != sessions.end())
            {
                (*it) = std::make_shared<XvfbSession>();
                (*it)->displayNum = freeDisplay;
            }

            return *it;
        }

        return nullptr;
    }

    std::string XvfbSessions::toJsonString(void) const
    {
        JsonArrayStream jas;
        std::scoped_lock guard{ lockSessions };

        for(const auto & ptr : sessions)
        {
            if(ptr)
            {
                jas.push(ptr->toJsonString());
            }
        }

        return jas.flush();
    }

    std::forward_list<std::string> getSessionDBusAddressesFromHome(const std::filesystem::path & dbusSessionPath, int displayNum)
    {
        std::forward_list<std::string> res;

        std::string_view dbusLabel = "DBUS_SESSION_BUS_ADDRESS='";
        auto displaySuffix = Tools::joinToString("-", displayNum);

        for(auto const & dirEntry : std::filesystem::directory_iterator{dbusSessionPath})
        {
            if(! endsWith(dirEntry.path().native(), displaySuffix))
            {
                continue;
            }

            std::ifstream ifs(dirEntry.path());
            std::string line;

            while(std::getline(ifs, line))
            {
                if(! startsWith(line, dbusLabel))
                {
                    continue;
                }

                auto it1 = line.begin() + dbusLabel.size();
                auto it2 = std::prev(line.end());

                // remove last \'
                while(std::iscntrl(*it2) || *it2 == '\'')
                {
                    it2 = std::prev(it2);
                }

                res.emplace_front(it1, it2);
            }
        }

        return res;
    }

    std::forward_list<std::string> getSessionDBusAddresses(const UserInfo & userInfo, int displayNum)
    {
        auto dbusSessionPath = std::filesystem::path(userInfo.home()) / ".dbus" / "session-bus";
        std::forward_list<std::string> dbusAddresses;

        // home may be nfs and deny for root
        try
        {
            if(std::filesystem::is_directory(dbusSessionPath))
            {
                dbusAddresses = getSessionDBusAddressesFromHome(dbusSessionPath, displayNum);
            }

            auto dbusBrokerPath = userInfo.xdgRuntimeDir() / "bus";

            if(std::filesystem::is_socket(dbusBrokerPath))
            {
                dbusAddresses.emplace_front(Tools::joinToString("unix:path=", dbusBrokerPath.native()));
            }

            // ltsm path from /etc/ltsm/xclients
            auto dbusLtsmSessionPath = userInfo.xdgRuntimeDir() / "ltsm" / Tools::joinToString("dbus_session_", displayNum);

            if(std::filesystem::is_regular_file(dbusLtsmSessionPath))
            {
                dbusAddresses.emplace_front(Tools::fileToString(dbusLtsmSessionPath));
            }
        }
        catch(const std::filesystem::filesystem_error &)
        {
        }

        return dbusAddresses;
    }

    void redirectStdoutStderrTo(bool out, bool err, const std::filesystem::path & file)
    {
        auto dir = file.parent_path();
        std::error_code fserr;

        if(! std::filesystem::is_directory(dir, fserr))
        {
            std::filesystem::create_directories(dir, fserr);
        }

        int fd = open(file.c_str(), O_RDWR | O_CREAT, 0640);

        if(0 <= fd)
        {
            if(out)
            {
                dup2(fd, STDOUT_FILENO);
            }

            if(err)
            {
                dup2(fd, STDERR_FILENO);
            }

            close(fd);
        }
        else
        {
            const char* devnull = "/dev/null";
            Application::warning("%s: %s, path: `%s', uid: %" PRId32, __FUNCTION__, "open failed", file.c_str(), getuid());

            if(file != devnull)
            {
                redirectStdoutStderrTo(out, err, devnull);
            }
        }
    }

    void closefds(std::initializer_list<int> exclude)
    {
        std::vector<int> pids;
        pids.reserve(255);

        auto fdpath = std::filesystem::path("/proc") / std::to_string(getpid()) / "fd";

        if(std::filesystem::is_directory(fdpath))
        {
            // read all pids first
            for(auto const & dirEntry : std::filesystem::directory_iterator{fdpath})
            {
                try
                {
                    pids.push_back(std::stoi(dirEntry.path().filename()));
                }
                catch(...)
                {
                    continue;
                }
            }
        }
        else
        {
            Application::warning("%s: path not found: `%s'", __FUNCTION__, fdpath.c_str());

            pids.resize(255);
            std::iota(pids.begin(), pids.end(), 0);
        }

        for(auto fd : pids)
        {
            if(std::any_of(exclude.begin(), exclude.end(), [&](auto & val) { return val == fd; }))
            continue;

            close(fd);
        }
    }

    bool runSystemScript(XvfbSessionPtr xvfb, const std::string & cmd)
    {
        if(cmd.empty())
        {
            return false;
        }

        std::error_code err;

        if(! std::filesystem::exists(cmd.substr(0, cmd.find(0x20)), err))
        {
            Application::warning("%s: %s, path: `%s'", __FUNCTION__, err.message().c_str(), cmd.c_str());
            return false;
        }

        auto str = Tools::replace(cmd, "%{display}", xvfb->displayNum);
        str = Tools::replace(str, "%{user}", xvfb->userInfo->user());

        std::thread([ptr = std::move(xvfb), str]()
        {
            int ret = std::system(str.c_str());
            Application::debug(DebugType::App, "%s: command: `%s', return code: %" PRId32 ", display: %" PRId32,
                               "runSystemScript", str.c_str(), ret, ptr->displayNum);
        }).detach();

        return true;
    }

    bool switchToUser(const UserInfo & userInfo)
    {
        Application::debug(DebugType::App, "%s: pid: %" PRId32 ", uid: %" PRId32 ", gid: %" PRId32 ", home:`%s', shell: `%s'",
                           __FUNCTION__, getpid(), userInfo.uid(), userInfo.gid(), userInfo.home(), userInfo.shell());
        auto xdgRuntimeDir = userInfo.xdgRuntimeDir();
        std::error_code err;

        if(! std::filesystem::exists(xdgRuntimeDir, err))
        {
            std::filesystem::create_directories(xdgRuntimeDir, err);
        }

        if(std::filesystem::exists(xdgRuntimeDir, err))
        {
            // fix mode 0700
            std::filesystem::permissions(xdgRuntimeDir, std::filesystem::perms::group_all | std::filesystem::perms::others_all,
                                         std::filesystem::perm_options::remove, err);
            // fix owner
            Tools::setFileOwner(xdgRuntimeDir, userInfo.uid(), userInfo.gid());
        }

        // set groups
        auto gids = userInfo.groups();

        if(! gids.empty())
        {
            setgroups(gids.size(), gids.data());
        }

        if(0 != setgid(userInfo.gid()))
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "setgid", strerror(errno), errno);
            return false;
        }

        if(0 != setuid(userInfo.uid()))
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "setuid", strerror(errno), errno);
            return false;
        }

        if(0 != chdir(userInfo.home()))
        {
            Application::warning("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                                 __FUNCTION__, "chdir", strerror(errno), errno, userInfo.home());
        }

        setenv("USER", userInfo.user(), 1);
        setenv("LOGNAME", userInfo.user(), 1);
        setenv("HOME", userInfo.home(), 1);
        setenv("SHELL", userInfo.shell(), 1);
        setenv("TERM", "linux", 1);

        if(Application::isDebugLevel(DebugLevel::Debug))
        {
            auto cwd = std::filesystem::current_path();
            auto sgroups = Tools::join(gids.begin(), gids.end(), ",");
            Application::debug(DebugType::App, "%s: groups: (%s), current dir: `%s'", __FUNCTION__, sgroups.c_str(), cwd.c_str());
        }

        return true;
    }

    /// RunAs namespace
    namespace RunAs
    {
        int waitPid(pid_t pid)
        {
            Application::debug(DebugType::App, "%s: pid: %" PRId32, __FUNCTION__, pid);
            // waitpid
            int status;
            int ret = waitpid(pid, &status, 0);

            if(0 > ret)
            {
                Application::error("%s: %s failed, error: %s, code: %" PRId32,
                                   __FUNCTION__, "waitpid", strerror(errno), errno);
            }
            else if(WIFSIGNALED(status))
            {
                Application::error("%s: process killed, pid: %" PRId32, __FUNCTION__, pid);
            }
            else
            {
                Application::debug(DebugType::App, "%s: process ended, pid: %" PRId32 ", status: %" PRId32,
                                   __FUNCTION__, pid, status);
            }

            return status;
        }

        void runChildProcess(XvfbSessionPtr xvfb, int pipeout, const std::filesystem::path & cmd,
                             std::list<std::string> params)
        {
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);

            Application::info("%s: pid: %" PRId32 ", cmd: `%s %s'",
                              __FUNCTION__, getpid(), cmd.c_str(), Tools::join(params.begin(), params.end(), " ").c_str());

            if(switchToUser(*xvfb->userInfo))
            {
                for(const auto & [key, val] : xvfb->environments)
                {
                    setenv(key.c_str(), val.c_str(), 1);
                }

                setenv("XAUTHORITY", xvfb->xauthfile.c_str(), 1);
                setenv("DISPLAY", xvfb->displayAddr.c_str(), 1);
                setenv("LTSM_REMOTEADDR", xvfb->remoteAddr.c_str(), 1);
                setenv("LTSM_TYPECONN", xvfb->conntype.c_str(), 1);
                std::vector<const char*> argv;
                argv.reserve(128);
                // create argv[]
                argv.push_back(cmd.c_str());

                for(const auto & val : params)
                {
                    if(! val.empty())
                    {
                        argv.push_back(val.c_str());
                    }
                }

                argv.push_back(nullptr);
                // errlog folder
                auto ltsmLogFolder = std::filesystem::path(xvfb->userInfo->home()) / ".ltsm" / "log";
                std::error_code fserr;

                if(! std::filesystem::is_directory(ltsmLogFolder, fserr))
                {
                    std::filesystem::create_directory(ltsmLogFolder, fserr);
                }

                auto logFile = ltsmLogFolder / cmd.filename();
                logFile.replace_extension(".log");

                if(0 > pipeout)
                {
                    // redirect stdout, atderr
                    redirectStdoutStderrTo(true, true, logFile.native());
                }
                else
                {
                    // redirect stderr
                    redirectStdoutStderrTo(false, true, logFile.native());

                    // redirect stdout
                    if(0 > dup2(pipeout, STDOUT_FILENO))
                    {
                        Application::warning("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "dup2", strerror(errno), errno);
                    }

                    close(pipeout);
                    pipeout = -1;
                }

                closefds({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, pipeout});
                int res = execv(cmd.c_str(), (char* const*) argv.data());

                if(res < 0)
                {
                    Application::error("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                                       __FUNCTION__, "execv", strerror(errno), errno, cmd.c_str());
                }
            }
        }

        StatusStdout jobWaitStdout(pid_t pid, int fd)
        {
            bool error = false;
            bool loop = true;
            const size_t block = 1024;
            std::vector<uint8_t> res(block);
            uint8_t* ptr = res.data();
            size_t last = block;

            while(loop && ! error)
            {
                int ret = read(fd, ptr, last);

                if(ret < 0)
                {
                    if(EAGAIN != errno && EINTR != errno)
                    {
                        Application::error("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "read", strerror(errno), errno);
                        error = true;
                    }

                    // EAGAIN or EINTR continue
                    continue;
                }

                // end stream
                if(ret == 0)
                {
                    res.resize(res.size() - last);
                    loop = false;
                    continue;
                }

                ptr += ret;
                last -= ret;

                if(last == 0)
                {
                    auto pos = res.size();
                    res.resize(res.size() + block);
                    last = block;
                    ptr = res.data() + pos;
                }
            }

            if(error)
            {
                res.clear();
            }

            int status = waitPid(pid);
            return std::make_pair(status, std::move(res));
        }

        PidStatusStdout runSessionCommandStdout(XvfbSessionPtr xvfb, const std::filesystem::path & cmd,
                                                std::list<std::string> params)
        {
            if(! xvfb)
            {
                Application::error("%s: xvfb session null", __FUNCTION__);
                throw service_error(NS_FuncName);
            }

            std::error_code err;

            if(! std::filesystem::exists(cmd, err))
            {
                Application::error("%s: %s, path: `%s', uid: %" PRId32,
                                   __FUNCTION__, (err ? err.message().c_str() : "not found"), cmd.c_str(), getuid());
                throw service_error(NS_FuncName);
            }

            Application::info("%s: request for user: %s, display: %" PRId32 ", cmd: `%s'",
                              __FUNCTION__, xvfb->userInfo->user(), xvfb->displayNum, cmd.c_str());

            if(! std::filesystem::is_directory(xvfb->userInfo->home(), err))
            {
                Application::error("%s: %s, path: `%s', uid: %" PRId32,
                                   __FUNCTION__, (err ? err.message().c_str() : "not directory"), xvfb->userInfo->home(), getuid());
                throw service_error(NS_FuncName);
            }

            int pipefd[2];

            if(0 > pipe(pipefd))
            {
                Application::error("%s: %s failed, error: %s, code: %" PRId32,
                                   __FUNCTION__, "pipe", strerror(errno), errno);
                throw service_error(NS_FuncName);
            }

            pid_t pid = Application::forkMode();

            if(0 == pid)
            {
                close(pipefd[0]);
                runChildProcess(xvfb, pipefd[1], cmd, std::move(params));
                // child ended
                std::exit(0);
            }

            // main thread processed
            close(pipefd[1]);

            if(0 > fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK))
            {
                Application::error("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "fcntl", strerror(errno), errno);
            }

            // planned get stdout from running job
            auto future = std::async(std::launch::async, & RunAs::jobWaitStdout, pid, pipefd[0]);
            return std::make_pair(pid, std::move(future));
        }

        PidStatus runSessionCommand(XvfbSessionPtr xvfb, const std::filesystem::path & cmd, std::list<std::string> params)
        {
            if(! xvfb)
            {
                Application::error("%s: xvfb session null", __FUNCTION__);
                throw service_error(NS_FuncName);
            }

            std::error_code err;

            if(! std::filesystem::exists(cmd, err))
            {
                Application::error("%s: %s, path: `%s', uid: %" PRId32,
                                   __FUNCTION__, (err ? err.message().c_str() : "not found"), cmd.c_str(), getuid());
                throw service_error(NS_FuncName);
            }

            Application::info("%s: request for: %s, display: %" PRId32 ", cmd: `%s %s'",
                              __FUNCTION__, xvfb->userInfo->user(), xvfb->displayNum, cmd.c_str(), Tools::join(params.begin(), params.end(), " ").c_str());

            if(! std::filesystem::is_directory(xvfb->userInfo->home(), err))
            {
                Application::error("%s: %s, path: `%s', uid: %" PRId32,
                                   __FUNCTION__, (err ? err.message().c_str() : "not directory"), xvfb->userInfo->home(), getuid());
                throw service_error(NS_FuncName);
            }

            pid_t pid = Application::forkMode();

            if(0 == pid)
            {
                runChildProcess(xvfb, -1, cmd, std::move(params));
                // child ended
                std::exit(0);
            }

            // main thread processed
            auto future = std::async(std::launch::async, [pid]()
            {
                Application::debug(DebugType::App, "%s: pid: %" PRId32, "AsyncWaitPid", pid);

                int status;
                // waitpid
                int ret = waitpid(pid, & status, 0);

                if(0 > ret && errno != ECHILD)
                {
                    Application::error("%s: %s failed, error: %s, code: %" PRId32, "AsyncWaitPid", "waitpid", strerror(errno), errno);
                }

                return status;
            });
            return std::make_pair(pid, std::move(future));
        }

    }; // RunAs

    /* DBusAdaptor */
    DBusAdaptor::DBusAdaptor(sdbus::IConnection & conn, const std::string & confile)
        : ApplicationJsonConfig("ltsm_service", confile)
#ifdef SDBUS_2_0_API
        , AdaptorInterfaces(conn, sdbus::ObjectPath {LTSM::dbus_manager_service_path})
#else
        , AdaptorInterfaces(conn, LTSM::dbus_manager_service_path)
#endif
        , XvfbSessions(300)
    {
        checkStartConfig();
        createXauthDir();

        // registry
        registerAdaptor();
        // check sessions timepoint limit
        timer1 = Tools::BaseTimer::create<std::chrono::seconds>(3, true,
                 std::bind(& DBusAdaptor::sessionsTimeLimitAction, this));

        // check sessions killed
        timer2 = Tools::BaseTimer::create<std::chrono::seconds>(1, true,
                 std::bind(& DBusAdaptor::sessionsEndedAction, this));

        // check sessions alive
        timer3 = Tools::BaseTimer::create<std::chrono::seconds>(20, true,
                 std::bind(& DBusAdaptor::sessionsCheckConnectedAction, this));

        inotifyWatchStart();
    }

    DBusAdaptor::~DBusAdaptor()
    {
        unregisterAdaptor();
    }

    void DBusAdaptor::checkStartConfig(void)
    {
        // check present executable files
        for(const auto & key : config().keys())
        {
            // only for path
            if(startsWith(key, "#") || ! endsWith(key, ":path"))
            {
                continue;
            }

            // skip comment
            if(0 == std::ispunct(key.front()))
            {
                continue;
            }

            if(auto value = configGetString(key);
                    ! std::filesystem::exists(value))
            {
                Application::warning("%s: path not found: `%s'", "CheckProgram", value.c_str());
            }
        }

        int min = configGetInteger("display:min", 55);
        int max = configGetInteger("display:max", 99);

        size_t poolsz = std::abs(max - min);

        if(poolsz > sessions.size())
        {
            std::scoped_lock guard{ lockSessions };
            sessions.resize(poolsz);
        }
    }

    void DBusAdaptor::shutdownService(void)
    {
        busShutdownService();
    }

    void DBusAdaptor::configReloadedEvent(void)
    {
        int min = configGetInteger("display:min", 55);
        int max = configGetInteger("display:max", 99);
        size_t poolsz = std::abs(max - min);

        if(poolsz > sessions.size())
        {
            std::scoped_lock guard{ lockSessions };
            sessions.resize(poolsz);
        }

        Application::notice("%s: success", __FUNCTION__);
    }

    void DBusAdaptor::sessionsTimeLimitAction(void)
    {
        for(const auto & ptr : findTimepointLimitSessions())
        {
            uint32_t lastSecStarted = UINT32_MAX;
            uint32_t lastSecOnlined = UINT32_MAX;

            // check started timepoint
            if(0 < ptr->startTimeLimitSec)
            {
                auto startedSec = ptr->sessionStartedSec();

                if(startedSec.count() > ptr->startTimeLimitSec)
                {
                    Application::notice("%s: %s limit, display: %" PRId32 ", limit: %" PRIu32 "sec, session alive: %" PRIu64 "sec",
                                        __FUNCTION__, "started", ptr->displayNum, static_cast<uint32_t>(ptr->startTimeLimitSec), startedSec.count());
                    displayShutdown(ptr, true);
                    continue;
                }

                if(ptr->mode != SessionMode::Login)
                {
                    lastSecStarted = ptr->startTimeLimitSec - startedSec.count();
                }
            }

            // check online timepoint
            if(ptr->mode == SessionMode::Connected && 0 < ptr->onlineTimeLimitSec)
            {
                auto onlinedSec = ptr->sessionOnlinedSec();

                if(onlinedSec.count() > ptr->onlineTimeLimitSec)
                {
                    Application::notice("%s: %s limit, display: %" PRId32 ", limit: %" PRIu32 "sec, session alive: %" PRIu64 "sec",
                                        __FUNCTION__, "online", ptr->displayNum, static_cast<uint32_t>(ptr->onlineTimeLimitSec), onlinedSec.count());
                    emitShutdownConnector(ptr->displayNum);
                    continue;
                }

                uint32_t lastSecOnlined = ptr->onlineTimeLimitSec - onlinedSec.count();
            }

            // check offline timepoint
            if(ptr->mode == SessionMode::Disconnected && 0 < ptr->offlineTimeLimitSec)
            {
                auto offlinedSec = ptr->sessionOfflinedSec();

                if(offlinedSec.count() > ptr->offlineTimeLimitSec)
                {
                    Application::notice("%s: %s limit, display: %" PRId32 ", limit: %" PRIu32 "sec, session alive: %" PRIu64 "sec",
                                        __FUNCTION__, "offline", ptr->displayNum, static_cast<uint32_t>(ptr->offlineTimeLimitSec), offlinedSec.count());
                    displayShutdown(ptr, true);
                    continue;
                }
            }

            if(auto lastsec = std::min(lastSecStarted, lastSecOnlined); lastsec < UINT32_MAX)
            {
                // inform alert
                if(100 > lastsec)
                {
                    emitClearRenderPrimitives(ptr->displayNum);
                    // send render rect
                    const uint16_t fw = ptr->width;
                    const uint16_t fh = 24;
                    emitAddRenderRect(ptr->displayNum, {0, 0, fw, fh}, {0x10, 0x17, 0x80}, true);
                    // send render text
                    const char* type = lastSecStarted < lastSecOnlined ? "Session limit - " : "Onlined limit - ";
                    auto text = Tools::joinToString(type, "time left: ", lastsec, "sec");
                    const int16_t px = (fw - text.size() * 8) / 2;
                    const int16_t py = (fh - 16) / 2;
                    emitAddRenderText(ptr->displayNum, text, {px, py}, {0xFF, 0xFF, 0});
                }

                // inform beep
                if(10 > lastsec)
                {
                    emitSendBellSignal(ptr->displayNum);
                }
            }
        }
    }

    void DBusAdaptor::sessionsEndedAction(void)
    {
        std::scoped_lock guard{ lockSessions, lockRunning };

        // childEnded
        if(childsRunning.empty())
        {
            return;
        }

        childsRunning.remove_if([this](auto & pidStatus)
        {
            if(pidStatus.second.wait_for(std::chrono::milliseconds(3)) != std::future_status::ready)
            {
                return false;
            }

            // find child
            auto it = std::find_if(this->sessions.begin(), this->sessions.end(), [pid2 = pidStatus.first](auto & ptr)
            {
                return ptr && ptr->pid2 == pid2;
            });

            if(it != this->sessions.end() && *it)
            {
                auto & ptr = *it;
                auto res = pidStatus.second.get();
                ptr->pid2 = 0;
                Application::notice("%s: helper ended, display: %" PRId32 ", ret: %" PRId32,
                                        "sessionsEndedAction", ptr->displayNum, pidStatus.second.get());
                this->displayShutdown(ptr, true);
            }

            return true;
        });
    }

    void DBusAdaptor::sessionsCheckConnectedAction(void)
    {
        for(const auto & ptr : getOnlineSessions())
        {
            // check alive connectors
            if(! ptr->checkStatus(Flags::SessionStatus::CheckConnection))
            {
                ptr->setStatus(Flags::SessionStatus::CheckConnection);
                emitPingConnector(ptr->displayNum);
            }
            else
            {
                // not reply
                Application::warning("connector not reply, display: %" PRId32 ", connector id: %" PRId32, ptr->displayNum, ptr->connectorId);
                // complete shutdown
                busConnectorTerminated(ptr->displayNum, -1);
            }
        }
    }

    bool DBusAdaptor::checkXvfbSocket(int display) const
    {
        return 0 < display ?
               Tools::checkUnixSocket(Tools::x11UnixPath(display)) : false;
    }

    void DBusAdaptor::removeXvfbSocket(int display) const
    {
        if(0 < display)
        {
            std::filesystem::path socketPath = Tools::x11UnixPath(display);

            try
            {
                std::filesystem::remove(socketPath);
            }
            catch(const std::filesystem::filesystem_error &)
            {
            }
        }
    }

    bool DBusAdaptor::displayShutdown(XvfbSessionPtr xvfb, bool emitSignal)
    {
        if(! xvfb || xvfb->mode == SessionMode::Shutdown)
        {
            return false;
        }

        Application::notice("%s: shutdown display: %" PRId32 " %s", __FUNCTION__, xvfb->displayNum, "starting");
        xvfb->mode = SessionMode::Shutdown;

        if(emitSignal)
        {
            emitShutdownConnector(xvfb->displayNum);
        }

        // dbus no wait, remove background
        bool notSysUser = std::string_view(ltsm_user_conn) != xvfb->userInfo->user();

        if(notSysUser)
        {
            closeSystemSession(xvfb);
        }

        // script run in thread
        std::thread([wait = emitSignal, ptr = std::move(xvfb), notsys = notSysUser, this]()
        {
            if(wait)
            {
                std::this_thread::sleep_for(300ms);
            }

            auto displayNum = ptr->displayNum;

            if(notsys)
            {
                runSystemScript(std::move(ptr), configGetString("system:logoff"));
            }

            this->removeDisplaySession(displayNum);
            this->removeXvfbSocket(displayNum);
            this->emitDisplayRemoved(displayNum);
            Application::debug(DebugType::App, "%s: shutdown display: %" PRId32 " %s", "displayShutdown", displayNum, "complete");
        }).detach();

        return true;
    }

    void DBusAdaptor::closeSystemSession(XvfbSessionPtr xvfb)
    {
        Application::info("%s: user: %s, display: %" PRId32, __FUNCTION__, xvfb->userInfo->user(), xvfb->displayNum);
        runSessionScript(xvfb, configGetString("session:disconnect"));
        // PAM close
        xvfb->pam.reset();
        // unreg sessreg
        runSystemScript(std::move(xvfb), configGetString("system:disconnect"));
    }

    bool DBusAdaptor::waitXvfbStarting(int display, const std::vector<uint8_t> & cookie, uint32_t ms) const
    {
        if(0 >= display)
        {
            return false;
        }

        return Tools::waitCallable<std::chrono::milliseconds>(ms, 100, [this, display, auth = std::addressof(cookie)]()
        {
            if(this->checkXvfbSocket(display))
            {
                try
                {
                    if(auto xcb = std::make_unique<XCB::Connector>(display, auth))
                    {
                        return 0 == xcb->hasError();
                    }
                }
                catch(const std::exception &)
                {
                }
            }

            return false;
        });
    }

    std::filesystem::path DBusAdaptor::createXauthFile(int displayNum, const std::vector<uint8_t> & mcookie)
    {
        std::string xauthFileTemplate = configGetString("xauth:file", "/var/run/ltsm/auth_%{display}");
        xauthFileTemplate = Tools::replace(xauthFileTemplate, "%{pid}", getpid());
        xauthFileTemplate = Tools::replace(xauthFileTemplate, "%{display}", displayNum);
        std::filesystem::path xauthFilePath(xauthFileTemplate);
        Application::debug(DebugType::App, "%s: path: `%s'", __FUNCTION__, xauthFilePath.c_str());
        std::ofstream ofs(xauthFilePath, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);

        if(ofs)
        {
            // create xautfile
            auto host = Tools::getHostname();
            auto display = std::to_string(displayNum);
            std::string_view magic{"MIT-MAGIC-COOKIE-1"};
            StreamBuf sb;
            // format: 01 00 [ <host len:be16> [ host ]] [ <display len:be16> [ display ]] [ <magic len:be16> [ magic ]] [ <cookie len:be16> [ cookie ]]
            sb.writeInt8(1);
            sb.writeInt8(0);
            sb.writeIntBE16(host.size());
            sb.write(host);
            sb.writeIntBE16(display.size());
            sb.write(display);
            sb.writeIntBE16(magic.size());
            sb.write(magic);
            sb.writeIntBE16(mcookie.size());
            sb.write(mcookie);
            auto & rawbuf = sb.rawbuf();
            ofs.write((const char*) rawbuf.data(), rawbuf.size());
            ofs.close();
        }
        else
        {
            Application::error("%s: create xauthfile failed, path: `%s'", __FUNCTION__, xauthFilePath.c_str());
            return "";
        }

        std::error_code err;

        if(! std::filesystem::exists(xauthFilePath, err))
        {
            return "";
        }

        // set permissons 0440
        std::filesystem::permissions(xauthFilePath, std::filesystem::perms::owner_read |
                                     std::filesystem::perms::group_read, std::filesystem::perm_options::replace, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), xauthFilePath.c_str(), getuid());
        }

        return xauthFilePath;
    }

    bool DBusAdaptor::createSessionConnInfo(XvfbSessionPtr xvfb, bool destroy)
    {
        auto ltsmInfo = xvfb->userInfo->xdgRuntimeDir() / "ltsm" / Tools::joinToString("conninfo_", xvfb->displayNum);
        auto dir = ltsmInfo.parent_path();
        std::error_code err;

        if(! std::filesystem::is_directory(dir, err))
        {
            if(! std::filesystem::create_directory(dir, err))
            {
                Application::error("%s: %s, path: `%s', uid: %" PRId32,
                                   __FUNCTION__, (err ? err.message().c_str() : "create failed"), dir.c_str(), getuid());
                return false;
            }
        }

        // set permissions 0750
        std::filesystem::permissions(dir, std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_all, std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), dir.c_str(), getuid());
        }

        std::filesystem::remove(ltsmInfo);
        std::ofstream ofs(ltsmInfo, std::ofstream::trunc);

        if(! ofs)
        {
            Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "create file", ltsmInfo.c_str());
            return false;
        }

        ofs << "LTSM_REMOTEADDR" << "=" << (destroy ? "" : xvfb->remoteAddr) << std::endl <<
               "LTSM_TYPECONN" << "=" << (destroy ? "" : xvfb->conntype) << std::endl;
        ofs.close();
        Tools::setFileOwner(ltsmInfo, xvfb->userInfo->uid(), xvfb->userInfo->gid());
        return true;
    }

    pid_t DBusAdaptor::runSessionCommandSafe(XvfbSessionPtr xvfb, const std::filesystem::path & cmd,
            std::list<std::string> params)
    {
        std::error_code err;

        if(! std::filesystem::exists(cmd, err))
        {
            Application::warning("%s: path not found: `%s'", __FUNCTION__, cmd.c_str());
            return 0;
        }

        try
        {
            std::scoped_lock guard{ lockRunning };
            childsRunning.emplace_front(
                RunAs::runSessionCommand(std::move(xvfb), cmd, std::move(params)));
            return childsRunning.front().first;
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

        return 0;
    }

    void DBusAdaptor::waitPidBackgroundSafe(pid_t pid)
    {
        // create wait pid task
        std::packaged_task<int(pid_t)> waitPidTask{& RunAs::waitPid};
        std::scoped_lock guard{ lockRunning };
        childsRunning.emplace_front(std::make_pair(pid, waitPidTask.get_future()));
        std::thread(std::move(waitPidTask), pid).detach();
    }

    void DBusAdaptor::runSessionScript(XvfbSessionPtr xvfb, const std::string & cmd)
    {
        if(! cmd.empty())
        {
            auto params = Tools::split(Tools::replace(
                                           Tools::replace(cmd, "%{display}", xvfb->displayNum), "%{user}", xvfb->userInfo->user()), 0x20);

            if(! params.empty())
            {
                auto bin = params.front();
                params.pop_front();
                runSessionCommandSafe(std::move(xvfb), bin, std::move(params));
            }
        }
    }

    XvfbSessionPtr DBusAdaptor::runXvfbDisplayNewSession(uint8_t depth, uint16_t width, uint16_t height,
            UserInfoPtr userInfo)
    {
        std::scoped_lock guard{ lockSessions };
        auto its = std::find_if(sessions.begin(), sessions.end(), [](auto & ptr)
        {
            return ! ptr;
        });

        if(its == sessions.end())
        {
            Application::error("%s: all displays busy", __FUNCTION__);
            return nullptr;
        }

        int min = configGetInteger("display:min", 55);
        int max = configGetInteger("display:max", 99);
        auto freeDisplay = min;

        for(; freeDisplay <= max; ++freeDisplay)
        {
            if(std::none_of(sessions.begin(), sessions.end(), [&](auto & ptr) { return ptr && ptr->displayNum == freeDisplay; }))
            {
                break;
            }
        }

        if(freeDisplay > max)
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, freeDisplay);
            return nullptr;
        }

        auto xvfbSocket = Tools::x11UnixPath(freeDisplay);
        auto x11unix = std::filesystem::path(xvfbSocket).parent_path();

        if(! std::filesystem::is_directory(x11unix))
        {
            std::error_code fserr;
            std::filesystem::create_directory(x11unix, fserr);
            // default permision: 1777
            std::filesystem::permissions(x11unix,
                                         std::filesystem::perms::sticky_bit | std::filesystem::perms::owner_all | std::filesystem::perms::group_all | std::filesystem::perms::others_all,
                                         std::filesystem::perm_options::replace, fserr);
        }

        auto sess = std::make_shared<XvfbSession>();
        sess->userInfo = std::move(userInfo);
        sess->groupInfo = Tools::getGidInfo(sess->userInfo->gid());

        if(! sess->groupInfo)
        {
            Application::error("%s: gid not found: %" PRId32 ", user: `%s'",
                               __FUNCTION__, (int) sess->userInfo->gid(), sess->userInfo->user());
            return nullptr;
        }

        sess->mode = SessionMode::Login;
        sess->displayNum = freeDisplay;
        sess->depth = depth;
        sess->width = width;
        sess->height = height;
        sess->displayAddr = Tools::joinToString(":", sess->displayNum);

        sess->tpStart = std::chrono::system_clock::now();
        sess->startTimeLimitSec = configGetInteger("session:started:timeout", 0);

        // generate session key
        sess->mcookie = Tools::randomBytes(128);
        // session xauthfile
        sess->xauthfile = createXauthFile(sess->displayNum, sess->mcookie);

        if(sess->xauthfile.empty())
        {
            return nullptr;
        }

        Tools::setFileOwner(sess->xauthfile, sess->userInfo->uid(), sess->userInfo->gid());
        std::string xvfbBin = configGetString("xvfb:path");
        std::string xvfbArgs = configGetString("xvfb:args");
        // xvfb args
        xvfbArgs = Tools::replace(xvfbArgs, "%{display}", sess->displayNum);
        xvfbArgs = Tools::replace(xvfbArgs, "%{depth}", sess->depth);
        xvfbArgs = Tools::replace(xvfbArgs, "%{width}", sess->width);
        xvfbArgs = Tools::replace(xvfbArgs, "%{height}", sess->height);
        xvfbArgs = Tools::replace(xvfbArgs, "%{authfile}", sess->xauthfile.native());

        Application::debug(DebugType::App, "%s: bin: `%s', args: `%s'", __FUNCTION__, xvfbBin.c_str(), xvfbArgs.c_str());

        try
        {
            sess->pid1 = Application::forkMode();
        }
        catch(const std::exception &)
        {
            return nullptr;
        }

        if(0 == sess->pid1)
        {
            if(! switchToUser(*sess->userInfo))
            {
                execl("/bin/true", "/bin/true", nullptr);
                // execl failed
                std::exit(0);
            }

            // errlog folder
            auto ltsmLogFolder = std::filesystem::path(sess->userInfo->home()) / ".ltsm" / "log";
            std::error_code fserr;

            if(! std::filesystem::is_directory(ltsmLogFolder, fserr))
            {
                std::filesystem::create_directory(ltsmLogFolder, fserr);
            }

            auto logFile = ltsmLogFolder / std::filesystem::path(xvfbBin).filename();
            logFile.replace_extension(".log");
            // redirect stdout, atderr
            redirectStdoutStderrTo(true, true, logFile.native());
            // create argv
            std::list<std::string> list = Tools::split(xvfbArgs, 0x20);
            std::vector<const char*> argv;
            argv.reserve(list.size() + 2);
            argv.push_back(xvfbBin.c_str());

            for(const auto & str : list)
            {
                argv.push_back(str.c_str());
            }

            argv.push_back(nullptr);

            if(! Tools::fileReadable(sess->xauthfile))
            {
                Application::error("%s: %s failed, user: %s, error: %s", __FUNCTION__, "access", sess->userInfo->user(),
                                   strerror(errno));
            }

            closefds({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO});
            int res = execv(xvfbBin.c_str(), (char* const*) argv.data());

            if(res < 0)
            {
                Application::error("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                                   __FUNCTION__, "execv", strerror(errno), errno, xvfbBin.c_str());
            }

            // child exit
            std::exit(0);
        }

        // main thread
        Application::debug(DebugType::App, "%s: xvfb started, pid: %" PRId32 ", display: %" PRId32,
                           __FUNCTION__, sess->pid1, sess->displayNum);

        (*its) = std::move(sess);
        return *its;
    }

    int DBusAdaptor::runUserSession(XvfbSessionPtr xvfb, const std::filesystem::path & sessionBin, PamSession* pam)
    {
        if(! pam)
        {
            Application::error("%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "PAM failed", xvfb->displayNum, xvfb->userInfo->user());
            return -1;
        }

        pid_t pid = Application::forkMode();

        if(0 != pid)
        {
            // main thread
            return pid;
        }

        // child only
        Application::info("%s: pid: %" PRId32, __FUNCTION__, getpid());

        auto childExit = [](int res)
        {
            execl("/bin/true", "/bin/true", nullptr);
            std::exit(res);
            return res;
        };

        if(xvfb->userInfo->uid() == 0)
        {
            Application::error("%s: deny for root", __FUNCTION__);
            return childExit(-1);
        }

        std::error_code err;

        if(! std::filesystem::is_directory(xvfb->userInfo->home(), err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, (err ? err.message().c_str() : "not directory"), xvfb->userInfo->home(), getuid());
            return childExit(-1);
        }

        if(0 != initgroups(xvfb->userInfo->user(), xvfb->userInfo->gid()))
        {
            Application::error("%s: %s failed, user: %s, gid: %" PRId32 ", error: %s",
                               __FUNCTION__, "initgroups", xvfb->userInfo->user(), xvfb->userInfo->gid(), strerror(errno));
            return childExit(-1);
        }

        if(! pam->openSession())
        {
            Application::error("%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "PAM open session failed", xvfb->displayNum, xvfb->userInfo->user());
            return childExit(-1);
        }

        Application::debug(DebugType::App, "%s: child mode, type: %s, uid: %" PRId32,
                           __FUNCTION__, "session", getuid());

        // assign groups
        if(! switchToUser(*xvfb->userInfo))
        {
            return childExit(-1);
        }

        for(const auto & [key, val] : xvfb->environments)
        {
            setenv(key.c_str(), val.c_str(), 1);
        }

        setenv("XAUTHORITY", xvfb->xauthfile.c_str(), 1);
        setenv("DISPLAY", xvfb->displayAddr.c_str(), 1);
        setenv("LTSM_REMOTEADDR", xvfb->remoteAddr.c_str(), 1);
        setenv("LTSM_TYPECONN", xvfb->conntype.c_str(), 1);
        // pam environments
        auto environments = pam->getEnvList();

        // putenv: valid string
        for(const auto & env : environments)
        {
            Application::debug(DebugType::App, "%s: pam put environment: %s", __FUNCTION__, env.c_str());

            if(0 > putenv(const_cast<char*>(env.c_str())))
            {
                Application::error("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "putenv", strerror(errno), errno);
            }
        }

        createSessionConnInfo(xvfb);
        closefds({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO});
        int res = execl(sessionBin.c_str(), sessionBin.c_str(), (char*) nullptr);

        if(res < 0)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                               __FUNCTION__, "execl", strerror(errno), errno, sessionBin.c_str());
        }

        return childExit(0);
    }

    int32_t DBusAdaptor::busStartLoginSession(const int32_t & connectorId, const uint8_t & depth,
            const std::string & remoteAddr, const std::string & connType)
    {
        Application::debug(DebugType::Dbus, "%s: login request, remote: %s, type: %s",
                           __FUNCTION__, remoteAddr.c_str(), connType.c_str());

        auto displayWidth = configGetInteger("default:width", 1024);
        auto displayHeight = configGetInteger("default:height", 768);
        auto userInfo = Tools::getUserInfo(ltsm_user_conn);

        if(! userInfo)
        {
            Application::error("%s: user not found: `%s'", __FUNCTION__, ltsm_user_conn);
            return -1;
        }

        auto xvfb = runXvfbDisplayNewSession(depth, displayWidth, displayHeight, std::move(userInfo));

        if(! xvfb)
        {
            return -1;
        }

        // registered xvfb job
        waitPidBackgroundSafe(xvfb->pid1);

        // update screen
        xvfb->remoteAddr = remoteAddr;
        xvfb->conntype = connType;
        xvfb->connectorId = connectorId;
        // fix permission
        auto groupAuthGid = Tools::getGroupGid(ltsm_group_auth);
        Tools::setFileOwner(xvfb->xauthfile, xvfb->userInfo->uid(), groupAuthGid);

        // wait Xvfb display starting
        if(! waitXvfbStarting(xvfb->displayNum, xvfb->mcookie, 5000 /* 5 sec */))
        {
            Application::error("%s: %s failed, display: %" PRId32, __FUNCTION__, "waitXvfbStarting", xvfb->displayNum);
            return -1;
        }

        // check socket
        std::filesystem::path socketPath = Tools::x11UnixPath(xvfb->displayNum);
        std::error_code err;

        if(! std::filesystem::is_socket(socketPath, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, (err ? err.message().c_str() : "not socket"), socketPath.c_str(), getuid());
            return -1;
        }

        // fix socket pemissions 0660
        std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), socketPath.c_str(), getuid());
        }

        Tools::setFileOwner(socketPath, xvfb->userInfo->uid(), groupAuthGid);
        std::string helperArgs = configGetString("helper:args");

        if(helperArgs.size())
        {
            helperArgs = Tools::replace(helperArgs, "%{display}", xvfb->displayNum);
            helperArgs = Tools::replace(helperArgs, "%{authfile}", xvfb->xauthfile.native());
        }

        // simple cursor
        if(configHasKey("display:cursor"))
        {
            runSessionCommandSafe(xvfb, "/usr/bin/xsetroot", { "-cursor_name", configGetString("display:cursor") });
        }

        // runas login helper
        xvfb->pid2 = runSessionCommandSafe(xvfb, configGetString("helper:path"), Tools::split(helperArgs, 0x20));

        if(0 >= xvfb->pid2)
        {
            return -1;
        }

        startLoginChannels(xvfb);

        return xvfb->displayNum;
    }

    int32_t DBusAdaptor::busStartUserSession(const int32_t & oldScreen, const int32_t & connectorId,
            const std::string & userName, const std::string & remoteAddr, const std::string & connType)
    {
        Application::debug(DebugType::Dbus, "%s: session request, user: %s, remote: %s, display: %" PRId32,
                           __FUNCTION__, userName.c_str(), remoteAddr.c_str(), oldScreen);

        std::string sessionBin = configGetString("session:path");

        auto userInfo = Tools::getUserInfo(userName);

        if(! userInfo)
        {
            Application::error("%s: user not found: `%s'", __FUNCTION__, userName.c_str());
            return -1;
        }

        std::error_code err;

        if(! std::filesystem::is_directory(userInfo->home(), err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, (err ? err.message().c_str() : "not directory"), userInfo->home(), getuid());
            return -1;
        }

        auto loginSess = findDisplaySession(oldScreen);

        if(! loginSess)
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, oldScreen);
            return -1;
        }

        // auto close login session
        std::unique_ptr<PamSession> pam = std::move(loginSess->pam);

        if(! pam)
        {
            Application::error("%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "PAM failed", loginSess->displayNum, userInfo->user());
            return -1;
        }

        if(! pam->isAuthenticated())
        {
            Application::error("%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "PAM authenticate failed", loginSess->displayNum, userInfo->user());
            return -1;
        }

        if(! pam->isLogin(userInfo->user()))
        {
            Application::error("%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "PAM login failed", loginSess->displayNum, userInfo->user());
            return -1;
        }

        auto oldSess = findUserSession(userName);

        if(oldSess && 0 <= oldSess->displayNum && checkXvfbSocket(oldSess->displayNum))
        {
            // parent continue
            oldSess->remoteAddr = remoteAddr;
            oldSess->conntype = connType;
            oldSess->connectorId = connectorId;
            oldSess->environments = std::move(loginSess->environments);
            oldSess->options = std::move(loginSess->options);
            oldSess->encryption = std::move(loginSess->encryption);
            oldSess->layout = std::move(loginSess->layout);

            // reinit pam session
            if(! oldSess->pam || ! oldSess->pam->refreshCreds())
            {
                Application::error("%s: %s, display: %" PRId32 ", user: %s",
                                   __FUNCTION__, "PAM failed", oldSess->displayNum, oldSess->userInfo->user());
                displayShutdown(oldSess, true);
                return -1;
            }

            // update conn info
            createSessionConnInfo(oldSess);
            Application::debug(DebugType::App, "%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "user connect to session", oldSess->displayNum, oldSess->userInfo->user());
            emitSessionReconnect(remoteAddr, connType);

            if(configGetBoolean("session:kill:stop", false))
            {
                auto cmd = std::string("/usr/bin/killall -s SIGCONT -u ").append(oldSess->userInfo->user());
                int ret = std::system(cmd.c_str());
                Application::debug(DebugType::App, "%s: command: `%s', return code: %" PRId32 ", display: %" PRId32,
                                   __FUNCTION__, cmd.c_str(), ret, oldSess->displayNum);
            }

            sessionRunSetxkbmapLayout(oldSess);
            startSessionChannels(oldSess);
            runSessionScript(oldSess, configGetString("session:connect"));
            return oldSess->displayNum;
        }

        // get owner screen
        auto newSess = runXvfbDisplayNewSession(loginSess->depth, loginSess->width, loginSess->height, std::move(userInfo));

        if(! newSess)
        {
            return -1;
        }

        // registered xvfb job
        waitPidBackgroundSafe(newSess->pid1);

        // update screen
        newSess->environments = std::move(loginSess->environments);
        newSess->options = std::move(loginSess->options);
        newSess->encryption = std::move(loginSess->encryption);
        newSess->layout = std::move(loginSess->layout);
        newSess->remoteAddr = remoteAddr;
        newSess->conntype = connType;
        newSess->connectorId = connectorId;
        newSess->policy = sessionPolicy(Tools::lower(configGetString("session:policy")));
        newSess->mode = SessionMode::Started;
        newSess->tpStart = std::chrono::system_clock::now();
        newSess->startTimeLimitSec = configGetInteger("session:started:timeout", 0);

        if(! configGetBoolean("transfer:file:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel:: TransferFiles);
        }

        if(! configGetBoolean("channel:printer:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RedirectPrinter);
        }

        if(! configGetBoolean("channel:audio:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RedirectAudio);
        }

        if(! configGetBoolean("channel:pcsc:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RedirectPcsc);
        }

        if(! configGetBoolean("channel:sane:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RedirectScanner);
        }

        if(! configGetBoolean("channel:fuse:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RemoteFilesUse);
        }

        // fix permission
        auto groupAuthGid = Tools::getGroupGid(ltsm_group_auth);
        Tools::setFileOwner(newSess->xauthfile, newSess->userInfo->uid(), groupAuthGid);

        // wait Xvfb display starting
        if(! waitXvfbStarting(newSess->displayNum, newSess->mcookie, 5000 /* 5 sec */))
        {
            Application::error("%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "waitXvfbStarting failed", newSess->displayNum, newSess->userInfo->user());
            return -1;
        }

        // check socket
        std::filesystem::path socketPath = Tools::x11UnixPath(oldScreen);

        if(! std::filesystem::is_socket(socketPath, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, (err ? err.message().c_str() : "not socket"), socketPath.c_str(), getuid());
            return -1;
        }

        // fix socket pemissions 0660
        std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), socketPath.c_str(), getuid());
        }

        Tools::setFileOwner(socketPath, newSess->userInfo->uid(), groupAuthGid);

        // fixed environments
        for(auto & [key, val] : newSess->environments)
        {
            if(std::string::npos != val.find("%{user}"))
            {
                val = Tools::replace(val, "%{user}", userName);
            }
            else if(std::string::npos != val.find("%{runtime_dir}"))
            {
                val = Tools::replace(val, "%{runtime_dir}", newSess->userInfo->xdgRuntimeDir().native());
            }
        }

        // move pam from login session
        newSess->pam = std::move(pam);
        newSess->pid2 = runUserSession(newSess, sessionBin, newSess->pam.get());

        if(newSess->pid2 < 0)
        {
            Application::error("%s: user session failed, result: %" PRId32, __FUNCTION__, newSess->pid2);
            return -1;
        }

        // registered session job
        waitPidBackgroundSafe(newSess->pid2);

        // parent continue
        Application::debug(DebugType::App, "%s: user session started, pid: %" PRId32 ", display: %" PRId32,
                           __FUNCTION__, newSess->pid2, newSess->displayNum);
        sessionRunSetxkbmapLayout(newSess);
        runSystemScript(newSess, configGetString("system:logon"));
        runSystemScript(newSess, configGetString("system:connect"));
        startSessionChannels(newSess);
        runSessionScript(newSess, configGetString("session:connect"));
        return newSess->displayNum;
    }

    int32_t DBusAdaptor::busGetServiceVersion(void)
    {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);
        return LTSM::service_version;
    }

    std::string DBusAdaptor::busDisplayAuthFile(const int32_t & display)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32, __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display))
        {
            return xvfb->xauthfile;
        }

        Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        return "";
    }

    void DBusAdaptor::busShutdownDisplay(const int32_t & display)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32, __FUNCTION__, display);

        if(auto ptr = findDisplaySession(display))
        {
            std::thread([this, xvfb = std::move(ptr)]()
            {
                std::this_thread::sleep_for(10ms);
                this->displayShutdown(xvfb, true);
            }).detach();
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busShutdownConnector(const int32_t & display)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32, __FUNCTION__, display);
        std::thread([this, display]()
        {
            std::this_thread::sleep_for(10ms);
            this->emitShutdownConnector(display);
        }).detach();
    }

    void DBusAdaptor::busShutdownService(void)
    {
        Application::debug(DebugType::Dbus, "%s: %s, pid: %" PRId32, __FUNCTION__, "starting", getpid());

        // terminate connectors
        for(const auto & ptr : sessions)
        {
            if(ptr)
            {
                displayShutdown(ptr, true);
            }
        }

        auto isValidSession = [](XvfbSessionPtr & ptr)
        {
            return ptr;
        };

        // wait sessions
        while(auto sessionsAlive = std::count_if(sessions.begin(), sessions.end(), isValidSession))
        {
            Application::debug(DebugType::App, "%s: wait sessions: %lu", __FUNCTION__, sessionsAlive);
            std::this_thread::sleep_for(100ms);
        }

        std::scoped_lock guard{ lockRunning };

        // childEnded
        if(! childsRunning.empty())
        {
            auto childsCount = std::count_if(childsRunning.begin(), childsRunning.end(), [](auto & pair)
            {
                return 0 < pair.first;
            });

            Application::error("%s: running childs: %lu, killed process", __FUNCTION__, childsCount);

            for(const auto & [pid, futureStatus] : childsRunning)
            {
                kill(pid, SIGTERM);
            }

            std::this_thread::sleep_for(100ms);

            for(const auto & [pid, futureStatus] : childsRunning)
            {
                futureStatus.wait();
            }

            childsRunning.clear();
        }

        Application::notice("%s: %s, pid: %" PRId32, __FUNCTION__, "complete", getpid());
        serviceConn->leaveEventLoop();
    }

    bool DBusAdaptor::sessionRunZenity(XvfbSessionPtr xvfb, std::initializer_list<std::string> params)
    {
        std::filesystem::path zenity = configGetString("zenity:path", "/usr/bin/zenity");
        return 0 != runSessionCommandSafe(std::move(xvfb), zenity, std::move(params));
    }

    void DBusAdaptor::busSendMessage(const int32_t & display, const std::string & message)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", message: `%s'", __FUNCTION__, display, message.c_str());

        if(auto xvfb = findDisplaySession(display))
        {
            if(xvfb->mode == SessionMode::Connected ||
                    xvfb->mode == SessionMode::Disconnected)
            {
                sessionRunZenity(xvfb, { "--info", "--no-wrap", "--text", Tools::quotedString(message) });
                return;
            }

            Application::warning("%s: %s failed, display: %" PRId32, __FUNCTION__, "session mode", display);
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSessionIdleTimeout(const int32_t & display)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32, __FUNCTION__, display);

        if(auto ptr = findDisplaySession(display))
        {
            emitSessionIdleTimeout(ptr->displayNum, ptr->userInfo->user());

            if(configGetBoolean("session:idle:disconnect", false))
            {
                std::thread([this, xvfb = std::move(ptr)]()
                {
                    std::this_thread::sleep_for(10ms);
                    this->emitShutdownConnector(xvfb->displayNum);
                }).detach();
            }
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busConnectorAlive(const int32_t & display)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32, __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->resetStatus(Flags::SessionStatus::CheckConnection);
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetLoginsDisable(const bool & action)
    {
        Application::debug(DebugType::Dbus, "%s: action: %s", __FUNCTION__, (action ? "true" : "false"));
        loginsDisable = action;
    }

#ifdef LTSM_BUILD_COVERAGE_TESTS
    bool skipLoginShutdown(int display)
    {
        try
        {
            if(auto env = getenv("LTSM_SESSION_TEST"))
            {
                if(auto sid = std::stoi(env); sid == display)
                {
                    return true;
                }
            }
        }
        catch(...)
        {
        }

        return false;
    }
#endif

    void DBusAdaptor::busConnectorConnected(const int32_t & display, const int32_t & connectorId)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32, __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->connectorId = connectorId;
            xvfb->tpOnline = std::chrono::system_clock::now();
            xvfb->onlineTimeLimitSec = configGetInteger("session:online:timeout", 0);
            xvfb->mode = SessionMode::Connected;

            emitSessionOnline(xvfb->displayNum, xvfb->userInfo->user());
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busConnectorTerminated(const int32_t & display, const int32_t & connectorId)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32, __FUNCTION__, display);

        auto ptr = findDisplaySession(display);

        if(! ptr)
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
            return;
        }

        if(ptr->mode == SessionMode::Login)
        {
#ifdef LTSM_BUILD_COVERAGE_TESTS

            if(skipLoginShutdown(display))
            {
                return;
            }

#endif
            stopLoginChannels(ptr);
            displayShutdown(std::move(ptr), false);
        }
        else if(ptr->mode == SessionMode::Connected)
        {
            ptr->resetStatus(Flags::SessionStatus::CheckConnection);
            ptr->remoteAddr.clear();
            ptr->conntype.clear();
            ptr->encryption.clear();
            ptr->connectorId = 0;
            ptr->tpOffline = std::chrono::system_clock::now();
            ptr->offlineTimeLimitSec = configGetInteger("session:offline:timeout", 0);
            ptr->mode = SessionMode::Disconnected;
            createSessionConnInfo(ptr, false);

            // stop user process
            if(configGetBoolean("session:kill:stop", false))
            {
                auto cmd = std::string("/usr/bin/killall -s SIGSTOP -u ").append(ptr->userInfo->user());
                int ret = std::system(cmd.c_str());
                Application::debug(DebugType::App, "%s: command: `%s', return code: %" PRId32 ", display: %" PRId32,
                                   __FUNCTION__, cmd.c_str(), ret, ptr->displayNum);
            }

            emitSessionOffline(ptr->displayNum, ptr->userInfo->user());
            stopSessionChannels(std::move(ptr));
        }
    }

    void DBusAdaptor::transferFilesRequestCommunication(XvfbSessionPtr xvfb, std::filesystem::path zenity,
            std::vector<FileNameSize> files, TransferRejectFunc emitTransferReject, PidStatus zenityQuestionResult)
    {
        // copy all files to (Connector) user home, after success move to real user
        auto connectorHome = Tools::getUserHome(ltsm_user_conn);
        // wait zenity question
        int status = zenityQuestionResult.second.get();

        // yes = 0, no: 256
        if(status == 256)
        {
            emitTransferReject(xvfb->displayNum, files);
            return;
        }

        // zenity select directory
        std::future<StatusStdout> zenitySelectDirectoryResult;
        bool error = false;

        try
        {
            auto pair = RunAs::runSessionCommandStdout(xvfb, zenity,
            { "--file-selection", "--directory", "--title", "Select directory", "--width", "640", "--height", "480" });
            zenitySelectDirectoryResult = std::move(pair.second);
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
            emitTransferReject(xvfb->displayNum, files);
            return;
        }

        // wait file selection
        // get StatusStdout
        auto ret = zenitySelectDirectoryResult.get();
        status = ret.first;

        // ok = 0, cancel: 256
        if(status == 256)
        {
            emitTransferReject(xvfb->displayNum, files);
            return;
        }

        // get dstdir
        auto & buf = ret.second;
        auto end = buf.back() == 0x0a ? std::prev(buf.end()) : buf.end();
        std::filesystem::path dstdir(std::string(buf.begin(), end));
        std::error_code err;

        if(! std::filesystem::is_directory(dstdir, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, err.message().c_str(), dstdir.c_str(), getuid());
            emitTransferReject(xvfb->displayNum, files);
            return;
        }

        for(const auto & info : files)
        {
            auto tmpname = std::filesystem::path(connectorHome) / "transfer_";
            tmpname += Tools::randomHexString(8);
            Application::debug(DebugType::App, "%s: transfer file request, display: %" PRId32 ", select dir: `%s', tmp name: `%s'",
                               __FUNCTION__, xvfb->displayNum, dstdir.c_str(), tmpname.c_str());
            auto filepath = std::filesystem::path(std::get<0>(info));
            auto filesize = std::get<1>(info);
            // check disk space limited
            //size_t ftotal = std::accumulate(files.begin(), files.end(), 0, [](size_t sum, auto & val){ return sum += std::get<1>(val); });
            auto spaceInfo = std::filesystem::space(dstdir, err);

            if(spaceInfo.available < filesize)
            {
                busSendNotify(xvfb->displayNum, "Transfer Rejected", "not enough disk space",
                              NotifyParams::Error, NotifyParams::UrgencyLevel::Normal);
                break;
            }

            // check dstdir writeable / filename present
            auto dstfile = dstdir / filepath.filename();

            if(std::filesystem::exists(dstfile, err))
            {
                Application::error("%s: file present and skipping, path: `%s'", __FUNCTION__, dstfile.c_str());
                busSendNotify(xvfb->displayNum, "Transfer Skipping",
                              Tools::StringFormat("such a file exists: %1").arg(dstfile.c_str()),
                              NotifyParams::Warning, NotifyParams::UrgencyLevel::Normal);
                continue;
            }

            xvfb->allowTransfer.emplace_front(filepath);
            emitTransferAllow(xvfb->displayNum, filepath, tmpname, dstdir);
        }
    }

    void DBusAdaptor::transferFileStartBackground(XvfbSessionPtr xvfb, std::string tmpfile, std::string dstfile, uint32_t filesz)
    {
        bool error = false;
        std::error_code fserr;

        while(! error)
        {
            // check fill data complete
            if(std::filesystem::exists(tmpfile, fserr) &&
                    std::filesystem::file_size(tmpfile, fserr) >= filesz)
            {
                break;
            }

            // FIXME create progress informer session

            // check lost conn
            if(xvfb->mode != SessionMode::Connected)
            {
                busSendNotify(xvfb->displayNum, "Transfer Error", Tools::StringFormat("transfer connection is lost"),
                              NotifyParams::Error, NotifyParams::UrgencyLevel::Normal);
                error = true;
                std::filesystem::remove(tmpfile, fserr);
                continue;
            }

            std::this_thread::sleep_for(350ms);
        }

        xvfb->allowTransfer.remove(tmpfile);

        if(error)
        {
            return;
        }

        // move tmpfile to dstfile
        std::filesystem::rename(tmpfile, dstfile, fserr);

        if(fserr)
        {
            if(fserr.value() == 18)
            {
                std::filesystem::copy_file(tmpfile, dstfile, fserr);
            }
            else
            {
                Application::error("%s: %s, path: `%s'", __FUNCTION__, fserr.message().c_str(), dstfile.c_str());
                error = true;
            }

            std::filesystem::remove(tmpfile, fserr);
        }

        if(! error)
        {
            Tools::setFileOwner(dstfile, xvfb->userInfo->uid(), xvfb->userInfo->gid());
            busSendNotify(xvfb->displayNum, "Transfer Complete",
                          Tools::StringFormat("new file added: <a href=\"file://%1\">%2</a>").
                          arg(dstfile).arg(std::filesystem::path(dstfile).filename().c_str()),
                          NotifyParams::Information, NotifyParams::UrgencyLevel::Normal);
        }
    }

    bool DBusAdaptor::busTransferFilesRequest(const int32_t & display, const std::vector<FileNameSize> & files)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", count: %lu", __FUNCTION__, display, files.size());
        auto xvfb = findDisplaySession(display);

        if(! xvfb)
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::TransferFiles))
        {
            Application::warning("%s: display %" PRId32 ", transfer reject", __FUNCTION__, display);
            busSendNotify(display, "Transfer Restricted", "transfer is blocked, contact the administrator",
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        if(configHasKey("transfer:group:only"))
        {
            if(auto groupInfo = Tools::getGroupInfo(configGetString("transfer:group:only")))
            {
                auto gids = xvfb->userInfo->groups();

                if(std::none_of(gids.begin(), gids.end(), [&](auto & gid) { return gid == groupInfo->gid(); }))
                {
                    Application::warning("%s: display %" PRId32 ", transfer reject", __FUNCTION__, display);
                    busSendNotify(display, "Transfer Restricted", "transfer is blocked, contact the administrator",
                                  NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
                    return false;
                }
            }
        }

        std::filesystem::path zenity = configGetString("zenity:path", "/usr/bin/zenity");
        auto msg = Tools::joinToString("Can you receive remote files? (", files.size(), ")");
        PidStatus zenityResult;

        TransferRejectFunc emitTransferReject = [this](int display, const std::vector<FileNameSize> & files)
        {
            for(const auto & info : files)
            {
                // empty dst/file erase job
                this->emitTransferAllow(display, std::get<0>(info), "", "");
            }
        };

        try
        {
            zenityResult = RunAs::runSessionCommand(xvfb, zenity, { "--question", "--default-cancel", "--text", msg });
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            emitTransferReject(display, files);
            return false;
        }

        //run background
        std::thread(& DBusAdaptor::transferFilesRequestCommunication, this, xvfb, zenity, files, std::move(emitTransferReject),
                    std::move(zenityResult)).detach();
        return true;
    }

    bool DBusAdaptor::busTransferFileStarted(const int32_t & display, const std::string & tmpfile,
            const uint32_t & filesz, const std::string & dstfile)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", tmp file: `%s', dst file: `%s'",
                           __FUNCTION__, display, tmpfile.c_str(), dstfile.c_str());

        if(auto xvfb = findDisplaySession(display))
        {
            std::thread(&DBusAdaptor::transferFileStartBackground, this, std::move(xvfb), tmpfile, dstfile, filesz).detach();
            return true;
        }

        Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        return false;
    }

    void sendNotifyCall(XvfbSessionPtr xvfb, std::string summary, std::string body, uint8_t icontype)
    {
        // wait new session started
        while(xvfb->sessionOnlinedSec() < std::chrono::seconds(2))
        {
            std::this_thread::sleep_for(1s);
        }

        Application::debug(DebugType::App, "%s: display: %" PRId32 ", user: %s, summary: %s",
                           __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), summary.c_str());

        std::string notificationIcon("dialog-information");

        switch(icontype)
        {
            //case NotifyParams::IconType::Information:
            case NotifyParams::IconType::Warning:
                notificationIcon.assign("dialog-error");
                break;

            case NotifyParams::IconType::Error:
                notificationIcon.assign("dialog-warning");
                break;

            case NotifyParams::IconType::Question:
                notificationIcon.assign("dialog-question");
                break;

            default:
                break;
        }

        auto dbusAddresses = getSessionDBusAddresses(*xvfb->userInfo, xvfb->displayNum);

        if(dbusAddresses.empty())
        {
            Application::warning("%s: %s, display: %" PRId32 ", user: %s",
                                 __FUNCTION__, "dbus address empty", xvfb->displayNum, xvfb->userInfo->user());
            return;
        }

        auto destinationName = "org.freedesktop.Notifications";
        auto objectPath = "/org/freedesktop/Notifications";
        std::vector<std::string> actions;
        std::map<std::string, sdbus::Variant> hints;
        int32_t expirationTimeout = -1;
        std::string applicationName("LTSM");
        uint32_t replacesID = 0;

#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto addr = Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";");
            Application::debug(DebugType::App, "%s: dbus address: `%s'", __FUNCTION__, addr.c_str());

            auto conn = sdbus::createSessionBusConnectionWithAddress(addr);
#ifdef SDBUS_2_0_API
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), sdbus::ServiceName {destinationName}, sdbus::ObjectPath {objectPath});
#else
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);
#endif
            concatenatorProxy->callMethod("Notify").onInterface("org.freedesktop.Notifications").withArguments(applicationName,
                    replacesID, notificationIcon,
                    summary, body, actions, hints, expirationTimeout).dontExpectReply();
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %" PRId32 ", sdbus error: %s, msg: %s",
                               __FUNCTION__, xvfb->displayNum, err.getName().c_str(), err.getMessage().c_str());
        }
        catch(std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }

#else
        Application::warning("%s: sdbus address not supported, use 1.2 version", __FUNCTION__);
#endif
    }

    void DBusAdaptor::busSendNotify(const int32_t & display, const std::string & summary, const std::string & body,
                                    const uint8_t & icontype, const uint8_t & urgency)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", summary: `%s', body: `%s'",
                           __FUNCTION__, display, summary.c_str(), body.c_str());

        // urgency:  NotifyParams::UrgencyLevel { Low, Normal, Critical }
        // icontype: NotifyParams::IconType { Information, Warning, Error, Question }
        if(auto xvfb = findDisplaySession(display))
        {
            if(xvfb->mode == SessionMode::Connected ||
                    xvfb->mode == SessionMode::Disconnected)
            {
                // thread mode
                std::thread(& sendNotifyCall, std::move(xvfb), summary, body, icontype /*, urgency2 = urgency */).detach();
                return;
            }

            Application::warning("%s: %s failed, display: %" PRId32, __FUNCTION__, "session mode", display);
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::helperWidgetStartedAction(const int32_t & display)
    {
        Application::info("%s: display: %" PRId32, __FUNCTION__, display);

        std::thread([this, display]()
        {
            this->emitHelperWidgetStarted(display);
        }).detach();
    }

    std::forward_list<std::string> DBusAdaptor::getAllowLogins(void) const
    {
        // uids names: "access:uid:min", "access:uid:max"
        int minUidRange = configGetInteger("access:uid:min", 0);
        int maxUidRange = configGetInteger("access:uid:max", INT32_MAX);
        auto accessUidNames = Tools::getSystemUsers(minUidRange, maxUidRange);
        // access list: "access:users"
        auto accessUsersNames = config().getStdListForward<std::string>("access:users");

        // append list: "access:groups"
        for(const auto & group : config().getStdListForward<std::string>("access:groups"))
        {
            try
            {
                accessUsersNames.splice_after(accessUsersNames.begin(), GroupInfo(group).members());
            }
            catch(const std::exception &)
            {
            }
        }

        if(accessUsersNames.empty())
        {
            return accessUidNames;
        }

        accessUsersNames.sort();
        accessUsersNames.unique();

        if(accessUidNames.empty())
        {
            return accessUsersNames;
        }

        accessUidNames.sort();
        accessUidNames.unique();
        std::forward_list<std::string> allowNames;
        std::set_intersection(accessUsersNames.begin(), accessUsersNames.end(), accessUidNames.begin(), accessUidNames.end(),
                              std::front_inserter(allowNames));
        return allowNames;
    }

    std::vector<std::string> DBusAdaptor::helperGetUsersList(const int32_t & display)
    {
        auto allowLogins = getAllowLogins();
        return std::vector<std::string>(allowLogins.begin(), allowLogins.end());
    }

    bool DBusAdaptor::busSetAuthenticateToken(const int32_t & display, const std::string & login)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", user: %s",
                           __FUNCTION__, display, login.c_str());

        if(auto xvfb = this->findDisplaySession(display))
        {
            std::thread(&DBusAdaptor::pamAuthenticate, this, std::move(xvfb), login, "******", true).detach();
            return true;
        }

        Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        return true;
    }

    bool DBusAdaptor::busSetAuthenticateLoginPass(const int32_t & display, const std::string & login,
            const std::string & password)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", user: %s",
                           __FUNCTION__, display, login.c_str());

        if(auto xvfb = this->findDisplaySession(display))
        {
            std::thread(&DBusAdaptor::pamAuthenticate, this, std::move(xvfb), login, password, false).detach();
            return true;
        }

        Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        return false;
    }

    bool DBusAdaptor::pamAuthenticate(XvfbSessionPtr xvfb, const std::string & login, const std::string & password,
                                      bool token)
    {
        Application::info("%s: display: %" PRId32 ", user: %s", __FUNCTION__, xvfb->displayNum, login.c_str());
        auto users = getAllowLogins();

        if(users.empty())
        {
            Application::error("%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "login disabled", xvfb->displayNum, login.c_str());
            emitLoginFailure(xvfb->displayNum, "login disabled");
            return false;
        }

        if(std::none_of(users.begin(), users.end(), [&](auto & val) { return val == login; }))
        {
            Application::error("%s: %s, display: %" PRId32 ", user: %s",
                               __FUNCTION__, "login not found", xvfb->displayNum, login.c_str());
            emitLoginFailure(xvfb->displayNum, "login not found");
            return false;
        }

        if(loginsDisable)
        {
            Application::info("%s: %s, display: %" PRId32, __FUNCTION__, "logins disabled", xvfb->displayNum);
            emitLoginFailure(xvfb->displayNum, "logins disabled by the administrator");
            return false;
        }

        int loginFailuresConf = configGetInteger("login:failures_count", 0);

        if(0 > loginFailuresConf)
        {
            loginFailuresConf = 0;
        }

        // open PAM
        auto pam = std::make_unique<PamSession>(configGetString("pam:service"), login, password);

        if(! pam->pamStart(login))
        {
            emitLoginFailure(xvfb->displayNum, "pam error");
            return false;
        }

        if(! token)
        {
            // check user/pass
            if(! pam->authenticate())
            {
                emitLoginFailure(xvfb->displayNum, pam->error());
                xvfb->loginFailures += 1;

                if(loginFailuresConf < xvfb->loginFailures)
                {
                    Application::error("%s: login failures limit, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
                    emitLoginFailure(xvfb->displayNum, "failures limit");
                    displayShutdown(xvfb, true);
                }

                return false;
            }

            pam->setItem(PAM_XDISPLAY, xvfb->displayAddr.c_str());
            pam->setItem(PAM_TTY, std::string("X11:").append(xvfb->displayAddr.c_str()).c_str());
            pam->setItem(PAM_RHOST, xvfb->remoteAddr.empty() ? "127.0.0.1" : xvfb->remoteAddr.c_str());

            if(! pam->validateAccount())
            {
                Application::error("%s: %s failed", __FUNCTION__, "validateAccount");
                return false;
            }
        }

        // auth success
        if(0 < loginFailuresConf)
        {
            xvfb->loginFailures = 0;
        }

        // check connection policy
        auto userSess = findUserSession(login);

        if(userSess && 0 < userSess->displayNum &&
                userSess->mode == SessionMode::Connected)
        {
            if(userSess->policy == SessionPolicy::AuthLock)
            {
                Application::error("%s: session busy, policy: %s, user: %s, session display: %" PRId32 ", from: %s, display: %" PRId32,
                                   __FUNCTION__, "authlock", login.c_str(), userSess->displayNum, userSess->remoteAddr.c_str(), xvfb->displayNum);
                // informer login display
                emitLoginFailure(xvfb->displayNum, Tools::joinToString("session busy, from: ", userSess->remoteAddr));
                return false;
            }
            else if(userSess->policy == SessionPolicy::AuthTake)
            {
                // shutdown prev connect
                emitShutdownConnector(userSess->displayNum);
                // wait session
                Tools::waitCallable<std::chrono::milliseconds>(1000, 50, [& userSess]()
                {
                    return userSess->mode == SessionMode::Disconnected;
                });
            }
        }

        Application::notice("%s: success, display: %" PRId32 ", user: %s, token: %s",
                            __FUNCTION__, xvfb->displayNum, login.c_str(), (token ? "true" : "false"));

        xvfb->pam = std::move(pam);
        emitLoginSuccess(xvfb->displayNum, login, Tools::getUserUid(login));
        return true;
    }

    void DBusAdaptor::sessionRunSetxkbmapLayout(XvfbSessionPtr xvfb)
    {
        if(xvfb && ! xvfb->layout.empty())
        {
            std::thread([this, ptr = std::move(xvfb)]
            {
                this->runSessionCommandSafe(ptr, "/usr/bin/setxkbmap", { "-layout", ptr->layout, "-option", "\"\"" });
            }).detach();
        }
    }

    void DBusAdaptor::busSetSessionKeyboardLayouts(const int32_t & display, const std::vector<std::string> & layouts)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", layouts: [%s]",
                           __FUNCTION__, display, Tools::join(layouts.begin(), layouts.end(), ",").c_str());

        if(auto xvfb = findDisplaySession(display))
        {
            if(layouts.empty())
            {
                return;
            }

            std::ostringstream os;

            for(auto it = layouts.begin(); it != layouts.end(); ++it)
            {
                auto id = Tools::lower((*it).substr(0, 2));

                if(id == "en")
                {
                    id = "us";
                }

                os << id;

                if(std::next(it) != layouts.end())
                {
                    os << ",";
                }
            }

            xvfb->layout = Tools::quotedString(os.str());
            sessionRunSetxkbmapLayout(xvfb);
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionEnvironments(const int32_t & display, const std::map<std::string, std::string> & map)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", env counts: %lu",
                           __FUNCTION__, display, map.size());

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->environments.clear();

            for(const auto & [key, val] : map)
            {
                Application::info("%s: %s = `%s'", __FUNCTION__, key.c_str(), val.c_str());
                xvfb->environments.emplace(key, val);

                if(key == "TZ")
                {
                    emitHelperSetTimezone(display, val);
                }
            }
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionOptions(const int32_t & display, const std::map<std::string, std::string> & map)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", opts counts: %lu",
                           __FUNCTION__, display, map.size());

        auto xvfb = findDisplaySession(display);

        if(! xvfb)
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
            return;
        }

        xvfb->options.clear();
        std::string login, pass;

        for(const auto & [key, val] : map)
        {
            Application::info("%s: %s = `%s'", __FUNCTION__, key.c_str(), (key != "password" ? val.c_str() : "HIDDEN"));

            if(key == "redirect:cups")
            {
                if(configGetBoolean("channel:printer:disabled", false))
                {
                    continue;
                }
            }
            else if(key == "redirect:fuse")
            {
                if(configGetBoolean("channel:fuse:disabled", false))
                {
                    continue;
                }
            }
            else if(key == "redirect:audio")
            {
                if(configGetBoolean("channel:audio:disabled", false))
                {
                    continue;
                }
            }
            else if(key == "redirect:pcsc")
            {
                if(configGetBoolean("channel:pcsc:disabled", false))
                {
                    continue;
                }

                xvfb->environments.emplace("PCSCLITE_CSOCK_NAME", "%{runtime_dir}/pcsc2ltsm");
            }
            else if(key == "redirect:sane")
            {
                if(configGetBoolean("channel:sane:disabled", false))
                {
                    continue;
                }

                auto socket = configGetString("channel:sane:format", "/var/run/ltsm/sane/%{user}");
                xvfb->environments.emplace("SANE_UNIX_PATH", socket);
            }
            else if(key == "username")
            {
                login = val;
            }
            else if(key == "password")
            {
                pass = val;
            }
            else if(key == "pkcs11:auth")
            {
                startPkcs11Listener(xvfb, "");
                emitHelperPkcs11ListennerStarted(display, xvfb->connectorId);
            }

            xvfb->options.emplace(key, val);
        }

        if(! login.empty())
        {
            emitHelperSetLoginPassword(display, login, pass, ! pass.empty());
        }
    }

    void fixPermissionJob(std::filesystem::path path, uid_t uid, gid_t gid, mode_t mode)
    {
        auto tp = std::chrono::steady_clock::now();
        bool failed = false;
        std::error_code fserr;

        while(! failed)
        {
            if(std::filesystem::exists(path, fserr))
            {
                break;
            }

            std::this_thread::sleep_for(300ms);
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp);

            if(dt.count() > 3500)
            {
                failed = true;
            }
        }

        if(! failed)
        {
            Tools::setFileOwner(path, uid, gid, mode);
        }
    }

    void DBusAdaptor::startSessionChannels(XvfbSessionPtr xvfb)
    {
        auto printer = xvfb->options.find("redirect:cups");

        if(xvfb->options.end() != printer)
        {
            startPrinterListener(xvfb, printer->second);
        }

        auto sane = xvfb->options.find("redirect:sane");

        if(xvfb->options.end() != sane)
        {
            startSaneListener(xvfb, sane->second);
        }

        auto audio = xvfb->options.find("redirect:audio");

        if(xvfb->options.end() != audio)
        {
            startAudioListener(xvfb, audio->second);
        }

        auto pcsc = xvfb->options.find("redirect:pcsc");

        if(xvfb->options.end() != pcsc)
        {
            startPcscListener(xvfb, pcsc->second);
        }

        auto fuse = xvfb->options.find("redirect:fuse");

        if(xvfb->options.end() != fuse && ! fuse->second.empty())
        {
            try
            {
                for(const auto & share : JsonContentString(Tools::unescaped(fuse->second)).toArray().toStdList<std::string>())
                {
                    startFuseListener(xvfb, share);
                }
            }
            catch(...)
            {
                Application::warning("%s: invalid json array: `%s'", __FUNCTION__, fuse->second.c_str());
            }
        }
    }

    void DBusAdaptor::stopSessionChannels(XvfbSessionPtr xvfb)
    {
        if(0 < xvfb->connectorId)
        {
            auto fuse = xvfb->options.find("redirect:fuse");

            if(xvfb->options.end() != fuse && ! fuse->second.empty())
            {
                for(const auto & share : JsonContentString(Tools::unescaped(fuse->second)).toArray().toStdList<std::string>())
                {
                    stopFuseListener(xvfb, share);
                }
            }

            auto audio = xvfb->options.find("redirect:audio");

            if(xvfb->options.end() != audio)
            {
                stopAudioListener(xvfb, audio->second);
            }

            auto pcsc = xvfb->options.find("redirect:pcsc");

            if(xvfb->options.end() != pcsc)
            {
                stopPcscListener(xvfb, pcsc->second);
            }
        }
    }

    void DBusAdaptor::startLoginChannels(XvfbSessionPtr xvfb)
    {
    }

    void DBusAdaptor::stopLoginChannels(XvfbSessionPtr xvfb)
    {
        if(0 < xvfb->connectorId)
        {
            auto pkcs11 = xvfb->options.find("pkcs11:auth");

            if(xvfb->options.end() != pkcs11)
            {
                stopPkcs11Listener(xvfb, pkcs11->second);
            }
        }
    }

    bool DBusAdaptor::startPrinterListener(XvfbSessionPtr xvfb, const std::string & clientUrl)
    {
        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectPrinter))
        {
            Application::warning("%s: display %" PRId32 ", redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "printer");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("printer"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: url: %s", __FUNCTION__, clientUrl.c_str());
        auto[clientType, clientAddress] = Channel::parseUrl(clientUrl);

        if(clientType == Channel::ConnectorType::Unknown)
        {
            Application::error("%s: %s, unknown client url: %s", __FUNCTION__, "printer", clientUrl.c_str());
            return false;
        }

        auto printerSocket = configGetString("channel:printer:format", "/var/run/ltsm/cups/printer_%{user}");
        auto socketFolder = std::filesystem::path(printerSocket).parent_path();
        auto lp = Tools::getGroupGid("lp");
        std::error_code err;

        if(! std::filesystem::is_directory(socketFolder, err) &&
                ! std::filesystem::create_directories(socketFolder, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, "create directory failed", socketFolder.c_str(), getuid());
            return false;
        }

        // fix mode 0750
        std::filesystem::permissions(socketFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), socketFolder.c_str(), getuid());
        }

        // fix owner xvfb.lp
        Tools::setFileOwner(socketFolder, Tools::getUserUid(ltsm_user_conn), lp);
        printerSocket = Tools::replace(printerSocket, "%{user}", xvfb->userInfo->user());

        if(std::filesystem::is_socket(printerSocket, err))
        {
            std::filesystem::remove(printerSocket, err);
        }

        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, printerSocket);
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::WriteOnly),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadOnly), "medium", 5,
                           static_cast<uint32_t>(Channel::OptsFlags::ZLibCompression));
        // fix permissions job
        std::thread(fixPermissionJob, printerSocket, xvfb->userInfo->uid(), lp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP).detach();
        return true;
    }

    bool startAudioSessionJob(DBusAdaptor* owner, XvfbSessionPtr xvfb, std::string audioSocket)
    {
        // wait new session started
        while(xvfb->sessionOnlinedSec() < std::chrono::seconds(2))
        {
            std::this_thread::sleep_for(550ms);
        }

        Application::info("%s: display: %" PRId32 ", user: %s, socket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), audioSocket.c_str());

#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = getSessionDBusAddresses(*xvfb->userInfo, xvfb->displayNum);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: %s, display: %" PRId32 ", user: %s",
                                     __FUNCTION__, "dbus address empty", xvfb->displayNum, xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto addr = Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";");
            Application::debug(DebugType::App, "%s: dbus address: `%s'", __FUNCTION__, addr.c_str());

            auto conn = sdbus::createSessionBusConnectionWithAddress(addr);
#ifdef SDBUS_2_0_API
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), sdbus::ServiceName {dbus_session_audio_name}, sdbus::ObjectPath {dbus_session_audio_path});
            auto method1 = concatenatorProxy->createMethodCall(sdbus::InterfaceName{dbus_session_audio_ifce}, sdbus::MethodName{"getVersion"});
#else
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), dbus_session_audio_name, dbus_session_audio_path);
            auto method1 = concatenatorProxy->createMethodCall(dbus_session_audio_ifce, "getVersion");
#endif
            auto reply1 = concatenatorProxy->callMethod(method1);
            int32_t version = 0;
            concatenatorProxy->callMethod("getVersion").onInterface(dbus_session_audio_ifce).storeResultsTo(version);

            if(version < LTSM_SESSION_AUDIO_VERSION)
            {
                Application::error("%s: unsupported %s, version: %" PRId32, __FUNCTION__, "session_audio", version);
                throw service_error(NS_FuncName);
            }

            bool ret = false;
            concatenatorProxy->callMethod("connectChannel").onInterface(dbus_session_audio_ifce).withArguments(audioSocket).storeResultsTo(ret);

            if(! ret)
            {
                Application::error("%s: %s failed", __FUNCTION__, "audio session connect");
            }

            return ret;
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %" PRId32 ", sdbus error: %s, msg: %s",
                               __FUNCTION__, xvfb->displayNum, err.getName().c_str(), err.getMessage().c_str());
        }
        catch(std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

#else
        Application::warning("%s: sdbus address not supported, use 1.2 version", __FUNCTION__);
#endif
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, audioSocket);
        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Audio, "");
        owner->emitDestroyListener(xvfb->displayNum, clientUrl, serverUrl);
        return false;
    }

    bool DBusAdaptor::startAudioListener(XvfbSessionPtr xvfb, const std::string & encoding)
    {
        if(xvfb->mode == SessionMode::Login)
        {
            Application::error("%s: login session skipped, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectAudio))
        {
            Application::warning("%s: display %" PRId32 ", redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "audio");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("audio"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: encoding: %s", __FUNCTION__, encoding.c_str());
        auto audioFormat = configGetString("channel:audio:format", "/var/run/ltsm/audio/%{user}");
        auto audioFolder = std::filesystem::path(Tools::replace(audioFormat, "%{user}", xvfb->userInfo->user()));
        std::error_code err;

        if(! std::filesystem::is_directory(audioFolder, err) &&
                ! std::filesystem::create_directories(audioFolder, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, "create directory failed", audioFolder.c_str(), getuid());
            return false;
        }

        // fix mode 0750
        std::filesystem::permissions(audioFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), audioFolder.c_str(), getuid());
        }

        // fix owner xvfb.user
        Tools::setFileOwner(audioFolder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid());
        auto audioSocket = std::filesystem::path(audioFolder) / std::to_string(xvfb->connectorId);
        audioSocket += ".sock";

        if(std::filesystem::is_socket(audioSocket, err))
        {
            std::filesystem::remove(audioSocket, err);
        }

        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Audio, "");
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, audioSocket.native());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "fast", 5, 0);
        // fix permissions job
        std::thread(fixPermissionJob, audioSocket, xvfb->userInfo->uid(), xvfb->userInfo->gid(), S_IRUSR | S_IWUSR).detach();
        // start session audio helper
        std::thread(startAudioSessionJob, this, std::move(xvfb), audioSocket.native()).detach();
        return true;
    }

    void DBusAdaptor::stopAudioListener(XvfbSessionPtr xvfb, const std::string & encoding)
    {
        Application::info("%s: encoding: %s", __FUNCTION__, encoding.c_str());
        auto audioFormat = configGetString("channel:audio:format", "/var/run/ltsm/audio/%{user}");
        auto audioFolder = std::filesystem::path(Tools::replace(audioFormat, "%{user}", xvfb->userInfo->user()));
        auto audioSocket = std::filesystem::path(audioFolder) / std::to_string(xvfb->connectorId);
        audioSocket += ".sock";
        Application::info("%s: display: %" PRId32 ", user: %s, socket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), audioSocket.c_str());
#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = getSessionDBusAddresses(*xvfb->userInfo, xvfb->displayNum);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: %s, display: %" PRId32 ", user: %s",
                                     __FUNCTION__, "dbus address empty", xvfb->displayNum, xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto addr = Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";");
            Application::debug(DebugType::App, "%s: dbus address: `%s'", __FUNCTION__, addr.c_str());

            auto conn = sdbus::createSessionBusConnectionWithAddress(addr);
#ifdef SDBUS_2_0_API
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), sdbus::ServiceName {dbus_session_audio_name}, sdbus::ObjectPath {dbus_session_audio_path});
#else
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), dbus_session_audio_name, dbus_session_audio_path);
#endif
            concatenatorProxy->callMethod("disconnectChannel").onInterface(dbus_session_audio_ifce).withArguments(
                audioSocket.native()).dontExpectReply();
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %" PRId32 ", sdbus error: %s, msg: %s", __FUNCTION__, xvfb->displayNum,
                               err.getName().c_str(), err.getMessage().c_str());
        }
        catch(std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

#else
        Application::warning("%s: sdbus address not supported, use 1.2 version", __FUNCTION__);
#endif
    }

    bool DBusAdaptor::startSaneListener(XvfbSessionPtr xvfb, const std::string & clientUrl)
    {
        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectScanner))
        {
            Application::warning("%s: display %" PRId32 ", redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "scanner");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("scanner"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: url: %s", __FUNCTION__, clientUrl.c_str());
        auto[clientType, clientAddress] = Channel::parseUrl(clientUrl);

        if(clientType == Channel::ConnectorType::Unknown)
        {
            Application::error("%s: %s, unknown client url: %s", __FUNCTION__, "sane", clientUrl.c_str());
            return false;
        }

        auto saneSocket = configGetString("channel:sane:format", "/var/run/ltsm/sane/%{user}");
        auto socketFolder = std::filesystem::path(saneSocket).parent_path();
        std::error_code err;

        if(! std::filesystem::is_directory(socketFolder, err) &&
                ! std::filesystem::create_directories(socketFolder, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, "create directory failed", socketFolder.c_str(), getuid());
            return false;
        }

        // fix mode 0750
        std::filesystem::permissions(socketFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), socketFolder.c_str(), getuid());
        }

        // fix owner xvfb.user
        Tools::setFileOwner(socketFolder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid());
        saneSocket = Tools::replace(saneSocket, "%{user}", xvfb->userInfo->user());

        if(std::filesystem::is_socket(saneSocket, err))
        {
            std::filesystem::remove(saneSocket, err);
        }

        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, saneSocket);
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "medium", 5,
                           static_cast<uint32_t>(Channel::OptsFlags::ZLibCompression));
        // fix permissions job
        std::thread(fixPermissionJob, saneSocket, xvfb->userInfo->uid(), xvfb->userInfo->gid(),
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP).detach();
        return true;
    }

    bool startPcscSessionJob(DBusAdaptor* owner, XvfbSessionPtr xvfb, std::string pcscSocket)
    {
        // wait new session started
        while(xvfb->sessionOnlinedSec() < std::chrono::seconds(2))
        {
            std::this_thread::sleep_for(550ms);
        }

        Application::info("%s: display: %" PRId32 ", user: %s, socket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), pcscSocket.c_str());

#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = getSessionDBusAddresses(*xvfb->userInfo, xvfb->displayNum);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: dbus address empty, display: %" PRId32 ", user: %s",
                                     __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto addr = Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";");
            Application::debug(DebugType::App, "%s: dbus address: `%s'", __FUNCTION__, addr.c_str());

            auto conn = sdbus::createSessionBusConnectionWithAddress(addr);
#ifdef SDBUS_2_0_API
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), sdbus::ServiceName {dbus_session_pcsc_name}, sdbus::ObjectPath {dbus_session_pcsc_path});
            auto method1 = concatenatorProxy->createMethodCall(sdbus::InterfaceName{dbus_session_pcsc_ifce}, sdbus::MethodName{"getVersion"});
#else
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), dbus_session_pcsc_name, dbus_session_pcsc_path);
            auto method1 = concatenatorProxy->createMethodCall(dbus_session_pcsc_ifce, "getVersion");
#endif
            auto reply1 = concatenatorProxy->callMethod(method1);
            int32_t version = 0;
            concatenatorProxy->callMethod("getVersion").onInterface(dbus_session_pcsc_ifce).storeResultsTo(version);

            if(version < LTSM_SESSION_PCSC_VERSION)
            {
                Application::error("%s: unsupported %s, version: %" PRId32, __FUNCTION__, "session_pcsc", version);
                throw service_error(NS_FuncName);
            }

            bool ret = false;
            concatenatorProxy->callMethod("connectChannel").onInterface(dbus_session_pcsc_ifce).withArguments(pcscSocket).storeResultsTo(ret);

            if(! ret)
            {
                Application::error("%s: %s failed", __FUNCTION__, "pcsc session connect");
            }

            return ret;
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %" PRId32 ", sdbus error: %s, msg: %s",
                               __FUNCTION__, xvfb->displayNum, err.getName().c_str(), err.getMessage().c_str());
        }
        catch(std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

#else
        Application::warning("%s: sdbus address not supported, use 1.2 version", __FUNCTION__);
#endif
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, pcscSocket);
        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Pcsc, "");
        owner->emitDestroyListener(xvfb->displayNum, clientUrl, serverUrl);
        return false;
    }

    bool DBusAdaptor::startPcscListener(XvfbSessionPtr xvfb, const std::string & param)
    {
        if(xvfb->mode == SessionMode::Login)
        {
            Application::error("%s: login session skipped, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectPcsc))
        {
            Application::warning("%s: display %" PRId32 ", redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "pcsc");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("pcsc"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: param: `%s'", __FUNCTION__, param.c_str());
        auto pcscFormat = configGetString("channel:pcsc:format", "/var/run/ltsm/pcsc/%{user}");
        auto pcscFolder = std::filesystem::path(Tools::replace(pcscFormat, "%{user}", xvfb->userInfo->user()));
        std::error_code err;

        if(! std::filesystem::is_directory(pcscFolder, err) &&
                ! std::filesystem::create_directories(pcscFolder, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, "create directory failed", pcscFolder.c_str(), getuid());
            return false;
        }

        // fix mode 0750
        std::filesystem::permissions(pcscFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), pcscFolder.c_str(), getuid());
        }

        // fix owner xvfb.user
        Tools::setFileOwner(pcscFolder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid());
        auto pcscSocket = std::filesystem::path(pcscFolder) / "sock";

        if(std::filesystem::is_socket(pcscSocket, err))
        {
            std::filesystem::remove(pcscSocket, err);
        }

        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Pcsc, "");
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, pcscSocket.native());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "medium", 5, 0);
        // fix permissions job
        std::thread(fixPermissionJob, pcscSocket, xvfb->userInfo->uid(), xvfb->userInfo->gid(), S_IRUSR | S_IWUSR).detach();
        // start session pcsc helper
        std::thread(startPcscSessionJob, this, std::move(xvfb), pcscSocket.native()).detach();
        return true;
    }

    void DBusAdaptor::stopPcscListener(XvfbSessionPtr xvfb, const std::string & param)
    {
        Application::info("%s: param: `%s'", __FUNCTION__, param.c_str());
        auto pcscFormat = configGetString("channel:pcsc:format", "/var/run/ltsm/pcsc/%{user}");
        auto pcscFolder = std::filesystem::path(Tools::replace(pcscFormat, "%{user}", xvfb->userInfo->user()));
        auto pcscSocket = std::filesystem::path(pcscFolder) / "sock";

        Application::info("%s: display: %" PRId32 ", user: %s, socket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), pcscSocket.c_str());

#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = getSessionDBusAddresses(*xvfb->userInfo, xvfb->displayNum);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: %s, display: %" PRId32 ", user: %s",
                                     __FUNCTION__, "dbus address empty", xvfb->displayNum, xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto addr = Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";");
            Application::debug(DebugType::App, "%s: dbus address: `%s'", __FUNCTION__, addr.c_str());

            auto conn = sdbus::createSessionBusConnectionWithAddress(addr);
#ifdef SDBUS_2_0_API
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), sdbus::ServiceName {dbus_session_pcsc_name}, sdbus::ObjectPath {dbus_session_pcsc_path});
#else
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), dbus_session_pcsc_name, dbus_session_pcsc_path);
#endif
            concatenatorProxy->callMethod("disconnectChannel").onInterface(dbus_session_pcsc_ifce).withArguments(
                pcscSocket.native()).dontExpectReply();
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %" PRId32 ", sdbus error: %s, msg: %s",
                               __FUNCTION__, xvfb->displayNum, err.getName().c_str(), err.getMessage().c_str());
        }
        catch(std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

#else
        Application::warning("%s: sdbus address not supported, use 1.2 version", __FUNCTION__);
#endif
    }

    bool DBusAdaptor::startPkcs11Listener(XvfbSessionPtr xvfb, const std::string & param)
    {
        if(xvfb->mode != SessionMode::Login)
        {
            Application::warning("%s: login session only, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
            return false;
        }

        Application::info("%s: param: `%s'", __FUNCTION__, param.c_str());
        auto pkcs11Format = configGetString("channel:pkcs11:format", "/var/run/ltsm/pkcs11/%{display}");
        auto pkcs11Folder = std::filesystem::path(Tools::replace(pkcs11Format, "%{display}", xvfb->displayNum));
        std::error_code err;

        if(! std::filesystem::is_directory(pkcs11Folder, err) &&
                ! std::filesystem::create_directories(pkcs11Folder, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, "create directory failed", pkcs11Folder.c_str(), getuid());
            return false;
        }

        // fix mode 0750
        std::filesystem::permissions(pkcs11Folder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), pkcs11Folder.c_str(), getuid());
        }

        // fix owner xvfb.user
        Tools::setFileOwner(pkcs11Folder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid());
        auto pkcs11Socket = std::filesystem::path(pkcs11Folder) / "sock";

        if(std::filesystem::is_socket(pkcs11Socket, err))
        {
            std::filesystem::remove(pkcs11Socket, err);
        }

        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Pkcs11, "");
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, pkcs11Socket.native());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "slow", 5,
                           static_cast<uint32_t>(Channel::OptsFlags::AllowLoginSession));
        // fix permissions job
        std::thread(fixPermissionJob, pkcs11Socket, xvfb->userInfo->uid(), xvfb->userInfo->gid(), S_IRUSR | S_IWUSR).detach();
        return true;
    }

    void DBusAdaptor::stopPkcs11Listener(XvfbSessionPtr xvfb, const std::string & param)
    {
        Application::info("%s: param: `%s'", __FUNCTION__, param.c_str());
    }

    bool startFuseSessionJob(DBusAdaptor* owner, XvfbSessionPtr xvfb, std::string localPoint, std::string remotePoint,
                             std::string fuseSocket)
    {
        // wait new session started
        while(xvfb->sessionOnlinedSec() < std::chrono::seconds(2))
        {
            std::this_thread::sleep_for(550ms);
        }

        Application::info("%s: display: %" PRId32 ", user: %s, local: `%s', remote: `%s', socket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), localPoint.c_str(), remotePoint.c_str(), fuseSocket.c_str());

#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = getSessionDBusAddresses(*xvfb->userInfo, xvfb->displayNum);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: %s, display: %" PRId32 ", user: %s",
                                     __FUNCTION__, "dbus address empty", xvfb->displayNum, xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto addr = Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";");
            Application::debug(DebugType::App, "%s: dbus address: `%s'", __FUNCTION__, addr.c_str());

            auto conn = sdbus::createSessionBusConnectionWithAddress(addr);
#ifdef SDBUS_2_0_API
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), sdbus::ServiceName {dbus_session_fuse_name}, sdbus::ObjectPath {dbus_session_fuse_path});
            auto method1 = concatenatorProxy->createMethodCall(sdbus::InterfaceName{dbus_session_fuse_ifce}, sdbus::MethodName{"getVersion"});
#else
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), dbus_session_fuse_name, dbus_session_fuse_path);
            auto method1 = concatenatorProxy->createMethodCall(dbus_session_fuse_ifce, "getVersion");
#endif
            auto reply1 = concatenatorProxy->callMethod(method1);
            int32_t version = 0;
            concatenatorProxy->callMethod("getVersion").onInterface(dbus_session_fuse_ifce).storeResultsTo(version);

            if(version < LTSM_SESSION_FUSE_VERSION)
            {
                Application::error("%s: unsupported %s, version: %" PRId32, __FUNCTION__, "session_fuse", version);
                throw service_error(NS_FuncName);
            }

            bool ret = false;
            concatenatorProxy->callMethod("mountPoint").onInterface(dbus_session_fuse_ifce).withArguments(localPoint, remotePoint,
                    fuseSocket).storeResultsTo(ret);

            if(! ret)
            {
                Application::error("%s: %s failed", __FUNCTION__, "fuse session mount");
            }

            return ret;
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %" PRId32 ", sdbus error: %s, msg: %s",
                               __FUNCTION__, xvfb->displayNum, err.getName().c_str(), err.getMessage().c_str());
        }
        catch(std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

#else
        Application::warning("%s: sdbus address not supported, use 1.2 version", __FUNCTION__);
#endif
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, fuseSocket);
        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Fuse, "");
        owner->emitDestroyListener(xvfb->displayNum, clientUrl, serverUrl);
        return false;
    }

    bool DBusAdaptor::startFuseListener(XvfbSessionPtr xvfb, const std::string & remotePoint)
    {
        if(xvfb->mode == SessionMode::Login)
        {
            Application::error("%s: login session skipped, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RemoteFilesUse))
        {
            Application::warning("%s: display %" PRId32 ", redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "fuse");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("fuse"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: remote point: %s", __FUNCTION__, remotePoint.c_str());
        auto userShareFormat = configGetString("channel:fuse:format", "/var/run/ltsm/fuse/%{user}");
        auto userShareFolder = Tools::replace(userShareFormat, "%{user}", xvfb->userInfo->user());
        auto fusePointName = std::filesystem::path(remotePoint).filename();
        auto fusePointFolder = std::filesystem::path(userShareFolder) / fusePointName;
        std::error_code err;

        if(! std::filesystem::is_directory(fusePointFolder, err) &&
                ! std::filesystem::create_directories(fusePointFolder, err))
        {
            Application::error("%s: %s, path: `%s', uid: %" PRId32,
                               __FUNCTION__, "create directory failed", fusePointFolder.c_str(), getuid());
            return false;
        }

        // fix mode 0750
        std::filesystem::permissions(userShareFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), fusePointFolder.c_str(), getuid());
        }

        // fix owner xvfb.user
        Tools::setFileOwner(userShareFolder, Tools::getUserUid(ltsm_user_conn), xvfb->userInfo->gid());
        // fix mode 0700
        std::filesystem::permissions(fusePointFolder, std::filesystem::perms::group_all | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), fusePointFolder.c_str(), getuid());
        }

        // fix owner user.user
        Tools::setFileOwner(fusePointFolder, xvfb->userInfo->uid(), xvfb->userInfo->gid());
        auto fuseSocket = std::filesystem::path(userShareFolder) / fusePointName;
        fuseSocket += ".sock";

        if(std::filesystem::is_socket(fuseSocket, err))
        {
            std::filesystem::remove(fuseSocket, err);
        }

        auto clientUrl = Channel::createUrl(Channel::ConnectorType::Fuse, "");
        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, fuseSocket.native());
        emitCreateListener(xvfb->displayNum, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                           serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite), "fast", 5, 0);
        // fix permissions job
        std::thread(fixPermissionJob, fuseSocket, xvfb->userInfo->uid(), xvfb->userInfo->gid(), S_IRUSR | S_IWUSR).detach();
        // start session fuse helper
        std::thread(startFuseSessionJob, this, std::move(xvfb), fusePointFolder.native(), remotePoint,
                    fuseSocket.native()).detach();
        return true;
    }

    void DBusAdaptor::stopFuseListener(XvfbSessionPtr xvfb, const std::string & remotePoint)
    {
        auto userShareFormat = configGetString("channel:fuse:format", "/var/run/ltsm/fuse/%{user}");
        auto userShareFolder = Tools::replace(userShareFormat, "%{user}", xvfb->userInfo->user());
        auto fusePointName = std::filesystem::path(remotePoint).filename();
        auto fusePointFolder = std::filesystem::path(userShareFolder) / fusePointName;
        auto localPoint = fusePointFolder.native();

        Application::info("%s: display: %" PRId32 ", user: %s, local point: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), localPoint.c_str());

#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = getSessionDBusAddresses(*xvfb->userInfo, xvfb->displayNum);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: %s, display: %" PRId32 ", user: %s",
                                     __FUNCTION__, "dbus address empty", xvfb->displayNum, xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto addr = Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";");
            Application::debug(DebugType::App, "%s: dbus address: `%s'", __FUNCTION__, addr.c_str());

            auto conn = sdbus::createSessionBusConnectionWithAddress(addr);
#ifdef SDBUS_2_0_API
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), sdbus::ServiceName {dbus_session_fuse_name}, sdbus::ObjectPath {dbus_session_fuse_path});
#else
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), dbus_session_fuse_name, dbus_session_fuse_path);
#endif
            concatenatorProxy->callMethod("umountPoint").onInterface(dbus_session_fuse_ifce).withArguments(localPoint).dontExpectReply();
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %" PRId32 ", sdbus error: %s, msg: %s",
                               __FUNCTION__, xvfb->displayNum, err.getName().c_str(), err.getMessage().c_str());
        }
        catch(std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

#else
        Application::warning("%s: sdbus address not supported, use 1.2 version", __FUNCTION__);
#endif
    }

    void DBusAdaptor::busSetDebugLevel(const std::string & level)
    {
        Application::debug(DebugType::Dbus, "%s: level: %s", __FUNCTION__, level.c_str());
        Application::setDebugLevel(level);
    }

    void DBusAdaptor::busSetChannelDebug(const int32_t & display, const uint8_t & channel, const bool & debug)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", channel: %" PRIu8 ", debug: %s",
                           __FUNCTION__, display, channel, (debug ? "true" : "false"));
        emitDebugChannel(display, channel, debug);
    }

    std::string DBusAdaptor::busEncryptionInfo(const int32_t & display)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32,
                           __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display))
        {
            return xvfb->encryption;
        }

        Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        return "none";
    }

    void DBusAdaptor::busDisplayResized(const int32_t & display, const uint16_t & width, const uint16_t & height)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", width: %" PRIu16 ", height: %" PRIu16,
                           __FUNCTION__, display, width, height);

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->width = width;
            xvfb->height = height;
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetEncryptionInfo(const int32_t & display, const std::string & info)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", encryption: %s", __FUNCTION__, display, info.c_str());

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->encryption = info;
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionDurationLimitSec(const int32_t & display, const uint32_t & limit)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", limit: %" PRIu32, __FUNCTION__, display, limit);

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->startTimeLimitSec = limit;
            emitClearRenderPrimitives(display);
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionOnlineLimitSec(const int32_t & display, const uint32_t & limit)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", limit: %" PRIu32, __FUNCTION__, display, limit);

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->onlineTimeLimitSec = limit;
            emitClearRenderPrimitives(display);
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionOfflineLimitSec(const int32_t & display, const uint32_t & limit)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", limit: %" PRIu32, __FUNCTION__, display, limit);

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->offlineTimeLimitSec = limit;
            emitClearRenderPrimitives(display);
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::busSetSessionPolicy(const int32_t & display, const std::string & policy)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32 ", policy: %s", __FUNCTION__, display, policy.c_str());

        if(auto xvfb = findDisplaySession(display))
        {
            if(Tools::lower(policy) == "authlock")
            {
                xvfb->policy = SessionPolicy::AuthLock;
            }
            else if(Tools::lower(policy) == "authtake")
            {
                xvfb->policy = SessionPolicy::AuthTake;
            }
            else if(Tools::lower(policy) == "authshare")
            {
                xvfb->policy = SessionPolicy::AuthShare;
            }
            else
            {
                Application::warning("%s: %s, display: %" PRId32 ", policy: %s", __FUNCTION__, "unknown value", display, policy.c_str());
            }
        }
        else
        {
            Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        }
    }

    void DBusAdaptor::helperSetSessionLoginPassword(const int32_t & display, const std::string & login,
            const std::string & password, const bool & action)
    {
        Application::info("%s: display: %" PRId32 ", user: %s", __FUNCTION__, display, login.c_str());

        std::thread([this, display, login, password, action]()
        {
            std::this_thread::sleep_for(10ms);
            this->emitHelperSetLoginPassword(display, login, password, action);
        }).detach();
    }

    std::string DBusAdaptor::busGetSessionJson(const int32_t & display)
    {
        Application::debug(DebugType::Dbus, "%s: display: %" PRId32, __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display))
        {
            return xvfb->toJsonString();
        }

        Application::warning("%s: display not found: %" PRId32, __FUNCTION__, display);
        return "{}";
    }

    std::string DBusAdaptor::busGetSessionsJson(void)
    {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);
        return XvfbSessions::toJsonString();
    }

    void DBusAdaptor::busRenderRect(const int32_t & display, const TupleRegion & rect, const TupleColor & color, const bool & fill)
    {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);

        std::thread([this, display, rect, color, fill]()
        {
            std::this_thread::sleep_for(10ms);
            this->emitAddRenderRect(display, rect, color, fill);
        }).detach();
    }

    void DBusAdaptor::busRenderText(const int32_t & display, const std::string & text, const TuplePosition & pos, const TupleColor & color)
    {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);

        std::thread([this, display, text, pos, color]()
        {
            std::this_thread::sleep_for(10ms);
            this->emitAddRenderText(display, text, pos, color);
        }).detach();
    }

    void DBusAdaptor::busRenderClear(const int32_t & display)
    {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);

        std::thread([this, display]()
        {
            std::this_thread::sleep_for(10ms);
            this->emitClearRenderPrimitives(display);
        }).detach();
    }

    bool DBusAdaptor::busCreateChannel(const int32_t & display, const std::string & client, const std::string & cmode,
                                       const std::string & server, const std::string & smode, const std::string & speed)
    {
        Application::debug(DebugType::Dbus, "%s:, display: %" PRId32 ", client: (%s, %s), server: (%s, %s), speed: %s",
                           __FUNCTION__, display, client.c_str(), cmode.c_str(), server.c_str(), smode.c_str(), speed.c_str());

        auto modes = { "ro", "rw", "wo" };

        if(std::none_of(modes.begin(), modes.end(), [&](auto & val) { return cmode == val; }))
        {
            Application::error("%s: incorrect %s mode: %s", __FUNCTION__, "client", cmode.c_str());
            return false;
        }

        if(std::none_of(modes.begin(), modes.end(), [&](auto & val) { return smode == val; }))
        {
            Application::error("%s: incorrect %s mode: %s", __FUNCTION__, "server", smode.c_str());
            return false;
        }

        emitCreateChannel(display, client, cmode, server, smode, speed);
        return true;
    }

    bool DBusAdaptor::busDestroyChannel(const int32_t & display, const uint8_t & channel)
    {
        Application::debug(DebugType::Dbus, "%s:, display: %" PRId32 ", channel: 0x%02" PRIx8,
                           __FUNCTION__, display, channel);

        emitDestroyChannel(display, channel);
        return true;
    }

    bool DBusAdaptor::createXauthDir(void)
    {
        auto xauthFile = configGetString("xauth:file", "/var/run/ltsm/auth_%{display}");

        // find group id
        gid_t setgid = Tools::getGroupGid(ltsm_group_auth);
        // check directory
        auto folderPath = std::filesystem::path(xauthFile).parent_path();

        if(folderPath.empty())
        {
            Application::error("%s: path not found: `%s'",
                               __FUNCTION__, folderPath.c_str());
            return false;
        }

        std::error_code err;

        // create
        if(! std::filesystem::is_directory(folderPath, err))
        {
            if(! std::filesystem::create_directory(folderPath, err))
            {
                Application::error("%s: %s, path: `%s', uid: %" PRId32,
                                   __FUNCTION__, "create directory failed", folderPath.c_str(), getuid());
                return false;
            }
        }

        // fix mode 0755
        std::filesystem::permissions(folderPath, std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_read | std::filesystem::perms::others_exec, std::filesystem::perm_options::replace, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %" PRId32,
                                 __FUNCTION__, err.message().c_str(), folderPath.c_str(), getuid());
        }

        Tools::setFileOwner(folderPath, 0, setgid);
        return true;
    }

    /* SystemService */
    SystemService::SystemService(int argc, const char** argv)
    {
        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--background"))
            {
                isBackground = true;
            }
            else if(0 == std::strcmp(argv[it], "--config") && it + 1 < argc)
            {
                config.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            {
                std::cout << "usage: " << argv[0] << " [--config file] [--background]" << std::endl;
                throw 0;
            }
        }

    }

    int SystemService::start(void)
    {
        if(isBackground && fork())
        {
            return 0;
        }

        if(0 < getuid())
        {
            std::cerr << "need root privileges" << std::endl;
            return EXIT_FAILURE;
        }

#ifdef SDBUS_2_0_API
        serviceConn = sdbus::createSystemBusConnection(sdbus::ServiceName {LTSM::dbus_manager_service_name});
#else
        serviceConn = sdbus::createSystemBusConnection(LTSM::dbus_manager_service_name);
#endif

        if(! serviceConn)
        {
            Application::error("%s: %s failed",
                               "ServiceStart", "dbus connection");
            return EXIT_FAILURE;
        }

        auto connectorHome = Tools::getUserHome(ltsm_user_conn);

        if(! std::filesystem::is_directory(connectorHome))
        {
            Application::error("%s: path not found: `%s'", __FUNCTION__, connectorHome.c_str());
            return EXIT_FAILURE;
        }

        // remove old sockets
        for(auto const & dirEntry : std::filesystem::directory_iterator{connectorHome})
        {
            if(dirEntry.is_socket())
            {
                std::filesystem::remove(dirEntry);
            }
        }

        signal(SIGTERM, signalHandler);
        //signal(SIGCHLD, signalHandler);
        signal(SIGINT, isBackground ? SIG_IGN : signalHandler);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        serviceAdaptor = std::make_unique<DBusAdaptor>(*serviceConn, config);
        Application::notice("%s: service started, uid: %d, pid: %d, version: %d", "ServiceStart", getuid(), getpid(), LTSM::service_version);

#ifdef WITH_SYSTEMD
        sd_notify(0, "READY=1");
#endif

        serviceConn->enterEventLoop();

#ifdef WITH_SYSTEMD
        sd_notify(0, "STOPPING=1");
#endif
        Application::notice("%s: service stopped", "ServiceStart");
        serviceAdaptor.reset();

        return EXIT_SUCCESS;
    }
}

int main(int argc, const char** argv)
{
    int res = 0;

    try
    {
        LTSM::Manager::SystemService app(argc, argv);
        res = app.start();
    }
    catch(const sdbus::Error & err)
    {
        LTSM::Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
