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

#include <spa/debug/types.h>
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

        void on_process(void* data) {
            if(auto pipeWire = static_cast<BaseStream*>(data)) {
                pipeWire->onProcessCb();
            }
        }

        void on_stream_param_changed(void* data, uint32_t id, const spa_pod* param) {
            if(param == nullptr || id != SPA_PARAM_Format) {
                return;
            }

            if(auto pipeWire = static_cast<BaseStream*>(data)) {
                pipeWire->onStreamParamChangedCb(param);
            }
        }

        void on_stream_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* error) {
            if(auto pipeWire = static_cast<BaseStream*>(data)) {
                pipeWire->onStreamStateChangedCb(old, state, error);
            }
        }

        const struct pw_stream_events stream_events = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = on_stream_state_changed,
            .param_changed = on_stream_param_changed,
            .process = on_process
        };

        struct PwThreadLoopLocker {
            pw_thread_loop* loop = nullptr;

            PwThreadLoopLocker(pw_thread_loop* ptr) : loop(ptr) {
                pw_thread_loop_lock(loop);
            }

            ~PwThreadLoopLocker() {
                if(loop) {
                    pw_thread_loop_unlock(loop);
                }
            }
        };
    }

    PipeWire::BaseStream::BaseStream(const MediaCategory & mcat, const spa_audio_format & fmt, uint32_t rate, uint8_t channels)
        : media_category_(mcat), format_(fmt), rate_(rate), channels_(channels) {

        pw_init(nullptr, nullptr);

        Application::info("{}: pipewire headers version: {}, library version: {}",
                          __FUNCTION__, pw_get_headers_version(), pw_get_library_version());

        loop_.reset(pw_thread_loop_new("LtsmPipeWireLoop", nullptr));

        if(! loop_) {
            Application::error("%s: %s failed", __FUNCTION__, "pw_thread_loop_new");
            throw std::runtime_error(__FUNCTION__);
        }

        auto props = pw_properties_new(
                         PW_KEY_MEDIA_TYPE, "Audio",
                         PW_KEY_MEDIA_CATEGORY, (media_category_ == MediaCategory::Capture ? "Capture" : "Playback"),
                         PW_KEY_MEDIA_ROLE, "Music",
                         nullptr
                     );

        if(! props) {
            Application::error("{}: {} failed", __FUNCTION__, "pw_properties_new");
            throw std::runtime_error(NS_FuncNameS);
        }

        stream_.reset(
            pw_stream_new_simple(
                pw_thread_loop_get_loop(loop_.get()),
                (media_category_ == MediaCategory::Capture ? "LtsmCaptureStream" : "LtsmPlaybackStream"),
                props,
                &stream_events,
                this
            )
        );

        if(! stream_) {
            Application::error("{}: {} failed", __FUNCTION__, "pw_stream_new_simple");
            throw std::runtime_error(NS_FuncNameS);
        }

        if(0 != pw_thread_loop_start(loop_.get())) {
            Application::error("%s: %s failed", __FUNCTION__, "pw_thread_loop_start");
            throw std::runtime_error(__FUNCTION__);
        }
    }

    PipeWire::BaseStream::~BaseStream() {
        pw_thread_loop_stop(loop_.get());

        if(stream_) {
            pw_stream_disconnect(stream_.get());
        }

        stream_.reset();
        loop_.reset();

        pw_deinit();
    }

    void PipeWire::BaseStream::onStreamParamChangedCb(const spa_pod* param) {
        const PwThreadLoopLocker lock{ loop_.get() };

        uint32_t media_type;
        uint32_t media_subtype;

        if(int ret = spa_format_parse(param, &media_type, &media_subtype); 0 > ret) {
            Application::error("{}: {} failed, code: {}", __FUNCTION__, "pw_stream_connect", ret);
            return;
        }

        if(media_type != SPA_MEDIA_TYPE_audio) {
            const char* type_name = spa_debug_type_find_name(spa_type_media_type, media_type);
            Application::warning("{}: unsupported media type: {}({:#08x})", __FUNCTION__, type_name, media_type);
            return;
        }

        if(media_subtype == SPA_MEDIA_SUBTYPE_raw) {
            const char* subtype_name = spa_debug_type_find_name(spa_type_media_subtype, media_subtype);
            Application::warning("{}: unsupported media subtype: {}({:#08x})", __FUNCTION__, subtype_name, media_subtype);
            return;
        }

        spa_audio_info_raw raw;

        if(int ret = spa_format_audio_raw_parse(param, & raw); 0 > ret) {
            Application::error("{}: {} failed, code: {}", __FUNCTION__, "spa_format_audio_raw_parse", ret);
            return;
        }

        if(raw.format != format_) {
            const char* format_name = spa_debug_type_find_name(spa_type_audio_format, raw.format);
            Application::warning("{}: unsupported audio format: {}", __FUNCTION__, format_name);
            return;
        }

        if(raw.rate != rate_) {
            Application::warning("{}: unsupported audio format, rate: {}", __FUNCTION__, raw.rate);
            return;
        }

        if(raw.channels != channels_) {
            Application::warning("{}: unsupported audio format, channels: {}", __FUNCTION__, raw.channels);
            return;
        }
    }

    void PipeWire::BaseStream::onStreamStateChangedCb(pw_stream_state old, pw_stream_state state, const char* error) {
        // unconnected connecting paused error unconnected
        // unconnected connecting paused streaming paused unconnected
        Application::info("{}: old: {}, new: {}, error: {}",
                          __FUNCTION__, pw_stream_state_as_string(old), pw_stream_state_as_string(state), error);
    }

    bool PipeWire::BaseStream::streamConnect(bool pause) {
        const PwThreadLoopLocker lock{ loop_.get() };

        uint8_t builder_buf[1024] = {};
        spa_pod_builder builder = {};
        const spa_pod* params[1] = {};

        spa_pod_builder_init(&builder, builder_buf, sizeof(builder_buf));

        auto raw_format = SPA_AUDIO_INFO_RAW_INIT(.format = format_, .rate = rate_, .channels = channels_);
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, & raw_format);

        //auto mp3_format = SPA_AUDIO_INFO_MP3_INIT(.rate = 128 * 1024, .channels = SPA_AUDIO_MP3_CHANNEL_MODE_STEREO);
        //params[0] = spa_format_audio_mp3_build(&builder, SPA_PARAM_EnumFormat, & mp3_format);

        auto flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;

        if(pause) {
            flags |= PW_STREAM_FLAG_INACTIVE;
        }

        int ret = pw_stream_connect(stream_.get(),
                                    (media_category_ == MediaCategory::Capture ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT),
                                    PW_ID_ANY,
                                    (pw_stream_flags) flags,
                                    params, 1);

        if(0 > ret) {
            Application::error("{}: {} failed, code: {}", __FUNCTION__, "pw_stream_connect", ret);
            return false;
        }

        Application::debug(DebugType::Audio, "{}: success", __FUNCTION__);

        return true;
    }

    void PipeWire::BaseStream::streamDisconnect(void) {
        if(stream_) {
            const PwThreadLoopLocker lock{ loop_.get() };
            pw_stream_disconnect(stream_.get());
            stream_.reset();
        }
    }

    bool PipeWire::BaseStream::streamActivate(bool active) {
        const PwThreadLoopLocker lock{ loop_.get() };

        if(int ret = pw_stream_set_active(stream_.get(), active); 0 > ret) {
            Application::error("{}: {} failed, code: {}", __FUNCTION__, "pw_stream_set_active", ret);
            return false;
        }

        return true;
    }

    pw_stream_state PipeWire::BaseStream::streamState(void) const {
        const PwThreadLoopLocker lock{ loop_.get() };

        return pw_stream_get_state(stream_.get(), nullptr);
    }

    void PipeWire::BaseStream::streamPause(void) {
        if(! streamPaused()) {
            streamActivate(false);
        }
    }

    void PipeWire::BaseStream::streamUnPause(void) {
        if(streamPaused()) {
            streamActivate(true);
        }
    }

    bool PipeWire::BaseStream::streamPaused(void) const {
        return PW_STREAM_STATE_PAUSED == streamState();
    }

    // CaptureStream
    void PipeWire::AudioCapture::onProcessCb(void) {
        const PwThreadLoopLocker lock{ loop_.get() };

        if(auto buf = pw_stream_dequeue_buffer(stream_.get())) {
            const auto & ptr = static_cast<uint8_t*>(buf->buffer->datas[0].data);
            const auto & len = buf->buffer->datas[0].chunk->size;

            if(ptr && len) {
                Application::debug(DebugType::Audio, "{}: buf - size: {}, chunk: {}, requested: {}",
                                   __FUNCTION__, buf->size, len, buf->requested);

                read_event_cb_(ptr, len);
            }

            pw_stream_queue_buffer(stream_.get(), buf);
        } else {
            Application::error("{}: {} failed", __FUNCTION__, "pw_stream_dequeue_buffer");
        }
    }

    // PlaybackStream
    void PipeWire::AudioPlayback::onProcessCb(void) {
        const PwThreadLoopLocker lock{ loop_.get() };

        if(auto buf = pw_stream_dequeue_buffer(stream_.get())) {
/*
            auto & ptr = static_cast<uint8_t*>(buf->buffer->datas[0].data);
            auto & len = buf->buffer->datas[0].chunk->size;

            if(ptr && len) {
                Application::debug(DebugType::Audio, "{}: buf - size: {}, chunk: {}, requested: {}",
                                   __FUNCTION__, buf->size, len, buf->requested);
            }
*/
            pw_stream_queue_buffer(stream_.get(), buf);
        } else {
            Application::error("{}: {} failed", __FUNCTION__, "pw_stream_dequeue_buffer");
        }
    }
}
