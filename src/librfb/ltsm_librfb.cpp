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

#include <chrono>
#include <cstring>
#include <fstream>
#include <algorithm>

#include "ltsm_application.h"
#include "ltsm_librfb.h"

using namespace std::chrono_literals;

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

    RFB::ServerEncoding::ServerEncoding(int sockfd)
    {
        if(0 < sockfd)
            socket.reset(new SocketStream(sockfd));
        else
            socket.reset(new InetStream());

        streamIn = streamOut = socket.get();
    }

    void RFB::ServerEncoding::sendFlush(void)
    {
        if(serviceAlive())
            streamOut->sendFlush();
    }

    void RFB::ServerEncoding::sendRaw(const void* ptr, size_t len)
    {
        if(serviceAlive())
        {
            streamOut->sendRaw(ptr, len);
            netStatTx += len;
        }
    }
    
    void RFB::ServerEncoding::recvRaw(void* ptr, size_t len) const
    {
        if(serviceAlive())
        {
            streamIn->recvRaw(ptr, len);
            netStatRx += len;
        }
    }
    
    void RFB::ServerEncoding::recvRaw(void* ptr, size_t len, size_t timeout) const
    {
        if(serviceAlive())
        {
            streamIn->recvRaw(ptr, len, timeout);
            netStatRx += len;
        }
    }
    
    bool RFB::ServerEncoding::hasInput(void) const
    {
        return serviceAlive() ? streamIn->hasInput() : false;
    }

    size_t RFB::ServerEncoding::hasData(void) const
    {
        return serviceAlive() ? streamIn->hasData() : 0;
    }
    
    uint8_t RFB::ServerEncoding::peekInt8(void) const
    {
        return serviceAlive() ? streamIn->peekInt8() : 0;
    }

    bool RFB::ServerEncoding::isUpdateProcessed(void) const
    {
        return fbUpdateProcessing || ! encodingJobs.empty();
    }

    void RFB::ServerEncoding::waitUpdateProcess(void)
    {
        while(isUpdateProcessed())
            std::this_thread::sleep_for(1ms);
    }

    void RFB::ServerEncoding::serverSetPixelFormat(const PixelFormat & pf)
    {
        serverFormat = pf;
    }

    bool RFB::ServerEncoding::serverAuthVncInit(const std::string & passwdFile)
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

    bool RFB::ServerEncoding::serverAuthVenCryptInit(const SecurityInfo & secInfo)
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
            Application::debug("%s: client choice vencrypt mode: 0x%08x", __FUNCTION__, mode);

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

            if(! std::filesystem::exists(secInfo.caFile))
                errFile = &secInfo.caFile;
            if(! std::filesystem::exists(secInfo.certFile))
                errFile = &secInfo.certFile;
            if(! std::filesystem::exists(secInfo.keyFile))
                errFile = &secInfo.keyFile;

            if(errFile)
            {
                Application::error("%s: file not found: %s", __FUNCTION__, errFile->c_str());
                sendInt8(0).sendFlush();
                return false;
            }
        }

        sendInt8(1).sendFlush();

        // init hasdshake
        tls.reset(new TLS::Stream(socket.get()));
        bool tlsInitHandshake = x509Mode ?
                                tls->initX509Handshake(secInfo.tlsPriority, true, secInfo.caFile, secInfo.certFile, secInfo.keyFile, secInfo.crlFile, secInfo.tlsDebug) :
                                tls->initAnonHandshake(secInfo.tlsPriority, true, secInfo.tlsDebug);

        if(tlsInitHandshake)
            streamIn = streamOut = tls.get();

        return tlsInitHandshake;
    }

    int RFB::ServerEncoding::serverHandshakeVersion(void)
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

    bool RFB::ServerEncoding::serverSecurityInit(int protover, const SecurityInfo & secInfo)
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

                    if(! std::filesystem::exists(secInfo.passwdFile))
                    {
                        Application::error("%s: file not found: %s", __FUNCTION__, secInfo.passwdFile.c_str());
                        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                        return false;
                    }

                    if(! serverAuthVncInit(secInfo.passwdFile))
                    {
                        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                        return false;
                    }

                    sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
                }
                else if(clientSecurity == RFB::SECURITY_TYPE_VENCRYPT && secInfo.authVenCrypt)
                {
                    if(! serverAuthVenCryptInit(secInfo))
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

    void RFB::ServerEncoding::serverClientInit(std::string_view desktopName)
    {
        // RFB 6.3.1 client init
        int clientSharedFlag = recvInt8();
        Application::debug("%s: client shared: 0x%02x", __FUNCTION__, clientSharedFlag);
        // RFB 6.3.2 server init
        auto wsz = xcbDisplay()->size();
        sendIntBE16(wsz.width);
        sendIntBE16(wsz.height);
        Application::debug("%s: server pixel format, bpp: %d, depth: %d, bigendian: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                           __FUNCTION__, serverFormat.bitsPerPixel, xcbDisplay()->depth(), big_endian, 
                            serverFormat.redMax, serverFormat.redShift, serverFormat.greenMax, serverFormat.greenShift, serverFormat.blueMax, serverFormat.blueShift);
        clientFormat = serverFormat;
        // send pixel format
        sendInt8(serverFormat.bitsPerPixel);
        sendInt8(xcbDisplay()->depth());
        sendInt8(big_endian ? 1 : 0);
        // true color
        sendInt8(1);
        sendIntBE16(serverFormat.redMax);
        sendIntBE16(serverFormat.greenMax);
        sendIntBE16(serverFormat.blueMax);
        sendInt8(serverFormat.redShift);
        sendInt8(serverFormat.greenShift);
        sendInt8(serverFormat.blueShift);
        // send padding
        sendInt8(0);
        sendInt8(0);
        sendInt8(0);
        // send name desktop
        sendIntBE32(desktopName.size()).sendString(desktopName).sendFlush();
    }

    void RFB::ServerEncoding::serverSendUpdateBackground(const XCB::Region & area)
    {
        fbUpdateProcessing = true;
        // background job
        std::thread([=]()
        {
            bool error = false;

            try
            {
                this->serverSendFrameBufferUpdate(area);
                fbUpdateProcessing = false;
            }
            catch(const std::exception & err)
            {
                error = true;
                Application::error("%s: vnc exception: %s", __FUNCTION__, err.what());
            }
            catch(...)
            {
                error = true;
            }

            if(error)
                this->serviceStop();
        }).detach();
    }

    void RFB::ServerEncoding::clientSetPixelFormat(void)
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
        Application::notice("%s: bpp: %d, depth: %d, be: %d, truecol: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                            __FUNCTION__, bitsPerPixel, depth, bigEndian, trueColor, redMax, redShift, greenMax, greenShift, blueMax, blueShift);

        switch(bitsPerPixel)
        {
            case 32:
            case 16:
            case 8:
                break;

            default:
                throw std::runtime_error(Tools::StringFormat("%1: unknown client pixel format").arg(__FUNCTION__));
        }
        
        if(trueColor == 0 || redMax == 0 || greenMax == 0 || blueMax == 0)
            throw std::runtime_error(Tools::StringFormat("%1: unsupported pixel format").arg(__FUNCTION__));

        clientTrueColor = trueColor;
        clientBigEndian = bigEndian;
        clientFormat = PixelFormat(bitsPerPixel, redMax, greenMax, blueMax, 0, redShift, greenShift, blueShift, 0);

        colourMap.clear();
    }

    bool RFB::ServerEncoding::clientSetEncodings(void)
    {
        waitUpdateProcess();
        // RFB: 6.4.2
        // skip padding
        recvSkip(1);
        int numEncodings = recvIntBE16();

        Application::notice("%s: encoding counts: %d", __FUNCTION__, numEncodings);
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
        
        return true;
    }

    std::pair<bool, XCB::Region>
    RFB::ServerEncoding::clientFramebufferUpdate(void)
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
        auto serverRegion = xcbDisplay()->region();

        if(fullUpdate)
        {
            clientRegion = serverRegion;
            desktopResizeModeInit();
        }
        else
        {
            clientRegion = serverRegion.intersected(clientRegion);

            if(clientRegion.empty())
            {
                Application::warning("%s: client region intersection with display [%d, %d] failed", 
                                        __FUNCTION__, serverRegion.width, serverRegion.height);
            }
        }

        return std::make_pair(fullUpdate, clientRegion);
    }

    void RFB::ServerEncoding::clientKeyEvent(bool xcbAllow, const JsonObject* keymap)
    {
        // RFB: 6.4.4
        int pressed = recvInt8();
        recvSkip(2);
        int keysym = recvIntBE32();
        Application::debug("%s: action %s, keysym: 0x%08x", __FUNCTION__, (pressed ? "pressed" : "released"), keysym);

        if(xcbAllow)
        {
            // local keymap priority "vnc:keymap:file"
            if(auto value = (keymap ? keymap->getValue(Tools::hex(keysym, 8)) : nullptr))
            {
                if(value->isArray())
                {
                    auto ja = static_cast<const JsonArray*>(value);

                    for(auto keycode : ja->toStdVector<int>())
                        xcbDisplay()->fakeInputKeycode(keycode, 0 < pressed);
                }
                else
                    xcbDisplay()->fakeInputKeycode(value->getInteger(), 0 < pressed);
            }
            else
                xcbDisplay()->fakeInputKeysym(keysym, 0 < pressed);
        }
    }

    void RFB::ServerEncoding::clientPointerEvent(bool xcbAllow)
    {
        // RFB: 6.4.5
        int mask = recvInt8(); // button1 0x01, button2 0x02, button3 0x04
        int posx = recvIntBE16();
        int posy = recvIntBE16();
        Application::debug("%s: mask: 0x%02x, posx: %d, posy: %d", __FUNCTION__, mask, posx, posy);

        if(xcbAllow)
        {
            if(this->pressedMask ^ mask)
            {
                for(int num = 0; num < 8; ++num)
                {
                    int bit = 1 << num;

                    if(bit & mask)
                    {
                        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
                            Application::debug("%s: xfb fake input pressed: %d", __FUNCTION__, num + 1);

                        xcbDisplay()->fakeInputTest(XCB_BUTTON_PRESS, num + 1, posx, posy);
                        this->pressedMask |= bit;
                    }
                    else if(bit & pressedMask)
                    {
                        if(Application::isDebugLevel(DebugLevel::SyslogTrace))
                            Application::debug("%s: xfb fake input released: %d", __FUNCTION__, num + 1);

                        xcbDisplay()->fakeInputTest(XCB_BUTTON_RELEASE, num + 1, posx, posy);
                        this->pressedMask &= ~bit;
                    }
                }
            }
            else
            {
                if(Application::isDebugLevel(DebugLevel::SyslogTrace))
                    Application::debug("%s: xfb fake input move, posx: %d, posy: %d", __FUNCTION__, posx, posy);

                xcbDisplay()->fakeInputTest(XCB_MOTION_NOTIFY, 0, posx, posy);
            }
        }
    }

    void RFB::ServerEncoding::clientCutTextEvent(bool xcbAllow, bool clipboardEnable)
    {
        // RFB: 6.4.6
        // skip padding
        recvSkip(3);
        size_t length = recvIntBE32();
        Application::debug("%s: text length: %d", __FUNCTION__, length);

        if(xcbAllow && clipboardEnable)
        {
            size_t maxreq = xcbDisplay()->getMaxRequest();
            size_t chunk = std::min(maxreq, length);
            std::vector<uint8_t> buffer;
            buffer.reserve(chunk);

            for(size_t pos = 0; pos < chunk; ++pos)
                buffer.push_back(recvInt8());

            recvSkip(length - chunk);
            xcbDisplay()->setClipboardEvent(buffer);
        }
        else
            recvSkip(length);
    }

    void RFB::ServerEncoding::clientEnableContinuousUpdates(void)
    {
        int enable = recvInt8();
        int regx = recvIntBE16();
        int regy = recvIntBE16();
        int regw = recvIntBE16();
        int regh = recvIntBE16();
        Application::notice("%s: region: [%d,%d,%d,%d], enabled: %d", __FUNCTION__, regx, regy, regw, regh, enable);
        throw std::runtime_error("clientEnableContinuousUpdates: not implemented");
    }

    void RFB::ServerEncoding::clientSetDesktopSizeEvent(void)
    {
        // skip padding (one byte!)
        recvSkip(1);
        int width = recvIntBE16();
        int height = recvIntBE16();
        int numOfScreens = recvInt8();
        recvSkip(1);
        Application::notice("%s: size [%dx%d], screens: %d", __FUNCTION__, width, height, numOfScreens);

        // screens array
        std::vector<RFB::ScreenInfo> screens(numOfScreens);
        for(auto & info : screens)
        {
            info.id = recvIntBE32();
            info.xpos = recvIntBE16();
            info.ypos = recvIntBE16();
            info.width = recvIntBE16();
            info.height = recvIntBE16();
            info.flags = recvIntBE32();
        }

        desktopResizeModeSet(RFB::DesktopResizeMode::ClientRequest, screens);
    }

    void RFB::ServerEncoding::clientDisconnectedEvent(int display)
    {
        Application::warning("%s: display: %d", __FUNCTION__, display);
    }

    void RFB::ServerEncoding::serverSendColourMap(int first)
    {
        const std::lock_guard<std::mutex> lock(networkBusy);
        Application::notice("%s: first: %d, colour map length: %d", __FUNCTION__, first, colourMap.size());
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

    void RFB::ServerEncoding::serverSendBell(void)
    {
        const std::lock_guard<std::mutex> lock(networkBusy);
        Application::notice("%s: process", __FUNCTION__);
        // RFB: 6.5.3
        sendInt8(RFB::SERVER_BELL);
        sendFlush();
    }

    void RFB::ServerEncoding::serverSendCutText(const std::vector<uint8_t> & buf)
    {
        const std::lock_guard<std::mutex> lock(networkBusy);
        Application::info("%s: length text: %d", __FUNCTION__, buf.size());
        // RFB: 6.5.4
        sendInt8(RFB::SERVER_CUT_TEXT);
        sendInt8(0); // padding
        sendInt8(0); // padding
        sendInt8(0); // padding
        sendIntBE32(buf.size());
        sendRaw(buf.data(), buf.size());
        sendFlush();
    }

    void RFB::ServerEncoding::serverSendEndContinuousUpdates(void)
    {
        // RFB: 6.5.5
        Application::notice("%s: process", __FUNCTION__);
        sendInt8(RFB::CLIENT_ENABLE_CONTINUOUS_UPDATES).sendFlush();
    }

    bool RFB::ServerEncoding::serverSendFrameBufferUpdate(const XCB::Region & reg)
    {
        const std::lock_guard<std::mutex> lock(networkBusy);

        if(auto reply = xcbDisplay()->copyRootImageRegion(reg))
        {
            const int bytePerPixel = xcbDisplay()->pixmapBitsPerPixel(reply->depth()) >> 3;

            if(Application::isDebugLevel(DebugLevel::SyslogTrace))
            {
                if(const xcb_visualtype_t* visual = xcbDisplay()->visual(reply->visId()))
                {
                    Application::debug("%s: shm request size [%d, %d], reply: length: %d, depth: %d, bits per rgb value: %d, red: %08x, green: %08x, blue: %08x, color entries: %d",
                                       __FUNCTION__, reg.width, reg.height, reply->size(), reply->depth(), visual->bits_per_rgb_value, visual->red_mask,
                                       visual->green_mask, visual->blue_mask, visual->colormap_entries);
                }

            }

            Application::debug("%s: server send fb update: [%d, %d, %d, %d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

            // fix align
            if(reply->size() != reg.width * reg.height * bytePerPixel)
                throw std::runtime_error(Tools::StringFormat("%1: region not aligned").arg(__FUNCTION__));

            // RFB: 6.5.1
            sendInt8(RFB::SERVER_FB_UPDATE);
            // padding
            sendInt8(0);
            FrameBuffer frameBuffer(reply->data(), reg, serverFormat);
            serverPostProcessingFrameBuffer(frameBuffer);
            int encodingLength = 0;
            // send encodings
            size_t netStatTx2 = netStatTx;
            prefEncodingsPair.first(frameBuffer);

            if(Application::isDebugLevel(DebugLevel::SyslogTrace))
            {
                size_t rawLength = 14 /* raw header for one region */ + reg.width * reg.height * clientFormat.bytePerPixel();
                double optimize = 100.0f - (netStatTx - netStatTx2) * 100 / static_cast<double>(rawLength);
                Application::debug("encoding %s optimize: %.*f%% (send: %d, raw: %d), region(%d, %d)", RFB::encodingName(prefEncodingsPair.second), 2, optimize, encodingLength, rawLength, reg.width, reg.height);
            }

            sendFlush();
            xcbDisplay()->damageSubtrack(reg);
        }
        else
            Application::error("%s: failed", __FUNCTION__);

        return true;
    }

    std::string RFB::ServerEncoding::serverEncryptionInfo(void) const
    {
        return tls ? tls->sessionDescription() : std::string();
    }

    int RFB::ServerEncoding::sendPixel(uint32_t pixel)
    {
        if(clientTrueColor)
        {
            switch(clientFormat.bytePerPixel())
            {
                case 4:
                    if(clientBigEndian)
                        sendIntBE32(clientFormat.convertFrom(serverFormat, pixel));
                    else
                        sendIntLE32(clientFormat.convertFrom(serverFormat, pixel));

                    return 4;

                case 2:
                    if(clientBigEndian)
                        sendIntBE16(clientFormat.convertFrom(serverFormat, pixel));
                    else
                        sendIntLE16(clientFormat.convertFrom(serverFormat, pixel));

                    return 2;

                case 1:
                    sendInt8(clientFormat.convertFrom(serverFormat, pixel));
                    return 1;

                default:
                    break;
            }
        }
        else if(colourMap.size())
            Application::error("%s: not usable", __FUNCTION__);

        throw std::runtime_error("sendPixel: unknown format");
    }

    int RFB::ServerEncoding::sendCPixel(uint32_t pixel)
    {
        if(clientTrueColor && clientFormat.bitsPerPixel == 32)
        {
            auto pixel2 = clientFormat.convertFrom(serverFormat, pixel);
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

    int RFB::ServerEncoding::sendRunLength(size_t length)
    {
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

    void RFB::ServerEncoding::desktopResizeModeDisable(void)
    {
        desktopMode = DesktopResizeMode::Disabled;
    }

    void RFB::ServerEncoding::desktopResizeModeSet(const DesktopResizeMode & mode, std::vector<RFB::ScreenInfo> screens)
    {
        desktopMode = DesktopResizeMode::ServerInform;
        screensInfo.swap(screens);
    }

    bool RFB::ServerEncoding::desktopResizeModeChange(const XCB::Size & sz)
    {
        if(DesktopResizeMode::Undefined != desktopMode &&
               DesktopResizeMode::Disabled != desktopMode &&
               (screensInfo.empty() || (screensInfo.front().width != sz.width || screensInfo.front().height != sz.height)))
        {
            screensInfo.push_back({ .width = sz.width, .height = sz.height });
            desktopMode = DesktopResizeMode::ServerInform;
            return true;
        }

        return false;
    }

    bool RFB::ServerEncoding::desktopResizeModeInit(void)
    {
        if(desktopMode == DesktopResizeMode::Undefined &&
            std::any_of(clientEncodings.begin(), clientEncodings.end(),
                           [=](auto & val) { return  val == RFB::ENCODING_EXT_DESKTOP_SIZE; }))
        {
            desktopMode = DesktopResizeMode::ServerInform;
            return true;
        }

        return false;
    }

    bool RFB::ServerEncoding::isClientEncodings(int enc) const
    {
        return std::any_of(clientEncodings.begin(), clientEncodings.end(),  [=](auto & val) { return val == enc; });
    }

    void RFB::ServerEncoding::setDisabledEncodings(std::list<std::string> list)
    {
        disabledEncodings.swap(list);
    }

    void RFB::ServerEncoding::setPrefferedEncodings(std::list<std::string> list)
    {
        prefferedEncodings.swap(list);
    }

    void RFB::ServerEncoding::setEncodingDebug(int v)
    {
        encodingDebug = v;
    }

    void RFB::ServerEncoding::setEncodingThreads(int threads)
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

    void RFB::ServerEncoding::zlibDeflateStart(size_t len)
    {
        if(! zlib)
            zlib.reset(new ZLib::DeflateStream());

        zlib->prepareSize(len);
        streamOut = zlib.get();
    }

    std::vector<uint8_t> RFB::ServerEncoding::zlibDeflateStop(void)
    {
        streamOut = tls ? tls.get() : socket.get();
        return zlib->syncFlush();
    }
}

