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

#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <signal.h>

#include <sys/wait.h>
#include <sys/fcntl.h>
#include <sys/inotify.h>

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

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_sockets.h"
#include "ltsm_service.h"
#include "ltsm_channels.h"
#include "ltsm_xcb_wrapper.h"

using namespace std::chrono_literals;

namespace LTSM
{
    void XvfbSession::destroy(void)
    {
        if(0 < pid2)
        {
            int status;
            // kill session
            Application::debug("%s: kill %s, pid: %d", __FUNCTION__, "helper", pid2);
            kill(pid2, SIGTERM);
            pid2 = 0;
        }

        if(0 < pid1)
        {
            int status;
            // kill xvfb
            Application::debug("%s: kill %s, pid: %d", __FUNCTION__, "xvfb", pid1);
            kill(pid1, SIGTERM);
            pid1 = 0;
        }

        if(xauthfile.size())
        {
            // remove xautfile
            std::filesystem::remove(xauthfile);
            xauthfile.clear();
        }
    }

    /* XvfbSessions */
    XvfbSessions::~XvfbSessions()
    {
        for(auto it = _xvfb->begin(); it != _xvfb->end(); ++it)
            (*it).second.destroy();
    }

    std::pair<int, XvfbSession*> XvfbSessions::findUserSession(const std::string & username)
    {
        auto it = std::find_if(_xvfb->begin(), _xvfb->end(), [&](auto & pair)
        {
            return (pair.second.mode == XvfbMode::SessionOnline || pair.second.mode == XvfbMode::SessionSleep) &&
                   pair.second.user == username;
        });

        if(it != _xvfb->end())
            return std::make_pair((*it).first, &(*it).second);

        return std::make_pair<int, XvfbSession*>(-1, nullptr);
    }

    XvfbSession* XvfbSessions::getXvfbInfo(int screen)
    {
        auto it = _xvfb->find(screen);
        return it != _xvfb->end() ? & (*it).second : nullptr;
    }

    void XvfbSessions::removeXvfbDisplay(int screen)
    {
        auto it = _xvfb->find(screen);

        if(it != _xvfb->end())
        {
            (*it).second.destroy();
            _xvfb->erase(it);
        }
    }

    XvfbSession* XvfbSessions::registryXvfbSession(int screen, XvfbSession && st)
    {
        auto res = _xvfb->emplace(screen, std::move(st));
        return & res.first->second;
    }

    std::vector<xvfb2tuple> XvfbSessions::toSessionsList(void)
    {
        std::vector<xvfb2tuple> res;
        res.reserve(_xvfb->size());

        for(auto it = _xvfb->begin(); it != _xvfb->end(); ++it)
        {
            const auto & [display, session] = *it;
            int32_t sesmode = 0; // SessionLogin

            switch(session.mode)
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

            int32_t conpol = 0; // AuthLock

            switch(session.policy)
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

            res.emplace_back(
                display,
                session.pid1,
                session.pid2,
                session.width,
                session.height,
                session.uid,
                session.gid,
                session.durationlimit,
                sesmode,
                conpol,
                session.user,
                session.xauthfile,
                session.remoteaddr,
                session.conntype,
                session.encryption
            );
        }

        return res;
    }

    int Manager::Object::getFreeDisplay(void) const
    {
        int min = _config->getInteger("display:min", 55);
        int max = _config->getInteger("display:max", 99);

        if(max < min) std::swap(max, min);

        std::vector<int> ranges(max - min, 0);
        std::iota(ranges.begin(), ranges.end(), min);
        auto it = std::find_if(ranges.begin(), ranges.end(), [&](auto display)
        {
            return ! checkXvfbLocking(display) && ! checkXvfbSocket(display);
        });
        return it != ranges.end() ? *it : -1;
    }

    std::tuple<uid_t, gid_t, std::filesystem::path, std::string> Manager::getUserInfo(std::string_view user)
    {
        Application::debug("%s: user: %s", __FUNCTION__, user.data());

        if(user.size())
        {
            struct passwd* st = getpwnam(user.data());

            if(st)
                return std::make_tuple<uid_t, gid_t, std::filesystem::path, std::string>((uid_t)st->pw_uid, (gid_t)st->pw_gid, st->pw_dir, st->pw_shell);

            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "getpwnam", strerror(errno), errno);
        }

