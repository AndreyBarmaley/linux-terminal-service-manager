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

#include <signal.h>

#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_audio.h"
#include "ltsm_global.h"
#include "ltsm_sockets.h"
#include "ltsm_audio_session.h"

using namespace std::chrono_literals;

namespace LTSM {
    std::unique_ptr<sdbus::IConnection> conn;

    void signalHandler(int sig) {
        if(sig == SIGTERM || sig == SIGINT) {
            if(conn) {
                conn->leaveEventLoop();
            }
        }
    }

    /// AudioClient
    bool AudioClient::socketInitialize(void) {
        std::error_code fserr;

        while(! shutdown) {
            // wait socket
            if(std::filesystem::is_socket(socketPath, fserr)) {
                if(int fd = UnixSocket::connect(socketPath); 0 < fd) {
                    sock = std::make_unique<SocketStream>(fd, false /* statistic */);
                    break;
                }

                std::this_thread::sleep_for(100ms);
            }
        }

        if(shutdown) {
            return false;
        }

        if(! sock) {
            Application::error("%s: %s failed", __FUNCTION__, "socket");
            return false;
        }

        const pa_sample_format_t defaultFormat = PA_SAMPLE_S16LE;
        const uint8_t defaultChannels = 2;
        const uint16_t bitsPerSample = PulseAudio::formatBits(defaultFormat);
        // send initialize packet
        sock->sendIntLE16(AudioOp::Init);
        // send proto ver
        sock->sendIntLE16(1);
        int numenc = 1;
#ifdef LTSM_WITH_OPUS
        numenc++;
#endif
        // send num encodings
        sock->sendIntLE16(numenc);
        // encoding type PCM
        sock->sendIntLE16(AudioEncoding::PCM);
        sock->sendIntLE16(defaultChannels);
        sock->sendIntLE32(44100);
        sock->sendIntLE16(bitsPerSample);
#ifdef LTSM_WITH_OPUS
        // encoding type OPUS
        sock->sendIntLE16(AudioEncoding::OPUS);
        sock->sendIntLE16(defaultChannels);
        sock->sendIntLE32(48000);
        sock->sendIntLE16(bitsPerSample);
#endif
        sock->sendFlush();
        // client reply
        auto cmd = sock->recvIntLE16();
        auto err = sock->recvIntLE16();

        if(cmd != AudioOp::Init) {
            Application::error("%s: %s: failed, cmd: 0x%" PRIx16, __FUNCTION__, "id", cmd);
            return false;
        }

        if(err) {
            auto str = sock->recvString(err);
            Application::error("%s: recv error: %s", __FUNCTION__, str.c_str());
            return false;
        }

        // proto version
        auto ver = sock->recvIntLE16();
        // encoding
        auto enc = sock->recvIntLE16();
        Application::info("%s: client proto version: %" PRIu16 ", encode type: 0x%" PRIx16, __FUNCTION__, ver, enc);
        uint32_t defaultBitRate = 44100;
        uint32_t bufFragSize = 1024;
        // Opus: frame counts - at 48kHz the permitted values are 120, 240, 480, or 960
        const uint32_t opusFrames = 480;

        if(enc == AudioEncoding::OPUS) {
#ifdef LTSM_WITH_OPUS
            defaultBitRate = 48000;
            const uint32_t opusFrameLength = defaultChannels * bitsPerSample / 8;
            bufFragSize = opusFrames * opusFrameLength;
#endif
        }

        const pa_buffer_attr bufferAttr = { bufFragSize, UINT32_MAX, UINT32_MAX, UINT32_MAX, bufFragSize };

        try {
            pulse = std::make_unique<PulseAudio::OutputStream>(defaultFormat, defaultBitRate, defaultChannels,
                    std::bind(& AudioClient::pcmDataNotify, this, std::placeholders::_1, std::placeholders::_2));
        } catch(const std::exception & err) {
            return false;
        }

        // wait PulseAudio started
        while(true) {
            if(shutdown) {
                return false;
            }

            if(pulse->initContext()) {
                if(pulse->streamConnect(false /* not paused */, & bufferAttr)) {
                    break;
                }
            } else {
                LTSM::Application::warning("%s: wait pulseaudio", __FUNCTION__);
                std::this_thread::sleep_for(1s);
            }
        }

        if(enc == AudioEncoding::OPUS) {
#ifdef LTSM_WITH_OPUS
            encoder = std::make_unique<AudioEncoder::Opus>(defaultBitRate, defaultChannels, bitsPerSample, opusFrames);
            Application::info("%s: selected encoder: %s", __FUNCTION__, "OPUS");
#else
            Application::error("%s: unsupported encoder: %s", __FUNCTION__, "OPUS");
            throw audio_error(NS_FuncName);
#endif
        } else {
            Application::info("%s: selected encoder: %s", __FUNCTION__, "PCM");
        }

        return true;
    }

