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

namespace LTSM
{
    std::unique_ptr<sdbus::IConnection> conn;

    void signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
        {
            if(conn)
            {
                conn->leaveEventLoop();
            }
        }
    }

    int waitPid(pid_t pid)
    {
        Application::debug(DebugType::App, "%s: pid: %" PRId32, __FUNCTION__, pid);

        int status;
        int ret = waitpid(pid, &status, 0);

        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                __FUNCTION__, "waitpid", strerror(errno), errno);
            return ret;
        }

        if(WIFSIGNALED(status))
        {
            Application::warning("%s: process %s, pid: %" PRId32 ", signal: %" PRId32,
                __FUNCTION__, "killed", pid, WTERMSIG(status));
        }
        else
        if(WIFEXITED(status))
        {
            Application::info("%s: process %s, pid: %" PRId32 ", return: %" PRId32,
                __FUNCTION__, "exited", pid, WEXITSTATUS(status));
        }
        else
        {
            Application::debug(DebugType::App, "%s: process %s, pid: %" PRId32 ", wstatus: 0x%08" PRIx32,
                __FUNCTION__, "ended", pid, status);
        }

        return status;
    }

    StatusStdout waitStatusStdout(pid_t pid, int fd)
    {
        const size_t block = 1024;
        std::vector<uint8_t> res(block);
        res.reserve(4 * block);

        uint8_t* ptr = res.data();
        size_t last = block;

        while(true)
        {
            int ret = read(fd, ptr, last);

            if(ret < 0)
            {
                if(EAGAIN == errno || EINTR == errno)
                {
                    continue;
                }

                Application::error("%s: %s failed, error: %s, code: %" PRId32, __FUNCTION__, "read", strerror(errno), errno);
                res.clear();
                break;
            }

            // end stream
            if(ret == 0)
            {
                res.resize(res.size() - last);
                break;
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

        int status = waitPid(pid);
        return std::make_pair(status, std::move(res));
    }

    void runChildProcess(const std::filesystem::path & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs, int pipeout)
    {
        Application::debug(DebugType::App, "%s: pid: %" PRId32 ", cmd: `%s'", __FUNCTION__, getpid(), cmd.c_str());

        for(const auto & env: envs)
        {
            putenv(const_cast<char*>(env.c_str()));
        }

        // create argv[]
        std::vector<const char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(cmd.c_str());

        for(const auto & arg : args)
        {
            argv.push_back(arg.c_str());
        }

        argv.push_back(nullptr);

        // redirect stdout
        dup2(pipeout, STDOUT_FILENO);

        // redirect stderr
        if(auto home = getenv("HOME"))
        {
            auto ltsmLog = std::filesystem::path{home} / ".ltsm" / "log";

            if(! std::filesystem::is_directory(ltsmLog))
            {
                std::filesystem::create_directory(ltsmLog);
            }

            auto logFile = ltsmLog / cmd.filename();
            logFile.replace_extension(".log");

            if(int fd = open(logFile.c_str(), O_WRONLY | O_CREAT, 0640); 0 <= fd)
            {
                dup2(fd, STDERR_FILENO);
            }
        }

        // skip STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO
        for(int fd = 3; fd < 255; ++fd)
        {
            if(fd == pipeout)
                continue;

            close(fd);
        }

        if(int res = execv(cmd.c_str(), (char* const*) argv.data()); res < 0)
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32 ", path: `%s'",
                __FUNCTION__, "execv", strerror(errno), errno, cmd.c_str());

            execl("/bin/true", "/bin/true", nullptr);
            std::exit(res);
        }
    }

    PidStatusStdout runForkCommand(const std::filesystem::path & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs)
    {
        int pfd[2] = {};

        if(0 > pipe(pfd))
        {
            Application::error("%s: %s failed, error: %s, code: %" PRId32,
                __FUNCTION__, "pipe", strerror(errno), errno);
            throw std::runtime_error(NS_FuncName);
        }

        pid_t pid = Application::forkMode(Application::isDebugLevel(DebugLevel::Debug));

        if(0 == pid)
        {
            close(pfd[0]);
            Application::setDebugTarget(DebugTarget::Console);
            runChildProcess(cmd, args, envs, pfd[1]);
            // child ended
        }

        Application::info("%s: uid: %" PRId32 ", pid: %" PRId32 ", cmd: `%s'", __FUNCTION__, getuid(), pid, cmd.c_str());

        if(Application::isDebugLevel(DebugLevel::Debug))
        {
            Application::debug(DebugType::App, "%s: uid: %" PRId32 "args: [ %s ]",
                __FUNCTION__, getuid(), Tools::join(args.begin(), args.end(), ", ").c_str());
            Application::debug(DebugType::App, "%s: uid: %" PRId32 "envs: [ %s ]",
                __FUNCTION__, getuid(), Tools::join(envs.begin(), envs.end(), ", ").c_str());
        }

        // main thread processed
        close(pfd[1]);

        // planned get stdout from running job
        auto future = std::async(std::launch::async, & waitStatusStdout, pid, pfd[0]);
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

        Application::warning("%s: %s found, display: %" PRId32,
            __FUNCTION__, "xcb cookie not", displayNum);

        return {};
    }

    /// DisplaySessionBus
    DisplaySessionBus::DisplaySessionBus(sdbus::IConnection & conn, int display) : ApplicationJsonConfig("ltsm_session_display"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(conn, sdbus::ObjectPath {dbus_session_display_path}),
#else
        AdaptorInterfaces(conn, dbus_session_display_path),
#endif
        displayNum(display)
    {
        displayStr.append(":").append(std::to_string(displayNum));
        setenv("DISPLAY", displayStr.c_str(), 1);

        defaultWidth = configGetInteger("default:width", 1280);
        defaultHeight = configGetInteger("default:height", 1024);
        defaultDepth = configGetInteger("default:depth", 24);

        timer1 = Tools::BaseTimer::create<std::chrono::milliseconds>(300, true,
            std::bind(& DisplaySessionBus::checkChildCommandsComplete, this));

        started = std::chrono::system_clock::now();
        registerAdaptor();
    }

    DisplaySessionBus::~DisplaySessionBus()
    {
        timer1->stop();
        childStop();
        unregisterAdaptor();
    }

    void DisplaySessionBus::childStop(void)
    {
        std::scoped_lock guard{ lockCommands };

        if(! childCommands.empty())
        {
            for(auto & pidStatus: childCommands)
            {
                kill(pidStatus.first, SIGTERM);
            }

            while(true)
            {
                auto process = std::count_if(childCommands.begin(), childCommands.end(), [](auto & pidStatus)
                {
                    auto & futureStatus = pidStatus.second;
                    return futureStatus.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready;
                });

                if(0 == process)
                    break;

                std::this_thread::sleep_for(50ms);
            }

            childCommands.clear();
        }
    }

    bool DisplaySessionBus::xcbConnect(void)
    {
        if(Tools::checkUnixSocket(Tools::x11UnixPath(displayNum)))
        {
            try
            {
                xcb = std::make_unique<XCB::Connector>(displayNum, std::addressof(mcookie));
                return 0 == xcb->hasError();
            }
            catch(const std::exception &)
            {
            }
        }

        return false;
    }

    void DisplaySessionBus::childProcessEnded(int pid, std::future<StatusStdout> futureStatus)
    {
        auto statusStdout = futureStatus.get();

        if(pid == pidXorg || pid == pidSession)
        {
            if(pid == pidSession)
            {
                if(0 < pidXorg)
                    kill(pidXorg, SIGTERM);
            }

            pidXorg = -1;
            pidSession = -1;

            std::thread([this]()
            {
                std::this_thread::sleep_for(10ms);
                this->serviceShutdown();
            }).detach();

            return;
        }

        if(WIFEXITED(statusStdout.first))
        {
            emitRunSessionCommandSuccess(pid, WEXITSTATUS(statusStdout.first), statusStdout.second);
        }
        else
        { 
            emitRunSessionCommandFailed(pid, statusStdout.first);
        }
    }

    void DisplaySessionBus::checkChildCommandsComplete(void)
    {
        std::scoped_lock guard{ lockCommands };

        if(childCommands.empty())
        {
            return;
        }

        childCommands.remove_if([this](auto & pidStatus)
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

    int DisplaySessionBus::start(void)
    {
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

        if(! getenv("DISPLAY"))
        {
            Application::error("%s: %s not found", __FUNCTION__, "DISPLAY");
            return EXIT_FAILURE;
        }

        // xauth file
        if(auto xauthfile = getenv("XAUTHORITY"))
        {
            if(0 != access(xauthfile, R_OK))
            {
                Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "xauthfile read", xauthfile);
                return EXIT_FAILURE;
            }

            xauthFile.assign(xauthfile);
            mcookie = readXauthFile(xauthfile, displayNum);
        }
        else
        {
            Application::error("%s: %s not found", __FUNCTION__, "LTSM_XAUTHFILE");
            return EXIT_FAILURE;
        }

        // xorg bin
        auto xorgBin = configGetString("xvfb:path");

        if(0 != access(xorgBin.c_str(), X_OK))
        {
            Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "xorg exec", xorgBin.c_str());
            return EXIT_FAILURE;
        }

        // xorg args
        auto ja = config().getArray("xvfb:args");

        if(!ja)
        {
            Application::error("%s: %s failed", __FUNCTION__, "xorg args");
            return EXIT_FAILURE;
        }

        auto xorgArgs = ja->toStdVector<std::string>();

        for(auto & str: xorgArgs)
        {
            str = Tools::replace(str, "%{width}", defaultWidth);
            str = Tools::replace(str, "%{height}", defaultHeight);
            str = Tools::replace(str, "%{depth}", defaultDepth);
            str = Tools::replace(str, "%{display}", displayNum);
            str = Tools::replace(str, "%{authfile}", xauthFile);
        }

        // session bin
        auto sessionBin = configGetString("session:path");

        if(0 != access(xorgBin.c_str(), X_OK))
        {
            Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "session exec", sessionBin.c_str());
            return EXIT_FAILURE;
        }

        // session args
        ja = config().getArray("session:args");
        auto sessionArgs = ja ? ja->toStdVector<std::string>() : std::vector<std::string>();

        if(getenv("LTSM_LOGIN_MODE"))
        {
            // helper login
            auto helperBin = configGetString("helper:path", "/usr/libexec/ltsm/LTSM_helper");

            if(0 != access(helperBin.c_str(), X_OK))
            {
                Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "login helper", helperBin.c_str());
                return EXIT_FAILURE;
            }

            sessionBin = helperBin;
            sessionArgs.clear();
        }

        Application::info("service started, uid: %d, pid: %d, version: %d", getuid(), getpid(), LTSM_SESSION_DISPLAY_VERSION);

        signal(SIGTERM, signalHandler);
        signal(SIGINT, signalHandler);

        // 1. start Xorg
        childCommands.emplace_front( runForkCommand(xorgBin, xorgArgs, {}) );
        pidXorg = childCommands.front().first;

        auto xvfbTimeoutMs = configGetInteger("xvfb:timeout", 3500);
        if(! Tools::waitCallable<std::chrono::milliseconds>(xvfbTimeoutMs, 100, std::bind(& DisplaySessionBus::xcbConnect, this)))
        {
            Application::error("%s: %s failed", __FUNCTION__, "X11 connect");
            return EXIT_FAILURE;            
        }

        // fix X11 socket pemissions 0660
        std::filesystem::permissions(Tools::x11UnixPath(displayNum),
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read | std::filesystem::perms::group_write, std::filesystem::perm_options::replace);

        auto xsetupBin = "/etc/ltsm/xsetup";
        if(std::filesystem::exists(xsetupBin))
        {
            childCommands.emplace_front( runForkCommand(xsetupBin, {}, {}) );
        }

        // 2. start Session
        childCommands.emplace_front( runForkCommand(sessionBin, sessionArgs, {}) );
        pidSession = childCommands.front().first;

        // start main loop
        conn->enterEventLoop();

        // loop ended
        timer1->stop();

        if(0 < pidXorg)
            kill(pidXorg, SIGTERM);

        if(0 < pidSession)
            kill(pidSession, SIGTERM);

        childStop();

        Application::debug(DebugType::App, "service stopped");
        return EXIT_SUCCESS;
    }

    int32_t DisplaySessionBus::getVersion(void)
    {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);
        return LTSM_SESSION_DISPLAY_VERSION;
    }

    void DisplaySessionBus::serviceShutdown(void)
    {
        Application::debug(DebugType::Dbus, "%s: pid: %s", __FUNCTION__, getpid());
        conn->leaveEventLoop();
    }

    void DisplaySessionBus::setDebug(const std::string & level)
    {
        Application::debug(DebugType::Dbus, "%s: level: %s", __FUNCTION__, level.c_str());
        setDebugLevel(level);
    }

    int32_t DisplaySessionBus::runSessionCommand(const std::string & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs)
    {
        Application::debug(DebugType::Dbus, "%s: cmd: `%s'", __FUNCTION__, cmd.c_str());

        try
        {
            std::scoped_lock guard{ lockCommands };
            childCommands.emplace_front( runForkCommand(cmd, args, envs) );

            // return pid
            return childCommands.front().first;
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }

        return -1;
    }

    std::string DisplaySessionBus::jsonStatus(void)
    {
        JsonObjectStream jos;

        jos.push("display:num", displayNum);
        jos.push("xorg:pid", pidXorg);
        jos.push("session:pid", pidSession);
        jos.push("running:sec", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - started).count());
 
        return jos.flush();
    }
}

