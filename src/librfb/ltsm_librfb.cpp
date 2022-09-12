/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#include "ltsm_librfb.h"

namespace LTSM
{
    const char* RFB::desktopResizeModeString(const DesktopResizeMode & mode)
    {
        switch(mode)
        {
            case DesktopResizeMode::Disabled:
                return "Disabled";

            case DesktopResizeMode::Success:
                return "Success";

            case DesktopResizeMode::ServerInform:
                return "ServerInform";

            case DesktopResizeMode::ClientRequest:
                return "ClientRequest";

            default:
                break;
        }

        return "Undefined";
    }

    const char* RFB::encodingName(int type)
    {
        switch(type)
        {
            case ENCODING_RAW:
                return "Raw";

            case ENCODING_COPYRECT:
                return "CopyRect";

            case ENCODING_RRE:
                return "RRE";

            case ENCODING_CORRE:
                return "CoRRE";

            case ENCODING_HEXTILE:
                return "HexTile";

            case ENCODING_ZLIB:
                return "ZLib";

            case ENCODING_TIGHT:
                return "Tight";

            case ENCODING_ZLIBHEX:
                return "ZLibHex";

            case ENCODING_TRLE:
                return "TRLE";

            case ENCODING_ZRLE:
                return "ZRLE";

            case ENCODING_DESKTOP_SIZE:
                return "DesktopSize";

            case ENCODING_EXT_DESKTOP_SIZE:
                return "ExtendedDesktopSize";

            case ENCODING_LAST_RECT:
                return "ExtendedLastRect";

            case ENCODING_COMPRESS9:
                return "ExtendedCompress9";

            case ENCODING_COMPRESS8:
                return "ExtendedCompress8";

            case ENCODING_COMPRESS7:
                return "ExtendedCompress7";

            case ENCODING_COMPRESS6:
                return "ExtendedCompress6";

            case ENCODING_COMPRESS5:
                return "ExtendedCompress5";

            case ENCODING_COMPRESS4:
                return "ExtendedCompress4";

            case ENCODING_COMPRESS3:
                return "ExtendedCompress3";

            case ENCODING_COMPRESS2:
                return "ExtendedCompress2";

            case ENCODING_COMPRESS1:
                return "ExtendedCompress1";

            case ENCODING_CONTINUOUS_UPDATES:
                return "ExtendedContinuousUpdates";

            default:
                break;
        }

        return "unknown";
    }
}
