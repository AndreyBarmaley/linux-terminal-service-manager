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

#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <forward_list>

#include "ltsm_streambuf.h"
#include "ltsm_application.h"
#include "ltsm_audio_pulse.h"
#include "ltsm_audio_encoder.h"
#include "ltsm_audio_adaptor.h"

namespace LTSM
{
    struct AudioClient
    {
        std::string socketPath;

        std::unique_ptr<PulseAudio::OutputStream> pulse;
        std::unique_ptr<AudioEncoder::BaseEncoder> encoder;
        std::unique_ptr<SocketStream> sock;

        std::thread thread;
        std::atomic<bool> shutdown{false};

        AudioClient(const std::string & );
        ~AudioClient();

        void pcmDataNotify(const uint8_t* ptr, size_t len);
        bool socketInitialize(void);
    };

    class AudioSessionBus : public sdbus::AdaptorInterfaces<Session::AUDIO_adaptor>, public Application
    {
        std::forward_list<AudioClient> clients;

    public:
        AudioSessionBus(sdbus::IConnection &, bool debug = false);
        virtual ~AudioSessionBus();

        int start(void) override;

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;
        void setDebug(const std::string & level) override;

        bool connectChannel(const std::string & clientSocket) override;
        void disconnectChannel(const std::string & clientSocket) override;
    };
}

#endif // _LTSM_AUDIO_SESSION_