        return std::make_tuple<uid_t, gid_t, std::filesystem::path, std::string>(0, 0, "/tmp", "/bin/false");
    }

    uid_t Manager::getUserUid(std::string_view user)
    {
        auto userInfo = getUserInfo(user);
        return std::get<0>(userInfo);
    }

    gid_t Manager::getUserGid(std::string_view user)
    {
        auto userInfo = getUserInfo(user);
        return std::get<1>(userInfo);
    }

    std::filesystem::path Manager::getUserHome(std::string_view user)
    {
        auto userInfo = getUserInfo(user);
        return std::get<2>(userInfo);
    }

    gid_t Manager::getGroupGid(std::string_view group)
    {
        Application::debug("%s: group: %s", __FUNCTION__, group.data());

        if(group.size())
        {
            struct group* group_st = getgrnam(group.data());

            if(group_st)
                return group_st->gr_gid;

            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "getgrnam", strerror(errno), errno);
        }

        return 0;
    }

    std::list<std::string> Manager::getSessionDbusAddresses(std::string_view user)
    {
        auto home = getUserHome(user);
        auto dbusPath = std::filesystem::path(home) / ".dbus" / "session-bus";
        std::list<std::string> dbusAddresses;

        if(std::filesystem::is_directory(dbusPath))
        {
            std::string_view dbusLabel = "DBUS_SESSION_BUS_ADDRESS='";

            for(auto const & dirEntry : std::filesystem::directory_iterator{dbusPath})
            {
                std::ifstream ifs(dirEntry.path());
                std::string line;

                while(std::getline(ifs, line))
                {
                    auto pos = line.find(dbusLabel);
                    if(pos != std::string::npos)
                    {
                        dbusAddresses.emplace_back(line.substr(pos + dbusLabel.size()));
                        // remove last \'
                        dbusAddresses.back().pop_back();
                    }
                }
            }
        }
        else
        {
            Application::error("%s: path not found: `%s'", __FUNCTION__, dbusPath.c_str());
        }

        return dbusAddresses;
    }

    std::list<std::string> Manager::getGroupMembers(std::string_view group)
    {
        Application::debug("%s: group: %s", __FUNCTION__, group.data());

        std::list<std::string> res;
        struct group* group_st = getgrnam(group.data());

        if(! group_st)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "getgrnam", strerror(errno), errno);
            return res;
        }

        if(group_st->gr_mem)
        {
            while(const char* memb =  *(group_st->gr_mem))
            {
                res.emplace_back(memb);
                group_st->gr_mem++;
            }
        }

        return res;
    }

    std::list<std::string> Manager::getSystemUsersRange(int uidMin, int uidMax)
    {
        std::list<std::string> logins;
        setpwent();

        if(uidMin > uidMax)
            std::swap(uidMin, uidMax);

        while(struct passwd* st = getpwent())
        {
            if((uidMin <= 0 || uidMin <= st->pw_uid) && (uidMax <= 0 || st->pw_uid <= uidMax))
                logins.emplace_back(st->pw_name);
        }

        endpwent();
        return logins;
    }

    void Manager::closefds(void)
    {
        long fdlimit = sysconf(_SC_OPEN_MAX);

        for(int fd = STDERR_FILENO + 1; fd < fdlimit; fd++)
            close(fd);
    }

    bool Manager::checkFileReadable(const std::filesystem::path & path)
    {
        Application::debug("%s: path: %s", __FUNCTION__, path.c_str());

        return 0 == access(path.c_str(), R_OK);
    }

    void Manager::setFileOwner(const std::filesystem::path & path, uid_t uid, gid_t gid)
    {
        Application::debug("%s: path: %s, uid: %d, gid: %d", __FUNCTION__, path.c_str(), uid, gid);

        if(0 != chown(path.c_str(), uid, gid))
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "chown", strerror(errno), errno);
    }

    bool Manager::runSystemScript(int display, const std::string & user, const std::string & cmd)
    {
        if(cmd.empty())
            return false;

        if(! std::filesystem::exists(cmd.substr(0, cmd.find(0x20))))
        {
            Application::warning("%s: path not found: `%s'", __FUNCTION__, cmd.c_str());
            return false;
        }

        auto str = Tools::replace(cmd, "%{display}", display);
        str = Tools::replace(str, "%{user}", user);
        std::thread([str = std::move(str), screen = display]()
        {
            int ret = std::system(str.c_str());
            Application::debug("%s: run command: `%s', return code: %d, display: %d", __FUNCTION__, str.c_str(), ret, screen);
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

    bool Manager::switchToUser(const std::string & user)
    {
        auto [uid, gid, home, shell] = getUserInfo(user);
        Application::debug("%s: uid: %d, gid: %d, home:`%s', shell: `%s'", __FUNCTION__, uid, gid, home.c_str(), shell.c_str());

        // set groups
        std::string sgroups;
        gid_t groups[8];
        int ngroups = 8;
        int ret = getgrouplist(user.c_str(), gid, groups, & ngroups);

        if(0 < ret)
        {
            setgroups(ret, groups);

            for(int it = 0; it < ret; ++it)
            {
                sgroups.append(std::to_string(groups[it]));

                if(it + 1 < ret) sgroups.append(",");
            }
        }

        if(0 != chdir(home.c_str()))
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "chdir", strerror(errno), errno);

        if(0 != setgid(gid))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "setgid", strerror(errno), errno);
            return false;
        }

        if(0 != setuid(uid))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "setuid", strerror(errno), errno);
            return false;
        }

        setenv("USER", user.c_str(), 1);
        setenv("LOGNAME", user.c_str(), 1);
        setenv("HOME", home.c_str(), 1);
        setenv("SHELL", shell.c_str(), 1);
        setenv("TERM", "linux", 1);

        Application::debug("%s: groups: (%s), current dir: `%s'", __FUNCTION__, sgroups.c_str(), get_current_dir_name());
        return true;
    }

    /// RunAs namespace
    class RunAs
    {
    public:
        static int waitPid(pid_t pid)
        {
            Application::debug("%s: pid: %d", __FUNCTION__, pid);

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
                    Application::debug("%s: process ended, pid: %d, status: %d", __FUNCTION__, pid, status);
                }
            }

            return status;
        }

    private:
        static void childProcess(const XvfbSession* xvfb, int pipeout, const std::filesystem::path & cmd, std::list<std::string> params)
        {
            openlog("ltsm_service", 0, LOG_USER);

            Application::info("%s: pid: %d, cmd: `%s %s'", __FUNCTION__, getpid(), cmd.c_str(), Tools::join(params, " ").c_str());

            long fdlimit = sysconf(_SC_OPEN_MAX);
            for(int fd = STDERR_FILENO + 1; fd < fdlimit; fd++)
                if(fd != pipeout) close(fd);

            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);

            if(Manager::switchToUser(xvfb->user))
            {
                for(auto & [key, val] : xvfb->environments)
                    setenv(key.c_str(), val.c_str(), 1);

                setenv("XAUTHORITY", xvfb->xauthfile.c_str(), 1);
                setenv("DISPLAY", xvfb->display.c_str(), 1);
                setenv("LTSM_REMOTEADDR", xvfb->remoteaddr.c_str(), 1);
                setenv("LTSM_TYPECONN", xvfb->conntype.c_str(), 1);

                std::vector<const char*> argv;
                argv.reserve(params.size() + 2);

                // create argv[]
                argv.push_back(cmd.c_str());
                for(auto & val : params)
                    if(! val.empty()) argv.push_back(val.c_str());
                argv.push_back(nullptr);

                int null = open("/dev/null", 0);
                if(0 <= null)
                {
                    if(0 > dup2(null, STDERR_FILENO))
                        Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "dup2", strerror(errno), errno);
                    close(null);
                }

                // close stdout
                if(0 > pipeout)
                {
                    null = open("/dev/null", 0);
                    if(0 <= null)
                    {
                        if(0 > dup2(null, STDOUT_FILENO))
                            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "dup2", strerror(errno), errno);
                        close(null);
                    }
                }
                else
                // redirect stdout
                {
                    if(0 > dup2(pipeout, STDOUT_FILENO))
                        Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "dup2", strerror(errno), errno);
                    close(pipeout);
                }

                int res = execv(cmd.c_str(), (char* const*) argv.data());
                if(res < 0)
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "execv", strerror(errno), errno);
            }

            closelog();
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
                res.clear();

            int status = waitPid(pid);

            return std::make_pair(status, std::move(res));
        }

    public:
        static PidStatusStdout sessionCommandStdout(const XvfbSession* xvfb, const std::filesystem::path & cmd, std::list<std::string> params)
        {
            if(! xvfb)
            {
                Application::error("%s: xvfb session null", __FUNCTION__);
                throw service_error(NS_FuncName);
            }

            if(! std::filesystem::exists(cmd))
            {
                Application::error("%s: path not found: `%s'", __FUNCTION__, cmd.c_str());
                throw service_error(NS_FuncName);
            }

            auto & userName = xvfb->user;
            Application::info("%s: request for user: %s, display: %s, cmd: `%s'", __FUNCTION__, userName.c_str(), xvfb->display.c_str(), cmd.c_str());
            auto [uid, gid, home, shell] = Manager::getUserInfo(userName);

            if(! std::filesystem::is_directory(home))
            {
                Application::error("%s: path not found: `%s', user: %s", __FUNCTION__, home.c_str(), userName.c_str());
                throw service_error(NS_FuncName);
            }

            // run login session or autorized session only
            if(xvfb->mode != XvfbMode::SessionLogin && ! xvfb->pamh)
            {
                Application::error("%s: session not authorized, user: %s", __FUNCTION__, userName.c_str());
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
                closelog();
                close(pipefd[0]);
                childProcess(xvfb, pipefd[1], cmd, std::move(params));
                // child ended
                std::exit(0);
            }
        
            // main thread processed
            close(pipefd[1]);

            if(0 > fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK))
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fcntl", strerror(errno), errno);

            // planned get stdout from running job
            auto future = std::async(std::launch::async, jobWaitStdout, pid, pipefd[0]);
            return std::make_pair(pid, std::move(future));
        }

        static PidStatus sessionCommand(const XvfbSession* xvfb, const std::filesystem::path & cmd, std::list<std::string> params)
        {
            if(! xvfb)
            {
                Application::error("%s: xvfb session null", __FUNCTION__);
                throw service_error(NS_FuncName);
            }

            if(! std::filesystem::exists(cmd))
            {
                Application::error("%s: path not found: `%s'", __FUNCTION__, cmd.c_str());
                throw service_error(NS_FuncName);
            }

            auto & userName = xvfb->user;
            Application::info("%s: request for user: %s, display: %s, cmd: `%s'", __FUNCTION__, userName.c_str(), xvfb->display.c_str(), cmd.c_str());
            auto [uid, gid, home, shell] = Manager::getUserInfo(userName);

            if(! std::filesystem::is_directory(home))
            {
                Application::error("%s: path not found: `%s', user: %s", __FUNCTION__, home.c_str(), userName.c_str());
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
                closelog();
                childProcess(xvfb, -1, cmd, std::move(params));
                // child ended
                std::exit(0);
            }
        
            // main thread processed
            auto future = std::async(std::launch::async, [pid]
            {
                Application::debug("%s: pid: %d", "AsyncWaitPid", pid);

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
    Manager::Object::Object(sdbus::IConnection & conn, const JsonObject & jo, const Application & app)
        : AdaptorInterfaces(conn, LTSM::dbus_object_path), _app(& app), _config(& jo)
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

        _running = true;
    }

    Manager::Object::~Object()
    {
        unregisterAdaptor();
    }

    bool Manager::Object::isRunning(void) const
    {
        return _running;
    }

    void Manager::Object::shutdown(void)
    {
        busShutdownService();
    }

    void Manager::Object::openlog(void) const
    {
        _app->openlog();
    }

    void Manager::Object::sessionsTimeLimitAction(void)
    {
        for(auto it = _xvfb->begin(); it != _xvfb->end(); ++it)
        {
            auto & [ display, session] = *it;

            // find timepoint session limit
            if(session.mode != XvfbMode::SessionLogin && 0 < session.durationlimit)
            {
                // task background
                std::thread([display = (*it).first, xvfb = & session, this]()
                {
                    auto sessionAliveSec = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - xvfb->tpstart);
                    auto lastsec = std::chrono::seconds(xvfb->durationlimit) - sessionAliveSec;

                    // shutdown session
                    if(std::chrono::seconds(xvfb->durationlimit) < sessionAliveSec)
                    {
                        Application::notice("time point limit, display: %d", display);
                        displayShutdown(display, true, xvfb);
                    }
                    else

                        // inform alert
                        if(std::chrono::seconds(100) > lastsec)
                        {
                            this->emitClearRenderPrimitives(display);
                            // send render rect
                            const uint16_t fw = xvfb->width;
                            const uint16_t fh = 24;
                            this->emitAddRenderRect(display, {0, 0, fw, fh}, {0x10, 0x17, 0x80}, true);
                            // send render text
                            std::string text("time left: ");
                            text.append(std::to_string(lastsec.count())).append("sec");
                            const int16_t px = (fw - text.size() * 8) / 2;
                            const int16_t py = (fh - 16) / 2;
                            this->emitAddRenderText(display, text, {px, py}, {0xFF, 0xFF, 0});
                        }

                    // inform beep
                    if(std::chrono::seconds(10) > lastsec)
                        this->emitSendBellSignal(display);
                }).detach();
            }
        }
    }

    void Manager::Object::sessionsEndedAction(void)
    {
        // childEnded
        if(! _childsRunning.empty())
        {
            std::scoped_lock<std::mutex> guard(_lockRunning);

            _childsRunning.remove_if([this](auto & pidStatus)
            {
                if(pidStatus.second.wait_for(std::chrono::milliseconds(3)) != std::future_status::ready)
                    return false;

                // find child
                auto it = std::find_if(this->_xvfb->begin(), this->_xvfb->end(), [pid2 = pidStatus.first](auto & pair)
                {
                    return pair.second.pid2 == pid2;
                });

                pidStatus.second.wait();

                if(it != this->_xvfb->end())
                {
                    auto & [ display, session] = *it;

                    // skip login helper, or arnormal shutdown only
                    if(session.mode != XvfbMode::SessionLogin || 0 < pidStatus.second.get())
                    {
                        session.pid2 = 0;
                        this->displayShutdown(display, true, & session);
                    }
                }

                return true;
            });
        }
    }

    void Manager::Object::sessionsCheckAliveAction(void)
    {
        for(auto it = _xvfb->begin(); it != _xvfb->end(); ++it)
        {
            auto & [ display, session] = *it;

            if(session.mode == XvfbMode::SessionOnline)
            {
                // check alive connectors
                if(! session.checkconn)
                {
                    session.checkconn = true;
                    emitPingConnector(display);
                }
                else
                    // not reply
                {
                    session.mode = XvfbMode::SessionSleep;
                    session.checkconn = false;
                    Application::warning("connector not reply, display: %d", display);
                    // complete shutdown
                    busConnectorTerminated(display);
                }
            }
        }
    }

    bool Manager::Object::checkXvfbSocket(int display) const
    {
        return 0 < display ?
               Tools::checkUnixSocket(Tools::replace(_config->getString("xvfb:socket"), "%{display}", display)) : false;
    }

    bool Manager::Object::checkXvfbLocking(int display) const
    {
        if(0 < display)
        {
            std::filesystem::path xvfbLock = Tools::replace(_config->getString("xvfb:lock"), "%{display}", display);
            return checkFileReadable(xvfbLock);
        }

        return false;
    }

    void Manager::Object::removeXvfbSocket(int display) const
    {
        if(0 < display)
        {
            std::string socketFormat = _config->getString("xvfb:socket");
            std::filesystem::path socketPath = Tools::replace(socketFormat, "%{display}", display);
            std::filesystem::remove(socketPath);
        }
    }

    bool Manager::Object::displayShutdown(int display, bool emitSignal, XvfbSession* xvfb)
    {
        if(! xvfb)
            xvfb = getXvfbInfo(display);

        if(!xvfb || xvfb->shutdown)
            return false;

        xvfb->shutdown = true;
        Application::notice("%s: %s, display: %d", __FUNCTION__, "start", display);

        if(emitSignal) emitShutdownConnector(display);

        // dbus no wait, remove background
        std::string sysuser = _config->getString("user:xvfb");
        std::string user = xvfb->user;

        if(sysuser != user)
            closeSystemSession(display, *xvfb);

        // script run in thread
        std::thread([=]()
        {
            std::this_thread::sleep_for(100ms);
            this->removeXvfbDisplay(display);
            this->removeXvfbSocket(display);
            this->emitDisplayRemoved(display);

            if(sysuser != user)
                runSystemScript(display, user, _config->getString("system:logoff"));

            Application::debug("%s: %s, display: %d", "displayShutdown", "complete", display);
        }).detach();
        return true;
    }

    void Manager::Object::closeSystemSession(int display, XvfbSession & info)
    {
        Application::info("%s: user: %s, display: %d", __FUNCTION__, info.user.c_str(), display);
        runSessionScript(display, info.user, _config->getString("session:disconnect"));

        // PAM close
        if(info.pamh)
        {
            pam_close_session(info.pamh, 0);
            pam_end(info.pamh, PAM_SUCCESS);
            info.pamh = nullptr;
        }

        // unreg sessreg
        runSystemScript(display, info.user, _config->getString("system:disconnect"));
    }

    bool Manager::Object::waitXvfbStarting(int display, uint32_t ms) const
    {
        if(0 >= display)
            return false;

        return Tools::waitCallable<std::chrono::milliseconds>(ms, 50, [=]()
        {
            return ! checkXvfbSocket(display);
        });
    }

    std::filesystem::path Manager::Object::createXauthFile(int display, const std::vector<uint8_t> & mcookie, const std::string & userName, const std::string & remoteAddr)
    {
        std::string xauthFileTemplate = _config->getString("xauth:file");
        std::string groupAuth = _config->getString("group:auth");

        xauthFileTemplate = Tools::replace(xauthFileTemplate, "%{pid}", getpid());
        xauthFileTemplate = Tools::replace(xauthFileTemplate, "%{remoteaddr}", remoteAddr);
        xauthFileTemplate = Tools::replace(xauthFileTemplate, "%{display}", display);

        std::filesystem::path xauthFilePath(xauthFileTemplate);
        Application::debug("%s: path: %s", __FUNCTION__, xauthFilePath.c_str());

        std::ofstream ofs( xauthFilePath, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc );
     
        if(ofs)
        {
            // create xautfile
            std::string_view host{"localhost"};
            std::string_view magic{"MIT-MAGIC-COOKIE-1"};
        
            uint16_t hostlen = host.size();
            uint16_t magclen = magic.size();
            uint16_t cooklen = mcookie.size();
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
            hostlen = __builtin_bswap16(hostlen);
            magclen = __builtin_bswap16(magclen);
            cooklen = __builtin_bswap16(cooklen);
#endif
            // format: 01 00 [ <host len:be16> [ host ]] [ <magic len:be16> [ magic ]] [ <cookie len:be16> [ cookie ]]
            ofs.put(1).put(0);
            ofs.write((const char*) &hostlen, 2).write(host.data(), host.size());
            ofs.write((const char*) &magclen, 2).write(magic.data(), magic.size());
            ofs.write((const char*) &cooklen, 2).write((const char*) mcookie.data(), mcookie.size());
            ofs.close();
        }
        else
        {
            Application::error("%s: create xauthfile failed, path: %s", __FUNCTION__, xauthFilePath.c_str());
            return "";
        }

        // set permissons 0440
        std::filesystem::permissions(xauthFilePath, std::filesystem::perms::owner_read |
                                     std::filesystem::perms::group_read, std::filesystem::perm_options::replace);
        setFileOwner(xauthFilePath, getUserUid(userName), getGroupGid(groupAuth));

        return xauthFilePath;
    }

    std::filesystem::path Manager::Object::createSessionConnInfo(const std::filesystem::path & home, const XvfbSession* xvfb)
    {
        auto ltsmInfo = std::filesystem::path(home) / ".ltsm" / "conninfo";
        auto dir = ltsmInfo.parent_path();

        if(! std::filesystem::is_directory(dir))
            std::filesystem::create_directory(dir);

        // set permissions 0750
        std::filesystem::permissions(dir, std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_all, std::filesystem::perm_options::remove);
        std::ofstream ofs(ltsmInfo, std::ofstream::trunc);

        if(ofs.good())
        {
            ofs << "LTSM_REMOTEADDR" << "=" << (xvfb ? xvfb->remoteaddr : "") << std::endl <<
                "LTSM_TYPECONN" << "=" << (xvfb ? xvfb->conntype : "") << std::endl;
        }
        else
            Application::error("can't create file: %s", ltsmInfo.c_str());

        ofs.close();
        return ltsmInfo;
    }

    pid_t Manager::Object::runSessionCommandSafe(const XvfbSession* xvfb, const std::filesystem::path & cmd, std::list<std::string> params)
    {
        try
        {
            std::scoped_lock<std::mutex> guard(_lockRunning);
            _childsRunning.emplace_back(
                            RunAs::sessionCommand(xvfb, cmd, std::move(params)));
            return _childsRunning.back().first;
        }
        catch(const std::system_error &)
        {
            Application::error("%s: failed, check thread limit", __FUNCTION__);
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }

        return 0;    
    }

    void Manager::Object::waitPidBackgroundSafe(pid_t pid)
    {
        // create wait pid task
        std::packaged_task<int(pid_t)> waitPidTask(RunAs::waitPid);

        std::scoped_lock<std::mutex> guard(_lockRunning);
        _childsRunning.emplace_back(std::make_pair(pid, waitPidTask.get_future()));

        std::thread(std::move(waitPidTask), pid).detach();
    }

    void Manager::Object::runSessionScript(int display, const std::string & user, const std::string & cmd)
    {
        if(cmd.size())
        {
            auto params = Tools::split(Tools::replace(
                                Tools::replace(cmd, "%{display}", display), "%{user}", user), 0x20);

            if(! params.empty())
            {
                auto bin = params.front();
                params.pop_front();

                runSessionCommandSafe(getXvfbInfo(display), bin, std::move(params));
            }
        }
    }

    pid_t Manager::Object::runXvfbDisplay(int display, uint8_t depth, uint16_t width, uint16_t height, const std::filesystem::path & xauthFile, const std::string & userXvfb)
    {
        std::string xvfbBin = _config->getString("xvfb:path");
        std::string xvfbArgs = _config->getString("xvfb:args");
        // xvfb args
        xvfbArgs = Tools::replace(xvfbArgs, "%{display}", display);
        xvfbArgs = Tools::replace(xvfbArgs, "%{depth}", depth);
        xvfbArgs = Tools::replace(xvfbArgs, "%{width}", width);
        xvfbArgs = Tools::replace(xvfbArgs, "%{height}", height);
        xvfbArgs = Tools::replace(xvfbArgs, "%{authfile}", xauthFile.native());

        Application::debug("%s: args: `%s'", __FUNCTION__, xvfbArgs.c_str());

        closelog();
        pid_t pid = fork();
        openlog();

        if(0 == pid)
        {
            // child mode
            closefds();
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);
            Application::debug("%s: uid: %d, gid: ", __FUNCTION__, getuid(), getgid());

            if(switchToUser(userXvfb))
            {
                int null = open("/dev/null", 0);
                if(0 <= null)
                {
                    if(0 > dup2(null, STDERR_FILENO))
                        Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "dup2", strerror(errno), errno);
                    close(null);
                }

                null = open("/dev/null", 0);
                if(0 <= null)
                {
                    if(0 > dup2(null, STDOUT_FILENO))
                        Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "dup2", strerror(errno), errno);
                    close(null);
                }

                // create argv
                std::list<std::string> list = Tools::split(xvfbArgs, 0x20);
                std::vector<const char*> argv;
                argv.reserve(list.size() + 2);
                argv.push_back(xvfbBin.c_str());

                for(auto & str : list)
                    argv.push_back(str.c_str());

                argv.push_back(nullptr);

                if(! checkFileReadable(xauthFile))
                    Application::error("%s: %s failed, user: %s, error: %s", __FUNCTION__, "access", userXvfb.c_str(), strerror(errno));

                int res = execv(xvfbBin.c_str(), (char* const*) argv.data());

                if(res < 0)
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "execv", strerror(errno), errno);
            }

            closelog();
            // child exit
            std::exit(0);
        }

        return pid;
    }

    int Manager::Object::runUserSession(int display, const std::filesystem::path & sessionBin, const XvfbSession & xvfb)
    {
        closelog();
        int pid = fork();
        openlog();

        if(pid < 0)
        {
            pam_close_session(xvfb.pamh, 0);
            pam_end(xvfb.pamh, PAM_SYSTEM_ERR);
        }
        else if(0 == pid)
        {
            Application::info("%s: pid: %d", __FUNCTION__, getpid());
            auto [uid, gid, home, shell] = Manager::getUserInfo(xvfb.user);

            if(uid == 0)
            {
                Application::error("%s: deny for root", __FUNCTION__);
                pam_end(xvfb.pamh, PAM_SYSTEM_ERR);
                // child exit
                std::exit(0);
            }

            if(! std::filesystem::is_directory(home))
            {
                Application::error("%s: path not found: `%s', user: %s", __FUNCTION__, home.c_str(), xvfb.user.c_str());
                pam_end(xvfb.pamh, PAM_SYSTEM_ERR);
                // child exit
                std::exit(0);
            }

            if(0 != initgroups(xvfb.user.c_str(), gid))
            {
                Application::error("%s: %s failed, user: %s, gid: %d, error: %s", __FUNCTION__, "initgroups", xvfb.user.c_str(), gid, strerror(errno));
                pam_end(xvfb.pamh, PAM_SYSTEM_ERR);
                // child exit
                std::exit(0);
            }

            // open session
            int ret = pam_setcred(xvfb.pamh, PAM_ESTABLISH_CRED);

            if(ret != PAM_SUCCESS)
            {
                Application::error("%s: %s failed, user: %s, error: %s", __FUNCTION__, "pam_setcred", xvfb.user.c_str(), pam_strerror(xvfb.pamh, ret));
                pam_end(xvfb.pamh, ret);
                // child exit
                std::exit(0);
            }

            ret = pam_open_session(xvfb.pamh, 0);

            if(ret != PAM_SUCCESS)
            {
                Application::error("%s: %s failed, user: %s, error: %s", __FUNCTION__, "pam_open_session", xvfb.user.c_str(), pam_strerror(xvfb.pamh, ret));
                ret = pam_setcred(xvfb.pamh, PAM_DELETE_CRED);
                pam_end(xvfb.pamh, ret);
                // child exit
                std::exit(0);
            }

            ret = pam_setcred(xvfb.pamh, PAM_REINITIALIZE_CRED);

            if(ret != PAM_SUCCESS)
            {
                Application::error("%s: %s failed, user: %s, error: %s", __FUNCTION__, "pam_setcred", xvfb.user.c_str(), pam_strerror(xvfb.pamh, ret));
                ret = pam_close_session(xvfb.pamh, 0);
                pam_end(xvfb.pamh, ret);
                // child exit
                std::exit(0);
            }

            // child mode
            closefds();
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);
            Application::debug("%s: child mode, type: %s, uid: %d", __FUNCTION__, "session", getuid());

            // assign groups
            if(switchToUser(xvfb.user))
            {
                for(auto & [key, val] : xvfb.environments)
                    setenv(key.c_str(), val.c_str(), 1);

                setenv("XAUTHORITY", xvfb.xauthfile.c_str(), 1);
                setenv("DISPLAY", xvfb.display.c_str(), 1);
                setenv("LTSM_REMOTEADDR", xvfb.remoteaddr.c_str(), 1);
                setenv("LTSM_TYPECONN", xvfb.conntype.c_str(), 1);

                createSessionConnInfo(home, & xvfb);
                int res = execl(sessionBin.c_str(), sessionBin.c_str(), (char*) nullptr);

                if(res < 0)
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "execl", strerror(errno), errno);
            }

            closelog();
            // child exit
            std::exit(0);
        }

        return pid;
    }

    int32_t Manager::Object::busStartLoginSession(const uint8_t & depth, const std::string & remoteAddr, const std::string & connType)
    {
        Application::info("%s: login request, remote: %s, type: %s", __FUNCTION__, remoteAddr.c_str(), connType.c_str());

        std::string userXvfb = _config->getString("user:xvfb");
        std::string groupAuth = _config->getString("group:auth");
        // get free screen
        int screen = getFreeDisplay();
        if(0 >= screen)
        {
            Application::error("%s: all displays busy", __FUNCTION__);
            return -1;
        }

        uid_t uid; gid_t gid;
        std::tie(uid, gid, std::ignore, std::ignore) = getUserInfo(userXvfb);

        int width = _config->getInteger("default:width");
        int height = _config->getInteger("default:height");

        // generate session key
        auto mcookie = Tools::randomBytes(128);
        // get xauthfile
        auto xauthFile = createXauthFile(screen, mcookie, userXvfb, remoteAddr);

        if(xauthFile.empty())
            return -1;

        std::string socketFormat = _config->getString("xvfb:socket");
        std::filesystem::path socketPath = Tools::replace(socketFormat, "%{display}", screen);

        if(std::filesystem::is_socket(socketPath))
        {
            // fixed xvfb socket pemissions 0660
            std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                         std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace);
            setFileOwner(socketPath, uid, getGroupGid(groupAuth));
        }

        auto pid1 = runXvfbDisplay(screen, depth, width, height, xauthFile, userXvfb);
        if(0 > pid1)
        {
            Application::error("%s: %s failed, code: %d", __FUNCTION__, "fork", pid1);
            std::filesystem::remove(xauthFile);
            return -1;
        }

        // parent continue
        Application::debug("%s: xvfb started, pid: %d, display: %d", __FUNCTION__, pid1, screen);

        // registered xvfb job
        waitPidBackgroundSafe(pid1);

        // wait Xvfb display starting
        if(! waitXvfbStarting(screen, 5000 /* 5 sec */))
        {
            Application::error("%s: %s failed", "busStartLoginSession", "waitXvfbStarting");
            std::filesystem::remove(xauthFile);
            return -1;
        }

        // parent continue
        struct XvfbSession st;

        st.pid1 = pid1;
        st.pid2 = 0;
        st.width = width;
        st.height = height;
        st.xauthfile = xauthFile;
        st.display = std::string(":").append(std::to_string(screen));
        st.remoteaddr = remoteAddr;
        st.conntype = connType;
        st.mode = XvfbMode::SessionLogin;
        st.uid = uid;
        st.gid = gid;
        st.user.assign(userXvfb);
        st.durationlimit = _config->getInteger("session:duration_max_sec", 0);

        // registry screen
        auto xvfb = registryXvfbSession(screen, std::move(st));

        std::string helperArgs = _config->getString("helper:args");
        if(helperArgs.size())
        {
            helperArgs = Tools::replace(helperArgs, "%{display}", screen);
            helperArgs = Tools::replace(helperArgs, "%{authfile}", xauthFile.native());
        }

        // runas login helper
        xvfb->pid2 = runSessionCommandSafe(xvfb, _config->getString("helper:path"), Tools::split(helperArgs, 0x20));
        if(0 == xvfb->pid2)
        {
            std::filesystem::remove(xauthFile);
            return -1;
        }

        // Application::debug("login helper registered, display: %d", screen);
        return screen;
    }

    int32_t Manager::Object::busStartUserSession(const int32_t & oldScreen, const std::string & userName, const std::string & remoteAddr, const std::string & connType)
    {
        std::string userXvfb = _config->getString("user:xvfb");
        std::string sessionBin = _config->getString("session:path");
        Application::info("%s: session request, user: %s, remote: %s, display: %d", __FUNCTION__, userName.c_str(), remoteAddr.c_str(), oldScreen);
        auto [uid, gid, home, shell] = getUserInfo(userName);

        if(! std::filesystem::is_directory(home))
        {
            Application::error("%s: path not found: `%s', user: %s", __FUNCTION__, home.c_str(), userName.c_str());
            return -1;
        }

        int userScreen;
        XvfbSession* userSess;
        std::tie(userScreen, userSess) = findUserSession(userName);

        if(0 <= userScreen && checkXvfbSocket(userScreen) && userSess)
        {
            // parent continue
            userSess->remoteaddr = remoteAddr;
            userSess->conntype = connType;
            userSess->mode = XvfbMode::SessionOnline;
            // update conn info
            auto file = createSessionConnInfo(home, userSess);
            setFileOwner(file, uid, gid);
            Application::debug("%s: user session connected, display: %d", __FUNCTION__, userScreen);
            emitSessionReconnect(remoteAddr, connType);
            emitSessionChanged(userScreen);
#ifdef LTSM_CHANNELS
            startSessionChannels(userScreen);
#endif
            runSessionScript(userScreen, userName, _config->getString("session:connect"));
            return userScreen;
        }

        // get owner screen
        XvfbSession* xvfb = getXvfbInfo(oldScreen);
        if(! xvfb)
        {
            Application::error("%s: display not found: %d", __FUNCTION__, oldScreen);
            return -1;
        }

        if(xvfb->mode != XvfbMode::SessionLogin)
        {
            Application::error("%s: session busy, display: %d, user: %s", __FUNCTION__, oldScreen, xvfb->user.c_str());
            return -1;
        }

        if(! xvfb->pamh)
        {
            Application::error("%s: pam not started, display: %d, user: %s", __FUNCTION__, oldScreen, xvfb->user.c_str());
            return -1;
        }

        // xauthfile pemissions 0440
        std::string groupAuth = _config->getString("group:auth");
        gid_t gidAuth = getGroupGid(groupAuth);
        std::filesystem::permissions(xvfb->xauthfile, std::filesystem::perms::owner_read |
                                     std::filesystem::perms::group_read, std::filesystem::perm_options::replace);
        setFileOwner(xvfb->xauthfile, uid, gidAuth);
        // xvfb socket pemissions 0660
        std::string socketFormat = _config->getString("xvfb:socket");
        std::filesystem::path socketPath = Tools::replace(socketFormat, "%{display}", oldScreen);
        std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace);
        setFileOwner(socketPath, uid, gidAuth);
        // registry screen
        xvfb->remoteaddr = remoteAddr;
        xvfb->conntype = connType;
        xvfb->mode = XvfbMode::SessionOnline;
        xvfb->uid = uid;
        xvfb->gid = gid;
        xvfb->user.assign(userName);
        xvfb->tpstart = std::chrono::system_clock::now();

        auto policy = _config->getString("session:policy");
        if(Tools::lower(policy) == "authtake")
            xvfb->policy = SessionPolicy::AuthTake;
        else if(Tools::lower(policy) == "authshare")
            xvfb->policy = SessionPolicy::AuthShare;

        xvfb->pid2 = runUserSession(oldScreen, sessionBin, *xvfb);
        if(xvfb->pid2 < 0)
        {
            Application::error("%s: user session failed, result: %d", __FUNCTION__, xvfb->pid2);
            return -1;
        }

        // registered session job
        waitPidBackgroundSafe(xvfb->pid2);

        // parent continue
        Application::debug("%s: user session started, pid: %d, display: %d", __FUNCTION__, xvfb->pid2, oldScreen);
        runSystemScript(oldScreen, userName, _config->getString("system:logon"));
        runSystemScript(oldScreen, userName, _config->getString("system:connect"));

        emitSessionChanged(oldScreen);
