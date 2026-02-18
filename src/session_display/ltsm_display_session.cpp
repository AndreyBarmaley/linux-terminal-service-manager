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
#include <fstream>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_streambuf.h"
#include "ltsm_sdbus_proxy.h"
#include "ltsm_display_session.h"

using namespace std::chrono_literals;

namespace LTSM::DisplaySession {
    std::unique_ptr<sdbus::IConnection> sessionConn;

    void signalHandler(int sig) {
        if(sig == SIGTERM || sig == SIGINT) {
            if(sessionConn) {
                sessionConn->leaveEventLoop();
            }
        }
    }

    PidStatus runForkCommand(const std::filesystem::path & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) {
        pid_t pid = ForkMode::forkStart();

        // child
        if(0 == pid) {
            ForkMode::runChildProcess(cmd, args, envs, RedirectLog::StdoutStderr);
            // child ended
        }

        // parent
        if(Application::isDebugLevel(DebugLevel::Debug)) {
            auto sargs = Tools::join(args.begin(), args.end(), ", ");
            auto senvs = Tools::join(envs.begin(), envs.end(), ", ");
            Application::info("{}: uid: {}, pid: {}, cmd: `{}', args: [ {} ], envs: [ {} ]",
                              __FUNCTION__, getuid(), pid, cmd, sargs, senvs);
        } else {
            Application::info("{}: uid: {}, pid: {}, cmd: `{}'", __FUNCTION__, getuid(), pid, cmd);
        }

        // main thread processed
        auto future = std::async(std::launch::async, [pid]() {
            return ForkMode::waitPid(pid);
        });
        return std::make_pair(pid, std::move(future));
    }

    std::vector<uint8_t> jobBlockRead(int fd) {
        const size_t block = 1024;
        std::vector<uint8_t> res(block);
        uint8_t* ptr = res.data();
        size_t last = block;

        while(true) {
            int ret = read(fd, ptr, last);

            if(ret < 0) {
                // EAGAIN or EINTR continue
                if(EAGAIN == errno && EINTR == errno) {
                    continue;
                }

                // end stream: pipe close
                if(EBADF == errno) {
                    res.resize(res.size() - last);
                    break;
                }

                Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "read", strerror(errno), errno);
                res.clear();
                break;
            }

            // no data: continue
            if(ret == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            ptr += ret;
            last -= ret;

            if(last == 0) {
                auto pos = res.size();
                res.resize(res.size() + block);
                last = block;
                ptr = res.data() + pos;
            }
        }

        return res;
    }

    StatusStdout jobWaitStdout(int pid, int fd) {
        auto future = std::async(std::launch::async, &jobBlockRead, fd);
        int status = ForkMode::waitPid(pid);

        if(future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            // force stop jobBlockRead
            close(fd);
        }

        return std::make_pair(status, future.get());
    }

    PidStatusStdout runForkCommandStdout(const std::filesystem::path & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) {
        int pipefd[2] = {};

        if(0 > pipe(pipefd)) {
            Application::error("{}: {} failed, error: {}, code: {}",
                               __FUNCTION__, "pipe", strerror(errno), errno);
            throw std::runtime_error(NS_FuncNameS);
        }

        int pid = ForkMode::forkStart(pipefd[1]);

        // child
        if(0 == pid) {
            ForkMode::runChildProcess(cmd, args, envs, RedirectLog::StdoutFd, pipefd[1]);
            // child ended
        }

        // parent
        if(Application::isDebugLevel(DebugLevel::Debug)) {
            auto sargs = Tools::join(args.begin(), args.end(), ", ");
            auto senvs = Tools::join(envs.begin(), envs.end(), ", ");
            Application::info("{}: uid: {}, pid: {}, cmd: `{}', args: [ {} ], envs: [ {} ]",
                              __FUNCTION__, getuid(), pid, cmd, sargs, senvs);
        } else {
            Application::info("{}: uid: {}, pid: {}, cmd: `{}'", __FUNCTION__, getuid(), pid, cmd);
        }

        // main thread processed
        close(pipefd[1]);

        auto future = std::async(std::launch::async, &jobWaitStdout, pid, pipefd[0]);
        return std::make_pair(pid, std::move(future));
    }

