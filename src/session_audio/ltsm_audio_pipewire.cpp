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
 
#include <spa/param/audio/raw.h>
#include <spa/param/audio/format-utils.h>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_audio_pipewire.h"

namespace LTSM {
    namespace PipeWire {
        uint16_t formatBits(const spa_audio_format & fmt) {
            switch(fmt) {
                case SPA_AUDIO_FORMAT_S16_LE:
                case SPA_AUDIO_FORMAT_S16_BE:
                    return 16;

                case SPA_AUDIO_FORMAT_S24_LE:
                case SPA_AUDIO_FORMAT_S24_BE:
                    return 24;

                case SPA_AUDIO_FORMAT_S32_LE:
                case SPA_AUDIO_FORMAT_S32_BE:
                    return 32;

                default:
                    break;
            }

            return 0;
        }

        const char* mediaAudioSubtype(int val) {
            switch(val) {
                case SPA_MEDIA_SUBTYPE_raw: return "raw";
                case SPA_MEDIA_SUBTYPE_dsp: return "dsp";
                case SPA_MEDIA_SUBTYPE_iec958: return "iec958";
                case SPA_MEDIA_SUBTYPE_dsd: return "dsd";

                case SPA_MEDIA_SUBTYPE_mp3: return "mp3";
                case SPA_MEDIA_SUBTYPE_aac: return "aac";
                case SPA_MEDIA_SUBTYPE_vorbis: return "vorbis";
                case SPA_MEDIA_SUBTYPE_wma: return "wma";
                case SPA_MEDIA_SUBTYPE_ra: return "ra";
                case SPA_MEDIA_SUBTYPE_sbc: return "sbc";
                case SPA_MEDIA_SUBTYPE_adpcm: return "aspcm";
                case SPA_MEDIA_SUBTYPE_g723: return "g723";
                case SPA_MEDIA_SUBTYPE_g726: return "g726";
                case SPA_MEDIA_SUBTYPE_g729: return "g729";
                case SPA_MEDIA_SUBTYPE_amr: return "amr";
                case SPA_MEDIA_SUBTYPE_gsm: return "gsm";
                case SPA_MEDIA_SUBTYPE_alac: return "alac";
                case SPA_MEDIA_SUBTYPE_flac: return "flac";
                case SPA_MEDIA_SUBTYPE_ape: return "ape";
                case SPA_MEDIA_SUBTYPE_opus: return "opus";
                default:  break;
            }
            return "unknown";
        }

        void on_process(void* data) {
            if(auto pipeWire = static_cast<OutputStream*>(data)) {
                pipeWire->onProcessCb();
            }
        }

        void on_stream_param_changed(void* data, uint32_t id, const spa_pod* param) {
            if(param == nullptr || id != SPA_PARAM_Format) {
                return;
            }

            if(auto pipeWire = static_cast<OutputStream*>(data)) {
                pipeWire->onStreamParamChangedCb(param);
            }
        }

        void on_stream_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* error) {
            if(auto pipeWire = static_cast<OutputStream*>(data)) {
                pipeWire->onStreamStateChangedCb(old, state, error);
            }
        }

