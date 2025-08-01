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

#ifndef _LTSM_AUDIO_
#define _LTSM_AUDIO_

#define LTSM_AUDIO2SESSION_VERSION 20240304

#include <cstdint>
#include <stdexcept>

namespace LTSM
{
    namespace AudioOp
    {
        enum
        {
            Init = 0xFE01,
            Data = 0xFE02,
            Silent = 0xFE03
        };
    }

    namespace AudioEncoding
    {
        enum
        {
            PCM = 0,
            OPUS = 1,
            AAC = 2
        };
    }

    struct AudioFormat
    {
        uint16_t type = 0;
        uint16_t channels = 0;
        uint32_t samplePerSec = 0;
        uint16_t bitsPerSample = 0;
    };

    class AudioPlayer
    {
    public:
        AudioPlayer() = default;
        virtual ~AudioPlayer() = default;

        virtual bool streamWrite(const uint8_t*, size_t) const = 0;
    };

    struct audio_error : public std::runtime_error
    {
        explicit audio_error(std::string_view what) : std::runtime_error(what.data()) {}
    };
}

#endif // _LTSM_AUDIO_
