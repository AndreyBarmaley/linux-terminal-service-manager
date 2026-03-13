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

#include <future>
#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_audio.h"
#include "ltsm_global.h"
#include "ltsm_sockets.h"
#include "ltsm_audio_session.h"
#include "ltsm_byte_streambuf.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM {
#ifdef LTSM_WITH_PULSE
    const pa_sample_format_t def_format_pulse = platformBigEndian() ? PA_SAMPLE_S16BE : PA_SAMPLE_S16LE;
#endif
#ifdef LTSM_WITH_PIPEWIRE
    const spa_audio_format def_format_pipew = platformBigEndian() ? SPA_AUDIO_FORMAT_S16_BE : SPA_AUDIO_FORMAT_S16_LE;
#endif

    AudioPacket::Base::Base(uint16_t id, uint32_t len) {
        id_ = endian::native_to_little(id);
        buffers_.emplace_back(&id_, sizeof(id_));

        len_ = endian::native_to_little(len);
        buffers_.emplace_back(&len_, sizeof(len_));
    }

    AudioPacket::Silent::Silent(uint32_t len) : Base(AudioOp::Silent, len) {
    }

    AudioPacket::Data::Data(std::vector<uint8_t> && data) : Base(AudioOp::Data, data.size()) {
        data_ = std::move(data);
        buffers_.emplace_back(data_.data(), data_.size());
    }

    /// AudioClient
    AudioClient::AudioClient(asio::local::stream_protocol::socket && sock,
                             asio::strand<asio::any_io_executor> && strand)
        : sock_{std::move(sock)}, strand_{std::move(strand)} {
    }

    AudioClient::~AudioClient() {
        sock_.cancel();
        sock_.close();
#ifdef LTSM_WITH_PIPEWIRE
        pipew_.reset();
#endif
#ifdef LTSM_WITH_PULSE
        pulse_.reset();
#endif
        encoder_.reset();
    }

    asio::awaitable<void> AudioClient::retryConnect(const std::string & path, int attempts) {
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer{executor};

        for(int it = 1; it <= attempts; it++) {
            try {
                co_await sock_.async_connect(path, asio::use_awaitable);
                co_return;
            } catch(const system::system_error& ec) {
                if(it == attempts) {
                    throw;
                }
            }

            timer.expires_after(300ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
    }

    asio::awaitable<void> AudioClient::remoteHandshake(void) {
#ifdef LTSM_WITH_PIPEWIRE
        // pipewire priority
        const uint16_t bitsPerSample = PipeWire::formatBits(def_format_pipew);
#else
#ifdef LTSM_WITH_PULSE
        const uint16_t bitsPerSample = PulseAudio::formatBits(def_format_pulse);
#else
        const uint16_t bitsPerSample = 0;
#endif
#endif

        asio::streambuf sb;
        byte::streambuf bs(sb);

        // send initialize packet
        bs.write_le16(AudioOp::Init);
        // send proto ver
        bs.write_le16(1);

        int numenc = 1;
#ifdef LTSM_WITH_OPUS
        numenc++;
#endif
        // send num encodings
        bs.write_le16(numenc);
        // encoding type PCM
        bs.write_le16(AudioEncoding::PCM);
        bs.write_le16(channels_);
        bs.write_le32(44100);
        bs.write_le16(bitsPerSample);
#ifdef LTSM_WITH_OPUS
        // encoding type OPUS
        bs.write_le16(AudioEncoding::OPUS);
        bs.write_le16(channels_);
        bs.write_le32(48000);
        bs.write_le16(bitsPerSample);
#endif

        co_await asio::async_write(sock_, sb, asio::transfer_all(), asio::use_awaitable);

        // rsz: cmd16, err16
        const size_t rsz = sizeof(uint16_t) + sizeof(uint16_t);
        co_await asio::async_read(sock_, sb, asio::transfer_exactly(rsz), asio::use_awaitable);

        auto cmd = bs.read_le16();
        auto err = bs.read_le16();

        if(cmd != AudioOp::Init) {
            Application::error("{}: {} failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
            throw audio_error(NS_FuncNameS);
        }

        if(err) {
            co_await asio::async_read(sock_, sb, asio::transfer_exactly(err), asio::use_awaitable);
            auto str = bs.read_string(err);
            Application::error("{}: recv error: {}", __FUNCTION__, str);
            throw audio_error(NS_FuncNameS);
        }

        // ver16, enc16
        co_await asio::async_read(sock_, sb, asio::transfer_exactly(rsz), asio::use_awaitable);

        // proto version
        auto ver = bs.read_le16();
        // encoding
        auto enc = bs.read_le16();

        Application::info("{}: client proto version: {}, encode type: {:#04x}", __FUNCTION__, ver, enc);

        if(enc == AudioEncoding::OPUS) {
#ifdef LTSM_WITH_OPUS
            // Opus: frame counts - at 48kHz the permitted values are 120, 240, 480, or 960
            const uint32_t opusFrames = 960;
            const uint32_t opusFrameLength = channels_ * bitsPerSample / 8;
            bit_rate_ = 48000;
            frag_size_ = opusFrames * opusFrameLength;

            encoder_ = std::make_unique<AudioEncoder::Opus>(bit_rate_, channels_, bitsPerSample);
            Application::info("{}: selected encoder: {}", __FUNCTION__, "OPUS");
#else
            Application::error("{}: unsupported encoder: {}", __FUNCTION__, "OPUS");
            throw audio_error(NS_FuncNameS);
#endif
        } else {
            Application::info("{}: selected encoder: {}", __FUNCTION__, "PCM");
        }

        // init engine
        auto executor = co_await asio::this_coro::executor;

        // timer for engine init
        asio::steady_timer timer_init(executor);

        while(true) {
            timer_init.expires_after(300ms);
            co_await timer_init.async_wait(asio::use_awaitable);

            if(engineInit()) {
                co_return;
            }

            LTSM::Application::warning("{}: wait audio engine init...", __FUNCTION__);
        }
    }

    bool AudioClient::engineInit(void) {
#ifdef LTSM_WITH_PIPEWIRE

        if(! pipew_) {
            try {
                pipew_ = std::make_unique<PipeWire::AudioCapture>(def_format_pipew, bit_rate_, channels_,
                         std::bind(& AudioClient::dataReadyNotify, this, std::placeholders::_1, std::placeholders::_2));
                pipew_->streamConnect(false /* pause */);
            } catch(const std::exception & err) {
            }
        }

        // wait PipeWire started
        if(pipew_) {
            if(PW_STREAM_STATE_STREAMING == pipew_->streamState()) {
                // success
                return true;
            } else {
                LTSM::Application::warning("{}: stream state: {}", __FUNCTION__, pipew_->streamStateName());
            }
        }

#else
#ifdef LTSM_WITH_PULSE
        const pa_buffer_attr bufferAttr = { frag_size_, UINT32_MAX, UINT32_MAX, UINT32_MAX, frag_size_ };

        // wait PulseAudio started
        try {
            pulse_ = std::make_unique<PulseAudio::OutputStream>(def_format_pulse, bit_rate_, channels_,
                     std::bind(& AudioClient::dataReadyNotify, this, std::placeholders::_1, std::placeholders::_2));
        } catch(const std::exception & err) {
        }

        if(pulse_ && pulse_->initContext() && pulse_->streamConnect(false /* not paused */, & bufferAttr)) {
            // success
            return true;
        }

        pulse_.reset();
#endif
#endif
        return false;
    }

    void AudioClient::dataReadyNotify(const uint8_t* ptr, size_t len) {
        // this thread - from audio engine callback
        if(sock_.is_open()) {
            auto packets = dataEncode(ptr, len);
            Application::debug(DebugType::Audio, "{}: data size: {}, packets: {}", __FUNCTION__, len, packets.size());

            asio::co_spawn(strand_, [this, list = std::move(packets)]() -> asio::awaitable<void> {
                for(auto & pkt : list) {
                    try {
                        co_await asio::async_write(sock_, pkt->buffers_, asio::transfer_all(), asio::use_awaitable);
                    } catch(const boost::system::error_code& ec) {
                        Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "dataReadyNotify", "write", ec.value(), ec.message());
                        sock_.close();
                        co_return;
                    }
                }
            }, asio::detached);
        }
    }

    std::list<AudioPacketPtr>
    AudioClient::dataEncode(const uint8_t* ptr, size_t len) {

        std::list<AudioPacketPtr> res;

        const bool silent = std::ranges::all_of(ptr, ptr + len, [](auto & val) {
            return val == 0;
        });

        if(silent) {
            res.emplace_back(std::make_unique<AudioPacket::Silent>(len));
            return res;
        }

        if(! encoder_) {
            res.emplace_back(std::make_unique<AudioPacket::Data>(std::vector<uint8_t> {ptr, ptr + len}));
            return res;
        }

        // encode data
        encoder_->push(ptr, len);

        try {
            while(true) {
                auto buf = encoder_->encode();

                if(buf.empty()) {
                    break;
                }

                res.emplace_back(std::make_unique<AudioPacket::Data>(std::move(buf)));
            }
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", __FUNCTION__, err.what());
            sock_.close();
        }

        return res;
    }

    /// AudioSessionBus
    AudioSessionBus::AudioSessionBus(DBusConnectionPtr conn, bool debug) : ApplicationLog("ltsm_session_audio"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(*conn, sdbus::ObjectPath {dbus_session_audio_path}),
#else
        AdaptorInterfaces(*conn, dbus_session_audio_path),
#endif
        signals_ {ioc_}, work_guard_ {asio::make_work_guard(ioc_)}, dbus_conn_ {std::move(conn)} {
        registerAdaptor();

        if(debug) {
            Application::setDebugLevel(DebugLevel::Debug);
        }
    }

    AudioSessionBus::~AudioSessionBus() {
        unregisterAdaptor();
        stop();
    }

    void AudioSessionBus::stop(void) {
        clients_.clear();
        signals_.cancel();
        work_guard_.reset();
        dbus_conn_->leaveEventLoop();
    }

    int AudioSessionBus::start(void) {

        Application::info("service started, uid: {}, pid: {}, version: {}", getuid(), getpid(), LTSM_SESSION_AUDIO_VERSION);

        auto sdbus_job = std::async(std::launch::async, [this]() {
            try {
                dbus_conn_->enterEventLoop();
            } catch(const std::exception & err) {
                Application::error("sdbus exception: {}", err.what());
                asio::post(ioc_, std::bind(&AudioSessionBus::stop, this));
            }
        });

        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        signals_.async_wait([this](const system::error_code & ec, int signal) {
            // skip canceled
            if(ec != asio::error::operation_aborted && (signal == SIGTERM || signal == SIGINT)) {
                this->stop();
            }
        });

        ioc_.run();

        dbus_conn_->leaveEventLoop();
        sdbus_job.wait();

        Application::debug(DebugType::App, "service stopped");
        return EXIT_SUCCESS;
    }

    int32_t AudioSessionBus::getVersion(void) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);
        return LTSM_SESSION_AUDIO_VERSION;
    }

    void AudioSessionBus::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "{}: pid: {}", __FUNCTION__, getpid());
        asio::post(ioc_, std::bind(&AudioSessionBus::stop, this));
    }

    void AudioSessionBus::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", __FUNCTION__, level);
        setDebugLevel(level);
    }

    bool AudioSessionBus::connectChannel(const std::string & socketPath) {
        Application::debug(DebugType::Dbus, "{}: socket path: `{}'", __FUNCTION__, socketPath);

        if(std::ranges::any_of(clients_, [&](auto & cli) {
                return cli->socketPath(socketPath) && cli->socketConnected(); })) {
            Application::error("{}: socket busy, path: `{}'", __FUNCTION__, socketPath);
            return false;
        }

        asio::co_spawn(ioc_,
        [socketPath, this]() -> asio::awaitable<void>  {
            auto executor = co_await asio::this_coro::executor;

            asio::local::stream_protocol::socket sock{executor};
            auto strand = asio::make_strand(executor);
            auto client = std::make_unique<AudioClient>(std::move(sock), std::move(strand));

            try {
                co_await client->retryConnect(socketPath, 5);
                co_await client->remoteHandshake();
                clients_.emplace_front(std::move(client));
            } catch(const system::error_code& ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "remoteHandshake", "asio", ec.value(), ec.message());
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", __FUNCTION__, "remoteHandshake", err.what());
            }

        }, asio::detached);

        return true;
    }

    void AudioSessionBus::disconnectChannel(const std::string & socketPath) {
        Application::debug(DebugType::Dbus, "{}: socket path: `{}'", __FUNCTION__, socketPath);
        std::erase_if(clients_, [&socketPath](auto & cli) {
            return cli->socketPath(socketPath);
        });
    }
}

int main(int argc, char** argv) {
    bool debug = false;

    for(int it = 1; it < argc; ++it) {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h")) {
            std::cout << "usage: " << argv[0] << "[--debug] [--version]" << std::endl;
            return EXIT_SUCCESS;
        } else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v")) {
            std::cout << "version: " << LTSM_SESSION_AUDIO_VERSION << std::endl;
            return EXIT_SUCCESS;
        } else if(0 == std::strcmp(argv[it], "--debug") || 0 == std::strcmp(argv[it], "-d")) {
            debug = true;
        }
    }

    if(0 == getuid()) {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    try {
#ifdef SDBUS_2_0_API
        auto conn = sdbus::createSessionBusConnection(sdbus::ServiceName {LTSM::dbus_session_audio_name});
#else
        auto conn = sdbus::createSessionBusConnection(LTSM::dbus_session_audio_name);
#endif
        return LTSM::AudioSessionBus(std::move(conn), debug).start();
    } catch(const sdbus::Error & err) {
        LTSM::Application::error("sdbus: [{}] {}", err.getName(), err.getMessage());
    } catch(const std::exception & err) {
        LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    return EXIT_FAILURE;
}
