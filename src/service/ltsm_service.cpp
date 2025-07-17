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
#include <sys/inotify.h>
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

using namespace std::chrono_literals;

namespace LTSM
{
    // Manager
    namespace Manager
    {
        std::atomic<bool> serviceRunning = false;
        std::atomic<bool> serviceKilled = false;
        std::unique_ptr<Object> serviceAdaptor;
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
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_start", pam_strerror(pamh, status),
                                   status);
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
                free(pr->resp);

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

                if(0 == strncasecmp(msg, "login:", 6))
                    return strdup(login.c_str());

                break;

            case PAM_PROMPT_ECHO_OFF:
                Application::info("%s: style: `%s', msg: `%s'", __FUNCTION__, "PAM_PROMPT_ECHO_OFF", msg);

                if(0 == strncasecmp(msg, "password:", 9))
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
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_authenticate", pam_strerror(pamh, status),
                               status);
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
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_chauthtok", pam_strerror(pamh, status),
                                   status);
                return false;
            }
        }
        else if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_acct_mgmt", pam_strerror(pamh, status),
                               status);
            return false;
        }

        return true;
    }

    bool PamSession::refreshCreds(void)
    {
        status = pam_setcred(pamh, PAM_REFRESH_CRED);

        if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_setcred", pam_strerror(pamh, status),
                               status);
            return false;
        }

        return true;
    }

    bool PamSession::openSession(void)
    {
        status = pam_setcred(pamh, PAM_ESTABLISH_CRED);

        if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_setcred", pam_strerror(pamh, status),
                               status);
            return false;
        }

        status = pam_open_session(pamh, 0);

        if(PAM_SUCCESS != status)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_open_session", pam_strerror(pamh, status),
                               status);
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
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_setcred", pam_strerror(pamh, status),
                               status);
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
            Application::debug(DebugType::Mgr, "%s: kill %s, pid: %d", "destroySession", "helper", pid2);
            kill(pid2, SIGTERM);
        }

        if(0 < pid1)
        {
            int status;
            // kill xvfb
            Application::debug(DebugType::Mgr, "%s: kill %s, pid: %d", "destroySession", "xvfb", pid1);
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
        int sesmode = 0, conpol = 0;

        switch(mode)
        {
            case XvfbMode::SessionOnline:
                sesmode = 1;
                break;

            case XvfbMode::SessionSleep:
                sesmode = 2;
                break;

            default:
                break;
        }

        switch(policy)
        {
            case SessionPolicy::AuthTake:
                conpol = 1;
                break;

            case SessionPolicy::AuthShare:
                conpol = 2;
                break;

            default:
                break;
        }

        JsonObjectStream jos;
        jos.push("displaynum", displayNum);
        jos.push("pid1", pid1);
        jos.push("pid2", pid2);
        jos.push("width", width);
        jos.push("height", height);
        jos.push("uid", static_cast<int>(userInfo->uid()));
        jos.push("gid", static_cast<int>(userInfo->gid()));
        jos.push("durationlimit", durationLimit);
        jos.push("sesmode", sesmode);
        jos.push("conpol", conpol);
        jos.push("user", userInfo->user());
        jos.push("xauthfile", xauthfile);
        jos.push("remoteaddr", remoteAddr);
        jos.push("conntype", conntype);
        jos.push("encryption", encryption);
        jos.push("alivesec", static_cast<size_t>(aliveSec().count()));
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
                   (ptr->mode == XvfbMode::SessionOnline || ptr->mode == XvfbMode::SessionSleep) &&
                   username == ptr->userInfo->user();
        });

        return it != sessions.end() ? *it : nullptr;
    }

    XvfbSessionPtr XvfbSessions::findDisplaySession(int screen)
    {
        std::scoped_lock guard{ lockSessions };
        auto it = std::find_if(sessions.begin(), sessions.end(), [ = ](auto & ptr)
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
            if(ptr && 0 < ptr->durationLimit)
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
            if(ptr && ptr->mode == XvfbMode::SessionOnline)
            {
                res.push_front(ptr);
            }
        }

        return res;
    }

    void XvfbSessions::removeDisplaySession(int screen)
    {
        std::scoped_lock guard{ lockSessions };
        auto it = std::find_if(sessions.begin(), sessions.end(), [ = ](auto & ptr)
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
            if(ptr)
            {
                jas.push(ptr->toJsonString());
            }

        return jas.flush();
    }

    std::forward_list<std::string> Manager::getSessionDBusAddresses(const UserInfo & userInfo)
    {
        auto dbusSessionPath = std::filesystem::path(userInfo.home()) / ".dbus" / "session-bus";
        std::forward_list<std::string> dbusAddresses;

        // home may be nfs and deny for root
        try
        {
            if(std::filesystem::is_directory(dbusSessionPath))
            {
                std::string_view dbusLabel = "DBUS_SESSION_BUS_ADDRESS='";

                for(auto const & dirEntry : std::filesystem::directory_iterator{dbusSessionPath})
                {
                    std::ifstream ifs(dirEntry.path());
                    std::string line;

                    while(std::getline(ifs, line))
                    {
                        auto pos = line.find(dbusLabel);

                        if(pos != std::string::npos)
                        {
                            dbusAddresses.emplace_front(line.substr(pos + dbusLabel.size()));
                            // remove last \'
                            dbusAddresses.front().pop_back();
                        }
                    }
                }
            }

            auto dbusBrokerPath = std::filesystem::path("/run/user") / std::to_string(userInfo.uid()) / "bus";

            if(std::filesystem::is_socket(dbusBrokerPath))
            {
                dbusAddresses.emplace_front(std::string("unix:path=").append(dbusBrokerPath.native()));
            }
        }
        catch(const std::filesystem::filesystem_error &)
        {
        }

        return dbusAddresses;
    }

    void Manager::redirectStdoutStderrTo(bool out, bool err, const std::filesystem::path & file)
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
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, "open failed", file.c_str(), getuid());

            if(0 != file.compare(devnull))
            {
                redirectStdoutStderrTo(out, err, devnull);
            }
        }
    }

    void Manager::closefds(std::initializer_list<int> exclude)
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

        for(auto fd: pids)
        {
            if(std::any_of(exclude.begin(), exclude.end(), [&](auto & val){ return val == fd; }))
                continue;

            close(fd);
        }
    }

    bool Manager::checkFileReadable(const std::filesystem::path & path)
    {
        // Application::trace(DebugType::Mgr, "%s: path: `%s'", __FUNCTION__, path.c_str());
        return 0 == access(path.c_str(), R_OK);
    }

    void Manager::setFileOwner(const std::filesystem::path & path, uid_t uid, gid_t gid)
    {
        // Application::trace(DebugType::Mgr, "%s: path: `%s', uid: %d, gid: %d", __FUNCTION__, path.c_str(), uid, gid);

        if(0 != chown(path.c_str(), uid, gid))
        {
            Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "chown", strerror(errno), errno,
                               path.c_str());
        }
    }

    bool Manager::runSystemScript(XvfbSessionPtr xvfb, const std::string & cmd)
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
            Application::debug(DebugType::Mgr, "%s: command: `%s', return code: %d, display: %d", "runSystemScript", str.c_str(), ret,
                               ptr->displayNum);
        }).detach();

        return true;
    }

    std::string Manager::quotedString(std::string_view str)
    {
        // quoted input values
        std::ostringstream os;
        os << std::quoted(str);
        return os.str();
    }

    bool Manager::switchToUser(const UserInfo & userInfo)
    {
        Application::debug(DebugType::Mgr, "%s: pid: %d, uid: %d, gid: %d, home:`%s', shell: `%s'", __FUNCTION__, getpid(), userInfo.uid(), userInfo.gid(),
                           userInfo.home(), userInfo.shell());
        auto xdgRuntimeDir = std::filesystem::path("/run/user") / std::to_string(userInfo.uid());
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
            setFileOwner(xdgRuntimeDir, userInfo.uid(), userInfo.gid());
        }

        // set groups
        auto gids = userInfo.groups();

        if(! gids.empty())
        {
            setgroups(gids.size(), gids.data());
        }

        if(0 != setgid(userInfo.gid()))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "setgid", strerror(errno), errno);
            return false;
        }

        if(0 != setuid(userInfo.uid()))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "setuid", strerror(errno), errno);
            return false;
        }

        if(0 != chdir(userInfo.home()))
        {
            Application::warning("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "chdir", strerror(errno), errno,
                                 userInfo.home());
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
            Application::debug(DebugType::Mgr, "%s: groups: (%s), current dir: `%s'", __FUNCTION__, sgroups.c_str(), cwd.c_str());
        }

        return true;
    }

    /// RunAs namespace
    class RunAs
    {
    public:
        static int waitPid(pid_t pid)
        {
            Application::debug(DebugType::Mgr, "%s: pid: %d", __FUNCTION__, pid);
            // waitpid
            int status;
            int ret = waitpid(pid, &status, 0);

            if(0 > ret)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "waitpid", strerror(errno), errno);
            }
            else
            {
                if(WIFSIGNALED(status))
                {
                    Application::error("%s: process killed, pid: %d", __FUNCTION__, pid);
                }
                else
                {
                    Application::debug(DebugType::Mgr, "%s: process ended, pid: %d, status: %d", __FUNCTION__, pid, status);
                }
            }

            return status;
        }

    private:
        static void childProcess(XvfbSessionPtr xvfb, int pipeout, const std::filesystem::path & cmd,
                                 std::list<std::string> params)
        {
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);

            if(Application::isDebugTarget(DebugTarget::Syslog))
            {
                Application::setDebugTarget(DebugTarget::Quiet);
            }

            Application::info("%s: pid: %d, cmd: `%s %s'", __FUNCTION__, getpid(), cmd.c_str(), Tools::join(params.begin(),
                              params.end(), " ").c_str());

            if(Manager::switchToUser(*xvfb->userInfo))
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
                    if(! val.empty())
                    {
                        argv.push_back(val.c_str());
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
                    Manager::redirectStdoutStderrTo(true, true, logFile.native());
                }
                else
                {
                    // redirect stderr
                    Manager::redirectStdoutStderrTo(false, true, logFile.native());

                    // redirect stdout
                    if(0 > dup2(pipeout, STDOUT_FILENO))
                    {
                        Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "dup2", strerror(errno), errno);
                    }

                    close(pipeout);
                    pipeout = -1;
                }

        	Manager::closefds({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, pipeout});
                int res = execv(cmd.c_str(), (char* const*) argv.data());

                if(res < 0)
                {
                    Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "execv", strerror(errno), errno,
                                       cmd.c_str());
                }
            }
        }

        static StatusStdout jobWaitStdout(pid_t pid, int fd)
        {
            bool error = false;
            bool loop = true;
            const size_t block = 1024;
            std::vector<uint8_t> res(block);
            uint8_t* ptr = res.data();
            size_t last = block;

            while(loop && !error)
            {
                int ret = read(fd, ptr, last);

                if(ret < 0)
                {
                    if(EAGAIN != errno && EINTR != errno)
                    {
                        Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "read", strerror(errno), errno);
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

    public:
        static PidStatusStdout sessionCommandStdout(XvfbSessionPtr xvfb, const std::filesystem::path & cmd,
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
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"),
                                   cmd.c_str(), getuid());
                throw service_error(NS_FuncName);
            }

            Application::info("%s: request for user: %s, display: %d, cmd: `%s'", __FUNCTION__, xvfb->userInfo->user(),
                              xvfb->displayNum, cmd.c_str());

            if(! std::filesystem::is_directory(xvfb->userInfo->home(), err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not directory"),
                                   xvfb->userInfo->home(), getuid());
                throw service_error(NS_FuncName);
            }

            int pipefd[2];

            if(0 > pipe(pipefd))
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pipe", strerror(errno), errno);
                throw service_error(NS_FuncName);
            }

            pid_t pid = fork();

            if(pid < 0)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fork", strerror(errno), errno);
                throw service_error(NS_FuncName);
            }

            if(0 == pid)
            {
                close(pipefd[0]);
                childProcess(xvfb, pipefd[1], cmd, std::move(params));
                // child ended
                std::exit(0);
            }

            // main thread processed
            close(pipefd[1]);

            if(0 > fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK))
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fcntl", strerror(errno), errno);
            }

            // planned get stdout from running job
            auto future = std::async(std::launch::async, jobWaitStdout, pid, pipefd[0]);
            return std::make_pair(pid, std::move(future));
        }

        static PidStatus sessionCommand(XvfbSessionPtr xvfb, const std::filesystem::path & cmd, std::list<std::string> params)
        {
            if(! xvfb)
            {
                Application::error("%s: xvfb session null", __FUNCTION__);
                throw service_error(NS_FuncName);
            }

            std::error_code err;

            if(! std::filesystem::exists(cmd, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"),
                                   cmd.c_str(), getuid());
                throw service_error(NS_FuncName);
            }

            Application::info("%s: request for: %s, display: %d, cmd: `%s %s'", __FUNCTION__, xvfb->userInfo->user(),
                              xvfb->displayNum, cmd.c_str(), Tools::join(params.begin(), params.end(), " ").c_str());

            if(! std::filesystem::is_directory(xvfb->userInfo->home(), err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not directory"),
                                   xvfb->userInfo->home(), getuid());
                throw service_error(NS_FuncName);
            }

            pid_t pid = fork();

            if(pid < 0)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fork", strerror(errno), errno);
                throw service_error(NS_FuncName);
            }

            if(0 == pid)
            {
                childProcess(xvfb, -1, cmd, std::move(params));
                // child ended
                std::exit(0);
            }

            // main thread processed
            auto future = std::async(std::launch::async, [pid]
            {
                Application::debug(DebugType::Mgr, "%s: pid: %d", "AsyncWaitPid", pid);

                int status;
                // waitpid
                int ret = waitpid(pid, & status, 0);

                if(0 > ret && errno != ECHILD)
                    Application::error("%s: %s failed, error: %s, code: %d", "AsyncWaitPid", "waitpid", strerror(errno), errno);

                return status;
            });
            return std::make_pair(pid, std::move(future));
        }

    }; // RunAs

    /* Manager::Object */
    Manager::Object::Object(sdbus::IConnection & conn, const JsonObject & jo, size_t displays, const Application & app)
        : AdaptorInterfaces(conn, LTSM::dbus_manager_service_path), XvfbSessions(displays), _app(& app), _config(& jo)
    {
        // registry
        registerAdaptor();
        // check sessions timepoint limit
        timer1 = Tools::BaseTimer::create<std::chrono::seconds>(3, true, [this]()
        {
            this->sessionsTimeLimitAction();
        });

        // check sessions killed
        timer2 = Tools::BaseTimer::create<std::chrono::seconds>(1, true, [this]()
        {
            this->sessionsEndedAction();
        });

        // check sessions alive
        timer3 = Tools::BaseTimer::create<std::chrono::seconds>(20, true, [this]()
        {
            this->sessionsCheckAliveAction();
        });
    }

    Manager::Object::~Object()
    {
        unregisterAdaptor();
    }

    void Manager::Object::shutdownService(void)
    {
        busShutdownService();
    }

    void Manager::Object::configReloadedEvent(void)
    {
        // deprecated
        if(auto str = _config->getString("service:debug"); !str.empty())
        {
            Application::setDebugLevel(str);
        }

        if(auto str = _config->getString("service:debug:level", "info"); !str.empty())
        {
            Application::setDebugLevel(str);
        }

        if(auto arr = _config->getArray("service:debug:types"))
        {
            Application::setDebugTypes(Tools::debugTypes(arr->toStdList<std::string>()));
        }

        int min = _config->getInteger("display:min", 55);
        int max = _config->getInteger("display:max", 99);
        size_t poolsz = std::abs(max - min);

        if(poolsz > sessions.size())
        {
            std::scoped_lock guard{ lockSessions };
            sessions.resize(poolsz);
        }
    }

    void Manager::Object::sessionsTimeLimitAction(void)
    {
        for(const auto & ptr : findTimepointLimitSessions())
        {
            // task background
            auto sessionAliveSec = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() -
                                   ptr->tpstart);
            auto lastsec = std::chrono::seconds(ptr->durationLimit) - ptr->aliveSec();

            // shutdown session
            if(std::chrono::seconds(ptr->durationLimit) < sessionAliveSec)
            {
                Application::notice("time point limit, display: %d, limit: %dsec, session alive: %dsec",
                                    ptr->displayNum, static_cast<int>(ptr->durationLimit), static_cast<int>(sessionAliveSec.count()));
                displayShutdown(ptr, true);
            }
            else

                // inform alert
                if(ptr->mode != XvfbMode::SessionLogin)
                {
                    if(std::chrono::seconds(100) > lastsec)
                    {
                        emitClearRenderPrimitives(ptr->displayNum);
                        // send render rect
                        const uint16_t fw = ptr->width;
                        const uint16_t fh = 24;
                        emitAddRenderRect(ptr->displayNum, {0, 0, fw, fh}, {0x10, 0x17, 0x80}, true);
                        // send render text
                        auto text = Tools::joinToString("time left: ", lastsec.count(), "sec");
                        const int16_t px = (fw - text.size() * 8) / 2;
                        const int16_t py = (fh - 16) / 2;
                        emitAddRenderText(ptr->displayNum, text, {px, py}, {0xFF, 0xFF, 0});
                    }

                    // inform beep
                    if(std::chrono::seconds(10) > lastsec)
                    {
                        emitSendBellSignal(ptr->displayNum);
                    }
                }
        }
    }

    void Manager::Object::sessionsEndedAction(void)
    {
        std::scoped_lock guard{ lockSessions, lockRunning };

        // childEnded
        if(! childsRunning.empty())
        {
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

                pidStatus.second.wait();

                if(it != this->sessions.end())
                {
                    auto & ptr = *it;

                    // skip login helper, or arnormal shutdown only
                    if(ptr && (ptr->mode != XvfbMode::SessionLogin || 0 < pidStatus.second.get()))
                    {
                        ptr->pid2 = 0;
                        this->displayShutdown(ptr, true);
                    }
                }

                return true;
            });
        }
    }

    void Manager::Object::sessionsCheckAliveAction(void)
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
                // not reply
            {
                Application::warning("connector not reply, display: %d", ptr->displayNum);
                // complete shutdown
                busConnectorTerminated(ptr->displayNum, -1);
            }
        }
    }

    bool Manager::Object::checkXvfbSocket(int display) const
    {
        return 0 < display ?
               Tools::checkUnixSocket(Tools::replace(_config->getString("xvfb:socket", "/tmp/.X11-unix/X%{display}"), "%{display}",
                                      display)) : false;
    }

    void Manager::Object::removeXvfbSocket(int display) const
    {
        if(0 < display)
        {
            std::filesystem::path socketPath = Tools::replace(_config->getString("xvfb:socket", "/tmp/.X11-unix/X%{display}"),
                                               "%{display}", display);

            try
            {
                std::filesystem::remove(socketPath);
            }
            catch(const std::filesystem::filesystem_error &)
            {
            }
        }
    }

    bool Manager::Object::displayShutdown(XvfbSessionPtr xvfb, bool emitSignal)
    {
        if(!xvfb || xvfb->mode == XvfbMode::SessionShutdown)
        {
            return false;
        }

        Application::notice("%s: shutdown display: %d %s", __FUNCTION__, xvfb->displayNum, "starting");
        xvfb->mode = XvfbMode::SessionShutdown;

        if(emitSignal)
        {
            emitShutdownConnector(xvfb->displayNum);
        }

        // dbus no wait, remove background
        std::string sysuser = _config->getString("user:xvfb");
        bool notSysUser = sysuser != xvfb->userInfo->user();

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
                runSystemScript(std::move(ptr), _config->getString("system:logoff"));
            }

            this->removeDisplaySession(displayNum);
            this->removeXvfbSocket(displayNum);
            this->emitDisplayRemoved(displayNum);
            Application::debug(DebugType::Mgr, "%s: shutdown display: %d %s", "displayShutdown", displayNum, "complete");
        }).detach();

        return true;
    }

    void Manager::Object::closeSystemSession(XvfbSessionPtr xvfb)
    {
        Application::info("%s: user: %s, display: %d", __FUNCTION__, xvfb->userInfo->user(), xvfb->displayNum);
        runSessionScript(xvfb, _config->getString("session:disconnect"));
        // PAM close
        xvfb->pam.reset();
        // unreg sessreg
        runSystemScript(std::move(xvfb), _config->getString("system:disconnect"));
    }

    bool Manager::Object::waitXvfbStarting(int display, uint32_t ms) const
    {
        if(0 >= display)
        {
            return false;
        }

        return Tools::waitCallable<std::chrono::milliseconds>(ms, 50, [ = ]()
        {
            return ! checkXvfbSocket(display);
        });
    }

    std::filesystem::path Manager::Object::createXauthFile(int displayNum, const std::vector<uint8_t> & mcookie)
    {
        std::string xauthFileTemplate = _config->getString("xauth:file", "/var/run/ltsm/auth_%{display}");
        xauthFileTemplate = Tools::replace(xauthFileTemplate, "%{pid}", getpid());
        xauthFileTemplate = Tools::replace(xauthFileTemplate, "%{display}", displayNum);
        std::filesystem::path xauthFilePath(xauthFileTemplate);
        Application::debug(DebugType::Mgr, "%s: path: `%s'", __FUNCTION__, xauthFilePath.c_str());
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
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), xauthFilePath.c_str(),
                                 getuid());
        }

        return xauthFilePath;
    }

    bool Manager::Object::createSessionConnInfo(XvfbSessionPtr xvfb, bool destroy)
    {
        auto ltsmInfo = std::filesystem::path(xvfb->userInfo->home()) / ".ltsm" / "conninfo";
        auto dir = ltsmInfo.parent_path();
        std::error_code err;

        if(! std::filesystem::is_directory(dir, err))
        {
            if(! std::filesystem::create_directory(dir, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "create failed"),
                                   dir.c_str(), getuid());
                return false;
            }
        }

        // set permissions 0750
        std::filesystem::permissions(dir, std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_all, std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), dir.c_str(), getuid());
        }

        std::filesystem::remove(ltsmInfo);
        std::ofstream ofs(ltsmInfo, std::ofstream::trunc);

        if(! ofs)
        {
            Application::error("can't create file: %s", ltsmInfo.c_str());
            return false;
        }

        ofs << "LTSM_REMOTEADDR" << "=" << (destroy ? "" : xvfb->remoteAddr) << std::endl <<
            "LTSM_TYPECONN" << "=" << (destroy ? "" : xvfb->conntype) << std::endl;
        ofs.close();
        setFileOwner(ltsmInfo, xvfb->userInfo->uid(), xvfb->userInfo->gid());
        return true;
    }

    pid_t Manager::Object::runSessionCommandSafe(XvfbSessionPtr xvfb, const std::filesystem::path & cmd,
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
                             RunAs::sessionCommand(std::move(xvfb), cmd, std::move(params)));
            return childsRunning.front().first;
        }
        catch(const std::system_error &)
        {
            Application::error("%s: failed, check thread limit", __FUNCTION__);
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

        return 0;
    }

    void Manager::Object::waitPidBackgroundSafe(pid_t pid)
    {
        // create wait pid task
        std::packaged_task<int(pid_t)> waitPidTask(RunAs::waitPid);
        std::scoped_lock guard{ lockRunning };
        childsRunning.emplace_front(std::make_pair(pid, waitPidTask.get_future()));
        std::thread(std::move(waitPidTask), pid).detach();
    }

    void Manager::Object::runSessionScript(XvfbSessionPtr xvfb, const std::string & cmd)
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

    XvfbSessionPtr Manager::Object::runXvfbDisplayNewSession(uint8_t depth, uint16_t width, uint16_t height,
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

        int min = _config->getInteger("display:min", 55);
        int max = _config->getInteger("display:max", 99);
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
            Application::error("%s: display not found: %d", __FUNCTION__, freeDisplay);
            return nullptr;
        }

        auto xvfbSocket = Tools::replace(_config->getString("xvfb:socket", "/tmp/.X11-unix/X%{display}"), "%{display}", freeDisplay);
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
            Application::error("%s: gid not found: %d, user: `%s'", __FUNCTION__, (int) sess->userInfo->gid(),
                               sess->userInfo->user());
            return nullptr;
        }

        sess->mode = XvfbMode::SessionLogin;
        sess->displayNum = freeDisplay;
        sess->depth = depth;
        sess->width = width;
        sess->height = height;
        sess->displayAddr = Tools::joinToString(":", sess->displayNum);
        sess->tpstart = std::chrono::system_clock::now();
        sess->durationLimit = _config->getInteger("idle:timeout:xvfb", 10);
        // generate session key
        auto mcookie = Tools::randomBytes(128);
        // session xauthfile
        sess->xauthfile = createXauthFile(sess->displayNum, mcookie);

        if(sess->xauthfile.empty())
        {
            return nullptr;
        }

        setFileOwner(sess->xauthfile, sess->userInfo->uid(), sess->userInfo->gid());
        std::string xvfbBin = _config->getString("xvfb:path");
        std::string xvfbArgs = _config->getString("xvfb:args");
        // xvfb args
        xvfbArgs = Tools::replace(xvfbArgs, "%{display}", sess->displayNum);
        xvfbArgs = Tools::replace(xvfbArgs, "%{depth}", sess->depth);
        xvfbArgs = Tools::replace(xvfbArgs, "%{width}", sess->width);
        xvfbArgs = Tools::replace(xvfbArgs, "%{height}", sess->height);
        xvfbArgs = Tools::replace(xvfbArgs, "%{authfile}", sess->xauthfile.native());

        Application::debug(DebugType::Mgr, "%s: bin: `%s', args: `%s'", __FUNCTION__, xvfbBin.c_str(), xvfbArgs.c_str());

        sess->pid1 = fork();

        if(0 > sess->pid1)
        {
	    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fork", strerror(errno), errno);
	    return nullptr;
        }

        if(0 == sess->pid1)
        {
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);

            // child mode
            if(Application::isDebugTarget(DebugTarget::Syslog))
            {
                Application::setDebugTarget(DebugTarget::Quiet);
            }

            if(switchToUser(*sess->userInfo))
            {
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
                Manager::redirectStdoutStderrTo(true, true, logFile.native());
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

                if(! checkFileReadable(sess->xauthfile))
                {
                    Application::error("%s: %s failed, user: %s, error: %s", __FUNCTION__, "access", sess->userInfo->user(),
                                       strerror(errno));
                }

        	closefds({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO});
                int res = execv(xvfbBin.c_str(), (char* const*) argv.data());

                if(res < 0)
                {
                    Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "execv", strerror(errno), errno,
                                       xvfbBin.c_str());
                }
            }
            else
            {
                Application::error("%s: switch to user(uid: %d) failed", __FUNCTION__, getuid());
		execl("/bin/true", "/bin/true", nullptr);
            }

            // child exit
            std::exit(0);
        }

        // main thread
        Application::debug(DebugType::Mgr, "%s: xvfb started, pid: %d, display: %d", __FUNCTION__, sess->pid1, sess->displayNum);
        (*its) = std::move(sess);
        return *its;
    }

    int Manager::Object::runUserSession(XvfbSessionPtr xvfb, const std::filesystem::path & sessionBin, PamSession* pam)
    {
        if(! pam)
        {
            Application::error("%s: %s failed, display: %d, user: %s", __FUNCTION__, "PAM", xvfb->displayNum,
                               xvfb->userInfo->user());
            return -1;
        }

        pid_t pid = fork();

        if(0 != pid)
        {
            // main thread
            return pid;
        }

        // child only
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        // child mode
        if(Application::isDebugTarget(DebugTarget::Syslog))
        {
            Application::setDebugTarget(DebugTarget::Quiet);
        }

        Application::info("%s: pid: %d", __FUNCTION__, getpid());

        auto childExit = []()
        {
            execl("/bin/true", "/bin/true", nullptr);
            std::exit(0);
        };

        if(xvfb->userInfo->uid() == 0)
        {
            Application::error("%s: deny for root", __FUNCTION__);
            childExit();
        }

        std::error_code err;

        if(! std::filesystem::is_directory(xvfb->userInfo->home(), err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not directory"),
                               xvfb->userInfo->home(), getuid());
            childExit();
        }

        if(0 != initgroups(xvfb->userInfo->user(), xvfb->userInfo->gid()))
        {
            Application::error("%s: %s failed, user: %s, gid: %d, error: %s", __FUNCTION__, "initgroups", xvfb->userInfo->user(),
                               xvfb->userInfo->gid(), strerror(errno));
            childExit();
        }

        if(! pam->openSession())
        {
            Application::error("%s: %s failed, display: %d, user: %s", __FUNCTION__, "PAM open session", xvfb->displayNum,
                               xvfb->userInfo->user());
            childExit();
        }

        Application::debug(DebugType::Mgr, "%s: child mode, type: %s, uid: %d", __FUNCTION__, "session", getuid());

        // assign groups
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
            // pam environments
            auto environments = pam->getEnvList();

            // putenv: valid string
            for(const auto & env : environments)
            {
                Application::debug(DebugType::Mgr, "%s: pam put environment: %s", __FUNCTION__, env.c_str());

                if(0 > putenv(const_cast<char*>(env.c_str())))
                {
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "putenv", strerror(errno), errno);
                }
            }

            createSessionConnInfo(xvfb);
    	    closefds({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO});
            int res = execl(sessionBin.c_str(), sessionBin.c_str(), (char*) nullptr);

            if(res < 0)
            {
                Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "execl", strerror(errno), errno,
                                   sessionBin.c_str());
            }
        }

        childExit();
        return 0;
    }

    int32_t Manager::Object::busStartLoginSession(const int32_t & connectorId, const uint8_t & depth,
            const std::string & remoteAddr, const std::string & connType)
    {
        Application::info("%s: login request, remote: %s, type: %s", __FUNCTION__, remoteAddr.c_str(), connType.c_str());
        auto userXvfb = _config->getString("user:xvfb");
        auto groupAuth = _config->getString("group:auth");
        auto displayWidth = _config->getInteger("default:width", 1024);
        auto displayHeight = _config->getInteger("default:height", 768);
        auto userInfo = Tools::getUserInfo(userXvfb);

        if(! userInfo)
        {
            Application::error("%s: user not found: `%s'", __FUNCTION__, userXvfb.c_str());
            return -1;
        }

        auto xvfb = runXvfbDisplayNewSession(depth, displayWidth, displayHeight, std::move(userInfo));

        if(! xvfb)
        {
            return -1;
        }

        // update screen
        xvfb->remoteAddr = remoteAddr;
        xvfb->conntype = connType;
        xvfb->connectorId = connectorId;
        // fix permission
        auto groupAuthGid = Tools::getGroupGid(groupAuth);
        setFileOwner(xvfb->xauthfile, xvfb->userInfo->uid(), groupAuthGid);
        // registered xvfb job
        waitPidBackgroundSafe(xvfb->pid1);

        // wait Xvfb display starting
        if(! waitXvfbStarting(xvfb->displayNum, 5000 /* 5 sec */))
        {
            Application::error("%s: %s failed, display: %d", __FUNCTION__, "waitXvfbStarting", xvfb->displayNum);
            return -1;
        }

        // check socket
        std::filesystem::path socketPath = Tools::replace(_config->getString("xvfb:socket", "/tmp/.X11-unix/X%{display}"),
                                           "%{display}", xvfb->displayNum);
        std::error_code err;

        if(! std::filesystem::is_socket(socketPath, err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not socket"),
                               socketPath.c_str(), getuid());
            return -1;
        }

        // fix socket pemissions 0660
        std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), socketPath.c_str(), getuid());
        }

        setFileOwner(socketPath, xvfb->userInfo->uid(), groupAuthGid);
        std::string helperArgs = _config->getString("helper:args");

        if(helperArgs.size())
        {
            helperArgs = Tools::replace(helperArgs, "%{display}", xvfb->displayNum);
            helperArgs = Tools::replace(helperArgs, "%{authfile}", xvfb->xauthfile.native());
        }

        // simple cursor
        if(_config->hasKey("display:cursor"))
        {
            runSessionCommandSafe(xvfb, "/usr/bin/xsetroot", { "-cursor_name", _config->getString("display:cursor") });
        }

        // runas login helper
        xvfb->pid2 = runSessionCommandSafe(xvfb, _config->getString("helper:path"), Tools::split(helperArgs, 0x20));

        if(0 >= xvfb->pid2)
        {
            return -1;
        }

        xvfb->durationLimit = _config->getInteger("idle:timeout:login", 80);

        startLoginChannels(xvfb);

        return xvfb->displayNum;
    }

    int32_t Manager::Object::busStartUserSession(const int32_t & oldScreen, const int32_t & connectorId,
            const std::string & userName, const std::string & remoteAddr, const std::string & connType)
    {
        std::string userXvfb = _config->getString("user:xvfb");
        std::string sessionBin = _config->getString("session:path");
        std::string groupAuth = _config->getString("group:auth");
        Application::info("%s: session request, user: %s, remote: %s, display: %" PRId32, __FUNCTION__, userName.c_str(),
                          remoteAddr.c_str(), oldScreen);
        auto userInfo = Tools::getUserInfo(userName);

        if(! userInfo)
        {
            Application::error("%s: user not found: `%s'", __FUNCTION__, userName.c_str());
            return -1;
        }

        std::error_code err;

        if(! std::filesystem::is_directory(userInfo->home(), err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not directory"),
                               userInfo->home(), getuid());
            return -1;
        }

        auto loginSess = findDisplaySession(oldScreen);

        if(! loginSess)
        {
            Application::error("%s: display not found: %" PRId32, __FUNCTION__, oldScreen);
            return -1;
        }

        // auto close login session
        loginSess->durationLimit = loginSess->aliveSec().count() + 3;
        std::unique_ptr<PamSession> pam = std::move(loginSess->pam);

        if(! pam)
        {
            Application::error("%s: %s failed, display: %d, user: %s", __FUNCTION__, "PAM", loginSess->displayNum,
                               userInfo->user());
            return -1;
        }

        if(! pam->isAuthenticated())
        {
            Application::error("%s: %s failed, display: %d, user: %s", __FUNCTION__, "PAM authenticate", loginSess->displayNum,
                               userInfo->user());
            return -1;
        }

        if(! pam->isLogin(userInfo->user()))
        {
            Application::error("%s: %s failed, display: %d, user: %s", __FUNCTION__, "PAM login", loginSess->displayNum,
                               userInfo->user());
            return -1;
        }

        auto oldSess = findUserSession(userName);

        if(oldSess && 0 <= oldSess->displayNum && checkXvfbSocket(oldSess->displayNum))
        {
            // parent continue
            oldSess->remoteAddr = remoteAddr;
            oldSess->conntype = connType;
            oldSess->connectorId = connectorId;
            oldSess->mode = XvfbMode::SessionOnline;
            oldSess->environments = std::move(loginSess->environments);
            oldSess->options = std::move(loginSess->options);
            oldSess->encryption = std::move(loginSess->encryption);
            oldSess->layout = std::move(loginSess->layout);

            // reinit pam session
            if(! oldSess->pam || ! oldSess->pam->refreshCreds())
            {
                Application::error("%s: %s failed, display: %d, user: %s", __FUNCTION__, "PAM", oldSess->displayNum,
                                   oldSess->userInfo->user());
                return -1;
            }

            // update conn info
            createSessionConnInfo(oldSess);
            Application::debug(DebugType::Mgr, "%s: user session connected, display: %d", __FUNCTION__, oldSess->displayNum);
            emitSessionReconnect(remoteAddr, connType);
            emitSessionChanged(oldSess->displayNum);

            if(_config->getBoolean("session:kill:stop", false))
            {
                auto cmd = std::string("/usr/bin/killall -s SIGCONT -u ").append(oldSess->userInfo->user());
                int ret = std::system(cmd.c_str());
                Application::debug(DebugType::Mgr, "%s: command: `%s', return code: %d, display: %d", __FUNCTION__, cmd.c_str(), ret,
                                   oldSess->displayNum);
            }

            sessionRunSetxkbmapLayout(oldSess);
            startSessionChannels(oldSess);
            runSessionScript(oldSess, _config->getString("session:connect"));
            return oldSess->displayNum;
        }

        // get owner screen
        auto newSess = runXvfbDisplayNewSession(loginSess->depth, loginSess->width, loginSess->height, std::move(userInfo));

        if(! newSess)
        {
            return -1;
        }

        // update screen
        newSess->environments = std::move(loginSess->environments);
        newSess->options = std::move(loginSess->options);
        newSess->encryption = std::move(loginSess->encryption);
        newSess->layout = std::move(loginSess->layout);
        newSess->remoteAddr = remoteAddr;
        newSess->conntype = connType;
        newSess->connectorId = connectorId;
        newSess->durationLimit = _config->getInteger("idle:timeout:logout", 0);
        newSess->policy = sessionPolicy(Tools::lower(_config->getString("session:policy")));

        if(! _config->getBoolean("transfer:file:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel:: TransferFiles);
        }

        if(! _config->getBoolean("channel:printer:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RedirectPrinter);
        }

        if(! _config->getBoolean("channel:audio:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RedirectAudio);
        }

        if(! _config->getBoolean("channel:pcsc:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RedirectPcsc);
        }

        if(! _config->getBoolean("channel:sane:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RedirectScanner);
        }

        if(! _config->getBoolean("channel:fuse:disabled", false))
        {
            newSess->setStatus(Flags::AllowChannel::RemoteFilesUse);
        }

        // fix permission
        auto groupAuthGid = Tools::getGroupGid(groupAuth);
        setFileOwner(newSess->xauthfile, newSess->userInfo->uid(), groupAuthGid);
        // registered xvfb job
        waitPidBackgroundSafe(newSess->pid1);

        // wait Xvfb display starting
        if(! waitXvfbStarting(newSess->displayNum, 5000 /* 5 sec */))
        {
            Application::error("%s: %s failed, display: %d", __FUNCTION__, "waitXvfbStarting", newSess->displayNum);
            return -1;
        }

        // check socket
        std::filesystem::path socketPath = Tools::replace(_config->getString("xvfb:socket", "/tmp/.X11-unix/X%{display}"),
                                           "%{display}", oldScreen);

        if(! std::filesystem::is_socket(socketPath, err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not socket"),
                               socketPath.c_str(), getuid());
            return -1;
        }

        // fix socket pemissions 0660
        std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), socketPath.c_str(), getuid());
        }

        setFileOwner(socketPath, newSess->userInfo->uid(), groupAuthGid);

        // fixed environments
        for(auto & [key, val] : newSess->environments)
        {
            if(std::string::npos != val.find("%{user}"))
            {
                val = Tools::replace(val, "%{user}", userName);
            }
            else if(std::string::npos != val.find("%{runtime_dir}"))
            {
                val = Tools::replace(val, "%{runtime_dir}", newSess->userInfo->runtime_dir());
            }
        }

        // move pam from login session
        newSess->pam = std::move(pam);
        newSess->pid2 = runUserSession(newSess, sessionBin, newSess->pam.get());

        if(newSess->pid2 < 0)
        {
            Application::error("%s: user session failed, result: %d", __FUNCTION__, newSess->pid2);
            return -1;
        }

        newSess->mode = XvfbMode::SessionOnline;
        // registered session job
        waitPidBackgroundSafe(newSess->pid2);
        // parent continue
        Application::debug(DebugType::Mgr, "%s: user session started, pid: %d, display: %d", __FUNCTION__, newSess->pid2, newSess->displayNum);
        sessionRunSetxkbmapLayout(newSess);
        runSystemScript(newSess, _config->getString("system:logon"));
        runSystemScript(newSess, _config->getString("system:connect"));
        emitSessionChanged(newSess->displayNum);
        startSessionChannels(newSess);
        runSessionScript(newSess, _config->getString("session:connect"));
        return newSess->displayNum;
    }

    int32_t Manager::Object::busGetServiceVersion(void)
    {
        return LTSM::service_version;
    }

    std::string Manager::Object::busCreateAuthFile(const int32_t & display)
    {
        Application::info("%s: display: %" PRId32, __FUNCTION__, display);
        auto xvfb = findDisplaySession(display);
        return xvfb ? xvfb->xauthfile : "";
    }

    bool Manager::Object::busShutdownDisplay(const int32_t & display)
    {
        Application::info("%s: display: %" PRId32, __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display))
        {
            displayShutdown(xvfb, true);
            return true;
        }

        return false;
    }

    bool Manager::Object::busShutdownConnector(const int32_t & display)
    {
        Application::info("%s: display: %" PRId32, __FUNCTION__, display);
        emitShutdownConnector(display);
        return true;
    }

    void Manager::Object::busShutdownService(void)
    {
        Application::info("%s: shutdown pid: %d %s", __FUNCTION__, getpid(), "starting");

        // terminate connectors
        for(const auto & ptr : sessions)
            if(ptr)
            {
                displayShutdown(ptr, true);
            }

        auto isValidSession = [](XvfbSessionPtr & ptr)
        {
            return ptr;
        };

        // wait sessions
        while(auto sessionsAlive = std::count_if(sessions.begin(), sessions.end(), isValidSession))
        {
            Application::info("%s: wait sessions: %u", __FUNCTION__, sessionsAlive);
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

            Application::error("%s: running childs: %u, killed process", __FUNCTION__, childsCount);

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

        Application::notice("%s: shutdown pid: %d %s", __FUNCTION__, getpid(), "complete");
        Manager::serviceRunning = false;
    }

    bool Manager::Object::sessionRunZenity(XvfbSessionPtr xvfb, std::initializer_list<std::string> params)
    {
        std::filesystem::path zenity = _config->getString("zenity:path", "/usr/bin/zenity");
        return 0 != runSessionCommandSafe(std::move(xvfb), zenity, std::move(params));
    }

    bool Manager::Object::busSendMessage(const int32_t & display, const std::string & message)
    {
        Application::info("%s: display: %" PRId32 ", message: `%s'", __FUNCTION__, display, message.c_str());

        if(auto xvfb = findDisplaySession(display))
        {
            if(xvfb->mode == XvfbMode::SessionLogin)
            {
                Application::error("%s: login session skipped, display: %" PRId32, __FUNCTION__, display);
                return false;
            }

            // new mode: zenity info
            return sessionRunZenity(xvfb, { "--info", "--no-wrap", "--text", quotedString(message) });
        }

        return false;
    }

    bool Manager::Object::busIdleTimeoutAction(const int32_t & display)
    {
        Application::info("%s: display: %" PRId32, __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display))
        {
            auto cmd = _config->getString("idle:action:path");

            // alse running
            if(xvfb->idleActionRunning.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout)
            {
                return false;
            }

            if(xvfb->mode != XvfbMode::SessionLogin && ! cmd.empty())
            {
                auto args = _config->getStdList<std::string>("idle:action:args");

                try
                {
                    PidStatus pidStatus = RunAs::sessionCommand(xvfb, cmd, std::move(args));
                    xvfb->idleActionRunning = pidStatus.second;
                }
                catch(const std::system_error &)
                {
                    Application::error("%s: failed, check thread limit", __FUNCTION__);
                }
                catch(const std::exception & err)
                {
                    Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
                }

                return true;
            }
        }

        return false;
    }

    bool Manager::Object::busConnectorAlive(const int32_t & display)
    {
        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->resetStatus(Flags::SessionStatus::CheckConnection);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetLoginsDisable(const bool & action)
    {
        loginsDisable = action;
        return true;
    }

    bool Manager::Object::busConnectorTerminated(const int32_t & display, const int32_t & connectorId)
    {
        Application::info("%s: display: %" PRId32, __FUNCTION__, display);

        if(auto xvfb = findDisplaySession(display))
        {
            if(xvfb->mode == XvfbMode::SessionLogin)
            {
                stopLoginChannels(xvfb);
                displayShutdown(std::move(xvfb), false);
            }
            else if(xvfb->mode == XvfbMode::SessionOnline)
            {
                xvfb->mode = XvfbMode::SessionSleep;
                xvfb->resetStatus(Flags::SessionStatus::CheckConnection);
                xvfb->remoteAddr.clear();
                xvfb->conntype.clear();
                xvfb->encryption.clear();
                createSessionConnInfo(xvfb, false);
                emitSessionChanged(display);

                // stop user process
                if(_config->getBoolean("session:kill:stop", false))
                {
                    auto cmd = std::string("/usr/bin/killall -s SIGSTOP -u ").append(xvfb->userInfo->user());
                    int ret = std::system(cmd.c_str());
                    Application::debug(DebugType::Mgr, "%s: command: `%s', return code: %d, display: %d", __FUNCTION__, cmd.c_str(), ret, xvfb->displayNum);
                }

                stopSessionChannels(std::move(xvfb));
            }
        }

        return true;
    }

    void Manager::Object::transferFilesRequestCommunication(Object* owner, XvfbSessionPtr xvfb,
            std::filesystem::path zenity, std::vector<sdbus::Struct<std::string, uint32_t>> files,
            std::function<void(int, const std::vector<sdbus::Struct<std::string, uint32_t>> &)> emitTransferReject,
            std::shared_future<int> zenityQuestionResult)
    {
        // copy all files to (Connector) user home, after success move to real user
        auto xvfbHome = Tools::getUserHome(owner->_config->getString("user:xvfb"));
        // wait zenity question
        zenityQuestionResult.wait();
        int status = zenityQuestionResult.get();

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
            auto pair = RunAs::sessionCommandStdout(xvfb, zenity,
            { "--file-selection", "--directory", "--title", "Select directory", "--width", "640", "--height", "480" });
            zenitySelectDirectoryResult = std::move(pair.second);
        }
        catch(const std::system_error &)
        {
            Application::error("%s: failed, check thread limit", "RunZenity");
            emitTransferReject(xvfb->displayNum, files);
            return;
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", "RunZenity", err.what());
            emitTransferReject(xvfb->displayNum, files);
            return;
        }

        // wait file selection
        zenitySelectDirectoryResult.wait();
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
            Application::error("%s: %s, path: `%s', uid: %d", "RunZenity", err.message().c_str(), dstdir.c_str(), getuid());
            emitTransferReject(xvfb->displayNum, files);
            return;
        }

        for(const auto & info : files)
        {
            auto tmpname = std::filesystem::path(xvfbHome) / "transfer_";
            tmpname += Tools::randomHexString(8);
            Application::debug(DebugType::Mgr, "%s: transfer file request, display: %d, select dir: `%s', tmp name: `%s'", "RunZenity",
                               xvfb->displayNum, dstdir.c_str(), tmpname.c_str());
            auto filepath = std::filesystem::path(std::get<0>(info));
            auto filesize = std::get<1>(info);
            // check disk space limited
            //size_t ftotal = std::accumulate(files.begin(), files.end(), 0, [](size_t sum, auto & val){ return sum += std::get<1>(val); });
            auto spaceInfo = std::filesystem::space(dstdir, err);

            if(spaceInfo.available < filesize)
            {
                owner->busSendNotify(xvfb->displayNum, "Transfer Rejected", "not enough disk space",
                                     NotifyParams::Error, NotifyParams::UrgencyLevel::Normal);
                break;
            }

            // check dstdir writeable / filename present
            auto dstfile = dstdir / filepath.filename();

            if(std::filesystem::exists(dstfile, err))
            {
                Application::error("%s: file present and skipping, path: `%s'", "RunZenity", dstfile.c_str());
                owner->busSendNotify(xvfb->displayNum, "Transfer Skipping",
                                     Tools::StringFormat("such a file exists: %1").arg(dstfile.c_str()),
                                     NotifyParams::Warning, NotifyParams::UrgencyLevel::Normal);
                continue;
            }

            std::scoped_lock guard{ owner->lockTransfer };
            owner->allowTransfer.emplace_front(filepath);
            owner->emitTransferAllow(xvfb->displayNum, filepath, tmpname, dstdir);
        }
    }

    void Manager::Object::transferFileStartBackground(Object* owner, XvfbSessionPtr xvfb, std::string tmpfile,
            std::string dstfile, uint32_t filesz)
    {
        bool error = false;
        std::error_code fserr;

        while(!error)
        {
            // check fill data complete
            if(std::filesystem::exists(tmpfile, fserr) &&
                    std::filesystem::file_size(tmpfile, fserr) >= filesz)
            {
                break;
            }

            // FIXME create progress informer session

            // check lost  conn
            if(xvfb->mode != XvfbMode::SessionOnline)
            {
                owner->busSendNotify(xvfb->displayNum, "Transfer Error", Tools::StringFormat("transfer connection is lost"),
                                     NotifyParams::Error, NotifyParams::UrgencyLevel::Normal);
                error = true;
                continue;
            }

            std::this_thread::sleep_for(350ms);
        }

        if(! error)
        {
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
                setFileOwner(dstfile, xvfb->userInfo->uid(), xvfb->userInfo->gid());
                owner->busSendNotify(xvfb->displayNum, "Transfer Complete",
                                     Tools::StringFormat("new file added: <a href=\"file://%1\">%2</a>").arg(dstfile).arg(std::filesystem::path(
                                                 dstfile).filename().c_str()),
                                     NotifyParams::Information, NotifyParams::UrgencyLevel::Normal);
            }
        }
    }

    bool Manager::Object::busTransferFilesRequest(const int32_t & display,
            const std::vector<sdbus::Struct<std::string, uint32_t>> & files)
    {
        Application::info("%s: display: %" PRId32 ", count: %u", __FUNCTION__, display, files.size());
        auto xvfb = findDisplaySession(display);

        if(! xvfb)
        {
            Application::error("%s: display not found: %" PRId32, __FUNCTION__, display);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::TransferFiles))
        {
            Application::warning("%s: display %" PRId32 ", transfer reject", __FUNCTION__, display);
            busSendNotify(display, "Transfer Restricted", "transfer is blocked, contact the administrator",
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        if(_config->hasKey("transfer:group:only"))
        {
            if(auto groupInfo = Tools::getGroupInfo(_config->getString("transfer:group:only")))
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

        std::filesystem::path zenity = this->_config->getString("zenity:path", "/usr/bin/zenity");
        auto msg = Tools::joinToString("Can you receive remote files? (", files.size(), ")");
        std::shared_future<int> zenityQuestionResult;
        auto emitTransferReject = [this](int display, const std::vector<sdbus::Struct<std::string, uint32_t>> & files)
        {
            for(const auto & info : files)
            {
                this->emitTransferAllow(display, std::get<0>(info), "", "");
            }
        };

        try
        {
            auto pidStatus = RunAs::sessionCommand(xvfb, zenity, { "--question", "--default-cancel", "--text", msg });
            zenityQuestionResult = std::move(pidStatus.second);
        }
        catch(const std::system_error &)
        {
            Application::error("%s: failed, check thread limit", __FUNCTION__);
            emitTransferReject(display, files);
            return false;
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            emitTransferReject(display, files);
            return false;
        }

        //run background
        std::thread(transferFilesRequestCommunication, this, xvfb, zenity, files, std::move(emitTransferReject),
                    std::move(zenityQuestionResult)).detach();
        return true;
    }

    bool Manager::Object::busTransferFileStarted(const int32_t & display, const std::string & tmpfile,
            const uint32_t & filesz, const std::string & dstfile)
    {
        Application::debug(DebugType::Mgr, "%s: display: %" PRId32 ", tmp file: `%s', dst file: `%s'", __FUNCTION__, display, tmpfile.c_str(),
                           dstfile.c_str());

        if(auto xvfb = findDisplaySession(display))
        {
            std::thread(transferFileStartBackground, this, xvfb, tmpfile, dstfile, filesz).detach();
        }

        std::scoped_lock guard{ lockTransfer };
        allowTransfer.remove(tmpfile);
        return true;
    }

    bool Manager::Object::busSendNotify(const int32_t & display, const std::string & summary, const std::string & body,
                                        const uint8_t & icontype, const uint8_t & urgency)
    {
        // urgency:  NotifyParams::UrgencyLevel { Low, Normal, Critical }
        // icontype: NotifyParams::IconType { Information, Warning, Error, Question }
        if(auto xvfb = findDisplaySession(display))
        {
            if(xvfb->mode == XvfbMode::SessionLogin)
            {
                Application::error("%s: login session skipped, display: %" PRId32, __FUNCTION__, display);
                return false;
            }

            // thread mode
            std::thread([xvfb = std::move(xvfb), summary2 = summary, body2 = body, icontype2 = icontype /*, urgency2 = urgency */]
            {
                // wait new session started
                while(xvfb->aliveSec() < std::chrono::seconds(3))
                    std::this_thread::sleep_for(550ms);

                Application::info("%s: notification display: %d, user: %s, summary: %s", "busSendNotify", xvfb->displayNum, xvfb->userInfo->user(), summary2.c_str());

                std::string notificationIcon("dialog-information");
                switch(icontype2)
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

                auto dbusAddresses = Manager::getSessionDBusAddresses(*xvfb->userInfo);

                if(dbusAddresses.empty())
                {
                    Application::warning("%s: dbus address empty, display: %d, user: %s", "busSendNotify", xvfb->displayNum,
                                         xvfb->userInfo->user());
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
                    auto conn = sdbus::createSessionBusConnectionWithAddress(Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";"));
                    auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);
                    concatenatorProxy->callMethod("Notify").onInterface("org.freedesktop.Notifications").withArguments(applicationName,
                            replacesID, notificationIcon,
                            summary2, body2, actions, hints, expirationTimeout).dontExpectReply();
                }
                catch(const sdbus::Error & err)
                {
                    Application::error("%s: failed, display: %d, sdbus error: %s, msg: %s", "busSendNotify", xvfb->displayNum,
                                       err.getName().c_str(), err.getMessage().c_str());
                }
                catch(std::exception & err)
                {
                    Application::error("%s: exception: %s", "busSendNotify", err.what());
                }
#else
                Application::warning("%s: sdbus address not supported, use 1.2 version", "busSendNotify");
#endif
            }).detach();
            return true;
        }

        return false;
    }

    bool Manager::Object::helperWidgetStartedAction(const int32_t & display)
    {
        Application::info("%s: display: %" PRId32, __FUNCTION__, display);
        emitHelperWidgetStarted(display);
        return true;
    }

    std::string Manager::Object::helperGetTitle(const int32_t & display)
    {
        return _config->getString("helper:title", "X11 Remote Desktop Service");
    }

    std::string Manager::Object::helperGetDateFormat(const int32_t & display)
    {
        return _config->getString("helper:dateformat");
    }

    bool Manager::Object::helperIsAutoComplete(const int32_t & display)
    {
        return _config->getBoolean("helper:autocomplete", false);
    }

    std::forward_list<std::string> Manager::Object::getAllowLogins(void) const
    {
        // uids names: "access:uid:min", "access:uid:max"
        int minUidRange = _config->getInteger("access:uid:min", 0);
        int maxUidRange = _config->getInteger("access:uid:max", INT32_MAX);
        auto accessUidNames = Tools::getSystemUsers(minUidRange, maxUidRange);
        // access list: "access:users"
        auto accessUsersNames = _config->getStdListForward<std::string>("access:users");

        // append list: "access:groups"
        for(const auto & group : _config->getStdListForward<std::string>("access:groups"))
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

    std::vector<std::string> Manager::Object::helperGetUsersList(const int32_t & display)
    {
        auto allowLogins = getAllowLogins();
        return std::vector<std::string>(allowLogins.begin(), allowLogins.end());
    }

    bool Manager::Object::busSetAuthenticateToken(const int32_t & display, const std::string & login)
    {
        if(auto xvfb = this->findDisplaySession(display))
        {
            std::thread([this, login, xvfb = std::move(xvfb)]()
            {
                auto res = this->pamAuthenticate(xvfb, login, "******", true);
                Application::notice("%s: check authenticate: %s, user: %s, display: %d", "busSetAuthenticateToken",
                                    (res ? "success" : "failed"), login.c_str(), xvfb->displayNum);
            }).detach();
        }
        else
        {
            Application::warning("%s: session nof found, user: %s, display: %" PRId32, __FUNCTION__, login.c_str(), display);
        }

        return true;
    }

    bool Manager::Object::busSetAuthenticateLoginPass(const int32_t & display, const std::string & login,
            const std::string & password)
    {
        if(auto xvfb = this->findDisplaySession(display))
        {
            std::thread([this, login, password, xvfb = std::move(xvfb)]()
            {
                auto res = this->pamAuthenticate(xvfb, login, password, false);
                Application::notice("%s: check authenticate: %s, user: %s, display: %d", "busSetAuthenticateLoginPass",
                                    (res ? "success" : "failed"), login.c_str(), xvfb->displayNum);
            }).detach();
        }
        else
        {
            Application::warning("%s: session nof found, user: %s, display: %" PRId32, __FUNCTION__, login.c_str(), display);
        }

        return true;
    }

    bool Manager::Object::pamAuthenticate(XvfbSessionPtr xvfb, const std::string & login, const std::string & password,
                                          bool token)
    {
        Application::info("%s: login: %s, display: %d", __FUNCTION__, login.c_str(), xvfb->displayNum);
        auto users = getAllowLogins();

        if(users.empty())
        {
            Application::error("%s: login not found: %s, display: %d", __FUNCTION__, login.c_str(), xvfb->displayNum);
            emitLoginFailure(xvfb->displayNum, "login disabled");
            return false;
        }

        if(std::none_of(users.begin(), users.end(), [&](auto & val) { return val == login; }))
        {
            Application::error("%s: login not found: %s, display: %d", __FUNCTION__, login.c_str(), xvfb->displayNum);
            emitLoginFailure(xvfb->displayNum, "login not found");
            return false;
        }

        if(loginsDisable)
        {
            Application::info("%s: logins disabled, display: %d", __FUNCTION__, xvfb->displayNum);
            emitLoginFailure(xvfb->displayNum, "logins disabled by the administrator");
            return false;
        }

        int loginFailuresConf = _config->getInteger("login:failures_count", 0);

        if(0 > loginFailuresConf)
        {
            loginFailuresConf = 0;
        }

        // open PAM
        auto pam = std::make_unique<PamSession>(_config->getString("pam:service"), login, password);

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
                    Application::error("%s: login failures limit, display: %d", __FUNCTION__, xvfb->displayNum);
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
                userSess->mode == XvfbMode::SessionOnline)
        {
            if(userSess->policy == SessionPolicy::AuthLock)
            {
                Application::error("%s: session busy, policy: %s, user: %s, session display: %d, from: %s, display: %d", __FUNCTION__,
                                   "authlock", login.c_str(), userSess->displayNum, userSess->remoteAddr.c_str(), xvfb->displayNum);
                // informer login display
                emitLoginFailure(xvfb->displayNum, Tools::joinToString("session busy, from: ", userSess->remoteAddr));
                return false;
            }
            else if(userSess->policy == SessionPolicy::AuthTake)
            {
                // shutdown prev connect
                emitShutdownConnector(userSess->displayNum);
                // wait session
                Tools::waitCallable<std::chrono::milliseconds>(1000, 50, [ = ]()
                {
                    return userSess->mode != XvfbMode::SessionSleep;
                });
            }
        }

        xvfb->pam = std::move(pam);
        emitLoginSuccess(xvfb->displayNum, login, Tools::getUserUid(login));
        return true;
    }

    void Manager::Object::sessionRunSetxkbmapLayout(XvfbSessionPtr xvfb)
    {
        if(xvfb && ! xvfb->layout.empty())
        {
            std::thread([this, ptr = std::move(xvfb)]
            {
                this->runSessionCommandSafe(ptr, "/usr/bin/setxkbmap", { "-layout", ptr->layout, "-option", "\"\"" });
            }).detach();
        }
    }

    bool Manager::Object::busSetSessionKeyboardLayouts(const int32_t & display, const std::vector<std::string> & layouts)
    {
        if(auto xvfb = findDisplaySession(display))
        {
            Application::info("%s: display: %" PRId32 ", layouts: [%s]", __FUNCTION__,
                              display, Tools::join(layouts.begin(), layouts.end(), ",").c_str());

            if(layouts.empty())
            {
                return false;
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

            xvfb->layout = quotedString(os.str());
            sessionRunSetxkbmapLayout(xvfb);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetSessionEnvironments(const int32_t & display, const std::map<std::string, std::string> & map)
    {
        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->environments.clear();

            for(const auto & [key, val] : map)
            {
                Application::info("%s: %s = `%s'", __FUNCTION__, key.c_str(), val.c_str());
                xvfb->environments.emplace(key, val);

                if(key == "TZ")
                {
                    emitHelperWidgetTimezone(display, val);
                }
            }

            return true;
        }

        return false;
    }

    bool Manager::Object::busSetSessionOptions(const int32_t & display, const std::map<std::string, std::string> & map)
    {
        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->options.clear();
            std::string login, pass;

            for(const auto & [key, val] : map)
            {
                Application::info("%s: %s = `%s'", __FUNCTION__, key.c_str(), (key != "password" ? val.c_str() : "HIDDEN"));

                if(key == "redirect:cups")
                {
                    if(_config->getBoolean("channel:printer:disabled", false))
                    {
                        continue;
                    }
                }

                if(key == "redirect:fuse")
                {
                    if(_config->getBoolean("channel:fuse:disabled", false))
                    {
                        continue;
                    }
                }

                if(key == "redirect:audio")
                {
                    if(_config->getBoolean("channel:audio:disabled", false))
                    {
                        continue;
                    }
                }
                else if(key == "redirect:pcsc")
                {
                    if(_config->getBoolean("channel:pcsc:disabled", false))
                    {
                        continue;
                    }

                    xvfb->environments.emplace("PCSCLITE_CSOCK_NAME", "%{runtime_dir}/pcsc2ltsm");
                }
                else if(key == "redirect:sane")
                {
                    if(_config->getBoolean("channel:sane:disabled", false))
                    {
                        continue;
                    }

                    auto socket = _config->getString("channel:sane:format", "/var/run/ltsm/sane/%{user}");
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

            return true;
        }

        return false;
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
            if(0 != chmod(path.c_str(), mode))
            {
                Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "chmod", strerror(errno), errno,
                                   path.c_str());
            }

            if(0 != chown(path.c_str(), uid, gid))
            {
                Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "chown", strerror(errno), errno,
                                   path.c_str());
            }
        }
    }

    void Manager::Object::startSessionChannels(XvfbSessionPtr xvfb)
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

        if(xvfb->options.end() != fuse)
        {
            for(const auto & share : JsonContentString(fuse->second).toArray().toStdList<std::string>())
            {
                startFuseListener(xvfb, share);
            }
        }
    }

    void Manager::Object::stopSessionChannels(XvfbSessionPtr xvfb)
    {
        if(0 < xvfb->connectorId)
        {
            auto fuse = xvfb->options.find("fuse");

            if(xvfb->options.end() != fuse)
            {
                for(const auto & share : Tools::split(fuse->second, '|'))
                {
                    stopFuseListener(xvfb, share);
                }
            }

            auto audio = xvfb->options.find("audio");

            if(xvfb->options.end() != audio)
            {
                stopAudioListener(xvfb, audio->second);
            }

            auto pcsc = xvfb->options.find("pcsc");

            if(xvfb->options.end() != pcsc)
            {
                stopPcscListener(xvfb, pcsc->second);
            }
        }
    }

    void Manager::Object::startLoginChannels(XvfbSessionPtr xvfb)
    {
    }

    void Manager::Object::stopLoginChannels(XvfbSessionPtr xvfb)
    {
        if(0 < xvfb->connectorId)
        {
            auto pkcs11 = xvfb->options.find("pkcs11");

            if(xvfb->options.end() != pkcs11)
            {
                stopPkcs11Listener(xvfb, pkcs11->second);
            }
        }
    }

    bool Manager::Object::startPrinterListener(XvfbSessionPtr xvfb, const std::string & clientUrl)
    {
        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectPrinter))
        {
            Application::warning("%s: display %d, redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "printer");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("printer"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: url: %s", __FUNCTION__, clientUrl.c_str());
        auto [ clientType, clientAddress ] = Channel::parseUrl(clientUrl);

        if(clientType == Channel::ConnectorType::Unknown)
        {
            Application::error("%s: %s, unknown client url: %s", __FUNCTION__, "printer", clientUrl.c_str());
            return false;
        }

        auto printerSocket = _config->getString("channel:printer:format", "/var/run/ltsm/cups/printer_%{user}");
        auto socketFolder = std::filesystem::path(printerSocket).parent_path();
        auto lp = Tools::getGroupGid("lp");
        std::error_code err;

        if(! std::filesystem::is_directory(socketFolder, err))
        {
            if(! std::filesystem::create_directories(socketFolder, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, "create directory failed", socketFolder.c_str(),
                                   getuid());
                return false;
            }
        }

        // fix mode 0750
        std::filesystem::permissions(socketFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), socketFolder.c_str(),
                                 getuid());
        }

        // fix owner xvfb.lp
        setFileOwner(socketFolder, Tools::getUserUid(_config->getString("user:xvfb")), lp);
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

    bool startAudioSessionJob(Manager::Object* owner, XvfbSessionPtr xvfb, std::string audioSocket)
    {
        // wait new session started
        while(xvfb->aliveSec() < std::chrono::seconds(3))
        {
            std::this_thread::sleep_for(550ms);
        }

        Application::info("%s: display: %d, user: %s, audioSocket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), audioSocket.c_str());
        auto destinationName = "ltsm.session.audio";
        auto objectPath = "/ltsm/session/audio";
        auto interfaceName = "LTSM.Session.AUDIO";
#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = Manager::getSessionDBusAddresses(*xvfb->userInfo);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: dbus address empty, display: %d, user: %s", __FUNCTION__, xvfb->displayNum,
                                     xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto conn = sdbus::createSessionBusConnectionWithAddress(Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";"));
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);
            auto method1 = concatenatorProxy->createMethodCall(interfaceName, "getVersion");
            auto reply1 = concatenatorProxy->callMethod(method1);
            int32_t version = 0;
            concatenatorProxy->callMethod("getVersion").onInterface(interfaceName).storeResultsTo(version);

            if(version < LTSM_AUDIO2SESSION_VERSION)
            {
                Application::error("%s: unsupported audio2session, version: %d", __FUNCTION__, version);
                throw service_error(NS_FuncName);
            }

            bool ret = false;
            concatenatorProxy->callMethod("connectChannel").onInterface(interfaceName).withArguments(audioSocket).storeResultsTo(
                ret);

            if(! ret)
            {
                Application::error("%s: %s failed", __FUNCTION__, "audio session connect");
            }

            return ret;
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %d, sdbus error: %s, msg: %s", __FUNCTION__, xvfb->displayNum,
                               err.getName().c_str(), err.getMessage().c_str());
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

    bool Manager::Object::startAudioListener(XvfbSessionPtr xvfb, const std::string & encoding)
    {
        if(xvfb->mode == XvfbMode::SessionLogin)
        {
            Application::error("%s: login session skipped, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectAudio))
        {
            Application::warning("%s: display %d, redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "audio");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("audio"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: encoding: %s", __FUNCTION__, encoding.c_str());
        auto audioFormat = _config->getString("channel:audio:format", "/var/run/ltsm/audio/%{user}");
        auto audioFolder = std::filesystem::path(Tools::replace(audioFormat, "%{user}", xvfb->userInfo->user()));
        std::error_code err;

        if(! std::filesystem::is_directory(audioFolder, err))
        {
            if(! std::filesystem::create_directories(audioFolder, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, "create directory failed", audioFolder.c_str(),
                                   getuid());
                return false;
            }
        }

        // fix mode 0750
        std::filesystem::permissions(audioFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), audioFolder.c_str(),
                                 getuid());
        }

        // fix owner xvfb.user
        setFileOwner(audioFolder, Tools::getUserUid(_config->getString("user:xvfb")), xvfb->userInfo->gid());
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

    void Manager::Object::stopAudioListener(XvfbSessionPtr xvfb, const std::string & encoding)
    {
        Application::info("%s: encoding: %s", __FUNCTION__, encoding.c_str());
        auto audioFormat = _config->getString("channel:audio:format", "/var/run/ltsm/audio/%{user}");
        auto audioFolder = std::filesystem::path(Tools::replace(audioFormat, "%{user}", xvfb->userInfo->user()));
        auto destinationName = "ltsm.session.audio";
        auto objectPath = "/ltsm/session/audio";
        auto interfaceName = "LTSM.Session.AUDIO";
        auto audioSocket = std::filesystem::path(audioFolder) / std::to_string(xvfb->connectorId);
        audioSocket += ".sock";
        Application::info("%s: display: %d, user: %s, socket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), audioSocket.c_str());
#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = Manager::getSessionDBusAddresses(*xvfb->userInfo);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: dbus address empty, display: %d, user: %s", __FUNCTION__, xvfb->displayNum,
                                     xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto conn = sdbus::createSessionBusConnectionWithAddress(Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";"));
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);
            concatenatorProxy->callMethod("disconnectChannel").onInterface(interfaceName).withArguments(
                audioSocket.native()).dontExpectReply();
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %d, sdbus error: %s, msg: %s", __FUNCTION__, xvfb->displayNum,
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

    bool Manager::Object::startSaneListener(XvfbSessionPtr xvfb, const std::string & clientUrl)
    {
        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectScanner))
        {
            Application::warning("%s: display %d, redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "scanner");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("scanner"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: url: %s", __FUNCTION__, clientUrl.c_str());
        auto [ clientType, clientAddress ] = Channel::parseUrl(clientUrl);

        if(clientType == Channel::ConnectorType::Unknown)
        {
            Application::error("%s: %s, unknown client url: %s", __FUNCTION__, "sane", clientUrl.c_str());
            return false;
        }

        auto saneSocket = _config->getString("channel:sane:format", "/var/run/ltsm/sane/%{user}");
        auto socketFolder = std::filesystem::path(saneSocket).parent_path();
        std::error_code err;

        if(! std::filesystem::is_directory(socketFolder, err))
        {
            if(! std::filesystem::create_directories(socketFolder, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, "create directory failed", socketFolder.c_str(),
                                   getuid());
                return false;
            }
        }

        // fix mode 0750
        std::filesystem::permissions(socketFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), socketFolder.c_str(),
                                 getuid());
        }

        // fix owner xvfb.user
        setFileOwner(socketFolder, Tools::getUserUid(_config->getString("user:xvfb")), xvfb->userInfo->gid());
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

    bool startPcscSessionJob(Manager::Object* owner, XvfbSessionPtr xvfb, std::string pcscSocket)
    {
        // wait new session started
        while(xvfb->aliveSec() < std::chrono::seconds(3))
        {
            std::this_thread::sleep_for(550ms);
        }

        Application::info("%s: display: %d, user: %s, pcscSocket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), pcscSocket.c_str());
        auto destinationName = "ltsm.session.pcsc";
        auto objectPath = "/ltsm/session/pcsc";
        auto interfaceName = "LTSM.Session.PCSC";
#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = Manager::getSessionDBusAddresses(*xvfb->userInfo);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: dbus address empty, display: %d, user: %s", __FUNCTION__, xvfb->displayNum,
                                     xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto conn = sdbus::createSessionBusConnectionWithAddress(Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";"));
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);
            auto method1 = concatenatorProxy->createMethodCall(interfaceName, "getVersion");
            auto reply1 = concatenatorProxy->callMethod(method1);
            int32_t version = 0;
            concatenatorProxy->callMethod("getVersion").onInterface(interfaceName).storeResultsTo(version);

            if(version < LTSM_PCSC2SESSION_VERSION)
            {
                Application::error("%s: unsupported pcsc2session, version: %d", __FUNCTION__, version);
                throw service_error(NS_FuncName);
            }

            bool ret = false;
            concatenatorProxy->callMethod("connectChannel").onInterface(interfaceName).withArguments(pcscSocket).storeResultsTo(
                ret);

            if(! ret)
            {
                Application::error("%s: %s failed", __FUNCTION__, "pcsc session connect");
            }

            return ret;
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %d, sdbus error: %s, msg: %s", __FUNCTION__, xvfb->displayNum,
                               err.getName().c_str(), err.getMessage().c_str());
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

    bool Manager::Object::startPcscListener(XvfbSessionPtr xvfb, const std::string & param)
    {
        if(xvfb->mode == XvfbMode::SessionLogin)
        {
            Application::error("%s: login session skipped, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RedirectPcsc))
        {
            Application::warning("%s: display %d, redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "pcsc");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("pcsc"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: param: `%s'", __FUNCTION__, param.c_str());
        auto pcscFormat = _config->getString("channel:pcsc:format", "/var/run/ltsm/pcsc/%{user}");
        auto pcscFolder = std::filesystem::path(Tools::replace(pcscFormat, "%{user}", xvfb->userInfo->user()));
        std::error_code err;

        if(! std::filesystem::is_directory(pcscFolder, err))
        {
            if(! std::filesystem::create_directories(pcscFolder, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, "create directory failed", pcscFolder.c_str(),
                                   getuid());
                return false;
            }
        }

        // fix mode 0750
        std::filesystem::permissions(pcscFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), pcscFolder.c_str(), getuid());
        }

        // fix owner xvfb.user
        setFileOwner(pcscFolder, Tools::getUserUid(_config->getString("user:xvfb")), xvfb->userInfo->gid());
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

    void Manager::Object::stopPcscListener(XvfbSessionPtr xvfb, const std::string & param)
    {
        Application::info("%s: param: `%s'", __FUNCTION__, param.c_str());
        auto pcscFormat = _config->getString("channel:pcsc:format", "/var/run/ltsm/pcsc/%{user}");
        auto pcscFolder = std::filesystem::path(Tools::replace(pcscFormat, "%{user}", xvfb->userInfo->user()));
        auto destinationName = "ltsm.session.pcsc";
        auto objectPath = "/ltsm/session/pcsc";
        auto interfaceName = "LTSM.Session.PCSC";
        auto pcscSocket = std::filesystem::path(pcscFolder) / "sock";
        Application::info("%s: display: %d, user: %s, socket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), pcscSocket.c_str());
#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = Manager::getSessionDBusAddresses(*xvfb->userInfo);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: dbus address empty, display: %d, user: %s", __FUNCTION__, xvfb->displayNum,
                                     xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto conn = sdbus::createSessionBusConnectionWithAddress(Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";"));
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);
            concatenatorProxy->callMethod("disconnectChannel").onInterface(interfaceName).withArguments(
                pcscSocket.native()).dontExpectReply();
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %d, sdbus error: %s, msg: %s", __FUNCTION__, xvfb->displayNum,
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

    bool Manager::Object::startPkcs11Listener(XvfbSessionPtr xvfb, const std::string & param)
    {
        if(xvfb->mode != XvfbMode::SessionLogin)
        {
            Application::warning("%s: login session only, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
            return false;
        }

        Application::info("%s: param: `%s'", __FUNCTION__, param.c_str());
        auto pkcs11Format = _config->getString("channel:pkcs11:format", "/var/run/ltsm/pkcs11/%{display}");
        auto pkcs11Folder = std::filesystem::path(Tools::replace(pkcs11Format, "%{display}", xvfb->displayNum));
        std::error_code err;

        if(! std::filesystem::is_directory(pkcs11Folder, err))
        {
            if(! std::filesystem::create_directories(pkcs11Folder, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, "create directory failed", pkcs11Folder.c_str(),
                                   getuid());
                return false;
            }
        }

        // fix mode 0750
        std::filesystem::permissions(pkcs11Folder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), pkcs11Folder.c_str(),
                                 getuid());
        }

        // fix owner xvfb.user
        setFileOwner(pkcs11Folder, Tools::getUserUid(_config->getString("user:xvfb")), xvfb->userInfo->gid());
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

    void Manager::Object::stopPkcs11Listener(XvfbSessionPtr xvfb, const std::string & param)
    {
        Application::info("%s: param: `%s'", __FUNCTION__, param.c_str());
    }

    bool startFuseSessionJob(Manager::Object* owner, XvfbSessionPtr xvfb, std::string localPoint, std::string remotePoint,
                             std::string fuseSocket)
    {
        // wait new session started
        while(xvfb->aliveSec() < std::chrono::seconds(3))
        {
            std::this_thread::sleep_for(550ms);
        }

        Application::info("%s: display: %d, user: %s, localPoint: `%s', remotePoint: `%s', fuseSocket: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), localPoint.c_str(), remotePoint.c_str(), fuseSocket.c_str());
        auto destinationName = "ltsm.session.fuse";
        auto objectPath = "/ltsm/session/fuse";
        auto interfaceName = "LTSM.Session.FUSE";
#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = Manager::getSessionDBusAddresses(*xvfb->userInfo);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: dbus address empty, display: %d, user: %s", __FUNCTION__, xvfb->displayNum,
                                     xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto conn = sdbus::createSessionBusConnectionWithAddress(Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";"));
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);
            auto method1 = concatenatorProxy->createMethodCall(interfaceName, "getVersion");
            auto reply1 = concatenatorProxy->callMethod(method1);
            int32_t version = 0;
            concatenatorProxy->callMethod("getVersion").onInterface(interfaceName).storeResultsTo(version);

            if(version < LTSM_FUSE2SESSION_VERSION)
            {
                Application::error("%s: unsupported fuse2session, version: %d", __FUNCTION__, version);
                throw service_error(NS_FuncName);
            }

            bool ret = false;
            concatenatorProxy->callMethod("mountPoint").onInterface(interfaceName).withArguments(localPoint, remotePoint,
                    fuseSocket).storeResultsTo(ret);

            if(! ret)
            {
                Application::error("%s: %s failed", __FUNCTION__, "fuse session mount");
            }

            return ret;
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %d, sdbus error: %s, msg: %s", __FUNCTION__, xvfb->displayNum,
                               err.getName().c_str(), err.getMessage().c_str());
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

    bool Manager::Object::startFuseListener(XvfbSessionPtr xvfb, const std::string & remotePoint)
    {
        if(xvfb->mode == XvfbMode::SessionLogin)
        {
            Application::error("%s: login session skipped, display: %" PRId32, __FUNCTION__, xvfb->displayNum);
            return false;
        }

        if(! xvfb->checkStatus(Flags::AllowChannel::RemoteFilesUse))
        {
            Application::warning("%s: display %d, redirect disabled: %s", __FUNCTION__, xvfb->displayNum, "fuse");
            busSendNotify(xvfb->displayNum, "Channel Disabled",
                          Tools::StringFormat("redirect %1 is blocked, contact the administrator").arg("fuse"),
                          NotifyParams::IconType::Warning, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        Application::info("%s: remote point: %s", __FUNCTION__, remotePoint.c_str());
        auto userShareFormat = _config->getString("channel:fuse:format", "/var/run/ltsm/fuse/%{user}");
        auto userShareFolder = Tools::replace(userShareFormat, "%{user}", xvfb->userInfo->user());
        auto fusePointName = std::filesystem::path(remotePoint).filename();
        auto fusePointFolder = std::filesystem::path(userShareFolder) / fusePointName;
        std::error_code err;

        if(! std::filesystem::is_directory(fusePointFolder, err))
        {
            if(! std::filesystem::create_directories(fusePointFolder, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, "create directory failed", fusePointFolder.c_str(),
                                   getuid());
                return false;
            }
        }

        // fix mode 0750
        std::filesystem::permissions(userShareFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), fusePointFolder.c_str(),
                                 getuid());
        }

        // fix owner xvfb.user
        setFileOwner(userShareFolder, Tools::getUserUid(_config->getString("user:xvfb")), xvfb->userInfo->gid());
        // fix mode 0700
        std::filesystem::permissions(fusePointFolder, std::filesystem::perms::group_all | std::filesystem::perms::others_all,
                                     std::filesystem::perm_options::remove, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), fusePointFolder.c_str(),
                                 getuid());
        }

        // fix owner user.user
        setFileOwner(fusePointFolder, xvfb->userInfo->uid(), xvfb->userInfo->gid());
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

    void Manager::Object::stopFuseListener(XvfbSessionPtr xvfb, const std::string & remotePoint)
    {
        auto userShareFormat = _config->getString("channel:fuse:format", "/var/run/ltsm/fuse/%{user}");
        auto userShareFolder = Tools::replace(userShareFormat, "%{user}", xvfb->userInfo->user());
        auto fusePointName = std::filesystem::path(remotePoint).filename();
        auto fusePointFolder = std::filesystem::path(userShareFolder) / fusePointName;
        auto destinationName = "ltsm.session.fuse";
        auto objectPath = "/ltsm/session/fuse";
        auto interfaceName = "LTSM.Session.FUSE";
        auto localPoint = fusePointFolder.native();
        Application::info("%s: display: %d, user: %s, localPoint: `%s'",
                          __FUNCTION__, xvfb->displayNum, xvfb->userInfo->user(), localPoint.c_str());
#ifdef SDBUS_ADDRESS_SUPPORT

        try
        {
            auto dbusAddresses = Manager::getSessionDBusAddresses(*xvfb->userInfo);

            if(dbusAddresses.empty())
            {
                Application::warning("%s: dbus address empty, display: %d, user: %s", __FUNCTION__, xvfb->displayNum,
                                     xvfb->userInfo->user());
                throw service_error(NS_FuncName);
            }

            auto conn = sdbus::createSessionBusConnectionWithAddress(Tools::join(dbusAddresses.begin(), dbusAddresses.end(), ";"));
            auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);
            concatenatorProxy->callMethod("umountPoint").onInterface(interfaceName).withArguments(localPoint).dontExpectReply();
        }
        catch(const sdbus::Error & err)
        {
            Application::error("%s: failed, display: %d, sdbus error: %s, msg: %s", __FUNCTION__, xvfb->displayNum,
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

    void Manager::Object::busSetDebugLevel(const std::string & level)
    {
        Application::info("%s: level: %s", __FUNCTION__, level.c_str());
        Application::setDebugLevel(level);
    }

    void Manager::Object::busSetConnectorDebugLevel(const int32_t & display, const std::string & level)
    {
        Application::info("%s: display: %" PRId32 ", level: %s", __FUNCTION__, display, level.c_str());
        emitDebugLevel(display, level);
    }

    void Manager::Object::busSetChannelDebug(const int32_t & display, const uint8_t & channel, const bool & debug)
    {
        Application::info("%s: display: %" PRId32 ", channel: %" PRIu8 ", debug: %d", __FUNCTION__, display, channel,
                          static_cast<int>(debug));
        emitDebugChannel(display, channel, debug);
    }

    std::string Manager::Object::busEncryptionInfo(const int32_t & display)
    {
        if(auto xvfb = findDisplaySession(display))
        {
            return xvfb->encryption;
        }

        return "none";
    }

    bool Manager::Object::busDisplayResized(const int32_t & display, const uint16_t & width, const uint16_t & height)
    {
        if(auto xvfb = findDisplaySession(display))
        {
            Application::info("%s: display: %" PRId32 ", width: %" PRIu16 ", height: %" PRIu16, __FUNCTION__, display, width,
                              height);
            xvfb->width = width;
            xvfb->height = height;
            emitHelperWidgetCentered(display);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetEncryptionInfo(const int32_t & display, const std::string & info)
    {
        Application::info("%s encryption: %s, display: %" PRId32, __FUNCTION__, info.c_str(), display);

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->encryption = info;
            emitSessionChanged(display);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetSessionDurationSec(const int32_t & display, const uint32_t & duration)
    {
        Application::info("%s: duration: %" PRIu32 ", display: %" PRId32, __FUNCTION__, duration, display);

        if(auto xvfb = findDisplaySession(display))
        {
            xvfb->durationLimit = duration;
            emitClearRenderPrimitives(display);
            emitSessionChanged(display);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetSessionPolicy(const int32_t & display, const std::string & policy)
    {
        Application::info("%s: policy: %s, display: %" PRId32, __FUNCTION__, policy.c_str(), display);

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
                Application::error("%s: unknown policy: %s, display: %" PRId32, __FUNCTION__, policy.c_str(), display);
            }

            emitSessionChanged(display);
            return true;
        }

        return false;
    }

    bool Manager::Object::helperSetSessionLoginPassword(const int32_t & display, const std::string & login,
            const std::string & password, const bool & action)
    {
        Application::info("%s: login: %s, display: %" PRId32, __FUNCTION__, login.c_str(), display);
        emitHelperSetLoginPassword(display, login, password, action);
        return true;
    }

    std::string Manager::Object::busGetSessionJson(const int32_t & display)
    {
        if(auto xvfb = findDisplaySession(display))
        {
            return xvfb->toJsonString();
        }

        return "{}";
    }

    std::string Manager::Object::busGetSessionsJson(void)
    {
        return XvfbSessions::toJsonString();
    }

    bool Manager::Object::busRenderRect(const int32_t & display,
                                        const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect,
                                        const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill)
    {
        emitAddRenderRect(display, rect, color, fill);
        return true;
    }

    bool Manager::Object::busRenderText(const int32_t & display, const std::string & text,
                                        const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color)
    {
        emitAddRenderText(display, text, pos, color);
        return true;
    }

    bool Manager::Object::busRenderClear(const int32_t & display)
    {
        emitClearRenderPrimitives(display);
        return true;
    }

    bool Manager::Object::busCreateChannel(const int32_t & display, const std::string & client, const std::string & cmode,
                                           const std::string & server, const std::string & smode, const std::string & speed)
    {
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

    bool Manager::Object::busDestroyChannel(const int32_t & display, const uint8_t & channel)
    {
        emitDestroyChannel(display, channel);
        return true;
    }

    /* Manager::Service */
    Manager::Service::Service(int argc, const char** argv)
        : ApplicationJsonConfig("ltsm_service")
    {
        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--background"))
            {
                isBackground = true;
            }
            else if(0 == std::strcmp(argv[it], "--config") && it + 1 < argc)
            {
                readConfig(argv[it + 1]);
                it = it + 1;
            }
            else
            {
                std::cout << "usage: " << argv[0] << " --config <path> [--background]" << std::endl;
                throw 0;
            }
        }

        if(! config().isValid())
        {
            Application::error("%s: %s failed", __FUNCTION__, "config");
            throw std::invalid_argument(__FUNCTION__);
        }

        // check present executable files
        for(auto key : config().keys())
        {
            if(5 < key.size() && 0 == key.substr(key.size() - 5).compare(":path") &&
                    0 != std::isalpha(key.front()) /* skip comment */)
            {
                auto value = configGetString(key);
                std::error_code err;

                if(! std::filesystem::exists(value, err))
                {
                    Application::error("%s: path not found: `%s'", "CheckProgram", value.c_str());
                    throw std::invalid_argument(__FUNCTION__);
                }
            }
        }
    }

    bool Manager::Service::createXauthDir(void)
    {
        auto xauthFile = configGetString("xauth:file", "/var/run/ltsm/auth_%{display}");
        auto groupAuth = configGetString("group:auth");
        // find group id
        gid_t setgid = Tools::getGroupGid(groupAuth);
        // check directory
        auto folderPath = std::filesystem::path(xauthFile).parent_path();

        if(! folderPath.empty())
        {
            std::error_code err;

            // create
            if(! std::filesystem::is_directory(folderPath, err))
            {
                if(! std::filesystem::create_directory(folderPath, err))
                {
                    Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, "create directory failed", folderPath.c_str(),
                                       getuid());
                    return false;
                }
            }

            // fix mode 0755
            std::filesystem::permissions(folderPath, std::filesystem::perms::owner_all |
                                         std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_read | std::filesystem::perms::others_exec, std::filesystem::perm_options::replace, err);

            if(err)
            {
                Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), folderPath.c_str(), getuid());
            }

            setFileOwner(folderPath, 0, setgid);
            return true;
        }

        return false;
    }

    bool Manager::Service::inotifyWatchConfigStart(void)
    {
        std::string filename = configGetString("config:path", "/etc/ltsm/config.json");
        int fd = inotify_init();

        if(0 > fd)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "inotify_init", strerror(errno), errno);
            return false;
        }

        int wd = inotify_add_watch(fd, filename.c_str(), IN_CLOSE_WRITE);

        if(0 > wd)
        {
            Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "inotify_add_watch", strerror(errno),
                               errno, filename.c_str());
            return false;
        }

        Application::info("%s: path: `%s'", __FUNCTION__, filename.c_str());

        if(0 > fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fcntl", strerror(errno), errno);
            return false;
        }

        timerInotifyWatchConfig = Tools::BaseTimer::create<std::chrono::seconds>(3, true, [fd1 = fd, this]()
        {
            // read single inotify_event (16byte)
            const int bufsz = sizeof(struct inotify_event);
            char buf[bufsz];
            auto len = read(fd1, buf, sizeof(buf));

            if(0 < len)
            {
                auto filename = this->configGetString("config:path", "/etc/ltsm/config.json");
                JsonContentFile jsonFile(filename);

                if(! jsonFile.isValid() || ! jsonFile.isObject())
                {
                    Application::error("%s: reload config %s, file: %s", "InotifyWatch", "failed", filename.c_str());
                }
                else
                {
                    this->configSet(jsonFile.toObject());
                    Application::notice("%s: reload config %s, file: %s", "InotifyWatch", "success", filename.c_str());

                    if(Manager::serviceAdaptor)
                    {
                        Manager::serviceAdaptor->configReloadedEvent();
                    }
                }
            }
        });

        return true;
    }

    int Manager::Service::start(void)
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

        auto conn = sdbus::createSystemBusConnection(LTSM::dbus_manager_service_name);

        if(! conn)
        {
            Application::error("%s: dbus connection failed", "ServiceStart");
            return EXIT_FAILURE;
        }

        auto xvfbHome = Tools::getUserHome(configGetString("user:xvfb"));
        std::error_code err;

        if(! std::filesystem::is_directory(xvfbHome, err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", "ServiceStart", err.message().c_str(), xvfbHome.c_str(), getuid());
            return EXIT_FAILURE;
        }

        LTSM::Application::setDebug(LTSM::DebugTarget::Syslog, LTSM::DebugLevel::Info);

        // remove old sockets
        for(auto const & dirEntry : std::filesystem::directory_iterator{xvfbHome})
        {
            if(dirEntry.is_socket(err))
            {
                std::filesystem::remove(dirEntry, err);
            }

            if(err)
            {
                Application::warning("%s: %s, path: `%s', uid: %d", "ServiceStart", err.message().c_str(), dirEntry.path().c_str(),
                                     getuid());
            }
        }

        signal(SIGTERM, signalHandler);
        //signal(SIGCHLD, signalHandler);
        signal(SIGINT, isBackground ? SIG_IGN : signalHandler);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        createXauthDir();
        int min = config().getInteger("display:min", 55);
        int max = config().getInteger("display:max", 99);
        serviceAdaptor.reset(new Manager::Object(*conn, config(), std::abs(max - min), *this));
        Manager::serviceRunning = true;
        inotifyWatchConfigStart();

        // deprecated
        if(auto str = config().getString("service:debug"); !str.empty())
        {
            Application::setDebugLevel(str);
        }

        if(auto str = config().getString("service:debug:level", "info"); !str.empty())
        {
            Application::setDebugLevel(str);
        }

        if(auto arr = config().getArray("service:debug:types"))
        {
            Application::setDebugTypes(Tools::debugTypes(arr->toStdList<std::string>()));
        }

        Application::notice("%s: runtime version: %d", "ServiceStart", LTSM::service_version);
#ifdef WITH_SYSTEMD
        sd_notify(0, "READY=1");
#endif

        while(Manager::serviceRunning)
        {
            conn->enterEventLoopAsync();
            std::this_thread::sleep_for(10ms);

            if(Manager::serviceKilled)
            {
                Application::notice("%s: receive kill signal", "ServiceStart");
                serviceAdaptor->shutdownService();
                Manager::serviceKilled = false;
            }
        }

#ifdef WITH_SYSTEMD
        sd_notify(0, "STOPPING=1");
#endif
        timerInotifyWatchConfig->stop();
        // wait dbus 100ms
        auto tp = std::chrono::steady_clock::now();

        while(true)
        {
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tp);

            if(dt.count() > 100)
            {
                break;
            }

            conn->enterEventLoopAsync();
            std::this_thread::sleep_for(10ms);
        }

        serviceAdaptor.reset();
        return EXIT_SUCCESS;
    }

    void Manager::Service::signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
        {
            Manager::serviceKilled = true;
        }
    }
}

int main(int argc, const char** argv)
{
    int res = 0;

    try
    {
        LTSM::Manager::Service app(argc, argv);
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
