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

#include <string>
#include <stdexcept>
#include <algorithm>

#include "ltsm_application.h"
#include "ltsm_streambuf.h"
#include "ltsm_tools.h"
#include "ltsm_librfb.h"
#include "librfb_extclip.h"

#ifdef __LINUX__
#include "librfb_server.h"
#endif

namespace LTSM
{
#ifdef __LINUX__
    xcb_atom_t atomUtf8String = XCB_ATOM_NONE;
    xcb_atom_t atomText = XCB_ATOM_NONE;
    xcb_atom_t atomString = XCB_ATOM_NONE;
    xcb_atom_t atomTextPlain = XCB_ATOM_NONE;
    xcb_atom_t atomTextPlainUtf8 = XCB_ATOM_NONE;

    xcb_atom_t atomTextRtf = XCB_ATOM_NONE;
    xcb_atom_t atomTextRichText = XCB_ATOM_NONE;

    xcb_atom_t atomTextHtml = XCB_ATOM_NONE;
    xcb_atom_t atomTextHtmlUtf8 = XCB_ATOM_NONE;

    xcb_atom_t atomImagePng = XCB_ATOM_NONE;
    xcb_atom_t atomImageGif = XCB_ATOM_NONE;
    xcb_atom_t atomImageJpg = XCB_ATOM_NONE;
    xcb_atom_t atomImageBmp = XCB_ATOM_NONE;

    xcb_atom_t atomListFiles1 = XCB_ATOM_NONE;
    xcb_atom_t atomListFiles2 = XCB_ATOM_NONE;
    xcb_atom_t atomListFiles3 = XCB_ATOM_NONE;

    void RFB::ExtClip::x11AtomsUpdate(const XCB::Connector & x11)
    {
        atomUtf8String = x11.getAtom("UTF8_STRING");
        atomText = x11.getAtom("TEXT");
        atomString = x11.getAtom("STRING");
        atomTextPlain = x11.getAtom("text/plain");
        atomTextPlainUtf8 = x11.getAtom("text/plain;charset=utf-8");

        atomTextRtf = x11.getAtom("text/rtf");
        atomTextRichText = x11.getAtom("text/richtext");

        atomTextHtml = x11.getAtom("text/html");
        atomTextHtmlUtf8 = x11.getAtom("text/html;charset=utf-8");

        atomImagePng = x11.getAtom("image/png");
        atomImageGif = x11.getAtom("image/gif");
        atomImageJpg = x11.getAtom("image/jpeg");
        atomImageBmp = x11.getAtom("image/bmp");

        atomListFiles1 = x11.getAtom("text/uri-list");
        atomListFiles2 = x11.getAtom("x-special/gnome-copied-files");
        atomListFiles3 = x11.getAtom("x-special/mate-copied-files");
    }

    std::vector<xcb_atom_t> RFB::ExtClip::typesToX11Atoms(uint16_t types, const XCB::Connector & x11)
    {
        std::vector<xcb_atom_t> targets;
        targets.reserve(32);

        if(types & ExtClipCaps::TypeText)
        {
            targets.push_back(atomUtf8String);
            targets.push_back(atomTextPlain);
            targets.push_back(atomTextPlainUtf8);
            targets.push_back(atomText);
            targets.push_back(atomString);
        }

        if(types & ExtClipCaps::TypeRtf)
        {
            targets.push_back(atomTextRtf);
            targets.push_back(atomTextRichText);
        }

        if(types & ExtClipCaps::TypeHtml)
        {
            targets.push_back(atomTextHtml);
            targets.push_back(atomTextHtmlUtf8);
        }

        if(types & ExtClipCaps::TypeDib)
        {
            targets.push_back(atomImagePng);
            targets.push_back(atomImageGif);
            targets.push_back(atomImageJpg);
            targets.push_back(atomImageBmp);
        }

        if(types & ExtClipCaps::TypeFiles)
        {
            targets.push_back(atomListFiles1);
            targets.push_back(atomListFiles2);
            targets.push_back(atomListFiles3);
        }

        return targets;
    }

