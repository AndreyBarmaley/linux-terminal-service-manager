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

        const pa_sample_format_t defaultFormat = PA_SAMPLE_S16LE;
        const uint8_t defaultChannels = 2;
        const uint16_t bitsPerSample = PulseAudio::formatBits(defaultFormat);

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
        bs.write_le16(defaultChannels);
        bs.write_le32(44100);
        bs.write_le16(bitsPerSample);
#ifdef LTSM_WITH_OPUS
        // encoding type OPUS
        bs.write_le16(AudioEncoding::OPUS);
        bs.write_le16(defaultChannels);
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

        uint32_t defaultBitRate = 44100;
        // Opus: frame counts - at 48kHz the permitted values are 120, 240, 480, or 960
        const uint32_t opusFrames = 480;

        if(enc == AudioEncoding::OPUS) {
#ifdef LTSM_WITH_OPUS
            defaultBitRate = 48000;
            const uint32_t opusFrameLength = defaultChannels * bitsPerSample / 8;
            frag_size_ = opusFrames * opusFrameLength;

            encoder_ = std::make_unique<AudioEncoder::Opus>(defaultBitRate, defaultChannels, bitsPerSample, opusFrames);
            Application::info("{}: selected encoder: {}", __FUNCTION__, "OPUS");
#else
            Application::error("{}: unsupported encoder: {}", __FUNCTION__, "OPUS");
            throw audio_error(NS_FuncNameS);
#endif
        } else {
            Application::info("{}: selected encoder: {}", __FUNCTION__, "PCM");
        }

        try {
            pulse_ = std::make_unique<PulseAudio::OutputStream>(defaultFormat, defaultBitRate, defaultChannels,
                    std::bind(& AudioClient::pcmDataNotify, this, std::placeholders::_1, std::placeholders::_2));
        } catch(const std::exception & err) {
            return false;
        }

        timer_wait_pulse_.expires_after(dur_wait_pulse_);
        timer_wait_pulse_.async_wait(std::bind(&AudioClient::timerWaitPulseStarted, this, std::placeholders::_1));

        return true;
    }

    void AudioClient::timerWaitPulseStarted(const boost::system::error_code & ec) {
        if(ec) {
            return;
        }

        const pa_buffer_attr bufferAttr = { frag_size_, UINT32_MAX, UINT32_MAX, UINT32_MAX, frag_size_ };

        // wait PulseAudio started
        if(pulse_->initContext() && pulse_->streamConnect(false /* not paused */, & bufferAttr)) {
            pulse_ready_ = true;
            // success
            return;
        }

        LTSM::Application::warning("{}: wait pulseaudio", __FUNCTION__);

        timer_wait_pulse_.expires_after(dur_wait_pulse_);
        timer_wait_pulse_.async_wait(std::bind(&AudioClient::timerWaitPulseStarted, this, std::placeholders::_1));
    }

    void AudioClient::pcmDataNotify(const uint8_t* ptr, size_t len) {
        if(! pulse_ready_) {
            LTSM::Application::warning("{}: wait pulseaudio, data len: {}", __FUNCTION__, len);
            return;
        }

        bool sampleNotSilent = std::ranges::any_of(ptr, ptr + len, [](auto & val) { return val != 0; });

        auto sb_ptr = std::make_unique<boost::asio::streambuf>();
        byte::streambuf bs(*sb_ptr);

        if(sampleNotSilent) {
            if(! encoder_) {
                // send raw samples
                bs.write_le16(AudioOp::Data);
                bs.write_le32(len);
                bs.write_bytes(ptr, len);
            } else if(encoder_->encode(ptr, len)) {
                // send enc samples
                bs.write_le16(AudioOp::Data);
                bs.write_le32(encoder_->size());
                bs.write_bytes(encoder_->data(), encoder_->size());
            }
        } else {
            bs.write_le16(AudioOp::Silent);
            bs.write_le32(len);
        }

        boost::asio::async_write(sock_, *sb_ptr, boost::asio::transfer_all(),
            boost::asio::bind_executor(sock_strand_,
                [this, save = std::move(sb_ptr)](boost::system::error_code ec, std::size_t){
                    if(ec) {
                        Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "async_write", ec.value(), ec.message());
                        sock_.close();
                    }
                }
            )
        );
    }

    AudioClient::AudioClient(boost::asio::io_context & ctx, const std::string & path)
        : ioc_{ctx}, socket_path_(path), timer_wait_pulse_{ioc_}, sock_{ioc_}, sock_strand_{boost::asio::make_strand(ioc_)} {

        sock_.async_connect(socket_path_,
            std::bind(&AudioClient::handlerSocketConnect, this, std::placeholders::_1));
    }

    AudioClient::~AudioClient() {
        pulse_ready_ = false;
        sock_.cancel();
        sock_.close();
        timer_wait_pulse_.cancel();
        pulse_.reset();
        encoder_.reset();
    }

    /// AudioSessionBus
    AudioSessionBus::AudioSessionBus(DBusConnectionPtr conn, bool debug) : ApplicationLog("ltsm_session_audio"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(*conn, sdbus::ObjectPath {dbus_session_audio_path}),
#else
        AdaptorInterfaces(*conn, dbus_session_audio_path),
#endif
        ioc_{2}, signals_{ioc_}, dbus_conn_{std::move(conn)}
    {
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
        dbus_conn_->leaveEventLoop();
        signals_.cancel();
        clients_.clear();
    }

    int AudioSessionBus::start(void) {

        Application::info("service started, uid: {}, pid: {}, version: {}", getuid(), getpid(), LTSM_SESSION_AUDIO_VERSION);

        sdbus_job_ = std::async(std::launch::async, [this]()
        {
            dbus_conn_->enterEventLoop();
        });

        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        signals_.async_wait([this](const boost::system::error_code& ec, int signal)
        {
            // skip canceled
            if(ec != boost::asio::error::operation_aborted && (signal == SIGTERM || signal == SIGINT))
            {
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

        if(std::ranges::any_of(clients_, [&](auto & cli) { return cli.socketPath(socketPath) && cli.socketConnected(); })) {
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
