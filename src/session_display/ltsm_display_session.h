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

    class Starter;

    class DBusAdaptor : public sdbus::AdaptorInterfaces<Session::Display_adaptor> {
        Starter & starter_;

      public:
        DBusAdaptor(sdbus::IConnection &, Starter &);
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
    };

    class Starter : public ApplicationJsonConfig {
        const std::chrono::system_clock::time_point started_;
        const std::chrono::milliseconds dur_childs_{350};

        boost::asio::io_context ioc_;
        boost::asio::signal_set signals_;
        boost::asio::steady_timer timer_childs_;

        std::future<void> sdbus_job_;

        std::string_view xauth_file_;
        const XCB::AuthCookie mcookie_;

        int default_width_ = 0;
        int default_height_ = 0;
        int default_depth_ = 0;
        int display_num_ = -1;
    
        std::mutex lock_childs_;
        std::list<bp::child> childs_;

        std::unique_ptr<sdbus::IConnection> dbus_conn_;
        std::unique_ptr<DBusAdaptor> dbus_adaptor_;
        std::unique_ptr<XCB::Connector> xcb_;

        bp::child ps_xorg_, ps_sess_;

      protected:
        friend class DBusAdaptor;

        void timerChildsAliveCheck(const boost::system::error_code&);

        void stop(void);
        bool startX11Display(void);
        bool startX11Session(void);

        // dbus callbacks
        void dbusSetDebug(const std::string & level);
        int32_t dbusRunSessionCommandAsync(const std::string& cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs);
        StatusStdout dbusRunSessionCommandSync(const std::string& cmd, const std::vector<std::string> & args, const std::vector<std::string> & envs);
        std::string dbusJsonStatus(void) const;

      public:
        Starter(int displayNum, const char* xauthFile);
        ~Starter();

        int start(void);
    };
}

#endif // _LTSM_USER_SESSION_