#ifdef LTSM_CHANNELS
        startSessionChannels(oldScreen);
#endif
        runSessionScript(oldScreen, userName, _config->getString("session:connect"));

        return oldScreen;
    }

    int32_t Manager::Object::busGetServiceVersion(void)
    {
        return LTSM::service_version;
    }

    std::string Manager::Object::busCreateAuthFile(const int32_t & display)
    {
        Application::info("%s: display: %d", __FUNCTION__, display);
        auto xvfb = getXvfbInfo(display);
        return xvfb ? xvfb->xauthfile : "";
    }

    bool Manager::Object::busShutdownDisplay(const int32_t & display)
    {
        Application::info("%s: display: %d", __FUNCTION__, display);
        displayShutdown(display, true, nullptr);
        return true;
    }

    bool Manager::Object::busShutdownConnector(const int32_t & display)
    {
        Application::info("%s: display: %d", __FUNCTION__, display);
        emitShutdownConnector(display);
        return true;
    }

    bool Manager::Object::busShutdownService(void)
    {
        Application::info("%s: %s, pid: %d", __FUNCTION__, "starting", getpid());

        // terminate connectors
        for(auto it = _xvfb->begin(); it != _xvfb->end(); ++it)
        {
            auto & [ display, session] = *it;
            if(! session.shutdown)
                displayShutdown(display, true, & session);
        }

        // wait sessions
        if(int sessions = std::count_if(_xvfb->begin(), _xvfb->end(), [](auto & val){ return ! val.second.shutdown; }))
        {
            Application::info("%s: wait displays: %d", __FUNCTION__, sessions);
            while(std::any_of(_xvfb->begin(), _xvfb->end(), [](auto & val){ return ! val.second.shutdown; }))
                std::this_thread::sleep_for(100ms);
        }

        _running = false;

        if(! _childsRunning.empty())
        {
            std::scoped_lock<std::mutex> guard(_lockRunning);
            Application::error("%s: running childs: %d, killed process", __FUNCTION__, _childsRunning.size());

            for(auto & [pid, futureStatus] : _childsRunning)
                kill(pid, SIGTERM);

            std::this_thread::sleep_for(100ms);

            for(auto & [pid, futureStatus] : _childsRunning)
                futureStatus.wait();
        }

        Application::notice("%s: %s, pid: %d", __FUNCTION__, "complete", getpid());
        return true;
    }

    bool Manager::Object::sessionRunZenity(const XvfbSession* xvfb, std::initializer_list<std::string> params)
    {
        std::filesystem::path zenity = _config->getString("zenity:path", "/usr/bin/zenity");

        return 0 != runSessionCommandSafe(xvfb, zenity, std::move(params));
    }

    bool Manager::Object::busSendMessage(const int32_t & display, const std::string & message)
    {
        Application::info("%s: display: %d, message: `%s'", __FUNCTION__, display, message.c_str());

        if(auto xvfb = getXvfbInfo(display))
        {
            if(xvfb->mode == XvfbMode::SessionLogin)
            {
                Application::error("%s: login session skipped, display: %d", __FUNCTION__, display);
                return false;
            }

            if(! xvfb->pamh)
            {
                Application::error("%s: unknown session, display: %d", __FUNCTION__, display);
                return false;
            }

            // new mode: zenity info
            if(sessionRunZenity(xvfb, { "--info", "--no-wrap", "--text", quotedString(message) }))
                return true;

            // compat mode: create spool file only
            auto home = Manager::getUserHome(xvfb->user);
            auto dtn = std::chrono::system_clock::now().time_since_epoch();
            auto file = std::filesystem::path(home) / ".ltsm" / "messages" / std::to_string(dtn.count());
            auto dir = file.parent_path();

            if(! std::filesystem::is_directory(dir))
            {
                if(! std::filesystem::create_directory(dir))
                {
                    Application::error("%s: %s failed, path: %s, error: %s, code: %d", __FUNCTION__, "mkdir", dir.c_str(), strerror(errno), errno);
                    return false;
                }
            }

            // fixed permissions 0750
            std::filesystem::permissions(dir, std::filesystem::perms::group_write |
                                         std::filesystem::perms::others_all, std::filesystem::perm_options::remove);
            setFileOwner(dir, xvfb->uid, xvfb->gid);
            std::ofstream ofs(file, std::ofstream::trunc);

            if(ofs.good())
            {
                ofs << message;
                ofs.close();
                setFileOwner(file, xvfb->uid, xvfb->gid);
                return true;
            }
            else
            {
                Application::error("%s: can't create file: %s", __FUNCTION__, file.c_str());
            }
        }

        return false;
    }

    bool Manager::Object::busIdleTimeoutAction(const int32_t& display)
    {
        Application::info("%s: display: %d", __FUNCTION__, display);

        if(auto xvfb = getXvfbInfo(display))
        {
/*
            _config->getString("idle:action:path");
            _config->getStdList<std::string>("idle:action:args");

            try
            {
                std::scoped_lock<std::mutex> guard(_lockRunning);
                PidStatus pidStatus = RunAs::sessionCommand(xvfb, cmd, std::move(params));
            }
            catch(const std::system_error &)
            {
                Application::error("%s: failed, check thread limit", __FUNCTION__);
            }
            catch(const std::exception & err)
            {
                Application::error("%s: exception: %s", __FUNCTION__, err.what());
            }
*/
            return true;
        }

        return false;
    }

    bool Manager::Object::busConnectorAlive(const int32_t & display)
    {
        std::thread([=]()
        {
            auto it = _xvfb->find(display);

            if(it != _xvfb->end())(*it).second.checkconn = false;
        }).detach();
        return true;
    }

    bool Manager::Object::busSetLoginsDisable(const bool & action)
    {
        _loginsDisable = action;
        return true;
    }

    bool Manager::Object::busConnectorTerminated(const int32_t & display)
    {
        Application::info("%s: display: %d", __FUNCTION__, display);

        if(auto xvfb = getXvfbInfo(display))
        {
            if(xvfb->mode == XvfbMode::SessionLogin)
                displayShutdown(display, false, xvfb);
            else if(xvfb->mode == XvfbMode::SessionOnline)
            {
                xvfb->mode = XvfbMode::SessionSleep;
                xvfb->remoteaddr.clear();
                xvfb->conntype.clear();
                xvfb->encryption.clear();
                createSessionConnInfo(getUserHome(xvfb->user), nullptr);
                emitSessionChanged(display);
            }
        }

        return true;
    }

    bool Manager::Object::busConnectorSwitched(const int32_t & oldDisplay, const int32_t & newDisplay)
    {
        Application::info("%s: old display: %d, new display: %d", __FUNCTION__, oldDisplay, newDisplay);
        displayShutdown(oldDisplay, false, nullptr);
        return true;
    }

    bool Manager::Object::busTransferFilesRequest(const int32_t& display, const std::vector<sdbus::Struct<std::string, uint32_t>>& files)
    {
        Application::info("%s: display: %d, count: %d", __FUNCTION__, display, files.size());

        auto xvfb = getXvfbInfo(display);
        if(! xvfb)
        {
            Application::error("%s: display not found: %d", __FUNCTION__, display);
            return false;
        }

        if(! xvfb->allowTransfer)
        {
            Application::error("%s: display %d, transfer reject", __FUNCTION__, display);

            busSendNotify(display, "Transfer Restricted", "transfer is blocked, contact the administrator",
                            NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
            return false;
        }

        if(_config->hasKey("transfer:group:only"))
        {
            auto members = Manager::getGroupMembers(_config->getString("transfer:group:only"));
            if(std::none_of(members.begin(), members.end(), [&](auto & user) { return user == xvfb->user; }))
            {
                Application::error("%s: display %d, transfer reject", __FUNCTION__, display);

                busSendNotify(display, "Transfer Restricted", "transfer is blocked, contact the administrator",
                                NotifyParams::IconType::Error, NotifyParams::UrgencyLevel::Normal);
                return false;
            }
        }

        std::filesystem::path zenity = this->_config->getString("zenity:path", "/usr/bin/zenity");
        auto msg = std::string("Can you receive remote files?(").append(std::to_string(files.size())).append(")");
        std::future<int> zenityQuestionResult;

        auto emitTransferReject = [this, display](const std::vector<sdbus::Struct<std::string, uint32_t>>& files)
        {
            for(auto & info : files)
                this->emitTransferAllow(display, std::get<0>(info), "", "");
        };

        try
        {
            auto pidStatus = RunAs::sessionCommand(xvfb, zenity, { "--question", "--default-cancel", "--text", msg });
            zenityQuestionResult = std::move(pidStatus.second);
        }
        catch(const std::system_error &)
        {
            Application::error("%s: failed, check thread limit", __FUNCTION__);
            emitTransferReject(files);
            return false;
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
            emitTransferReject(files);
            return false;
        }

        auto xvfbHome = getUserHome(_config->getString("user:xvfb"));

        //run background
        std::thread([this, zenity, display, files, xvfbHome,
            emitTransferReject = std::move(emitTransferReject), zenityQuestionResult = std::move(zenityQuestionResult)]() mutable
        {
            // wait zenity question
            zenityQuestionResult.wait();
            int status = zenityQuestionResult.get();

            // yes = 0, no: 256
            if(status == 256)
            {
                emitTransferReject(files);
                return;
            }

            // zenity select directory
            std::future<StatusStdout> zenitySelectDirectoryResult;
            bool error = false;

            try
            {
                auto pair = RunAs::sessionCommandStdout(this->getXvfbInfo(display), zenity,
                                { "--file-selection", "--directory", "--title", "Select directory", "--width", "640", "--height", "480" });
                zenitySelectDirectoryResult = std::move(pair.second);
            }
            catch(const std::system_error &)
            {
                Application::error("%s: failed, check thread limit", "RunZenity");
                emitTransferReject(files);
                return;
            }
            catch(const std::exception & err)
            {
                Application::error("%s: exception: %s", "RunZenity", err.what());
                emitTransferReject(files);
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
                emitTransferReject(files);
                return;
            }

            // get dstdir
            auto & buf = ret.second;
            auto end = buf.back() == 0x0a ? std::prev(buf.end()) : buf.end();
            std::filesystem::path dstdir(std::string(buf.begin(), end));

            if(! std::filesystem::is_directory(dstdir))
            {
                Application::error("%s: path not found: `%s', display: %d", "RunZenity", dstdir.c_str(), display);
                emitTransferReject(files);
                return;
            }

            std::scoped_lock<std::mutex> guard(_lockTransfer);
            for(auto & info : files)
            {
                auto tmpname = std::filesystem::path(xvfbHome) / std::string("transfer_").append( Tools::randomHexString(8));
                Application::debug("%s: transfer file request, display: %d, select dir: `%s', tmp name: `%s'", "RunZenity", display, dstdir.c_str(), tmpname.c_str());

                auto filepath = std::get<0>(info);
                auto filesize = std::get<1>(info);

                // check disk space limited
                //size_t ftotal = std::accumulate(files.begin(), files.end(), 0, [](size_t sum, auto & val){ return sum += std::get<1>(val); });
                auto spaceInfo = std::filesystem::space(dstdir);
                if(spaceInfo.available < filesize)
                {
                    busSendNotify(display, "Transfer Rejected", "not enough disk space",
                                            NotifyParams::Error, NotifyParams::UrgencyLevel::Normal);
                    break;
                }

                // check dstdir writeable / filename present
                auto filename = std::filesystem::path(filepath).filename();
                auto dstfile = std::filesystem::path(dstdir) / filename;

                if(std::filesystem::exists(dstfile))
                {
                    Application::error("%s: file present and skipping, display: %d, dst file: `%s'", "RunZenity", display, dstfile.c_str());

                    busSendNotify(display, "Transfer Skipping", Tools::StringFormat("such a file exists: %1").arg(dstfile.c_str()),
                                            NotifyParams::Warning, NotifyParams::UrgencyLevel::Normal);
                    continue;
                }

                _allowTransfer.emplace_back(filepath);
                emitTransferAllow(display, filepath, tmpname, dstdir);
            }
        }).detach();

        return true;
    }

    bool Manager::Object::busTransferFileStarted(const int32_t& display, const std::string& tmpfile, const uint32_t& filesz, const std::string& dstfile)
    {
        Application::debug("%s: display: %d, tmp file: `%s', dst file: `%s'", __FUNCTION__, display, tmpfile.c_str(), dstfile.c_str());

        if(auto xvfb = getXvfbInfo(display))
        {
            std::thread([this, xvfb, display, tmpfile, dstfile, filesz]
            {
                bool error = false;

                while(!error)
                {
                    if(std::filesystem::exists(tmpfile) &&
                        std::filesystem::file_size(tmpfile) >= filesz)
                        break;

                    // FIXME create progress informer session

                    if(xvfb->mode != XvfbMode::SessionOnline)
                    {
                        this->busSendNotify(display, "Transfer Error", Tools::StringFormat("transfer connection is lost"),
                                            NotifyParams::Error, NotifyParams::UrgencyLevel::Normal);
                        error = true;
                        continue;
                    }

                    std::this_thread::sleep_for(350ms);
                }

                if(! error)
                {
                    try
                    {
                        // move tmpfile to dstfile
                        std::filesystem::rename(tmpfile, dstfile);
                    }
                    catch(std::exception & err)
                    {
                        Application::error("%s: exception: %s", __FUNCTION__, err.what());
                    }

                    uid_t uid; gid_t gid;

                    std::tie(uid, gid, std::ignore, std::ignore) = getUserInfo(xvfb->user);
                    setFileOwner(dstfile, uid, gid);

                    this->busSendNotify(display, "Transfer Complete",
                        Tools::StringFormat("new file added: <a href=\"file://%1\">%2</a>").arg(dstfile).arg(std::filesystem::path(dstfile).filename().c_str()),
                                            NotifyParams::Information, NotifyParams::UrgencyLevel::Normal);
                }
            }).detach();
        }

        std::scoped_lock<std::mutex> guard(_lockTransfer);
        _allowTransfer.remove(tmpfile);

        return true;
    }

    bool Manager::Object::busSendNotify(const int32_t& display, const std::string& summary, const std::string& body, const uint8_t& icontype, const uint8_t& urgency)
    {
#ifdef SDBUS_ADDRESS_SUPPORT
        // urgency:  NotifyParams::UrgencyLevel { Low, Normal, Critical }
        // icontype: NotifyParams::IconType { Information, Warning, Error, Question }

        if(auto xvfb = getXvfbInfo(display))
        {
            if(xvfb->mode == XvfbMode::SessionLogin)
            {
                Application::error("%s: login session skipped, display: %d", __FUNCTION__, display);
                return false;
            }

            if(! xvfb->pamh)
            {
                Application::error("%s: unknown session, display: %d", __FUNCTION__, display);
                return false;
            }

            pid_t pid = fork();

            if(pid < 0)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fork", strerror(errno), errno);
                throw service_error(NS_FuncName);
            }

            if(0 < pid)
            {
                waitPidBackgroundSafe(pid);

                // main thread return
                return true;
            }

            // child mode
            long fdlimit = sysconf(_SC_OPEN_MAX);
            for(int fd = STDERR_FILENO + 1; fd < fdlimit; fd++)
                close(fd);

            Application::info("%s: notification child pid: %d, summary: %s", __FUNCTION__, getpid(), summary.c_str());
            std::string notificationIcon("dialog-information");

            switch(icontype)
            {
                //case NotifyParams::IconType::Information:
                case NotifyParams::IconType::Warning:   notificationIcon.assign("dialog-error"); break;
                case NotifyParams::IconType::Error:     notificationIcon.assign("dialog-warning"); break;
                case NotifyParams::IconType::Question:  notificationIcon.assign("dialog-question"); break;
                default: break;
            }

            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);

            auto dbusAddresses = Manager::getSessionDbusAddresses(xvfb->user);
     
            if(Manager::switchToUser(xvfb->user) && ! dbusAddresses.empty())
            {
                auto conn = sdbus::createSessionBusConnectionWithAddress(Tools::join(dbusAddresses, ";"));

                auto destinationName = "org.freedesktop.Notifications";
                auto objectPath = "/org/freedesktop/Notifications";
                auto concatenatorProxy = sdbus::createProxy(std::move(conn), destinationName, objectPath);

                auto interfaceName = "org.freedesktop.Notifications";
                auto method = concatenatorProxy->createMethodCall(interfaceName, "Notify");

                std::string applicationName("LTSM");
                uint32_t replacesID = 0;

                std::vector<std::string> actions;
                std::map<std::string, sdbus::Variant> hints;
                int32_t expirationTimeout = -1;

                method << applicationName << replacesID << notificationIcon << summary <<
                    body << actions << hints << expirationTimeout;

                try
                {
                    auto reply = concatenatorProxy->callMethod(method);
                }
                catch(const sdbus::Error & err)
                {
                    Application::error("%s: failed, display: %d, sdbus error: %s, msg: %s", __FUNCTION__, display, err.getName().c_str(), err.getMessage().c_str());
                }
                catch(std::exception & err)
                {
                    Application::error("%s: exception: %s", __FUNCTION__, err.what());
                }

                // wait and exit
                std::this_thread::sleep_for(300ms);
                execl("/bin/true", "/bin/true", nullptr);
            }

            // child exit
            std::exit(0);
        }