int main(int argc, char** argv)
{
    int display = -1;

    for(int it = 1; it < argc; ++it)
    {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
        {
            std::cout << "usage: " << argv[0] << " --display <num>" << std::endl;
            return EXIT_SUCCESS;
        }
        else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v"))
        {
            std::cout << "version: " << LTSM_SESSION_DISPLAY_VERSION << std::endl;
            return EXIT_SUCCESS;
        }
        else if((0 == std::strcmp(argv[it], "--display") || 0 == std::strcmp(argv[it], "-d")) && it + 1 < argc)
        {
            try
            {
                display = std::stoi(argv[it + 1]);
            }
            catch(...)
            {
            }

            it += 1;
        }
    }

    if(0 > display)
    {
        std::cerr << "invalid display number" << std::endl;
        return EXIT_FAILURE;
    }

    if(0 == getuid())
    {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    try
    {
#ifdef SDBUS_2_0_API
        LTSM::conn = sdbus::createSessionBusConnection(sdbus::ServiceName {LTSM::dbus_session_display_name});
#else
        LTSM::conn = sdbus::createSessionBusConnection(LTSM::dbus_session_display_name);
#endif

        if(! LTSM::conn)
        {
            LTSM::Application::error("dbus connection failed, uid: %d", getuid());
            return EXIT_FAILURE;
        }

        return LTSM::DisplaySessionBus(*LTSM::conn, display).start();
    }
    catch(const sdbus::Error & err)
    {
        LTSM::Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
    }

    return EXIT_FAILURE;
}