    uint16_t RFB::ExtClip::x11AtomToType(xcb_atom_t atom)
    {
        if(atom == atomUtf8String ||
            atom == atomTextPlain || atom == atomTextPlainUtf8 ||
            atom == atomText || atom == atomString)
        {
            return ExtClipCaps::TypeText;
        }

        if(atom == atomTextRtf || atom == atomTextRichText)
        {
            return ExtClipCaps::TypeRtf;
        }

        if(atom == atomTextHtml || atom == atomTextHtmlUtf8)
        {
            return ExtClipCaps::TypeHtml;
        }

        if(atom == atomImagePng || atom == atomImageGif ||
            atom == atomImageJpg || atom == atomImageBmp)
        {
            return ExtClipCaps::TypeDib;
        }

        if(atom == atomListFiles1 || atom == atomListFiles2 ||
            atom == atomListFiles3)
        {
            return ExtClipCaps::TypeFiles;
        }

        Application::warning("%s: empty types", __FUNCTION__);
        return 0;
    }
#endif

    void RFB::ExtClip::recvExtClipboardCaps(StreamBuf && sb)
    {
        if(4 > sb.last())
        {
            Application::error("%s: invalid format, failed `%s'", __FUNCTION__, "length");
            throw rfb_error(NS_FuncName);
        }
 
        auto flags = sb.readIntBE32();
        Application::debug(DebugType::Clip, "%s: flags: 0x%08" PRIx32, __FUNCTION__, flags);

        if(flags & ExtClipCaps::OpCaps)
        {
            auto typesCount = Tools::maskCountBits(flags & 0xFFFF);

            if(typesCount * 4 > sb.last())
            {
                Application::error("%s: invalid format, failed `%s'", __FUNCTION__, "types count");
                throw rfb_error(NS_FuncName);
            }

            recvExtClipboardCapsContinue(flags, std::move(sb));
        }
        else
        {
            const int allop = ExtClipCaps::OpRequest | ExtClipCaps::OpPeek | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide;
            auto opCount = Tools::maskCountBits(flags & allop);

            if(1 != opCount)
            {
                Application::warning("%s: ext clipboard invalid flags: 0x%08" PRIx32, __FUNCTION__, flags);
                return;
            }

            switch(flags & allop)
            {
                case ExtClipCaps::OpRequest: return recvExtClipboardRequest(flags);
                case ExtClipCaps::OpPeek:    return recvExtClipboardPeek();
                case ExtClipCaps::OpNotify:  return recvExtClipboardNotify(flags);
                case ExtClipCaps::OpProvide: return recvExtClipboardProvide(std::move(sb));
                default: break;
            }
        }
    }

    void RFB::ExtClip::recvExtClipboardCapsContinue(uint32_t flags, StreamBuf && sb)
    {
        Application::debug(DebugType::Clip, "%s: flags: 0x%08" PRIx32 ", data length: %" PRIu32, __FUNCTION__, flags, sb.last());

        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        auto typesCount = Tools::maskCountBits(flags & 0xFFFF);

        remoteExtClipTypeTextSz = 0;
        remoteExtClipTypeRtfSz  = 0;
        remoteExtClipTypeHtmlSz = 0;
        remoteExtClipTypeDibSz  = 0;
        remoteExtClipTypeFilesSz= 0;

        if(typesCount)
        {
            if(ExtClipCaps::TypeText & flags)
            {
                remoteExtClipTypeTextSz = sb.readIntBE32();
                typesCount--;
            }

            if(ExtClipCaps::TypeRtf & flags)
            {
                remoteExtClipTypeRtfSz = sb.readIntBE32();
                typesCount--;
            }

            if(ExtClipCaps::TypeHtml & flags)
            {
                remoteExtClipTypeHtmlSz = sb.readIntBE32();
                typesCount--;
            }

            if(ExtClipCaps::TypeDib & flags)
            {
                remoteExtClipTypeDibSz = sb.readIntBE32();
                typesCount--;
            }

            if(ExtClipCaps::TypeFiles & flags)
            {
                remoteExtClipTypeFilesSz = sb.readIntBE32();
                typesCount--;
            }
        }

        // skip unknown types size
        if(sb.last())
        {
            auto tmp = Tools::buffer2hexstring(sb.data(), sb.data() + sb.last(), 2, ",", false);
            Application::warning("%s: ext clipboard unknown data: [%s]", __FUNCTION__, tmp.c_str());
        }

        remoteExtClipboardFlags = flags & (~ExtClipCaps::OpCaps);
    }

