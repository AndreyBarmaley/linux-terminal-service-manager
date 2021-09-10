/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#include <string>
#include <thread>
#include <iostream>
#include <iterator>
#include <functional>
#include <algorithm>
#include <filesystem>

#ifdef __cplusplus
extern "C" {
#endif
 #include "spice/protocol.h"
 #include "spice-server/spice.h"
#ifdef __cplusplus
}
#endif

#include "ltsm_tools.h"
#include "ltsm_connector_spice.h"

using namespace std::chrono_literals;

struct SpiceTimer : protected LTSM::Tools::BaseTimer
{
    std::unique_ptr<BaseTimer> ptr;
    SpiceTimerFunc          callback;
    void*                   opaque;

public:
    SpiceTimer(SpiceTimerFunc func, void* data) : callback(func), opaque(data) {}

    void stop(void) { if(ptr) ptr->stop(); }

    void start(uint32_t ms)
    {
	if(callback)
	    ptr = BaseTimer::create<std::chrono::milliseconds>(ms, false, callback, opaque);
    }
};

namespace LTSM
{
    SpiceTimer* cbTimerAdd(SpiceTimerFunc func, void* opaque)
    {
	auto timer = new SpiceTimer(func, opaque);
        Application::debug("timer add: %p", timer);
        return timer;
    }

    void cbTimerCancel(SpiceTimer* timer)
    {
        if(timer)
        {
            Application::debug("timer stop: %p", timer);
            timer->stop();
        }
    }

    void cbTimerRemove(SpiceTimer* timer)
    {
        if(timer)
        {
            Application::debug("timer remove: %p", timer);
            delete timer;
        }
    }

    void cbTimerStart(SpiceTimer* timer, uint32_t ms)
    {
        if(timer)
        {
            Application::debug("timer start: %p", timer);
            timer->start(ms);
        }
    }

    void cbChannelEvent(int event, SpiceChannelEventInfo* info)
    {
    }

    class SpiceClientCallback
    {
        SpiceServer*            reds;
        SpiceCoreInterface      core;

    public:
        SpiceClientCallback(int fd, const std::string & remoteaddr, const JsonObject & config, Connector::SPICE* connector)
        {
            reds = spice_server_new();
            if(! reds)
            {
                Application::error("%s: failure", "spice_server_new");
                throw EXIT_FAILURE;
            }

            core.base.type = "SpiceClientCallback";
            core.base.description = "LTSM SPICE client callback";
            core.base.major_version = SPICE_INTERFACE_CORE_MAJOR;
            core.base.minor_version = SPICE_INTERFACE_CORE_MINOR;

            core.timer_add = cbTimerAdd;
            core.timer_start = cbTimerStart;
            core.timer_cancel = cbTimerCancel;
            core.timer_remove = cbTimerRemove;
            core.channel_event = cbChannelEvent;

/*
            core.watch_add = BaseWatch::watchAdd;
            core.watch_update_mask = BaseWatch::watchUpdateMask;
            core.watch_remove = BaseWatch::watchRemove;
*/

            spice_server_init(reds, & core);
            spice_server_set_agent_mouse(reds, 1);
            spice_server_set_agent_copypaste(reds, 1);
            spice_server_set_agent_file_xfer(reds, 0);

            spice_server_add_client(reds, fd, 1);
        }

        ~SpiceClientCallback()
        {
            if(reds) spice_server_destroy(reds);
        }
    };

    /* Connector::SPICE */
    int Connector::SPICE::communication(void)
    {
        if(0 >= busGetServiceVersion())
        {
            Application::error("%s: failed", "bus service");
            return EXIT_FAILURE;
        }

        const std::string home = Tools::getenv("HOME", "/tmp");
        const auto socketFile = std::filesystem::path(home) / std::string("rdp_pid").append(std::to_string(getpid()));

        if(! initUnixSockets(socketFile.string()))
            return EXIT_SUCCESS;

        // create x11 connect


        // all ok
        while(loopMessage)
        {
            //if(freeRdpClient->isShutdown())
            //    loopMessage = false;

            if(! ProxySocket::enterEventLoopAsync())
                loopMessage = false;

            // dbus processing
            _conn->enterEventLoopAsync();

            // wait
            std::this_thread::sleep_for(1ms);
        }


        Application::debug("under construction, remoteaddr: %s\n", _remoteaddr.c_str());
        return EXIT_SUCCESS;
    }
}
