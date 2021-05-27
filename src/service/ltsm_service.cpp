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
#include <unistd.h>
#include <string.h>

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <atomic>
#include <thread>
#include <cstring>
#include <numeric>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_service.h"
#include "ltsm_xcb_wrapper.h"

using namespace std::chrono_literals;

namespace LTSM
{
    void XvfbSession::destroy(int killedpid)
    {
        if(0 < pid2 && (0 == killedpid || pid1 == killedpid))
        {
            int status;
            // kill session
            Application::debug("kill helper, pid: %d", pid2);
            kill(pid2, SIGTERM);
            waitpid(pid2, &status, 0);
            pid2 = 0;
        }

        if(0 < pid1 && (0 == killedpid || pid2 == killedpid))
        {
            int status;
            // kill xvfb
            Application::debug("kill xvfb pid: %d", pid1);
            kill(pid1, SIGTERM);
            waitpid(pid1, &status, 0);
            pid1 = 0;
        }

        if(xauthfile.size())
        {
            // remove xautfile
            std::filesystem::remove(xauthfile);
            xauthfile.clear();
        }

        if(mcookie.size())
            mcookie.clear();
    }

    /* XvfbSessions */
    XvfbSessions::~XvfbSessions()
    {
        for(auto & pair : _xvfb)
            pair.second.destroy();
    }