    std::vector<uint8_t> readXauthFile(const std::filesystem::path & xauthFilePath, int displayNum) {
        std::vector<uint8_t> buf = Tools::fileToBinaryBuf(xauthFilePath);
        StreamBufRef sb(buf.data(), buf.size());
        uint16_t len = 0;

        while(sb.last()) {
            // format: 01 00 [ <host len:be16> [ host ]] [ <display len:be16> [ display ]] [ <magic len:be16> [ magic ]] [ <cookie len:be16> [ cookie ]]
            if(auto ver = sb.readIntBE16(); ver != 0x0100) {
                Application::error("{}: invalid xauth format, ver: {:#04x}", __FUNCTION__, ver);
                throw std::runtime_error(NS_FuncNameS);
            }

            len = sb.readIntBE16();
            auto host = sb.readString(len);

            len = sb.readIntBE16();
            auto display = sb.readString(len);

            len = sb.readIntBE16();
            auto magic = sb.readString(len);

            len = sb.readIntBE16();
            auto cookie = sb.read(len);

            if(display == std::to_string(displayNum)) {
                Application::debug(DebugType::App, "{}: {} found, display {}",
                                   __FUNCTION__, "xcb cookie", displayNum);
                return cookie;
            }
        }

        Application::error("{}: {} found, display: {}",
                           __FUNCTION__, "xcb cookie not", displayNum);

        throw std::runtime_error(NS_FuncNameS);
    }

    std::unique_ptr<XCB::Connector> waitX11DisplayStarting(int displayNum, const XCB::AuthCookie & mcookie, uint32_t ms) {
        std::unique_ptr<XCB::Connector> res;

        Tools::waitCallable<std::chrono::milliseconds>(ms, 100, [displayNum, auth = std::addressof(mcookie), &res]() {
            if(Tools::checkUnixSocket(Tools::x11UnixPath(displayNum))) {
                try {
                    if(res = std::make_unique<XCB::Connector>(displayNum, auth); !! res) {
                        return 0 == res->hasError();
                    }

                    res.reset();
                } catch(const std::exception &) {
                }
            }

            return false;
        });

        return res;
    }

    void clearSessionDbusAddress(int displayNum) {
        if(auto env = getenv("XDG_RUNTIME_DIR")) {
            auto dbusPath = std::filesystem::path{env} / "ltsm" / fmt::format("dbus_session_{}", displayNum);
            std::filesystem::remove(dbusPath);
        }
    }

    std::string waitSessionDbusAddress(int displayNum, uint32_t ms) {
        if(auto env = getenv("XDG_RUNTIME_DIR")) {
            // ltsm path from /etc/ltsm/xclients
            auto dbusPath = std::filesystem::path{env} / "ltsm" / fmt::format("dbus_session_{}", displayNum);
            std::string res;

            Tools::waitCallable<std::chrono::milliseconds>(ms, 100, [&dbusPath, &res]() {
                try {
                    if(std::filesystem::is_regular_file(dbusPath)) {
                        res = Tools::fileToString(dbusPath);
                        return ! res.empty();
                    }
                } catch(const std::exception &) {
                }

                return false;
            });

            return res;
        }

        Application::error("{}: {} not found", __FUNCTION__, "XDG_RUNTIME_DIR");
        return "";
    }

    // FreedesktopNotifications
    class FreedesktopNotifications : public SDBus::SessionProxy {
      public:
        FreedesktopNotifications() : SDBus::SessionProxy("org.freedesktop.Notifications",
                    "/org/freedesktop/Notifications", "org.freedesktop.Notifications") {}

        enum class IconType { Information, Warning, Error, Question };

