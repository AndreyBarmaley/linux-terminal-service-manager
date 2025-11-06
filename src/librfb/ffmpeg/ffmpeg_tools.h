/***************************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef _FFMPEG_TOOLS_
#define _FFMPEG_TOOLS_

#ifdef __cplusplus
extern "C" {
#endif

#include "libavformat/avformat.h"

#ifdef __cplusplus
}

#endif

namespace LTSM {
    namespace Tools {
        bool AV_PixelFormatEnumToMasks(AVPixelFormat format, int* bpp, uint32_t* rmask, uint32_t* gmask, uint32_t* bmask, uint32_t* amask, bool debug);
        AVPixelFormat AV_PixelFormatEnumFromMasks(int bpp, uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask, bool debug);
    }
}

#endif // _FFMPEG_TOOLS_
