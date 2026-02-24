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

#include <mutex>
#include <chrono>
#include <atomic>
#include <memory>
#include <future>
#include <vector>
#include <utility>
#include <forward_list>

#include <boost/asio.h>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_display_adaptor.h"

#define LTSM_SESSION_DISPLAY_VERSION 20260110

namespace LTSM::DisplaySession {
    using StdoutBuf = std::vector<uint8_t>;
    using StatusStdout = sdbus::Struct<int, StdoutBuf>;
    using PidStatus = std::pair<int, std::future<int>>;
    using PidStatusStdout = std::pair<int, std::future<StatusStdout>>;

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

        XCB::AuthCookie mcookie_;

        int defaultWidth_ = 0;
        int defaultHeight_ = 0;
        int defaultDepth_ = 0;
        int displayNum_ = -1;

        std::forward_list<PidStatusStdout> childCommands_;
        std::mutex lockCommands_;

        std::unique_ptr<XCB::Connector> xcb_;
        std::unique_ptr<Tools::BaseTimer> timer1_;
        std::unique_ptr<DBusAdaptor> dbus_;

        PidStatus pidXorg_, pidSession_;

      protected:
        friend class DBusAdaptor;

        void startX11Display(int displayNum, const char* xauthFile);
        bool startX11Session(void);
        void checkChildCommandsComplete(void);
        void childProcessEnded(int pid, StatusStdout);
        void stopChilds(void);
        void storeChild(PidStatusStdout);

        // dbus callbacks
        void dbusServiceShutdown(void) const;
        void dbusSetDebug(const std::string & level);
        std::string dbusJsonStatus(void) const;

      public:
        Starter(int displayNum, const char* xauthFile);
        ~Starter();

        int run(void);
    };
}

#endif // _LTSM_USER_SESSION_
