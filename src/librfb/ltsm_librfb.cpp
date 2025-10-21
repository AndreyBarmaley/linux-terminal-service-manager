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

#include <algorithm>

#include "ltsm_application.h"
#include "ltsm_tools.h"
#include "ltsm_librfb.h"

namespace LTSM {
    int RFB::desktopResizeStatusCode(const DesktopResizeStatus & status) {
        switch(status) {
            case DesktopResizeStatus::ServerRuntime:
                return 0;

            case DesktopResizeStatus::ClientSide:
                return 1;

            case DesktopResizeStatus::OtherClient:
                return 2;
        }

        return 0;
    }

    int RFB::desktopResizeErrorCode(const DesktopResizeError & err) {
        switch(err) {
            case DesktopResizeError::NoError:
                return 0;

            case DesktopResizeError::ResizeProhibited:
                return 1;

            case DesktopResizeError::OutOfResources:
                return 2;

            case DesktopResizeError::InvalidScreenLayout:
                return 3;
        }

        return 0;
    }

    const char* RFB::encodingName(int type) {
        switch(type) {
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

            case ENCODING_RICH_CURSOR:
                return "ExtendedRichCursor";

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

            case ENCODING_EXT_CLIPBOARD:
                return "ExtendedClipboard";

            case ENCODING_CONTINUOUS_UPDATES:
                return "ExtendedContinuousUpdates";

            case ENCODING_LTSM:
                return "LTSM_Channels";

            case ENCODING_FFMPEG_H264:
                return "FFMPEG_H264";

            case ENCODING_FFMPEG_AV1:
                return "FFMPEG_AV1";

            case ENCODING_FFMPEG_VP8:
                return "FFMPEG_VP8";

            case ENCODING_LTSM_LZ4:
                return "LTSM_LZ4";

            case ENCODING_LTSM_TJPG:
                return "LTSM_TJPG";

            case ENCODING_LTSM_QOI:
                return "LTSM_QOI";

            case ENCODING_LTSM_CURSOR:
                return "LTSM_CURSOR";

            default:
                break;
        }

        return "unknown";
    }

    bool RFB::isVideoEncoding(int type) {
        auto types = {
            ENCODING_RAW, ENCODING_RRE, ENCODING_CORRE, ENCODING_HEXTILE,
            ENCODING_ZLIB, ENCODING_TIGHT, ENCODING_ZLIBHEX, ENCODING_TRLE, ENCODING_ZRLE,
            ENCODING_FFMPEG_H264, ENCODING_FFMPEG_AV1, ENCODING_FFMPEG_VP8, ENCODING_LTSM_LZ4, ENCODING_LTSM_TJPG, ENCODING_LTSM_QOI
        };

        return std::any_of(types.begin(), types.end(), [ = ](auto & enc) {
            return enc == type;
        });
    }

    int RFB::encodingType(std::string_view name) {
        auto types = {
            ENCODING_RAW, ENCODING_COPYRECT, ENCODING_RRE, ENCODING_CORRE, ENCODING_HEXTILE,
            ENCODING_ZLIB, ENCODING_TIGHT, ENCODING_ZLIBHEX, ENCODING_TRLE, ENCODING_ZRLE,
            ENCODING_DESKTOP_SIZE, ENCODING_EXT_DESKTOP_SIZE, ENCODING_LAST_RECT, ENCODING_RICH_CURSOR,
            ENCODING_COMPRESS9, ENCODING_COMPRESS8, ENCODING_COMPRESS7, ENCODING_COMPRESS6, ENCODING_COMPRESS5, ENCODING_COMPRESS4, ENCODING_COMPRESS3, ENCODING_COMPRESS2, ENCODING_COMPRESS1,
            ENCODING_EXT_CLIPBOARD, ENCODING_CONTINUOUS_UPDATES,
            ENCODING_LTSM, ENCODING_FFMPEG_H264, ENCODING_FFMPEG_AV1, ENCODING_FFMPEG_VP8, ENCODING_LTSM_LZ4, ENCODING_LTSM_TJPG, ENCODING_LTSM_QOI
        };

        for(const auto & type : types) {
            if(Tools::lower(name) == Tools::lower(encodingName(type))) {
                return type;
            }
        }

        return ENCODING_UNKNOWN;
    }

    std::string RFB::encodingOpts(int type) {
        switch(type) {
            case ENCODING_ZLIB:
                return Tools::joinToString("--encoding ", Tools::lower(encodingName(type)), ",zlev:<[1],2,3,4,5,6,7,8,9>");

            case ENCODING_LTSM_TJPG:
                return Tools::joinToString("--encoding ", Tools::lower(encodingName(type)), ",qual:85,samp:<[420],422,440,444,gray,411>");

            default:
                break;
        }

        return "";
    }

    // StreamBits
    bool Tools::StreamBits::empty(void) const {
        return vecbuf.empty() ||
               (vecbuf.size() == 1 && bitpos == 7);
    }

    const std::vector<uint8_t> & Tools::StreamBits::toVector(void) const {
        return vecbuf;
    }

    // StreamBitsPack
    Tools::StreamBitsPack::StreamBitsPack(size_t rez) {
        bitpos = 7;
        vecbuf.reserve(rez);
    }

    void Tools::StreamBitsPack::pushBit(bool v) {
        if(bitpos == 7) {
            vecbuf.push_back(0);
        }

        if(v) {
            const uint8_t mask = 1 << bitpos;
            vecbuf.back() |= mask;
        }

        if(bitpos == 0) {
            bitpos = 7;
        } else {
            bitpos--;
        }
    }

    void Tools::StreamBitsPack::pushAlign(void) {
        bitpos = 7;
    }

    void Tools::StreamBitsPack::pushValue(int val, size_t field) {
        // field 1: mask 0x0001, field 2: mask 0x0010, field 4: mask 0x1000
        size_t mask = 1ul << (field - 1);

        while(mask) {
            pushBit(val & mask);
            mask >>= 1;
        }
    }

    // StreamBitsUnpack
    Tools::StreamBitsUnpack::StreamBitsUnpack(std::vector<uint8_t> && v, size_t counts, size_t field) : StreamBits(std::move(v)) {
        // check size
        size_t bits = field * counts;
        size_t len = bits >> 3;

        if((len << 3) < bits) {
            len++;
        }

        if(len < toVector().size()) {
            Application::error("%s: %s", __FUNCTION__, "incorrect data size");
            throw std::out_of_range(NS_FuncName);
        }

        bitpos = (len << 3) - bits;
    }

    bool Tools::StreamBitsUnpack::popBit(void) {
        if(vecbuf.empty()) {
            Application::error("%s: %s", __FUNCTION__, "empty data");
            throw std::invalid_argument(NS_FuncName);
        }

        uint8_t mask = 1 << bitpos;
        bool res = vecbuf.back() & mask;

        if(bitpos == 7) {
            vecbuf.pop_back();
            bitpos = 0;
        } else {
            bitpos++;
        }

        return res;
    }

    int Tools::StreamBitsUnpack::popValue(size_t field) {
        // field 1: mask 0x0001, field 2: mask 0x0010, field 4: mask 0x1000
        size_t mask1 = 1 << (field - 1);
        size_t mask2 = 1;
        int val = 0;

        while(mask1) {
            if(popBit()) {
                val |= mask2;
            }

            mask1 >>= 1;
            mask2 <<= 1;
        }

        return val;
    }
}
