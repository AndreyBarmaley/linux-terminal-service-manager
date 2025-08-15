/***********************************************************************
 *   Copyright © 2025 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_AUDIO_OPENAL_
#define _LTSM_AUDIO_OPENAL_

#include <memory>

#include "ltsm_audio.h"

#ifdef __APPLE__
 #include <OpenAL/al.h>
 #include <OpenAL/alc.h>
#else
 #include <AL/al.h>
 #include <AL/alc.h>
#endif

namespace LTSM
{
    namespace OpenAL
    {
        const char* alcErrorName(ALCenum);

        class Playback : public AudioPlayer
        {
            std::unique_ptr<ALCdevice, ALCboolean(*)(ALCdevice*)> dev{ nullptr, alcCloseDevice };
            std::unique_ptr<ALCcontext, void(*)(ALCcontext*)> ctx{ nullptr, alcDestroyContext };

            ALuint sourceId = 0;
            mutable ALuint playAfterBytes = 65536;

            ALenum fmtFormat = 0;
            ALsizei fmtFrequency = 0;

        protected:
            ALint getBuffersProcessed(void) const;
            ALint getBuffersQueued(void) const;
            ALuint findFreeBufferId(void) const;

        public:
            Playback(const AudioFormat &, ALuint autoPlayAfterSec = 0);
            ~Playback();

            bool streamWrite(const uint8_t*, size_t) const override;

            bool playStart(void) const;
            bool playStop(void) const;
            bool playPause(void) const;
            bool stateIsPlaying(void) const;
        };
    }
}

#endif // _LTSM_AUDIO_OPENAL_