    int XvfbSessions::findUserSession(const std::string & username)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        auto it = std::find_if(_xvfb.begin(), _xvfb.end(), [&](auto & pair)
        {
            return (pair.second.mode == XvfbMode::SessionOnline || pair.second.mode == XvfbMode::SessionSleep) &&
		pair.second.user == username;
        });
        return it != _xvfb.end() ? (*it).first : -1;
    }

    XvfbSession* XvfbSessions::getXvfbInfo(int screen)
    {
	if(0 < screen)
	{
    	    const std::lock_guard<std::mutex> lock(_mutex);
    	    auto it = _xvfb.find(screen);
    	    if(it != _xvfb.end()) return & (*it).second;
	}
	return nullptr;
    }

    int XvfbSessions::findXvfbPIDs(int pid)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        auto it = std::find_if(_xvfb.begin(), _xvfb.end(), [&](auto & pair)
        {
            return pair.second.pid1 == pid || pair.second.pid2 == pid;
        });
        return it != _xvfb.end() ? (*it).first : -1;
    }

    void XvfbSessions::removeXvfbDisplay(int screen)
    {
	if(0 < screen)
	{
    	    const std::lock_guard<std::mutex> lock(_mutex);
    	    auto it = _xvfb.find(screen);

    	    if(it != _xvfb.end())
    	    {
        	(*it).second.destroy();
        	_xvfb.erase(it);
    	    }
	}
    }

    XvfbSession* XvfbSessions::registryXvfbSession(int screen, const XvfbSession & st)
    {
	if(0 < screen)
	{
    	    const std::lock_guard<std::mutex> lock(_mutex);
    	    _xvfb[screen] = st;
    	    return & _xvfb[screen];
	}
	return nullptr;
    }

    void XvfbSessions::confirmCheckConnection(int screen)
    {
	if(0 < screen)
	{
    	    const std::lock_guard<std::mutex> lock(_mutex);
	    _xvfb[screen].checkconn = false;
	}
    }

    void XvfbSessions::unregistryChilds(int pid)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        auto it = std::find_if(_xvfb.begin(), _xvfb.end(), [=](auto & pair)
        {
            return pair.second.pid1 == pid || pair.second.pid2 == pid;
        });

        if(it != _xvfb.end())
        {
            // unregistered childs
            Application::debug("child unregistered, pid: %d", pid);
            (*it).second.destroy(pid);
            _xvfb.erase(it);
        }
    }

    std::vector<xvfb2tuple> XvfbSessions::toSessionsList(void)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        std::vector<xvfb2tuple> res;
        res.reserve(_xvfb.size());

        for(auto & pair : _xvfb)
        {
	    int32_t sesmode = 0; // SessionLogin
	    switch(pair.second.mode)
	    {
		case XvfbMode::SessionOnline: sesmode = 1; break;
		case XvfbMode::SessionSleep:  sesmode = 2; break;
		default: break;
	    }

	    int32_t conpol = 0; // AuthLock
	    switch(pair.second.policy)
	    {
		case SessionPolicy::AuthTake: conpol = 1; break;
		case SessionPolicy::AuthShare: conpol = 2; break;
		default: break;
	    }

            res.emplace_back(
                    pair.first,
                    pair.second.pid1,
                    pair.second.pid2,
                    pair.second.width,
                    pair.second.height,
                    pair.second.uid,
                    pair.second.gid,
                    pair.second.durationlimit,
                    sesmode,
                    conpol,
                    pair.second.user,
                    pair.second.xauthfile,
                    pair.second.remoteaddr,
                    pair.second.conntype
            );
        }

        return res;
    }

    /* Manager::Object */
    Manager::Object::Object(sdbus::IConnection & conn, const JsonObject & jo, const Application & app)
        : AdaptorInterfaces(conn, LTSM::dbus_object_path), _app(& app), _config(& jo), _sessionPolicy(SessionPolicy::AuthLock)
    {
        _loginFailuresConf = _config->getInteger("login:failures_count", 0);
        _helperIdleTimeout = _config->getInteger("helper:idletimeout", 0);
        _helperAutoComplete = _config->getBoolean("helper:autocomplete", false);
        _helperAutoCompeteMinUid = _config->getInteger("autocomplete:min", 0);
        _helperAutoCompeteMaxUid = _config->getInteger("autocomplete:max", 0);
        _helperAccessUsersList = _config->getStdVector<std::string>("helper:accesslist");
        _helperTitle = _config->getString("helper:title", "X11 Remote Desktop Service");
        _helperDateFormat = _config->getString("helper:dateformat");
        if(0 > _loginFailuresConf) _loginFailuresConf = 0;
        if(0 > _helperIdleTimeout) _helperIdleTimeout = 0;
        if(0 > _helperAutoCompeteMinUid) _helperAutoCompeteMinUid = 0;
        if(0 > _helperAutoCompeteMaxUid) _helperAutoCompeteMaxUid = 0;

	auto policy = _config->getString("session:policy");
	if(Tools::lower(policy) == "authtake")
	{
	    _sessionPolicy = SessionPolicy::AuthTake;
	}
	else
	if(Tools::lower(policy) == "authshare")
	{
	    _sessionPolicy = SessionPolicy::AuthShare;
	}

        auto itend = std::remove_if(_helperAccessUsersList.begin(), _helperAccessUsersList.end(), [](auto & str)
        {
            return str.empty();
        });
        _helperAccessUsersList.erase(itend, _helperAccessUsersList.end());
        registerAdaptor();
    }

    Manager::Object::~Object()
    {
        unregisterAdaptor();
    }

    void Manager::Object::openlog(void) const
    {
	_app->openlog();
    }

    void Manager::Object::systemTick(void)
    {
	// periodic tasks
	auto curtp = std::chrono::system_clock::now();

	// 3 sec
        if(std::chrono::seconds(3) < curtp - _tpsec3)
	{
    	    const std::lock_guard<std::mutex> lock(_mutex);

    	    for(auto & pair : _xvfb)
    	    {
		// find timepoint session limit
        	if(pair.second.mode != XvfbMode::SessionLogin && 0 < pair.second.durationlimit)
		{
		    // task background
		    std::thread([display = pair.first, tpstart = pair.second.tpstart, 
                        duration = pair.second.durationlimit, width = pair.second.width, curtp, this]()
		    {
			auto sessionAliveSec = std::chrono::duration_cast<std::chrono::seconds>(curtp - tpstart);
			auto lastsec = std::chrono::seconds(duration) - sessionAliveSec;

			// shutdown session
			if(std::chrono::seconds(duration) < sessionAliveSec)
			{
        		    Application::error("time point limit, display: %d", display);
        		    this->busShutdownDisplay(display);
        		}
			else
			// inform alert
                        if(std::chrono::seconds(100) > lastsec)
			{
			    this->emitClearRenderPrimitives(display);
        		    // send render rect
        		    const uint16_t fw = width;
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
			{
                            this->emitSendBellSignal(display);
                        }
		    }).detach();
		}
	    }

	    _tpsec3 = curtp;
	}

	// 30 sec
        if(std::chrono::seconds(30) < curtp - _tpsec30)
	{
    	    const std::lock_guard<std::mutex> lock(_mutex);

    	    for(auto & pair : _xvfb)
    	    {
		if(pair.second.mode == XvfbMode::SessionOnline)
		{
		    // check alive connectors
		    if(! pair.second.checkconn)
		    {
			pair.second.checkconn = true;
			emitPingConnector(pair.first);
		    }
		    else
		    // not reply
		    {
			pair.second.mode = XvfbMode::SessionSleep;
			pair.second.checkconn = false;
        		Application::error("connector not reply, display: %d", pair.first);
			// complete shutdown
			busConnectorTerminated(pair.first);
		    }
		}
	    }

	    _tpsec30 = curtp;
	}
    }

    bool Manager::Object::switchToUser(const std::string & user)
    {
        int uid, gid;
        std::string home, shell;
        std::tie(uid, gid, home, shell) = getUserInfo(user);

        if(uid < 0)
        {
            Application::error("system user not found: %s", user.c_str());
            return false;
        }

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

        if(0 != setgid(gid))
	{
            Application::error("setgid failed, gid: %d, error: %s", gid, strerror(errno));
            return false;
        }

        if(0 != setuid(uid))
        {
            Application::error("setuid failed, uid: %d, error: %s", uid, strerror(errno));
            return false;
        }

        Application::debug("switched to user: %s, uid: %d, gid: %d, groups: (%s)", user.c_str(), getuid(), getgid(), sgroups.c_str());
        setenv("USER", user.c_str(), 1);
        setenv("LOGNAME", user.c_str(), 1);
        setenv("HOME", home.c_str(), 1);
        setenv("SHELL", shell.c_str(), 1);
        setenv("TERM", "linux", 1);

        if(0 != chdir(home.c_str()))
            Application::error("chdir failed, dir: %s, error: %s", home.c_str(), strerror(errno));

        return true;
    }

    void Manager::Object::closefds(void) const
    {
        long fdlimit = sysconf(_SC_OPEN_MAX);
        for(int fd = STDERR_FILENO + 1; fd < fdlimit; fd++)
            close(fd);
    }

    bool Manager::Object::checkFileReadable(const std::string & file)
    {
        auto st = std::filesystem::status(file);
        return std::filesystem::file_type::not_found != st.type() &&
                (st.permissions() & std::filesystem::perms::owner_read) != std::filesystem::perms::none;
    }

    bool Manager::Object::checkXvfbSocket(int display)
    {
	if(0 < display)
        {
	    std::string socketPath = Tools::replace(_config->getString("xvfb:socket"), "%{display}", display);
    	    return Tools::checkUnixSocket(socketPath);
	}
	return false;
    }

    bool Manager::Object::checkXvfbLocking(int display)
    {
	if(0 < display)
        {
    	    std::string xvfbLock = Tools::replace(_config->getString("xvfb:lock"), "%{display}", display);
            return checkFileReadable(xvfbLock);
	}
	return false;
    }

    void Manager::Object::removeXvfbSocket(int display)
    {
	if(0 < display)
        {
    	    std::string socketFormat = _config->getString("xvfb:socket");
    	    std::string socketPath = Tools::replace(socketFormat, "%{display}", display);
    	    std::filesystem::remove(socketPath);
	}
    }

    int Manager::Object::getFreeDisplay(void)
    {
        int min = _config->getInteger("display:min", 55);
        int max = _config->getInteger("display:max", 99);
        if(max < min) std::swap(max, min);

        std::vector ranges(max - min, 0);
        std::iota(ranges.begin(), ranges.end(), min);

        auto it = std::find_if(ranges.begin(), ranges.end(), [&](auto display)
                        { return ! checkXvfbLocking(display) && ! checkXvfbSocket(display); });

        return it != ranges.end() ? *it : -1;
    }

    std::tuple<int, int, std::string, std::string> Manager::getUserInfo(const std::string & user)
    {
        if(user.size())
        {
            struct passwd* st = getpwnam(user.c_str());

            if(st)
                return std::make_tuple<int, int, std::string, std::string>(st->pw_uid, st->pw_gid, st->pw_dir, st->pw_shell);

            Application::error("getpwnam failed, user: %s, error: %s", user.c_str(), strerror(errno));
        }

        return std::make_tuple<int, int, std::string, std::string>(-1, -1, "", "");
    }

    int Manager::getGroupGid(const std::string & group)
    {
        if(group.size())
        {
            struct group* group_st = getgrnam(group.c_str());

            if(group_st)
                return group_st->gr_gid;

            Application::error("getgrnam failed, group: %s, error: %s", group.c_str(), strerror(errno));
        }

        return -1;
    }

    std::list<std::string> Manager::getGroupMembers(const std::string & group)
    {
        std::list<std::string> res;
        struct group* group_st = getgrnam(group.c_str());

        if(! group_st)
        {
            Application::error("getgrnam failed, group: %s, error: %s", group.c_str(), strerror(errno));
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

    void Manager::Object::displayShutdown(int display, bool emitSignal)
    {
        if(auto xvfb = getXvfbInfo(display))
        {
            xvfb->shutdown = true;

            if(emitSignal) emitShutdownConnector(display);

            // dbus no wait, remove background
            std::thread([=]()
            {
    	        std::string script = _config->getString("script:shutdown");
    	        std::string sysuser = _config->getString("user:xvfb");
                std::string user = xvfb->user;

                this->closeSystemSession(display, *xvfb);
                std::this_thread::sleep_for(300ms);
                this->removeXvfbDisplay(display);
		this->removeXvfbSocket(display);
                this->emitDisplayRemoved(display);

                if(! script.empty() && sysuser != user)
                {
                    script.append(" ").append(std::to_string(display)).append(" ").append(user);
                    int ret = std::system(script.c_str());
                    Application::debug("system cmd: `%s', return code: %d, display: %d", script.c_str(), ret, display);
                }
            }).detach();
        }
    }

    void Manager::Object::closeSystemSession(int display, XvfbSession & info)
    {
        Application::info("close system session, user: %s, display: %d", info.user.c_str(), display);

        // PAM close
        if(info.pamh)
        {
            pam_close_session(info.pamh, 0);
            pam_end(info.pamh, PAM_SUCCESS);
            info.pamh = nullptr;
	}

    	// unreg sessreg
    	std::string sessreg = _config->getString("sessreg:path");
    	if(! sessreg.empty())
    	{
    	    std::string args = _config->getString("sessreg:args2");
    	    args = Tools::replace(args, "%{display}", display);
    	    args = Tools::replace(args, "%{user}", info.user);
    	    sessreg.append(" ").append(args);
    	    int ret = std::system(sessreg.c_str());
    	    Application::debug("system cmd: `%s', return code: %d, display: %d", sessreg.c_str(), ret, display);
	}
    }

    bool Manager::Object::waitXvfbStarting(int display, uint32_t ms)
    {
	if(0 >= display)
	    return false;

    	const std::chrono::milliseconds waitms(ms);
    	auto tp = std::chrono::system_clock::now();

    	while(! checkXvfbSocket(display))
    	{
    	    std::this_thread::sleep_for(100ms);

    	    if(waitms < std::chrono::system_clock::now() - tp)
                return false;
    	}

    	return true;
    }

    std::string Manager::Object::createXauthFile(int display, const std::string & mcookie, const std::string & userName, const std::string & remoteAddr)
    {
        std::string xauthFile = _config->getString("xauth:file");
        std::string xauthBin = _config->getString("xauth:path");
        std::string xauthArgs = _config->getString("xauth:args");
        std::string groupAuth = _config->getString("group:auth");
        xauthFile = Tools::replace(xauthFile, "%{remoteaddr}", remoteAddr);
        xauthFile = Tools::replace(xauthFile, "%{display}", display);
        Application::debug("xauthfile path: %s", xauthFile.c_str());

        // create empty xauthFile
        std::ofstream tmp(xauthFile, std::ofstream::binary | std::ofstream::trunc);
        if(! tmp.good())
        {
            Application::error("can't create file: %s", xauthFile.c_str());
            tmp.close();
            return "";
        }

        // xauth registry
        xauthArgs = Tools::replace(xauthArgs, "%{authfile}", xauthFile);
        xauthArgs = Tools::replace(xauthArgs, "%{display}", display);
        xauthArgs = Tools::replace(xauthArgs, "%{mcookie}", mcookie);
        Application::debug("xauth args: %s", xauthArgs.c_str());
        xauthBin.append(" ").append(xauthArgs);

        int ret = std::system(xauthBin.c_str());
        Application::debug("system cmd: `%s', return code: %d, display: %d", xauthBin.c_str(), ret, display);

        // xauthfile pemission 440
        auto userInfo = getUserInfo(userName);
        std::filesystem::permissions(xauthFile, std::filesystem::perms::owner_read |
                    std::filesystem::perms::group_read, std::filesystem::perm_options::replace);
	setFileOwner(xauthFile, std::get<0>(userInfo), getGroupGid(groupAuth));

        return xauthFile;
    }

    std::string Manager::Object::createSessionConnInfo(const std::string & home, const XvfbSession* xvfb)
    {
        auto ltsmInfo = std::string(home).append("/.ltsm/conninfo");
        auto dir = Tools::dirname(ltsmInfo);

        if(! std::filesystem::is_directory(dir))
            std::filesystem::create_directory(dir);
        // set mode 750
        std::filesystem::permissions(dir, std::filesystem::perms::group_write |
                    std::filesystem::perms::others_all, std::filesystem::perm_options::remove);

        std::ofstream ofs(ltsmInfo, std::ofstream::trunc);
        if(ofs.good())
        {
            ofs << "LTSM_REMOTEADDR" << "=" << (xvfb ? xvfb->remoteaddr : "") << std::endl <<
                            "LTSM_TYPECONN" << "=" << (xvfb ? xvfb->conntype : "") << std::endl;
        }
        else
        {   
            Application::error("can't create file: %s", ltsmInfo.c_str());
        }
        ofs.close();

        return ltsmInfo;
    }

    void Manager::Object::setFileOwner(const std::string & file, int uid, int gid)
    {
        if(0 != chown(file.c_str(), uid, gid))
            Application::error("chown failed, file: %s, uid: %d, gid: %d, error: %s", file.c_str(), uid, gid, strerror(errno));
    }

    int Manager::Object::runXvfbDisplay(int display, int width, int height, const std::string & xauthFile, const std::string & userXvfb)
    {
        std::string xvfbBin = _config->getString("xvfb:path");
        std::string xvfbArgs = _config->getString("xvfb:args");
        // xvfb args
        xvfbArgs = Tools::replace(xvfbArgs, "%{display}", display);
        xvfbArgs = Tools::replace(xvfbArgs, "%{width}", width);
        xvfbArgs = Tools::replace(xvfbArgs, "%{height}", height);
        xvfbArgs = Tools::replace(xvfbArgs, "%{authfile}", xauthFile);
        Application::debug("xvfb args: %s", xvfbArgs.c_str());

	closelog();
        int pid = fork();
	openlog();

        if(0 == pid)
        {
            // child mode
            closefds();

            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);

            Application::debug("child exec, type: %s, uid: %d", "xvfb", getuid());

            if(switchToUser(userXvfb))
	    {
        	// create argv
        	std::list<std::string> list = Tools::split(xvfbArgs, 0x20);
        	std::vector<const char*> argv;
        	argv.reserve(list.size() + 2);
        	argv.push_back(xvfbBin.c_str());

        	for(auto & str : list)
            	    argv.push_back(str.c_str());

        	argv.push_back(nullptr);

                if(! checkFileReadable(xauthFile))
            	    Application::error("access failed, file: %s, user: %s, error: %s", xauthFile.c_str(), userXvfb.c_str(), strerror(errno));

        	int res = execv(xvfbBin.c_str(), (char* const*) argv.data());

        	if(res < 0)
            	    Application::error("execv failed: %s", strerror(errno));
	    }

	    closelog();
            _exit(0);
            // child end
        }

        return pid;
    }

    int Manager::Object::runLoginHelper(int display, const std::string & xauthFile, const std::string & userLogin)
    {
        std::string helperBin = _config->getString("helper:path");
        std::string helperArgs = _config->getString("helper:args");

        if(helperArgs.size())
        {
            helperArgs = Tools::replace(helperArgs, "%{display}", display);
            helperArgs = Tools::replace(helperArgs, "%{authfile}", xauthFile);
            Application::debug("helper args: %s", helperArgs.c_str());
        }

	closelog();
        int pid = fork();
	openlog();

        if(0 == pid)
        {
            // child mode
            closefds();

            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);

            Application::debug("child exec, type: %s, uid: %d", "helper", getuid());

            if(switchToUser(userLogin))
	    {
        	std::string strdisplay = std::string(":").append(std::to_string(display));
        	setenv("XAUTHORITY", xauthFile.c_str(), 1);
        	setenv("DISPLAY", strdisplay.c_str(), 1);

        	if(! checkFileReadable(xauthFile))
            	    Application::error("access failed, file: %s, user: %s, error: %s", xauthFile.c_str(), userLogin.c_str(), strerror(errno));

        	// create argv
        	int res = 0;

        	if(helperArgs.size())
        	{
        	    std::list<std::string> list = Tools::split(helperArgs, 0x20);
            	    std::vector<const char*> argv;
            	    argv.reserve(list.size() + 2);
            	    argv.push_back(helperBin.c_str());

            	    for(auto & str : list)
                	argv.push_back(str.c_str());

            	    argv.push_back(nullptr);
            	    res = execv(helperBin.c_str(), (char* const*) argv.data());
        	}
        	else
            	    res = execl(helperBin.c_str(), helperBin.c_str(), (char*) nullptr);

        	if(res < 0)
            	    Application::error("execv failed: %s", strerror(errno));
	    }

	    closelog();
            _exit(0);
            // child end
        }

        return pid;
    }

    int Manager::Object::runUserSession(int display, const std::string & sessionBin, const XvfbSession & xvfb)
    {
	closelog();
        int pid = fork();
	openlog();

	if(pid < 0)
	{
            pam_close_session(xvfb.pamh, 0);
            pam_end(xvfb.pamh, PAM_SYSTEM_ERR);
	}
	else
        if(0 == pid)
        {
    	    Application::info("run user session, pid: %d", getpid());

            int uid, gid;
            std::string home, shell;
	    std::tie(uid, gid, home, shell) = Manager::getUserInfo(xvfb.user);

	    if(uid == 0)
	    {
                Application::error("system error: %s", "root access deny");
    		pam_end(xvfb.pamh, PAM_SYSTEM_ERR);
        	_exit(0);
	    }

	    if(0 != initgroups(xvfb.user.c_str(), gid))
	    {
                Application::error("initgroups failed, user: %s, gid: %d, error: %s", xvfb.user.c_str(), gid, strerror(errno));
    		pam_end(xvfb.pamh, PAM_SYSTEM_ERR);
        	_exit(0);
	    }

	    // open session
	    int ret = pam_setcred(xvfb.pamh, PAM_ESTABLISH_CRED);
    	    if(ret != PAM_SUCCESS)
    	    {
		Application::error("pam_setcred failed, user: %s, display: %d, error: %s", xvfb.user.c_str(), display, pam_strerror(xvfb.pamh, ret));
    		pam_end(xvfb.pamh, ret);
        	_exit(0);
    	    }

    	    ret = pam_open_session(xvfb.pamh, 0);
    	    if(ret != PAM_SUCCESS)
    	    {
		Application::error("pam_open_session failed, user: %s, display: %d, error: %s", xvfb.user.c_str(), display, pam_strerror(xvfb.pamh, ret));
                ret = pam_setcred(xvfb.pamh, PAM_DELETE_CRED);
    		pam_end(xvfb.pamh, ret);
        	_exit(0);
    	    }

            ret = pam_setcred(xvfb.pamh, PAM_REINITIALIZE_CRED);
    	    if(ret != PAM_SUCCESS)
    	    {
		Application::error("pam_setcred failed, user: %s, display: %d, error: %s", xvfb.user.c_str(), display, pam_strerror(xvfb.pamh, ret));
                ret = pam_close_session(xvfb.pamh, 0);
    		pam_end(xvfb.pamh, ret);
        	_exit(0);
    	    }

            // child mode
            closefds();

            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);

            Application::debug("child exec, type: %s, uid: %d", "session", getuid());

            // assign groups
            if(switchToUser(xvfb.user))
	    {
        	std::string helperBin = _config->getString("helper:path");
        	std::string strdisplay = std::string(":").append(std::to_string(display));

        	setenv("XAUTHORITY", xvfb.xauthfile.c_str(), 1);
        	setenv("DISPLAY", strdisplay.c_str(), 1);

                createSessionConnInfo(home, & xvfb);

        	int res = execl(sessionBin.c_str(), sessionBin.c_str(), (char*) nullptr);

        	if(res < 0)
            	    Application::error("execl failed: %s", strerror(errno));
	    }

	    closelog();
            _exit(0);
            // child end
        }

        return pid;
    }

    int32_t Manager::Object::busStartLoginSession(const std::string & remoteAddr, const std::string & connType)
    {
        Application::info("login session request, remote: %s", remoteAddr.c_str());
        std::string userXvfb = _config->getString("user:xvfb");
        std::string groupAuth = _config->getString("group:auth");

        // get free screen
        int screen = getFreeDisplay();

        if(0 > screen)
        {
            Application::error("system error: %s", "all displays busy");
            return -1;
        }

        int uid, gid;
        std::tie(uid, gid, std::ignore, std::ignore) = getUserInfo(userXvfb);

        if(0 > uid)
        {
            Application::error("username not found, user: %s, uid: %d, gid: %d", userXvfb.c_str(), uid, gid);
            return -1;
        }

        int width = _config->getInteger("default:width");
        int height = _config->getInteger("default:height");
        // get xauthfile
        std::string mcookie = Tools::runcmd(_config->getString("mcookie:path"));
        Application::debug("use mcookie: %s", mcookie.c_str());
        std::string xauthFile = createXauthFile(screen, mcookie, userXvfb, remoteAddr);

        if(xauthFile.empty())
            return -1;

	std::string socketFormat = _config->getString("xvfb:socket");
    	std::string socketPath = Tools::replace(socketFormat, "%{display}", screen);

	if(std::filesystem::is_socket(socketPath))
        {
            // fixed xvfb socket pemission 660
            std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                    std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace);
	    setFileOwner(socketPath, uid, getGroupGid(groupAuth));
        }

        int pid1 = runXvfbDisplay(screen, width, height, xauthFile, userXvfb);
        if(0 > pid1)
        {
            Application::error("fork failed, result: %d", pid1);
            std::filesystem::remove(xauthFile);
            return -1;
        }

        // parent continue
        Application::debug("xvfb child started, pid: %d, display: %d", pid1, screen);

        // wait Xvfb display starting
        if(! waitXvfbStarting(screen, 5000 /* 5 sec */))
        {
            Application::error("system error: %s", "xvfb not started");
            std::filesystem::remove(xauthFile);
            return -1;
        }

        // running login helper
        int pid2 = runLoginHelper(screen, xauthFile, userXvfb);

        if(0 > pid2)
        {
            Application::error("fork failed, result: %d", pid2);
            std::filesystem::remove(xauthFile);
            return -1;
        }

        // parent continue
        Application::debug("helper child started, pid: %d, display: %d", pid2, screen);
        // registry screen
        struct XvfbSession st;
        st.pid1 = pid1;
        st.pid2 = pid2;
        st.width = width;
        st.height = height;
        st.xauthfile = xauthFile;
        st.mcookie = mcookie;
        st.remoteaddr = remoteAddr;
        st.conntype = connType;
        st.mode = XvfbMode::SessionLogin;
        st.uid = uid;
        st.gid = gid;
        st.user.assign(userXvfb);
        st.durationlimit = _config->getInteger("session:duration_max_sec", 0);

        registryXvfbSession(screen, st);

        Application::debug("login session registered, display: %d", screen);
        return screen;
    }

    int32_t Manager::Object::busStartUserSession(const int32_t & oldScreen, const std::string & userName, const std::string & remoteAddr, const std::string & connType)
    {
        std::string userXvfb = _config->getString("user:xvfb");
        std::string sessionBin = _config->getString("session:path");

        Application::info("user session request, remote: %s, user: %s", remoteAddr.c_str(), userName.c_str());

        int uid, gid;
        std::string home, shell;
        std::tie(uid, gid, home, shell) = getUserInfo(userName);

        if(0 > uid)
        {
            Application::error("username not found, user: %s, uid: %d, gid: %d", userName.c_str(), uid, gid);
            return -1;
        }

        int userScreen = findUserSession(userName);
        if(0 <= userScreen && checkXvfbSocket(userScreen))
        {
            // parent continue
            XvfbSession* xvfb = getXvfbInfo(userScreen);
            xvfb->remoteaddr = remoteAddr;
            xvfb->conntype = connType;
    	    xvfb->mode = XvfbMode::SessionOnline;

            // update conn info
            auto file = createSessionConnInfo(home, xvfb);
            setFileOwner(file, uid, gid);

            emitSessionReconnect(remoteAddr, connType);
            Application::debug("user session connected, display: %d", userScreen);

            return userScreen;
        }

        // get owner screen
        XvfbSession* xvfb = getXvfbInfo(oldScreen);

        if(! xvfb)
        {
            Application::error("display not found: %d", oldScreen);
            return -1;
        }

        if(xvfb->mode != XvfbMode::SessionLogin)
        {
            Application::error("session busy, display: %d, user: %s", oldScreen, xvfb->user.c_str());
            return -1;
        }

        if(! xvfb->pamh)
        {
            Application::error("system error: %s", "pam not started");
            return -1;
        }


        // xauthfile pemission 440
        std::string groupAuth = _config->getString("group:auth");
        int gidAuth = getGroupGid(groupAuth);
        std::filesystem::permissions(xvfb->xauthfile, std::filesystem::perms::owner_read |
                    std::filesystem::perms::group_read, std::filesystem::perm_options::replace);
	setFileOwner(xvfb->xauthfile, uid, gidAuth);

        // xvfb socket pemission 660
        std::string socketFormat = _config->getString("xvfb:socket");
        std::string socketPath = Tools::replace(socketFormat, "%{display}", oldScreen);
        std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                    std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace);
	setFileOwner(socketPath, uid, gidAuth);

        // registry screen
        struct XvfbSession st = *xvfb;

        st.remoteaddr = remoteAddr;
        st.conntype = connType;
        st.mode = XvfbMode::SessionOnline;
        st.uid = uid;
        st.gid = gid;
        st.user.assign(userName);
        st.tpstart = std::chrono::system_clock::now();

        st.pid2 = runUserSession(oldScreen, sessionBin, st);
	if(st.pid2 < 0)
	{
            Application::error("user session failed, result: %d", st.pid2);
            return -1;
	}

        // parent continue
        Application::debug("user session child started, pid: %d, display: %d", st.pid2, oldScreen);

        registryXvfbSession(oldScreen, st);

        std::string sessreg = _config->getString("sessreg:path");
        if(! sessreg.empty())
        {
            std::string args = _config->getString("sessreg:args1");
            args = Tools::replace(args, "%{display}", oldScreen);
            args = Tools::replace(args, "%{user}", userName);
            sessreg.append(" ").append(args);
            int ret = std::system(sessreg.c_str());
            Application::debug("system cmd: `%s', return code: %d, display: %d", sessreg.c_str(), ret, oldScreen);
        }

        Application::debug("user session registered, display: %d", oldScreen);

        return oldScreen;
    }

    int32_t Manager::Object::busGetServiceVersion(void)
    {
        return LTSM::service_version;
    }

    std::string Manager::Object::busCreateAuthFile(const int32_t & display)
    {
        Application::info("xauthfile request, screen: :%d\n", display);
        auto xvfb = getXvfbInfo(display);
        return xvfb ? xvfb->xauthfile : "";
    }

    bool Manager::Object::busShutdownDisplay(const int32_t & display)
    {
        Application::info("shutdown display: %d", display);
        displayShutdown(display, true);
        return true;
    }

    bool Manager::Object::busShutdownConnector(const int32_t& display)
    {
        Application::info("shutdown connector, display: %d", display);
        emitShutdownConnector(display);
        return true;
    }

    bool Manager::Object::busSendMessage(const int32_t& display, const std::string& message)
    {
        Application::info("send message, display: %d, message: %s", display, message.c_str());

        if(auto xvfb = getXvfbInfo(display))
	{
            std::string home;
	    std::tie(std::ignore, std::ignore, home, std::ignore) = Manager::getUserInfo(xvfb->user);

            auto  dtn = std::chrono::system_clock::now().time_since_epoch();
            auto file = std::string(home).append("/.ltsm/messages").append("/").append(std::to_string(dtn.count()));
            auto dir = Tools::dirname(file);

            if(! std::filesystem::is_directory(dir))
                std::filesystem::create_directory(dir);

            // fixed permissions 750
            std::filesystem::permissions(dir, std::filesystem::perms::group_write |
                    std::filesystem::perms::others_all, std::filesystem::perm_options::remove);
 
            setFileOwner(dir, xvfb->uid, xvfb->gid);

            std::ofstream ofs(file, std::ofstream::trunc);
            if(ofs.good())
            {
                ofs << message;
                ofs.close();
                setFileOwner(file, xvfb->uid, xvfb->gid);
            }
            else
            {
                Application::error("can't create file: %s", file.c_str());
            }
        }

        return true;
    }

    bool Manager::Object::busConnectorAlive(const int32_t & display)
    {
	std::thread([=](){ this->confirmCheckConnection(display); }).detach();
	return true;
    }

    bool Manager::Object::busConnectorTerminated(const int32_t & display)
    {
        Application::info("connector terminated, display: %d", display);

        if(auto xvfb = getXvfbInfo(display))
	{
	    if(xvfb->mode == XvfbMode::SessionLogin)
        	displayShutdown(display, false);
	    else
	    if(xvfb->mode == XvfbMode::SessionOnline)
	    {
		xvfb->mode = XvfbMode::SessionSleep;

	        auto userInfo = Manager::getUserInfo(xvfb->user);
                createSessionConnInfo(std::get<2>(userInfo), nullptr);

    		emitSessionSleeped(display);
	    }
	}

        return true;
    }

    bool Manager::Object::busConnectorSwitched(const int32_t & oldDisplay, const int32_t & newDisplay)
    {
        Application::info("connector switched, old display: %d, new display: %d", oldDisplay, newDisplay);
        displayShutdown(oldDisplay, false);
        return true;
    }

    bool Manager::Object::helperWidgetStartedAction(const int32_t & display)
    {
        Application::info("helper widget started, display: %d", display);
        emitHelperWidgetStarted(display);
        return true;
    }

    bool Manager::Object::helperIdleTimeoutAction(const int32_t & display)
    {
        Application::info("helper idle action, timeout: %d sec, display: %d", _helperIdleTimeout, display);
        displayShutdown(display, true);
        return true;
    }

    std::string Manager::Object::helperGetTitle(const int32_t & display)
    {
        return _helperTitle;
    }

    std::string Manager::Object::helperGetDateFormat(const int32_t & display)
    {
        return _helperDateFormat;
    }

    int32_t Manager::Object::helperGetIdleTimeoutSec(const int32_t & display)
    {
        return _helperIdleTimeout;
    }

    bool Manager::Object::helperIsAutoComplete(const int32_t & display)
    {
        return _helperAutoComplete;
    }

    std::list<std::string> Manager::Object::getSystemUsersList(int uidMin, int uidMax) const
    {
        std::list<std::string> logins;
        setpwent();

        while(struct passwd* st = getpwent())
        {
            if((uidMin <= 0 || uidMin <= st->pw_uid) && (uidMax <= 0 || st->pw_uid <= uidMax))
                logins.emplace_back(st->pw_name);
        }

        endpwent();
        return logins;
    }

    std::vector<std::string> Manager::Object::helperGetUsersList(const int32_t & display)
    {
        auto logins = getSystemUsersList(_helperAutoCompeteMinUid, _helperAutoCompeteMaxUid);

        if(! _helperAccessUsersList.empty())
        {
            auto allows = _helperAccessUsersList;
            auto itend = std::remove_if(allows.begin(), allows.end(), [&](auto & user)
            {
                return std::none_of(logins.begin(), logins.end(),
                            [&](auto & login) { return login == user; });
            });
            allows.erase(itend, allows.end());
            return allows;
        }

        return std::vector<std::string>(logins.begin(), logins.end());
    }

    bool Manager::Object::busCheckAuthenticate(const int32_t & display, const std::string & login, const std::string & password)
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
            Application::error("pam_conv: %s", "empty data");
            return PAM_CONV_ERR;
        }

        if(! msg || ! resp)
        {
            Application::error("pam_conv: %s", "empty params");
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
                    Application::error("pam error: %s", pm->msg);
                    break;

                case PAM_TEXT_INFO:
                    Application::info("pam info: %s", pm->msg);
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
        std::string pamService = _config->getString("pam:service");

        if(! _helperAccessUsersList.empty() &&
           std::none_of(_helperAccessUsersList.begin(), _helperAccessUsersList.end(), [&](auto & val) { return val == login; }))
        {
            Application::error("checking helper:accesslist, username not found: %s, display: %d", login.c_str(), display);
            emitLoginFailure(display, "login deny");
            return false;
        }
        auto users = helperGetUsersList(display);

        if(users.empty())
        {
            Application::error("%s", "system users list is empty, the login is blocked.");
            emitLoginFailure(display, "login blocked");
            return false;
        }

        if(std::none_of(users.begin(), users.end(), [&](auto & val) { return val == login; }))
        {
            Application::error("checking system users, username not found: %s, display: %d", login.c_str(), display);
            emitLoginFailure(display, "login not found");
            return false;
        }

        if(auto xvfbLogin = getXvfbInfo(display))
        {
            Application::info("pam authenticate, display: %d, username: %s", display, login.c_str());

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
                Application::error("pam_start failed, error: %d", ret);
                emitLoginFailure(display, "pam error");
                return false;
            }

            // check user
            ret = pam_authenticate(xvfbLogin->pamh, 0);

            if(PAM_SUCCESS != ret)
            {
                const char* err = pam_strerror(xvfbLogin->pamh, ret);
                Application::error("pam_authenticate failed, error: %s", err);
                emitLoginFailure(display, err);
                xvfbLogin->loginFailures += 1;

                if(_loginFailuresConf < xvfbLogin->loginFailures)
                {
                    Application::error("login failures limit, shutdown display: %d", display);
                    emitLoginFailure(display, "failures limit");
                    displayShutdown(display, true);
                }

                return false;
            }

            // auth success
            if(0 < _loginFailuresConf)
                xvfbLogin->loginFailures = 0;

	    // check connection policy
	    int sessionId = findUserSession(login);
	    if(0 < sessionId)
	    {
    		auto xvfbSession = getXvfbInfo(sessionId);
		if(xvfbSession && xvfbSession->mode == XvfbMode::SessionOnline)
		{
		    if(xvfbSession->policy == SessionPolicy::AuthLock)
        	    {
			Application::error("session busy, from: %s, shutdown display: %d", xvfbSession->remoteaddr.c_str(), display);
            		emitLoginFailure(display, std::string("session busy, from: ").append(xvfbSession->remoteaddr));
			return false;
		    }
		    else
		    if(xvfbSession->policy == SessionPolicy::AuthTake)
		    {
			// shutdown prev connect
			emitShutdownConnector(sessionId);
		    }
		}
	    }

            // check access
            ret = pam_acct_mgmt(xvfbLogin->pamh, 0);

            if(ret == PAM_NEW_AUTHTOK_REQD)
                ret = pam_chauthtok(xvfbLogin->pamh, PAM_CHANGE_EXPIRED_AUTHTOK);

            if(PAM_SUCCESS != ret)
            {
                const char* err = pam_strerror(xvfbLogin->pamh, ret);
                Application::error("pam_acct_mgmt error: %s", err);
                emitLoginFailure(display, err);
                return false;
            }

            emitLoginSuccess(display, login);
            return true;
        }

        return false;
    }

    bool Manager::Object::busSetDebugLevel(const std::string & level)
    {
        Application::info("set debug level: %s", level.c_str());
        Application::busSetDebugLevel(level);
        return true;
    }

    bool Manager::Object::busSetSessionDurationSec(const int32_t & display, const uint32_t & duration)
    {
        Application::info("set session duration: %d, display: %d", duration, display);

        if(auto xvfb = getXvfbInfo(display))
	{
	    const std::lock_guard<std::mutex> lock(_mutex);
	    xvfb->durationlimit = duration;
            emitSessionParamsChanged(display);
	    return true;
	}
	return false;
    }

    bool Manager::Object::busSetSessionPolicy(const int32_t& display, const std::string & policy)
    {
        Application::info("set session policy: %s, display: %d", policy.c_str(), display);

        if(auto xvfb = getXvfbInfo(display))
	{
	    const std::lock_guard<std::mutex> lock(_mutex);

            if(Tools::lower(policy) == "authlock")
                xvfb->policy = SessionPolicy::AuthLock;
            else
            if(Tools::lower(policy) == "authtake")
                xvfb->policy = SessionPolicy::AuthTake;
            else
            if(Tools::lower(policy) == "authshare")
                xvfb->policy = SessionPolicy::AuthShare;
	    else
    	        Application::error("unknown policy: %s, display: %d", policy.c_str(), display);

            emitSessionParamsChanged(display);
	    return true;
        }
	return false;
    }

    std::vector<xvfb2tuple> Manager::Object::busGetSessions(void)
    {
        return toSessionsList();
    }

    bool Manager::Object::busRenderRect(const int32_t& display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>& rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color, const bool& fill)
    {
        emitAddRenderRect(display, rect, color, fill);
        return true;
    }

    bool Manager::Object::busRenderText(const int32_t& display, const std::string& text, const sdbus::Struct<int16_t, int16_t>& pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color)
    {
        emitAddRenderText(display, text, pos, color);
        return true;
    }

    bool Manager::Object::busRenderClear(const int32_t& display)
    {
        emitClearRenderPrimitives(display);
        return true;
    }

    void Manager::Object::childEnded(int pid, int status)
    {
        Application::debug("child ended, pid: %d, status: %d", pid, status);
        int display = findXvfbPIDs(pid);

        if(auto xvfb = getXvfbInfo(display))
        {
            // child ended or abnormal shutdown only
            if(xvfb->pid2 == pid && ! xvfb->shutdown)
            {
                // helper closed
                if(xvfb->mode == XvfbMode::SessionLogin)
                {
                }
                else
                // session closed
                if(xvfb->mode == XvfbMode::SessionOnline || xvfb->mode == XvfbMode::SessionSleep)
                {
                    displayShutdown(display, true);
                }
            }
        }
    }

    namespace Manager
    {
        std::unique_ptr<Object> obj;
        std::atomic<bool> running(false);
    }

    /* Manager::Service */
    Manager::Service::Service(int argc, const char** argv)
        : ApplicationJsonConfig("ltsm_service", argc, argv)
    {
        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help"))
            {
                std::cout << "usage: " << argv[0] << " --config <path>" << std::endl;
                throw 0;
            }
        }

        // check present executable files
        for(auto key : _config.keys())
        {
            if(5 < key.size() && 0 == key.substr(key.size() - 5).compare(":path"))
            {
                auto value = _config.getString(key);
                Application::debug("checking executable: %s", value.c_str());

                if(! std::filesystem::exists(value))
                    throw std::string("application not found: ").append(value);
            }
        }
    }

    bool Manager::Service::createXauthDir(void)
    {
        std::string xauthFile = _config.getString("xauth:file");
        std::string groupAuth = _config.getString("group:auth");
        // find group id
        int setgid = getGroupGid(groupAuth);

        if(0 > setgid) setgid = 0;

        // check directory
        auto folder = Tools::dirname(xauthFile);

        if(folder.size())
        {
            // create
            if(! std::filesystem::is_directory(folder))
            {
                if(! std::filesystem::create_directory(folder))
                {
                    Application::error("mkdir failed, dir: %s", folder.c_str());
                    return false;
                }
            }

            // fix mode 755
	    std::filesystem::permissions(folder, std::filesystem::perms::owner_all |
		    std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
		    std::filesystem::perms::others_read | std::filesystem::perms::others_exec, std::filesystem::perm_options::replace);
            // fix owner
            if(0 != chown(folder.c_str(), 0, setgid))
                    Application::error("chown failed, dir: %s, uid: %d, gid: %d, error: %s", folder.c_str(), 0, setgid, strerror(errno));
            return true;
        }

        return false;
    }

    int Manager::Service::start(void)
    {
        if(0 < getuid())
        {
            Application::error("assert: %s", "root privileges");
            return EXIT_FAILURE;
        }

        auto conn = sdbus::createSystemBusConnection(LTSM::dbus_service_name);
        if(! conn)
        {
            Application::error("%s", "dbus create connection failed");
            return EXIT_FAILURE;
        }

        signal(SIGTERM, signalHandler);
        signal(SIGCHLD, signalHandler);
        signal(SIGINT,  signalHandler);
        signal(SIGHUP,  SIG_IGN);

        createXauthDir();

        Manager::obj.reset(new Manager::Object(*conn, _config, *this));
        Manager::running = true;
        Application::busSetDebugLevel(_config.getString("logging:level"));
        Application::info("%s", "dbus events loop");

        while(Manager::running)
        {
            conn->enterEventLoopAsync();
            std::this_thread::sleep_for(10ms);
            Manager::obj->systemTick();
        }

        Manager::running = false;
        Manager::obj.reset();
        conn->enterEventLoopAsync();

        return EXIT_SUCCESS;
    }

    void Manager::Service::signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
            Manager::running = false;
        else if(sig == SIGCHLD && Manager::running)
        {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);

            if(0 < pid)
                Manager::obj->childEnded(pid, status);
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
        LTSM::Application::info("%s", "terminate...");
    }
    catch(const std::string & err)
    {
        LTSM::Application::error("%s", err.c_str());
        LTSM::Application::info("%s", "terminate...");
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
