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

namespace LTSM
{
    namespace Channel
    {
        namespace Connector
        {
            // channel_system.cpp
            void loopWriter(ConnectorBase*, Remote2Local*);
            void loopReader(ConnectorBase*, Local2Remote*);
        }
    }
}

using namespace std::chrono_literals;

// createClientAudioConnector
std::unique_ptr<LTSM::Channel::ConnectorBase> LTSM::Channel::createClientAudioConnector(uint8_t channel,
        const std::string & url, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", url: `%s', mode: %s", __FUNCTION__, channel, url.c_str(),
                      Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::Unknown)
    {
        Application::error("%s: %s, mode: %s", __FUNCTION__, "audio mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<ConnectorClientAudio>(channel, url, mode, chOpts, sender);
}

/// ConnectorClientAudio
LTSM::Channel::ConnectorClientAudio::ConnectorClientAudio(uint8_t ch, const std::string & url,
        const ConnectorMode & mod, const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, mod, chOpts, srv), cid(ch)
{
    Application::info("%s: channelId: %" PRIu8, __FUNCTION__, cid);
    // start threads
    setRunning(true);
}

LTSM::Channel::ConnectorClientAudio::~ConnectorClientAudio()
{
    setRunning(false);
}

int LTSM::Channel::ConnectorClientAudio::error(void) const
{
    return 0;
}

uint8_t LTSM::Channel::ConnectorClientAudio::channel(void) const
{
    return cid;
}

void LTSM::Channel::ConnectorClientAudio::setSpeed(const Channel::Speed & speed)
{
}

void LTSM::Channel::ConnectorClientAudio::pushData(std::vector<uint8_t> && recv)
{
    Application::debug(DebugType::Audio, "%s: size: %u", __FUNCTION__, recv.size());
    StreamBufRef sb;

    if(last.empty())
    {
        sb.reset(recv.data(), recv.size());
    }
    else
    {
        std::copy(recv.begin(), recv.end(), std::back_inserter(last));
        recv.swap(last);
        sb.reset(recv.data(), recv.size());
        last.clear();
    }

    const uint8_t* beginPacket = nullptr;
    const uint8_t* endPacket = nullptr;

    try
    {
        while(2 < sb.last())
        {
            // audio stream format:
            // <CMD16> - audio cmd
            // <DATA> - audio data
            beginPacket = sb.data();
            endPacket = beginPacket + sb.last();
            auto audioCmd = sb.readIntLE16();
            Application::debug(DebugType::Audio, "%s: cmd: 0x%" PRIx16, __FUNCTION__, audioCmd);

            if(AudioOp::Init == audioCmd)
            {
                if(! audioOpInit(sb))
                {
                    throw channel_error(NS_FuncName);
                }
            }
            else if(AudioOp::Data == audioCmd)
            {
                audioOpData(sb);
            }
            else if(AudioOp::Silent == audioCmd)
            {
                audioOpSilent(sb);
            }
            else
            {
                Application::error("%s: %s failed, cmd: 0x%" PRIx16 ", recv size: %u", __FUNCTION__, "audio", audioCmd, recv.size());
                throw channel_error(NS_FuncName);
            }
        }

        if(sb.last())
        {
            throw std::underflow_error(NS_FuncName);
        }
    }
    catch(const std::underflow_error &)
    {
        Application::warning("%s: underflow data: %u", __FUNCTION__, sb.last());

        if(beginPacket)
        {
            last.assign(beginPacket, endPacket);
        }
        else
        {
            last.swap(recv);
        }
    }
}

bool LTSM::Channel::ConnectorClientAudio::audioOpInit(const StreamBufRef & sb)
{
    // audio format:
    // <VER16> - proto ver
    // <NUM16> - encodings
    if(4 > sb.last())
    {
        throw std::underflow_error(NS_FuncName);
    }

    audioVer = sb.readIntLE16();
    auto numEnc = sb.readIntLE16();
    Application::info("%s: server proto version: %" PRIu16 ", encodings count: %" PRIu16, __FUNCTION__, audioVer, numEnc);
    formats.clear();

    if(numEnc * 10 > sb.last())
    {
        throw std::underflow_error(NS_FuncName);
    }

    while(0 < numEnc)
    {
        auto type = sb.readIntLE16();
        auto channels = sb.readIntLE16();
        auto samplePerSec = sb.readIntLE32();
        auto bitsPerSample = sb.readIntLE16();
        formats.emplace_front(AudioFormat{ .type = type, .channels = channels, .samplePerSec = samplePerSec, .bitsPerSample = bitsPerSample });
        numEnc--;
    }

    std::string error;
#ifdef LTSM_WITH_OPUS

    if(! format)
    {
        auto it = std::find_if(formats.begin(), formats.end(), [](auto & fmt)
        {
            return fmt.type == AudioEncoding::OPUS;
        });

        if(it != formats.end())
        {
            format = std::addressof(*it);

            try
            {
                decoder = std::make_unique<AudioDecoder::Opus>(format->samplePerSec, format->channels, format->bitsPerSample);
                Application::info("%s: select encoding: `%s'", __FUNCTION__, "OPUS");
            }
            catch(const std::exception &)
            {
                format = nullptr;
            }
        }
    }

#endif

    if(! format)
    {
        auto it = std::find_if(formats.begin(), formats.end(), [](auto & fmt)
        {
            return fmt.type == AudioEncoding::PCM;
        });

        if(it != formats.end())
        {
            format = std::addressof(*it);
            Application::info("%s: select encoding: `%s'", __FUNCTION__, "PCM");
        }
        else
        {
            error.assign("PCM format not found");
        }
    }

    // reply
    StreamBuf reply(32);
    reply.writeIntLE16(AudioOp::Init);

    if(! format)
    {
        reply.writeIntLE16(error.size());
        reply.write(error);
        owner->sendLtsmChannelData(cid, reply.rawbuf());
        return false;
    }

    try
    {
        pa_sample_format fmt = PA_SAMPLE_INVALID;

        switch(format->bitsPerSample)
        {
            case 16:
                fmt = PA_SAMPLE_S16LE;
                break;

            case 24:
                fmt = PA_SAMPLE_S24LE;
                break;

            case 32:
                fmt = PA_SAMPLE_S32LE;
                break;

            default:
                break;
        }

        pulse = std::make_unique<PulseAudio::Playback>("LTSM_client", "LTSM Audio Input", fmt, format->samplePerSec,
                format->channels);
    }
    catch(const std::exception &)
    {
        error.assign("pulseaudio failed");
    }

    if(! pulse)
    {
        reply.writeIntLE16(error.size());
        reply.write(error);
        owner->sendLtsmChannelData(cid, reply.rawbuf());
        return false;
    }

    // no errors
    reply.writeIntLE16(0);
    // proto ver
    reply.writeIntLE16(1);
    // encoding type
    reply.writeIntLE16(format->type);
    owner->sendLtsmChannelData(cid, reply.rawbuf());
    return true;
}

void LTSM::Channel::ConnectorClientAudio::audioOpSilent(const StreamBufRef & sb)
{
    if(4 > sb.last())
    {
        throw std::underflow_error(NS_FuncName);
    }

    auto len = sb.readIntLE32();
    Application::debug(DebugType::Audio, "%s: data size: %u", __FUNCTION__, len);
    std::vector<uint8_t> buf(len, 0);
    pulse->streamWrite(buf.data(), buf.size());
}

void LTSM::Channel::ConnectorClientAudio::audioOpData(const StreamBufRef & sb)
{
    if(4 > sb.last())
    {
        throw std::underflow_error(NS_FuncName);
    }

    auto len = sb.readIntLE32();
    Application::debug(DebugType::Audio, "%s: data size: %u", __FUNCTION__, len);

    if(len > sb.last())
    {
        throw std::underflow_error(NS_FuncName);
    }

    if(decoder)
    {
        if(decoder->decode(sb.data(), len))
        {
            pulse->streamWrite(decoder->data(), decoder->size());
        }
    }
    else
    {
        pulse->streamWrite(sb.data(), len);
    }

    sb.skip(len);
}
