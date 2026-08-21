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

#ifndef _LTSM_DISPLAY_SESSION_
#define _LTSM_DISPLAY_SESSION_

#include <list>
#include <chrono>
#include <thread>
#include <memory>
#include <vector>
#include <utility>
#include <optional>
#include <filesystem>

#include <boost/asio.hpp>

#if BOOST_VERSION >= 108700
#include <boost/process/v1.hpp>
#include <boost/process/v1/child.hpp>
#else
#include <boost/process/child.hpp>
#endif

#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_display_adaptor.h"

#define LTSM_SESSION_DISPLAY_VERSION 20260821

namespace LTSM::DisplaySession {
    using StdoutBuf = std::vector<uint8_t>;
    using StatusStdout = sdbus::Struct<int, StdoutBuf>;

#if BOOST_VERSION >= 108700
    namespace bp = boost::process::v1;
#else
    namespace bp = boost::process;
#endif
    using ArgsList = std::vector<std::string>;

    template<typename... Args>
    class SessionProcess {
        std::string filename_;

        bp::child proc_;
        bp::ipstream proc_out_, proc_err_;


      public:
        SessionProcess() = default;
        SessionProcess(SessionProcess &&) noexcept = default;
        SessionProcess & operator=(SessionProcess &&) noexcept = default;

        SessionProcess(const std::string & cmd, const Args&... args)
            : filename_(std::filesystem::path(cmd).filename()) {
            proc_ = bp::child(cmd, args..., bp::std_in < bp::null, bp::std_out > proc_out_, bp::std_err > proc_err_);
        }

        ~SessionProcess() = default;

        void waitAndLogging(void) noexcept {
            try {
                std::string str_out{std::istreambuf_iterator<char>(proc_out_),
                                    std::istreambuf_iterator<char>()};

                std::string str_err{std::istreambuf_iterator<char>(proc_err_),
                                    std::istreambuf_iterator<char>()};

                proc_.wait();

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
            return proc_.id();
        }

        bool isValid(void) const {
            return proc_.valid();
        }

        bool isRunning(void) const {
            return const_cast<bp::child &>(proc_).running();
        }
    };

    using DBusConnectionPtr = std::unique_ptr<sdbus::IConnection>;

    struct X11Display {
        int default_width_ = 0;
        int default_height_ = 0;
        int default_depth_ = 0;
        int display_num_ = -1;

        std::string xauth_file_;
        XCB::AuthCookie mcookie_;

        SessionProcess<ArgsList> ps_xorg_;

    };

    struct X11SessionBase {
        std::string dbus_address_;
        SessionProcess<ArgsList, bp::environment> ps_sess_;
    };

    int startDisplaySession(int displayNum, const char* xauthFile, bool debug);

    class X11Session : public ApplicationJsonConfig, protected X11Display, protected X11SessionBase {
      protected:
        DBusConnectionPtr dbus_conn_;

      public:
        X11Session(ApplicationJsonConfig&&, X11Display&&, X11SessionBase&&, bool debug);

        int displayNum(void) const {
            return display_num_;
        }
        int pidXorg(void) const {
            return ps_xorg_.pid();
        }
        int pidSession(void) const {
            return ps_sess_.pid();
        }
    };

    class DBusAdaptor : public X11Session, public sdbus::AdaptorInterfaces<Session::Display_adaptor>, public std::enable_shared_from_this<DBusAdaptor> {
        const std::chrono::milliseconds dur_childs_{350};
        const std::chrono::system_clock::time_point started_;

        boost::asio::cancellation_signal signals_cancel_;
        boost::asio::cancellation_signal childs_cancel_;
        boost::asio::strand<boost::asio::any_io_executor> childs_strand_;

        std::list<bp::child> childs_;

        std::thread sdbus_job_;
        std::once_flag stop_flag_;

      protected:
        boost::asio::awaitable<void> childsAliveChecker(void);
        boost::asio::awaitable<void> signalsHandler(void);

        void stop(void) noexcept;
        void stopContexts(void);

      public:
        DBusAdaptor(const boost::asio::any_io_executor&, ApplicationJsonConfig&&, X11Display&&, X11SessionBase&&, bool debug);
        virtual ~DBusAdaptor();

        boost::asio::awaitable<void> start(void);

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;
        void setDebug(const std::string & level) override;

        std::string jsonStatus(void) override;

        int32_t runSessionCommandAsync(const std::string& cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) override;
        StatusStdout runSessionCommandSync(const std::string& cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs) override;
        StatusStdout runSessionZenity(const std::vector<std::string> & args) override;
        void setSessionKeyboardLayout(const std::string& layout) override;

        void notifyInfo(const std::string& summary, const std::string& body) override;
        void notifyWarning(const std::string& summary, const std::string& body) override;
        void notifyError(const std::string& summary, const std::string& body) override;

    };
}

#endif // _LTSM_DISPLAY_SESSION_
