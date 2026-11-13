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

#include <span>
#include <mutex>
#include <chrono>
#include <memory>
#include <string>

#include "ltsm_application.h"
#include "ltsm_async_sdbus.h"
#include "ltsm_async_socket.h"
#include "ltsm_audio_encoder.h"
#include "ltsm_audio_adaptor.h"

#ifdef LTSM_WITH_PULSE
#include "ltsm_audio_pulse.h"
#endif
#ifdef LTSM_WITH_PIPEWIRE
#include "ltsm_audio_pipewire.h"
#endif

#include <boost/asio/experimental/channel.hpp>

namespace LTSM {
    using AudioBuffers = boost::container::small_vector<boost::asio::const_buffer, 3>;

    class AudioPacket {
        uint16_t id_ = 0;
        uint32_t len_ = 0;

    public:
        AudioPacket(uint16_t id, uint32_t len);
        virtual ~AudioPacket() = default;
        virtual AudioBuffers buffers() const;
    };

    class AudioPacketSilent : public AudioPacket {
    public:
        explicit AudioPacketSilent(uint16_t len) : AudioPacket(AudioOp::Silent, len) {
        }
    };

    class AudioPacketPcm : public AudioPacket {
        const std::span<const uint8_t> pcm_;
    public:
        explicit AudioPacketPcm(std::span<const uint8_t> pcm) : AudioPacket(AudioOp::Data, pcm.size()), pcm_{pcm} {
        }
        AudioBuffers buffers() const override;
    };

    class AudioPacketEnc : public AudioPacket {
        const std::vector<uint8_t> enc_;
    public:
        explicit AudioPacketEnc(std::vector<uint8_t> enc) : AudioPacket(AudioOp::Data, enc.size()), enc_{std::move(enc)} {
        }
        AudioBuffers buffers() const override;
    };

    using AudioPackets = std::vector<std::unique_ptr<AudioPacket>>;
    using TransferChannel = boost::asio::experimental::channel<void(boost::system::error_code, BinaryBuf)>;
    constexpr int TransferChannelSize = 4;

    class AudioClient : std::enable_shared_from_this<AudioClient> {
        mutable AsyncLocalStream stream_;
        TransferChannel packets_channel_;
        boost::asio::steady_timer tm_wait_engine_;

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

    protected:
        boost::asio::awaitable<void> timerWaitEngine(void);

        bool engineInit(void);
        void dataReadyNotify(std::span<const uint8_t>);
        AudioPackets dataEncode(std::span<const uint8_t>);

    public:
        AudioClient(const boost::asio::any_io_executor & ex)
            : stream_(ex), packets_channel_{ex, TransferChannelSize}, tm_wait_engine_{ex} {
        }
        ~AudioClient() = default;
    
        boost::asio::awaitable<void> remoteHandshake(const std::string &, std::chrono::seconds connect_deadline);
        boost::asio::awaitable<void> sendPackets(void);
        void cancel();

        bool socketPath(std::string_view path) const {
            return stream_.socket().local_endpoint().path() == path;
        }

        bool socketConnected(void) const {
            return stream_.socket().is_open();
        }
    };

    using DBusConnectionPtr = std::unique_ptr<sdbus::IConnection>;
    using AudioClientPtr = std::shared_ptr<AudioClient>;
    using AudioClients = boost::container::small_vector<AudioClientPtr, 4>;

    class AudioSessionBus : public ApplicationLog, public sdbus::AdaptorInterfaces<Session::Audio_adaptor>, protected SDBus::AsioCoroConnector {
        boost::asio::io_context ioc_;
        boost::asio::signal_set signals_;
        boost::asio::cancellation_signal connect_cancel_;
        boost::asio::strand<boost::asio::any_io_executor> clients_strand_;

        AudioClients clients_;
        std::once_flag stop_flag_;

      protected:
        boost::asio::awaitable<void> signalsHandler(void);
        boost::asio::awaitable<void> sdbusHandler(void);
        void stopContext(void);
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
