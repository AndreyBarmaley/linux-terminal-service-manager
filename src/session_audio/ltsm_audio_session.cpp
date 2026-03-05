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

namespace LTSM {
#ifdef LTSM_WITH_PULSE
    const pa_sample_format_t def_format_pulse = platformBigEndian() ? PA_SAMPLE_S16BE : PA_SAMPLE_S16LE;
#endif
#ifdef LTSM_WITH_PIPEWIRE
    const spa_audio_format def_format_pipew = platformBigEndian() ? SPA_AUDIO_FORMAT_S16_BE : SPA_AUDIO_FORMAT_S16_LE;
#endif

    /// AudioClient
    void AudioClient::handlerSocketConnect(const boost::system::error_code & ec) {
        // wait socket
        if(ec) {
            sock_.async_connect(socket_path_,
                                std::bind(&AudioClient::handlerSocketConnect, this, std::placeholders::_1));
        } else {
            // success
            boost::asio::post(ioc_, std::bind(&AudioClient::clientHandshake, this));
        }
    }

    bool AudioClient::clientHandshake(void) {
        // socket valid
        assert(sock_.is_open());

        const uint8_t channels_ = 2;

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

        boost::asio::streambuf sb;
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
        boost::system::error_code ec;
        boost::asio::write(sock_, sb, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            sock_.close();
            return false;
        }

        auto rlen = boost::asio::read(sock_, sb, boost::asio::transfer_exactly(4), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            sock_.close();
            return false;
        }

        // client reply
        auto cmd = bs.read_le16();
        auto err = bs.read_le16();

        if(cmd != AudioOp::Init) {
            Application::error("{}: {} failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
            return false;
        }

        if(err) {
            boost::asio::read(sock_, sb, boost::asio::transfer_exactly(err), ec);
            auto str = bs.read_string(err);
            Application::error("{}: recv error: {}", __FUNCTION__, str);
            return false;
        }

        boost::asio::read(sock_, sb, boost::asio::transfer_exactly(4), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            sock_.close();
            return false;
        }

        // proto version
        auto ver = bs.read_le16();
        // encoding
        auto enc = bs.read_le16();

        Application::info("{}: client proto version: {}, encode type: {:#04x}", __FUNCTION__, ver, enc);

        // Opus: frame counts - at 48kHz the permitted values are 120, 240, 480, or 960
        const uint32_t opusFrames = 480;

        if(enc == AudioEncoding::OPUS) {
#ifdef LTSM_WITH_OPUS
            const uint32_t opusFrameLength = channels_ * bitsPerSample / 8;
            bit_rate_ = 48000;
            frag_size_ = opusFrames * opusFrameLength;

            encoder_ = std::make_unique<AudioEncoder::Opus>(bit_rate_, channels_, bitsPerSample, opusFrames);
            Application::info("{}: selected encoder: {}", __FUNCTION__, "OPUS");
#else
            Application::error("{}: unsupported encoder: {}", __FUNCTION__, "OPUS");
            throw audio_error(NS_FuncNameS);
#endif
        } else {
            Application::info("{}: selected encoder: {}", __FUNCTION__, "PCM");
        }

        // init engine
        timer_wait_.expires_after(300ms);
        timer_wait_.async_wait(std::bind(&AudioClient::timerWaitEngineStarted, this, std::placeholders::_1));

        return true;
    }

    void AudioClient::timerWaitEngineStarted(const boost::system::error_code & ec) {
        if(ec) {
            return;
        }

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
        if(pipew_ && PW_STREAM_STATE_STREAMING == pipew_->streamState()) {
            // success
            return;
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
            return;
        }

        pulse_.reset();
#else
        return;
#endif
#endif

        LTSM::Application::warning("{}: wait audio engine init...", __FUNCTION__);

        timer_wait_.expires_after(3s);
        timer_wait_.async_wait(std::bind(&AudioClient::timerWaitEngineStarted, this, std::placeholders::_1));
    }

    void AudioClient::dataReadyNotify(const uint8_t* ptr, size_t len) {
        Application::trace(DebugType::Audio, "{}: data size: {}", __FUNCTION__, len);

        std::scoped_lock guard{ queue_lock_ };
        queue_.emplace(ptr, ptr + len);

        if(! sending_) {
            sending_ = true;
            // next part async - to encode & send
            boost::asio::post(ioc_, std::bind(&AudioClient::dataEncodeAndSend, this));
        }
    }

    void AudioPacket::assign(bool silent, QueueData && data) {
        buffers_.clear();

        id_ = boost::endian::native_to_little(static_cast<uint16_t>(silent ? AudioOp::Silent : AudioOp::Data));
        buffers_.emplace_back(&id_, sizeof(id_));

        len_ = boost::endian::native_to_little(static_cast<uint32_t>(data.size()));
        buffers_.emplace_back(&len_, sizeof(len_));

        if(! silent) {
            data_ = std::move(data);
            buffers_.emplace_back(data_.data(), data_.size());
        }
    }

    void AudioClient::dataEncodeAndSend(void) {

        QueueData data;

        if(data.empty()) {
            std::scoped_lock guard{ queue_lock_ };
            data.swap(queue_.front());
            queue_.pop();
        }

        const bool silent = std::ranges::all_of(data, [](auto & val) {
            return val == 0;
        });

        // encode data
        if(! silent && encoder_) {
            if(! encoder_->encode(data.data(), data.size())) {
                Application::error("{}: {} failed", __FUNCTION__, "encoder");
                sock_.close();
                return;
            }

            data = QueueData{encoder_->data(), encoder_->data() + encoder_->size()};
        }

        packet_.assign(silent, std::move(data));

        boost::asio::async_write(sock_, packet_.buffers_, boost::asio::transfer_all(),
                                 std::bind(&AudioClient::dataSendComplete, this, std::placeholders::_1, std::placeholders::_2));
    }

    void AudioClient::dataSendComplete(const boost::system::error_code & ec, size_t sz) {
        if(ec) {
            sock_.close();
            return;
        }

        std::scoped_lock guard{ queue_lock_ };

        if(queue_.empty()) {
            sending_ = false;
            return;
        }

        // next part async - to encode & send
        boost::asio::post(ioc_, std::bind(&AudioClient::dataEncodeAndSend, this));
    }

    AudioClient::AudioClient(boost::asio::io_context & ctx, const std::string & path)
        : ioc_{ctx}, socket_path_(path), timer_wait_{ioc_}, sock_{ioc_} {

        sock_.async_connect(socket_path_,
                            std::bind(&AudioClient::handlerSocketConnect, this, std::placeholders::_1));
    }

    AudioClient::~AudioClient() {
        sock_.cancel();
        sock_.close();
        timer_wait_.cancel();
#ifdef LTSM_WITH_PIPEWIRE
        pipew_.reset();
#endif
#ifdef LTSM_WITH_PULSE
        pulse_.reset();
#endif
        encoder_.reset();
    }

    /// AudioSessionBus
    AudioSessionBus::AudioSessionBus(DBusConnectionPtr conn, bool debug) : ApplicationLog("ltsm_session_audio"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(*conn, sdbus::ObjectPath {dbus_session_audio_path}),
#else
        AdaptorInterfaces(*conn, dbus_session_audio_path),
#endif
        ioc_ {2}, signals_ {ioc_}, dbus_conn_ {std::move(conn)} {
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
        dbus_conn_->leaveEventLoop();
    }

    int AudioSessionBus::start(void) {

        Application::info("service started, uid: {}, pid: {}, version: {}", getuid(), getpid(), LTSM_SESSION_AUDIO_VERSION);

        sdbus_job_ = std::async(std::launch::async, [this]() {
            try {
                dbus_conn_->enterEventLoop();
            } catch(const std::exception & err) {
                Application::error("sdbus exception: {}", err.what());
                boost::asio::post(ioc_, std::bind(&AudioSessionBus::stop, this));
            }
        });

        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        signals_.async_wait([this](const boost::system::error_code & ec, int signal) {
            // skip canceled
            if(ec != boost::asio::error::operation_aborted && (signal == SIGTERM || signal == SIGINT)) {
                this->stop();
            }
        });

        ioc_.run();

        Application::debug(DebugType::App, "service stopped");
        return EXIT_SUCCESS;
    }

    int32_t AudioSessionBus::getVersion(void) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);
        return LTSM_SESSION_AUDIO_VERSION;
    }

    void AudioSessionBus::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "{}: pid: {}", __FUNCTION__, getpid());
        boost::asio::post(ioc_, std::bind(&AudioSessionBus::stop, this));
    }

    void AudioSessionBus::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", __FUNCTION__, level);
        setDebugLevel(level);
    }

    bool AudioSessionBus::connectChannel(const std::string & socketPath) {
        Application::debug(DebugType::Dbus, "{}: socket path: `{}'", __FUNCTION__, socketPath);

        if(std::ranges::any_of(clients_, [&](auto & cli) {
        return cli.socketPath(socketPath) && cli.socketConnected();
        })) {
            Application::error("{}: socket busy, path: `{}'", __FUNCTION__, socketPath);
            return false;
        }

        clients_.emplace_front(ioc_, socketPath);
        return true;
    }

    void AudioSessionBus::disconnectChannel(const std::string & socketPath) {
        Application::debug(DebugType::Dbus, "{}: socket path: `{}'", __FUNCTION__, socketPath);
        std::erase_if(clients_, [&socketPath](auto & cli) {
            return cli.socketPath(socketPath);
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
