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

//#include <boost/process/v2.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include "ltsm_zlib.h"
#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_sdbus_proxy.h"
#include "ltsm_display_session.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM::DisplaySession {
/*
    namespace bp2 = boost::process::v2;

    template<typename... Args>
    class SessionProcessV2 {
        std::string filename_;
                
        std::optional<bp2::process> proc_;
        std::optional<asio::readable_pipe> pipe_out_;
        std::optional<asio::readable_pipe> pipe_err_;
                
      public:
        SessionProcessV2() = default;
                
        SessionProcessV2(SessionProcessV2 &&) noexcept = default;
        SessionProcessV2 & operator=(SessionProcessV2 &&) noexcept = default;

        SessionProcessV2(const asio::any_io_executor& ex, const std::string & cmd, const Args&... args)
            : filename_(std::filesystem::path(cmd).filename()) {
            pipe_out_.emplace(ex);
            pipe_err_.emplace(ex);

            std::vector<std::string> process_args = { args... };
            bp2::process_stdio redirect{ .in = nullptr,  .out = *pipe_out_,  .err = *pipe_err_ };

            auto exe_path = bp2::environment::find_executable(cmd);

            if(exe_path.empty()) {
                exe_path = cmd;
            }
            
            proc_.emplace(ex, exe_path, process_args, std::move(redirect));
        }
        
        ~SessionProcessV2() = default;

        asio::awaitable<void> waitAndLogging(void) noexcept {
            try {
                if(! proc_ || ! pipe_out_ || ! pipe_err_) {
                    co_return;
                }
                    
                std::string str_out;
                std::string str_err;
                
                auto buffer_out = asio::dynamic_buffer(str_out);
                auto buffer_err = asio::dynamic_buffer(str_err);

                auto read_stdout = [&]() -> asio::awaitable<void> {
                    try {
                        co_await asio::async_read(*pipe_out_, buffer_out, asio::use_awaitable);
                    } catch(const system::system_error& err) {
                        if(err.code() != asio::error::eof) {
                            throw;
                        }
                    }
                    co_return;
                };

                auto read_stderr = [&]() -> asio::awaitable<void> {
                    try {
                        co_await asio::async_read(*pipe_err_, buffer_err, asio::use_awaitable);
                    } catch(const system::system_error& err) {
                        if(err.code() != asio::error::eof) {
                            throw;
                        }
                    }
                    co_return;
                };

                auto wait_proc = [&]() -> asio::awaitable<void> {
                    auto [ec, exit_code] = co_await proc_->async_wait(asio::use_awaitable);
                    if(ec) {
                        throw system::system_error(ec);
                    }
                    co_return;
                };

                using namespace asio::experimental::awaitable_operators;
                co_await (read_stdout() && read_stderr() && wait_proc());

                auto log_dir = std::filesystem::path{"/tmp"} / ".ltsm" / "log";

                if(auto home = getenv("HOME")) {
                    log_dir = std::filesystem::path{home} / ".ltsm" / "log";
                }
                    
                if(! std::filesystem::is_directory(log_dir)) {
                    std::filesystem::create_directories(log_dir);
                }

                auto log_file_out = log_dir / filename_;
                log_file_out.replace_extension(".out");
                std::ofstream(log_file_out) << str_out;

                auto log_file_err = log_dir / filename_;
                log_file_err.replace_extension(".err");
                std::ofstream(log_file_err) << str_err;

            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            }
        }
            
        int pid(void) const {
            return proc_ ? proc_->id() : -1;
        }

        bool isValid(void) const {
            return proc_.has_value();
        }

        bool isRunning(void) const {
            if(! proc_) {
                return false;
            }

            return const_cast<bp2::process &>(*proc_).running();
        }
    };
*/
    asio::awaitable<void> waitSocketConnectAwait(std::filesystem::path file) {
        if(std::filesystem::is_socket(file)) {
            co_return;
        }

        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer tm_pause{ex};

        while(! std::filesystem::is_socket(file)) {
            tm_pause.expires_after(100ms);
            co_await tm_pause.async_wait(asio::use_awaitable);
        }

        asio::local::stream_protocol::socket sock{ex};
        co_await sock.async_connect(asio::local::stream_protocol::endpoint{file.string()}, asio::use_awaitable);

        co_return;
    }

    asio::awaitable<void> waitSocketTimeoutAwait(std::filesystem::path file, std::chrono::milliseconds deadline_ms) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer tm_deadline{ex, deadline_ms};

        using namespace asio::experimental::awaitable_operators;
        auto results = co_await (waitSocketConnectAwait(file) || tm_deadline.async_wait(asio::use_awaitable));

        if(results.index() == 0) {
            tm_deadline.cancel();
            co_return;
        }
        
        Application::error("{}: deadline, path: {}", NS_FuncNameV, file.string());
        throw std::system_error(std::make_error_code(std::errc::timed_out), file.string());
    }