    void AudioClient::pcmDataNotify(const uint8_t* ptr, size_t len) {
        bool sampleNotSilent = std::any_of(ptr, ptr + len, [](auto & val) {
            return val != 0;
        });

        if(sampleNotSilent) {
            if(! encoder) {
                // send raw samples
                sock->sendIntLE16(AudioOp::Data);
                sock->sendIntLE32(len);
                sock->sendRaw(ptr, len);
                sock->sendFlush();
            } else if(encoder->encode(ptr, len)) {
                // send enc samples
                sock->sendIntLE16(AudioOp::Data);
                sock->sendIntLE32(encoder->size());
                sock->sendRaw(encoder->data(), encoder->size());
                sock->sendFlush();
            }
        } else {
            sock->sendIntLE16(AudioOp::Silent);
            sock->sendIntLE32(len);
            sock->sendFlush();
        }
    }

    AudioClient::AudioClient(const std::string & path) : socketPath(path) {
        thread = std::thread([this]() {
            try {
                this->socketInitialize();
            } catch(const std::exception & err) {
                LTSM::Application::error("%s: exception: %s", "AudioClientThread", err.what());
            }
        });
    }

    AudioClient::~AudioClient() {
        shutdown = true;

        if(thread.joinable()) {
            thread.join();
        }
    }

    /// AudioSessionBus
    AudioSessionBus::AudioSessionBus(sdbus::IConnection & conn, bool debug) : ApplicationLog("ltsm_session_audio"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(conn, sdbus::ObjectPath {dbus_session_audio_path})
#else
        AdaptorInterfaces(conn, dbus_session_audio_path)
#endif
    {
        registerAdaptor();

        if(debug) {
            Application::setDebugLevel(DebugLevel::Debug);
        }
    }

    AudioSessionBus::~AudioSessionBus() {
        unregisterAdaptor();
    }

    int AudioSessionBus::start(void) {
        Application::info("service started, uid: %d, pid: %d, version: %d", getuid(), getpid(), LTSM_SESSION_AUDIO_VERSION);

        signal(SIGTERM, signalHandler);
        signal(SIGINT, signalHandler);

        conn->enterEventLoop();

        Application::debug(DebugType::App, "service stopped");
        return EXIT_SUCCESS;
    }

    int32_t AudioSessionBus::getVersion(void) {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);
        return LTSM_SESSION_AUDIO_VERSION;
    }

    void AudioSessionBus::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "%s: pid: %s", __FUNCTION__, getpid());
        conn->leaveEventLoop();
    }

    void AudioSessionBus::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "%s: level: %s", __FUNCTION__, level.c_str());
        setDebugLevel(level);
    }

    bool AudioSessionBus::connectChannel(const std::string & clientSocket) {
        Application::debug(DebugType::Dbus, "%s: socket path: `%s'", __FUNCTION__, clientSocket.c_str());

        if(std::any_of(clients.begin(), clients.end(), [ &](auto & cli) {
        return cli.socketPath == clientSocket && ! ! cli.sock;
    })) {
            Application::error("%s: socket busy, path: `%s'", __FUNCTION__, clientSocket.c_str());
            return false;
        }

        clients.emplace_front(clientSocket);
        return true;
    }

    void AudioSessionBus::disconnectChannel(const std::string & clientSocket) {
        Application::debug(DebugType::Dbus, "%s: socket path: `%s'", __FUNCTION__, clientSocket.c_str());
        clients.remove_if([ &](auto & cli) {
            return cli.socketPath == clientSocket;
        });
    }
}

int main(int argc, char** argv) {
    bool debug = false;

    for(int it = 1; it < argc; ++it) {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h")) {
            std::cout << "usage: " << argv[0] << std::endl;
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
        LTSM::conn = sdbus::createSessionBusConnection(sdbus::ServiceName {LTSM::dbus_session_audio_name});
#else
        LTSM::conn = sdbus::createSessionBusConnection(LTSM::dbus_session_audio_name);
#endif

        if(! LTSM::conn) {
            LTSM::Application::error("dbus connection failed, uid: %d", getuid());
            return EXIT_FAILURE;
        }

        LTSM::AudioSessionBus audioSession(*LTSM::conn, debug);
        return audioSession.start();
    } catch(const sdbus::Error & err) {
        LTSM::Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    } catch(const std::exception & err) {
        LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
    }

    return EXIT_FAILURE;
}
