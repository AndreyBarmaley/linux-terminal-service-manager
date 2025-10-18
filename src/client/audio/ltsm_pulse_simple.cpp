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

#include "pulse/error.h"

#include "ltsm_tools.h"
#include "ltsm_pulse_simple.h"
#include "ltsm_application.h"

using namespace std::chrono_literals;

namespace LTSM {
    // PulseAudio::Simple
    bool PulseAudio::Simple::streamFlush(void) const {
        int error = 0;

        if(0 != pa_simple_flush(ctx.get(), & error)) {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_flush", pa_strerror(error));
            return false;
        }

        return true;
    }

    pa_usec_t PulseAudio::Simple::getLatency(void) const {
        int error = 0;
        auto res = pa_simple_get_latency(ctx.get(), & error);

        if(error) {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_get_latency", pa_strerror(error));
            return 0;
        }

        return res;
    }

    PulseAudio::Playback::Playback(const std::string & appName, const std::string & streamName,
                                   const AudioFormat & fmt, const pa_buffer_attr* attr) {
        switch(fmt.bitsPerSample) {
            case 16:
                audioSpec.format = PA_SAMPLE_S16LE;
                break;

            case 24:
                audioSpec.format = PA_SAMPLE_S24LE;
                break;

            case 32:
                audioSpec.format = PA_SAMPLE_S32LE;
                break;

            default:
                Application::error("%s: %s failed, bits: %" PRIu16 ", rate: %" PRIu16 ", channels: %" PRIu16,
                                   __FUNCTION__, "AudioFormat", fmt.bitsPerSample, fmt.samplePerSec, fmt.channels);
                throw audio_error(NS_FuncName);
        }

        audioSpec.rate = fmt.samplePerSec;
        audioSpec.channels = fmt.channels;

        if(0 == pa_sample_spec_valid(& audioSpec)) {
            Application::error("%s: %s failed, format: `%s', rate: %" PRIu32 ", channels: %" PRIu8,
                               __FUNCTION__, "pa_sample_spec_valid", pa_sample_format_to_string(audioSpec.format), audioSpec.rate, audioSpec.channels);
            throw audio_error(NS_FuncName);
        }

        int error = 0;
        ctx.reset(pa_simple_new(nullptr, appName.c_str(), PA_STREAM_PLAYBACK,
                                nullptr, streamName.c_str(), & audioSpec, nullptr, attr, & error));

        if(! ctx) {
            Application::error("%s: %s failed, error: `%s'", __FUNCTION__, "pa_simple_new", pa_strerror(error));
            throw audio_error(NS_FuncName);
        }
    }

    bool PulseAudio::Playback::streamWrite(const uint8_t* ptr, size_t len) const {
        int error = 0;

        if(0 != pa_simple_write(ctx.get(), ptr, len, & error)) {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_write", pa_strerror(error));
            return false;
        }

        return true;
    }

    bool PulseAudio::Playback::streamDrain(void) const {
        int error = 0;

        if(0 != pa_simple_drain(ctx.get(), & error)) {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_drain", pa_strerror(error));
            return false;
        }

        return true;
    }

    PulseAudio::Record::Record(const std::string & appName, const std::string & streamName,
                               const pa_sample_format_t & fmt, uint32_t rate, uint8_t channels, const pa_buffer_attr* attr) {
        audioSpec.format = fmt;
        audioSpec.rate = rate;
        audioSpec.channels = channels;

        if(0 == pa_sample_spec_valid(& audioSpec)) {
            Application::error("%s: %s failed, format: `%s', rate: %" PRIu32 ", channels: %" PRIu8,
                               __FUNCTION__, "pa_sample_spec_valid", pa_sample_format_to_string(audioSpec.format), audioSpec.rate, audioSpec.channels);
            throw audio_error(NS_FuncName);
        }

        int error = 0;
        ctx.reset(pa_simple_new(nullptr, appName.c_str(), PA_STREAM_RECORD,
                                nullptr, streamName.c_str(), & audioSpec, nullptr, attr, & error));

        if(! ctx) {
            Application::error("%s: %s failed, error: `%s'", __FUNCTION__, "pa_simple_new", pa_strerror(error));
            throw audio_error(NS_FuncName);
        }
    }

    std::vector<uint8_t> PulseAudio::Record::streamRead(size_t len) const {
        int error = 0;
        std::vector<uint8_t> buf(len, 0);

        if(0 != pa_simple_read(ctx.get(), buf.data(), buf.size(), & error)) {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_read", pa_strerror(error));
            return {};
        }

        return buf;
    }
}
