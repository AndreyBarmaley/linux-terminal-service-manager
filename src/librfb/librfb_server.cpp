/***********************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#include <cmath>
#include <string>
#include <chrono>
#include <thread>
#include <future>
#include <numeric>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <cstring>
#include <fstream>
#include <algorithm>

#include "ltsm_librfb.h"
#include "ltsm_application.h"
#include "librfb_server.h"

using namespace std::chrono_literals;

namespace LTSM
{
    RFB::ServerEncoder::ServerEncoder(int sockfd)
    {
        if(0 < sockfd)
            socket.reset(new SocketStream(sockfd));
        else
            socket.reset(new InetStream());

        streamIn = streamOut = socket.get();
    }

    void RFB::ServerEncoder::sendFlush(void)
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

    void RFB::ServerEncoder::sendRaw(const void* ptr, size_t len)
    {   
        try
        {
            if(rfbMessages)
            {
                streamOut->sendRaw(ptr, len);
                netStatTx += len;
            }
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());

            streamOut->setError();
            rfbMessagesShutdown();
        }
    }

    void RFB::ServerEncoder::recvRaw(void* ptr, size_t len) const
    {   
        try
        {
            if(rfbMessages)
            {
                streamIn->recvRaw(ptr, len);
                netStatRx += len;
            }
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());

            const_cast<NetworkStream*>(streamIn)->setError();
            const_cast<ServerEncoder*>(this)->rfbMessagesShutdown();
        }
    }

    bool RFB::ServerEncoder::hasInput(void) const
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
            const_cast<ServerEncoder*>(this)->rfbMessagesShutdown();
        }

        return false;
    }

    size_t RFB::ServerEncoder::hasData(void) const
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
            const_cast<ServerEncoder*>(this)->rfbMessagesShutdown();
        }

        return 0;
    }

    uint8_t RFB::ServerEncoder::peekInt8(void) const
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
            const_cast<ServerEncoder*>(this)->rfbMessagesShutdown();
        }

        return 0;
    }

    bool RFB::ServerEncoder::isUpdateProcessed(void) const
    {
        return fbUpdateProcessing || ! encodingJobs.empty();
    }

    void RFB::ServerEncoder::waitUpdateProcess(void)
    {
        while(isUpdateProcessed())
            std::this_thread::sleep_for(1ms);
    }

    bool RFB::ServerEncoder::authVncInit(const std::string & passwdFile)
    {
        std::vector<uint8_t> challenge = TLS::randomKey(16);

        if(Application::isDebugLevel(DebugLevel::SyslogDebug))
        {
            auto tmp = Tools::buffer2hexstring<uint8_t>(challenge.data(), challenge.size(), 2);
            Application::debug("%s: challenge: %s", __FUNCTION__, tmp.c_str());
        }

        sendRaw(challenge.data(), challenge.size());
        sendFlush();
        auto response = recvData(16);

        if(Application::isDebugLevel(DebugLevel::SyslogDebug))
        {
            auto tmp = Tools::buffer2hexstring<uint8_t>(response.data(), response.size(), 2);
            Application::debug("%s: response: %s", __FUNCTION__, tmp.c_str());
        }

        std::ifstream ifs(passwdFile, std::ifstream::in);

        while(ifs.good())
        {
            std::string pass;
            std::getline(ifs, pass);
            auto crypt = TLS::encryptDES(challenge, pass);

            if(Application::isDebugLevel(DebugLevel::SyslogDebug))
            {
                auto tmp = Tools::buffer2hexstring<uint8_t>(crypt.data(), crypt.size(), 2);
                Application::debug("%s: encrypt: %s", __FUNCTION__, tmp.c_str());
            }

            if(crypt == response)
                return true;
        }

        const std::string err("password mismatch");
        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(err.size()).sendString(err).sendFlush();
        Application::error("%s: %s, passwd file: %s", __FUNCTION__, err.c_str(), passwdFile.c_str());

        return false;
    }

    bool RFB::ServerEncoder::authVenCryptInit(const SecurityInfo & secInfo)
    {
        // VenCrypt version
        sendInt8(0).sendInt8(2).sendFlush();
        // client req
        int majorVer = recvInt8();
        int minorVer = recvInt8();
        Application::debug("%s: client vencrypt version %d.%d", __FUNCTION__, majorVer, minorVer);

        if(majorVer != 0 || (minorVer < 1 || minorVer > 2))
        {
            // send unsupported
            sendInt8(255).sendFlush();
            Application::error("%s: unsupported vencrypt version %d.%d", __FUNCTION__, majorVer, minorVer);
            return false;
        }

        // send supported
        sendInt8(0);
        bool x509Mode = false;

        if(minorVer == 1)
        {
            if(secInfo.tlsAnonMode)
                sendInt8(1).sendInt8(RFB::SECURITY_VENCRYPT01_TLSNONE).sendFlush();
            else
                sendInt8(2).sendInt8(RFB::SECURITY_VENCRYPT01_TLSNONE).sendInt8(RFB::SECURITY_VENCRYPT01_X509NONE).sendFlush();

            int mode = recvInt8();
            Application::debug("%s: client choice vencrypt mode: 0x%02x", __FUNCTION__, mode);

            switch(mode)
            {
                case RFB::SECURITY_VENCRYPT01_TLSNONE:
                    break;

                case RFB::SECURITY_VENCRYPT01_X509NONE:
                    if(secInfo.tlsAnonMode)
                    {
                        Application::error("%s: unsupported vencrypt mode: %s", __FUNCTION__, "x509");
                        return false;
                    }

                    x509Mode = true;
                    break;

                default:
                    Application::error("%s: unsupported vencrypt mode: 0x%02x", __FUNCTION__, mode);
                    return false;
            }
        }
        else
            // if(minorVer == 2)
        {
            if(secInfo.tlsAnonMode)
                sendInt8(1).sendIntBE32(RFB::SECURITY_VENCRYPT02_TLSNONE).sendFlush();
            else
                sendInt8(2).sendIntBE32(RFB::SECURITY_VENCRYPT02_TLSNONE).sendIntBE32(RFB::SECURITY_VENCRYPT02_X509NONE).sendFlush();

            int mode = recvIntBE32();
            Application::debug("%s: client choice vencrypt mode: %d", __FUNCTION__, mode);

            switch(mode)
            {
                case RFB::SECURITY_VENCRYPT02_TLSNONE:
                    break;

                case RFB::SECURITY_VENCRYPT02_X509NONE:
                    if(secInfo.tlsAnonMode)
                    {
                        Application::error("%s: unsupported vencrypt mode: %s", __FUNCTION__, "x509");
                        return false;
                    }

                    x509Mode = true;
                    break;

                default:
                    Application::error("%s: unsupported vencrypt mode: 0x%08x", __FUNCTION__, mode);
                    return false;
            }
        }

        if(x509Mode)
        {
            const std::string* errFile = nullptr;
            std::error_code fserr;

            if(! std::filesystem::exists(secInfo.caFile, fserr))
                errFile = &secInfo.caFile;

            if(! std::filesystem::exists(secInfo.certFile, fserr))
                errFile = &secInfo.certFile;

            if(! std::filesystem::exists(secInfo.keyFile, fserr))
                errFile = &secInfo.keyFile;

            if(errFile)
            {
                Application::error("%s: file not found: %s", __FUNCTION__, errFile->c_str());
                sendInt8(0).sendFlush();
                return false;
            }
        }

        sendInt8(1).sendFlush();

        try
        {
            if(x509Mode)
                tls = std::make_unique<TLS::X509Session>(socket.get(), secInfo.caFile, secInfo.certFile, secInfo.keyFile,
                        secInfo.crlFile, secInfo.tlsPriority, true, secInfo.tlsDebug);
            else
                tls = std::make_unique<TLS::AnonSession>(socket.get(), secInfo.tlsPriority, true, secInfo.tlsDebug);
        }
        catch(gnutls::exception & err)
        {
            Application::error("gnutls error: %s, code: %d", err.what(), err.get_code());
            return false;
        }

        streamIn = streamOut = tls.get();

        return true;
    }

    int RFB::ServerEncoder::serverHandshakeVersion(void)
    {
        // RFB 6.1.1 version
        int protover = 38;
        auto version = Tools::StringFormat("RFB 00%1.00%2\n").arg(RFB::VERSION_MAJOR).arg(RFB::VERSION_MINOR);
        sendString(version).sendFlush();
        std::string magick = recvString(12);
        Application::debug("%s: handshake version %s", __FUNCTION__, magick.c_str());

        if(magick == Tools::StringFormat("RFB 00%1.00%2\n").arg(RFB::VERSION_MAJOR).arg(3))
            protover = 33;
        else
        if(magick == Tools::StringFormat("RFB 00%1.00%2\n").arg(RFB::VERSION_MAJOR).arg(7))
            protover = 37;
        else
        if(magick != version)
        {
            Application::error("%s: handshake failure, unknown magic: %s", __FUNCTION__, magick.c_str());
            return 0;
        }

        return protover;
    }

    bool RFB::ServerEncoder::serverSecurityInit(int protover, const SecurityInfo & secInfo)
    {
        // RFB 6.1.2 security
        if(protover == 33)
        {
            uint32_t res = 0;
            if(secInfo.authVnc)
                res |= RFB::SECURITY_TYPE_VNC;
            if(secInfo.authNone)
                res |= RFB::SECURITY_TYPE_NONE;

            sendIntBE32(res);
        }
        else
        {
            std::vector<uint8_t> res;
            if(secInfo.authVenCrypt)
                res.push_back(RFB::SECURITY_TYPE_VENCRYPT);
            if(secInfo.authVnc)
                res.push_back(RFB::SECURITY_TYPE_VNC);
            if(secInfo.authNone)
                res.push_back(RFB::SECURITY_TYPE_NONE);
            sendInt8(res.size());

            if(res.empty())
            {
                Application::error("%s: server security invalid", __FUNCTION__);
                sendFlush();
                return false;
            }

            sendData(res);
        }
        sendFlush();

        if(protover != 33)
        {
            int clientSecurity = recvInt8();
            Application::debug("%s, client security: 0x%02x", __FUNCTION__, clientSecurity);

            if(protover == 38 || clientSecurity != RFB::SECURITY_TYPE_NONE)
            {
                // RFB 6.1.3 security result
                if(clientSecurity == RFB::SECURITY_TYPE_NONE && secInfo.authNone)
                    sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
                else if(clientSecurity == RFB::SECURITY_TYPE_VNC && secInfo.authVnc)
                {
                    if(secInfo.passwdFile.empty())
                    {
                        Application::error("%s: passwd file not defined", __FUNCTION__);
                        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                        return false;
                    }

                    std::error_code err;
                    if(! std::filesystem::exists(secInfo.passwdFile, err))
                    {
                        Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), secInfo.passwdFile.c_str(), getuid());
                        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                        return false;
                    }

                    if(! authVncInit(secInfo.passwdFile))
                    {
                        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                        return false;
                    }

                    sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
                }
                else if(clientSecurity == RFB::SECURITY_TYPE_VENCRYPT && secInfo.authVenCrypt)
                {
                    if(! authVenCryptInit(secInfo))
                    {
                        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                        return false;
                    }

                    sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
                }
                else
                {
                    const std::string err("no matching security types");
                    sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(err.size()).sendString(err).sendFlush();
                    Application::error("%s: error: %s", __FUNCTION__, err.c_str());
                    return false;
                }
            }
        }

        return true;
    }

    void RFB::ServerEncoder::serverClientInit(std::string_view desktopName, const XCB::Size & displaySize, int displayDepth, const PixelFormat & pf)
    {
        // RFB 6.3.1 client init
        int clientSharedFlag = recvInt8();
        Application::debug("%s: client shared: 0x%02x", __FUNCTION__, clientSharedFlag);
        // RFB 6.3.2 server init
        sendIntBE16(displaySize.width);
        sendIntBE16(displaySize.height);
        Application::info("%s: bpp: %d, depth: %d, bigendian: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                           __FUNCTION__, pf.bitsPerPixel, displayDepth, big_endian, 
                            pf.redMax, pf.redShift, pf.greenMax, pf.greenShift, pf.blueMax, pf.blueShift);
        clientFormat = serverFormat();
        // send pixel format
        sendInt8(pf.bitsPerPixel);
        sendInt8(displayDepth);
        sendInt8(big_endian ? 1 : 0);
        // true color
        sendInt8(1);
        sendIntBE16(pf.redMax);
        sendIntBE16(pf.greenMax);
        sendIntBE16(pf.blueMax);
        sendInt8(pf.redShift);
        sendInt8(pf.greenShift);
        sendInt8(pf.blueShift);
        // send padding
        sendInt8(0);
        sendInt8(0);
        sendInt8(0);
        // send name desktop
        sendIntBE32(desktopName.size()).sendString(desktopName).sendFlush();
    }

    bool RFB::ServerEncoder::sendUpdateSafe(const XCB::Region & area)
    {
        fbUpdateProcessing = true;
        bool res = true;

        try
        {
            auto reply = xcbFrameBuffer(area);
            sendFrameBufferUpdate(reply.fb);
            sendFrameBufferUpdateEvent(area);
        }
        catch(const std::exception & err)
        {
            Application::error("%s: vnc exception: %s", __FUNCTION__, err.what());
            res = false;
        }
        catch(...)
        {
            res = false;
        }

        fbUpdateProcessing = false;
        return res;
    }

    bool RFB::ServerEncoder::rfbMessagesRunning(void) const
    {
        return rfbMessages;
    }

    void RFB::ServerEncoder::rfbMessagesShutdown(void)
    {
#ifdef LTSM_CHANNELS
        channelsShutdown();
#endif
        std::this_thread::sleep_for(100ms);
        rfbMessages = false;
    }

    void RFB::ServerEncoder::rfbMessagesLoop(void)
    {
        Application::debug("%s: wait remote messages...", __FUNCTION__);

        while(rfbMessages)
        {
            if(! hasInput())
            {
                std::this_thread::sleep_for(1ms);
                continue;
            }

            int msgType = recvInt8();

#ifdef LTSM_CHANNELS
            if(msgType == RFB::PROTOCOL_LTSM)
            {
                if(! isClientEncodings(RFB::ENCODING_LTSM))
                {
                    Application::error("%s: client not support encoding: %s", __FUNCTION__, RFB::encodingName(RFB::ENCODING_LTSM));
                    throw rfb_error(NS_FuncName);
                }

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
#endif
            if(! rfbMessages)
                break;

            switch(msgType)
            {
                case RFB::CLIENT_SET_PIXEL_FORMAT:
                    recvPixelFormat();
                    break;

                case RFB::CLIENT_SET_ENCODINGS:
                    recvSetEncodings();
                    break;

                case RFB::CLIENT_REQUEST_FB_UPDATE:
                    recvFramebufferUpdate();
                    break;

                case RFB::CLIENT_EVENT_KEY:
                    recvKeyCode();
                    break;

                case RFB::CLIENT_EVENT_POINTER:
                    recvPointer();
                    break;

                case RFB::CLIENT_CUT_TEXT:
                    recvCutText();
                    break;

                case RFB::CLIENT_SET_DESKTOP_SIZE:
                    recvSetDesktopSize();
                    break;

                case RFB::CLIENT_CONTINUOUS_UPDATES:
                    recvSetContinuousUpdates();
                    break;

                default:
                    Application::error("%s: unknown message: 0x%02x", __FUNCTION__, msgType);
                    rfbMessagesShutdown();
            }
        }
    }

    void RFB::ServerEncoder::recvPixelFormat(void)
    {
        waitUpdateProcess();
        // RFB: 6.4.1
        // skip padding
        recvSkip(3);
        auto bitsPerPixel = recvInt8();
        auto depth = recvInt8();
        auto bigEndian = recvInt8();
        auto trueColor = recvInt8();
        auto redMax = recvIntBE16();
        auto greenMax = recvIntBE16();
        auto blueMax = recvIntBE16();
        auto redShift = recvInt8();
        auto greenShift = recvInt8();
        auto blueShift = recvInt8();
        // skip padding
        recvSkip(3);
        Application::notice("%s: bpp: %d, depth: %d, bigendian: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                            __FUNCTION__, bitsPerPixel, depth, bigEndian, redMax, redShift, greenMax, greenShift, blueMax, blueShift);

        switch(bitsPerPixel)
        {
            case 32:
            case 16:
            case 8:
                break;

            default:
            {
                Application::error("%s: %s", __FUNCTION__, " unknown pixel format");
                throw rfb_error(NS_FuncName);
            }
        }
        
        if(trueColor == 0 || redMax == 0 || greenMax == 0 || blueMax == 0)
        {
            Application::error("%s: %s", __FUNCTION__, " unsupported pixel format");
            throw rfb_error(NS_FuncName);
        }

        clientTrueColor = trueColor;
        clientBigEndian = bigEndian;
        clientFormat = PixelFormat(bitsPerPixel, redMax, greenMax, blueMax, 0, redShift, greenShift, blueShift, 0);

        colourMap.clear();
        recvPixelFormatEvent(clientFormat, clientBigEndian);
    }

    void RFB::ServerEncoder::recvSetEncodings(void)
    {
        waitUpdateProcess();
        // RFB: 6.4.2
        // skip padding
        recvSkip(1);
        int numEncodings = recvIntBE16();

        Application::info("%s: encoding counts: %d", __FUNCTION__, numEncodings);
        clientEncodings.clear();
        clientEncodings.reserve(numEncodings);

        while(0 < numEncodings--)
        {
            int encoding = recvIntBE32();

            if(! disabledEncodings.empty())
            {
                auto enclower = Tools::lower(RFB::encodingName(encoding));

                if(std::any_of(disabledEncodings.begin(), disabledEncodings.end(),
                               [&](auto & str) { return enclower == Tools::lower(str); }))
                {
                    Application::warning("%s: request encodings: %s (disabled)", __FUNCTION__, RFB::encodingName(encoding));
                    continue;
                }
            }

            clientEncodings.push_back(encoding);
            const char* name = RFB::encodingName(encoding);

            if(0 == std::strcmp(name, "unknown"))
                Application::info("%s: request encodings: 0x%08x", __FUNCTION__, encoding);
            else
                Application::info("%s: request encodings: %s", __FUNCTION__, RFB::encodingName(encoding));
        }

        if(! prefferedEncodings.empty())
        {
            std::sort(clientEncodings.begin(), clientEncodings.end(), [this](auto & v1, auto & v2)
            {
                auto s1 = Tools::lower(RFB::encodingName(v1));
                auto s2 = Tools::lower(RFB::encodingName(v2));
                auto p1 = std::find(prefferedEncodings.begin(), prefferedEncodings.end(), s1);
                auto p2 = std::find(prefferedEncodings.begin(), prefferedEncodings.end(), s2);

                if(p1 != prefferedEncodings.end() && p2 != prefferedEncodings.end())
                    return std::distance(prefferedEncodings.begin(), p1) < std::distance(prefferedEncodings.begin(), p2);

                if(p1 != prefferedEncodings.end())
                    return true;

                return false;
            });

            for(auto & enc : clientEncodings)
            {
                const char* name = RFB::encodingName(enc);

                if(0 == std::strcmp(name, "unknown"))
                    Application::debug("%s: server pref encodings: 0x%08x", __FUNCTION__, enc);
                else
                    Application::debug("%s: server pref encodings: %s", __FUNCTION__, RFB::encodingName(enc));
            }
        }

        if(isClientEncodings(RFB::ENCODING_CONTINUOUS_UPDATES))
            sendContinuousUpdates(true);

        recvSetEncodingsEvent(clientEncodings);
    }

    void RFB::ServerEncoder::recvFramebufferUpdate(void)
    {
        XCB::Region clientRegion;

        // RFB: 6.4.3
        int incremental = recvInt8();
        clientRegion.x = recvIntBE16();
        clientRegion.y = recvIntBE16();
        clientRegion.width = recvIntBE16();
        clientRegion.height = recvIntBE16();

        Application::debug("%s: request update, region [%d, %d, %d, %d], incremental: %d",
                           __FUNCTION__, clientRegion.x, clientRegion.y, clientRegion.width, clientRegion.height, incremental);

        bool fullUpdate = incremental == 0;
        recvFramebufferUpdateEvent(fullUpdate, clientRegion);
    }

    void RFB::ServerEncoder::recvKeyCode(void)
    {
        // RFB: 6.4.4
        bool pressed = recvInt8();
        recvSkip(2);
        uint32_t keysym = recvIntBE32();
        Application::debug("%s: action %s, keysym: 0x%08x", __FUNCTION__, (pressed ? "pressed" : "released"), keysym);

        recvKeyEvent(pressed, keysym);
    }

    void RFB::ServerEncoder::recvPointer(void)
    {
        // RFB: 6.4.5
        uint8_t buttons = recvInt8(); // button1 0x01, button2 0x02, button3 0x04
        uint16_t posx = recvIntBE16();
        uint16_t posy = recvIntBE16();
        Application::debug("%s: mask: 0x%02x, posx: %d, posy: %d", __FUNCTION__, buttons, posx, posy);

        recvPointerEvent(buttons, posx, posy);
    }

    void RFB::ServerEncoder::recvCutText(void)
    {
        // RFB: 6.4.6
        // skip padding
        recvSkip(3);
        size_t length = recvIntBE32();
        Application::debug("%s: text length: %d", __FUNCTION__, length);

        // limiting untrusted sources 64k
        size_t recv = std::min(length, size_t(65535));
        auto buffer = recvData(recv);
        recvSkip(length - recv);

        recvCutTextEvent(buffer);
    }


    void RFB::ServerEncoder::recvSetContinuousUpdates(void)
    {
        int enable = recvInt8();
        int regx = recvIntBE16();
        int regy = recvIntBE16();
        int regw = recvIntBE16();
        int regh = recvIntBE16();

        Application::info("%s: region: [%d,%d,%d,%d], enabled: %d", __FUNCTION__, regx, regy, regw, regh, enable);

        continueUpdatesSupport = true;
        continueUpdatesProcessed = enable;

        recvSetContinuousUpdatesEvent(enable, XCB::Region(regx, regy, regw, regh));
    }

    void RFB::ServerEncoder::recvSetDesktopSize(void)
    {
        // skip padding (one byte!)
        recvSkip(1);
        int width = recvIntBE16();
        int height = recvIntBE16();
        int numOfScreens = recvInt8();
        recvSkip(1);
        Application::info("%s: size [%dx%d], screens: %d", __FUNCTION__, width, height, numOfScreens);

        // screens array
        std::vector<RFB::ScreenInfo> screens;
        for(int it = 0; it < numOfScreens; it++)
        {
            uint32_t id = recvIntBE32();
            uint16_t posx = recvIntBE16();
            uint16_t posy = recvIntBE16();
            uint16_t width = recvIntBE16();
            uint16_t height = recvIntBE16();
            uint32_t flags = recvIntBE32();
            screens.push_back({ .id = id, .posx = posx, .posy = posy, .width = width, .height = height, .flags = flags });
        }

        recvSetDesktopSizeEvent(screens);
    }

    void RFB::ServerEncoder::clientDisconnectedEvent(int display)
    {
        Application::warning("%s: display: %d", __FUNCTION__, display);
    }

    void RFB::ServerEncoder::sendColourMap(int first)
    {
        Application::info("%s: first: %d, colour map length: %d", __FUNCTION__, first, colourMap.size());
        std::scoped_lock<std::mutex> guard(sendLock);
        // RFB: 6.5.2
        sendInt8(RFB::SERVER_SET_COLOURMAP);
        sendInt8(0); // padding
        sendIntBE16(first); // first color
        sendIntBE16(colourMap.size());

        for(auto & col : colourMap)
        {
            sendIntBE16(col.r);
            sendIntBE16(col.g);
            sendIntBE16(col.b);
        }
        
        sendFlush();
    }

    void RFB::ServerEncoder::sendBellEvent(void)
    {
        Application::info("%s: process", __FUNCTION__);
        std::scoped_lock<std::mutex> guard(sendLock);
        // RFB: 6.5.3
        sendInt8(RFB::SERVER_BELL);
        sendFlush();
    }

    void RFB::ServerEncoder::sendCutTextEvent(const std::vector<uint8_t> & buf)
    {
        Application::debug("%s: length text: %d", __FUNCTION__, buf.size());
        std::scoped_lock<std::mutex> guard(sendLock);
        // RFB: 6.5.4
        sendInt8(RFB::SERVER_CUT_TEXT);
        sendInt8(0); // padding
        sendInt8(0); // padding
        sendInt8(0); // padding
        sendIntBE32(buf.size());
        sendRaw(buf.data(), buf.size());
        sendFlush();
    }

    void RFB::ServerEncoder::sendContinuousUpdates(bool enable)
    {
        // RFB: 6.5.5
        Application::info("%s: status: %s", __FUNCTION__, (enable ? "enable" : "disable"));

        std::scoped_lock<std::mutex> guard(sendLock);
        sendInt8(RFB::SERVER_CONTINUOUS_UPDATES).sendFlush();

        continueUpdatesProcessed = enable;
    }

    void RFB::ServerEncoder::sendFrameBufferUpdate(const FrameBuffer & fb)
    {
        auto & reg = fb.region();

        Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        std::scoped_lock<std::mutex> guard(sendLock);

        // RFB: 6.5.1
        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);

        // send encodings
        prefEncodingsPair.first(fb);

        sendFlush();
    }

    void RFB::ServerEncoder::sendFrameBufferUpdateRichCursor(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot)
    {
        auto & reg = fb.region();

        Application::debug("%s: region: [%d, %d, %d, %d], xhot: %d, yhot: %d", __FUNCTION__, reg.x, reg.y, reg.width, reg.height, xhot, yhot);

        std::scoped_lock<std::mutex> guard1(sendLock);

        // RFB: 6.5.1
        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);

        // regions counts
        sendIntBE16(1);

        std::scoped_lock<std::mutex> guard2(encodingBusy);

        // region size
        sendIntBE16(xhot);
        sendIntBE16(yhot);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);

        // region type
        sendIntBE32(RFB::ENCODING_RICH_CURSOR);

        Tools::StreamBitsPack bitmask;

        for(int oy = 0; oy < reg.height; ++oy)
        {
            for(int ox = 0; ox < reg.width; ++ox)
            {
                auto pixel = fb.pixel(XCB::Point(ox, oy));
                sendPixel(pixel);
                bitmask.pushBit(fb.pixelFormat().alpha(pixel));
            }

            bitmask.pushAlign();
        }

        const std::vector<uint8_t> & bitmaskBuf = bitmask.toVector();
        size_t bitmaskSize = std::floor((reg.width + 7) / 8) * reg.height;

        if(bitmaskSize != bitmaskBuf.size())
        {
            Application::error("%s: bitmask missmatch, buf size: %d, bitmask size: %d", __FUNCTION__, bitmaskBuf.size(), bitmaskSize);
            throw rfb_error(NS_FuncName);
        }

        sendData(bitmaskBuf);
        sendFlush();
    }

    std::string RFB::ServerEncoder::serverEncryptionInfo(void) const
    {
        return tls ? tls->sessionDescription() : "none";
    }

    int RFB::ServerEncoder::sendPixel(uint32_t pixel)
    {
        if(clientTrueColor)
        {
            switch(clientFormat.bytePerPixel())
            {
                case 4:
                    if(clientBigEndian)
                        sendIntBE32(clientFormat.convertFrom(serverFormat(), pixel));
                    else
                        sendIntLE32(clientFormat.convertFrom(serverFormat(), pixel));

                    return 4;

                case 2:
                    if(clientBigEndian)
                        sendIntBE16(clientFormat.convertFrom(serverFormat(), pixel));
                    else
                        sendIntLE16(clientFormat.convertFrom(serverFormat(), pixel));

                    return 2;

                case 1:
                    sendInt8(clientFormat.convertFrom(serverFormat(), pixel));
                    return 1;

                default:
                    Application::error("%s: %s", __FUNCTION__, "unknown pixel format");
                    break;
            }
        }
        else if(colourMap.size())
            Application::error("%s: %s", __FUNCTION__, "color map not impemented");

        throw rfb_error(NS_FuncName);
    }

    int RFB::ServerEncoder::sendCPixel(uint32_t pixel)
    {
        if(clientTrueColor && clientFormat.bitsPerPixel == 32)
        {
            auto pixel2 = clientFormat.convertFrom(serverFormat(), pixel);
            auto red = clientFormat.red(pixel2);
            auto green = clientFormat.green(pixel2);
            auto blue = clientFormat.blue(pixel2);
            std::swap(red, blue);
            sendInt8(red);
            sendInt8(green);
            sendInt8(blue);
            return 3;
        }

        return sendPixel(pixel);
    }

    int RFB::ServerEncoder::sendRunLength(size_t length)
    {
        if(0 == length)
        {
            Application::error("%s: %s", __FUNCTION__, "length is zero");
            throw rfb_error(NS_FuncName);
        }

        int res = 0;

        while(255 < length)
        {
            sendInt8(255);
            res += 1;
            length -= 255;
        }

        sendInt8((length - 1) % 255);
        return res + 1;
    }

    bool RFB::ServerEncoder::isContinueUpdatesSupport(void) const
    {
        return continueUpdatesSupport;
    }

    bool RFB::ServerEncoder::isContinueUpdatesProcessed(void) const
    {
        return continueUpdatesSupport && continueUpdatesProcessed;
    }

    bool RFB::ServerEncoder::isClientEncodings(int enc) const
    {
        return std::any_of(clientEncodings.begin(), clientEncodings.end(),  [=](auto & val) { return val == enc; });
    }

    void RFB::ServerEncoder::setDisabledEncodings(std::list<std::string> list)
    {
        disabledEncodings.swap(list);
    }

    void RFB::ServerEncoder::setPrefferedEncodings(std::list<std::string> list)
    {
        prefferedEncodings.swap(list);
    }

    void RFB::ServerEncoder::setEncodingDebug(int v)
    {
        encodingDebug = v;
    }

    void RFB::ServerEncoder::setEncodingThreads(int threads)
    {
        if(threads < 1)
            threads = 1;
        else if(std::thread::hardware_concurrency() < threads)
        {
            threads = std::thread::hardware_concurrency();
            Application::error("%s: encoding threads incorrect, fixed to hardware concurrency: %d", __FUNCTION__, threads);
        }

        Application::info("%s: using encoding threads: %d", __FUNCTION__, threads);
        encodingThreads = threads;
    }

    void RFB::ServerEncoder::zlibDeflateStart(size_t len)
    {
        auto it = std::find_if(clientEncodings.begin(), clientEncodings.end(),
                    [=](auto & val){ return ENCODING_COMPRESS1 <= val && val <= ENCODING_COMPRESS9; });
        int level = it != clientEncodings.end() ? ENCODING_COMPRESS9 - *it : 9;

        if(! zlib)
            zlib.reset(new ZLib::DeflateStream(level));

        zlib->prepareSize(len);
        streamOut = zlib.get();
    }

    void RFB::ServerEncoder::zlibDeflateStop(bool uint16sz)
    {
        streamOut = tls ? tls.get() : socket.get();
        auto zip = zlib->deflateFlush();

        if(uint16sz)
            sendIntBE16(zip.size());
        else
            sendIntBE32(zip.size());

        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
            Application::debug("%s: compress data length: %d", __FUNCTION__, zip.size());
        sendRaw(zip.data(), zip.size());
    }

    bool RFB::ServerEncoder::serverSelectClientEncoding(void)
    {
        for(int type : clientEncodings)
        {
            switch(type)
            {
                case RFB::ENCODING_ZLIB:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingZLib(fb); }, type);
                    return true;

                case RFB::ENCODING_HEXTILE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingHextile(fb, false); }, type);
                    return true;

                case RFB::ENCODING_ZLIBHEX:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingHextile(fb, true); }, type);
                    return true;

                case RFB::ENCODING_CORRE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingRRE(fb, true); }, type);
                    return true;

                case RFB::ENCODING_RRE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingRRE(fb, false); }, type);
                    return true;

                case RFB::ENCODING_TRLE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingTRLE(fb, false); }, type);
                    return true;

                case RFB::ENCODING_ZRLE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingTRLE(fb, true); }, type);
                    return true;

                default:
                    break;
            }
        }

        prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingRaw(fb); }, RFB::ENCODING_RAW);
        return true;
    }

    void RFB::ServerEncoder::serverSelectEncodings(void)
    {
        serverSelectClientEncoding();
        Application::info("%s: select encoding: %s", __FUNCTION__, RFB::encodingName(prefEncodingsPair.second));
    }

    void RFB::ServerEncoder::sendEncodingRaw(const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        // regions counts
        sendIntBE16(1);
        sendEncodingRawSubRegion(XCB::Point(0, 0), reg0, fb, 1);
    }

    void RFB::ServerEncoder::sendEncodingRawSubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        std::scoped_lock<std::mutex> guard(encodingBusy);

        if(encodingDebug)
            Application::debug("%s: job id: %d, [%d, %d, %d, %d]", __FUNCTION__, jobId, reg.x, reg.y, reg.width, reg.height);

        // region size
        sendIntBE16(top.x + reg.x);
        sendIntBE16(top.y + reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        // region type
        sendIntBE32(RFB::ENCODING_RAW);
        sendEncodingRawSubRegionRaw(reg, fb);
    }

    void RFB::ServerEncoder::sendEncodingRawSubRegionRaw(const XCB::Region & reg, const FrameBuffer & fb)
    {
        if(serverFormat() != clientFormat)
        {
            for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
                sendPixel(fb.pixel(reg.topLeft() + coord));
        }
        else
        {
            for(int yy = 0; yy < reg.height; ++yy)
            {
                size_t line = reg.width * fb.bytePerPixel();
                sendRaw(fb.pitchData(reg.y + yy) + reg.x * fb.bytePerPixel(), line);
            }
        }
    }

    std::list<XCB::RegionPixel> processingRRE(const XCB::Region & badreg, const FrameBuffer & fb, int skipPixel)
    {
        std::list<XCB::RegionPixel> goods;
        std::list<XCB::Region> bads1 = { badreg };
        std::list<XCB::Region> bads2;

        do
        {
            while(! bads1.empty())
            {
                for(auto & subreg : bads1.front().divideCounts(2, 2))
                {
                    auto pixel = fb.pixel(subreg.topLeft());

                    if((subreg.width == 1 && subreg.height == 1) || fb.allOfPixel(pixel, subreg))
                    {
                        if(pixel != skipPixel)
                        {
                            // maybe join prev
                            if(! goods.empty() && goods.back().first.y == subreg.y && goods.back().first.height == subreg.height &&
                               goods.back().first.x + goods.back().first.width == subreg.x && goods.back().second == pixel)
                                goods.back().first.width += subreg.width;
                            else
                                goods.emplace_back(subreg, pixel);
                        }
                    }
                    else
                        bads2.push_back(subreg);
                }

                bads1.pop_front();
            }

            if(bads2.empty())
                break;

            bads2.swap(bads1);
            bads2.clear();
        }
        while(! bads1.empty());

        return goods;
    }

    /* RRE */
    void RFB::ServerEncoder::sendEncodingRRE(const FrameBuffer & fb, bool corre)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: type: %s, region: [%d, %d, %d, %d]", __FUNCTION__, (corre ? "CoRRE" : "RRE"), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz = corre ? XCB::Size(64, 64) : XCB::Size(128, 128);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= encodingThreads && ! regions.empty())
        {
            encodingJobs.push_back(std::async(std::launch::async, & RFB::ServerEncoder::sendEncodingRRESubRegion, this, top, regions.front() - top, fb, jobId, corre));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : encodingJobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::microseconds(1)) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & RFB::ServerEncoder::sendEncodingRRESubRegion, this, top, regions.front() - top, fb, jobId, corre);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // wait jobs
        for(auto & job : encodingJobs)
            job.wait();

        encodingJobs.clear();
    }

    void RFB::ServerEncoder::sendEncodingRRESubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId, bool corre)
    {
        auto map = fb.pixelMapWeight(reg);
        auto sendHeaderRRE = [this](const XCB::Region & reg, bool corre)
        {
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.width);
            this->sendIntBE16(reg.height);
            // region type
            this->sendIntBE32(corre ? RFB::ENCODING_CORRE : RFB::ENCODING_RRE);
        };

        if(map.empty())
        {
            Application::error("%s: %s", __FUNCTION__, "pixels map is empty");
            throw rfb_error(NS_FuncName);
        }

        if(map.size() > 1)
        {
            int back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = processingRRE(reg, fb, back);
            const size_t rawLength = reg.width * reg.height * fb.bytePerPixel();
            const size_t rreLength = 4 + fb.bytePerPixel() + goods.size() * (fb.bytePerPixel() + (corre ? 4 : 8));

            // compare with raw
            if(rawLength < rreLength)
                sendEncodingRawSubRegion(top, reg, fb, jobId);
            else
            {
                std::scoped_lock<std::mutex> guard(encodingBusy);

                if(encodingDebug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, sub rects: %d",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size());

                sendHeaderRRE(reg + top, corre);
                sendEncodingRRESubRects(reg, fb, jobId, back, goods, corre);
            }
        }
        // if(map.size() == 1)
        else
        {
            int back = fb.pixel(reg.topLeft());
            std::scoped_lock<std::mutex> guard(encodingBusy);

            if(encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            sendHeaderRRE(reg + top, corre);
            // num sub rects
            sendIntBE32(1);
            // back pixel
            sendPixel(back);
            /* one fake sub region : RRE requires */
            // subrect pixel
            sendPixel(back);

            // subrect region (relative coords)
            if(corre)
            {
                sendInt8(0);
                sendInt8(0);
                sendInt8(1);
                sendInt8(1);
            }
            else
            {
                sendIntBE16(0);
                sendIntBE16(0);
                sendIntBE16(1);
                sendIntBE16(1);
            }
        }
    }

    void RFB::ServerEncoder::sendEncodingRRESubRects(const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList, bool corre)
    {
        // num sub rects
        sendIntBE32(rreList.size());
        // back pixel
        sendPixel(back);

        for(auto & pair : rreList)
        {
            // subrect pixel
            sendPixel(pair.pixel());
            auto & region = pair.region();

            // subrect region (relative coords)
            if(corre)
            {
                sendInt8(region.x - reg.x);
                sendInt8(region.y - reg.y);
                sendInt8(region.width);
                sendInt8(region.height);
            }
            else
            {
                sendIntBE16(region.x - reg.x);
                sendIntBE16(region.y - reg.y);
                sendIntBE16(region.width);
                sendIntBE16(region.height);
            }

            if(1 < encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    /* HexTile */
    void RFB::ServerEncoder::sendEncodingHextile(const FrameBuffer & fb, bool zlibver)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz(16, 16);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= encodingThreads && ! regions.empty())
        {
            encodingJobs.push_back(std::async(std::launch::async, & RFB::ServerEncoder::sendEncodingHextileSubRegion, this, top, regions.front() - top, fb, jobId, zlibver));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : encodingJobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::microseconds(1)) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & RFB::ServerEncoder::sendEncodingHextileSubRegion, this, top, regions.front() - top, fb, jobId, zlibver);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // wait jobs
        for(auto & job : encodingJobs)
            job.wait();

        encodingJobs.clear();
    }

    void RFB::ServerEncoder::sendEncodingHextileSubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId, bool zlibver)
    {
        auto map = fb.pixelMapWeight(reg);
        auto sendHeaderHexTile = [this](const XCB::Region & reg, bool zlibver)
        {
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.width);
            this->sendIntBE16(reg.height);
            // region type
            this->sendIntBE32(zlibver ? RFB::ENCODING_ZLIBHEX : RFB::ENCODING_HEXTILE);
        };

        if(map.empty())
        {
            Application::error("%s: %s", __FUNCTION__, "pixels map is empty");
            throw rfb_error(NS_FuncName);
        }

        if(map.size() == 1)
        {
            // wait thread
            std::scoped_lock<std::mutex> guard(encodingBusy);
            sendHeaderHexTile(reg + top, zlibver);
            int back = fb.pixel(reg.topLeft());

            if(encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // hextile flags
            sendInt8(RFB::HEXTILE_BACKGROUND);
            sendPixel(back);
        }
        else if(map.size() > 1)
        {
            // no wait, worked
            int back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = processingRRE(reg, fb, back);
            // all other color
            bool foreground = std::all_of(goods.begin(), goods.end(),
                                          [col = goods.front().second](auto & pair)
            {
                return pair.pixel() == col;
            });
            const size_t hextileRawLength = 1 + reg.width * reg.height * fb.bytePerPixel();
            // wait thread
            std::scoped_lock<std::mutex> guard(encodingBusy);
            sendHeaderHexTile(reg + top, zlibver);

            if(foreground)
            {
                const size_t hextileForegroundLength = 2 + 2 * fb.bytePerPixel() + goods.size() * 2;

                // compare with raw
                if(hextileRawLength < hextileForegroundLength)
                {
                    if(encodingDebug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");

                    sendEncodingHextileSubRaw(reg, fb, jobId, zlibver);
                }
                else
                {
                    if(encodingDebug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, sub rects: %d, %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "foreground");

                    sendEncodingHextileSubForeground(reg, fb, jobId, back, goods);
                }
            }
            else
            {
                const size_t hextileColoredLength = 2 + fb.bytePerPixel() + goods.size() * (2 + fb.bytePerPixel());

                // compare with raw
                if(hextileRawLength < hextileColoredLength)
                {
                    if(encodingDebug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");

                    sendEncodingHextileSubRaw(reg, fb, jobId, zlibver);
                }
                else
                {
                    if(encodingDebug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, sub rects: %d, %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "colored");

                    sendEncodingHextileSubColored(reg, fb, jobId, back, goods);
                }
            }
        }
    }

    void RFB::ServerEncoder::sendEncodingHextileSubColored(const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {
        // hextile flags
        sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_COLOURED | RFB::HEXTILE_SUBRECTS);
        // hextile background
        sendPixel(back);
        // hextile subrects
        sendInt8(rreList.size());

        for(auto & pair : rreList)
        {
            auto & region = pair.region();
            sendPixel(pair.pixel());
            sendInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            sendInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            if(1 < encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    void RFB::ServerEncoder::sendEncodingHextileSubForeground(const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {
        // hextile flags
        sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_FOREGROUND | RFB::HEXTILE_SUBRECTS);
        // hextile background
        sendPixel(back);
        // hextile foreground
        sendPixel(rreList.front().second);
        // hextile subrects
        sendInt8(rreList.size());

        for(auto & pair : rreList)
        {
            auto & region = pair.region();
            sendInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            sendInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            if(1 < encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d]",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height);
        }
    }

    void RFB::ServerEncoder::sendEncodingHextileSubRaw(const XCB::Region & reg, const FrameBuffer & fb, int jobId, bool zlibver)
    {
        if(zlibver)
        {
            // hextile flags
            sendInt8(RFB::HEXTILE_ZLIBRAW);
            zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());
            sendEncodingRawSubRegionRaw(reg, fb);
            zlibDeflateStop(true);
        }
        else
        {
            // hextile flags
            sendInt8(RFB::HEXTILE_RAW);
            sendEncodingRawSubRegionRaw(reg, fb);
        }
    }

    /* ZLib */
    void RFB::ServerEncoder::sendEncodingZLib(const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        // zlib specific: single thread only
        sendIntBE16(1);
        sendEncodingZLibSubRegion(XCB::Point(0, 0), reg0, fb, 1);
    }

    void RFB::ServerEncoder::sendEncodingZLibSubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        std::scoped_lock<std::mutex> guard(encodingBusy);

        if(encodingDebug)
            Application::debug("%s: job id: %d, [%d, %d, %d, %d]", __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

        // region size
        sendIntBE16(top.x + reg.x);
        sendIntBE16(top.y + reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        // region type
        sendIntBE32(RFB::ENCODING_ZLIB);
        zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());
        sendEncodingRawSubRegionRaw(reg, fb);
        zlibDeflateStop();
    }

    /* TRLE */
    void RFB::ServerEncoder::sendEncodingTRLE(const FrameBuffer & fb, bool zrle)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: type: %s, region: [%d, %d, %d, %d]", __FUNCTION__, (zrle ? "ZRLE" : "TRLE"), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Size bsz(64, 64);
        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= encodingThreads && ! regions.empty())
        {
            encodingJobs.push_back(std::async(std::launch::async, & RFB::ServerEncoder::sendEncodingTRLESubRegion, this, top, regions.front() - top, fb, jobId, zrle));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : encodingJobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::microseconds(1)) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & RFB::ServerEncoder::sendEncodingTRLESubRegion, this, top, regions.front() - top, fb, jobId, zrle);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // wait jobs
        for(auto & job : encodingJobs)
            job.wait();

        encodingJobs.clear();
    }

    void RFB::ServerEncoder::sendEncodingTRLESubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId, bool zrle)
    {
        auto map = fb.pixelMapWeight(reg);
        // convert to palette
        int index = 0;

        for(auto & pair : map)
            pair.second = index++;

        auto sendHeaderTRLE = [this](const XCB::Region & reg, bool zrle)
        {
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.width);
            this->sendIntBE16(reg.height);
            // region type
            this->sendIntBE32(zrle ? RFB::ENCODING_ZRLE : RFB::ENCODING_TRLE);
        };
        // wait thread
        std::scoped_lock<std::mutex> guard(encodingBusy);
        sendHeaderTRLE(reg + top, zrle);

        if(zrle)
            zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());

        if(map.size() == 1)
        {
            int back = fb.pixel(reg.topLeft());

            if(encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // subencoding type: solid tile
            sendInt8(1);
            sendCPixel(back);
        }
        else if(2 <= map.size() && map.size() <= 16)
        {
            size_t fieldWidth = 1;

            if(4 < map.size())
                fieldWidth = 4;
            else if(2 < map.size())
                fieldWidth = 2;

            if(encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], palsz: %d, packed: %d",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), fieldWidth);

            sendEncodingTRLESubPacked(reg, fb, jobId, fieldWidth, map, zrle);
        }
        else
        {
            auto rleList = fb.FrameBuffer::toRLE(reg);
            // rle plain size
            const size_t rlePlainLength = std::accumulate(rleList.begin(), rleList.end(), 1,
                                          [](int v, auto & pair)
            {
                return v + 3 + std::floor((pair.second - 1) / 255.0) + 1;
            });
            // rle palette size (2, 127)
            const size_t rlePaletteLength = 1 < rleList.size() && rleList.size() < 128 ? std::accumulate(rleList.begin(), rleList.end(), 1 + 3 * map.size(),
                                            [](int v, auto & pair)
            {
                return v + 1 + std::floor((pair.second - 1) / 255.0) + 1;
            }) : 0xFFFF;
            // raw length
            const size_t rawLength = 1 + 3 * reg.width * reg.height;

            if(rlePlainLength < rlePaletteLength && rlePlainLength < rawLength)
            {
                if(encodingDebug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], length: %d, rle plain",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, rleList.size());

                sendEncodingTRLESubPlain(reg, fb, rleList);
            }
            else if(rlePaletteLength < rlePlainLength && rlePaletteLength < rawLength)
            {
                if(encodingDebug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], pal size: %d, length: %d, rle palette",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), rleList.size());

                sendEncodingTRLESubPalette(reg, fb, map, rleList);
            }
            else
            {
                if(encodingDebug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], raw",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

                sendEncodingTRLESubRaw(reg, fb);
            }
        }

        if(zrle)
            zlibDeflateStop();
    }

    void RFB::ServerEncoder::sendEncodingTRLESubPacked(const XCB::Region & reg, const FrameBuffer & fb, int jobId, size_t field, const PixelMapWeight & pal, bool zrle)
    {
        // subencoding type: packed palette
        sendInt8(pal.size());

        // send palette
        for(auto & pair : pal)
            sendCPixel(pair.first);

        Tools::StreamBitsPack sb;

        // send packed rows
        for(int oy = 0; oy < reg.height; ++oy)
        {
            for(int ox = 0; ox < reg.width; ++ox)
            {
                auto pixel = fb.pixel(reg.topLeft() + XCB::Point(ox, oy));
                auto it = pal.find(pixel);
                auto index = it != pal.end() ? (*it).second : 0;
                sb.pushValue(index, field);
            }

            sb.pushAlign();
        }

        sendData(sb.toVector());

        if(1 < encodingDebug)
        {
            std::string str = Tools::buffer2hexstring<uint8_t>(sb.toVector().data(), sb.toVector().size(), 2);
            Application::debug("%s: job id: %d, packed stream: %s", __FUNCTION__, jobId, str.c_str());
        }
    }

    void RFB::ServerEncoder::sendEncodingTRLESubPlain(const XCB::Region & reg, const FrameBuffer & fb, const std::list<PixelLength> & rle)
    {
        // subencoding type: rle plain
        sendInt8(128);

        // send rle content
        for(auto & pair : rle)
        {
            sendCPixel(pair.pixel());
            sendRunLength(pair.length());
        }
    }

    void RFB::ServerEncoder::sendEncodingTRLESubPalette(const XCB::Region & reg, const FrameBuffer & fb, const PixelMapWeight & pal, const std::list<PixelLength> & rle)
    {
        // subencoding type: rle palette
        sendInt8(pal.size() + 128);

        // send palette
        for(auto & pair : pal)
            sendCPixel(pair.first);

        // send rle indexes
        for(auto & pair : rle)
        {
            auto it = pal.find(pair.pixel());
            auto index = it != pal.end() ? (*it).second : 0;

            if(1 == pair.length())
                sendInt8(index);
            else
            {
                sendInt8(index + 128);
                sendRunLength(pair.length());
            }
        }
    }

    void RFB::ServerEncoder::sendEncodingTRLESubRaw(const XCB::Region & reg, const FrameBuffer & fb)
    {
        // subencoding type: raw
        sendInt8(0);

        // send pixels
        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
            sendCPixel(fb.pixel(reg.topLeft() + coord));
    }

    /* pseudo encodings DesktopSize/Extended */
    void RFB::ServerEncoder::sendEncodingDesktopResize(const DesktopResizeStatus & status, const DesktopResizeError & error, const XCB::Size & desktopSize)
    {
        auto statusCode = desktopResizeStatusCode(status);
        auto errorCode = desktopResizeErrorCode(error);

        Application::info("%s: status: %d, error: %d, size [%d, %d]", __FUNCTION__, statusCode, errorCode, desktopSize.width, desktopSize.height);

        if(! isClientEncodings(RFB::ENCODING_EXT_DESKTOP_SIZE))
        {
            Application::error("%s: %s", __FUNCTION__, "client not supported ExtDesktopResize encoding");
            throw rfb_error(NS_FuncName);
        }

        // send
        std::scoped_lock<std::mutex> guard(sendLock);
        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);
        // number of rects
        sendIntBE16(1);

        sendIntBE16(statusCode);
        sendIntBE16(errorCode);
        sendIntBE16(desktopSize.width);
        sendIntBE16(desktopSize.height);

        sendIntBE32(RFB::ENCODING_EXT_DESKTOP_SIZE);
        // number of screens
        sendInt8(1);
        // padding
        sendZero(3);
        // id
        sendIntBE32(0);
        // xpos
        sendIntBE16(0);
        // ypos
        sendIntBE16(0);
        // width
        sendIntBE16(desktopSize.width);
        // height
        sendIntBE16(desktopSize.height);
        // flags
        sendIntBE32(0);

        sendFlush();
    }

