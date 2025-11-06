/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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

#include "ltsm_application.h"
#include "ffmpeg_tools.h"

namespace LTSM {

    bool Tools::AV_PixelFormatEnumToMasks(AVPixelFormat format, int* bpp, uint32_t* rmask, uint32_t* gmask, uint32_t* bmask, uint32_t* amask, bool debug) {
        switch(format) {
            case AV_PIX_FMT_RGB24:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_RGB24");
                }

                *bpp = 24;
                *amask = 0;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                std::swap(*rmask, *bmask);
#endif
                return true;

            case AV_PIX_FMT_BGR24:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_BGR24");
                }

                *bpp = 24;
                *amask = 0;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                std::swap(*rmask, *bmask);
#endif
                return true;

            case AV_PIX_FMT_RGB0:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_RGB0");
                }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *amask = 0;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#else
                *bpp = 32;
                *rmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *bmask = 0x0000FF00;
                *amask = 0;
#endif
                return true;

            case AV_PIX_FMT_0BGR:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_0BGR");
                }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *rmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *bmask = 0x0000FF00;
                *amask = 0;
#else
                *bpp = 32;
                *amask = 0;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#endif
                return true;

            case AV_PIX_FMT_BGR0:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_BGR0");
                }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *amask = 0;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#else
                *bpp = 32;
                *bmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *rmask = 0x0000FF00;
                *amask = 0;
#endif
                return true;

            case AV_PIX_FMT_0RGB:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_0RGB");
                }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *bmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *rmask = 0x0000FF00;
                *amask = 0;
#else
                *bpp = 32;
                *amask = 0;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#endif
                return true;

            case AV_PIX_FMT_RGBA:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_RGBA");
                }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *amask = 0xFF000000;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#else
                *bpp = 32;
                *rmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *bmask = 0x0000FF00;
                *amask = 0x000000FF;
#endif
                return true;

            case AV_PIX_FMT_ABGR:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_ABGR");
                }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *rmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *bmask = 0x0000FF00;
                *amask = 0x000000FF;
#else
                *bpp = 32;
                *amask = 0xFF000000;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#endif
                return true;

            case AV_PIX_FMT_BGRA:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_BGRA");
                }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *amask = 0xFF000000;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#else
                *bpp = 32;
                *bmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *rmask = 0x0000FF00;
                *amask = 0x000000FF;
#endif
                return true;


            case AV_PIX_FMT_ARGB:
                if(debug) {
                    Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_ARGB");
                }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *bmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *rmask = 0x0000FF00;
                *amask = 0x000000FF;
#else
                *bpp = 32;
                *amask = 0xFF000000;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#endif
                return true;

            default:
                break;
        }

        return false;
    }

    AVPixelFormat Tools::AV_PixelFormatEnumFromMasks(int bpp, uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask, bool debug) {
        if(debug) {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
            bool bigEndian = false;
#else
            bool bigEndian = true;
#endif
            Application::info("%s: pixel format, bpp: %d, rmask: 0x%08" PRIx32 ", gmask: 0x%08" PRIx32 ", bmask: 0x%08" PRIx32 ", amask: 0x%08" PRIx32 ", be: %d",
                              __FUNCTION__, bpp, rmask, gmask, bmask, amask, (int) bigEndian);
        }

        if(24 == bpp) {
            if(amask == 0 && rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_BGR24;

#else
                return AV_PIX_FMT_RGB24;
#endif

            if(amask == 0 && bmask == 0x00FF0000 && gmask == 0x0000FF00 && rmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_RGB24;

#else
                return AV_PIX_FMT_BGR24;
#endif
        } else if(32 == bpp) {
            if(rmask == 0xFF000000 && gmask == 0x00FF0000 && bmask == 0x0000FF00 && amask == 0)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_0BGR;

#else
                return AV_PIX_FMT_RGB0;
#endif

            if(amask == 0 && bmask == 0x00FF0000 && gmask == 0x0000FF00 && rmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_RGB0;

#else
                return AV_PIX_FMT_0BGR;
#endif

            if(bmask == 0xFF000000 && gmask == 0x00FF0000 && rmask == 0x0000FF00 && amask == 0)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_0RGB;

#else
                return AV_PIX_FMT_BGR0;
#endif

            if(amask == 0 && rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_BGR0;

#else
                return AV_PIX_FMT_0RGB;
#endif

            if(rmask == 0xFF000000 && gmask == 0x00FF0000 && bmask == 0x0000FF00 && amask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_ABGR;

#else
                return AV_PIX_FMT_RGBA;
#endif

            if(amask == 0xFF000000 && bmask == 0x00FF0000 && gmask == 0x0000FF00 && rmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_RGBA;

#else
                return AV_PIX_FMT_ABGR;
#endif

            if(bmask == 0xFF000000 && gmask == 0x00FF0000 && rmask == 0x0000FF00 && amask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_ARGB;

#else
                return AV_PIX_FMT_BGRA;
#endif

            if(amask == 0xFF000000 && rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_BGRA;

#else
                return AV_PIX_FMT_ARGB;
#endif
        }

        Application::error("%s: unsupported pixel format, bpp: %d, rmask: 0x%08" PRIx32 ", gmask: 0x%08" PRIx32 ", bmask: 0x%08" PRIx32 ", amask: 0x%08" PRIx32,
                           __FUNCTION__, bpp, rmask, gmask, bmask, amask);

        return AV_PIX_FMT_NONE;
    }

} // LTSM