        void notify(const std::string & applicationName, uint32_t replacesId, const IconType & iconType,
                    const std::string & summary, const std::string & body, const std::vector<std::string> & actions,
                    const std::map<std::string, sdbus::Variant> & hints, int32_t expirationTime) const {

            std::string notificationIcon("dialog-information");

            switch(iconType) {
                case IconType::Information:
                    break;

                case IconType::Warning:
                    notificationIcon.assign("dialog-error");
                    break;

                case IconType::Error:
                    notificationIcon.assign("dialog-warning");
                    break;

                case IconType::Question:
                    notificationIcon.assign("dialog-question");
                    break;
            }

            CallProxyMethodNoResult("Notify", applicationName, replacesId, notificationIcon, summary, body, actions, hints, expirationTime);
        }

        inline void notifyInfo(const std::string & summary, const std::string & body, int32_t expirationTime = -1) const {
            notify("LTSM", 0, IconType::Information, summary, body, {}, {}, expirationTime);
        }

        inline void notifyWarning(const std::string & summary, const std::string & body, int32_t expirationTime = -1) const {
            notify("LTSM", 0, IconType::Warning, summary, body, {}, {}, expirationTime);
        }

        inline void notifyError(const std::string & summary, const std::string & body, int32_t expirationTime = -1) const {
            notify("LTSM", 0, IconType::Error, summary, body, {}, {}, expirationTime);
        }

        inline void notifyQuestion(const std::string & summary, const std::string & body, int32_t expirationTime = -1) const {
            notify("LTSM", 0, IconType::Question, summary, body, {}, {}, expirationTime);
        }
    };

    // DBusAdaptor
    DBusAdaptor::DBusAdaptor(sdbus::IConnection & conn, Starter & starter)
#ifdef SDBUS_2_0_API
        : AdaptorInterfaces(conn, sdbus::ObjectPath {dbus_session_display_path}),
#else
        :
        AdaptorInterfaces(conn, dbus_session_display_path),
#endif
          starter_(starter) {
        registerAdaptor();
    }

    DBusAdaptor::~DBusAdaptor() {
        unregisterAdaptor();
    }

    int32_t DBusAdaptor::getVersion(void) {
        return LTSM_SESSION_DISPLAY_VERSION;
    }

    void DBusAdaptor::serviceShutdown(void) {
        starter_.dbusServiceShutdown();
    }

    void DBusAdaptor::setDebug(const std::string & level) {
        starter_.dbusSetDebug(level);
    }

    std::string DBusAdaptor::jsonStatus(void) {
        return starter_.dbusJsonStatus();
    }

