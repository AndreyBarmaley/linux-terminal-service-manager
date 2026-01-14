/***********************************************************************
 *   Copyright © 2025 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <chrono>
#include <future>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_streambuf.h"
#include "ltsm_display_session.h"

using namespace std::chrono_literals;

namespace LTSM::DisplaySession
{
    std::unique_ptr<sdbus::IConnection> sessionConn;

    void signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
        {
            if(sessionConn)
            {
                sessionConn->leaveEventLoop();
            }
        }
    }

    PidStatus runForkCommand(const std::filesystem::path & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs)
    {
        pid_t pid = ForkMode::forkStart(Application::isDebugLevel(DebugLevel::Debug));

        // child
        if(0 == pid)
        {
            ForkMode::runChildProcess(cmd, args, envs, RedirectLog::StdoutStderr);
            // child ended
        }

        // parent
        if(Application::isDebugLevel(DebugLevel::Debug))
        {
            auto sargs = Tools::join(args.begin(), args.end(), ", ");
            auto senvs = Tools::join(envs.begin(), envs.end(), ", ");
            Application::info("%s: uid: %" PRId32 ", pid: %" PRId32 ", cmd: `%s', args: [ %s ], envs: [ %s ]",
                              __FUNCTION__, getuid(), pid, cmd.c_str(), sargs.c_str(), senvs.c_str());
        }
        else
        {
            Application::info("%s: uid: %" PRId32 ", pid: %" PRId32 ", cmd: `%s'", __FUNCTION__, getuid(), pid, cmd.c_str());
        }

        // main thread processed
        auto future = std::async(std::launch::async, [pid]()
        {
            return ForkMode::waitPid(pid);
        });
        return std::make_pair(pid, std::move(future));
    }

    std::vector<uint8_t> readXauthFile(const std::filesystem::path & xauthFilePath, int displayNum)
    {
        std::vector<uint8_t> buf = Tools::fileToBinaryBuf(xauthFilePath);
        StreamBufRef sb(buf.data(), buf.size());
        uint16_t len = 0;

        while(sb.last())
        {
            // format: 01 00 [ <host len:be16> [ host ]] [ <display len:be16> [ display ]] [ <magic len:be16> [ magic ]] [ <cookie len:be16> [ cookie ]]
            if(auto ver = sb.readIntBE16(); ver != 0x0100)
            {
                Application::error("%s: invalid xauth format, ver: 0x%04" PRIx16, __FUNCTION__, ver);
                throw std::runtime_error(NS_FuncName);
            }

            len = sb.readIntBE16();
            auto host = sb.readString(len);

            len = sb.readIntBE16();
            auto display = sb.readString(len);

            len = sb.readIntBE16();
            auto magic = sb.readString(len);

            len = sb.readIntBE16();
            auto cookie = sb.read(len);

            if(display == std::to_string(displayNum))
            {
                Application::debug(DebugType::App, "%s: %s found, display %" PRId32,
                                   __FUNCTION__, "xcb cookie", displayNum);
                return cookie;
            }
        }

        Application::error("%s: %s found, display: %" PRId32,
                           __FUNCTION__, "xcb cookie not", displayNum);

        throw std::runtime_error(NS_FuncName);
    }

    std::unique_ptr<XCB::Connector> waitX11DisplayStarting(int displayNum, const XCB::AuthCookie & mcookie, uint32_t ms)
    {
        std::unique_ptr<XCB::Connector> res;

        Tools::waitCallable<std::chrono::milliseconds>(ms, 100, [displayNum, auth = std::addressof(mcookie), &res]()
        {
            if(Tools::checkUnixSocket(Tools::x11UnixPath(displayNum)))
            {
                try
                {
                    if(res = std::make_unique<XCB::Connector>(displayNum, auth))
                    {
                        return 0 == res->hasError();
                    }
                }
                catch(const std::exception &)
                {
                }
            }

            return false;
        });

        return res;
    }

    void clearSessionDbusAddress(int displayNum)
    {
        if(auto env = getenv("XDG_RUNTIME_DIR"))
        {
            auto dbusPath = std::filesystem::path{env} / "ltsm" / Tools::joinToString("dbus_session_", displayNum);
            std::filesystem::remove(dbusPath);
        }
    }

    std::string waitSessionDbusAddress(int displayNum, uint32_t ms)
    {
        if(auto env = getenv("XDG_RUNTIME_DIR"))
        {
            // ltsm path from /etc/ltsm/xclients
            auto dbusPath = std::filesystem::path{env} / "ltsm" / Tools::joinToString("dbus_session_", displayNum);
            std::string res;

            Tools::waitCallable<std::chrono::milliseconds>(ms, 100, [&dbusPath, &res]()
            {
                try
                {
                    if(std::filesystem::is_regular_file(dbusPath))
                    {
                        res = Tools::fileToString(dbusPath);
                        return ! res.empty();
                    }
                }
                catch(const std::exception &)
                {
                }

                return false;
            });

            return res;
        }

        Application::error("%s: %s not found", __FUNCTION__, "XDG_RUNTIME_DIR");
        return "";
    }

    // DBusAdaptor
    DBusAdaptor::DBusAdaptor(sdbus::IConnection & conn, Starter & starter)
#ifdef SDBUS_2_0_API
        : AdaptorInterfaces(conn, sdbus::ObjectPath {dbus_session_display_path}),
#else
        :
        AdaptorInterfaces(conn, dbus_session_display_path),
#endif
          starter_(starter)
    {
        registerAdaptor();
    }

    DBusAdaptor::~DBusAdaptor()
    {
        unregisterAdaptor();
    }

    int32_t DBusAdaptor::getVersion(void)
    {
        return starter_.dbusGetVersion();
    }

    void DBusAdaptor::serviceShutdown(void)
    {
        starter_.dbusServiceShutdown();
    }

    void DBusAdaptor::setDebug(const std::string & level)
    {
        starter_.dbusSetDebug(level);
    }

    std::string DBusAdaptor::jsonStatus(void)
    {
        return starter_.dbusJsonStatus();
    }

    int32_t DBusAdaptor::runSessionCommand(const std::string & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs)
    {
        return starter_.dbusRunSessionCommand(cmd, args, envs);
    }

    // Starter
    Starter::Starter(int displayNum, const char* xauthFile)
        : ApplicationJsonConfig("ltsm_session_display"), started_(std::chrono::system_clock::now())
    {
        Application::info("service started, uid: %d, pid: %d, version: %d", getuid(), getpid(), LTSM_SESSION_DISPLAY_VERSION);
        startX11Display(displayNum, xauthFile);
    }

    Starter::~Starter()
    {
        if(timer1_)
        {
            timer1_->stop();
        }

        stopChilds();
    }

    void Starter::startX11Display(int displayNum, const char* xauthFile)
    {
        defaultWidth_ = configGetInteger("default:width", 1280);
        defaultHeight_ = configGetInteger("default:height", 1024);
        defaultDepth_ = configGetInteger("default:depth", 24);
        displayNum_ = displayNum;
        mcookie_ = readXauthFile(xauthFile, displayNum);

        std::string xorgBin;
        std::vector<std::string> xorgArgs;

        const char* ltsmX11 = "/etc/X11/ltsm.conf";
        const char* ltsmXorg = "/usr/bin/Xorg";
        const char* ltsmXvfb = "/usr/bin/Xvfb";

        if(configHasKey("xvfb:path"))
        {
            xorgBin = configGetString("xvfb:path");
        }
        else if(std::filesystem::exists(ltsmXorg) && std::filesystem::exists(ltsmX11))
        {
            xorgBin.assign(ltsmXorg);
        }
        else
        {
            xorgBin.assign(ltsmXvfb);
        }

        if(! std::filesystem::exists(xorgBin))
        {
            Application::error("%s: path not found: `%s'", __FUNCTION__, xorgBin.c_str());
            throw std::runtime_error(NS_FuncName);
        }

        const bool useXorg = std::filesystem::path(xorgBin).filename() == "Xorg";

        // xorg args
        if(auto ja = config().getArray("xvfb:args"))
        {
            xorgArgs = ja->toStdVector<std::string>();
        }
        else
        {
            // default options for Xvfb/Xorg
            xorgArgs.emplace_back(":%{display}");
            xorgArgs.emplace_back("-nolisten");
            xorgArgs.emplace_back("tcp");

            if(useXorg)
            {
                xorgArgs.emplace_back("-config");
                xorgArgs.emplace_back("ltsm.conf");
                xorgArgs.emplace_back("-quiet");
            }
            else
            {
                xorgArgs.emplace_back("-screen");
                xorgArgs.emplace_back("0");
                xorgArgs.emplace_back("%{width}x%{height}x24");
            }

            xorgArgs.emplace_back("-auth");
            xorgArgs.emplace_back("%{authfile}");
            xorgArgs.emplace_back("+extension");
            xorgArgs.emplace_back("DAMAGE");
            xorgArgs.emplace_back("+extension");
            xorgArgs.emplace_back("MIT-SHM");
            xorgArgs.emplace_back("+extension");
            xorgArgs.emplace_back("RANDR");
            xorgArgs.emplace_back("+extension");
            xorgArgs.emplace_back("XFIXES");
            xorgArgs.emplace_back("+extension");
            xorgArgs.emplace_back("XTEST");
        }

        for(auto & str : xorgArgs)
        {
            str = Tools::replace(str, "%{width}", defaultWidth_);
            str = Tools::replace(str, "%{height}", defaultHeight_);
            str = Tools::replace(str, "%{depth}", defaultDepth_);
            str = Tools::replace(str, "%{display}", displayNum_);
            str = Tools::replace(str, "%{authfile}", xauthFile);
        }

        // start Xorg
        childCommands_.emplace_front(runForkCommand(xorgBin, xorgArgs, {}));
        pidXorg_ = childCommands_.front().first;
    }

    bool Starter::startX11Session(void)
    {
        // session bin
        std::string sessionBin = configGetString("session:path");
        std::vector<std::string> sessionArgs;
        std::vector<std::string> sessionEnvs;

        if(! std::filesystem::exists(sessionBin))
        {
            Application::error("%s: path not found: `%s'", __FUNCTION__, sessionBin.c_str());
            return false;
        }

        // session args
        if(auto ja = config().getArray("session:args"))
        {
            sessionArgs = ja->toStdVector<std::string>();
        }

        if(getenv("LTSM_LOGIN_MODE"))
        {
            // helper login
            auto helperBin = configGetString("helper:path", "/usr/libexec/ltsm/LTSM_helper");

            if(! std::filesystem::exists(helperBin))
            {
                Application::error("%s: path not found: `%s'", __FUNCTION__, helperBin.c_str());
                return false;
            }

            sessionEnvs.emplace_back(std::string("XSESSION=").append(helperBin));
        }

        std::scoped_lock guard{ lockCommands_ };

        const char* xsetupBin = "/etc/ltsm/xsetup";

        if(std::filesystem::exists(xsetupBin))
        {
            // wait xsetup stopped
            auto pidStatus = runForkCommand(xsetupBin, {}, {});
            pidStatus.second.wait();
        }

        // start Session
        childCommands_.emplace_front(runForkCommand(sessionBin, sessionArgs, sessionEnvs));
        pidSession_ = childCommands_.front().first;

        timer1_ = Tools::BaseTimer::create<std::chrono::milliseconds>(300, true,
                  std::bind(& Starter::checkChildCommandsComplete, this));

        return true;
    }

    void Starter::stopChilds(void)
    {
        std::scoped_lock guard{ lockCommands_ };

        if(! childCommands_.empty())
        {
            for(auto & pidStatus : childCommands_)
            {
                kill(pidStatus.first, SIGTERM);
            }

            for(auto & pidStatus : childCommands_)
            {
                auto & futureStatus = pidStatus.second;
                futureStatus.wait();
            }

            childCommands_.clear();
        }
    }

    void Starter::childProcessEnded(int pid, std::future<int> futureStatus)
    {
        if(pid == pidXorg_ || pid == pidSession_)
        {
            sessionConn->leaveEventLoop();
            return;
        }

        if(dbus_)
        {
            int status = futureStatus.get();

            if(WIFEXITED(status))
            {
                dbus_->emitRunSessionCommandSuccess(pid, WEXITSTATUS(status), {});
            }
            else
            {
                dbus_->emitRunSessionCommandFailed(pid, WEXITSTATUS(status));
            }
        }
    }

    void Starter::checkChildCommandsComplete(void)
    {
        std::scoped_lock guard{ lockCommands_ };

        if(childCommands_.empty())
        {
            return;
        }

        // timer job
        childCommands_.remove_if([this](auto & pidStatus)
        {
            auto & futureStatus = pidStatus.second;

            if(futureStatus.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
            {
                return false;
            }

            this->childProcessEnded(pidStatus.first, std::move(futureStatus));
            return true;
        });
    }

    int Starter::run(void)
    {
        const uint32_t x11Timeout = configGetInteger("xvfb:timeout", 3500);

        xcb_ = waitX11DisplayStarting(displayNum_, mcookie_, x11Timeout);

        if(! xcb_)
        {
            Application::error("%s: %s failed", __FUNCTION__, "X11 connect");
            return EXIT_FAILURE;
        }

        clearSessionDbusAddress(displayNum_);

        if(! startX11Session())
        {
            Application::error("%s: %s failed", __FUNCTION__, "X11 session");
            return EXIT_FAILURE;
        }

        auto dbusAddress = waitSessionDbusAddress(displayNum_, x11Timeout);

        if(dbusAddress.empty())
        {
            Application::error("%s: %s failed", __FUNCTION__, "dbus session");
            return EXIT_FAILURE;
        }

        setenv("DBUS_SESSION_BUS_ADDRESS", dbusAddress.c_str(), 1);

#ifdef SDBUS_2_0_API
        DisplaySession::sessionConn = sdbus::createSessionBusConnection(sdbus::ServiceName {dbus_session_display_name});
#else
        DisplaySession::sessionConn = sdbus::createSessionBusConnection(dbus_session_display_name);
#endif

        if(! DisplaySession::sessionConn)
        {
            Application::error("%s: dbus connection failed, uid: %d", __FUNCTION__, getuid());
            return EXIT_FAILURE;
        }

        signal(SIGTERM, signalHandler);
        signal(SIGINT, signalHandler);

        dbus_ = std::make_unique<DBusAdaptor>(*sessionConn, *this);

        // start main loop
        sessionConn->enterEventLoop();

        return EXIT_SUCCESS;
    }

    int32_t Starter::dbusGetVersion(void) const
    {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);
        return LTSM_SESSION_DISPLAY_VERSION;
    }

    void Starter::dbusServiceShutdown(void) const
    {
        Application::debug(DebugType::Dbus, "%s: pid: %s", __FUNCTION__, getpid());
        sessionConn->leaveEventLoop();
    }

    void Starter::dbusSetDebug(const std::string & level)
    {
        Application::debug(DebugType::Dbus, "%s: level: %s", __FUNCTION__, level.c_str());
        setDebugLevel(level);
    }

    int32_t Starter::dbusRunSessionCommand(const std::string & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs)
    {
        Application::debug(DebugType::Dbus, "%s: cmd: `%s'", __FUNCTION__, cmd.c_str());

        try
        {
            std::scoped_lock guard{ lockCommands_ };
            childCommands_.emplace_front(runForkCommand(cmd, args, envs));

            // return pid
            return childCommands_.front().first;
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }

        return -1;
    }

    std::string Starter::dbusJsonStatus(void) const
    {
        JsonObjectStream jos;

        jos.push("display:num", displayNum_);
        jos.push("xorg:pid", pidXorg_);
        jos.push("session:pid", pidSession_);
        jos.push("running:sec", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - started_).count());

        return jos.flush();
    }
}

using namespace LTSM;

int main(int argc, char** argv)
{
    const char* displayAddr = nullptr;
    const char* xauthFile = nullptr;

    for(int it = 1; it < argc; ++it)
    {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
        {
            std::cout << "usage: " << argv[0] << " --display <addr> --xauth <file>" << std::endl;
            return EXIT_SUCCESS;
        }
        else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v"))
        {
            std::cout << "version: " << LTSM_SESSION_DISPLAY_VERSION << std::endl;
            return EXIT_SUCCESS;
        }
        else if((0 == std::strcmp(argv[it], "--display") || 0 == std::strcmp(argv[it], "-d")) && it + 1 < argc)
        {
            displayAddr = argv[it + 1];
            it += 1;
        }
        else if((0 == std::strcmp(argv[it], "--xauth") || 0 == std::strcmp(argv[it], "-x")) && it + 1 < argc)
        {
            xauthFile = argv[it + 1];
            it += 1;
        }
    }

    if(! displayAddr || displayAddr[0] != ':')
    {
        std::cerr << "invalid display addr" << std::endl;
        return EXIT_FAILURE;
    }

    if(! xauthFile || ! std::filesystem::exists(xauthFile))
    {
        std::cerr << "xautfile not found" << std::endl;
        return EXIT_FAILURE;
    }

    if(0 == getuid())
    {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    if(auto home = getenv("HOME"))
    {
        auto ltsmDir = std::filesystem::path{home} / ".ltsm";

        if(! std::filesystem::is_directory(ltsmDir))
        {
            std::filesystem::create_directory(ltsmDir);
        }
    }
    else
    {
        Application::error("%s: %s not found", __FUNCTION__, "HOME");
        return EXIT_FAILURE;
    }

    setenv("DISPLAY", displayAddr, 1);
    setenv("XAUTHORITY", xauthFile, 1);

    try
    {
        int displayNum = std::stoi(displayAddr + 1);
        return DisplaySession::Starter(displayNum, xauthFile).run();
    }
    catch(const sdbus::Error & err)
    {
        Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    }
    catch(const std::exception & err)
    {
        Application::error("exception: %s", err.what());
    }

    return EXIT_FAILURE;
}
