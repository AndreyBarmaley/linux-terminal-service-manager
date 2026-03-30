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

#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <filesystem>

#if BOOST_VERSION >= 108700
#include <boost/process/v1/environment.hpp>
#else
#include <boost/process.hpp>
#include <boost/process/environment.hpp>
#endif

#include "ltsm_zlib.h"
#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_sdbus_proxy.h"
#include "ltsm_byte_stream.h"
#include "ltsm_display_session.h"

using namespace std::chrono_literals;

namespace LTSM::DisplaySession {
    std::vector<uint8_t> readXauthFile(const std::filesystem::path & xauthFilePath, int displayNum) {
        std::ifstream ifs(xauthFilePath);
        byte::istream bs(ifs);

        while(ifs) {
            // format: 01 00 [ <host len:be16> [ host ]] [ <display len:be16> [ display ]] [ <magic len:be16> [ magic ]] [ <cookie len:be16> [ cookie ]]
            if(auto ver = bs.read_be16(); ver != 0x0100) {
                Application::error("{}: invalid xauth format, ver: {:#06x}", __FUNCTION__, ver);
                throw std::runtime_error(NS_FuncNameS);
            }

            auto len = bs.read_be16();
            auto host = bs.read_string(len);

            len = bs.read_be16();
            auto display = bs.read_string(len);

            len = bs.read_be16();
            auto magic = bs.read_string(len);

            len = bs.read_be16();
            auto cookie = bs.read_bytes(len);

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

    bool waitX11DisplayStarting(int displayNum, const XCB::AuthCookie & mcookie, uint32_t ms) {
        return Tools::waitCallable<std::chrono::milliseconds>(ms, 100, [displayNum, auth = std::addressof(mcookie)]() {
            if(Tools::checkUnixSocket(Tools::x11UnixPath(displayNum))) {
                try {
                    if(auto res = std::make_unique<XCB::Connector>(displayNum, auth)) {
                        return 0 == res->hasError();
                    }
                } catch(const std::exception &) {
                }
            }

            return false;
        });
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

    X11Session::X11Session(int displayNum, const char* xauthFile, bool debug)
        : ApplicationJsonConfig("ltsm_session_display"),
          xauth_file_{xauthFile}, mcookie_{readXauthFile(xauthFile, displayNum)}, display_num_{displayNum} {

        if(debug) {
            setDebugLevel(DebugLevel::Debug);
        }

        if(! startX11Display()) {
            throw std::runtime_error(NS_FuncNameS);
        }

        const uint32_t x11Timeout = configGetInteger("xvfb:timeout", 3500);

        if(! waitX11DisplayStarting(display_num_, mcookie_, x11Timeout)) {
            Application::error("{}: {} failed", __FUNCTION__, "X11 connect");
            throw std::runtime_error(NS_FuncNameS);
        }

        clearSessionDbusAddress(display_num_);

        if(! startX11Session()) {
            Application::error("{}: {} failed", __FUNCTION__, "X11 session");
            throw std::runtime_error(NS_FuncNameS);
        }

        dbus_address_ = waitSessionDbusAddress(display_num_, x11Timeout);

        if(dbus_address_.empty()) {
            Application::error("{}: {} failed", __FUNCTION__, "dbus session");
            throw std::runtime_error(NS_FuncNameS);
        }

        setenv("DBUS_SESSION_BUS_ADDRESS", dbus_address_.c_str(), 1);

#ifdef SDBUS_2_0_API
        dbus_conn_ = sdbus::createSessionBusConnection(sdbus::ServiceName {dbus_session_display_name});
#else
        dbus_conn_ = sdbus::createSessionBusConnection(dbus_session_display_name);
#endif
    }

    bool X11Session::startX11Display(void) {
        default_width_ = configGetInteger("default:width", 1280);
        default_height_ = configGetInteger("default:height", 1024);
        default_depth_ = configGetInteger("default:depth", 24);

        std::string xorgBin;
        ArgsList xorgArgs;

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
            return false;
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
            str = Tools::replace(str, "%{width}", default_width_);
            str = Tools::replace(str, "%{height}", default_height_);
            str = Tools::replace(str, "%{depth}", default_depth_);
            str = Tools::replace(str, "%{display}", display_num_);
            str = Tools::replace(str, "%{authfile}", xauth_file_);
        }

        // start Xorg
        ps_xorg_ = SessionProcess(xorgBin, xorgArgs);

        return true;
    }

    bool X11Session::startX11Session(void) {
        // session bin
        std::string sessionBin = configGetString("session:path");
        ArgsList sessionArgs;

        bp::environment sessionEnvs = boost::this_process::environment();

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
            auto helperBin = configGetString("helper:path", "/usr/libexec/ltsm/ltsm_helper");

            if(! std::filesystem::exists(helperBin)) {
                Application::error("{}: path not found: `{}'", __FUNCTION__, helperBin);
                return false;
            }

            sessionEnvs["XSESSION"] = helperBin;
        } else if(auto env = getenv("LTSM_CLIENT_OPTS")) {
            try {
                auto content = Tools::zlibUncompress(Tools::base64Decode(env));
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

        // start Session
        ps_sess_ = SessionProcess(sessionBin, sessionArgs, sessionEnvs);

        return true;
    }


    // DBusAdaptor
    DBusAdaptor::DBusAdaptor(int displayNum, const char* xauthFile, bool debug)
        : X11Session(displayNum, xauthFile, debug),
#ifdef SDBUS_2_0_API
          AdaptorInterfaces(*dbus_conn_, sdbus::ObjectPath {dbus_session_display_path}),
#else
          AdaptorInterfaces(*dbus_conn_, dbus_session_display_path),
#endif
          started_(std::chrono::system_clock::now()), signals_ {ioc_}, timer_childs_ {ioc_} {
        registerAdaptor();
    }

    DBusAdaptor::~DBusAdaptor() {
        unregisterAdaptor();
        stop();
    }

    int32_t DBusAdaptor::getVersion(void) {
        return LTSM_SESSION_DISPLAY_VERSION;
    }

    void DBusAdaptor::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "{}: pid: {}", __FUNCTION__, getpid());
        stop();
    }

    void DBusAdaptor::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", __FUNCTION__, level);
        setDebugLevel(level);
    }