#endif
        return false;
    }

    bool Manager::Object::helperWidgetStartedAction(const int32_t & display)
    {
        Application::info("%s: display: %d", __FUNCTION__, display);
        emitHelperWidgetStarted(display);
        return true;
    }

    bool Manager::Object::helperIdleTimeoutAction(const int32_t & display)
    {
        Application::info("%s: display: %d", __FUNCTION__, display);
        displayShutdown(display, true, nullptr);
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

    int32_t Manager::Object::helperGetIdleTimeoutSec(const int32_t & display)
    {
        int helperIdleTimeout = _config->getInteger("helper:idletimeout", 0);
        if(0 > helperIdleTimeout) helperIdleTimeout = 0;

        return helperIdleTimeout;
    }

    bool Manager::Object::helperIsAutoComplete(const int32_t & display)
    {
        return _config->getBoolean("helper:autocomplete", false);
    }

    std::list<std::string> Manager::Object::getAllowLogins(void) const
    {
        // filtered uids: "access:uid:min", "access:uid:max"
        auto systemLogins = getSystemUsersRange(_config->getInteger("access:uid:min", 0), _config->getInteger("access:uid:max", 0));

        // filtered access list: "access:users"
        auto allowLogins = _config->getStdList<std::string>("access:users");
        if(! allowLogins.empty())
        {
            auto itend = std::remove_if(allowLogins.begin(), allowLogins.end(),
                                    [](auto & str){ return str.empty(); });
            allowLogins.erase(itend, allowLogins.end());

            if(! allowLogins.empty())
            {
                allowLogins.sort();
                allowLogins.unique();

                systemLogins.sort();
                std::list<std::string> res;

                res.resize(std::min(systemLogins.size(), allowLogins.size()));
                auto it = std::set_intersection(systemLogins.begin(), systemLogins.end(), allowLogins.begin(), allowLogins.end(), res.begin());
                res.erase(it, res.end());
                res.swap(systemLogins);
            }
        }

        // filtered access group: "access:group"
        if(_config->hasKey("access:group"))
        {
            auto groupLogins = getGroupMembers(_config->getString("access:group"));
            if(! groupLogins.empty())
            {
                groupLogins.sort();
                groupLogins.unique();

                systemLogins.sort();
                std::list<std::string> res;

                res.resize(std::min(systemLogins.size(), groupLogins.size()));
                auto it = std::set_intersection(systemLogins.begin(), systemLogins.end(), groupLogins.begin(), groupLogins.end(), res.begin());
                res.erase(it, res.end());
                res.swap(systemLogins);
            }
        }

        return systemLogins;
    }

    std::vector<std::string> Manager::Object::helperGetUsersList(const int32_t & display)
    {
        auto allowLogins = getAllowLogins();
        return std::vector<std::string>(allowLogins.begin(), allowLogins.end());
    }

    bool Manager::Object::busSetAuthenticateInfo(const int32_t & display, const std::string & login, const std::string & password)
    {
        std::thread([=]()
        {
            this->pamAuthenticate(display, login, password);
        }).detach();
        return true;
    }

    int pam_conv_handle(int num_msg, const struct pam_message** msg, struct pam_response** resp, void* appdata_ptr)
    {
        if(! appdata_ptr)
        {
            Application::error("%s: pam error: %s",__FUNCTION__, "empty data");
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

            if(! *resp) return PAM_BUF_ERR;
        }

        auto pair = static_cast< std::pair<std::string, std::string>* >(appdata_ptr);

        for(int ii = 0 ; ii < num_msg; ++ii)
        {
            auto pm = msg[ii];
            auto pr = resp[ii];

            switch(pm->msg_style)
            {
                case PAM_ERROR_MSG:
                    Application::error("%s: pam error: %s", __FUNCTION__, pm->msg);
                    break;

                case PAM_TEXT_INFO:
                    Application::info("%s: pam info: %s", __FUNCTION__, pm->msg);
                    break;

                case PAM_PROMPT_ECHO_ON:
                    pr->resp = strdup(pair->first.c_str());

                    if(! pr->resp) return PAM_BUF_ERR;

                    break;

                case PAM_PROMPT_ECHO_OFF:
                    pr->resp = strdup(pair->second.c_str());

                    if(! pr->resp) return PAM_BUF_ERR;

                    break;

                default:
                    break;
            }
        }

        //
        return PAM_SUCCESS;
    }

    bool Manager::Object::pamAuthenticate(const int32_t & display, const std::string & login, const std::string & password)
    {
        Application::info("%s: display: %d, username: %s", __FUNCTION__, display, login.c_str());
        std::string pamService = _config->getString("pam:service");

        auto users = getAllowLogins();
        if(users.empty())
        {
            Application::error("%s: username not found: %s, display: %d", __FUNCTION__, login.c_str(), display);
            emitLoginFailure(display, "login disabled");
            return false;
        }

        if(std::none_of(users.begin(), users.end(), [&](auto & val){ return val == login; }))
        {
            Application::error("%s: username not found: %s, display: %d", __FUNCTION__, login.c_str(), display);
            emitLoginFailure(display, "login not found");
            return false;
        }

        if(_loginsDisable)
        {
            Application::info("%s: logins disabled, username: %s, display: %d", __FUNCTION__, login.c_str(), display);
            emitLoginFailure(display, "logins disabled by the administrator");
            return false;
        }

        if(auto xvfbLogin = getXvfbInfo(display))
        {
            int loginFailuresConf = _config->getInteger("login:failures_count", 0);
            if(0 > loginFailuresConf) loginFailuresConf = 0;

            // close prev session
            if(xvfbLogin->pamh)
            {
                pam_end(xvfbLogin->pamh, PAM_SUCCESS);
                xvfbLogin->pamh = nullptr;
            }

            std::pair<std::string, std::string> pamv = std::make_pair(login, password);
            const struct pam_conv pamc = { pam_conv_handle, & pamv };
            int ret = 0;
            ret = pam_start(pamService.c_str(), login.c_str(), & pamc, & xvfbLogin->pamh);

            if(PAM_SUCCESS != ret || ! xvfbLogin->pamh)
            {
                Application::error("%s: %s failed, code: %d", __FUNCTION__,  "pam_start", ret);
                emitLoginFailure(display, "pam error");
                return false;
            }

            // check user
            ret = pam_authenticate(xvfbLogin->pamh, 0);

            if(PAM_SUCCESS != ret)
            {
                const char* err = pam_strerror(xvfbLogin->pamh, ret);
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_authenticate", err, ret);
                emitLoginFailure(display, err);
                xvfbLogin->loginFailures += 1;

                if(loginFailuresConf < xvfbLogin->loginFailures)
                {
                    Application::error("%s: login failures limit, display: %d", __FUNCTION__, display);
                    emitLoginFailure(display, "failures limit");
                    displayShutdown(display, true, xvfbLogin);
                }

                return false;
            }

            // auth success
            if(0 < loginFailuresConf)
                xvfbLogin->loginFailures = 0;

            // check connection policy
            int userScreen;
            XvfbSession* userSess;
            std::tie(userScreen, userSess) = findUserSession(login);

            if(0 < userScreen && userSess)
            {
                if(userSess->mode == XvfbMode::SessionOnline)
                {
                    if(userSess->policy == SessionPolicy::AuthLock)
                    {
                        Application::error("%s: session busy, policy: %s, user: %s, session display: %d, from: %s, display: %d", __FUNCTION__, "authlock", login.c_str(), userScreen, userSess->remoteaddr.c_str(), display);
                        // informer login display
                        emitLoginFailure(display, std::string("session busy, from: ").append(userSess->remoteaddr));
                        return false;
                    }
                    else if(userSess->policy == SessionPolicy::AuthTake)
                    {
                        // shutdown prev connect
                        emitShutdownConnector(userScreen);
                        // wait session
                        Tools::waitCallable<std::chrono::milliseconds>(1000, 50, [=]()
                        {
                            return userSess->mode != XvfbMode::SessionSleep;
                        });
                    }
                }
            }

            ret = pam_acct_mgmt(xvfbLogin->pamh, 0);
            if(ret != PAM_SUCCESS)
            {
                const char* err = pam_strerror(xvfbLogin->pamh, ret);
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_acct_mgmt", err, ret);
                emitLoginFailure(display, err);
                return false;
            }

            if(ret == PAM_NEW_AUTHTOK_REQD)
            {
                ret = pam_chauthtok(xvfbLogin->pamh, PAM_CHANGE_EXPIRED_AUTHTOK);
                if(PAM_SUCCESS != ret)
                {
                    const char* err = pam_strerror(xvfbLogin->pamh, ret);
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pam_chauthtok", err, ret);
                    emitLoginFailure(display, err);
                    return false;
                }
            }

            emitLoginSuccess(display, login);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetSessionKeyboardLayouts(const int32_t& display, const std::vector<std::string>& layouts)
    {
        if(auto xvfb = getXvfbInfo(display))
        {
            Application::info("%s: display: %d, layouts: [%s]", __FUNCTION__, display, Tools::join(layouts, ",").c_str());

            if(layouts.empty())
                return false;

            std::ostringstream os;
            for(auto it = layouts.begin(); it != layouts.end(); ++it)
            {
                auto id = Tools::lower((*it).substr(0, 2));
                if(id == "en") id = "us";
                os << id;
                if(std::next(it) != layouts.end())
                    os << ",";
            }

            runSessionCommandSafe(xvfb, "/usr/bin/setxkbmap", { "-layout", quotedString(os.str()), "-option", "\"\"" });
            return true;
        }
        return false;
    }

    bool Manager::Object::busSetSessionEnvironments(const int32_t & display, const std::map<std::string, std::string>& map)
    {
        if(auto xvfb = getXvfbInfo(display))
        {
            xvfb->environments.clear();

            for(auto & [key, val] : map)
            {
                Application::info("%s: %s = `%s'", __FUNCTION__, key.c_str(), val.c_str());
                xvfb->environments.emplace(key, val);

                if(key == "TZ")
                    emitHelperWidgetTimezone(display, val);
            }

            return true;
        }

        return false;
    }

    bool Manager::Object::busSetSessionOptions(const int32_t& display, const std::map<std::string, std::string>& map)
    {
        if(auto xvfb = getXvfbInfo(display))
        {
            xvfb->options.clear();

            for(auto & [key, val] : map)
                xvfb->options.emplace(key, val);

            return true;
        }

        return false;
    }

#ifdef LTSM_CHANNELS
    void Manager::Object::startSessionChannels(int display)
    {
        if(auto xvfb = getXvfbInfo(display))
        {
            auto printer = xvfb->options.find("printer");
            if(xvfb->options.end() != printer)
                startPrinterListener(display, *xvfb, printer->second);

            auto pulseaudio = xvfb->options.find("pulseaudio");
            if(xvfb->options.end() != pulseaudio)
                startPulseAudioListener(display, *xvfb, pulseaudio->second);
        }
    }

    bool Manager::Object::startPrinterListener(int display, const XvfbSession & xvfb, const std::string & clientUrl)
    {
        auto [ clientType, clientAddress ] = Channel::parseUrl(clientUrl);

        if(clientType == Channel::ConnectorType::Unknown)
        {
            Application::error("%s: %s, unknown client url: %s", __FUNCTION__, "printer", clientUrl.c_str());
            return false;
        }

        auto printerSocket = _config->getString("channel:printer:format", "/var/run/ltsm/cups/printer_%{user}");
        auto socketFolder = std::filesystem::path(printerSocket).parent_path();

        if(! std::filesystem::is_directory(socketFolder))
        {
            std::filesystem::create_directory(socketFolder);

            // fix mode 0750
            std::filesystem::permissions(socketFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                        std::filesystem::perm_options::remove);

            // fix owner xvfb
            setFileOwner(socketFolder, getUserUid(_config->getString("user:xvfb")), 0);
        }

        printerSocket = Tools::replace(printerSocket, "%{user}", xvfb.user);

        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, printerSocket);
        emitCreateListener(display, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::WriteOnly),
                                    serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadOnly));

        return true;
    }

    bool Manager::Object::startPulseAudioListener(int display, const XvfbSession & xvfb, const std::string & clientUrl)
    {
        auto [ clientType, clientAddress ] = Channel::parseUrl(clientUrl);

        if(clientType == Channel::ConnectorType::Unknown)
        {
            Application::error("%s: %s, unknown client url: %s", __FUNCTION__, "pulseaudio", clientUrl.c_str());
            return false;
        }

        auto pulseAudioSocket = _config->getString("channel:pulseaudio:format", "/var/run/ltsm/pulse/%{user}");
        auto socketFolder = std::filesystem::path(pulseAudioSocket).parent_path();

        if(! std::filesystem::is_directory(socketFolder))
        {
            std::filesystem::create_directory(socketFolder);

            // fix mode 0750
            std::filesystem::permissions(socketFolder, std::filesystem::perms::group_write | std::filesystem::perms::others_all,
                                        std::filesystem::perm_options::remove);

            // fix owner xvfb
            setFileOwner(socketFolder, getUserUid(_config->getString("user:xvfb")), 0);
        }

        pulseAudioSocket = Tools::replace(pulseAudioSocket, "%{user}", xvfb.user);

        auto serverUrl = Channel::createUrl(Channel::ConnectorType::Unix, pulseAudioSocket);
        emitCreateListener(display, clientUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite),
                                    serverUrl, Channel::Connector::modeString(Channel::ConnectorMode::ReadWrite));

        return true;
    }
