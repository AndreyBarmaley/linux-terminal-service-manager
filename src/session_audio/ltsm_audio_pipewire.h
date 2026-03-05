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

#ifndef _LTSM_AUDIO_PIPEWIRE_
#define _LTSM_AUDIO_PIPEWIRE_

#include <future>
#include <string>
#include <memory>
#include <functional>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format.h>

namespace LTSM::PipeWire {

    using DataReadyFunc = std::function<void(const uint8_t*, size_t)>;
    uint16_t formatBits(const spa_audio_format & fmt);

    enum class MediaCategory { Playback, Capture };

    class BaseStream {
      protected:
        std::unique_ptr<pw_thread_loop, void(*)(pw_thread_loop*)> loop_{nullptr, pw_thread_loop_destroy};
        std::unique_ptr<pw_stream, void(*)(pw_stream*)> stream_{nullptr, pw_stream_destroy};

        const MediaCategory media_category_;
        const spa_audio_format format_;
        const uint32_t rate_;
        const uint32_t channels_;

      protected:
        bool streamActivate(bool active);

      public:
        BaseStream(const MediaCategory &, const spa_audio_format &, uint32_t rate, uint8_t channels);
        virtual ~BaseStream();

        virtual void onProcessCb(void) = 0;

        void onStreamParamChangedCb(const spa_pod* param);
        void onStreamStateChangedCb(pw_stream_state old, pw_stream_state state, const char* error);

        bool streamConnect(bool pause);
        void streamDisconnect(void);

        void streamPause(void);
        void streamUnPause(void);
        bool streamPaused(void) const;

        // PW_STREAM_STATE_ERROR, PW_STREAM_STATE_UNCONNECTED, PW_STREAM_STATE_CONNECTING, PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_STREAMING
        pw_stream_state streamState(void) const;
    };

    class AudioCapture : public BaseStream {
        DataReadyFunc data_ready_cb_;

      public:
        AudioCapture(const spa_audio_format & fmt, uint32_t rate, uint8_t channels, const DataReadyFunc & func) :
            BaseStream(MediaCategory::Capture, fmt, rate, channels), data_ready_cb_{func} {}

        void onProcessCb(void) override;
    };

    class AudioPlayback : public BaseStream {
      public:
        AudioPlayback(const spa_audio_format & fmt, uint32_t rate, uint8_t channels) :
            BaseStream(MediaCategory::Capture, fmt, rate, channels) {}

        void onProcessCb(void) override;
    };
}

#endif // _LTSM_AUDIO_PIPEWIRE_
