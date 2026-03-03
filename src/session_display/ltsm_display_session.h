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
#include <mutex>
#include <chrono>
#include <atomic>
#include <memory>
#include <future>
#include <vector>
#include <utility>
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

#define LTSM_SESSION_DISPLAY_VERSION 20260210

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

        mutable bp::child proc_;
        bp::ipstream proc_out_, proc_err_;

      protected:
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
                Application::error("{}: exception: {}", __FUNCTION__, err.what());
            }
        }

      public:
        SessionProcess() = default;
        SessionProcess(SessionProcess &&) = default;
        SessionProcess & operator=(SessionProcess &&) = default;

        SessionProcess(const std::string & cmd, const Args&... args)
            : filename_(std::filesystem::path(cmd).filename()) {
            proc_ = bp::child(cmd, args..., bp::std_out > proc_out_, bp::std_err > proc_err_);
        }

        ~SessionProcess() {
            if(! filename_.empty()) {
                waitAndLogging();
            }
        }

        int pid(void) const {
            return proc_.id();
        }

        bool isValid(void) const {
            return proc_.valid();
        }

        bool isRunning(void) const {
            return proc_.running();
        }
    };

    using DBusConnectionPtr = std::unique_ptr<sdbus::IConnection>;

    class X11Session : public ApplicationJsonConfig {
        std::string dbus_address_;

        std::string_view xauth_file_;
        const XCB::AuthCookie mcookie_;

        int default_width_ = 0;
        int default_height_ = 0;
        int default_depth_ = 0;
        int display_num_ = -1;

      protected:
        DBusConnectionPtr dbus_conn_;

        SessionProcess<ArgsList> ps_xorg_;
        SessionProcess<ArgsList, bp::environment> ps_sess_;

      protected:
        bool startX11Display(void);
        bool startX11Session(void);

      public:
        X11Session(int displayNum, const char* xauthFile, bool debug);

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

    class DBusAdaptor : public X11Session, public sdbus::AdaptorInterfaces<Session::Display_adaptor> {
        const std::chrono::system_clock::time_point started_;
        const std::chrono::milliseconds dur_childs_{350};

        boost::asio::io_context ioc_;
        boost::asio::signal_set signals_;
        boost::asio::steady_timer timer_childs_;

        std::future<void> sdbus_job_;

        std::mutex lock_childs_;
        std::list<bp::child> childs_;

        void timerChildsAliveCheck(const boost::system::error_code &);

        void stop(void);

      public:
        DBusAdaptor(int displayNum, const char* xauthFile, bool debug);
        virtual ~DBusAdaptor();

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

        int start(void);
    };
}

#endif // _LTSM_DISPLAY_SESSION_