#endif

    bool Manager::Object::busSetDebugLevel(const std::string & level)
    {
        Application::info("%s: level: %s", __FUNCTION__, level.c_str());
        Application::setDebugLevel(level);
        return true;
    }

    std::string Manager::Object::busEncryptionInfo(const int32_t & display)
    {
        if(auto xvfb = getXvfbInfo(display))
            return xvfb->encryption;

        return "none";
    }

    bool Manager::Object::busDisplayResized(const int32_t & display, const uint16_t & width, const uint16_t & height)
    {
        if(auto xvfb = getXvfbInfo(display))
        {
            xvfb->width = width;
            xvfb->height = height;
            emitHelperWidgetCentered(display);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetEncryptionInfo(const int32_t & display, const std::string & info)
    {
        Application::info("%s encryption: %s, display: %d", __FUNCTION__, info.c_str(), display);

        if(auto xvfb = getXvfbInfo(display))
        {
            xvfb->encryption = info;
            emitSessionChanged(display);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetSessionDurationSec(const int32_t & display, const uint32_t & duration)
    {
        Application::info("%s: duration: %d, display: %d", __FUNCTION__, duration, display);

        if(auto xvfb = getXvfbInfo(display))
        {
            xvfb->durationlimit = duration;
            emitClearRenderPrimitives(display);
            emitSessionChanged(display);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetSessionPolicy(const int32_t & display, const std::string & policy)
    {
        Application::info("%s: policy: %s, display: %d", __FUNCTION__, policy.c_str(), display);

        if(auto xvfb = getXvfbInfo(display))
        {
            if(Tools::lower(policy) == "authlock")
                xvfb->policy = SessionPolicy::AuthLock;
            else if(Tools::lower(policy) == "authtake")
                xvfb->policy = SessionPolicy::AuthTake;
            else if(Tools::lower(policy) == "authshare")
                xvfb->policy = SessionPolicy::AuthShare;
            else
                Application::error("%s: unknown policy: %s, display: %d", __FUNCTION__, policy.c_str(), display);

            emitSessionChanged(display);
            return true;
        }

        return false;
    }

    bool Manager::Object::helperSetSessionLoginPassword(const int32_t & display, const std::string & login, const std::string & password, const bool & action)
    {
        Application::info("%s: login: %s, display: %d", __FUNCTION__, login.c_str(), display);
        emitHelperSetLoginPassword(display, login, password, action);
        return true;
    }

    std::vector<xvfb2tuple> Manager::Object::busGetSessions(void)
    {
        return toSessionsList();
    }

    bool Manager::Object::busRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill)
    {
        emitAddRenderRect(display, rect, color, fill);
        return true;
    }

    bool Manager::Object::busRenderText(const int32_t & display, const std::string & text, const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color)
    {
        emitAddRenderText(display, text, pos, color);
        return true;
    }

    bool Manager::Object::busRenderClear(const int32_t & display)
    {
        emitClearRenderPrimitives(display);
        return true;
    }

    bool Manager::Object::busCreateChannel(const int32_t & display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode)
    {
        auto modes = { "ro", "rw", "wo" };

        if(std::none_of(modes.begin(), modes.end(), [&](auto & val){ return cmode == val; }))
        {
            Application::error("%s: incorrect %s mode: %s", __FUNCTION__, "client", cmode.c_str());
            return false;
        }

        if(std::none_of(modes.begin(), modes.end(), [&](auto & val){ return smode == val; }))
        {
            Application::error("%s: incorrect %s mode: %s", __FUNCTION__, "server", smode.c_str());
            return false;
        }

        emitCreateChannel(display, client, cmode, server, smode);
        return true;
    }

    bool Manager::Object::busDestroyChannel(const int32_t & display, const uint8_t & channel)
    {
        emitDestroyChannel(display, channel);
        return true;
    }

    /* Manager::Service */
    std::atomic<bool> Manager::Service::isRunning = false;
    std::unique_ptr<Manager::Object> Manager::Service::objAdaptor;

    Manager::Service::Service(int argc, const char** argv)
        : ApplicationJsonConfig("ltsm_service", argc, argv)
    {
        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                std::cout << "usage: " << argv[0] << " --config <path> [--background]" << std::endl;
                throw 0;
            }

            if(0 == std::strcmp(argv[it], "--background"))
            {
                isBackground = true;
            }
        }

        // check present executable files
        for(auto key : _config.keys())
        {
            if(5 < key.size() && 0 == key.substr(key.size() - 5).compare(":path") && 0 != std::isalpha(key.front()) /* skip comment */)
            {
                auto value = _config.getString(key);

                if(! std::filesystem::exists(value))
                {
                    Application::error("%s: path not found: `%s'", "CheckProgram", value.c_str());
                    throw 1;
                }
            }
        }
    }

    bool Manager::Service::createXauthDir(void)
    {
        auto xauthFile = _config.getString("xauth:file");
        auto groupAuth = _config.getString("group:auth");
        // find group id
        gid_t setgid = getGroupGid(groupAuth);

        // check directory
        auto folderPath = std::filesystem::path(xauthFile).parent_path();

        if(! folderPath.empty())
        {
            // create
            if(! std::filesystem::is_directory(folderPath))
            {
                if(! std::filesystem::create_directory(folderPath))
                {
                    Application::error("%s: %s failed, path: `%s', error: %s, code: %d", __FUNCTION__, "mkdir", folderPath.c_str(), strerror(errno), errno);
                    return false;
                }
            }

            // fix mode 0755
            std::filesystem::permissions(folderPath, std::filesystem::perms::owner_all |
                                         std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_read | std::filesystem::perms::others_exec, std::filesystem::perm_options::replace);
            setFileOwner(folderPath, 0, setgid);
            return true;
        }

        return false;
    }

    bool Manager::Service::inotifyWatchConfigStart(void)
    {
        std::string filename = _config.getString("config:path", "/etc/ltsm/config.json");

        int fd = inotify_init();
        if(0 > fd)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "inotify_nit", strerror(errno), errno);
            return false;
        }

        int wd = inotify_add_watch(fd, filename.c_str(), IN_CLOSE_WRITE);
        if(0 > wd)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "inotify_add_watch", strerror(errno), errno);
            return false;
        }

        Application::info("%s: path: `%s'", __FUNCTION__, filename.c_str());

        if(0 > fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK))
        {
            Application::error("%s: fcntl failed, error: %s, code: %d", __FUNCTION__, strerror(errno), errno);
            return false;
        }

        timerInotifyWatchConfig = Tools::BaseTimer::create<std::chrono::seconds>(3, true, [fd1 = fd, jconfig = & _config]()
        {
            // read single inotify_event (16byte)
            const int bufsz = sizeof(struct inotify_event);
            char buf[bufsz];

            auto len = read(fd1, buf, sizeof(buf));
            if(0 < len)
            {
                auto filename = jconfig->getString("config:path", "/etc/ltsm/config.json");
                JsonContentFile jsonFile(filename);

                if(! jsonFile.isValid() || ! jsonFile.isObject())
                {
                    Application::error("%s: reload config %s, file: %s", "InotifyWatch", "failed", filename.c_str());
                }
                else
                {
                    auto jo = jsonFile.toObject();
                    const_cast<JsonObject*>(jconfig)->swap(jo);

                    Application::info("%s: reload config %s, file: %s", "InotifyWatch", "success", filename.c_str());
                    Application::setDebugLevel(jconfig->getString("service:debug"));
                }
            }
        });

        return true;
    }

    int Manager::Service::start(void)
    {
        if(isBackground && fork())
            return 0;

        if(0 < getuid())
        {
            std::cerr << "need root privileges" << std::endl;
            return EXIT_FAILURE;
        }

        auto conn = sdbus::createSystemBusConnection(LTSM::dbus_service_name);
        if(! conn)
        {
            Application::error("%s: dbus connection failed", "ServiceStart");
            return EXIT_FAILURE;
        }

        auto xvfbHome = getUserHome(_config.getString("user:xvfb"));

        if(! std::filesystem::is_directory(xvfbHome))
        {
            Application::error("%s: path not found: `%s'", "ServiceStart", xvfbHome.c_str());
            return EXIT_FAILURE;
        }

        // remove old sockets
        for(auto const & dirEntry : std::filesystem::directory_iterator{xvfbHome})
            if(dirEntry.is_socket()) std::filesystem::remove(dirEntry);

        signal(SIGTERM, signalHandler);
        //signal(SIGCHLD, signalHandler);
        signal(SIGINT,  isBackground ? SIG_IGN : signalHandler);
        signal(SIGHUP,  SIG_IGN);

        createXauthDir();
        objAdaptor.reset(new Manager::Object(*conn, _config, *this));

        inotifyWatchConfigStart();

        isRunning = true;
        Application::setDebugLevel(_config.getString("service:debug"));
        Application::info("%s: runtime version: %d", "ServiceStart", LTSM::service_version);

        while(isRunning && objAdaptor->isRunning())
        {
            conn->enterEventLoopAsync();
            std::this_thread::sleep_for(1ms);
        }

        timerInotifyWatchConfig->stop();

        // service stopped from signal
        if(objAdaptor->isRunning())
        {
            objAdaptor->shutdown();
            std::this_thread::sleep_for(60ms);
        }

        objAdaptor.reset();
        conn->enterEventLoopAsync();

        return EXIT_SUCCESS;
    }

    void Manager::Service::signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
            isRunning = false;
    }
}

int main(int argc, const char** argv)
{
    LTSM::Application::setDebugLevel(LTSM::DebugLevel::SyslogInfo);

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
        LTSM::Application::error("local exception: %s", err.what());
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
