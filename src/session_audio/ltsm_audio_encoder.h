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

#ifndef _LTSM_AUDIO_ENCODER_
#define _LTSM_AUDIO_ENCODER_

#include <span>
#include <vector>
#include <memory>

#ifdef LTSM_WITH_OPUS
#include "opus/opus.h"
#endif

namespace LTSM {
    namespace AudioEncoder {
        class BaseEncoder {
          public:
            BaseEncoder() = default;
            virtual ~BaseEncoder() = default;

            virtual void push(std::span<const uint8_t>) = 0;
            virtual std::vector<uint8_t> encode(void) = 0;
        };

#ifdef LTSM_WITH_OPUS
        struct OpusDeleter {
            void operator()(OpusEncoder * st) {
                opus_encoder_destroy(st);
            }
        };

        class Opus : public BaseEncoder {
            std::unique_ptr<OpusEncoder, OpusDeleter> ctx;

            // Opus: frame size - at 48kHz the permitted values are 120, 240, 480, or 960, the remainder will be stored here.
            std::vector<uint8_t> last;
            const uint16_t sampleLength;

          public:
            Opus(uint32_t samplesPerSec, uint8_t audioChannels, uint8_t bitsPerSample);

            void push(std::span<const uint8_t>) override;
            std::vector<uint8_t> encode(void) override;
        };

#endif
    }
}

#endif // _LTSM_AUDIO_ENCODER_
