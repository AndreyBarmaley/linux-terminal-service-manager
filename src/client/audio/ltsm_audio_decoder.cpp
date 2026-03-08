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

namespace LTSM {
#ifdef LTSM_WITH_OPUS
    AudioDecoder::Opus::Opus(uint32_t samplesPerSec, uint8_t audioChannels, uint8_t bitsPerSample)
        : sampleLength(audioChannels * (bitsPerSample >> 3)) {
        if(bitsPerSample != sizeof(opus_int16) * 8) {
            Application::error("{}: {} failed", __FUNCTION__, "bitsPerSample");
            throw audio_error(NS_FuncNameS);
        }

        int error = OPUS_OK;
        ctx.reset(opus_decoder_create(samplesPerSec, audioChannels, & error));

        if(! ctx || error != OPUS_OK) {
            Application::error("{}: {} failed, error: {}, sampleRate: {}, audioChannels: {}", __FUNCTION__,
                               "opus_decoder_create", error, samplesPerSec, audioChannels);
            throw audio_error(NS_FuncNameS);
        }
    }

    std::vector<uint8_t> AudioDecoder::Opus::decode(const uint8_t* ptr, size_t len) {
        int frames = opus_decoder_get_nb_samples(ctx.get(), ptr, len);

        if(0 > frames) {
            Application::error("{}: {} failed, error: {}, data size: {}", __FUNCTION__, "opus_decoder_get_nb_samples", frames, len);
            throw audio_error(NS_FuncNameS);
        }

        if(0 == frames) {
            Application::warning("{}: {} failed, empty frames", __FUNCTION__, "opus_decoder_get_nb_samples");
            return {};
        }

        std::vector<uint8_t> tmp(frames * sampleLength);
        int nSamples = opus_decode(ctx.get(), ptr, len, (opus_int16*) tmp.data(), frames, 0);

        if(nSamples < 0) {
            Application::error("{}: {} failed, error: {}", __FUNCTION__, "opus_decode", nSamples);
            tmp.clear();
        } else {
            tmp.resize(nSamples * sampleLength);
            Application::trace(DebugType::Audio, "{}: decode samples: {}, size: {}", __FUNCTION__, nSamples, tmp.size());
        }

        return tmp;
    }

#endif
}
