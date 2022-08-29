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

#include <tuple>
#include <cctype>
#include <string>
#include <chrono>
#include <thread>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <iterator>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_x11vnc.h"
#include "ltsm_connector_vnc.h"

using namespace std::chrono_literals;

namespace LTSM
{
    const char* desktopResizeModeString(const DesktopResizeMode & mode)
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

    /* Connector::VNC */
    Connector::VNC::VNC(int fd, const JsonObject & jo)
        : DisplayProxy(jo), streamIn(nullptr), streamOut(nullptr), encodingDebug(0), encodingThreads(2), netStatRx(0), netStatTx(0), pressedMask(0),
          loopMessage(true), fbUpdateProcessing(false), sendBellFlag(false), desktopResizeMode(DesktopResizeMode::Disabled)
    {
        socket.reset(new InetStream());

        if(jo.getBoolean("DesktopResized"))
            desktopResizeMode = DesktopResizeMode::Undefined;

        if(0 < fd)
            socket.reset(new SocketStream(fd));
        else
            socket.reset(new InetStream());

        streamIn = streamOut = socket.get();
    }

    void Connector::VNC::sendFlush(void)
    {
        if(loopMessage)
            streamOut->sendFlush();
    }

    void Connector::VNC::sendRaw(const void* ptr, size_t len)
    {
        if(loopMessage)
        {
            streamOut->sendRaw(ptr, len);
            netStatTx += len;
        }
    }

    void Connector::VNC::recvRaw(void* ptr, size_t len) const
    {
        if(loopMessage)
        {
            streamIn->recvRaw(ptr, len);
            netStatRx += len;
        }
    }

    bool Connector::VNC::hasInput(void) const
    {
        return loopMessage ? streamIn->hasInput() : false;
    }

    size_t Connector::VNC::hasData(void) const
    {
        return loopMessage ? streamIn->hasData() : false;
    }

    uint8_t Connector::VNC::peekInt8(void) const
    {
        return loopMessage ? streamIn->peekInt8() : 0;
    }

    bool Connector::VNC::clientAuthVnc(void)
    {
        std::vector<uint8_t> challenge = TLS::randomKey(16);
        std::vector<uint8_t> response(16);
        std::string tmp = Tools::buffer2hexstring<uint8_t>(challenge.data(), challenge.size(), 2);
        Application::debug("%s: challenge: %s", __FUNCTION__, tmp.c_str());
        sendRaw(challenge.data(), challenge.size());
        sendFlush();
        recvRaw(response.data(), response.size());
        tmp = Tools::buffer2hexstring<uint8_t>(response.data(), response.size(), 2);
        Application::debug("%s: response: %s", __FUNCTION__, tmp.c_str());
        std::ifstream ifs(_config->getString("passwdfile"), std::ifstream::in);

        while(ifs.good())
        {
            std::string pass;
            std::getline(ifs, pass);
            auto crypt = TLS::encryptDES(challenge, pass);
            tmp = Tools::buffer2hexstring<uint8_t>(crypt.data(), crypt.size(), 2);
            Application::debug("%s: encrypt: %s", __FUNCTION__, tmp.c_str());

            if(crypt == response)
                return true;
        }

        const std::string err("password mismatch");
        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(err.size()).sendString(err).sendFlush();
        Application::error("error: %s", err.c_str());
        return false;
    }

