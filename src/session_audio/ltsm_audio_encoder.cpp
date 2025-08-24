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

#include "ltsm_tools.h"
#include "ltsm_audio.h"
#include "ltsm_application.h"
#include "ltsm_audio_encoder.h"

namespace LTSM
{
#ifdef LTSM_WITH_OPUS
    AudioEncoder::Opus::Opus(uint32_t samplesPerSec, uint16_t audioChannels, uint16_t bitsPerSample, uint16_t frames)
        : framesCount(frames), sampleLength(audioChannels * (bitsPerSample >> 3))
    {
        const size_t reserveSize = 256 * 1024;
        last.reserve(reserveSize);
        int error = OPUS_OK;
        ctx.reset(opus_encoder_create(samplesPerSec, audioChannels,
                                      OPUS_APPLICATION_AUDIO /* OPUS_APPLICATION_RESTRICTED_LOWDELAY OPUS_APPLICATION_VOIP OPUS_APPLICATION_AUDIO */,
                                      & error));

        if(! ctx || error != OPUS_OK)
        {
            Application::error("%s: %s failed, error: %d, sampleRate: %" PRIu32 ", audioChannels: %" PRIu16, __FUNCTION__,
                               "opus_encoder_create", error, samplesPerSec, audioChannels);
            throw audio_error(NS_FuncName);
        }

        /*
                error = opus_encoder_ctl(ctx.get(), OPUS_SET_BITRATE(bitRate));
                if(error != OPUS_OK)
                {
                    Application::error("%s: %s failed, error: %d", __FUNCTION__, "opus_encoder_ctl", error);
                    throw audio_error(NS_FuncName);
                }
        */
    }

    bool AudioEncoder::Opus::encode(const uint8_t* ptr, size_t len)
    {
        if(len)
        {
            last.insert(last.end(), ptr, ptr + len);
        }

        const size_t samplesCount = last.size() / sampleLength;

        if(framesCount > samplesCount)
        {
            return false;
        }

        auto src = reinterpret_cast<const opus_int16*>(last.data());
        int nBytes = opus_encode(ctx.get(), src, framesCount, tmp.data(), tmp.size());

        if(nBytes < 0)
        {
            Application::error("%s: %s failed, error: %d", __FUNCTION__, "opus_encode", nBytes);
            return false;
        }

        last.erase(last.begin(), Tools::nextToEnd(last.begin(), framesCount * sampleLength, last.end()));
        encodeSize = nBytes;
        return 0 < encodeSize;
    }

    const uint8_t* AudioEncoder::Opus::data(void) const
    {
        return tmp.data();
    }

    size_t AudioEncoder::Opus::size(void) const
    {
        if(encodeSize > tmp.size())
        {
            Application::error("%s: out of range, size: %lu, buf: %lu", __FUNCTION__, encodeSize, tmp.size());
            throw audio_error(NS_FuncName);
        }

        return encodeSize;
    }

#endif
}
