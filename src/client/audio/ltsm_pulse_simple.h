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

#ifndef _LTSM_PULSE_PLAYBACK_
#define _LTSM_PULSE_PLAYBACK_

#include <string>
#include <vector>

#include "pulse/simple.h"
#include "ltsm_audio.h"

namespace LTSM {
    namespace PulseAudio {
        struct SimpleDeleter {
            void operator()(pa_simple* ctx) {
                pa_simple_free(ctx);
            }
        };

        class Simple {
          protected:
            pa_sample_spec audioSpec = { .format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 2 };
            std::unique_ptr<pa_simple, SimpleDeleter> ctx;

          public:
            Simple() = default;
            virtual ~Simple() = default;

            bool streamFlush(void) const;
            pa_usec_t getLatency(void) const;
        };

        class Playback : public Simple, public AudioPlayer {
          public:
            Playback(const std::string & appName, const std::string & streamName,
                     const AudioFormat &, const pa_buffer_attr* attr = nullptr);

            bool streamWrite(const uint8_t*, size_t) const override;
            bool streamDrain(void) const;
        };

        class Record : public Simple {
          public:
            Record(const std::string & appName, const std::string & streamName, const pa_sample_format_t &,
                   uint32_t rate, uint8_t channels, const pa_buffer_attr* attr = nullptr);

            std::vector<uint8_t> streamRead(size_t) const;
        };
    }
}

#endif // _LTSM_AUDIO_PULSE_