    std::string DBusAdaptor::jsonStatus(void) {
        JsonObjectStream jos;

        jos.push("display:num", displayNum());
        jos.push("xorg:pid", pidXorg());
        jos.push("session:pid", pidSession());
        jos.push("running:sec", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - started_).count());

        return jos.flush();
    }

    int32_t DBusAdaptor::runSessionCommandAsync(const std::string & cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) {
        Application::debug(DebugType::Dbus, "{}: cmd: {}, args: [{}]", __FUNCTION__, cmd, Tools::join(args, ", "));

        bp::environment env = boost::this_process::environment();

        for(auto & str : envs) {
            if(auto pos = str.find("="); pos != std::string::npos) {
                env[str.substr(0, pos)] = str.substr(pos + 1);
            }
        }

        try {
            std::scoped_lock guard{ lock_childs_ };
            childs_.emplace_back(bp::child(cmd, args, envs));
            return childs_.back().id();

        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", __FUNCTION__, err.what());
        }

        return -1;
    }

    StatusStdout DBusAdaptor::runSessionCommandSync(const std::string& cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) {
        Application::debug(DebugType::Dbus, "{}: cmd: {}, args: [{}]", __FUNCTION__, cmd, Tools::join(args, ", "));

        bp::environment env = boost::this_process::environment();

        for(auto & str : envs) {
            if(auto pos = str.find("="); pos != std::string::npos) {
                env[str.substr(0, pos)] = str.substr(pos + 1);
            }
        }

        try {
            bp::ipstream ips;
            auto proc = bp::child(cmd, args, env, bp::std_out > ips);

            StdoutBuf res{std::istreambuf_iterator<char>(ips),
                          std::istreambuf_iterator<char>()};

            proc.wait();
            return StatusStdout{proc.exit_code(), std::move(res)};

        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", __FUNCTION__, err.what());
        }

        return StatusStdout{ -1, {} };
    }

    StatusStdout DBusAdaptor::runSessionZenity(const std::vector<std::string> & args) {
        auto zenityBin = configGetString("zenity:path", "/usr/bin/zenity");
        return runSessionCommandSync(zenityBin, args, {});
    }

    void DBusAdaptor::setSessionKeyboardLayout(const std::string & layout) {
        Application::debug(DebugType::Dbus, "{}: layout: {}", __FUNCTION__, layout);
        runSessionCommandSync("/usr/bin/setxkbmap", { "-layout", layout, "-option", "\"\"" }, {});
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

    void DBusAdaptor::timerChildsAliveCheck(const boost::system::error_code& ec) {
        if(ec) {
            return;
        }

        // xorg stopped
        if(ps_xorg_.isValid() && ! ps_xorg_.isRunning()) {
            Application::warning("{}: {} exited, pid: {}, session shutdown", __FUNCTION__, "xorg", ps_xorg_.pid());
            boost::asio::post(ioc_, std::bind(&DBusAdaptor::stop, this));
            return;
        }

        // session stopped
        if(ps_sess_.isValid() && ! ps_sess_.isRunning()) {
            Application::warning("{}: {} exited, pid: {}, session shutdown", __FUNCTION__, "session", ps_sess_.pid());
            boost::asio::post(ioc_, std::bind(&DBusAdaptor::stop, this));
            return;
        }

        auto removeChildsEnded = [this]() {
            std::scoped_lock guard{ lock_childs_ };
            auto ended = std::ranges::remove_if(childs_, [](auto & ps) {
                return ! ps.valid() || ! ps.running();
            });

            if(! ended.empty()) {
                std::error_code ec;

                for(auto & ps : ended) {
                    ps.wait(ec);
                }

                childs_.erase(ended.begin(), ended.end());
            }
        };

        removeChildsEnded();

        timer_childs_.expires_after(dur_childs_);
        timer_childs_.async_wait(std::bind(&DBusAdaptor::timerChildsAliveCheck, this, std::placeholders::_1));
    }

    void DBusAdaptor::stop(void) {
        dbus_conn_->leaveEventLoop();

        signals_.cancel();
        timer_childs_.cancel();

        if(ps_xorg_.isRunning()) {
            kill(ps_xorg_.pid(), SIGTERM);
        }

        if(ps_sess_.isRunning()) {
            kill(ps_sess_.pid(), SIGTERM);
        }

        std::scoped_lock guard{ lock_childs_ };

        for(auto & ps : childs_) {
            if(ps.valid() && ps.running()) {
                kill(ps.id(), SIGTERM);
                ps.wait();
            }
        }

        childs_.clear();
    }

    int DBusAdaptor::start(void) {

        Application::info("service started, uid: {}, gid: {}, pid: {}, version: {}",
                          getuid(), getgid(), getpid(), LTSM_SESSION_DISPLAY_VERSION);

        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        signals_.async_wait([this](const boost::system::error_code & ec, int signal) {
            // skip canceled
            if(ec != boost::asio::error::operation_aborted && (signal == SIGTERM || signal == SIGINT)) {
                this->stop();
            }
        });

        timer_childs_.expires_after(dur_childs_);
        timer_childs_.async_wait(std::bind(&DBusAdaptor::timerChildsAliveCheck, this, std::placeholders::_1));

        auto sdbus_job = std::thread([this]() {
           try {
                dbus_conn_->enterEventLoop();
            } catch(const std::exception & err) {
                Application::error("sdbus exception: {}", err.what());
                boost::asio::post(ioc_, std::bind(&DBusAdaptor::stop, this));
            }
        });

        ioc_.run();

        dbus_conn_->leaveEventLoop();
        sdbus_job.join();

        Application::notice("{}: Display session shutdown", __FUNCTION__);

        return EXIT_SUCCESS;
    }
}

using namespace LTSM;

int main(int argc, char** argv) {
    const char* displayAddr = nullptr;
    const char* xauthFile = nullptr;
    bool debug = false;

    for(int it = 1; it < argc; ++it) {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h")) {
            std::cout << "usage: " << argv[0] << " --display <addr> --xauth <file> [--debug] [--version]" << std::endl;
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
        } else if(0 == std::strcmp(argv[it], "--debug") || 0 == std::strcmp(argv[it], "-d")) {
            debug = true;
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
        return DisplaySession::DBusAdaptor(displayNum, xauthFile, debug).start();
    } catch(const sdbus::Error & err) {
        Application::error("sdbus: [{}] {}", err.getName(), err.getMessage());
    } catch(const std::exception & err) {
        Application::error("exception: {}", err.what());
    }

    return EXIT_FAILURE;
}
