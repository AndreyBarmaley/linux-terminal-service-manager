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

#ifndef _LTSM_AUDIO_DECODER_
#define _LTSM_AUDIO_DECODER_

#include <array>
#include <vector>
#include <memory>

#ifdef LTSM_WITH_OPUS
#include "opus/opus.h"
#endif

namespace LTSM {
    namespace AudioDecoder {
        class BaseDecoder {
          public:
            BaseDecoder() = default;
            virtual ~BaseDecoder() = default;

            virtual std::vector<uint8_t> decode(const uint8_t* ptr, size_t len) = 0;
        };

#ifdef LTSM_WITH_OPUS
        struct OpusDeleter {
            void operator()(OpusDecoder* st) {
                opus_decoder_destroy(st);
            }
        };

        class Opus : public BaseDecoder {
            std::unique_ptr<OpusDecoder, OpusDeleter> ctx;
            const uint8_t sampleLength;

          public:
            Opus(uint32_t samplesPerSec, uint8_t audioChannels, uint8_t bitsPerSample);

            std::vector<uint8_t> decode(const uint8_t* ptr, size_t len) override;
        };

#endif
    }
}

#endif // _LTSM_AUDIO_DECODER_