    void RFB::ExtClip::recvExtClipboardRequest(uint32_t flags)
    {
        Application::debug(DebugType::Clip, "%s: flags: 0x%08" PRIx32, __FUNCTION__, flags);
        // The recipient should respond with a PROVIDE message with the clipboard data for the formats indicated in flags.
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding

        if(localExtClipboardFlags & ExtClipCaps::OpRequest)
        {
            const int allowTypes = 0xFFFF & localExtClipboardFlags & flags;
            sendExtClipboardProvide(allowTypes);
        }
        else
        {
            Application::error("%s: ext clipboard unsupport op: 0x%08" PRIx32, __FUNCTION__, localExtClipboardFlags);
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::ExtClip::recvExtClipboardPeek(void)
    {
        Application::debug(DebugType::Clip, "%s", __FUNCTION__);
        // The recipient should send a new notify message indicating which formats are available.
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding

        if(localExtClipboardFlags & ExtClipCaps::OpPeek)
        {
            const int allowTypes = 0xFFFF & localExtClipboardFlags;
            sendExtClipboardNotify(allowTypes & extClipboardLocalTypes());
        }
        else
        {
            Application::error("%s: ext clipboard unsupport op: 0x%08" PRIx32, __FUNCTION__, localExtClipboardFlags);
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::ExtClip::recvExtClipboardNotify(uint32_t flags)
    {
        Application::debug(DebugType::Clip, "%s: flags: 0x%08" PRIx32, __FUNCTION__, flags);

        // This message indicates which formats are available on the remote side
        // and should be sent whenever the clipboard changes, or as a response to a peek message.
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding

        if(localExtClipboardFlags & ExtClipCaps::OpNotify)
        {
            const int allowTypes = 0xFFFF & remoteExtClipboardFlags & flags;
            extClipboardRemoteTypesEvent(allowTypes);
        }
        else
        {
            Application::error("%s: ext clipboard unsupport op: 0x%08" PRIx32, __FUNCTION__, localExtClipboardFlags);
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::ExtClip::recvExtClipboardProvide(StreamBuf && sb)
    {
        Application::debug(DebugType::Clip, "%s, data length: %" PRIu32, __FUNCTION__, sb.last());
        // This message includes the actual clipboard data and should be sent whenever the clipboard changes and the data for each format.
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding

        if(localExtClipboardFlags & ExtClipCaps::OpProvide)
        {
            const std::scoped_lock guard{localProvide};
            auto len = sb.readIntBE32();

            if(len)
            {
                auto zlib = std::make_unique<ZLib::InflateStream>();
                zlib->appendData(sb.read(len));

                // The header is followed by a Zlib stream which contains a pair of size and data for each format
                for(auto & type: Tools::maskUnpackBits(localProvideTypes))
                {
                    auto len = zlib->recvIntBE32();
                    auto raw = zlib->recvData(len);

                    extClipboardRemoteDataEvent(type, std::move(raw));
                    localProvideTypes &= ~type;
                }
            }
            else
            {
                Application::warning("%s: zlib empty", __FUNCTION__);
            }
        }
        else
        {
            Application::error("%s: ext clipboard unsupport op: 0x%08" PRIx32, __FUNCTION__, localExtClipboardFlags);
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::ExtClip::sendExtClipboardCaps(void)
    {
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        Application::debug(DebugType::Clip, "%s: server flags: 0x%08" PRIx32, __FUNCTION__, localExtClipboardFlags);

        const int allowFlags = ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml | ExtClipCaps::TypeDib |
            ExtClipCaps::OpRequest | ExtClipCaps::OpPeek | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide;

        const int serverAllow = localExtClipboardFlags & allowFlags;

        StreamBuf sb;

        // Following flags is an array indicating the maximing unsolicited size for each format:
        sb.writeIntBE32(ExtClipCaps::OpCaps | serverAllow);

        if(serverAllow & ExtClipCaps::TypeText)
        {
            sb.writeIntBE32(localExtClipTypeTextSz);
        }

        if(serverAllow & ExtClipCaps::TypeRtf)
        {
            sb.writeIntBE32(localExtClipTypeRtfSz);
        }

        if(serverAllow & ExtClipCaps::TypeHtml)
        {
            sb.writeIntBE32(localExtClipTypeHtmlSz);
        }

        if(serverAllow & ExtClipCaps::TypeDib)
        {
            sb.writeIntBE32(localExtClipTypeDibSz);
        }

        if(serverAllow & ExtClipCaps::TypeFiles)
        {
            sb.writeIntBE32(localExtClipTypeFilesSz);
        }

        extClipboardSendEvent(sb.rawbuf());
    }

    void RFB::ExtClip::sendExtClipboardRequest(uint16_t types)
    {
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        Application::debug(DebugType::Clip, "%s: types: 0x%04" PRIx16, __FUNCTION__, types);

        if(! types)
        {
            Application::warning("%s: types empty", __FUNCTION__);
            return;
        }

        const std::scoped_lock guard{localProvide};

        // skip types, see recvExtClipboardProvide
        if((localProvideTypes & types) == types)
        {
            Application::warning("%s: also provided, types: 0x%04" PRIx16, __FUNCTION__, types);
            return;
        }

        const int allowTypes = remoteExtClipboardFlags & types;

        StreamBuf sb;
        sb.writeIntBE32(ExtClipCaps::OpRequest | allowTypes);

        extClipboardSendEvent(sb.rawbuf());

        // The recipient should respond with a provide messages
        localProvideTypes |= allowTypes;
    }

    void RFB::ExtClip::sendExtClipboardPeek(void)
    {
        Application::debug(DebugType::Clip, "%s", __FUNCTION__);
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        StreamBuf sb;
        sb.writeIntBE32(ExtClipCaps::OpPeek);

        extClipboardSendEvent(sb.rawbuf());
    }

    void RFB::ExtClip::sendExtClipboardNotify(uint16_t types)
    {
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        Application::debug(DebugType::Clip, "%s: types: 0x%04" PRIx16, __FUNCTION__, types);

        const int allowTypes = remoteExtClipboardFlags & types;

        StreamBuf sb;
        sb.writeIntBE32(ExtClipCaps::OpNotify | allowTypes);

        extClipboardSendEvent(sb.rawbuf());
    }

    void RFB::ExtClip::sendExtClipboardProvide(uint16_t types)
    {
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        Application::debug(DebugType::Clip, "%s: types: 0x%04" PRIx16, __FUNCTION__, types);

        auto zlib = std::make_unique<ZLib::DeflateStream>();

        for(auto type: { ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml | ExtClipCaps::TypeDib |  ExtClipCaps::TypeFiles })
        {
            if(types & type)
            {
                auto buf = extClipboardLocalData(type);

                zlib->sendIntBE32(buf.size());
                zlib->sendData(buf);
            }
        }

        auto zip = zlib->deflateFlush();

        StreamBuf sb;
        sb.writeIntBE32(zip.size());
        sb.write(zip);

        extClipboardSendEvent(sb.rawbuf());
    }
    
    int RFB::ExtClip::extClipboardLocalCaps(void) const
    {
        return localExtClipboardFlags;
    }

    void RFB::ExtClip::setExtClipboardLocalCaps(int flags)
    {
        localExtClipboardFlags = flags;
    }

    int RFB::ExtClip::extClipboardRemoteCaps(void) const
    {
        return remoteExtClipboardFlags;
    }

    void RFB::ExtClip::setExtClipboardRemoteCaps(int flags)
    {
        remoteExtClipboardFlags = flags;
    }
}
