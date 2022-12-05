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
 **************************************************************************/

#include <chrono>
#include <thread>
#include <cstring>
#include <algorithm>

#include "ltsm_librfb.h"
#include "ltsm_application.h"
#include "librfb_client.h"

using namespace std::chrono_literals;

namespace LTSM
{
    /* RFB::ClientDecoder */
    void RFB::ClientDecoder::setInetStreamMode(void)
    {
        socket.reset(new InetStream());
        streamIn = streamOut = socket.get();
    }

    void RFB::ClientDecoder::setSocketStreamMode(int sockfd)
    {
        socket.reset(new SocketStream(sockfd));
        streamIn = streamOut = socket.get();
    }

    void RFB::ClientDecoder::sendFlush(void)
    {
        try
        {
            if(rfbMessages)
                streamOut->sendFlush();
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());

            streamOut->setError();
            rfbMessagesShutdown();
        }
    }

    void RFB::ClientDecoder::sendRaw(const void* ptr, size_t len)
    {
        try
        {
            if(rfbMessages)
                streamOut->sendRaw(ptr, len);
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());

            streamOut->setError();
            rfbMessagesShutdown();
        }
    }

    void RFB::ClientDecoder::recvRaw(void* ptr, size_t len) const
    {
        try
        {
            if(rfbMessages)
                streamIn->recvRaw(ptr, len);
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());

            const_cast<NetworkStream*>(streamIn)->setError();
            const_cast<ClientDecoder*>(this)->rfbMessagesShutdown();
        }
    }

    bool RFB::ClientDecoder::hasInput(void) const
    {
        try
        {
            if(rfbMessages)
                return streamIn->hasInput();
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());

            const_cast<NetworkStream*>(streamIn)->setError();
            const_cast<ClientDecoder*>(this)->rfbMessagesShutdown();
        }

        return false;
    }

    size_t RFB::ClientDecoder::hasData(void) const
    {
        try
        {
            if(rfbMessages)
                return streamIn->hasData();
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());

            const_cast<NetworkStream*>(streamIn)->setError();
            const_cast<ClientDecoder*>(this)->rfbMessagesShutdown();
        }

        return 0;
    }

    uint8_t RFB::ClientDecoder::peekInt8(void) const
    {
        try
        {
            if(rfbMessages)
                return streamIn->peekInt8();
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());

            const_cast<NetworkStream*>(streamIn)->setError();
            const_cast<ClientDecoder*>(this)->rfbMessagesShutdown();
        }

        return 0;
    }

    bool RFB::ClientDecoder::authVncInit(std::string_view password)
    {
        // recv challenge 16 bytes
        auto challenge = recvData(16);
        if(Application::isDebugLevel(DebugLevel::SyslogDebug))
        {
            auto tmp = Tools::buffer2hexstring<uint8_t>(challenge.data(), challenge.size(), 2);
            Application::debug("%s: challenge: %s", __FUNCTION__, tmp.c_str());
        }
        auto crypt = TLS::encryptDES(challenge, password);
        if(Application::isDebugLevel(DebugLevel::SyslogDebug))
        {
            auto tmp = Tools::buffer2hexstring<uint8_t>(crypt.data(), crypt.size(), 2);
            Application::debug("%s: encrypt: %s", __FUNCTION__, tmp.c_str());
        }
        sendRaw(crypt.data(), crypt.size());
        sendFlush();

        return true;
    }

    bool RFB::ClientDecoder::authVenCryptInit(const SecurityInfo & sec)
    {
        // server VenCrypt version
        int majorVer = recvInt8();
        int minorVer = recvInt8();

        Application::debug("%s: server vencrypt version %d.%d", __FUNCTION__, majorVer, minorVer);

        // client VenCrypt version 0.2
        sendInt8(0).sendInt8(2).sendFlush();

        // recv support flag
        if(int unsupportedVersion = recvInt8())
        {
            Application::error("%s: server unsupported vencrypt version", __FUNCTION__);
            return false;
        }

        // rect vencrypt types
        std::vector<int> venCryptTypes;
        int typesCount = recvInt8();

        if(0 >= typesCount)
        {
            Application::error("%s: server vencrypt sub-types failure: %d", __FUNCTION__, typesCount);
            return false;
        }

        while(typesCount--)
            venCryptTypes.push_back(recvIntBE32());

        int mode = RFB::SECURITY_VENCRYPT02_TLSNONE;
        if(sec.tlsAnonMode)
        {
            if(std::none_of(venCryptTypes.begin(), venCryptTypes.end(), [=](auto & val){ return val == RFB::SECURITY_VENCRYPT02_TLSNONE; }))
            {
                Application::error("%s: server unsupported tls: %s mode", __FUNCTION__, "anon");
                return false;
            }
        }
        else
        {
            if(std::none_of(venCryptTypes.begin(), venCryptTypes.end(), [=](auto & val){ return val == RFB::SECURITY_VENCRYPT02_X509NONE; }))
            {
                Application::error("%s: server unsupported tls: %s mode", __FUNCTION__, "x509");
                return false;
            }
            mode = RFB::SECURITY_VENCRYPT02_X509NONE;
        }

        Application::debug("%s: send vencrypt mode: %d", __FUNCTION__, mode);
        sendIntBE32(mode).sendFlush();

        int status = recvInt8();
        if(0 == status)
        {
            Application::error("%s: server invalid status", __FUNCTION__);
            return false;
        }

        try
        {
            if(mode == RFB::SECURITY_VENCRYPT02_X509NONE)
                tls = std::make_unique<TLS::X509Session>(socket.get(), sec.caFile, sec.certFile, sec.keyFile,
                            sec.crlFile, sec.tlsPriority, false, sec.tlsDebug);
            else
                tls = std::make_unique<TLS::AnonSession>(socket.get(), sec.tlsPriority, false, sec.tlsDebug);
        }
        catch(gnutls::exception & err)
        {
            Application::error("gnutls error: %s, code: %d", err.what(), err.get_code());
            return false;
        }

        streamIn = streamOut = tls.get();

        return true;
    }

    bool RFB::ClientDecoder::rfbHandshake(const SecurityInfo & sec)
    {
        // https://vncdotool.readthedocs.io/en/0.8.0/rfbproto.html
        // RFB 1.7.1.1 version
        auto version = Tools::StringFormat("RFB 00%1.00%2\n").arg(RFB::VERSION_MAJOR).arg(RFB::VERSION_MINOR);
        std::string magick = recvString(12);

        if(magick.empty())
        {
            Application::error("%s: handshake failure", __FUNCTION__);
            return false;
        }

        Application::debug("%s: handshake version: %s", __FUNCTION__, magick.substr(0, magick.size() - 1).c_str());

        if(magick != version)
        {
            Application::error("%s: handshake failure", __FUNCTION__);
            return false;
        }

        // 12 bytes
        sendString(version).sendFlush();

        // RFB 1.7.1.2 security
        int counts = recvInt8();
        Application::debug("%s: security counts: %d", __FUNCTION__, counts);

        if(0 == counts)
        {
            int len = recvIntBE32();
            auto err = recvString(len);
            Application::error("%s: receive error: %s", __FUNCTION__, err.c_str());
            return false;
        }

        std::vector<int> security;
        while(0 < counts--)
            security.push_back(recvInt8());

        if(sec.authVenCrypt && std::any_of(security.begin(), security.end(), [=](auto & val){ return val == RFB::SECURITY_TYPE_VENCRYPT; }))
        {
            Application::debug("%s: security: %s selected", __FUNCTION__, "vencrypt");
            sendInt8(RFB::SECURITY_TYPE_VENCRYPT).sendFlush();
            authVenCryptInit(sec);
        }
        else
        if(sec.authNone && std::any_of(security.begin(), security.end(), [=](auto & val){ return val == RFB::SECURITY_TYPE_NONE; }))
        {
            Application::debug("%s: security: %s selected", __FUNCTION__, "noauth");
            sendInt8(RFB::SECURITY_TYPE_NONE).sendFlush();
        }
        else
        if(sec.authVnc && std::any_of(security.begin(), security.end(), [=](auto & val){ return val == RFB::SECURITY_TYPE_VNC; }))
        {
            auto & password = sec.passwdFile;
            if(password.empty())
            {
                Application::error("%s: security vnc: password empty", __FUNCTION__);
                return false;
            }

            Application::debug("%s: security: %s selected", __FUNCTION__, "vncauth");
            sendInt8(RFB::SECURITY_TYPE_VNC).sendFlush();
            authVncInit(password);
        }
        else
        {
            Application::error("%s: security vnc: not supported", __FUNCTION__);
            return false;
        }

        // RFB 1.7.1.3 security result
        if(RFB::SECURITY_RESULT_OK != recvIntBE32())
        {
            int len = recvIntBE32();
            auto err = recvString(len);
            Application::error("%s: receive error: %s", __FUNCTION__, err.c_str());
            return false;
        }

        bool shared = false;
        Application::debug("%s: send share flags: %d", __FUNCTION__,  (int) shared);
        // RFB 6.3.1 client init (shared flag)
        sendInt8(shared ? 1 : 0).sendFlush();
        
        // RFB 6.3.2 server init
        auto fbWidth = recvIntBE16();
        auto fbHeight = recvIntBE16();
        Application::debug("%s: remote framebuffer size: %dx%d", __FUNCTION__, fbWidth, fbHeight);

        // recv server pixel format
        serverFormat.bitsPerPixel = recvInt8();
        int depth = recvInt8();
        serverBigEndian = recvInt8();
        serverTrueColor = recvInt8();
        serverFormat.redMax = recvIntBE16();
        serverFormat.greenMax = recvIntBE16();
        serverFormat.blueMax = recvIntBE16();
        serverFormat.redShift = recvInt8();
        serverFormat.greenShift = recvInt8();
        serverFormat.blueShift = recvInt8();
        recvSkip(3);

        Application::debug("%s: remote pixel format: bpp: %d, depth: %d, bigendian: %d, true color: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                    __FUNCTION__, serverFormat.bitsPerPixel, depth, serverBigEndian, serverTrueColor,
                    serverFormat.redMax, serverFormat.redShift, serverFormat.greenMax, serverFormat.greenShift, serverFormat.blueMax, serverFormat.blueShift);

        // check server format
        switch(serverFormat.bitsPerPixel)
        {
            case 32: case 16: case 8:
                break;

            default:
                Application::error("%s: unknown pixel format, bpp: %d", __FUNCTION__, serverFormat.bitsPerPixel);
                return false;
        }

        if(! serverTrueColor || serverFormat.redMax == 0 || serverFormat.greenMax == 0 || serverFormat.blueMax == 0)
        {
            Application::error("%s: unsupported pixel format", __FUNCTION__);
            return false;
        }

        pixelFormatEvent(serverFormat, fbWidth, fbHeight);

        // recv name desktop
        auto nameLen = recvIntBE32();
        auto nameDesktop = recvString(nameLen);

        Application::debug("%s: server desktop name: %s", __FUNCTION__, nameDesktop.c_str());

        return true;
    }

    bool RFB::ClientDecoder::isContinueUpdatesSupport(void) const
    {
        return continueUpdatesSupport;
    }

    bool RFB::ClientDecoder::isContinueUpdatesProcessed(void) const
    {
        return continueUpdatesProcessed;
    }

    bool RFB::ClientDecoder::rfbMessagesRunning(void) const
    {
        return rfbMessages;
    }

    const PixelFormat & RFB::ClientDecoder::serverPixelFormat(void) const
    {
        return serverFormat;
    }

    void RFB::ClientDecoder::rfbMessagesShutdown(void)
    {
        channelsShutdown();
        std::this_thread::sleep_for(100ms);
        rfbMessages = false;
    }

    void RFB::ClientDecoder::rfbMessagesLoop(void)
    {
        std::initializer_list<int> encodings = { ENCODING_LAST_RECT, ENCODING_RICH_CURSOR,
                                        ENCODING_LTSM,
                                        ENCODING_EXT_DESKTOP_SIZE,
                                        ENCODING_CONTINUOUS_UPDATES,
                                        ENCODING_ZRLE, ENCODING_TRLE, ENCODING_HEXTILE,
                                        ENCODING_ZLIB, ENCODING_CORRE, ENCODING_RRE, ENCODING_RAW };

        sendEncodings(encodings);
        sendPixelFormat();
        // request full update
        sendFrameBufferUpdate(false);

        Application::debug("%s: wait remote messages...", __FUNCTION__);

        auto cur = std::chrono::steady_clock::now();

        while(rfbMessages)
        {
            auto now = std::chrono::steady_clock::now();

            if((! continueUpdatesSupport || ! continueUpdatesProcessed) &&
                std::chrono::milliseconds(300) <= now - cur)
            {
                // request incr update
                sendFrameBufferUpdate(true);
                cur = now;
            }

            if(! hasInput())
            {
                std::this_thread::sleep_for(5ms);
                continue;
            }

            int msgType = recvInt8();

            if(ltsmSupport && msgType == PROTOCOL_LTSM)
            {
                try
                {
                    recvLtsm(*this);
                }
                catch(const std::runtime_error & err)
                {
                    Application::error("%s: exception: %s", __FUNCTION__, err.what());
                    rfbMessagesShutdown();
                }
                catch(const std::exception & err)
                {
                    Application::error("%s: exception: %s", __FUNCTION__, err.what());
                }
                continue;
            }

            if(! rfbMessages)
                break;

            switch(msgType)
            {
                case SERVER_FB_UPDATE:          recvFBUpdateEvent(); break;
                case SERVER_SET_COLOURMAP:      recvColorMapEvent(); break;
                case SERVER_BELL:               recvBellEvent(); break;
                case SERVER_CUT_TEXT:           recvCutTextEvent(); break;
                case SERVER_CONTINUOUS_UPDATES: recvContinuousUpdatesEvent(); break;
                default:
                {
                    Application::error("%s: unknown message: 0x%02x", __FUNCTION__, msgType);
                    rfbMessagesShutdown();
                }
            }
        }
    }

    void RFB::ClientDecoder::sendPixelFormat(void)
    {
        auto & clientFormat = clientPixelFormat();

        Application::debug("%s: local pixel format: bpp: %d, bigendian: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                    __FUNCTION__, clientFormat.bitsPerPixel, big_endian,
                    clientFormat.redMax, clientFormat.redShift, clientFormat.greenMax, clientFormat.greenShift, clientFormat.blueMax, clientFormat.blueShift);

        std::scoped_lock<std::mutex> guard(sendLock);

        // send pixel format
        sendInt8(RFB::CLIENT_SET_PIXEL_FORMAT);
        sendZero(3); // padding
        sendInt8(clientFormat.bitsPerPixel);
        sendInt8(24); // depth
        sendInt8(big_endian);
        sendInt8(1); // trueColor
        sendIntBE16(clientFormat.redMax);
        sendIntBE16(clientFormat.greenMax);
        sendIntBE16(clientFormat.blueMax);
        sendInt8(clientFormat.redShift);
        sendInt8(clientFormat.greenShift);
        sendInt8(clientFormat.blueShift);
        sendZero(3); // padding
        sendFlush();
    }

    void RFB::ClientDecoder::sendEncodings(std::initializer_list<int> encodings)
    {
        for(auto type : encodings)
            Application::debug("%s: %s", __FUNCTION__, encodingName(type));

        std::scoped_lock<std::mutex> guard(sendLock);

        sendInt8(RFB::CLIENT_SET_ENCODINGS);
        sendZero(1); // padding
        sendIntBE16(encodings.size());
        for(auto val : encodings)
            sendIntBE32(val);
        sendFlush();
    }

    void RFB::ClientDecoder::sendKeyEvent(bool pressed, uint32_t keysym)
    {
        Application::debug("%s: keysym: 0x%08x, pressed: %d", __FUNCTION__, keysym, pressed);

        std::scoped_lock<std::mutex> guard(sendLock);

        sendInt8(RFB::CLIENT_EVENT_KEY);
        sendInt8(pressed ? 1 : 0);
        // padding
        sendZero(2);
        sendIntBE32(keysym);
        sendFlush();
    }

    void RFB::ClientDecoder::sendPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy)
    {
        Application::debug("%s: pointer: [%d, %d], buttons: 0x%02x", __FUNCTION__, posx, posy, buttons);

        std::scoped_lock<std::mutex> guard(sendLock);

        sendInt8(RFB::CLIENT_EVENT_POINTER);

        sendInt8(buttons);
        sendIntBE16(posx);
        sendIntBE16(posy);
        sendFlush();
    }

    void RFB::ClientDecoder::sendCutTextEvent(const char* buf, size_t len)
    {
        Application::debug("%s: buffer size: %d", __FUNCTION__, len);

        std::scoped_lock<std::mutex> guard(sendLock);

        sendInt8(RFB::CLIENT_CUT_TEXT);
        // padding
        sendZero(3);
        sendIntBE32(len);
        sendRaw(buf, len);
        sendFlush();
    }

    void RFB::ClientDecoder::sendContinuousUpdates(bool enable, const XCB::Region & reg)
    {
        Application::debug("%s: status: %s, region [%d,%d,%d,%d]", __FUNCTION__, (enable ? "enable" : "disable"), reg.x, reg.y, reg.width, reg.height);

        std::scoped_lock<std::mutex> guard(sendLock);
        sendInt8(CLIENT_CONTINUOUS_UPDATES);
        sendInt8(enable ? 1 : 0);
        sendIntBE16(reg.x);
        sendIntBE16(reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        sendFlush();

        continueUpdatesProcessed = enable;
    }

    void RFB::ClientDecoder::sendFrameBufferUpdate(bool incr)
    {
        auto csz = clientSize();
        sendFrameBufferUpdate(XCB::Region(0, 0, csz.width, csz.height), incr);
    }

    void RFB::ClientDecoder::sendFrameBufferUpdate(const XCB::Region & reg, bool incr)
    {
        Application::debug("%s: region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        std::scoped_lock<std::mutex> guard(sendLock);

        // send framebuffer update request
        sendInt8(CLIENT_REQUEST_FB_UPDATE);
        sendInt8(incr ? 1 : 0);
        sendIntBE16(reg.x);
        sendIntBE16(reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        sendFlush();
    }

    void RFB::ClientDecoder::recvFBUpdateEvent(void)
    {
        auto start = std::chrono::steady_clock::now();

        // padding
        recvSkip(1);
        auto numRects = recvIntBE16();
        XCB::Region reg;

        Application::debug("%s: num rects: %d", __FUNCTION__, numRects);

        while(0 < numRects--)
        {
            reg.x = recvIntBE16();
            reg.y = recvIntBE16();
            reg.width = recvIntBE16();
            reg.height = recvIntBE16();
            int encodingType = recvIntBE32();

            Application::debug("%s: region [%d,%d,%d,%d], encodingType: %s",
                     __FUNCTION__, reg.x, reg.y, reg.width, reg.height, RFB::encodingName(encodingType));

            switch(encodingType)
            {
                case ENCODING_RAW:      recvDecodingRaw(reg); break;
                case ENCODING_RRE:      recvDecodingRRE(reg, false); break;
                case ENCODING_CORRE:    recvDecodingRRE(reg, true); break;
                case ENCODING_HEXTILE:  recvDecodingHexTile(reg); break;
                case ENCODING_TRLE:     recvDecodingTRLE(reg, false); break;
                case ENCODING_ZLIB:     recvDecodingZlib(reg); break;
                case ENCODING_ZRLE:     recvDecodingTRLE(reg, true); break;
                case ENCODING_LTSM:     recvDecodingLtsm(); break;

                case ENCODING_LAST_RECT:
                    recvDecodingLastRect(reg);
                    numRects = 0;
                    break;

                case ENCODING_RICH_CURSOR:
                    recvDecodingRichCursor(reg);
                    break;

                case ENCODING_EXT_DESKTOP_SIZE:
                    recvDecodingExtDesktopSize(reg.x, reg.y, reg.toSize());
                    break;

                case RFB::ENCODING_DESKTOP_SIZE:
                    break;

                default:
                {
                    Application::error("%s: %s", __FUNCTION__, "unknown decoding");
                    throw rfb_error(NS_FuncName);
                }
            }
        }

        auto dt = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
        Application::debug("%s: update time: %dus", __FUNCTION__, dt.count());

        fbUpdateEvent();
    }

    void RFB::ClientDecoder::recvColorMapEvent(void)
    {
        // padding
        recvSkip(1);
        auto firstColor = recvIntBE16();
        auto numColors = recvIntBE16();
        Application::debug("%s: num colors: %d, first color: %d", __FUNCTION__, numColors, firstColor);

        std::vector<Color> colors(numColors);

        for(auto & col : colors)
        {
            col.r = recvInt8();
            col.g = recvInt8();
            col.b = recvInt8();

            if(Application::isDebugLevel(DebugLevel::SyslogTrace))
                Application::debug("%s: color [0x%02x,0x%02x,0x%02x]", __FUNCTION__, col.r, col.g, col.b);
        }

        setColorMapEvent(colors);
    }

    void RFB::ClientDecoder::recvBellEvent(void)
    {
        Application::debug("%s: message", __FUNCTION__);
        bellEvent();
    }

    void RFB::ClientDecoder::recvCutTextEvent(void)
    {
        // padding
        recvSkip(3);
        auto length = recvIntBE32();

        Application::debug("%s: length: %d", __FUNCTION__, length);

        if(0 < length)
        {
            auto text = recvData(length);
            cutTextEvent(std::move(text));
        }
    }

    void RFB::ClientDecoder::recvContinuousUpdatesEvent(void)
    {
        continueUpdatesSupport = true;

        sendContinuousUpdates(false, { XCB::Point(0,0), clientSize() });
    }

    void RFB::ClientDecoder::recvDecodingRaw(const XCB::Region & reg)
    {
        if(decodingDebug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        for(int yy = 0; yy < reg.height; ++yy)
            for(int xx = 0; xx < reg.width; ++yy)
                setPixel(XCB::Point(reg.x + xx, reg.y + yy), recvPixel());
    }

    void RFB::ClientDecoder::recvDecodingLastRect(const XCB::Region & reg)
    {
        if(decodingDebug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);
    }

    void RFB::ClientDecoder::recvDecodingRichCursor(const XCB::Region & reg)
    {
        if(decodingDebug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        auto buf = recvData(reg.width * reg.height * serverFormat.bytePerPixel());
        auto mask = recvData(std::floor((reg.width + 7) / 8) * reg.height);

        richCursorEvent(reg, std::move(buf), std::move(mask));
    }

    void RFB::ClientDecoder::recvDecodingRRE(const XCB::Region & reg, bool corre)
    {
        if(decodingDebug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        auto subRects = recvIntBE32();
        auto bgColor = recvPixel();

        if(1 < decodingDebug)
            Application::debug("%s: back pixel: 0x%08x, sub rects: %d", __FUNCTION__, bgColor, subRects);

        fillPixel(reg, bgColor);

        while(0 < subRects--)
        {
            XCB::Region dst;
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

            if(2 < decodingDebug)
                Application::debug("%s: sub region [%d,%d,%d,%d]", __FUNCTION__, dst.x, dst.y, dst.width, dst.height);

            dst.x += reg.x;
            dst.y += reg.y;

            if(dst.x + dst.width > reg.x + reg.width || dst.y + dst.height > reg.y + reg.height)
            {
                Application::error("%s: %s", __FUNCTION__, "sub region out of range");
                throw rfb_error(NS_FuncName);
            }

            fillPixel(dst, pixel);
        }
    }

    void RFB::ClientDecoder::recvDecodingHexTile(const XCB::Region & reg)
    {
        if(decodingDebug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        int bgColor = -1;
        int fgColor = -1;
        const XCB::Size bsz(16, 16);

        for(auto & reg0: reg.divideBlocks(bsz))
            recvDecodingHexTileRegion(reg0, bgColor, fgColor);
    }

    void RFB::ClientDecoder::recvDecodingHexTileRegion(const XCB::Region & reg, int & bgColor, int & fgColor)
    {
        auto flag = recvInt8();

        if(1 < decodingDebug)
            Application::debug("%s: sub encoding mask: 0x%02x, sub region [%d,%d,%d,%d]", __FUNCTION__, flag, reg.x, reg.y, reg.width, reg.height);

        if(flag & RFB::HEXTILE_RAW)
        {
            if(2 < decodingDebug)
                Application::debug("%s: type: %s", __FUNCTION__, "raw");

            for(int yy = 0; yy < reg.height; ++yy)
                for(int xx = 0; xx < reg.width; ++yy)
                    setPixel(XCB::Point(reg.x + xx, reg.y + yy), recvPixel());
        }
        else
        {
            if(flag & RFB::HEXTILE_BACKGROUND)
            {
                bgColor = recvPixel();

                if(2 < decodingDebug)
                    Application::debug("%s: type: %s, pixel: 0x%08x", __FUNCTION__, "background", bgColor);
            }

            fillPixel(reg, bgColor);

            if(flag & HEXTILE_FOREGROUND)
            {
                fgColor = recvPixel();
                flag &= ~HEXTILE_COLOURED;

                if(2 < decodingDebug)
                    Application::debug("%s: type: %s, pixel: 0x%08x", __FUNCTION__, "foreground", fgColor);
            }

            if(flag & HEXTILE_SUBRECTS)
            {
                int subRects = recvInt8();
                XCB::Region dst;

                if(2 < decodingDebug)
                    Application::debug("%s: type: %s, count: %d", __FUNCTION__, "subrects", subRects);

                while(0 < subRects--)
                {
                    auto pixel = fgColor;
                    if(flag & HEXTILE_COLOURED)
                    {
                        pixel = recvPixel();
                        if(3 < decodingDebug)
                            Application::debug("%s: type: %s, pixel: 0x%08x", __FUNCTION__, "colored", pixel);
                    }

                    auto val1 = recvInt8();
                    auto val2 = recvInt8();

                    dst.x = (0x0F & (val1 >> 4));
                    dst.y = (0x0F & val1);
                    dst.width = 1 + (0x0F & (val2 >> 4));
                    dst.height = 1 + (0x0F & val2);

                    if(3 < decodingDebug)
                        Application::debug("%s: type: %s, region: [%d,%d,%d,%d], pixel: 0x%08x", __FUNCTION__, "subrects", dst.x, dst.y, dst.width, dst.height, pixel);

                    dst.x += reg.x;
                    dst.y += reg.y;

                    if(dst.x + dst.width > reg.x + reg.width || dst.y + dst.height > reg.y + reg.height)
                    {
                        Application::error("%s: %s", __FUNCTION__, "sub region out of range");
                        throw rfb_error(NS_FuncName);
                    }

                    fillPixel(dst, pixel);
                }
            }
        }
    }

    void RFB::ClientDecoder::recvDecodingZlib(const XCB::Region & reg)
    {
        if(decodingDebug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        zlibInflateStart();
        recvDecodingRaw(reg);
        zlibInflateStop();
    }

    void RFB::ClientDecoder::recvDecodingTRLE(const XCB::Region & reg, bool zrle)
    {
        if(decodingDebug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        const XCB::Size bsz(64, 64);

        if(zrle)
            zlibInflateStart();

        for(auto & reg0: reg.XCB::Region::divideBlocks(bsz))
            recvDecodingTRLERegion(reg0, zrle);

        if(zrle)
            zlibInflateStop();
    }

    void RFB::ClientDecoder::recvDecodingTRLERegion(const XCB::Region & reg, bool zrle)
    {
        auto type = recvInt8();

        if(1 < decodingDebug)
            Application::debug("%s: sub encoding type: 0x%02x, sub region: [%d,%d,%d,%d], zrle: %d",
                        __FUNCTION__, type, reg.x, reg.y, reg.width, reg.height, (int) zrle);

        // trle raw
        if(0 == type)
        {
            if(2 < decodingDebug)
                Application::debug("%s: type: %s", __FUNCTION__, "raw");

            for(auto coord = XCB::PointIterator(0, 0, reg.toSize()); coord.isValid(); ++coord)
            {
                auto pixel = recvCPixel();
                setPixel(reg.topLeft() + coord, pixel);
            }

            if(3 < decodingDebug)
                Application::debug("%s: complete: %s", __FUNCTION__, "raw");
        }
        else
        // trle solid
        if(1 == type)
        {
            auto solid = recvCPixel();

            if(2 < decodingDebug)
                Application::debug("%s: type: %s, pixel: 0x%08x", __FUNCTION__, "solid", solid);

            fillPixel(reg, solid);

            if(3 < decodingDebug)
                Application::debug("%s: complete: %s", __FUNCTION__, "solid");
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

            if(2 < decodingDebug)
                Application::debug("%s: type: %s, size: %d", __FUNCTION__, "packed palette", palette.size());

            if(3 < decodingDebug)
            {
                std::string str = Tools::buffer2hexstring<int>(palette.data(), palette.size(), 8);
                Application::debug("%s: type: %s, palette: %s", __FUNCTION__, "packed palette", str.c_str());
            }

            // recv packed rows
            for(int oy = 0; oy < reg.height; ++oy)
            {
                Tools::StreamBitsUnpack sb(recvData(rowsz), reg.width, field);

                for(int ox = reg.width - 1; 0 <= ox; --ox)
                {
                    auto pos = reg.topLeft() + XCB::Point(ox, oy);
                    auto index = sb.popValue(field);

                    if(4 < decodingDebug)
                        Application::debug("%s: type: %s, pos: [%d,%d], index: %d", __FUNCTION__, "packed palette", pos.x, pos.y, index);

                    if(index >= palette.size())
                    {
                        Application::error("%s: %s", __FUNCTION__, "index out of range");
                        throw rfb_error(NS_FuncName);
                    }

                    setPixel(pos, palette[index]);
                }
            }

            if(3 < decodingDebug)
                Application::debug("%s: complete: %s", __FUNCTION__, "packed palette");
        }
        else
        if((17 <= type && type <= 127) || type == 129)
        {
            Application::error("%s: %s", __FUNCTION__, "invalid trle type");
            throw rfb_error(NS_FuncName);
        }
        else
        if(128 == type)
        {
            if(2 < decodingDebug)
                Application::debug("%s: type: %s", __FUNCTION__, "plain rle");

            auto coord = XCB::PointIterator(0, 0, reg.toSize());

            while(coord.isValid())
            {
                auto pixel = recvCPixel();
                auto runLength = recvRunLength();

                if(4 < decodingDebug)
                    Application::debug("%s: type: %s, pixel: 0x%08x, length: %d", __FUNCTION__, "plain rle", pixel, runLength);

                while(runLength--)
                {
                    setPixel(reg.topLeft() + coord, pixel);
                    ++coord;

                    if(! coord.isValid() && runLength)
                    {
                        Application::error("%s: %s", __FUNCTION__, "plain rle: coord out of range");
                        throw rfb_error(NS_FuncName);
                    }
                }
            }

            if(3 < decodingDebug)
                Application::debug("%s: complete: %s", __FUNCTION__, "plain rle");
        }
        else
        if(130 <= type)
        {
            size_t palsz = type - 128;
            std::vector<int> palette(palsz);
            
            for(auto & val: palette)
                val = recvCPixel();

            if(2 < decodingDebug)
                Application::debug("%s: type: %s, size: %d", __FUNCTION__, "rle palette", palsz);

            if(3 < decodingDebug)
            {
                std::string str = Tools::buffer2hexstring<int>(palette.data(), palette.size(), 8);
                Application::debug("%s: type: %s, palette: %s", __FUNCTION__, "rle palette", str.c_str());
            }

            auto coord = XCB::PointIterator(0, 0, reg.toSize());

            while(coord.isValid())
            {
                auto index = recvInt8();

                if(index < 128)
                {
                    if(index >= palette.size())
                    {
                        Application::error("%s: %s", __FUNCTION__, "index out of range");
                        throw rfb_error(NS_FuncName);
                    }

                    auto pixel = palette[index];
                    setPixel(reg.topLeft() + coord, pixel);

                    ++coord;
                }
                else
                {
                    index -= 128;

                    if(index >= palette.size())
                    {
                        Application::error("%s: %s", __FUNCTION__, "index out of range");
                        throw rfb_error(NS_FuncName);
                    }

                    auto pixel = palette[index];
                    auto runLength = recvRunLength();

                    if(4 < decodingDebug)
                        Application::debug("%s: type: %s, index: %d, length: %d", __FUNCTION__, "rle palette", index, runLength);

                    while(runLength--)
                    {
                        setPixel(reg.topLeft() + coord, pixel);
                        ++coord;

                        if(! coord.isValid() && runLength)
                        {
                            Application::error("%s: %s", __FUNCTION__, "rle palette: coord out of range");
                            throw rfb_error(NS_FuncName);
                        }
                    }
                }
            }

            if(3 < decodingDebug)
                Application::debug("%s: complete: %s", __FUNCTION__, "rle palette");
        }
    }

    void RFB::ClientDecoder::recvDecodingExtDesktopSize(uint16_t status, uint16_t err, const XCB::Size & sz)
    {
        Application::info("%s: status: 0x%02x, error: 0x%02x, width: %d, height: %d", __FUNCTION__, status, err, sz.width, sz.height);

        auto numOfScreens = recvInt8();
        recvSkip(3);

        std::vector<RFB::ScreenInfo> screens(numOfScreens);
        for(auto & screen : screens)
        {
            screen.id = recvIntBE32();
            auto posx = recvIntBE16();
            auto posy = recvIntBE16();
            screen.width = recvIntBE16();
            screen.height = recvIntBE16();
            auto flags = recvIntBE32();
            Application::debug("%s: screen: %d, area: [%d, %d, %d, %d], flags: %d", __FUNCTION__, screen.id, posx, posy, screen.width, screen.height, flags);
        }

        decodingExtDesktopSizeEvent(status, err, sz, screens); 
    }

    void RFB::ClientDecoder::sendSetDesktopSize(uint16_t width, uint16_t height)
    {
        Application::info("%s: width: %d, height: %d", __FUNCTION__, width, height);

        std::scoped_lock<std::mutex> guard(sendLock);
        
        sendInt8(RFB::CLIENT_SET_DESKTOP_SIZE);
        sendZero(1);
        sendIntBE16(width);
        sendIntBE16(height);

        // num of screens
        sendInt8(1);
        sendZero(1);

        // screen id
        sendIntBE32(0);
        // posx, posy
        sendIntBE32(0);
        sendIntBE16(width);
        sendIntBE16(height);
        // flag
        sendIntBE32(0);

        sendFlush();
    }

    int RFB::ClientDecoder::recvPixel(void)
    {
        auto & pf = clientPixelFormat();

        switch(pf.bytePerPixel())
        {
            case 4: return serverBigEndian ? recvIntBE32() : recvIntLE32();
            case 2: return serverBigEndian ? recvIntBE16() : recvIntLE16();
            case 1: return recvInt8();
            default: break;
        }

        Application::error("%s: %s", __FUNCTION__, "unknown format");
        throw rfb_error(NS_FuncName);
    }

    int RFB::ClientDecoder::recvCPixel(void)
    {
        auto & pf = clientPixelFormat();

        if(serverTrueColor && pf.bitsPerPixel == 32)
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

    size_t RFB::ClientDecoder::recvRunLength(void)
    {
        size_t length = 0;

        while(true)
        {
            auto val = recvInt8();
            length += val;

            if(val != 255)
            {
                length += 1;
                break;
            }
        }

        return length;
    }

    void RFB::ClientDecoder::zlibInflateStart(bool uint16sz)
    {
        if(! zlib)
            zlib.reset(new ZLib::InflateStream());

        size_t zipsz = 0;

        if(uint16sz)
            zipsz = recvIntBE16();
        else
            zipsz = recvIntBE32();

        auto zip = recvData(zipsz);

        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
            Application::debug("%s: compress data length: %d", __FUNCTION__, zip.size());

        zlib->appendData(zip);
        streamIn = zlib.get();
    }

    void RFB::ClientDecoder::zlibInflateStop(void)
    {
        streamIn = tls ? tls.get() : socket.get();
    }

    void RFB::ClientDecoder::recvDecodingLtsm(void)
    {
        Application::info("%s: success", __FUNCTION__);
        ltsmSupport = true;

        size_t len = recvIntBE32();
        auto data = recvData(len);

        decodingLtsmEvent(data);
    }

    void RFB::ClientDecoder::sendLtsmEvent(uint8_t channel, const uint8_t* buf, size_t len)
    {
        sendLtsm(*this, sendLock, channel, buf, len);
    }

    void RFB::ClientDecoder::recvChannelSystem(const std::vector<uint8_t> & buf)
    {
        JsonContent jc;
        jc.parseBinary(reinterpret_cast<const char*>(buf.data()), buf.size());

        if(! jc.isObject())
        {
            Application::error("%s: %s", __FUNCTION__, "json broken");
            throw std::invalid_argument(NS_FuncName);
        }

        auto jo = jc.toObject();
        auto cmd = jo.getString("cmd");

        if(cmd.empty())
        {
            Application::error("%s: %s", __FUNCTION__, "format message broken");
            throw std::invalid_argument(NS_FuncName);
        }

        if(cmd == SystemCommand::ChannelOpen)
            systemChannelOpen(jo);
        else
        if(cmd == SystemCommand::ChannelListen)
            systemChannelListen(jo);
        else
        if(cmd == SystemCommand::ChannelClose)
            systemChannelClose(jo);
        else
        if(cmd == SystemCommand::ChannelConnected)
            systemChannelConnected(jo);
        else
        if(cmd == SystemCommand::FuseProxy)
            systemFuseProxy(jo);
        else
        if(cmd == SystemCommand::ChannelError)
            systemChannelError(jo);
        else
        if(cmd == SystemCommand::TokenAuth)
            systemTokenAuth(jo);
        else
        if(cmd == SystemCommand::LoginSuccess)
            systemLoginSuccess(jo);
        else
        {
            Application::error("%s: %s", __FUNCTION__, "unknown cmd");
            throw std::invalid_argument(NS_FuncName);
        }
    }
}
