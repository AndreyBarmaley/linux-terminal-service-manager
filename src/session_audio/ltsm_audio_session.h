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

#include <chrono>
#include <memory>
#include <string>
#include <forward_list>

#include <boost/container/small_vector.hpp>

#include "ltsm_application.h"
#include "ltsm_async_socket.h"
#include "ltsm_audio_encoder.h"
#include "ltsm_audio_adaptor.h"

#ifdef LTSM_WITH_PULSE
#include "ltsm_audio_pulse.h"
#endif
#ifdef LTSM_WITH_PIPEWIRE
#include "ltsm_audio_pipewire.h"
#endif

namespace LTSM {
    struct AudioPacket {
        uint16_t id_ = 0;
        uint32_t len_ = 0;
        std::vector<uint8_t> data_;

        AudioPacket(uint32_t len);
        AudioPacket(std::vector<uint8_t> &&);
    };

    using AudioPacketPtr = std::unique_ptr<AudioPacket>;

    struct AudioClient : protected AsyncSocket<boost::asio::local::stream_protocol::socket> {
        boost::asio::strand<boost::asio::any_io_executor> strand_;

        const uint8_t channels_ = 2;

#ifdef LTSM_WITH_PIPEWIRE
        std::unique_ptr<PipeWire::AudioCapture> pipew_;
#endif
#ifdef LTSM_WITH_PULSE
        std::unique_ptr<PulseAudio::OutputStream> pulse_;
#endif
        std::unique_ptr<AudioEncoder::BaseEncoder> encoder_;

        uint32_t bit_rate_ = 44100;
        uint32_t frag_size_ = 1024;

        AudioClient(boost::asio::local::stream_protocol::socket && sock,
                    boost::asio::strand<boost::asio::any_io_executor> && strand)
            : AsyncSocket<boost::asio::local::stream_protocol::socket>(std::move(sock)), strand_{std::move(strand)} {
        }
    
        ~AudioClient();

        boost::asio::awaitable<void> retryConnect(const std::string &, int);
        boost::asio::awaitable<void> remoteHandshake(void);
        boost::asio::awaitable<void> timerWaitEngine(void);

        bool engineInit(void);
        void dataReadyNotify(const uint8_t* ptr, size_t len);
        std::list<AudioPacketPtr> dataEncode(const uint8_t* ptr, size_t len);

        bool socketPath(std::string_view path) const {
            return socket().local_endpoint().path() == path;
        }

        bool socketConnected(void) const {
            return socket().is_open();
        }
    };

    using DBusConnectionPtr = std::unique_ptr<sdbus::IConnection>;
    using AudioClientPtr = std::unique_ptr<AudioClient>;

    class AudioSessionBus : public ApplicationLog, public sdbus::AdaptorInterfaces<Session::Audio_adaptor> {
        boost::asio::io_context ioc_;
        boost::asio::signal_set signals_;

        DBusConnectionPtr dbus_conn_;
        std::forward_list<AudioClientPtr> clients_;

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