    bool Connector::VNC::clientAuthVenCrypt(void)
    {
        const std::string tlsPriority = "NORMAL:+ANON-ECDH:+ANON-DH";
        int tlsDebug = _config->getInteger("vnc:gnutls:debug", 3);
        bool noAuth = _config->getBoolean("noauth", false);
        // VenCrypt version
        sendInt8(0).sendInt8(2).sendFlush();
        // client req
        int majorVer = recvInt8();
        int minorVer = recvInt8();
        Application::debug("RFB 6.2.19, client vencrypt version: %d.%d", majorVer, minorVer);

        if(majorVer != 0 || (minorVer < 1 || minorVer > 2))
        {
            // send unsupported
            sendInt8(255).sendFlush();
            Application::error("error: %s", "unsupported vencrypt version");
            return false;
        }

        // send supported
        sendInt8(0);

        if(minorVer == 1)
        {
            sendInt8(1).sendInt8(noAuth ? RFB::SECURITY_VENCRYPT01_TLSNONE : RFB::SECURITY_VENCRYPT01_TLSVNC).sendFlush();
            int res = recvInt8();
            Application::debug("RFB 6.2.19.0.1, client choice vencrypt security: 0x%02x", res);

            switch(res)
            {
                case RFB::SECURITY_VENCRYPT01_TLSNONE:
                case RFB::SECURITY_VENCRYPT01_TLSVNC:
                    break;

                case RFB::SECURITY_VENCRYPT01_X509NONE:
                    Application::error("error: %s", "unsupported vencrypt security");
                    return false;

                default:
                    Application::error("error: %s", "unsupported vencrypt security");
                    return false;
            }
        }
        else
            // if(minorVer == 2)
        {
            sendInt8(1).sendIntBE32(noAuth ? RFB::SECURITY_VENCRYPT02_TLSNONE : RFB::SECURITY_VENCRYPT02_TLSVNC).sendFlush();
            int res = recvIntBE32();
            Application::debug("RFB 6.2.19.0.2, client choice vencrypt security: 0x%08x", res);

            switch(res)
            {
                case RFB::SECURITY_VENCRYPT02_TLSNONE:
                case RFB::SECURITY_VENCRYPT02_TLSVNC:
                    break;

                case RFB::SECURITY_VENCRYPT02_X509NONE:
                    Application::error("error: %s", "unsupported vencrypt security");
                    return false;

                default:
                    Application::error("error: %s", "unsupported vencrypt security");
                    return false;
            }
        }

        sendInt8(1).sendFlush();
        // init hasdshake
        tls.reset(new TLS::Stream(socket.get()));

        if(tls->initAnonHandshake(tlsPriority, tlsDebug))
        {
            streamIn = streamOut = tls.get();
            return noAuth ? true : clientAuthVnc();
        }

        return false;
    }

