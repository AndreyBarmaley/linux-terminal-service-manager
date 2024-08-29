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

#include <array>
#include <vector>
#include <memory>

#ifdef LTSM_WITH_OPUS
#include "opus/opus.h"
#endif

namespace LTSM
{
    namespace AudioEncoder
    {
        class BaseEncoder
        {
        public:
            BaseEncoder() = default;
            virtual ~BaseEncoder() = default;

            virtual bool encode(const uint8_t* ptr, size_t len) = 0;

            virtual const uint8_t* data(void) const = 0;
            virtual size_t size(void) const = 0;
        };

#ifdef LTSM_WITH_OPUS
        struct OpusDeleter
        {
            void operator()(OpusEncoder* st)
            {
                opus_encoder_destroy(st);
            }
        };

        class Opus : public BaseEncoder
        {
            std::unique_ptr<OpusEncoder, OpusDeleter> ctx;

            // ref: https://www.opus-codec.org/docs/html_api/group__opusencoder.html
            // max_packet is the maximum number of bytes that can be written in the packet (1276 bytes is recommended)
            std::array<uint8_t, 1276> tmp;
            size_t encodeSize = 0;

            // Opus: frame size - at 48kHz the permitted values are 120, 240, 480, or 960, the remainder will be stored here.
            std::vector<uint8_t> last;

            const size_t framesCount;
            const size_t sampleLength;

        public:
            Opus(uint32_t samplesPerSec, uint16_t audioChannels, uint16_t bitsPerSample, uint16_t frames = 480);

            bool encode(const uint8_t* ptr, size_t len) override;

            const uint8_t* data(void) const override;
            size_t size(void) const override;
        };

#endif
    }
}

#endif // _LTSM_AUDIO_ENCODER_
