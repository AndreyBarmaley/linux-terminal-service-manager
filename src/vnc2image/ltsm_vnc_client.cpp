/***************************************************************************
 *   Copyright (C) 2022 by MultiCapture team <public.irkutsk@gmail.com>    *
 *                                                                         *
 *   Part of the MultiCapture engine:                                      *
 *   https://github.com/AndreyBarmaley/multi-capture                       *
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

#include <chrono>
#include <thread>
#include <cstring>
#include <fstream>
#include <algorithm>

#include "ltsm_vnc_client.h"

using namespace std::chrono_literals;

namespace RFB
{
    /* Connector */
    ClientConnector::ClientConnector(const SWE::JsonObject & jo)
        : streamIn(nullptr), streamOut(nullptr), debug(0), loopMessage(true), config(& jo)
    {
        debug = config->getInteger("debug", 0);

        // file debug
        auto netDebug = config->getString("network:debug");

        if(netDebug.size())
            socket.reset(new Network::TCPClientDebug(netDebug));
        else
            socket.reset(new Network::TCPClient());

        streamIn = streamOut = socket.get();
    }

    void ClientConnector::sendFlush(void)
    {
        if(loopMessage)
            streamOut->sendFlush();
    }

    void ClientConnector::sendRaw(const void* ptr, size_t len)
    {
        if(loopMessage)
            streamOut->sendRaw(ptr, len);
    }

    void ClientConnector::recvRaw(void* ptr, size_t len) const
    {
        if(loopMessage)
            streamIn->recvRaw(ptr, len);
    }

    void ClientConnector::recvRaw(void* ptr, size_t len, size_t timeout /* ms */) const
    {
        if(loopMessage)
            streamIn->recvRaw(ptr, len, timeout);
    }

    bool ClientConnector::hasInput(void) const
    {
        return loopMessage ? streamIn->hasInput() : false;
    }

    size_t ClientConnector::hasInput(void) const
    {
        return loopMessage ? streamIn->hasData() : 0;
    }

    void ClientConnector::shutdown(void)
    {
        loopMessage = false;
    }

    bool ClientConnector::communication(const std::string & host, int port, const std::string & password)
    {
        if(! static_cast<Network::TCPClient*>(socket.get())->connect(host, port))
        {
            Application::error("%s: connect failed, host: %s, port: %d", __FUNCTION__, host.c_str(), port);
            return false;
        }

        if(debug)
            Application::debug("%s: connect to host: %s, port: %d", __FUNCTION__, host.c_str(), port);

        // https://vncdotool.readthedocs.io/en/0.8.0/rfbproto.html
        // RFB 1.7.1.1 version
        const std::string version = SWE::StringFormat("RFB 00%1.00%2\n").arg(RFB::VERSION_MAJOR).arg(RFB::VERSION_MINOR);
        std::string magick = recvString(12);

        if(magick.empty())
        {
            Application::error("%s: handshake failure", __FUNCTION__);
            return false;
        }

        if(debug)
            Application::debug("%s: RFB 1.7.1.1, handshake version: %s", __FUNCTION__, magick.substr(0, magick.size() - 1).c_str());

        if(magick != version)
        {
            Application::error("%s: handshake failure", __FUNCTION__);
            return false;
        }

        // 12 bytes
        sendString(version).sendFlush();

        // RFB 1.7.1.2 security
        int counts = recvInt8();
        if(debug)
            Application::debug("%s: RFB 1.7.1.2, security counts: %d", __FUNCTION__, counts);

        if(0 == counts)
        {
            int len = recvIntBE32();
            auto err = recvString(len);
            ERROR(err);
            return false;
        }

        std::vector<int> security;
        while(0 < counts--)
            security.push_back(recvInt8());

        if(std::any_of(security.begin(), security.end(), [=](auto & val){ return val == RFB::SECURITY_TYPE_NONE; }))
        {
            if(debug)
                Application::debug("%s: RFB 1.7.2.1, security: noauth", __FUNCTION__);
            sendInt8(RFB::SECURITY_TYPE_NONE).sendFlush();
        }
        else
        {
            if(std::none_of(security.begin(), security.end(), [=](auto & val){ return val == RFB::SECURITY_TYPE_VNC; }))
            {
                Application::error("%s: security vnc: not supported", __FUNCTION__);
                return false;
            }

            if(password.empty())
            {
                Application::error("%s: security vnc: password empty", __FUNCTION__);
                return false;
            }

            if(debug)
                DEBUG("RFB 1.7.2.2" << ", security: vnc auth");
            sendInt8(RFB::SECURITY_TYPE_VNC).sendFlush();
            // recv challenge 16 bytes
            auto challenge = recvData(16);
            auto crypt = TLS::encryptDES(challenge, password);
            sendRaw(crypt.data(), crypt.size());
            sendFlush();
        }

        // RFB 1.7.1.3 security result
        if(RFB::SECURITY_RESULT_OK != recvIntBE32())
        {
            int len = recvIntBE32();
            auto err = recvString(len);
            ERROR(err);
            return false;
        }

        bool shared = config->getBoolean("shared", false);
        if(debug)
            DEBUG("RFB 1.7.3.1" << ", send share flags: " << String::Bool(shared));
        // RFB 6.3.1 client init (shared flag)
        sendInt8(shared ? 1 : 0).sendFlush();
        
        // RFB 6.3.2 server init
        auto fbWidth = recvIntBE16();
        auto fbHeight = recvIntBE16();
        if(debug)
            DEBUG("RFB 1.7.3.2" << ", remote framebuffer size: " << fbWidth << "x" << fbHeight);

        // recv server pixel format
        PixelFormat serverFormat;
        serverFormat.bitsPerPixel = recvInt8();
        serverFormat.depth = recvInt8();
        if(0 != recvInt8())
        {
            serverFormat.flags |= PixelFormat::BigEndian;
        }
        if(0 != recvInt8())
        {
            serverFormat.flags |= PixelFormat::TrueColor;
        }
        serverFormat.redMax = recvIntBE16();
        serverFormat.greenMax = recvIntBE16();
        serverFormat.blueMax = recvIntBE16();
        serverFormat.redShift = recvInt8();
        serverFormat.greenShift = recvInt8();
        serverFormat.blueShift = recvInt8();
        recvSkip(3);

        if(2 < debug)
        {
            DEBUG("RFB 1.7.3.2" << ", remote pixel format: "<<
                " bpp: " << (int) serverFormat.bitsPerPixel <<
                ", depth: " << (int) serverFormat.depth <<
                ", big endian: " << (int) serverFormat.bigEndian() <<
                ", true color: " << (int) serverFormat.trueColor() <<
                ", red(" << serverFormat.redMax << ", " << (int) serverFormat.redShift <<
                "), green(" << serverFormat.greenMax << ", " << (int) serverFormat.greenShift <<
                "), blue(" << serverFormat.blueMax << ", " << (int) serverFormat.blueShift << ")");
        }

        // check server format
        switch(serverFormat.bitsPerPixel)
        {
            case 32: case 16: case 8:
                break;

            default:
                ERROR("unknown server pixel format");
                return false;
        }

        if(! serverFormat.trueColor() || serverFormat.redMax == 0 || serverFormat.greenMax == 0 || serverFormat.blueMax == 0)
        {
            ERROR("unsupported pixel format");
            return false;
        }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        bool big_endian = false;
#else
        bool big_endian = true;
#endif

        // bpp: 32, depth: 24, bigendian, truecol, rgb format
        auto clientFormat = PixelFormat(serverFormat.bitsPerPixel, 24, big_endian, true,
                                serverFormat.rmask(), serverFormat.gmask(), serverFormat.bmask());

        fbPtr.reset(new FrameBuffer(Region(0, 0, fbWidth, fbHeight), clientFormat));

        // recv name desktop
        auto nameLen = recvIntBE32();
        auto nameDesktop = recvString(nameLen);

        if(1 < debug)
            DEBUG("server desktop name: " << nameDesktop);

        return true;
    }

    void ClientConnector::messages(void)
    {
        std::initializer_list<int> encodings = { ENCODING_LAST_RECT,
                                        ENCODING_ZRLE, ENCODING_TRLE, ENCODING_HEXTILE,
                                        ENCODING_ZLIB, ENCODING_CORRE, ENCODING_RRE, ENCODING_RAW };

        clientSetEncodings(encodings);
        clientPixelFormat();
        // request full update
        clientFrameBufferUpdateReq(false);

        if(this->debug)
            DEBUG("RFB 1.7.5" << ", wait remote messages...");

        auto cur = std::chrono::steady_clock::now();

        while(this->loopMessage)
        {
            auto now = std::chrono::steady_clock::now();

            if(std::chrono::milliseconds(300) <= now - cur)
            {
                // request incr update
                clientFrameBufferUpdateReq(true);
                cur = now;
            }

            if(this->hasInput())
            {
                int msgType = this->recvInt8();

                switch(msgType)
                {
                    case SERVER_FB_UPDATE:          this->serverFBUpdateEvent(); break;
                    case SERVER_SET_COLOURMAP:      this->serverSetColorMapEvent(); break;
                    case SERVER_BELL:               this->serverBellEvent(); break;
                    case SERVER_CUT_TEXT:           this->serverCutTextEvent(); break;

                    default:
                    {
                        ERROR("unknown message type: " << SWE::String::hex(msgType, 2));
                        this->loopMessage = false;
                    }
                }
            }
            else
            {
                std::this_thread::sleep_for(5ms);
            }
        }
    }

    void ClientConnector::clientPixelFormat(void)
    {
        auto & clientFormat = fbPtr->pixelFormat();

        if(debug)
        {
            DEBUG("RFB 1.7.4.1" <<
                ", bpp: " << (int) clientFormat.bitsPerPixel <<
                ", depth: " << (int) clientFormat.depth <<
                ", big endian: " << (int) clientFormat.bigEndian() <<
                ", true color: " << (int) clientFormat.trueColor() <<
                ", red(" << clientFormat.redMax << ", " << (int) clientFormat.redShift <<
                "), green(" << clientFormat.greenMax << ", " << (int) clientFormat.greenShift <<
                "), blue(" << clientFormat.blueMax << ", " << (int) clientFormat.blueShift << ")");
        }

        // send pixel format
        sendInt8(RFB::CLIENT_SET_PIXEL_FORMAT);
        sendZero(3); // padding
        sendInt8(clientFormat.bitsPerPixel);
        sendInt8(clientFormat.depth);
        sendInt8(clientFormat.bigEndian());
        sendInt8(clientFormat.trueColor());
        sendIntBE16(clientFormat.redMax);
        sendIntBE16(clientFormat.greenMax);
        sendIntBE16(clientFormat.blueMax);
        sendInt8(clientFormat.redShift);
        sendInt8(clientFormat.greenShift);
        sendInt8(clientFormat.blueShift);
        sendZero(3); // padding
        sendFlush();
    }

    void ClientConnector::clientSetEncodings(std::initializer_list<int> encodings)
    {
        if(debug)
            DEBUG("RFB 1.7.4.2" << ", count: " << encodings.size());

        sendInt8(RFB::CLIENT_SET_ENCODINGS);
        sendZero(1); // padding
        sendIntBE16(encodings.size());
        for(auto val : encodings)
            sendIntBE32(val);
        sendFlush();
    }

    void ClientConnector::clientFrameBufferUpdateReq(bool incr)
    {
        clientFrameBufferUpdateReq(fbPtr->region(), incr);
    }

    void ClientConnector::clientFrameBufferUpdateReq(const XCB::Region & reg, bool incr)
    {
        if(debug)
            DEBUG("RFB 1.7.4.3" << ", region [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]");

        // send framebuffer update request
        sendInt8(CLIENT_REQUEST_FB_UPDATE);
        sendInt8(incr ? 1 : 0);
        sendIntBE16(reg.x);
        sendIntBE16(reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        sendFlush();
    }

    void ClientConnector::serverFBUpdateEvent(void)
    {
        auto tick = SWE::Tools::ticks();

        // padding
        recvSkip(1);
        auto numRects = recvIntBE16();
        Region reg;

        if(debug)
            DEBUG("RFB 1.7.5.1" << ", num rects: " << numRects);

        const std::lock_guard<std::mutex> lock(fbChange);

        while(0 < numRects--)
        {
            reg.x = recvIntBE16();
            reg.y = recvIntBE16();
            reg.width = recvIntBE16();
            reg.height = recvIntBE16();
            int encodingType = recvIntBE32();

            if(1 < debug)
            {
                DEBUG("region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]" <<
                        ", encodingType: " << RFB::encodingName(encodingType));
            }

            switch(encodingType)
            {
                case ENCODING_RAW:      recvDecodingRaw(reg); break;
                case ENCODING_RRE:      recvDecodingRRE(reg, false); break;
                case ENCODING_CORRE:    recvDecodingRRE(reg, true); break;
                case ENCODING_HEXTILE:  recvDecodingHexTile(reg); break;
                case ENCODING_TRLE:     recvDecodingTRLE(reg, false); break;
                case ENCODING_ZLIB:     recvDecodingZlib(reg); break;
                case ENCODING_ZRLE:     recvDecodingTRLE(reg, true); break;

                case ENCODING_LAST_RECT:
                    recvDecodingLastRect(reg);
                    numRects = 0;
                    break;

                default:
                    throw std::runtime_error(SWE::StringFormat("%1: unknown encoding: %2").arg(__FUNCTION__).arg(SWE::String::hex(encodingType, 8)));
            }
        }

        if(debug)
            DEBUG("fb update: " << SWE::Tools::ticks() - tick << "ms");
    }

    void ClientConnector::serverSetColorMapEvent(void)
    {
        // padding
        recvSkip(1);
        auto firstColor = recvIntBE16();
        auto numColors = recvIntBE16();
        if(debug)
            DEBUG("RFB 1.7.5.2" << ", num colors: " << numColors << ", first color: " << firstColor);

        while(0 < numColors--)
        {
            size_t cr = recvInt8();
            size_t cg = recvInt8();
            size_t cb = recvInt8();

            if(2 < debug)
            {
                DEBUG("color [" << cr << "," << cg << "," << cb << "]");
            }
        }
    }

    void ClientConnector::serverBellEvent(void)
    {
        if(debug)
            DEBUG("RFB 1.7.5.3");
    }

    void ClientConnector::serverCutTextEvent(void)
    {
        // padding
        recvSkip(3);
        auto length = recvIntBE32();

        if(debug)
            DEBUG("RFB 1.7.5.4" << ", length: " << length);

        if(0 < length)
            recvSkip(length);
    }

    void ClientConnector::syncFrameBuffer(Surface & sf)
    {
        const std::lock_guard<std::mutex> lock(fbChange);

        auto & pixelFormat = fbPtr->pixelFormat();
        SDL_Surface* ptr = SDL_CreateRGBSurfaceFrom(fbPtr->pitchData(0), fbPtr->width(), fbPtr->height(),
                            fbPtr->bitsPerPixel(), fbPtr->pitchSize(), pixelFormat.rmask(), pixelFormat.gmask(), pixelFormat.bmask(), 0);

        sf = Surface::copy(ptr);
    }

    void ClientConnector::recvDecodingRaw(const XCB::Region & reg)
    {
        if(1 < debug)
            DEBUG("decoding region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]");

        FrameBuffer fb(reg, *fbPtr);

        for(int yy = 0; yy < reg.height; ++yy)
            recvRaw(fb.pitchData(yy), fb.pitchSize());
    }

    void ClientConnector::recvDecodingLastRect(const XCB::Region & reg)
    {
        if(1 < debug)
        {
            DEBUG("decoding region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]");
        }
    }

    void ClientConnector::recvDecodingRRE(const XCB::Region & reg, bool corre)
    {
        if(2 < debug)
        {
            DEBUG("decoding region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]");
        }

        auto subRects = recvIntBE32();
        auto bgColor = recvPixel();

        if(3 < debug)
        {
            DEBUG("type: " << (corre ? "corre" : "rre") <<
                ", back pixel: " << SWE::String::hex(bgColor) << ", sub rects: " << subRects);
        }

        fbPtr->fillPixel(reg, bgColor);

        while(0 < subRects--)
        {
            Region dst;
            auto pixel = recvPixel();

            if(corre)
            {
                dst.x = recvInt8();
                dst.y = recvInt8();
                dst.width = recvInt8();
                dst.height = recvInt8();
            }
            else
            {
                dst.x = recvIntBE16();
                dst.y = recvIntBE16();
                dst.width = recvIntBE16();
                dst.height = recvIntBE16();
            }

            if(4 < debug)
            {
                DEBUG("type: " << (corre ? "corre" : "rre") <<
                        ", sub region: [" << dst.x << "," << dst.y << "," << dst.width << "," << dst.height << "]");
            }

            dst.x += reg.x;
            dst.y += reg.y;

            if(dst.x + dst.width > reg.x + reg.width || dst.y + dst.height > reg.y + reg.height)
                throw std::runtime_error(SWE::StringFormat("%1: failed, region: [%2,%3,%4,%5]").arg(__FUNCTION__).
                                        arg(dst.x).arg(dst.y).arg(dst.width).arg(dst.height));
            fbPtr->fillPixel(dst, pixel);
        }
    }

    void ClientConnector::recvDecodingHexTile(const XCB::Region & reg)
    {
        if(2 < debug)
        {
            DEBUG("decoding region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]");
        }

        int bgColor = -1;
        int fgColor = -1;
        const Size bsz = Size(16, 16);

        for(auto & reg0: Region::divideBlocks(reg, bsz))
            recvDecodingHexTileRegion(reg0, bgColor, fgColor);
    }

    void ClientConnector::recvDecodingHexTileRegion(const XCB::Region & reg, int & bgColor, int & fgColor)
    {
        auto flag = recvInt8();

        if(3 < debug)
        {
            DEBUG("subencoding mask: " << SWE::String::hex(flag, 2) << ", sub region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]");
        }

        if(flag & RFB::HEXTILE_RAW)
        {
            if(3 < debug)
            {
                DEBUG("type: " << "raw");
            }
    
            FrameBuffer fb(reg, *fbPtr);

            for(int yy = 0; yy < reg.height; ++yy)
                recvRaw(fb.pitchData(yy), fb.pitchSize());
        }
        else
        {
            if(flag & RFB::HEXTILE_BACKGROUND)
            {
                bgColor = recvPixel();

                if(3 < debug)
                {
                    DEBUG("type: " << "background" << ", pixel: " << SWE::String::hex(bgColor));
                }
            }

            fbPtr->fillPixel(reg, bgColor);

            if(flag & HEXTILE_FOREGROUND)
            {
                fgColor = recvPixel();
                flag &= ~HEXTILE_COLOURED;

                if(3 < debug)
                {
                    DEBUG("type: " << "foreground" << ", pixel: " << SWE::String::hex(fgColor));
                }
            }

            if(flag & HEXTILE_SUBRECTS)
            {
                int subRects = recvInt8();
                Region dst;

                if(3 < debug)
                {
                    DEBUG("type: " << "subrects" << ", count: " << subRects);
                }

                while(0 < subRects--)
                {
                    auto pixel = fgColor;
                    if(flag & HEXTILE_COLOURED)
                    {
                        pixel = recvPixel();
                        if(3 < debug)
                        {
                            DEBUG("type: " << "colored" << ", pixel: " << SWE::String::hex(pixel));
                        }
                    }

                    auto val1 = recvInt8();
                    auto val2 = recvInt8();

                    dst.x = (0x0F & (val1 >> 4));
                    dst.y = (0x0F & val1);
                    dst.width = 1 + (0x0F & (val2 >> 4));
                    dst.height = 1 + (0x0F & val2);

                    if(4 < debug)
                    {
                        DEBUG("type: " << "subrects" << ", rect: [" << dst.x << "," << dst.y << "," << dst.width << "," << dst.height << "]" <<
                                     ", pixel: " << SWE::String::hex(pixel));
                    }

                    dst.x += reg.x;
                    dst.y += reg.y;

                    if(dst.x + dst.width > reg.x + reg.width || dst.y + dst.height > reg.y + reg.height)
                        throw std::runtime_error(SWE::StringFormat("%1: failed, hextile, region: [%2,%3,%4,%5]").arg(__FUNCTION__).
                                        arg(dst.x).arg(dst.y).arg(dst.width).arg(dst.height));

                    fbPtr->fillPixel(dst, pixel);
                }
            }
        }
    }

    void ClientConnector::recvDecodingZlib(const XCB::Region & reg)
    {
        if(2 < debug)
        {
            DEBUG("decoding region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]");
        }

        zlibInflateStart();
        recvDecodingRaw(reg);
        zlibInflateStop();
    }

    void ClientConnector::recvDecodingTRLE(const XCB::Region & reg, bool zrle)
    {
        if(2 < debug)
        {
            DEBUG("decoding region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]");
        }

        const Size bsz = Size(64, 64);

        if(zrle)
            zlibInflateStart();

        for(auto & reg0: Region::divideBlocks(reg, bsz))
            recvDecodingTRLERegion(reg0, zrle);

        if(zrle)
            zlibInflateStop();
    }

    void ClientConnector::recvDecodingTRLERegion(const XCB::Region & reg, bool zrle)
    {
        auto type = recvInt8();

        if(3 < debug)
        {
            DEBUG("subencoding type: " << SWE::String::hex(type, 2) << ", sub region: [" << reg.x << "," << reg.y << "," << reg.width << "," << reg.height << "]" << ", zrle: " << SWE::String::Bool(zrle));
        }

        // trle raw
        if(0 == type)
        {
            if(3 < debug)
            {
                DEBUG("type: " << "raw");
            }

            for(auto coord = PointIterator(0, 0, reg.toSize()); coord.isValid(); ++coord)
            {
                auto pixel = recvCPixel();
                fbPtr->setPixel(reg.topLeft() + coord, pixel);
            }

            if(4 < debug)
                DEBUG("type: " << "raw" << ": compete");
        }
        else
        // trle solid
        if(1 == type)
        {

            auto solid = recvCPixel();

            if(3 < debug)
            {
                DEBUG("type: " << "solid" << ", pixel: " << SWE::String::hex(solid));
            }

            fbPtr->fillPixel(reg, solid);

            if(4 < debug)
                DEBUG("type: " << "solid" << ": compete");
        }
        else
        if(2 <= type && type <= 16)
        {
            size_t field = 1;

            if(4 < type)
                field = 4;
            else
            if(2 < type)
                field = 2;

            size_t bits = field * reg.width;
            size_t rowsz = bits >> 3;
            if((rowsz << 3) < bits) rowsz++;

            //  recv palette
            std::vector<int> palette(type);
            for(auto & val : palette) val = recvCPixel();

            if(2 < debug)
            {

                DEBUG("type: " << "packed palette" << ", size: " << palette.size());
                if(4 < debug)
                {
                    std::string str = SWE::Tools::buffer2HexString<int>(palette.data(), palette.size(), 8);
                    DEBUG("type: " << "packed palette" << ", palette: " << str);
                }
            }

            // recv packed rows
            for(int oy = 0; oy < reg.height; ++oy)
            {
                ::Tools::StreamBitsUnpack sb(recvData(rowsz), reg.width, field);

                for(int ox = reg.width - 1; 0 <= ox; --ox)
                {
                    auto pos = reg.topLeft() + Point(ox, oy);
                    auto index = sb.popValue(field);

                    if(4 < debug)
                    {
                        DEBUG("type: " << "packed palette" << ", pos: [" << pos.x << "," << pos.y << "], index: " << index);
                    }

                    if(index >= palette.size())
                        throw std::runtime_error(SWE::StringFormat("%1: out of range, index: %2, palette size: %3").arg(__FUNCTION__).arg(index).arg(palette.size()));

                    fbPtr->setPixel(pos, palette[index]);
                }
            }

            if(4 < debug)
                DEBUG("type: " << "packed palette" << ": compete");
        }
        else
        if((17 <= type && type <= 127) || type == 129)
        {
            throw std::runtime_error(SWE::StringFormat("%1: out of range, type: %2, unused").arg(__FUNCTION__).arg(type));
        }
        else
        if(128 == type)
        {
            if(3 < debug)
            {
                DEBUG("type: " << "plain rle");
            }

            auto coord = PointIterator(0, 0, reg.toSize());

            while(coord.isValid())
            {
                auto pixel = recvCPixel();
                auto runLength = recvRunLength();

                if(4 < debug)
                {
                    DEBUG("type: " << "plain rle" << ", pixel: " << SWE::String::hex(pixel) << ", length: " << runLength);
                }

                while(runLength--)
                {
                    fbPtr->setPixel(reg.topLeft() + coord, pixel);
                    ++coord;

                    if(! coord.isValid() && runLength)
                        throw std::runtime_error(SWE::StringFormat("%1: out of range, run length: %2").arg(__FUNCTION__).arg(runLength));
                }
            }

            if(4 < debug)
                DEBUG("type: " << "plain rle" << ": compete");
        }
        else
        if(130 <= type)
        {
            size_t palsz = type - 128;
            std::vector<int> palette(palsz);
            
            for(auto & val: palette)
                val = recvCPixel();

            if(3 < debug)
            {
                DEBUG("type: " << "rle palette" << ", size: " << palsz);

                if(4 < debug)
                {
                    std::string str = SWE::Tools::buffer2HexString<int>(palette.data(), palette.size(), 8);
                    DEBUG("type: " << "rle palette" << ", palette: " << str);
                }
            }

            auto coord = PointIterator(0, 0, reg.toSize());

            while(coord.isValid())
            {
                auto index = recvInt8();

                if(index < 128)
                {
                    if(index >= palette.size())
                        throw std::runtime_error(SWE::StringFormat("%1: out of range, index: %2, palette size: %3").arg(__FUNCTION__).arg(index).arg(palette.size()));

                    auto pixel = palette[index];
                    fbPtr->setPixel(reg.topLeft() + coord, pixel);

                    ++coord;
                }
                else
                {
                    index -= 128;

                    if(index >= palette.size())
                        throw std::runtime_error(SWE::StringFormat("%1: out of range, index: %2, palette size: %3").arg(__FUNCTION__).arg(index).arg(palette.size()));

                    auto pixel = palette[index];
                    auto runLength = recvRunLength();

                    if(4 < debug)
                    {
                        DEBUG("type: " << "rle palette" << ", index: " << index << ", length: " << runLength);
                    }

                    while(runLength--)
                    {
                        fbPtr->setPixel(reg.topLeft() + coord, pixel);
                        ++coord;

                        if(! coord.isValid() && runLength)
                            throw std::runtime_error(SWE::StringFormat("%1: out of range, run length: %2").arg(__FUNCTION__).arg(runLength));
                    }
                }
            }

            if(4 < debug)
                DEBUG("type: " << "rle palette" << ": compete");
        }
    }

    int ClientConnector::recvPixel(void)
    {
        auto & pf = fbPtr->pixelFormat();

        switch(pf.bytePerPixel())
        {
            case 4: return pf.bigEndian() ? recvIntBE32() : recvIntLE32();
            case 2: return pf.bigEndian() ? recvIntBE16() : recvIntLE16();
            case 1: return recvInt8();
            default: break;
        }

        throw std::runtime_error("recv pixel: unknown format");
        return 0;
    }

    int ClientConnector::recvCPixel(void)
    {
        auto & pf = fbPtr->pixelFormat();

        if(pf.trueColor() && pf.bitsPerPixel == 32)
        {
            auto colr = recvInt8();
            auto colg = recvInt8();
            auto colb = recvInt8();
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
            std::swap(colr, colb);
#endif
            return pf.pixel(Color(colr, colg, colb));
        }

        return recvPixel();
    }

    size_t ClientConnector::recvRunLength(void)
    {
        size_t length = 0;

        while(true)
        {
            auto val = recvInt8();
            length += val;

            if(val != 255)
                return length + 1;
        }

        return 0;
    }

    void ClientConnector::zlibInflateStart(bool uint16sz)
    {
        if(! zlib)
            zlib.reset(new Network::InflateStream());

        size_t zipsz = 0;

        if(uint16sz)
            zipsz = recvIntBE16();
        else
            zipsz = recvIntBE32();

        if(2 < debug)
            DEBUG("compress data length: " << zipsz);

        auto zip = recvData(zipsz);
        zlib->appendData(zip);

        streamIn = zlib.get();
    }
    
    void ClientConnector::zlibInflateStop(void)
    {
        streamIn = socket.get();
    }
}