    int Connector::VNC::communication(void)
    {
        Application::info("%s: remote addr: %s", __FUNCTION__, _remoteaddr.c_str());
        encodingThreads = _config->getInteger("threads", 2);

        if(encodingThreads < 1)
            encodingThreads = 1;
        else if(std::thread::hardware_concurrency() < encodingThreads)
        {
            encodingThreads = std::thread::hardware_concurrency();
            Application::error("encoding threads incorrect, fixed to hardware concurrency: %d", encodingThreads);
        }

        Application::info("using encoding threads: %d", encodingThreads);
        encodingDebug = _config->getInteger("vnc:encoding:debug", 0);
        prefEncodingsPair = selectEncodings();
        disabledEncodings = _config->getStdList<std::string>("vnc:encoding:blacklist");
        prefferedEncodings = _config->getStdList<std::string>("vnc:encoding:preflist");

        // lower list
        for(auto & enc : prefferedEncodings)
            enc = Tools::lower(enc);

        if(_config->hasKey("keymapfile"))
        {
            auto file = _config->getString("keymapfile");
            JsonContentFile jc(file);

            if(jc.isValid() && jc.isObject())
            {
                keymap.reset(new JsonObject());
                auto jo = jc.toObject();

                for(auto & key : jo.keys())
                    if(auto map = jo.getObject(key))
                        keymap->join(*map);

                Application::notice("keymap loaded: %s, items: %d", file.c_str(), keymap->size());
            }
            else
                Application::error("keymap invalid: %s", file.c_str());
        }

        std::string encryptionInfo = "none";

        if(! disabledEncodings.empty())
            disabledEncodings.remove_if([](auto & str)
        {
            return 0 == Tools::lower(str).compare("raw");
        });
        // Xvfb: session request
        Application::info("default encoding: %s", RFB::encodingName(prefEncodingsPair.second));

        if(! xcbConnect())
        {
            Application::error("%s", "xcb connect failed");
            return EXIT_FAILURE;
        }

        const xcb_visualtype_t* visual = _xcbDisplay->visual();

        if(! visual)
        {
            Application::error("%s", "xcb visual empty");
            return EXIT_FAILURE;
        }

        Application::info("xcb max request: %d", _xcbDisplay->getMaxRequest());
        // RFB 6.1.1 version
        auto version = Tools::StringFormat("RFB 00%1.00%2\n").arg(RFB::VERSION_MAJOR).arg(RFB::VERSION_MINOR);
        sendString(version).sendFlush();
        std::string magick = recvString(12);
        Application::debug("RFB 6.1.1, handshake version: %s", magick.c_str());

        if(magick != version)
        {
            Application::error("%s", "handshake failure");
            return EXIT_FAILURE;
        }

        // init server format
        serverFormat = PixelFormat(_xcbDisplay->bitsPerPixel(), visual->red_mask, visual->green_mask, visual->blue_mask, 0);
        bool tlsDisable = _config->getBoolean("notls", false);
        bool noAuth = _config->getBoolean("noauth", false);

        // RFB 6.1.2 security
        if(tlsDisable)
            sendInt8(1);
        else
        {
            sendInt8(2);
            sendInt8(RFB::SECURITY_TYPE_VENCRYPT);
        }

        sendInt8(noAuth ? RFB::SECURITY_TYPE_NONE : RFB::SECURITY_TYPE_VNC);
        sendFlush();
        int clientSecurity = recvInt8();
        Application::debug("RFB 6.1.2, client security: 0x%02x", clientSecurity);

        if(noAuth && clientSecurity == RFB::SECURITY_TYPE_NONE)
            sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
        else if(clientSecurity == RFB::SECURITY_TYPE_VNC)
        {
            if(! clientAuthVnc())
                return EXIT_FAILURE;

            sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
        }
        else if(clientSecurity == RFB::SECURITY_TYPE_VENCRYPT)
        {
            if(! clientAuthVenCrypt())
                return EXIT_FAILURE;

            encryptionInfo = tls->sessionDescription();
            sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
        }
        else
        {
            const std::string err("no matching security types");
            sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(err.size()).sendString(err).sendFlush();
            Application::error("error: %s", err.c_str());
            return EXIT_FAILURE;
        }

        // RFB 6.3.1 client init
        int clientSharedFlag = recvInt8();
        Application::debug("RFB 6.3.1, client shared: 0x%02x", clientSharedFlag);
        // RFB 6.3.2 server init
        auto wsz = _xcbDisplay->size();
        sendIntBE16(wsz.width);
        sendIntBE16(wsz.height);
        Application::debug("server send: pixel format, bpp: %d, depth: %d, bigendian: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                           serverFormat.bitsPerPixel, _xcbDisplay->depth(), big_endian,
                           serverFormat.redMax, serverFormat.redShift, serverFormat.greenMax, serverFormat.greenShift, serverFormat.blueMax, serverFormat.blueShift);
        clientFormat = serverFormat;
        // send pixel format
        sendInt8(serverFormat.bitsPerPixel);
        sendInt8(_xcbDisplay->depth());
        sendInt8(big_endian);
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
        const std::string desktopName("X11 Remote Desktop");
        sendIntBE32(desktopName.size()).sendString(desktopName).sendFlush();
        Application::info("connector starting: %s", "wait RFB messages...");
        // xcb on
        setEnableXcbMessages(true);
        XCB::Region damageRegion(0, 0, 0, 0);
        bool clientUpdateReq = false;
        bool nodamage = _config->getBoolean("xcb:nodamage", false);
        std::vector<uint8_t> selbuf;

        while(loopMessage)
        {
            // RFB: mesage loop
            if(hasInput())
            {
                int msgType = recvInt8();

                switch(msgType)
                {
                    case RFB::CLIENT_SET_PIXEL_FORMAT:
                        clientSetPixelFormat();
                        damageRegion = _xcbDisplay->region();
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_SET_ENCODINGS:
                        if(clientSetEncodings())
                        {
                            // full update
                            damageRegion = _xcbDisplay->region();
                            clientUpdateReq = true;
                        }

                        break;

                    case RFB::CLIENT_REQUEST_FB_UPDATE:

                        // full update
                        if(clientFramebufferUpdate())
                            damageRegion = _xcbDisplay->region();

                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_EVENT_KEY:
                        clientKeyEvent();
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_EVENT_POINTER:
                        clientPointerEvent();
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_CUT_TEXT:
                        clientCutTextEvent();
                        clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_SET_DESKTOP_SIZE:
                        clientSetDesktopSizeEvent();
                        //clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_ENABLE_CONTINUOUS_UPDATES:
                        clientEnableContinuousUpdates();
                        break;

                    default:
                        throw std::runtime_error(std::string("RFB unknown message: ").append(Tools::hex(msgType, 2)));
                }
            }

            if(isAllowXcbMessages())
            {
                if(auto err = _xcbDisplay->hasError())
                {
                    setEnableXcbMessages(false);
                    Application::error("xcb display error connection: %d", err);
                    break;
                }

                // get all damages and join it
                while(auto ev = _xcbDisplay->poolEvent())
                {
                    int shmOpcode = _xcbDisplay->eventErrorOpcode(ev, XCB::Module::SHM);

                    if(0 <= shmOpcode)
                    {
                        _xcbDisplay->extendedError(ev.toerror(), "SHM extension");
                        loopMessage = false;
                        break;
                    }

                    if(_xcbDisplay->isDamageNotify(ev))
                    {
                        auto notify = reinterpret_cast<xcb_damage_notify_event_t*>(ev.get());
                        damageRegion.join(notify->area);
                    }
                    else if(_xcbDisplay->isRandrCRTCNotify(ev))
                    {
                        auto notify = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());
                        xcb_randr_crtc_change_t cc = notify->u.cc;

                        if(0 < cc.width && 0 < cc.height)
                        {
                            if(DesktopResizeMode::Undefined != desktopResizeMode &&
                               DesktopResizeMode::Disabled != desktopResizeMode &&
                               (screensInfo.empty() || (screensInfo.front().width != cc.width || screensInfo.front().height != cc.height)))
                            {
                                screensInfo.push_back({ .width = cc.width, .height = cc.height });
                                desktopResizeMode = DesktopResizeMode::ServerInform;
                            }
                        }
                    }
                    else if(_xcbDisplay->isSelectionNotify(ev))
                    {
                        auto notify = reinterpret_cast<xcb_selection_notify_event_t*>(ev.get());

                        if(_xcbDisplay->selectionNotifyAction(notify))
                            selbuf = _xcbDisplay->getSelectionData();
                    }
                }

                if(nodamage)
                {
                    damageRegion = _xcbDisplay->region();
                    clientUpdateReq = true;
                }
                else if(! damageRegion.empty())
                    // fix out of screen
                    damageRegion = _xcbDisplay->region().intersected(damageRegion.align(4));

                // server action
                if(! isUpdateProcessed())
                {
                    if(DesktopResizeMode::Undefined != desktopResizeMode &&
                       DesktopResizeMode::Disabled != desktopResizeMode && DesktopResizeMode::Success != desktopResizeMode)
                    {
                        serverSendDesktopSize(desktopResizeMode, isAllowXcbMessages());
                        desktopResizeMode = DesktopResizeMode::Success;
                    }

                    if(sendBellFlag)
                    {
                        serverSendBell();
                        sendBellFlag = false;
                    }

                    if(selbuf.size())
                    {
                        serverSendCutText(selbuf);
                        selbuf.clear();
                    }

                    if(clientUpdateReq && ! damageRegion.empty())
                    {
                        XCB::Region res;

                        if(XCB::Region::intersection(clientRegion, damageRegion, & res))
                        {
                            fbUpdateProcessing = true;
                            // background job
                            std::thread([=]()
                            {
                                try
                                {
                                    this->serverSendFrameBufferUpdate(res);
                                }
                                catch(const std::exception & err)
                                {
                                    Application::error("exception: %s", err.what());
                                    loopMessage = false;
                                }
                                catch(...)
                                {
                                    loopMessage = false;
                                }
                            }).detach();
                        }

                        damageRegion.reset();
                        clientUpdateReq = false;
                    }
                }
            }

            // wait
            std::this_thread::sleep_for(1ms);
        }

