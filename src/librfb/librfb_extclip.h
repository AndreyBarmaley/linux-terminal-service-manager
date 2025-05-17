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

#ifndef _LIBRFB_EXTCLIP_
#define _LIBRFB_EXTCLIP_

#include <mutex>
#include <vector>
#include <string>
#include <cinttypes>

#include "ltsm_sockets.h"

#ifdef __LINUX__
#include "ltsm_xcb_wrapper.h"
#endif

namespace LTSM
{
    enum ExtClipCaps
    {
        TypeText = 1 << 0,
        TypeRtf  = 1 << 1,
        TypeHtml = 1 << 2,
        TypeDib  = 1 << 3,
        TypeFiles= 1 << 4,
        Type5    = 1 << 5,
        Type6    = 1 << 6,
        Type7    = 1 << 7,
        Type8    = 1 << 8,
        Type9    = 1 << 9,
        Type10   = 1 << 10,
        Type11   = 1 << 11,
        Type12   = 1 << 12,
        Type13   = 1 << 13,
        Type14   = 1 << 14,
        Type15   = 1 << 15,
        OpCaps   = 1 << 24,
        OpRequest= 1 << 25,
        OpPeek   = 1 << 26,
        OpNotify = 1 << 27,
        OpProvide= 1 << 28
    };

    struct ExtClipTypes
    {
        int flags = 0;
        uint32_t textSize = 0;
        uint32_t rtfSize  = 0;
        uint32_t htmlSize = 0;
        uint32_t dibSize  = 0;
        uint32_t fileSize = 0;
    };

    namespace RFB
    {
        class ServerEncoder;

        /// ExtClip
        class ExtClip
        {
        protected:
            int remoteExtClipboardFlags = 0;
            uint32_t remoteExtClipTypeTextSz = 0;
            uint32_t remoteExtClipTypeRtfSz  = 0;
            uint32_t remoteExtClipTypeHtmlSz = 0;
            uint32_t remoteExtClipTypeDibSz  = 0;
            uint32_t remoteExtClipTypeFilesSz= 0;
    
            int localExtClipboardFlags = 0;
            uint32_t localExtClipTypeTextSz = 0;
            uint32_t localExtClipTypeRtfSz  = 0;
            uint32_t localExtClipTypeHtmlSz = 0;
            uint32_t localExtClipTypeDibSz  = 0;
            uint32_t localExtClipTypeFilesSz= 0;
            
            std::mutex localProvide;
            uint32_t localProvideTypes = 0;

        protected:
            void recvExtClipboardCapsContinue(uint32_t flags, StreamBuf && sb);
            void recvExtClipboardRequest(uint32_t flags);
            void recvExtClipboardPeek(void);
            void recvExtClipboardNotify(uint32_t flags);
            void recvExtClipboardProvide(StreamBuf && sb);

            void sendExtClipboardRequest(uint16_t types);
            void sendExtClipboardPeek(void);
            void sendExtClipboardNotify(uint16_t types);
            void sendExtClipboardProvide(uint16_t types);

            virtual uint16_t extClipboardLocalTypes(void) const = 0;
            virtual std::vector<uint8_t> extClipboardLocalData(uint16_t type) const = 0;
            virtual void extClipboardRemoteTypesEvent(uint16_t type) = 0;
            virtual void extClipboardRemoteDataEvent(uint16_t type, std::vector<uint8_t> &&) = 0;
            virtual void extClipboardSendEvent(const std::vector<uint8_t> & buf) = 0;

        public:
            ExtClip() = default;
            virtual ~ExtClip() = default;

            void sendExtClipboardCaps(void);
            void recvExtClipboardCaps(StreamBuf &&);

            void setExtClipboardRemoteCaps(int);
            int extClipboardRemoteCaps(void) const;

            void setExtClipboardLocalCaps(int);
            int extClipboardLocalCaps(void) const;

#ifdef __LINUX__
            static std::vector<xcb_atom_t> typesToX11Atoms(uint16_t types, const XCB::Connector &);
            static uint16_t x11AtomToType(xcb_atom_t);
            static void x11AtomsUpdate(const XCB::Connector &);
#endif
        };
    }
}

#endif // _LIBRFB_EXTCLIP_
