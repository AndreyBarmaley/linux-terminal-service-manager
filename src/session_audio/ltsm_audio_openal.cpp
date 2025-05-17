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

#include "ltsm_tools.h"
#include "ltsm_audio.h"
#include "ltsm_application.h"
#include "ltsm_audio_openal.h"

namespace LTSM
{
    const ALuint format1SecBytes = 2 * 2 * 44100 /* 16 bit, 2 channels, 44100 */;

    const char* OpenAL::alcErrorName(ALCenum err)
    {
        switch(err)
        {
            case ALC_NO_ERROR: return "ALC_NO_ERROR";
            case ALC_INVALID_DEVICE: return "ALC_INVALID_DEVICE";
            case ALC_INVALID_CONTEXT: return "ALC_INVALID_CONTEXT";
            case ALC_INVALID_ENUM: return "ALC_INVALID_ENUM";
            case ALC_INVALID_VALUE: return "ALC_INVALID_VALUE";
            case ALC_OUT_OF_MEMORY: return "ALC_OUT_OF_MEMORY";
            default: break;
        }

        return "unknown";
    }

    OpenAL::Playback::Playback(const AudioFormat & fmt, ALuint autoPlayAfterSec)
    {
        fmtFrequency = fmt.samplePerSec;

        if(8 == fmt.bitsPerSample)
        {
            if(fmt.channels == 2)
                fmtFormat = AL_FORMAT_STEREO8;
            else if(fmt.channels == 2)
                fmtFormat = AL_FORMAT_MONO8;
        }
        else if(16 == fmt.bitsPerSample)
        {
            if(fmt.channels == 2)
                fmtFormat = AL_FORMAT_STEREO16;
            else if(fmt.channels == 2)
                fmtFormat = AL_FORMAT_MONO16;
        }

        if(! fmtFormat)
        {
            Application::error("%s: %s failed, bits: %" PRIu16 ", rate: %" PRIu16 ", channels: %" PRIu16,
                               __FUNCTION__, "AudioFormat", fmt.bitsPerSample, fmt.samplePerSec, fmt.channels);
            throw audio_error(NS_FuncName);
        }

        if(autoPlayAfterSec)
        {
            // calculate playAfterBytes
            switch(fmtFormat)
            {
                case AL_FORMAT_MONO8:   playAfterBytes = 1 * fmtFrequency * autoPlayAfterSec; break;
                case AL_FORMAT_STEREO8: playAfterBytes = 2 * fmtFrequency * autoPlayAfterSec; break;
                case AL_FORMAT_MONO16:  playAfterBytes = 2 * fmtFrequency * autoPlayAfterSec; break;
                case AL_FORMAT_STEREO16:playAfterBytes = 4 * fmtFrequency * autoPlayAfterSec; break;
                default: break;
            }
        }

        dev.reset(alcOpenDevice(nullptr /* pref device name */));

        if(! dev)
        {
            Application::error("%s: %s failed", __FUNCTION__, "alcOpenDevice");
            throw audio_error(NS_FuncName);
        }

        ctx.reset(alcCreateContext(dev.get(), nullptr /* attr list */));

        if(! ctx)
        {
            Application::error("%s: %s failed", __FUNCTION__, "alcCreateContext");
            throw audio_error(NS_FuncName);
        }

        if(! alcMakeContextCurrent(ctx.get()))
        {
            Application::error("%s: %s failed", __FUNCTION__, "alcMakeContextCurrent");
            throw audio_error(NS_FuncName);
        }

        alGenSources(1, & sourceId);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alcMakeContextCurrent", alcErrorName(err));
            throw audio_error(NS_FuncName);
        }
    }

    OpenAL::Playback::~Playback()
    {
        alDeleteSources(1, & sourceId);
        alcMakeContextCurrent(nullptr);

        ctx.reset();
        dev.reset();
    }

    ALint OpenAL::Playback::getBuffersProcessed(void) const
    {
        ALint res = 0;
        alGetSourcei(sourceId, AL_BUFFERS_PROCESSED, & res);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alGetSourcei", alcErrorName(err));
            return 0;
        }

        return res;
    }

    ALint OpenAL::Playback::getBuffersQueued(void) const
    {
        ALint res = 0;
        alGetSourcei(sourceId, AL_BUFFERS_QUEUED, & res);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alGetSourcei", alcErrorName(err));
            return 0;
        }

        return res;
    }

    ALuint OpenAL::Playback::findFreeBufferId(void) const
    {
        if(ALint bufProcesses = getBuffersProcessed(); 0 <= bufProcesses)
        {
            ALuint bufId = 0;
            alSourceUnqueueBuffers(sourceId, 1, & bufId);

            if(auto err = alGetError(); err != AL_NO_ERROR)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "alSourceUnqueueBuffers", alcErrorName(err));
                return 0;
            }

            return bufId;
        }

        return 0;
    }

    bool OpenAL::Playback::playStart(void) const
    {
        alSourcePlay(sourceId);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alSourcePlay", alcErrorName(err));
            return false;
        }

        return true;
    }

    bool OpenAL::Playback::playStop(void) const
    {
        alSourceStop(sourceId);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alSourceStop", alcErrorName(err));
            return false;
        }

        return true;
    }

    bool OpenAL::Playback::playPause(void) const
    {
        alSourcePause(sourceId);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alSourcePause", alcErrorName(err));
            return false;
        }

        return true;
    }

    bool OpenAL::Playback::stateIsPlaying(void) const
    {
        ALint sourceState = 0;
        alGetSourcei(sourceId, AL_SOURCE_STATE, & sourceState);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alGetSourcei", alcErrorName(err));
            return false;
        }

        return sourceState == AL_PLAYING;
    }

    bool OpenAL::Playback::streamWrite(const uint8_t* buf, size_t len) const
    {
        ALuint bufId = findFreeBufferId();

        if(0 == bufId)
        {
            if(stateIsPlaying())
                return false;

            alGenBuffers(1, & bufId);

            if(auto err = alGetError(); err != AL_NO_ERROR)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "alGetBuffers", alcErrorName(err));
                return false;
            }
        }

        alBufferData(bufId, fmtFormat, buf, len, fmtFrequency);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alBufferData", alcErrorName(err));
            return false;
        }

        alSourceQueueBuffers(sourceId, 1, & bufId);

        if(auto err = alGetError(); err != AL_NO_ERROR)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "alSourceQueueBuffers", alcErrorName(err));
            return false;
        }

        if(playAfterBytes)
        {
            if(playAfterBytes < len)
            {
                playAfterBytes = 0;
                playStart();
            }
            else
            {
                playAfterBytes -= len;
            }
        }

        return true;
    }
}