        return EXIT_SUCCESS;
    }

    bool Connector::VNC::isUpdateProcessed(void) const
    {
        return fbUpdateProcessing || ! jobsEncodings.empty();
    }

    void Connector::VNC::waitSendingFBUpdate(void) const
    {
        while(isUpdateProcessed())
            std::this_thread::sleep_for(1ms);
    }

    void Connector::VNC::clientSetPixelFormat(void)
    {
        waitSendingFBUpdate();
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
        Application::notice("RFB 6.4.1, set pixel format, bpp: %d, depth: %d, be: %d, truecol: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                            bitsPerPixel, depth, bigEndian, trueColor, redMax, redShift, greenMax, greenShift, blueMax, blueShift);

        switch(bitsPerPixel >> 3)
        {
            case 4:
            case 2:
            case 1:
                break;

            default:
                throw std::runtime_error("clientSetPixelFormat: unknown pixel format");
        }

        if(trueColor == 0 || redMax == 0 || greenMax == 0 || blueMax == 0)
            throw std::runtime_error("clientSetPixelFormat: unsupported pixel format");

        clientTrueColor = trueColor;
        clientBigEndian = bigEndian;
        clientFormat = PixelFormat(bitsPerPixel, redMax, greenMax, blueMax, 0, redShift, greenShift, blueShift, 0);

        if(colourMap.size()) colourMap.clear();
    }

    bool Connector::VNC::clientSetEncodings(void)
    {
        waitSendingFBUpdate();
        // RFB: 6.4.2
        // skip padding
        recvSkip(1);
        int previousType = prefEncodingsPair.second;
        int numEncodings = recvIntBE16();
        Application::notice("RFB 6.4.2, set encodings, counts: %d", numEncodings);
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
                    Application::warning("RFB request encodings: %s (disabled)", RFB::encodingName(encoding));
                    continue;
                }
            }

            clientEncodings.push_back(encoding);
            const char* name = RFB::encodingName(encoding);

            if(0 == std::strcmp(name, "unknown"))
                Application::info("RFB request encodings: 0x%08x", encoding);
            else
                Application::info("RFB request encodings: %s", RFB::encodingName(encoding));
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
                    Application::debug("server pref encodings: 0x%08x", enc);
                else
                    Application::debug("server pref encodings: %s", RFB::encodingName(enc));
            }
        }

        prefEncodingsPair = selectEncodings();
        Application::notice("server select encoding: %s", RFB::encodingName(prefEncodingsPair.second));

        if(std::any_of(clientEncodings.begin(), clientEncodings.end(),
                       [=](auto & val) { return val == RFB::ENCODING_CONTINUOUS_UPDATES; }))
        {
            // RFB 1.7.7.15
            // The server must send a EndOfContinuousUpdates message the first time
            // it sees a SetEncodings message with the ContinuousUpdates pseudo-encoding,
            // in order to inform the client that the extension is supported.
            //
            // serverSendEndContinuousUpdates();
        }
        return previousType != prefEncodingsPair.second;
    }

    bool Connector::VNC::clientFramebufferUpdate(void)
    {
        // RFB: 6.4.3
        int incremental = recvInt8();
        clientRegion.x = recvIntBE16();
        clientRegion.y = recvIntBE16();
        clientRegion.width = recvIntBE16();
        clientRegion.height = recvIntBE16();

        if(0)
        {
            Application::debug("RFB 6.4.3, request update fb, region [%d, %d, %d, %d], incremental: %d",
                               clientRegion.x, clientRegion.y, clientRegion.width, clientRegion.height, incremental);
        }

        bool fullUpdate = incremental == 0;
        auto serverRegion = _xcbDisplay->region();

        if(fullUpdate)
        {
            clientRegion = serverRegion;

            if(desktopResizeMode == DesktopResizeMode::Undefined &&
               std::any_of(clientEncodings.begin(), clientEncodings.end(),
                           [=](auto & val) { return  val == RFB::ENCODING_EXT_DESKTOP_SIZE; }))
            {
                desktopResizeMode = DesktopResizeMode::ServerInform;
            }
        }
        else
        {
            clientRegion = serverRegion.intersected(clientRegion);

            if(clientRegion.empty())
                Application::warning("client region intersection with display [%d, %d] failed", serverRegion.width, serverRegion.height);
        }

        return fullUpdate;
    }

    void Connector::VNC::clientKeyEvent(void)
    {
        // RFB: 6.4.4
        int pressed = recvInt8();
        recvSkip(2);
        int keysym = recvIntBE32();
        Application::debug("RFB 6.4.4, key event (%s), keysym: 0x%08x", (pressed ? "pressed" : "released"), keysym);

        if(isAllowXcbMessages())
        {
            // local keymap priority "vnc:keymap:file"
            if(auto value = (keymap ? keymap->getValue(Tools::hex(keysym, 8)) : nullptr))
            {
                if(value->isArray())
                {
                    auto ja = static_cast<const JsonArray*>(value);

                    for(auto keycode : ja->toStdVector<int>())
                        _xcbDisplay->fakeInputKeycode(value->getInteger(), 0 < pressed);
                }
                else
                    _xcbDisplay->fakeInputKeycode(value->getInteger(), 0 < pressed);
            }
            else
                _xcbDisplay->fakeInputKeysym(keysym, 0 < pressed);
        }
    }

    void Connector::VNC::clientPointerEvent(void)
    {
        // RFB: 6.4.5
        int mask = recvInt8(); // button1 0x01, button2 0x02, button3 0x04
        int posx = recvIntBE16();
        int posy = recvIntBE16();

        if(0)
            Application::debug("RFB 6.4.5, pointer event, mask: 0x%02x, posx: %d, posy: %d", mask, posx, posy);

        if(isAllowXcbMessages())
        {
            if(this->pressedMask ^ mask)
            {
                for(int num = 0; num < 8; ++num)
                {
                    int bit = 1 << num;

                    if(bit & mask)
                    {
                        if(1 < encodingDebug)
                            Application::debug("xfb fake input pressed: %d", num + 1);

                        _xcbDisplay->fakeInputTest(XCB_BUTTON_PRESS, num + 1, posx, posy);
                        this->pressedMask |= bit;
                    }
                    else if(bit & pressedMask)
                    {
                        if(1 < encodingDebug)
                            Application::debug("xfb fake input released: %d", num + 1);

                        _xcbDisplay->fakeInputTest(XCB_BUTTON_RELEASE, num + 1, posx, posy);
                        this->pressedMask &= ~bit;
                    }
                }
            }
            else
            {
                if(1 < encodingDebug)
                    Application::debug("xfb fake input move, posx: %d, posy: %d", posx, posy);

                _xcbDisplay->fakeInputTest(XCB_MOTION_NOTIFY, 0, posx, posy);
            }
        }
    }

    void Connector::VNC::clientCutTextEvent(void)
    {
        // RFB: 6.4.6
        // skip padding
        recvSkip(3);
        size_t length = recvIntBE32();
        Application::debug("RFB 6.4.6, cut text event, length: %d", length);

        if(isAllowXcbMessages())
        {
            size_t maxreq = _xcbDisplay->getMaxRequest();
            size_t chunk = std::min(maxreq, length);
            std::vector<uint8_t> buffer;
            buffer.reserve(chunk);

            for(size_t pos = 0; pos < chunk; ++pos)
                buffer.push_back(recvInt8());

            recvSkip(length - chunk);
            _xcbDisplay->setClipboardEvent(buffer);
        }
        else
            recvSkip(length);
    }

    void Connector::VNC::clientEnableContinuousUpdates(void)
    {
        int enable = recvInt8();
        int regx = recvIntBE16();
        int regy = recvIntBE16();
        int regw = recvIntBE16();
        int regh = recvIntBE16();
        Application::notice("RFB 1.7.4.7, enable continuous updates, region: [%d,%d,%d,%d], enabled: %d", regx, regy, regw, regh, enable);
        throw std::runtime_error("clientEnableContinuousUpdates: not implemented");
    }

    void Connector::VNC::clientSetDesktopSizeEvent(void)
    {
        // skip padding (one byte!)
        recvSkip(1);
        int width = recvIntBE16();
        int height = recvIntBE16();
        int numOfScreens = recvInt8();
        recvSkip(1);
        Application::notice("RFB 1.7.4.10, set desktop size event, size: %dx%d, screens: %d", width, height, numOfScreens);
        screensInfo.resize(numOfScreens);

        // screens array
        for(auto & info : screensInfo)
        {
            info.id = recvIntBE32();
            info.xpos = recvIntBE16();
            info.ypos = recvIntBE16();
            info.width = recvIntBE16();
            info.height = recvIntBE16();
            info.flags = recvIntBE32();
        }

        desktopResizeMode = DesktopResizeMode::ClientRequest;
    }

    void Connector::VNC::serverSendColourMap(int first)
    {
        const std::lock_guard<std::mutex> lock(sendGlobal);
        Application::notice("server send: colour map, first: %d, colour map length: %d", first, colourMap.size());
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

    void Connector::VNC::serverSendBell(void)
    {
        const std::lock_guard<std::mutex> lock(sendGlobal);
        Application::notice("server send: %s", "bell");
        // RFB: 6.5.3
        sendInt8(RFB::SERVER_BELL);
        sendFlush();
    }

    void Connector::VNC::serverSendCutText(const std::vector<uint8_t> & buf)
    {
        const std::lock_guard<std::mutex> lock(sendGlobal);
        Application::info("server send: cut text, length: %d", buf.size());
        // RFB: 6.5.4
        sendInt8(RFB::SERVER_CUT_TEXT);
        sendInt8(0); // padding
        sendInt8(0); // padding
        sendInt8(0); // padding
        sendIntBE32(buf.size());
        sendRaw(buf.data(), buf.size());
        sendFlush();
    }

    void Connector::VNC::serverSendEndContinuousUpdates(void)
    {
        // RFB: 1.7.5.5
        sendInt8(RFB::CLIENT_ENABLE_CONTINUOUS_UPDATES).sendFlush();
    }

    bool Connector::VNC::serverSendFrameBufferUpdate(const XCB::Region & reg)
    {
        const std::lock_guard<std::mutex> lock(sendGlobal);

        if(auto reply = _xcbDisplay->copyRootImageRegion(reg))
        {
            const int bytePerPixel = _xcbDisplay->pixmapBitsPerPixel(reply->depth()) >> 3;

            if(encodingDebug)
            {
                if(const xcb_visualtype_t* visual = _xcbDisplay->visual(reply->visId()))
                {
                    Application::debug("shm request size [%d, %d], reply: length: %d, depth: %d, bits per rgb value: %d, red: %08x, green: %08x, blue: %08x, color entries: %d",
                                       reg.width, reg.height, reply->size(), reply->depth(), visual->bits_per_rgb_value, visual->red_mask,
                                       visual->green_mask, visual->blue_mask, visual->colormap_entries);
                }
            }

            if(0)
                Application::debug("server send fb update: [%d, %d, %d, %d]", reg.x, reg.y, reg.width, reg.height);

            // fix align
            if(reply->size() != reg.width * reg.height * bytePerPixel)
                throw std::runtime_error("serverSendFrameBufferUpdate: region not aligned");

            // RFB: 6.5.1
            sendInt8(RFB::SERVER_FB_UPDATE);
            // padding
            sendInt8(0);
            FrameBuffer frameBuffer(reply->data(), reg, serverFormat);
            // apply render primitives
            int encodingLength = 0;
            // send encodings
            size_t netStatTx2 = netStatTx;
            prefEncodingsPair.first(frameBuffer);

            if(encodingDebug)
            {
                size_t rawLength = 14 /* raw header for one region */ + reg.width * reg.height * clientFormat.bytePerPixel();
                double optimize = 100.0f - (netStatTx - netStatTx2) * 100 / static_cast<double>(rawLength);
                Application::debug("encoding %s optimize: %.*f%% (send: %d, raw: %d), region(%d, %d)", RFB::encodingName(prefEncodingsPair.second), 2, optimize, encodingLength, rawLength, reg.width, reg.height);
            }

            _xcbDisplay->damageSubtrack(reg);
            sendFlush();
        }
        else
            Application::error("%s: failed", __FUNCTION__);

        fbUpdateProcessing = false;
        return true;
    }

    int Connector::VNC::sendPixel(uint32_t pixel)
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
            Application::error("%s", "not usable");

        throw std::runtime_error("sendPixel: unknown format");
        return 0;
    }

    int Connector::VNC::sendCPixel(uint32_t pixel)
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

    int Connector::VNC::sendRunLength(size_t length)
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

    void Connector::VNC::zlibDeflateStart(size_t len)
    {
        if(! zlib)
            zlib.reset(new ZLib::DeflateStream());

        zlib->prepareSize(len);
        streamOut = zlib.get();
    }

    std::vector<uint8_t> Connector::VNC::zlibDeflateStop(void)
    {
        streamOut = tls ? tls.get() : socket.get();
        return zlib->syncFlush();
    }
}