    int32_t DBusAdaptor::runSessionCommandAsync(const std::string & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) {
        auto sargs = Tools::join(args.begin(), args.end(), ", ");
        Application::debug(DebugType::Dbus, "{}: args: [ {} ]", __FUNCTION__, sargs);

        try {
            if(auto pidStatus = runForkCommandStdout(cmd, args, envs); 0 < pidStatus.first) {
                int pid = pidStatus.first;
                starter_.storeChild(std::move(pidStatus));
                return pid;
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", __FUNCTION__, err.what());
        }

        return -1;
    }

    StatusStdout DBusAdaptor::runSessionCommandSync(const std::string& cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) {
        auto sargs = Tools::join(args.begin(), args.end(), ", ");
        Application::debug(DebugType::Dbus, "{}: args: [ {} ]", __FUNCTION__, sargs);

        try {
            if(auto pidStatus = runForkCommandStdout(cmd, args, envs); 0 < pidStatus.first) {
                return pidStatus.second.get();
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", __FUNCTION__, err.what());
        }

        return StatusStdout{ -1, {} };
    }

    StatusStdout DBusAdaptor::runSessionZenity(const std::vector<std::string> & args) {
        auto sargs = Tools::join(args.begin(), args.end(), ", ");
        Application::debug(DebugType::Dbus, "{}: args: [ {} ]", __FUNCTION__, sargs);

        auto zenityBin = starter_.configGetString("zenity:path", "/usr/bin/zenity");

        try {
            if(auto pidStatus = runForkCommandStdout(zenityBin, args, {}); 0 < pidStatus.first) {
                return pidStatus.second.get();
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", __FUNCTION__, err.what());
        }

        return StatusStdout{ -1, {} };
    }

    void DBusAdaptor::setSessionKeyboardLayout(const std::string & layout) {
        Application::debug(DebugType::Dbus, "{}: [ {} ]", __FUNCTION__, layout);

        try {
            if(auto pidStatus = runForkCommandStdout("/usr/bin/setxkbmap", { "-layout", layout, "-option", "\"\"" }, {}); 0 < pidStatus.first) {
                pidStatus.second.wait();
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", __FUNCTION__, err.what());
        }
    }

    void DBusAdaptor::notifyInfo(const std::string& summary, const std::string& body) {
        FreedesktopNotifications().notifyInfo(summary, body, 2000 /* ms */);
    }

    void DBusAdaptor::notifyWarning(const std::string& summary, const std::string& body) {
        FreedesktopNotifications().notifyWarning(summary, body, 2000 /* ms */);
    }

    void DBusAdaptor::notifyError(const std::string& summary, const std::string& body) {
        FreedesktopNotifications().notifyError(summary, body, 2000 /* ms */);
    }

    // Starter
    Starter::Starter(int displayNum, const char* xauthFile)
        : ApplicationJsonConfig("ltsm_session_display"), started_(std::chrono::system_clock::now()) {
        Application::info("service started, uid: {}, gid: {}, pid: {}, version: {}",
                                getuid(), getgid(), getpid(), LTSM_SESSION_DISPLAY_VERSION);
        startX11Display(displayNum, xauthFile);
    }

    Starter::~Starter() {
        if(timer1_) {
            timer1_->stop();
        }

        stopChilds();
    }

    void Starter::startX11Display(int displayNum, const char* xauthFile) {
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

        if(configHasKey("xvfb:path")) {
            xorgBin = configGetString("xvfb:path");
        } else if(std::filesystem::exists(ltsmXorg) && std::filesystem::exists(ltsmX11)) {
            xorgBin.assign(ltsmXorg);
        } else {
            xorgBin.assign(ltsmXvfb);
        }

        if(! std::filesystem::exists(xorgBin)) {
            Application::error("{}: path not found: `{}'", __FUNCTION__, xorgBin);
            throw std::runtime_error(NS_FuncNameS);
        }

        const bool useXorg = std::filesystem::path(xorgBin).filename() == "Xorg";

        // xorg args
        if(auto ja = config().getArray("xvfb:args")) {
            xorgArgs = ja->toStdVector<std::string>();
        } else {
            // default options for Xvfb/Xorg
            xorgArgs.emplace_back(":%{display}");
            xorgArgs.emplace_back("-nolisten");
            xorgArgs.emplace_back("tcp");

            if(useXorg) {
                xorgArgs.emplace_back("-config");
                xorgArgs.emplace_back("ltsm.conf");
                xorgArgs.emplace_back("-quiet");
            } else {
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

        for(auto & str : xorgArgs) {
            str = Tools::replace(str, "%{width}", defaultWidth_);
            str = Tools::replace(str, "%{height}", defaultHeight_);
            str = Tools::replace(str, "%{depth}", defaultDepth_);
            str = Tools::replace(str, "%{display}", displayNum_);
            str = Tools::replace(str, "%{authfile}", xauthFile);
        }

        // start Xorg
        pidXorg_ = runForkCommand(xorgBin, xorgArgs, {});
    }

    bool Starter::startX11Session(void) {
        // session bin
        std::string sessionBin = configGetString("session:path");
        std::vector<std::string> sessionArgs;
        std::vector<std::string> sessionEnvs;

        if(! std::filesystem::exists(sessionBin)) {
            Application::error("{}: path not found: `{}'", __FUNCTION__, sessionBin);
            return false;
        }

        // session args
        if(auto ja = config().getArray("session:args")) {
            sessionArgs = ja->toStdVector<std::string>();
        }

        auto xresources = std::filesystem::path{getenv("HOME")} / ".ltsm" / ".Xresources";
        std::filesystem::remove(xresources);

        if(getenv("LTSM_LOGIN_MODE")) {
            // helper login
            auto helperBin = configGetString("helper:path", "/usr/libexec/ltsm/LTSM_helper");

            if(! std::filesystem::exists(helperBin)) {
                Application::error("{}: path not found: `{}'", __FUNCTION__, helperBin);
                return false;
            }

            sessionEnvs.emplace_back(std::string("XSESSION=").append(helperBin));
        }
        else if(auto env = getenv("LTSM_CLIENT_OPTS")) {
            try {
                auto content = Tools::zlibUncompress(BinaryBuf(Tools::base64Decode(env)));
                auto jo = JsonContentString(std::string_view{(const char*) content.data(), content.size()}).toObject();
                // set session dpi
                if(auto dpi = jo.getInteger("x11:dpi", 0); 0 < dpi) {
                    std::ofstream ofs(xresources, std::ios::trunc);
                    ofs << "Xft.dpi: " << dpi << std::endl;
                }
            } catch(const std::exception & err) {
                Application::error("{}: exception: `{}'", __FUNCTION__, err.what());
            }
        }

        std::scoped_lock guard{ lockCommands_ };

        const char* xsetupBin = "/etc/ltsm/xsetup";

        if(std::filesystem::exists(xsetupBin)) {
            // wait xsetup stopped
            auto pidStatus = runForkCommand(xsetupBin, {}, {});
            pidStatus.second.wait();
        }

        // start Session
        pidSession_ = runForkCommand(sessionBin, sessionArgs, sessionEnvs);

        timer1_ = Tools::BaseTimer::create<std::chrono::milliseconds>(300, true,
                  std::bind(& Starter::checkChildCommandsComplete, this));

        return true;
    }

    void Starter::stopChilds(void) {
        std::scoped_lock guard{ lockCommands_ };

        if(0 < pidXorg_.first) {
            kill(pidXorg_.first, SIGTERM);
        }

        if(0 < pidSession_.first) {
            kill(pidSession_.first, SIGTERM);
        }

        if(! childCommands_.empty()) {
            for(auto & pidStatus : childCommands_) {
                kill(pidStatus.first, SIGTERM);
            }

            for(auto & pidStatus : childCommands_) {
                auto & statusStdout = pidStatus.second;
                statusStdout.wait();
            }

            childCommands_.clear();
        }
    }

    void Starter::childProcessEnded(int pid, StatusStdout statusStdout) {
        if(dbus_) {
            const int & wstatus = std::get<0>(statusStdout);
            dbus_->emitRunSessionCommandAsyncComplete(pid, static_cast<bool>(WIFEXITED(wstatus)), wstatus, std::get<1>(statusStdout));
        }
    }

    void Starter::checkChildCommandsComplete(void) {
        // check xorg shutdown
        if(pidXorg_.second.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
            pidXorg_.first = -1;
            sessionConn->leaveEventLoop();
            return;
        }

        // check session shutdown
        if(pidSession_.second.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
            pidSession_.first = -1;
            sessionConn->leaveEventLoop();
            return;
        }

        std::scoped_lock guard{ lockCommands_ };

        if(childCommands_.empty()) {
            return;
        }

        // timer job
        std::erase_if(childCommands_, [this](auto & pidStatus) {
            auto & futureStatus = pidStatus.second;

            if(futureStatus.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
                this->childProcessEnded(pidStatus.first, futureStatus.get());
                return true;
            }

            return false;
        });
    }

    int Starter::run(void) {
        const uint32_t x11Timeout = configGetInteger("xvfb:timeout", 3500);

        xcb_ = waitX11DisplayStarting(displayNum_, mcookie_, x11Timeout);

        if(! xcb_) {
            Application::error("{}: {} failed", __FUNCTION__, "X11 connect");
            return EXIT_FAILURE;
        }

        clearSessionDbusAddress(displayNum_);

        if(! startX11Session()) {
            Application::error("{}: {} failed", __FUNCTION__, "X11 session");
            return EXIT_FAILURE;
        }

        auto dbusAddress = waitSessionDbusAddress(displayNum_, x11Timeout);

        if(dbusAddress.empty()) {
            Application::error("{}: {} failed", __FUNCTION__, "dbus session");
            return EXIT_FAILURE;
        }

        setenv("DBUS_SESSION_BUS_ADDRESS", dbusAddress.c_str(), 1);

#ifdef SDBUS_2_0_API
        DisplaySession::sessionConn = sdbus::createSessionBusConnection(sdbus::ServiceName {dbus_session_display_name});
#else
        DisplaySession::sessionConn = sdbus::createSessionBusConnection(dbus_session_display_name);
#endif

        if(! DisplaySession::sessionConn) {
            Application::error("{}: dbus connection failed, uid: {}", __FUNCTION__, getuid());
            return EXIT_FAILURE;
        }

        signal(SIGTERM, signalHandler);
        signal(SIGINT, signalHandler);

        dbus_ = std::make_unique<DBusAdaptor>(*sessionConn, *this);

        // start main loop
        sessionConn->enterEventLoop();

        return EXIT_SUCCESS;
    }

    void Starter::dbusServiceShutdown(void) const {
        Application::debug(DebugType::Dbus, "{}: pid: {}", __FUNCTION__, getpid());
        sessionConn->leaveEventLoop();
    }

    void Starter::dbusSetDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", __FUNCTION__, level);
        setDebugLevel(level);
    }

    void Starter::storeChild(PidStatusStdout pidStatus) {
        std::scoped_lock guard{ lockCommands_ };
        childCommands_.emplace_front(std::move(pidStatus));
    }

    std::string Starter::dbusJsonStatus(void) const {
        JsonObjectStream jos;

        jos.push("display:num", displayNum_);
        jos.push("xorg:pid", pidXorg_.first);
        jos.push("session:pid", pidSession_.first);
        jos.push("running:sec", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - started_).count());

        return jos.flush();
    }
}

using namespace LTSM;

int main(int argc, char** argv) {
    const char* displayAddr = nullptr;
    const char* xauthFile = nullptr;

    for(int it = 1; it < argc; ++it) {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h")) {
            std::cout << "usage: " << argv[0] << " --display <addr> --xauth <file>" << std::endl;
            return EXIT_SUCCESS;
        } else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v")) {
            std::cout << "version: " << LTSM_SESSION_DISPLAY_VERSION << std::endl;
            return EXIT_SUCCESS;
        } else if((0 == std::strcmp(argv[it], "--display") || 0 == std::strcmp(argv[it], "-d")) && it + 1 < argc) {
            displayAddr = argv[it + 1];
            it += 1;
        } else if((0 == std::strcmp(argv[it], "--xauth") || 0 == std::strcmp(argv[it], "-x")) && it + 1 < argc) {
            xauthFile = argv[it + 1];
            it += 1;
        }
    }

    if(! displayAddr || displayAddr[0] != ':') {
        std::cerr << "invalid display addr" << std::endl;
        return EXIT_FAILURE;
    }

    if(! xauthFile || ! std::filesystem::exists(xauthFile)) {
        std::cerr << "xautfile not found" << std::endl;
        return EXIT_FAILURE;
    }

    if(0 == getuid()) {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    if(auto home = getenv("HOME")) {
        auto ltsmDir = std::filesystem::path{home} / ".ltsm";

        if(! std::filesystem::is_directory(ltsmDir)) {
            std::filesystem::create_directory(ltsmDir);
        }
    } else {
        Application::error("{}: {} not found", __FUNCTION__, "HOME");
        return EXIT_FAILURE;
    }

    setenv("DISPLAY", displayAddr, 1);
    setenv("XAUTHORITY", xauthFile, 1);

    try {
        int displayNum = std::stoi(displayAddr + 1);
        return DisplaySession::Starter(displayNum, xauthFile).run();
    } catch(const sdbus::Error & err) {
        Application::error("sdbus: [{}] {}", err.getName(), err.getMessage());
    } catch(const std::exception & err) {
        Application::error("exception: {}", err.what());
    }

    return EXIT_FAILURE;
}
