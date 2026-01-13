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

#ifndef _LTSM_USER_SESSION_
#define _LTSM_USER_SESSION_

#include <mutex>
#include <chrono>
#include <atomic>
#include <memory>
#include <future>
#include <vector>
#include <utility>
#include <forward_list>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_display_adaptor.h"

#define LTSM_SESSION_DISPLAY_VERSION 20250910

namespace LTSM
{
    using StdoutBuf = std::vector<uint8_t>;
    using StatusStdout = std::pair<int, StdoutBuf>;
    using PidStatusStdout = std::pair<int, std::future<StatusStdout>>;

    class DisplaySessionBus : public ApplicationJsonConfig, public sdbus::AdaptorInterfaces<Session::Display_adaptor>
    {
        std::forward_list<PidStatusStdout> childCommands;
        std::mutex lockCommands;

        XCB::AuthCookie mcookie;
        std::unique_ptr<XCB::Connector> xcb;

        std::unique_ptr<Tools::BaseTimer> timer1;

        std::chrono::system_clock::time_point started;

        std::string displayStr;
        std::string xauthFile;

        int defaultWidth = 0;
        int defaultHeight = 0;
        int defaultDepth = 0;

        int displayNum = -1;
        int pidXorg = -1;
        int pidSession = -1;

    protected:
        void checkChildCommandsComplete(void);
        void childProcessEnded(int pid, std::future<StatusStdout>);
        bool xcbConnect(void);
        void childStop(void);

    public:
        DisplaySessionBus(sdbus::IConnection &, int display);
        virtual ~DisplaySessionBus();

        int start(void);

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;
        void setDebug(const std::string & level) override;

        std::string jsonStatus(void) override;
        int32_t runSessionCommand(const std::string& cmd, const std::vector<std::string>& args, const std::vector<std::string>& envs) override;
    };
}

#endif // _LTSM_USER_SESSION_
