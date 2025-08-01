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
#include "ltsm_audio_decoder.h"

namespace LTSM
{
#ifdef LTSM_WITH_OPUS
    AudioDecoder::Opus::Opus(uint32_t samplesPerSec, uint16_t audioChannels, uint16_t bitsPerSample)
        : sampleLength(audioChannels * (bitsPerSample >> 3))
    {
        if(bitsPerSample != sizeof(opus_int16) * 8)
        {
            Application::error("%s: %s failed", __FUNCTION__, "bitsPerSample");
            throw audio_error(NS_FuncName);
        }

        int error = OPUS_OK;
        ctx.reset(opus_decoder_create(samplesPerSec, audioChannels, & error));

        if(! ctx || error != OPUS_OK)
        {
            Application::error("%s: %s failed, error: %d, sampleRate: %" PRIu32 ", audioChannels: %" PRIu16, __FUNCTION__,
                               "opus_decoder_create", error, samplesPerSec, audioChannels);
            throw audio_error(NS_FuncName);
        }
    }

    bool AudioDecoder::Opus::decode(const uint8_t* ptr, size_t len)
    {
        int frames = opus_decoder_get_nb_samples(ctx.get(), ptr, len);

        if(0 > frames)
        {
            Application::error("%s: %s failed, error: %d, data size: %u", __FUNCTION__, "opus_decoder_get_nb_samples", frames, len);
            throw audio_error(NS_FuncName);
        }

        if(0 == frames)
        {
            return false;
        }

        tmp.resize(frames* sampleLength);
        int nSamples = opus_decode(ctx.get(), ptr, len, (opus_int16*) tmp.data(), frames, 0);

        if(nSamples < 0)
        {
            Application::error("%: %s failed, error: %d", __FUNCTION__, "opus_decode", nSamples);
            return false;
        }

        decodeSize = nSamples * sampleLength;
        return true;
    }

    const uint8_t* AudioDecoder::Opus::data(void) const
    {
        return tmp.data();
    }

    size_t AudioDecoder::Opus::size(void) const
    {
        if(decodeSize > tmp.size())
        {
            Application::error("%s: out of range, size: %u, buf: %u", __FUNCTION__, decodeSize, tmp.size());
            throw audio_error(NS_FuncName);
        }

        return decodeSize;
    }

#endif
}