#ifdef LTSM_CHANNELS
    void RFB::ServerEncoder::sendEncodingLtsmSupported(void)
    {
        Application::info("%s: server supported", __FUNCTION__);
        std::scoped_lock<std::mutex> guard(sendLock);

        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);
        // rects
        sendIntBE16(1);

        sendIntBE16(0);
        sendIntBE16(0);
        sendIntBE16(0);
        sendIntBE16(0);
        sendIntBE32(ENCODING_LTSM);
        // raw size + raw data
        sendIntBE32(0);
        sendFlush();
    }

    void RFB::ServerEncoder::sendLtsmEvent(uint8_t channel, const uint8_t* buf, size_t len)
    {
        if(isClientEncodings(RFB::ENCODING_LTSM))
            sendLtsm(*this, sendLock, channel, buf, len);
    }

    void RFB::ServerEncoder::recvChannelSystem(const std::vector<uint8_t> & buf)
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

        Application::info("%s: cmd: %s", __FUNCTION__, cmd.c_str());

        if(cmd == SystemCommand::ClientVariables)
            systemClientVariables(jo);
        else
        if(cmd == SystemCommand::KeyboardChange)
            systemKeyboardChange(jo);
        else
        if(cmd == SystemCommand::TransferFiles)
            systemTransferFiles(jo);
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
#endif

}
