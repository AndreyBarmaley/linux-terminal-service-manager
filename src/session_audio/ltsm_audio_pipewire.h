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
    using ReadEventFunc = std::function<void(const uint8_t*, size_t)>;

    uint16_t formatBits(const spa_audio_format & fmt);

    class OutputStream {
        std::unique_ptr<pw_context, void(*)(pw_context*)> context_{nullptr, pw_context_destroy};
        std::unique_ptr<pw_core, int(*)(pw_core*)> core_{nullptr, pw_core_disconnect};
        std::unique_ptr<pw_main_loop, void(*)(pw_main_loop*)> loop_{nullptr, pw_main_loop_destroy};
        std::unique_ptr<pw_stream, void(*)(pw_stream*)> stream_{nullptr, pw_stream_destroy};

        std::future<int> loop_run_;
        ReadEventFunc read_event_cb_;

        const spa_audio_format format_;
        const uint32_t rate_;
        const uint32_t channels_;

      protected:
        bool streamActivate(bool active);
        pw_stream_state streamState(void) const;

      public:
        OutputStream(const spa_audio_format &, uint32_t rate, uint8_t channels, const ReadEventFunc &);
        ~OutputStream();

        void onProcessCb(void);
        void onStreamParamChangedCb(const spa_pod* param);
        void onStreamStateChangedCb(pw_stream_state old, pw_stream_state state, const char* error);

        bool streamConnect(bool pause);
        void streamDisconnect(void);

        void streamPause(void);
        void streamUnPause(void);
        bool streamPaused(void) const;

        std::string streamStateString(void) const;
    };
}

#endif // _LTSM_AUDIO_PIPEWIRE_
