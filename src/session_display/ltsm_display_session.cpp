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
#include <boost/process/popen.hpp>
#include <boost/process/environment.hpp>
#else
#include <boost/process/v2/popen.hpp>
#include <boost/process/v2/environment.hpp>
#endif

#include <boost/asio/experimental/awaitable_operators.hpp>

#include "ltsm_zlib.h"
#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_sdbus_proxy.h"
#include "ltsm_display_session.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM::DisplaySession {

    bp::process_stdio SessionProcess::createRedirect(const std::string& cmd) {
        const auto filename = std::filesystem::path(cmd).filename();
        auto log_dir = std::filesystem::path{"/tmp"} / ".ltsm" / "log";

        if(auto home = getenv("HOME")) {
            log_dir = std::filesystem::path{home} / ".ltsm" / "log";
        }

        if(! std::filesystem::is_directory(log_dir)) {
            std::filesystem::create_directories(log_dir);
        }

        auto log_file_out = log_dir / filename;
        log_file_out.replace_extension(".out");

        auto log_file_err = log_dir / filename;
        log_file_err.replace_extension(".err");

        return bp::process_stdio{ .in = nullptr, .out = log_file_out, .err = log_file_err };
    }

    asio::awaitable<void> waitSocketConnectAwait(const std::filesystem::path& file) {
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

    asio::awaitable<void> waitSocketTimeoutAwait(const std::filesystem::path& file, std::chrono::milliseconds deadline_ms) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer tm_deadline{ex, deadline_ms};

        using namespace asio::experimental::awaitable_operators;
        auto results = co_await(waitSocketConnectAwait(file) || tm_deadline.async_wait(asio::use_awaitable));

        if(results.index() == 0) {
            tm_deadline.cancel();
            co_return;
        }

        Application::error("{}: deadline, path: {}", NS_FuncNameV, file.string());
        throw std::system_error(std::make_error_code(std::errc::timed_out), file.string());
    }

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
        } catch(const system::system_error& err) {
            if(err.code() != asio::error::eof) {
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
        auto results = co_await(waitFileAwait(file) || tm_deadline.async_wait(asio::use_awaitable));

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

        auto ex = co_await asio::this_coro::executor;

        // start Xorg
        res.ps_xorg_ = std::make_shared<bp::process>(ex, xorgBin, std::move(xorgArgs), SessionProcess::createRedirect(xorgBin));

        // wait started
        const uint32_t deadline_ms = json.configGetInteger("xvfb:timeout", 3500);
        auto socket_path = Tools::x11UnixPath(res.display_num_);

        co_await waitSocketTimeoutAwait(socket_path, std::chrono::milliseconds(deadline_ms));
        Application::info("{}: cmd: {}, pid: {}, display: {}, socket: {}",
                NS_FuncNameV, xorgBin, res.ps_xorg_->id(), displayNum, socket_path);

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

        std::unordered_map<bp::environment::key, bp::environment::value> sessionEnvs;
        for (const auto& kv: bp::environment::current()) {
            sessionEnvs[kv.key()] = kv.value();
        }

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

        auto ex = co_await asio::this_coro::executor;

        X11SessionBase res;
        // start Session
        res.ps_sess_ = std::make_shared<bp::process>(ex, sessionBin, std::move(sessionArgs),
                bp::process_environment{sessionEnvs}, SessionProcess::createRedirect(sessionBin));

        // wait dbus
        const uint32_t deadline_ms = json.configGetInteger("xvfb:timeout", 3500);
        res.dbus_address_ = co_await waitSessionDbusAddressAwait(displayNum, deadline_ms);

        Application::info("{}: cmd: {}, pid: {}, display: {}, dbus address: `{}'",
                NS_FuncNameV, sessionBin, res.ps_sess_->id(), displayNum, res.dbus_address_);

        co_return res;
    }

    X11Session::X11Session(ApplicationJsonConfig&& config, X11Display&& xorg, X11SessionBase&& sess, bool debug)
        : ApplicationJsonConfig(std::move(config)), X11Display(std::move(xorg)), X11SessionBase(std::move(sess)) {

        Application::setDebugTarget(DebugTarget::Syslog, "ltsm_session_display");

        if(debug) {
            Application::setDebugLevel(DebugLevel::Debug);
        }

        setenv("DBUS_SESSION_BUS_ADDRESS", dbus_address_.c_str(), 1);

#ifdef SDBUS_2_0_API
        dbus_conn_ = sdbus::createSessionBusConnection(sdbus::ServiceName {dbus_session_display_name});
#else
        dbus_conn_ = sdbus::createSessionBusConnection(dbus_session_display_name);
#endif
    }

    // DBusAdaptor
    DBusAdaptor::DBusAdaptor(asio::io_context& ioc, ApplicationJsonConfig&& config, X11Display&& xorg, X11SessionBase&& sess, bool debug)
        : X11Session(std::move(config), std::move(xorg), std::move(sess), debug),
#ifdef SDBUS_2_0_API
          AdaptorInterfaces(*dbus_conn_, sdbus::ObjectPath {dbus_session_display_path}),
#else
          AdaptorInterfaces(*dbus_conn_, dbus_session_display_path),
#endif
          started_(std::chrono::system_clock::now()), ioc_ {ioc} {
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
        asio::post(ioc_, [self = shared_from_this()]() {
            self->stop();
        });
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

        std::unordered_map<bp::environment::key, bp::environment::value> p_envs;
        for (const auto& kv: bp::environment::current()) {
            p_envs[kv.key()] = kv.value();
        }

        for(auto & str : envs) {
            if(auto pos = str.find("="); pos != std::string::npos) {
                p_envs[str.substr(0, pos)] = str.substr(pos + 1);
            }
        }

        try {
            auto proc = bp::popen(ioc_, cmd, args, bp::process_environment{p_envs});
            auto pid = proc.id();

            Application::debug(DebugType::Dbus, "{}: running cmd: {}, pid: {}", NS_FuncNameV, cmd, pid);
            child_pids_.insert(pid);

            asio::co_spawn(ioc_, [self=shared_from_this(),proc=std::move(proc),cmd,pid]() mutable -> asio::awaitable<void> {
                StdoutBuf res;

                try {
                    co_await asio::async_read(proc, asio::dynamic_buffer(res), asio::use_awaitable);
                } catch(const system::system_error& err) {
                    if(auto ec = err.code(); ec != asio::error::eof) {
                        Application::error("{}: system error: {}, code: {}", "runSessionCommandAsync", ec.message(), ec.value());
                        self->child_pids_.erase(pid);
                        self->emitRunSessionCommandAsyncComplete(pid, false, 0, {});
                        co_return;
                    }
                }

                int exit_code = co_await proc.async_wait(asio::use_awaitable);
                Application::debug(DebugType::Dbus, "{}: {} exited, pid: {}, code: {}", "WaitProcess", cmd, proc.id(), exit_code);

                self->child_pids_.erase(pid);
                self->emitRunSessionCommandAsyncComplete(pid, true, exit_code, std::move(res));

                co_return;
            }, asio::detached);

            return pid;

        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        }

        return -1;
    }

    StatusStdout DBusAdaptor::runSessionCommandSync(const std::string& cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) {
        Application::debug(DebugType::Dbus, "{}: cmd: {}, args: [{}]", NS_FuncNameV, cmd, Tools::join(args, ", "));

        std::unordered_map<bp::environment::key, bp::environment::value> p_envs;
        for (const auto& kv: bp::environment::current()) {
            p_envs[kv.key()] = kv.value();
        }

        for(auto & str : envs) {
            if(auto pos = str.find("="); pos != std::string::npos) {
                p_envs[str.substr(0, pos)] = str.substr(pos + 1);
            }
        }

        try {
            auto proc = bp::popen(ioc_, cmd, args, bp::process_environment{p_envs});

            Application::debug(DebugType::Dbus, "{}: running cmd: {}, pid: {}", NS_FuncNameV, cmd, proc.id());
            StdoutBuf res;

            system::error_code ec;
            asio::read(proc, asio::dynamic_buffer(res), ec);

            if (ec && ec != asio::error::eof) {
                throw system::system_error(ec);
            }
        
            int exit_code = proc.wait();
            Application::debug(DebugType::Dbus, "{}: {} exited, pid: {}, code: {}", "WaitProcess", cmd, proc.id(), exit_code);

            return StatusStdout{exit_code, std::move(res)};

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
        [[maybe_unused]] auto stdout = runSessionCommandSync("/usr/bin/setxkbmap", { "-layout", layout, "-option", "\"\"" }, {});
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

    void DBusAdaptor::stop(void) noexcept {
        std::call_once(stop_flag_, [this]() {
            try {
                stopContexts();
            } catch(const std::exception &) {
            }
        });
    }

    void DBusAdaptor::stopContexts(void) {
        dbus_conn_->leaveEventLoop();
        signals_cancel_.emit(asio::cancellation_type::terminal);

        // part1: request exit
        if(ps_sess_ && ps_sess_->running()) {
            ps_sess_->request_exit();
        }

        if(ps_xorg_ && ps_xorg_->running()) {
            ps_xorg_->request_exit();
        }

        for(auto pid: child_pids_) {
            kill(pid, SIGTERM);
        }

        child_pids_.clear();

        if(sdbus_job_.joinable()) {
            sdbus_job_.join();
        }

        Application::notice("{}: Display session shutdown", NS_FuncNameV);
    }

    asio::awaitable<void> DBusAdaptor::signalsHandler(void) {
        asio::signal_set signals{ioc_, SIGTERM, SIGINT};

        try {
            for(;;) {
                int signal = co_await signals.async_wait(asio::use_awaitable);

                if(signal == SIGTERM || signal == SIGINT) {
                    asio::post(ioc_, [self = shared_from_this()]() {
                        self->stop();
                    });
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

        auto self = shared_from_this();

        asio::co_spawn(ioc_, [self]() -> asio::awaitable<void> {
            auto& proc = self->ps_xorg_;
            co_await proc->async_wait(asio::use_awaitable);
            Application::info("{}: {} exited, pid: {}, service shutdown...", "WaitProcess", "xorg", proc->id());
            self->stop();
            co_return;
        }, asio::detached);

        asio::co_spawn(ioc_, [self]() -> asio::awaitable<void> {
            auto& proc = self->ps_sess_;
            co_await proc->async_wait(asio::use_awaitable);
            Application::info("{}: {} exited, pid: {}, service shutdown...", "WaitProcess", "session", proc->id());
            self->stop();
            co_return;
        }, asio::detached);

        asio::co_spawn(ioc_, [self]() -> asio::awaitable<void> {
            co_await self->signalsHandler();
            co_return;
        }, asio::bind_cancellation_slot(signals_cancel_.slot(), asio::detached));

        sdbus_job_ = std::thread([self]() {
            try {
                self->dbus_conn_->enterEventLoop();
            } catch(const sdbus::Error& err) {
                Application::error("{}: failed, sdbus error: {}", NS_FuncNameV, err.getName());
                self->stop();
            }
        });

        co_return;
    }

    asio::awaitable<void> startDisplaySessionAwait(asio::io_context& ioc, int displayNum, const char* xauthFile, bool debug) {
        try {
            auto json = ApplicationJsonConfig("ltsm_session_display");
            auto starter = co_await startDisplayAwait(json, displayNum, xauthFile);

            clearSessionDbusAddress(displayNum);

            auto session = co_await startSessionAwait(json, displayNum);
            auto adaptor = std::make_shared<DBusAdaptor>(ioc, std::move(json), std::move(starter), std::move(session), debug);

            co_await adaptor->start();

        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        }
    }

    int startDisplaySession(int displayNum, const char* xauthFile, bool debug) {
        asio::io_context ioc;
        asio::co_spawn(ioc, startDisplaySessionAwait(ioc, displayNum, xauthFile, debug), asio::detached);
        ioc.run();
        return EXIT_SUCCESS;
    }
}

using namespace LTSM;

#ifdef LTSM_WITH_SANITIZE
extern "C" const char* __asan_default_options() {
    return "log_path=/var/tmp/asan_ltsm_display.log";
}
#endif

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