/*
    template<typename Buffer>
    asio::awaitable<Buffer> readFileAwait(std::filesystem::path file) {
        auto ex = co_await asio::this_coro::executor;
        asio::stream_file stream_file{ex, file.string(), asio::stream_file::read_only};

        Buffer content;
        auto buffer = asio::dynamic_buffer(content);

        try {
            co_await asio::async_read(stream_file, buffer, asio::use_awaitable);
        } catch (const system::system_error& err) {
            if (err.code() != asio::error::eof) {
                throw;
            }
        }

        co_return content;
    }
*/

    template<typename Buffer>
    asio::awaitable<Buffer> readFileAwait(std::filesystem::path file) {
        auto ex = co_await asio::this_coro::executor;

        int fd = open(file.c_str(), O_RDONLY | O_NONBLOCK);
        if(fd < 0) {
            if(errno == ENOENT) {
                throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory), file.string());
            }
            throw std::system_error(errno,  std::generic_category(),  file.string());
        }

        asio::posix::stream_descriptor sd{ex, fd};

        Buffer content;
        auto buffer = asio::dynamic_buffer(content);

        try {
            co_await asio::async_read(sd, buffer, asio::use_awaitable);
        } catch (const system::system_error& err) {
            if (err.code() != asio::error::eof) {
                throw;
            }
        }

        co_return content;
    }

    asio::awaitable<void> waitFileAwait(std::filesystem::path file) {
        if(std::filesystem::is_regular_file(file)) {
            co_return;
        }

        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer tm_pause{ex};

        while(! std::filesystem::is_regular_file(file) ||
            0 == std::filesystem::file_size(file)) {
            tm_pause.expires_after(100ms);
            co_await tm_pause.async_wait(asio::use_awaitable);
        }

        co_return;
    }

    asio::awaitable<void> waitFileTimeoutAwait(std::filesystem::path file, std::chrono::milliseconds deadline_ms) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer tm_deadline{ex, deadline_ms};

        using namespace asio::experimental::awaitable_operators;
        auto results = co_await (waitFileAwait(file) || tm_deadline.async_wait(asio::use_awaitable));

        if(results.index() == 0) {
            tm_deadline.cancel();
            co_return;
        }
        
        Application::error("{}: deadline, path: {}", NS_FuncNameV, file.string());
        throw std::system_error(std::make_error_code(std::errc::timed_out), file.string());
    }

    void clearSessionDbusAddress(int displayNum) {
        if(auto env = getenv("XDG_RUNTIME_DIR")) {
            auto dbusPath = std::filesystem::path{env} / "ltsm" / fmt::format("dbus_session_{}", displayNum);
            std::filesystem::remove(dbusPath);
        }
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

    asio::awaitable<BinaryBuf> readXauthFileAwait(std::filesystem::path xauth_file, int display_num) {
        auto buf = co_await readFileAwait<BinaryBuf>(xauth_file);
        StreamBufRef sb(buf.data(), buf.size());

        while(sb.last()) {
            // format: 01 00 [ <host len:be16> [ host ]] [ <display len:be16> [ display ]] [ <magic len:be16> [ magic ]] [ <cookie len:be16> [ cookie ]]
            if(auto ver = sb.readIntBE16(); ver != 0x0100) {
                Application::error("{}: invalid xauth format, ver: {:#06x}", NS_FuncNameV, ver);
                throw std::runtime_error(NS_FuncNameS);
            }

            auto len = sb.readIntBE16();
            auto host = sb.readString(len);

            len = sb.readIntBE16();
            auto display = sb.readString(len);

            len = sb.readIntBE16();
            auto magic = sb.readString(len);

            len = sb.readIntBE16();
            auto cookie = sb.read(len);

            if(display == std::to_string(display_num)) {
                Application::debug(DebugType::App, "{}: {} found, display {}",
                                   NS_FuncNameV, "xcb cookie", display_num);
                co_return cookie;
            }
        }

        Application::error("{}: {} found, display: {}",
                           NS_FuncNameV, "xcb cookie not", display_num);

        throw std::runtime_error(NS_FuncNameS);
    }

    asio::awaitable<X11Display> startDisplayAwait(const ApplicationJsonConfig & json, int displayNum, const char* xauthFile) {
        X11Display res;

        res.xauth_file_ = xauthFile;
        res.display_num_ = displayNum;
        res.mcookie_ = co_await readXauthFileAwait(xauthFile, displayNum);
    
        res.default_width_ = json.configGetInteger("default:width", 1280);
        res.default_height_ = json.configGetInteger("default:height", 1024);
        res.default_depth_ = json.configGetInteger("default:depth", 24);

        switch(res.default_depth_) {
            case 32:
            // xorg supported: 30, 24, 16, 15, 8
            res.default_depth_ = 30;
            break;

            case 30:
            case 24:
            case 16:
            case 15:
            break;

            default:
            Application::warning("{}: {} failed: {}", NS_FuncNameV, "default:depth", res.default_depth_);
            res.default_depth_ = 24;
            break;
        }

        std::string xorgBin;
        ArgsList xorgArgs;

        const char* ltsmX11 = "/etc/X11/ltsm.conf";
        const char* ltsmXorg = "/usr/bin/Xorg";
        const char* ltsmXvfb = "/usr/bin/Xvfb";

        if(json.configHasKey("xvfb:path")) {
            xorgBin = json.configGetString("xvfb:path");
        } else if(std::filesystem::exists(ltsmXorg) && std::filesystem::exists(ltsmX11)) {
            xorgBin.assign(ltsmXorg);
        } else {
            xorgBin.assign(ltsmXvfb);
        }

        if(! std::filesystem::exists(xorgBin)) {
            Application::error("{}: path not found: `{}'", NS_FuncNameV, xorgBin);
            throw std::runtime_error(NS_FuncNameS);
        }

        const bool useXorg = std::filesystem::path(xorgBin).filename() == "Xorg";

        // xorg args
        if(auto ja = json.config().getArray("xvfb:args")) {
            xorgArgs = ja->toStdVector<std::string>();
        } else {
            // default options for Xvfb/Xorg
            xorgArgs.emplace_back(":%{display}");
            xorgArgs.emplace_back("-nolisten");
            xorgArgs.emplace_back("tcp");

            if(useXorg) {
                xorgArgs.emplace_back("-config");
                xorgArgs.emplace_back("ltsm.conf");
                xorgArgs.emplace_back("-depth");
                xorgArgs.emplace_back("%{depth}");
                xorgArgs.emplace_back("-quiet");
            } else {
                xorgArgs.emplace_back("-screen");
                xorgArgs.emplace_back("0");
                xorgArgs.emplace_back("%{width}x%{height}x%{depth}");
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
            str = Tools::replace(str, "%{width}", res.default_width_);
            str = Tools::replace(str, "%{height}", res.default_height_);
            str = Tools::replace(str, "%{depth}", res.default_depth_);
            str = Tools::replace(str, "%{display}", res.display_num_);
            str = Tools::replace(str, "%{authfile}", res.xauth_file_);
        }

        // start Xorg
        res.ps_xorg_ = SessionProcess(xorgBin, xorgArgs);

        // wait started
        const uint32_t deadline_ms = json.configGetInteger("xvfb:timeout", 3500);
        auto socket_path = Tools::x11UnixPath(res.display_num_);

        co_await waitSocketTimeoutAwait(socket_path, std::chrono::milliseconds(deadline_ms));
        Application::info("{}: display: {}, pid: {}, socket: {}", NS_FuncNameV, displayNum, res.ps_xorg_.pid(), socket_path);

        co_return res;
    }

    asio::awaitable<std::string> waitSessionDbusAddressAwait(int displayNum, uint32_t deadline_ms) {
        if(auto env = getenv("XDG_RUNTIME_DIR")) {
            // ltsm path from /etc/ltsm/xclients
            auto dbusPath = std::filesystem::path{env} / "ltsm" / fmt::format("dbus_session_{}", displayNum);
            std::string res;

            co_await waitFileTimeoutAwait(dbusPath, std::chrono::milliseconds(deadline_ms));
            auto dbusAddress = co_await readFileAwait<std::string>(dbusPath);
            // remove endl
            dbusAddress.erase(
                std::find_if(dbusAddress.rbegin(), dbusAddress.rend(), [](auto ch) {
                    return std::isprint(ch);
                }).base(), dbusAddress.end());
            co_return dbusAddress;
        }

        Application::error("{}: env not found: {}", NS_FuncNameV, "XDG_RUNTIME_DIR");
        throw std::runtime_error(NS_FuncNameS);
    }

    asio::awaitable<X11SessionBase> startSessionAwait(const ApplicationJsonConfig & json, int displayNum) {
        // session bin
        std::string sessionBin = json.configGetString("session:path");
        ArgsList sessionArgs;

        bp::environment sessionEnvs = this_process::environment();

        if(! std::filesystem::exists(sessionBin)) {
            Application::error("{}: path not found: `{}'", NS_FuncNameV, sessionBin);
            throw std::runtime_error(NS_FuncNameS);
        }

        // session args
        if(auto ja = json.config().getArray("session:args")) {
            sessionArgs = ja->toStdVector<std::string>();
        }

        auto xresources = std::filesystem::path{getenv("HOME")} / ".ltsm" / ".Xresources";
        std::filesystem::remove(xresources);

        if(getenv("LTSM_LOGIN_MODE")) {
            // helper login
            auto helperBin = json.configGetString("helper:path", "/usr/libexec/ltsm/ltsm_helper");

            if(! std::filesystem::exists(helperBin)) {
                Application::error("{}: path not found: `{}'", NS_FuncNameV, helperBin);
                throw std::runtime_error(NS_FuncNameS);
            }

            sessionEnvs["XSESSION"] = helperBin;
        } else if(auto env = getenv("LTSM_CLIENT_OPTS")) {
            auto content = Tools::zlibUncompress(Tools::base64Decode(env));
            auto jo = JsonContentString(std::string_view{(const char*) content.data(), content.size()}).toObject();

            // set session dpi
            if(auto dpi = jo.getInteger("x11:dpi", 0); 0 < dpi) {
                std::ofstream ofs(xresources, std::ios::trunc);
                ofs << "Xft.dpi: " << dpi << std::endl;
            }
        }

        X11SessionBase res;
        // start Session
        res.ps_sess_ = SessionProcess(sessionBin, sessionArgs, sessionEnvs);

        // wait dbus
        const uint32_t deadline_ms = json.configGetInteger("xvfb:timeout", 3500);
        res.dbus_address_ = co_await waitSessionDbusAddressAwait(displayNum, deadline_ms);

        Application::info("{}: display: {}, pid: {}, dbus address: `{}'", NS_FuncNameV, displayNum, res.ps_sess_.pid(), res.dbus_address_);

        co_return res;
    }

    X11Session::X11Session(ApplicationJsonConfig&& config, X11Display&& xorg, X11SessionBase&& sess, bool debug)
        : ApplicationJsonConfig(std::move(config)), X11Display(std::move(xorg)), X11SessionBase(std::move(sess)) {

        if(debug) {
            setDebugLevel(DebugLevel::Debug);
        }

        setenv("DBUS_SESSION_BUS_ADDRESS", dbus_address_.c_str(), 1);

#ifdef SDBUS_2_0_API
        dbus_conn_ = sdbus::createSessionBusConnection(sdbus::ServiceName {dbus_session_display_name});
#else
        dbus_conn_ = sdbus::createSessionBusConnection(dbus_session_display_name);
#endif
    }

    // DBusAdaptor
    DBusAdaptor::DBusAdaptor(const boost::asio::any_io_executor& ex, ApplicationJsonConfig&& config, X11Display&& xorg, X11SessionBase&& sess, bool debug)
        : X11Session(std::move(config), std::move(xorg), std::move(sess), debug),
#ifdef SDBUS_2_0_API
          AdaptorInterfaces(*dbus_conn_, sdbus::ObjectPath {dbus_session_display_path}),
#else
          AdaptorInterfaces(*dbus_conn_, dbus_session_display_path),
#endif
          started_(std::chrono::system_clock::now()), childs_strand_{ex} {
        registerAdaptor();
    }

    DBusAdaptor::~DBusAdaptor() {
        unregisterAdaptor();
    }

    int32_t DBusAdaptor::getVersion(void) {
        return LTSM_SESSION_DISPLAY_VERSION;
    }

    void DBusAdaptor::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "{}: pid: {}", NS_FuncNameV, getpid());
        stop();
    }

    void DBusAdaptor::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", NS_FuncNameV, level);
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
        Application::debug(DebugType::Dbus, "{}: cmd: {}, args: [{}]", NS_FuncNameV, cmd, Tools::join(args, ", "));

        bp::environment env = this_process::environment();

        for(auto & str : envs) {
            if(auto pos = str.find("="); pos != std::string::npos) {
                env[str.substr(0, pos)] = str.substr(pos + 1);
            }
        }

        try {
            bp::child proc(cmd, args, envs);
            auto pid = proc.id();

            asio::post(childs_strand_, [this, child=std::move(proc)]() mutable {
                childs_.emplace_back(std::move(child));
            });
            return pid;

        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        }

        return -1;
    }

    StatusStdout DBusAdaptor::runSessionCommandSync(const std::string& cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) {
        Application::debug(DebugType::Dbus, "{}: cmd: {}, args: [{}]", NS_FuncNameV, cmd, Tools::join(args, ", "));

        bp::environment env = this_process::environment();

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
            LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        }

        return StatusStdout{ -1, {} };
    }

    StatusStdout DBusAdaptor::runSessionZenity(const std::vector<std::string> & args) {
        auto zenityBin = configGetString("zenity:path", "/usr/bin/zenity");
        return runSessionCommandSync(zenityBin, args, {});
    }

    void DBusAdaptor::setSessionKeyboardLayout(const std::string & layout) {
        Application::debug(DebugType::Dbus, "{}: layout: {}", NS_FuncNameV, layout);
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

    asio::awaitable<void> DBusAdaptor::childsAliveChecker(void) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer tm_pause{ex};

        auto removeChildsEndedCb = [this]() {
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

        try {
            for(;;) {
                tm_pause.expires_after(dur_childs_);
                co_await tm_pause.async_wait(asio::use_awaitable);

                // xorg stopped
                if(ps_xorg_.isValid() && ! ps_xorg_.isRunning()) {
                    Application::warning("{}: {} exited, pid: {}, session shutdown", NS_FuncNameV, "xorg", ps_xorg_.pid());
                    asio::post(ex, std::bind(&DBusAdaptor::stop, this));
                    co_return;
                }

                // session stopped
                if(ps_sess_.isValid() && ! ps_sess_.isRunning()) {
                    Application::warning("{}: {} exited, pid: {}, session shutdown", NS_FuncNameV, "session", ps_sess_.pid());
                    asio::post(ex, std::bind(&DBusAdaptor::stop, this));
                    co_return;
                }

                asio::post(childs_strand_, removeChildsEndedCb);
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            }
        }
    }

    void DBusAdaptor::stop(void) noexcept {
        std::call_once(stop_flag_, [self=shared_from_this()]() {
            try {
                self->stopContexts();
            } catch(const std::exception&) {
            }
        });
    }

    void DBusAdaptor::stopContexts(void) {
        dbus_conn_->leaveEventLoop();

        signals_cancel_.emit(asio::cancellation_type::terminal);
        childs_cancel_.emit(asio::cancellation_type::terminal);

        if(ps_xorg_.isRunning()) {
            kill(ps_xorg_.pid(), SIGTERM);
        }

        if(ps_sess_.isRunning()) {
            kill(ps_sess_.pid(), SIGTERM);
        }

        ps_xorg_.waitAndLogging();
        ps_sess_.waitAndLogging();

        for(auto & ps: childs_) {
            if(ps.valid() && ps.running()) {
                kill(ps.id(), SIGTERM);
                ps.wait();
            }
        }

        childs_.clear();

        if(sdbus_job_.joinable()) {
            sdbus_job_.join();
        }

        Application::notice("{}: Display session shutdown", NS_FuncNameV);
    }

    asio::awaitable<void> DBusAdaptor::signalsHandler(void) {
        auto ex = co_await asio::this_coro::executor;
        asio::signal_set signals{ex, SIGTERM, SIGINT};

        try {
            for(;;) {
                int signal = co_await signals.async_wait(asio::use_awaitable);
                if(signal == SIGTERM || signal == SIGINT) {
                    asio::post(ex, std::bind(&DBusAdaptor::stop, this));
                    co_return;
                }
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            }
        }
    }

    asio::awaitable<void> DBusAdaptor::start(void) {
        Application::info("service started, uid: {}, gid: {}, pid: {}, version: {}",
                          getuid(), getgid(), getpid(), LTSM_SESSION_DISPLAY_VERSION);

        auto ex = co_await asio::this_coro::executor;

        asio::co_spawn(ex, signalsHandler(), asio::bind_cancellation_slot(signals_cancel_.slot(), asio::detached));
        asio::co_spawn(ex, childsAliveChecker(), asio::bind_cancellation_slot(childs_cancel_.slot(), asio::detached));

        sdbus_job_ = std::thread([self=shared_from_this()]() {
           try {
                self->dbus_conn_->enterEventLoop();
            } catch(const sdbus::Error& err) {
                Application::error("{}: failed, sdbus error: {}", NS_FuncNameV, err.getName());
                self->stop();
            }
        });
    }

    asio::awaitable<void> startDisplaySessionAwait(int displayNum, const char* xauthFile, bool debug) {
        try {
            auto json = ApplicationJsonConfig("ltsm_session_display");
            auto starter = co_await startDisplayAwait(json, displayNum, xauthFile);

            clearSessionDbusAddress(displayNum);
            auto session = co_await startSessionAwait(json, displayNum);

            auto ex = co_await asio::this_coro::executor;
            auto ptr = std::make_shared<DBusAdaptor>(ex, std::move(json), std::move(starter), std::move(session), debug);

            asio::co_spawn(ex, [adaptor=std::move(ptr)]() -> asio::awaitable<void> {
                co_await adaptor->start();
                co_return;
            }, asio::detached);

        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        }
    }

    int startDisplaySession(int displayNum, const char* xauthFile, bool debug) {
        asio::io_context ioc;
        asio::co_spawn(ioc, startDisplaySessionAwait(displayNum, xauthFile, debug), asio::detached);
        ioc.run();
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
        Application::error("{}: {} not found", NS_FuncNameV, "HOME");
        return EXIT_FAILURE;
    }

    setenv("DISPLAY", displayAddr, 1);
    setenv("XAUTHORITY", xauthFile, 1);

    try {
        int displayNum = std::stoi(displayAddr + 1);
        return DisplaySession::startDisplaySession(displayNum, xauthFile, debug);
    } catch(const sdbus::Error & err) {
        Application::error("sdbus: [{}] {}", err.getName(), err.getMessage());
    } catch(const std::exception & err) {
        Application::error("exception: {}", err.what());
    }

    return EXIT_FAILURE;
}
