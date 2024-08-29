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

#ifndef _LTSM_LIBRFB_
#define _LTSM_LIBRFB_

#include <exception>
#include <functional>

#include "ltsm_framebuffer.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    struct rfb_error : public std::runtime_error
    {
        explicit rfb_error(std::string_view what) : std::runtime_error(what.data()) {}
    };

    namespace RFB
    {
        // RFB protocol constant
        const int VERSION_MAJOR = 3;
        const int VERSION_MINOR = 8;

        const int SECURITY_TYPE_NONE = 1;
        const int SECURITY_TYPE_VNC = 2;
        const int SECURITY_TYPE_TLS = 18;
        const int SECURITY_TYPE_VENCRYPT = 19;
        const int SECURITY_VENCRYPT01_PLAIN = 19;
        const int SECURITY_VENCRYPT01_TLSNONE = 20;
        const int SECURITY_VENCRYPT01_TLSVNC = 21;
        const int SECURITY_VENCRYPT01_TLSPLAIN = 22;
        const int SECURITY_VENCRYPT01_X509NONE = 23;
        const int SECURITY_VENCRYPT01_X509VNC = 24;
        const int SECURITY_VENCRYPT01_X509PLAIN = 25;
        const int SECURITY_VENCRYPT02_PLAIN = 256;
        const int SECURITY_VENCRYPT02_TLSNONE = 257;
        const int SECURITY_VENCRYPT02_TLSVNC = 258;
        const int SECURITY_VENCRYPT02_TLSPLAIN = 259;
        const int SECURITY_VENCRYPT02_X509NONE = 260;
        const int SECURITY_VENCRYPT02_X509VNC = 261;
        const int SECURITY_VENCRYPT02_X509PLAIN = 261;
        const int SECURITY_TYPE_GSSAPI = 77;

        const int SECURITY_RESULT_OK = 0;
        const int SECURITY_RESULT_ERR = 1;

        const int CLIENT_SET_PIXEL_FORMAT = 0;
        const int CLIENT_SET_ENCODINGS = 2;
        const int CLIENT_REQUEST_FB_UPDATE = 3;
        const int CLIENT_EVENT_KEY = 4;
        const int CLIENT_EVENT_POINTER = 5;
        const int CLIENT_CUT_TEXT = 6;
        const int CLIENT_CONTINUOUS_UPDATES = 150;
        const int CLIENT_SET_DESKTOP_SIZE = 251;

        const int SERVER_FB_UPDATE = 0;
        const int SERVER_SET_COLOURMAP = 1;
        const int SERVER_BELL = 2;
        const int SERVER_CUT_TEXT = 3;
        const int SERVER_CONTINUOUS_UPDATES = 150;

        // RFB protocol constants
        const int ENCODING_RAW = 0;
        const int ENCODING_COPYRECT = 1;
        const int ENCODING_RRE = 2;
        const int ENCODING_CORRE = 4;
        const int ENCODING_HEXTILE = 5;
        const int ENCODING_ZLIB = 6;
        const int ENCODING_TIGHT = 7;
        const int ENCODING_ZLIBHEX = 8;
        const int ENCODING_TRLE = 15;
        const int ENCODING_ZRLE = 16;

        // hextile constants
        const int HEXTILE_RAW = 1;
        const int HEXTILE_BACKGROUND = 2;
        const int HEXTILE_FOREGROUND = 4;
        const int HEXTILE_SUBRECTS = 8;
        const int HEXTILE_COLOURED = 16;
        const int HEXTILE_ZLIBRAW = 32;
        const int HEXTILE_ZLIB = 64;

        // pseudo encodings
        const int ENCODING_DESKTOP_SIZE = -223;
        const int ENCODING_EXT_DESKTOP_SIZE = -308;
        const int ENCODING_CONTINUOUS_UPDATES = -313;
        const int ENCODING_LAST_RECT = -224;
        const int ENCODING_RICH_CURSOR = -239;
        const int ENCODING_COMPRESS9 = -247;
        const int ENCODING_COMPRESS8 = -248;
        const int ENCODING_COMPRESS7 = -249;
        const int ENCODING_COMPRESS6 = -250;
        const int ENCODING_COMPRESS5 = -251;
        const int ENCODING_COMPRESS4 = -252;
        const int ENCODING_COMPRESS3 = -253;
        const int ENCODING_COMPRESS2 = -254;
        const int ENCODING_COMPRESS1 = -255;

        const int ENCODING_LTSM = 0x4C54534D;
        const int ENCODING_FFMPEG_H264 = 0x48464D50;
        const int ENCODING_FFMPEG_AV1 = 0x41563100;
        const int ENCODING_FFMPEG_VP8 = 0x56503800;
        const int PROTOCOL_LTSM = 119;

        struct ScreenInfo
        {
            uint32_t id = 0;
            uint16_t posx = 0;
            uint16_t posy = 0;
            uint16_t width = 0;
            uint16_t height = 0;
            uint32_t flags = 0;
        };

        enum class DesktopResizeStatus { ServerRuntime, ClientSide, OtherClient };
        enum class DesktopResizeError { NoError, ResizeProhibited, OutOfResources, InvalidScreenLayout };

        int desktopResizeErrorCode(const DesktopResizeError &);
        int desktopResizeStatusCode(const DesktopResizeStatus &);

        const char* encodingName(int type);

        /// SecurityInfo
        struct SecurityInfo
        {
            std::string passwdFile;
            std::string tlsPriority{"NORMAL:+ANON-ECDH:+ANON-DH"};
            std::string caFile;
            std::string certFile;
            std::string keyFile;
            std::string crlFile;
            std::string krb5Service;
            std::string krb5Name;

            int tlsDebug = 0;

            bool authNone = false;
            bool authVnc = false;
            bool authVenCrypt = false;
            bool authKrb5 = false;
            bool tlsAnonMode = false;
        };
    }
}

#endif // _LTSM_LIBRFB_
