/***********************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_AUDIO_SESSION_
#define _LTSM_AUDIO_SESSION_

#include <future>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <forward_list>

#include <boost/asio.hpp>

#include "ltsm_application.h"
#include "ltsm_audio_pulse.h"
#include "ltsm_audio_encoder.h"
#include "ltsm_audio_adaptor.h"

namespace LTSM {
    struct AudioClient {
        std::string socketPath;

        std::unique_ptr<PulseAudio::OutputStream> pulse;
        std::unique_ptr<AudioEncoder::BaseEncoder> encoder;
        std::unique_ptr<SocketStream> sock;

        std::thread thread;
        std::atomic<bool> shutdown{false};

        AudioClient(const std::string &);
        ~AudioClient();

        void pcmDataNotify(const uint8_t* ptr, size_t len);
        bool socketInitialize(void);
    };

    using DBusConnectionPtr = std::unique_ptr<sdbus::IConnection>;

    class AudioSessionBus : public ApplicationLog, public sdbus::AdaptorInterfaces<Session::Audio_adaptor> {
        boost::asio::io_context ioc_;
        boost::asio::signal_set signals_;

        std::future<void> sdbus_job_;
        DBusConnectionPtr dbus_conn_;

        std::forward_list<AudioClient> clients;

      protected:
        void stop(void);

      public:
        AudioSessionBus(DBusConnectionPtr, bool debug = false);
        virtual ~AudioSessionBus();

        int start(void);

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;
        void setDebug(const std::string & level) override;

        bool connectChannel(const std::string & clientSocket) override;
        void disconnectChannel(const std::string & clientSocket) override;
    };
}

#endif // _LTSM_AUDIO_SESSION_