        const struct pw_stream_events stream_events = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = on_stream_state_changed,
            .param_changed = on_stream_param_changed,
            .process = on_process
        };
    }

    PipeWire::OutputStream::OutputStream(const spa_audio_format & fmt, uint32_t rate, uint8_t channels, const ReadEventFunc & func)
        : read_event_cb_(func), format_(fmt), rate_(rate), channels_(channels)
    {
        pw_init(nullptr, nullptr);
        loop_.reset(pw_main_loop_new(nullptr));

        auto props = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Music",
                nullptr
            );

        if(!props) {
            Application::error("{}: {} failed", __FUNCTION__, "pw_properties_new");
            throw std::runtime_error(NS_FuncNameS);
        }

        stream_.reset(
            pw_stream_new_simple(
                pw_main_loop_get_loop(loop_.get()),
                "LtsmAudioCapture",
                props,
                &stream_events,
                this
            )
        );

        if(! stream_) {
            Application::error("{}: {} failed", __FUNCTION__, "pw_stream_new_simple");
            throw std::runtime_error(NS_FuncNameS);
        }

        loop_run_ = std::async(std::launch::async, pw_main_loop_run, loop_.get());
    }

    PipeWire::OutputStream::~OutputStream() {
        if(loop_run_.valid()) {
            pw_main_loop_quit(loop_.get());
            loop_run_.wait();
        }

        if(stream_) {
            pw_stream_disconnect(stream_.get());
        }

        stream_.reset();
        loop_.reset();

        pw_deinit();
    }

    void PipeWire::OutputStream::onProcessCb(void) {
        if(auto buf = pw_stream_dequeue_buffer(stream_.get())) {
            Application::trace(DebugType::Audio, "{}: buf - size: {}, chunk: {}, requested: {}",
                __FUNCTION__, buf->size, buf->buffer->datas[0].chunk->size, buf->requested);

            read_event_cb_(
                static_cast<uint8_t*>(buf->buffer->datas[0].data),
                buf->buffer->datas[0].chunk->size);

            pw_stream_queue_buffer(stream_.get(), buf);
        } else {
            Application::error("{}: {} failed", __FUNCTION__, "pw_stream_dequeue_buffer");
        }
    }

    void PipeWire::OutputStream::onStreamParamChangedCb(const spa_pod* param) {
        uint32_t media_type;
        uint32_t media_subtype;
   
        if(int ret = spa_format_parse(param, &media_type, &media_subtype); 0 > ret) {
            Application::error("{}: {} failed, code: {}", __FUNCTION__, "pw_stream_connect", ret);
            return;
        }

        if(media_type != SPA_MEDIA_TYPE_audio) {
            Application::warning("{}: unknown media type: {:#08x}", __FUNCTION__, media_type);
            return;
        }

        Application::info("{}: media subtype: {}({:#08x})", __FUNCTION__, mediaAudioSubtype(media_subtype), media_subtype);

        if(media_subtype == SPA_MEDIA_SUBTYPE_raw)
        {
            spa_audio_info_raw raw;

            if(int ret = spa_format_audio_raw_parse(param, & raw); 0 > ret) {
                Application::error("{}: {} failed, code: {}", __FUNCTION__, "spa_format_audio_raw_parse", ret);
            } else {
                Application::info("{}: raw format - rate: {}, channels: {}", __FUNCTION__, raw.rate, raw.channels);
            }
        }
    }

    void PipeWire::OutputStream::onStreamStateChangedCb(pw_stream_state old, pw_stream_state state, const char* error) {
        Application::info("{}: old: {}, new: {}, error: {}",
                __FUNCTION__, pw_stream_state_as_string(old), pw_stream_state_as_string(state), error);
    }

    bool PipeWire::OutputStream::streamConnect(bool pause) {
        if(loop_run_.valid()) {
            pw_main_loop_quit(loop_.get());
            loop_run_.wait();
        }

        uint8_t builder_buf[1024] = {};
        spa_pod_builder builder = {};
        const spa_pod* params[1] = {};

        spa_pod_builder_init(&builder, builder_buf, sizeof(builder_buf));

        auto raw_format = SPA_AUDIO_INFO_RAW_INIT(.format = format_, .rate = rate_, .channels = channels_);
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, & raw_format);

        //auto mp3_format = SPA_AUDIO_INFO_MP3_INIT(.rate = 128 * 1024, .channels = SPA_AUDIO_MP3_CHANNEL_MODE_STEREO);
        //params[0] = spa_format_audio_mp3_build(&builder, SPA_PARAM_EnumFormat, & mp3_format);
        
        auto flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
        if(pause) {
            flags |= PW_STREAM_FLAG_INACTIVE;
        }

        int ret = pw_stream_connect(stream_.get(),
                                    PW_DIRECTION_INPUT,
                                    PW_ID_ANY,
                                    (pw_stream_flags) flags,
                                    params, 1);

        loop_run_ = std::async(std::launch::async, pw_main_loop_run, loop_.get());

        if(0 > ret) {
            Application::error("{}: {} failed, code: {}", __FUNCTION__, "pw_stream_connect", ret);
            return false;
        }

        return true;
    }

    void PipeWire::OutputStream::streamDisconnect(void) {
        if(loop_run_.valid()) {
            pw_main_loop_quit(loop_.get());
            loop_run_.wait();
        }

        pw_stream_disconnect(stream_.get());
        loop_run_ = std::async(std::launch::async, pw_main_loop_run, loop_.get());
    }

    bool PipeWire::OutputStream::streamActivate(bool active) {
        if(loop_run_.valid()) {
            pw_main_loop_quit(loop_.get());
            loop_run_.wait();
        }

        int ret = pw_stream_set_active(stream_.get(), active);

        if(0 > ret) {
            Application::error("{}: {} failed, code: {}", __FUNCTION__, "pw_stream_set_active", ret);
            return false;
        }

        loop_run_ = std::async(std::launch::async, pw_main_loop_run, loop_.get());
        return true;
    }

    pw_stream_state PipeWire::OutputStream::streamState(void) const {
        return pw_stream_get_state(stream_.get(), nullptr);
    }

    void PipeWire::OutputStream::streamPause(void) {
        if(! streamPaused()) {
            streamActivate(false);
        }
    }

    void PipeWire::OutputStream::streamUnPause(void) {
        if(streamPaused()) {
            streamActivate(true);
        }
    }

    bool PipeWire::OutputStream::streamPaused(void) const {
        return PW_STREAM_STATE_PAUSED == streamState();
    }
}
