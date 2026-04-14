/***************************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 **************************************************************************/

#include <chrono>
#include <string>
#include <memory>
#include <filesystem>

#include "ltsm_audio.h"
#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_channels.h"
#include "ltsm_application.h"
#include "ltsm_audio_decoder.h"

#include "librfb_client.h"

#ifdef LTSM_WITH_PLAYBACK_OPENAL
#include "ltsm_audio_openal.h"
#endif

namespace LTSM {
    namespace Channel {
        namespace Connector {
            // channel_system.cpp
            void loopWriter(ConnectorBase*, Remote2Local*);
            void loopReader(ConnectorBase*, Local2Remote*);
        }
    }
}

using namespace std::chrono_literals;

// createClientAudioConnector
std::unique_ptr<LTSM::Channel::ConnectorBase> LTSM::Channel::createClientAudioConnector(uint8_t channel,
        const std::string & url, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender) {
    Application::info("{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url,
                      Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::Unknown) {
        Application::error("{}: {}, mode: {}", NS_FuncNameV, "audio mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncNameS);
    }

    return std::make_unique<ConnectorClientAudio>(channel, url, mode, chOpts, sender);
}

/// ConnectorClientAudio
LTSM::Channel::ConnectorClientAudio::ConnectorClientAudio(uint8_t ch, const std::string & url,
        const ConnectorMode & mod, const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, mod, chOpts, srv), cid(ch) {
    Application::info("{}: channelId: {}", NS_FuncNameV, cid);
    // start threads
    setRunning(true);
}

LTSM::Channel::ConnectorClientAudio::~ConnectorClientAudio() {
    setRunning(false);
}

int LTSM::Channel::ConnectorClientAudio::error(void) const {
    return 0;
}

uint8_t LTSM::Channel::ConnectorClientAudio::channel(void) const {
    return cid;
}

void LTSM::Channel::ConnectorClientAudio::setSpeed(const Channel::Speed & speed) {
}

void LTSM::Channel::ConnectorClientAudio::pushData(std::vector<uint8_t> && recv) {
    Application::debug(DebugType::Audio, "{}: size: {}", NS_FuncNameV, recv.size());
    StreamBufRef sb;

    if(last.empty()) {
        sb.reset(recv.data(), recv.size());
    } else {
        std::ranges::copy(recv, std::back_inserter(last));
        recv.swap(last);
        sb.reset(recv.data(), recv.size());
        last.clear();
    }

    const uint8_t* beginPacket = nullptr;
    const uint8_t* endPacket = nullptr;

    try {
        while(2 < sb.last()) {
            // audio stream format:
            // <CMD16> - audio cmd
            // <DATA> - audio data
            beginPacket = sb.data();
            endPacket = beginPacket + sb.last();

            auto audioCmd = sb.readIntLE16();
            Application::debug(DebugType::Audio, "{}: cmd: {:#06x}", NS_FuncNameV, audioCmd);

            if(AudioOp::Init == audioCmd) {
                if(! audioOpInit(sb)) {
                    throw channel_error(NS_FuncNameS);
                }
            } else if(AudioOp::Data == audioCmd) {
                audioOpData(sb);
            } else if(AudioOp::Silent == audioCmd) {
                audioOpSilent(sb);
            } else {
                Application::error("{}: {} failed, cmd: {:#06x}, recv size: {}", NS_FuncNameV, "audio", audioCmd, recv.size());
                throw channel_error(NS_FuncNameS);
            }
        }

        if(sb.last()) {
            throw std::underflow_error(NS_FuncNameS);
        }
    } catch(const std::underflow_error & err) {
        Application::warning("{}: underflow data: {}, func: {}", NS_FuncNameV, sb.last(), err.what());

        if(beginPacket) {
            last.assign(beginPacket, endPacket);
        } else {
            last.swap(recv);
        }
    }
}

bool LTSM::Channel::ConnectorClientAudio::audioOpInit(const StreamBufRef & sb) {
    // audio format:
    // <VER16> - proto ver
    // <NUM16> - encodings
    if(4 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    auto protoVer = sb.readIntLE16();
    auto numEnc = sb.readIntLE16();

    if(protoVer != AudioOp::ProtoVer) {
        Application::error("{}: unsupported version: {}", NS_FuncNameV, protoVer);
        throw audio_error(NS_FuncNameS);
    }

    Application::info("{}: server proto version: {}, encodings count: {}", NS_FuncNameV, protoVer, numEnc);
    formats.clear();

    if(numEnc * 10 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    while(0 < numEnc) {
        auto type = sb.readIntLE16();
        auto channels = sb.readIntLE16();
        auto samplePerSec = sb.readIntLE32(); // 44100, 22050, other
        auto bitsPerSample = sb.readIntLE16();
        formats.emplace_front(AudioFormat{ .type = type, .channels = channels, .samplePerSec = samplePerSec, .bitsPerSample = bitsPerSample });
        numEnc--;
    }

    std::string error;
#ifdef LTSM_WITH_OPUS

    int prefferedAudioEnc = RFB::ENCODING_LTSM_OPUS;
    
    if(auto rfb = dynamic_cast<const RFB::ClientDecoder*>(owner)) {
        // opus or pcm
        if(rfb->clientPrefferedAudioEncoding()) {
            prefferedAudioEnc = rfb->clientPrefferedAudioEncoding();
        }
    }

    if(! format && (prefferedAudioEnc == RFB::ENCODING_LTSM_OPUS)) {
        auto it = std::ranges::find_if(formats, [](auto & fmt) {
            return fmt.type == AudioEncoding::OPUS;
        });

        if(it != formats.end()) {
            format = std::addressof(*it);

            try {
                decoder = std::make_unique<AudioDecoder::Opus>(format->samplePerSec, format->channels, format->bitsPerSample);
                Application::info("{}: select encoding: `{}'", NS_FuncNameV, "OPUS");
            } catch(const std::exception &) {
                format = nullptr;
            }
        }
    }

#endif

    if(! format) {
        auto it = std::ranges::find_if(formats, [](auto & fmt) {
            return fmt.type == AudioEncoding::PCM;
        });

        if(it != formats.end()) {
            format = std::addressof(*it);
            Application::info("{}: select encoding: `{}'", NS_FuncNameV, "PCM");
        } else {
            error.assign("PCM format not found");
        }
    }

    // reply
    StreamBuf reply(32);
    reply.writeIntLE16(AudioOp::Init);

    if(! format) {
        reply.writeIntLE16(error.size());
        reply.write(error);
        owner->sendLtsmChannelData(cid, reply.rawbuf());
        return false;
    }

    Application::info("{}: audio format: channels: {}, samples: {}, bits: {}",
                      NS_FuncNameV, format->channels, format->samplePerSec, format->bitsPerSample);

#ifdef LTSM_WITH_PLAYBACK_OPENAL
    Application::info("{}: audio playback: {}", NS_FuncNameV, "OpenAL");

    try {
            player = std::make_unique<OpenAL::Playback>(*format, 1 /* buffer sec, and autoplay */);
    } catch(const std::exception &) {
            error.assign("openal failed");
    }

#else
    throw std::runtime_error("unknown audio playback");
#endif

    if(! player) {
        reply.writeIntLE16(error.size());
        reply.write(error);
        owner->sendLtsmChannelData(cid, reply.rawbuf());
        return false;
    }

    // no errors
    reply.writeIntLE16(0);
    // proto ver
    reply.writeIntLE16(AudioOp::ProtoVer);
    // encoding type
    reply.writeIntLE16(format->type);
    owner->sendLtsmChannelData(cid, reply.rawbuf());
    return true;
}

void LTSM::Channel::ConnectorClientAudio::audioOpSilent(const StreamBufRef & sb) {
    if(4 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    auto len = sb.readIntLE32();
    Application::debug(DebugType::Audio, "{}: data size: {}", NS_FuncNameV, len);

    if(silent) {
        if(std::chrono::steady_clock::now() - *silent < 3s) {
            std::vector<uint8_t> buf(len, 0);
            player->streamWrite(buf);
        } else if(player->isPlaying()) {
            Application::info("{}: play stop", NS_FuncNameV);
            player->playStop();
        }
    } else {
        silent = std::make_unique<TimePoint>(std::chrono::steady_clock::now());
        std::vector<uint8_t> buf(len, 0);
        player->streamWrite(buf);
    }
}

void LTSM::Channel::ConnectorClientAudio::audioOpData(const StreamBufRef & sb) {
    if(4 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    auto len = sb.readIntLE32();
    Application::debug(DebugType::Audio, "{}: data size: {}", NS_FuncNameV, len);

    if(len > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    if(silent) {
        silent.reset();
    }

    std::span<const uint8_t> span{sb.data(), len};

    if(! decoder) {
        player->streamWrite(span);
    } else if(auto buf = decoder->decode(span); !buf.empty()) {
        Application::debug(DebugType::Audio, "{}: decode size: {}", NS_FuncNameV, buf.size());

        if(auto env = getenv("LTSM_AUDIO_SAVE_FILE")) {
            Tools::binaryToFile(buf.data(), buf.size(), env, true);
        }

        player->streamWrite(buf);
    }

    sb.skip(len);
}
