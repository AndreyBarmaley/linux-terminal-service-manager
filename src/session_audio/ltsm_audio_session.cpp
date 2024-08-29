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
#include <atomic>
#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_audio.h"
#include "ltsm_sockets.h"
#include "ltsm_audio_session.h"

using namespace std::chrono_literals;

namespace LTSM
{
    std::unique_ptr<sdbus::IConnection> conn;
    std::atomic<bool> shutdownAudio{false};

    void signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
        {
            shutdownAudio = true;

            if(conn)
            {
                conn->leaveEventLoop();
            }
        }
    }

    /// AudioClient
    bool audioClientSocketInitialize(AudioClient* client)
    {
        std::error_code fserr;

        while(! client->shutdown)
        {
            // wait socket
            if(std::filesystem::is_socket(client->socketPath, fserr))
            {
                if(int fd = UnixSocket::connect(client->socketPath); 0 < fd)
                {
                    client->sock = std::make_unique<SocketStream>(fd);
                    break;
                }

                std::this_thread::sleep_for(100ms);
            }
        }

        if(client->shutdown)
        {
            return false;
        }

        if(! client->sock)
        {
            Application::error("%s: %s failed", __FUNCTION__, "socket");
            return false;
        }

        const pa_sample_format_t defaultFormat = PA_SAMPLE_S16LE;
        const uint8_t defaultChannels = 2;
        const uint16_t bitsPerSample = PulseAudio::formatBits(defaultFormat);
        // send initialize packet
        client->sock->sendIntLE16(AudioOp::Init);
        // send proto ver
        client->sock->sendIntLE16(1);
        int numenc = 1;
#ifdef LTSM_WITH_OPUS
        numenc++;
#endif
        // send num encodings
        client->sock->sendIntLE16(numenc);
        // encoding type PCM
        client->sock->sendIntLE16(AudioEncoding::PCM);
        client->sock->sendIntLE16(defaultChannels);
        client->sock->sendIntLE32(44100);
        client->sock->sendIntLE16(bitsPerSample);
#ifdef LTSM_WITH_OPUS
        // encoding type OPUS
        client->sock->sendIntLE16(AudioEncoding::OPUS);
        client->sock->sendIntLE16(defaultChannels);
        client->sock->sendIntLE32(48000);
        client->sock->sendIntLE16(bitsPerSample);
#endif
        client->sock->sendFlush();
        // client reply
        auto cmd = client->sock->recvIntLE16();
        auto err = client->sock->recvIntLE16();

        if(cmd != AudioOp::Init)
        {
            Application::error("%s: %s: failed, cmd: 0x%" PRIx16, __FUNCTION__, "id", cmd);
            return false;
        }

        if(err)
        {
            auto str = client->sock->recvString(err);
            Application::error("%s: recv error: %s", __FUNCTION__, str.c_str());
            return false;
        }

        // proto version
        auto ver = client->sock->recvIntLE16();
        // encoding
        auto enc = client->sock->recvIntLE16();
        Application::info("%s: client proto version: %" PRIu16 ", encode type: 0x%" PRIx16, __FUNCTION__, ver, enc);
        uint32_t defaultBitRate = 44100;
        uint32_t bufFragSize = 1024;
        // Opus: frame counts - at 48kHz the permitted values are 120, 240, 480, or 960
        const uint32_t opusFrames = 480;

        if(enc == AudioEncoding::OPUS)
        {
#ifdef LTSM_WITH_OPUS
            defaultBitRate = 48000;
            const uint32_t opusFrameLength = defaultChannels * bitsPerSample / 8;
            bufFragSize = opusFrames * opusFrameLength;
#endif
        }

        // PulseAudio init loop
        while(! client->shutdown)
        {
            try
            {
                client->pulse = std::make_unique<PulseAudio::OutputStream>(defaultFormat, defaultBitRate, defaultChannels);
            }
            catch(const std::exception & err)
            {
            }

            if(client->pulse && client->pulse->initContext())
            {
                const pa_buffer_attr bufferAttr = { bufFragSize, UINT32_MAX, UINT32_MAX, UINT32_MAX, bufFragSize };

                if(client->pulse->streamConnect(false /* not paused */, & bufferAttr))
                {
                    break;
                }
            }
            else
            {
                LTSM::Application::warning("%s: wait pulseaudio", __FUNCTION__);
                std::this_thread::sleep_for(1s);
            }
        }

        if(enc == AudioEncoding::OPUS)
        {
#ifdef LTSM_WITH_OPUS
            client->encoder = std::make_unique<AudioEncoder::Opus>(defaultBitRate, defaultChannels, bitsPerSample, opusFrames);
            Application::info("%s: selected encoder: %s", __FUNCTION__, "OPUS");
#else
            Application::error("%s: unsupported encoder: %s", __FUNCTION__, "OPUS");
            throw audio_error(NS_FuncName);
#endif
        }
        else
        {
            Application::info("%s: selected encoder: %s", __FUNCTION__, "PCM");
        }

        // transfer loop
        while(! client->shutdown)
        {
            if(! client->pulse->pcmEmpty())
            {
                auto raw = client->pulse->pcmData();
                bool sampleNotSilent = std::any_of(raw.begin(), raw.end(), [](auto & val) { return val != 0; });

                if(sampleNotSilent)
                {
                    if(client->encoder)
                    {
                        if(client->encoder->encode(raw.data(), raw.size()))
                        {
                            // send enc samples
                            client->sock->sendIntLE16(AudioOp::Data);
                            client->sock->sendIntLE32(client->encoder->size());
                            client->sock->sendRaw(client->encoder->data(), client->encoder->size());
                            client->sock->sendFlush();
                        }
                    }
                    else
                    {
                        // send raw samples
                        client->sock->sendIntLE16(AudioOp::Data);
                        client->sock->sendIntLE32(raw.size());
                        client->sock->sendData(raw);
                        client->sock->sendFlush();
                    }
                }
                else
                {
                    client->sock->sendIntLE16(AudioOp::Silent);
                    client->sock->sendIntLE32(raw.size());
                    client->sock->sendFlush();
                }
            }

            std::this_thread::sleep_for(3ms);
        }

        return true;
    }

    AudioClient::AudioClient(const std::string & path) : socketPath(path)
    {
        thread = std::thread([this]()
        {
            try
            {
                if(audioClientSocketInitialize(this))
                {
                    return;
                }
            }
            catch(const std::exception & err)
            {
                LTSM::Application::error("%s: exception: %s", "AudioClientThread", err.what());
            }

            this->shutdown = true;
        });
    }

    AudioClient::~AudioClient()
    {
        shutdown = true;

        if(thread.joinable())
        {
            thread.join();
        }
    }

    /// AudioSessionBus
    AudioSessionBus::AudioSessionBus(sdbus::IConnection & conn)
        : AdaptorInterfaces(conn, dbus_session_audio_path), Application("ltsm_audio2session")
    {
        //setDebug(DebugTarget::Console, DebugLevel::Debug);
        Application::setDebug(DebugTarget::Syslog, DebugLevel::Info);
        Application::info("started, uid: %d, pid: %d, version: %d", getuid(), getpid(), LTSM_AUDIO2SESSION_VERSION);
        registerAdaptor();
    }

    AudioSessionBus::~AudioSessionBus()
    {
        unregisterAdaptor();
    }

    int AudioSessionBus::start(void)
    {
        signal(SIGTERM, signalHandler);
        signal(SIGINT, signalHandler);

        while(! shutdownAudio)
        {
            conn->enterEventLoopAsync();
            std::this_thread::sleep_for(5ms);
        }

        return EXIT_SUCCESS;
    }

    int32_t AudioSessionBus::getVersion(void)
    {
        Application::debug("%s", __FUNCTION__);
        return LTSM_AUDIO2SESSION_VERSION;
    }

    void AudioSessionBus::serviceShutdown(void)
    {
        Application::info("%s", __FUNCTION__);
        shutdownAudio = true;
    }

    void AudioSessionBus::setDebug(const std::string & level)
    {
        setDebugLevel(level);
    }

    bool AudioSessionBus::connectChannel(const std::string & clientSocket)
    {
        Application::info("%s: socket path: `%s'", __FUNCTION__, clientSocket.c_str());

        if(std::any_of(clients.begin(), clients.end(), [&](auto & cli) { return cli.socketPath == clientSocket && !! cli.sock; }))
        {
            Application::error("%s: socket busy, path: `%s'", __FUNCTION__, clientSocket.c_str());
            return false;
        }

        clients.emplace_front(clientSocket);
        return true;
    }

    void AudioSessionBus::disconnectChannel(const std::string & clientSocket)
    {
        Application::info("%s: socket path: `%s'", __FUNCTION__, clientSocket.c_str());
        clients.remove_if([&](auto & cli)
        {
            return cli.socketPath == clientSocket;
        });
    }

    void AudioSessionBus::pulseFragmentSize(const uint32_t & fragsz)
    {
        Application::info("%s: fragment size: %" PRIu32, __FUNCTION__, fragsz);

        for(auto & cl : clients)
            if(cl.pulse)
            {
                cl.pulse->setFragSize(fragsz);
            }
    }
}

int main(int argc, char** argv)
{
    for(int it = 1; it < argc; ++it)
    {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
        {
            std::cout << "usage: " << argv[0] << std::endl;
            return EXIT_SUCCESS;
        }
        else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v"))
        {
            std::cout << "version: " << LTSM_AUDIO2SESSION_VERSION << std::endl;
            return EXIT_SUCCESS;
        }
    }

    if(0 == getuid())
    {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    try
    {
        LTSM::conn = sdbus::createSessionBusConnection(LTSM::dbus_session_audio_name);

        if(! LTSM::conn)
        {
            LTSM::Application::error("dbus connection failed, uid: %d", getuid());
            return EXIT_FAILURE;
        }

        LTSM::AudioSessionBus audioSession(*LTSM::conn);
        return audioSession.start();
    }
    catch(const sdbus::Error & err)
    {
        LTSM::Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());
    }

    return EXIT_FAILURE;
}
