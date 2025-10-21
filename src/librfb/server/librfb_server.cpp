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
#include <cstring>
#include <fstream>
#include <algorithm>

#include "ltsm_application.h"
#include "librfb_server.h"

#ifdef LTSM_ENCODING_FFMPEG
#include "librfb_ffmpeg.h"
#endif

using namespace std::chrono_literals;

namespace LTSM {
    // ClintEncodings
    void ClientEncodings::setPriority(const std::vector<int> & priorities) {
        encs.remove_if([&](auto & enc) {
            return std::any_of(priorities.begin(), priorities.end(),
            [&](auto & val) {
                return val == enc;
            });
        });

        encs.insert(encs.begin(), priorities.begin(), priorities.end());
    }

    bool ClientEncodings::isPresent(int type) const {
        return std::any_of(encs.begin(), encs.end(), [ = ](auto & val) {
            return val == type;
        });
    }

    int ClientEncodings::findPriorityFrom(std::initializer_list<int> priorities) const {
        for(const auto & enc : encs) {
            if(std::any_of(priorities.begin(), priorities.end(),
            [&](auto & val) {
            return val == enc;
        })) {
                return enc;
            }
        }

        return RFB::ENCODING_RAW;
    }

    // ServerEncoder
    RFB::ServerEncoder::ServerEncoder(int sockfd) {
        if(0 < sockfd) {
            socket = std::make_unique<SocketStream>(sockfd);
        } else {
            socket = std::make_unique<InetStream>();
        }

        streamIn = streamOut = socket.get();
    }

    void RFB::ServerEncoder::sendFlush(void) {
        try {
            if(rfbMessages) {
                streamOut->sendFlush();
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            rfbMessagesShutdown();
        }
    }

    void RFB::ServerEncoder::sendRaw(const void* ptr, size_t len) {
        try {
            if(rfbMessages) {
                streamOut->sendRaw(ptr, len);
                netStatTx += len;
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            rfbMessagesShutdown();
        }
    }

    void RFB::ServerEncoder::recvRaw(void* ptr, size_t len) const {
        try {
            if(rfbMessages) {
                streamIn->recvRaw(ptr, len);
                netStatRx += len;
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            const_cast<ServerEncoder*>(this)->rfbMessagesShutdown();
        }
    }

    bool RFB::ServerEncoder::hasInput(void) const {
        try {
            if(rfbMessages) {
                return streamIn->hasInput();
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            const_cast<ServerEncoder*>(this)->rfbMessagesShutdown();
        }

        return false;
    }

    size_t RFB::ServerEncoder::hasData(void) const {
        try {
            if(rfbMessages) {
                return streamIn->hasData();
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            const_cast<ServerEncoder*>(this)->rfbMessagesShutdown();
        }

        return 0;
    }

    uint8_t RFB::ServerEncoder::peekInt8(void) const {
        try {
            if(rfbMessages) {
                return streamIn->peekInt8();
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            const_cast<ServerEncoder*>(this)->rfbMessagesShutdown();
        }

        return 0;
    }

    bool RFB::ServerEncoder::isUpdateProcessed(void) const {
        return fbUpdateProcessing;
    }

    void RFB::ServerEncoder::waitUpdateProcess(void) {
        while(isUpdateProcessed()) {
            std::this_thread::sleep_for(5ms);
        }
    }

#ifdef LTSM_WITH_GNUTLS
    bool RFB::ServerEncoder::authVncInit(const std::string & passwdFile) {
        std::vector<uint8_t> challenge = TLS::randomKey(16);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto tmp = Tools::buffer2hexstring(challenge.begin(), challenge.end(), 2);
            Application::debug(DebugType::Rfb, "%s: challenge: %s", __FUNCTION__, tmp.c_str());
        }

        sendRaw(challenge.data(), challenge.size());
        sendFlush();
        auto response = recvData(16);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto tmp = Tools::buffer2hexstring(response.begin(), response.end(), 2);
            Application::debug(DebugType::Rfb, "%s: response: %s", __FUNCTION__, tmp.c_str());
        }

        std::ifstream ifs(passwdFile, std::ifstream::in);

        while(ifs.good()) {
            std::string pass;
            std::getline(ifs, pass);
            auto crypt = TLS::encryptDES(challenge, pass);

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto tmp = Tools::buffer2hexstring(crypt.begin(), crypt.end(), 2);
                Application::debug(DebugType::Rfb, "%s: encrypt: %s", __FUNCTION__, tmp.c_str());
            }

            if(crypt == response) {
                return true;
            }
        }

        const std::string err("password mismatch");
        sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(err.size()).sendString(err).sendFlush();
        Application::error("%s: %s, passwd file: %s", __FUNCTION__, err.c_str(), passwdFile.c_str());
        return false;
    }

    bool RFB::ServerEncoder::authVenCryptInit(const SecurityInfo & secInfo) {
        // VenCrypt version
        sendInt8(0).sendInt8(2).sendFlush();
        // client req
        int majorVer = recvInt8();
        int minorVer = recvInt8();
        Application::debug(DebugType::Rfb, "%s: client vencrypt version %d.%d", __FUNCTION__, majorVer, minorVer);

        if(majorVer != 0 || (minorVer < 1 || minorVer > 2)) {
            // send unsupported
            sendInt8(255).sendFlush();
            Application::error("%s: unsupported vencrypt version %d.%d", __FUNCTION__, majorVer, minorVer);
            return false;
        }

        // send supported
        sendInt8(0);
        bool x509Mode = false;

        if(minorVer == 1) {
            if(secInfo.tlsAnonMode) {
                sendInt8(1).sendInt8(RFB::SECURITY_VENCRYPT01_TLSNONE).sendFlush();
            } else {
                sendInt8(2).sendInt8(RFB::SECURITY_VENCRYPT01_TLSNONE).sendInt8(RFB::SECURITY_VENCRYPT01_X509NONE).sendFlush();
            }

            int mode = recvInt8();
            Application::debug(DebugType::Rfb, "%s: client choice vencrypt mode: %d", __FUNCTION__, mode);

            switch(mode) {
                case RFB::SECURITY_VENCRYPT01_TLSNONE:
                    break;

                case RFB::SECURITY_VENCRYPT01_X509NONE:
                    if(secInfo.tlsAnonMode) {
                        Application::error("%s: unsupported vencrypt mode: %s", __FUNCTION__, "x509");
                        return false;
                    }

                    x509Mode = true;
                    break;

                default:
                    Application::error("%s: unsupported vencrypt mode: %d", __FUNCTION__, mode);
                    return false;
            }
        } else
            // if(minorVer == 2)
        {
            if(secInfo.tlsAnonMode) {
                sendInt8(1).sendIntBE32(RFB::SECURITY_VENCRYPT02_TLSNONE).sendFlush();
            } else {
                sendInt8(2).sendIntBE32(RFB::SECURITY_VENCRYPT02_TLSNONE).sendIntBE32(RFB::SECURITY_VENCRYPT02_X509NONE).sendFlush();
            }

            int mode = recvIntBE32();
            Application::debug(DebugType::Rfb, "%s: client choice vencrypt mode: %d", __FUNCTION__, mode);

            switch(mode) {
                case RFB::SECURITY_VENCRYPT02_TLSNONE:
                    break;

                case RFB::SECURITY_VENCRYPT02_X509NONE:
                    if(secInfo.tlsAnonMode) {
                        Application::error("%s: unsupported vencrypt mode: %s", __FUNCTION__, "x509");
                        return false;
                    }

                    x509Mode = true;
                    break;

                default:
                    Application::error("%s: unsupported vencrypt mode: %d", __FUNCTION__, mode);
                    return false;
            }
        }

        if(x509Mode) {
            const std::string* errFile = nullptr;
            std::error_code fserr;

            if(! std::filesystem::exists(secInfo.caFile, fserr)) {
                errFile = &secInfo.caFile;
            }

            if(! std::filesystem::exists(secInfo.certFile, fserr)) {
                errFile = &secInfo.certFile;
            }

            if(! std::filesystem::exists(secInfo.keyFile, fserr)) {
                errFile = &secInfo.keyFile;
            }

            if(errFile) {
                Application::error("%s: file not found: %s", __FUNCTION__, errFile->c_str());
                sendInt8(0).sendFlush();
                return false;
            }
        }

        sendInt8(1).sendFlush();

        try {
            if(x509Mode)
                tls = std::make_unique<TLS::X509Session>(socket.get(), secInfo.caFile, secInfo.certFile, secInfo.keyFile,
                      secInfo.crlFile, secInfo.tlsPriority, true, secInfo.tlsDebug);
            else {
                tls = std::make_unique<TLS::AnonSession>(socket.get(), secInfo.tlsPriority, true, secInfo.tlsDebug);
            }
        } catch(gnutls::exception & err) {
            Application::error("gnutls error: %s, code: %d", err.what(), err.get_code());
            return false;
        }

        socket->useStatistic(false);
        streamIn = streamOut = tls.get();
        return true;
    }
#endif

    int RFB::ServerEncoder::serverHandshakeVersion(void) {
        // RFB 6.1.1 version
        int protover = RFB::VERSION_MAJOR * 10 + RFB::VERSION_MINOR;
        auto version = Tools::joinToString("RFB 00", RFB::VERSION_MAJOR, ".00", RFB::VERSION_MINOR, "\n");
        sendString(version).sendFlush();
        std::string magick = recvString(12);
        Application::debug(DebugType::Rfb, "%s: handshake version %s", __FUNCTION__, magick.c_str());

        if(magick == "RFB 003.003\n") {
            protover = 33;
        } else if(magick == "RFB 003.007\n") {
            protover = 37;
        } else if(magick != version) {
            Application::error("%s: handshake failure, unknown magic: %s", __FUNCTION__, magick.c_str());
            return 0;
        }

        return protover;
    }

    bool RFB::ServerEncoder::serverSecurityInit(int protover, const SecurityInfo & secInfo) {
        // RFB 6.1.2 security
        if(protover == 33) {
            uint32_t res = 0;

            if(secInfo.authVnc) {
                res |= RFB::SECURITY_TYPE_VNC;
            }

            if(secInfo.authNone) {
                res |= RFB::SECURITY_TYPE_NONE;
            }

            sendIntBE32(res);
        } else {
            std::vector<uint8_t> res;
#ifdef LTSM_WITH_GSSAPI
            res.push_back(SECURITY_TYPE_GSSAPI);
#endif

#ifdef LTSM_WITH_GNUTLS

            if(secInfo.authVenCrypt) {
                res.push_back(RFB::SECURITY_TYPE_VENCRYPT);
            }

            if(secInfo.authVnc) {
                res.push_back(RFB::SECURITY_TYPE_VNC);
            }

#endif

            if(noVncMode()) {
                res.clear();
            }

            if(secInfo.authNone) {
                res.push_back(RFB::SECURITY_TYPE_NONE);
            }

            sendInt8(res.size());

            if(res.empty()) {
                Application::error("%s: server security invalid", __FUNCTION__);
                sendFlush();
                return false;
            }

            sendData(res);
        }

        sendFlush();

        // unsupported
        if(protover == 33) {
            return true;
        }

        int clientSecurity = recvInt8();
        Application::debug(DebugType::Rfb, "%s, client security: 0x%02x", __FUNCTION__, clientSecurity);

        if(protover == 38 || clientSecurity != RFB::SECURITY_TYPE_NONE) {
            // RFB 6.1.3 security result
            if(clientSecurity == RFB::SECURITY_TYPE_NONE && secInfo.authNone) {
                sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
            }

#ifdef LTSM_WITH_GNUTLS
            else if(clientSecurity == RFB::SECURITY_TYPE_VNC && secInfo.authVnc) {
                if(secInfo.passwdFile.empty()) {
                    Application::error("%s: passwd file not defined", __FUNCTION__);
                    sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                    return false;
                }

                std::error_code err;

                if(! std::filesystem::exists(secInfo.passwdFile, err)) {
                    Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"),
                                       secInfo.passwdFile.c_str(), getuid());
                    sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                    return false;
                }

                if(! authVncInit(secInfo.passwdFile)) {
                    sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                    return false;
                }

                sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
            } else if(clientSecurity == RFB::SECURITY_TYPE_VENCRYPT && secInfo.authVenCrypt) {
                if(! authVenCryptInit(secInfo)) {
                    sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(0).sendFlush();
                    return false;
                }

                sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
            }

#endif
#ifdef LTSM_WITH_GSSAPI
            else if(clientSecurity == SECURITY_TYPE_GSSAPI) {
                try {
                    auto krb = std::make_unique<GssApi::Server>(socket.get());
                    Application::info("%s: kerberos service: `%s'", __FUNCTION__, secInfo.krb5Service.c_str());

                    if(krb->handshakeLayer(secInfo.krb5Service)) {
                        auto remoteName = Gss::displayName(krb->securityContext()->name);
                        std::unique_ptr<JsonObject> jo;

                        if(auto len = krb->recvIntBE32(); 0 < len) {
                            auto raw = krb->recvData(len);
                            jo = std::make_unique<JsonObject>(JsonContentString(std::string(raw.begin(), raw.end())).toObject());
                        }

                        // stop kerbero session
                        krb.reset();
                        Application::info("%s: kerberos auth: %s, remote: %s", __FUNCTION__, "success", remoteName.c_str());

                        if(auto pos = remoteName.find("@"); pos != std::string::npos) {
                            clientAuthName = remoteName.substr(0, pos);
                            clientAuthDomain = remoteName.substr(pos + 1);
                        } else {
                            clientAuthName = remoteName;
                        }

                        // check json info
                        if(jo) {
                            auto tls = jo->getBoolean("continue:tls", false);

                            if(tls && ! authVenCryptInit(secInfo)) {
                                return false;
                            }
                        }

                        sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
                        return true;
                    }
                } catch(const std::exception & err) {
                    LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
                }

                const std::string err("security kerberos failed");
                sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(err.size()).sendString(err).sendFlush();
                Application::error("%s: error: %s", __FUNCTION__, err.c_str());
                return false;
            }

#endif
            else {
                const std::string err("no matching security types");
                sendIntBE32(RFB::SECURITY_RESULT_ERR).sendIntBE32(err.size()).sendString(err).sendFlush();
                Application::error("%s: error: %s", __FUNCTION__, err.c_str());
                return false;
            }
        }

        return true;
    }

    void RFB::ServerEncoder::serverClientInit(std::string_view desktopName, const XCB::Size & displaySize, int displayDepth,
            const PixelFormat & pf) {
        // RFB 6.3.1 client init
        int clientSharedFlag = recvInt8();
        Application::debug(DebugType::Rfb, "%s: client shared: 0x%02x", __FUNCTION__, clientSharedFlag);
        // RFB 6.3.2 server init
        sendIntBE16(displaySize.width);
        sendIntBE16(displaySize.height);
        Application::notice("%s: server pf - bpp: %" PRIu8 ", depth: %d, bigendian: %d, red(%" PRIu16 ",%" PRIu8 "), green(%" PRIu16 ",%"
                            PRIu8 "), blue(%" PRIu16 ",%" PRIu8 ")",
                            __FUNCTION__, pf.bitsPerPixel(), displayDepth, (int) platformBigEndian(),
                            pf.rmax(), pf.rshift(), pf.gmax(), pf.gshift(), pf.bmax(), pf.bshift());
        clientPf = serverFormat();
        // send pixel format
        sendInt8(pf.bitsPerPixel());
        sendInt8(displayDepth);
        sendInt8(platformBigEndian() ? 1 : 0);
        // true color
        sendInt8(1);
        sendIntBE16(pf.rmax());
        sendIntBE16(pf.gmax());
        sendIntBE16(pf.bmax());
        sendInt8(pf.rshift());
        sendInt8(pf.gshift());
        sendInt8(pf.bshift());
        // send padding
        sendInt8(0);
        sendInt8(0);
        sendInt8(0);
        // send name desktop
        sendIntBE32(desktopName.size()).sendString(desktopName).sendFlush();
    }

    bool RFB::ServerEncoder::sendUpdateSafe(const XCB::Region & area) {
        fbUpdateProcessing = true;
        bool res = false;

        try {
            auto reply = serverFrameBuffer(area);

            if(sendFrameBufferUpdate(reply.fb)) {
                serverSendFBUpdateEvent(area);
                res = true;
            }
        } catch(const std::exception & err) {
            Application::error("%s: vnc exception: %s", __FUNCTION__, err.what());
        } catch(...) {
        }

        fbUpdateProcessing = false;
        return res;
    }

    bool RFB::ServerEncoder::rfbMessagesRunning(void) const {
        return rfbMessages;
    }

    void RFB::ServerEncoder::rfbMessagesShutdown(void) {
        channelsShutdown();
        std::this_thread::sleep_for(100ms);
        rfbMessages = false;
    }

    void RFB::ServerEncoder::rfbMessagesLoop(void) {
        Application::debug(DebugType::Rfb, "%s: wait remote messages...", __FUNCTION__);

        while(rfbMessages) {
            if(! hasInput()) {
                std::this_thread::sleep_for(5ms);
                continue;
            }

            int msgType = recvInt8();

            if(msgType == RFB::PROTOCOL_LTSM) {
                if(! clientLtsmSupported) {
                    Application::error("%s: client not support encoding: %s", __FUNCTION__, RFB::encodingName(RFB::ENCODING_LTSM));
                    throw rfb_error(NS_FuncName);
                }

                try {
                    recvLtsmProto(*this);
                } catch(const std::runtime_error & err) {
                    Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
                    rfbMessagesShutdown();
                } catch(const std::exception & err) {
                    Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
                }

                continue;
            }

            if(! rfbMessages) {
                break;
            }

            switch(msgType) {
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
                    break;
            }
        }
    }

    void RFB::ServerEncoder::recvPixelFormat(void) {
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
        Application::notice("%s: client pf - bpp: %" PRIu8 ", depth: %" PRIu8 ", bigendian: %d, red(%" PRIu16 ",%" PRIu8 "), green(%" PRIu16
                            ",%" PRIu8 "), blue(%" PRIu16 ",%" PRIu8 ")",
                            __FUNCTION__, bitsPerPixel, depth, (int) bigEndian, redMax, redShift, greenMax, greenShift, blueMax, blueShift);

        switch(bitsPerPixel) {
            case 32:
            case 16:
            case 8:
                break;

            default: {
                Application::error("%s: %s", __FUNCTION__, " unknown pixel format");
                throw rfb_error(NS_FuncName);
            }
        }

        if(trueColor == 0 || redMax == 0 || greenMax == 0 || blueMax == 0) {
            Application::error("%s: %s", __FUNCTION__, " unsupported pixel format");
            throw rfb_error(NS_FuncName);
        }

        clientTrueColor = trueColor;
        clientBigEndian = bigEndian;
        clientPf = PixelFormat(bitsPerPixel, redMax, greenMax, blueMax, 0, redShift, greenShift, blueShift, 0);
        colourMap.clear();
        serverRecvPixelFormatEvent(clientPf, clientBigEndian);
    }

    bool RFB::ServerEncoder::clientIsBigEndian(void) const {
        return clientBigEndian;
    }

    const PixelFormat & RFB::ServerEncoder::clientFormat(void) const {
        return clientPf;
    }

    void RFB::ServerEncoder::recvSetEncodings(void) {
        waitUpdateProcess();
        // RFB: 6.4.2
        // skip padding
        recvSkip(1);
        int numEncodings = recvIntBE16();
        Application::info("%s: encoding counts: %d", __FUNCTION__, numEncodings);

        bool extendedClipboard = false;
        bool continueUpdates = false;
        auto disabledEncodings = serverDisabledEncodings();

        std::vector<int> recvEncodings;
        recvEncodings.reserve(numEncodings);

        while(0 < numEncodings--) {
            int encoding = recvIntBE32();

            if(! disabledEncodings.empty()) {
                auto enclower = Tools::lower(RFB::encodingName(encoding));

                if(std::any_of(disabledEncodings.begin(), disabledEncodings.end(), [&](auto & str) {
                return enclower == Tools::lower(str);
                })) {
                    Application::warning("%s: request encodings: %s (disabled)", __FUNCTION__, RFB::encodingName(encoding));
                    continue;
                }
            }

            switch(encoding) {
                case RFB::ENCODING_LTSM:
                case RFB::ENCODING_LTSM_QOI:
                case RFB::ENCODING_LTSM_LZ4:
                case RFB::ENCODING_LTSM_TJPG:
                    clientLtsmSupported = true;
                    break;

                case RFB::ENCODING_FFMPEG_H264:
                case RFB::ENCODING_FFMPEG_AV1:
                case RFB::ENCODING_FFMPEG_VP8:
                    clientVideoSupported = true;
                    break;

                case RFB::ENCODING_CONTINUOUS_UPDATES:
                    continueUpdates = true;
                    break;

                case RFB::ENCODING_EXT_CLIPBOARD:
                    extendedClipboard = true;
                    break;

                default:
                    break;
            }

            recvEncodings.push_back(encoding);
            const char* name = RFB::encodingName(encoding);

            if(0 == std::strcmp(name, "unknown")) {
                Application::info("%s: request encodings: 0x%08x", __FUNCTION__, encoding);
            } else {
                Application::info("%s: request encodings: %s", __FUNCTION__, RFB::encodingName(encoding));
            }
        }

        clientEncodings.setPriority(recvEncodings);

        if(continueUpdates) {
            sendContinuousUpdates(true);
        }

        if(extendedClipboard) {
            // The server must send a ServerCutText message with caps set on
            // each SetEncodings message received which includes the Extended Clipboard pseudo-encoding.

            // The client may send a ClientCutText message with caps set back to indicate its capabilities.
            // Otherwise the client is assumed to support text, rtf, html, request, notify and provide and a maximum size of 20 MiB for text and 0 bytes for the other types.

            setExtClipboardRemoteCaps(ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml |
                                      ExtClipCaps::OpRequest | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide);

            setExtClipboardLocalCaps(ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml |
                                     ExtClipCaps::OpRequest | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide);

            ExtClip::remoteExtClipTypeTextSz = 20 * 1024 * 1024;
            sendExtClipboardCaps();
        }

        serverRecvSetEncodingsEvent(recvEncodings);
    }

    void RFB::ServerEncoder::recvFramebufferUpdate(void) {
        XCB::Region clientRegion;
        // RFB: 6.4.3
        int incremental = recvInt8();
        clientRegion.x = recvIntBE16();
        clientRegion.y = recvIntBE16();
        clientRegion.width = recvIntBE16();
        clientRegion.height = recvIntBE16();
        Application::debug(DebugType::Rfb, "%s: request update, region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], incremental: %d",
                           __FUNCTION__, clientRegion.x, clientRegion.y, clientRegion.width, clientRegion.height, incremental);
        serverRecvFBUpdateEvent(incremental != 0, clientRegion);
    }

    void RFB::ServerEncoder::recvKeyCode(void) {
        // RFB: 6.4.4
        bool pressed = recvInt8();
        recvSkip(2);
        uint32_t keysym = recvIntBE32();
        Application::debug(DebugType::Rfb, "%s: action %s, keysym: 0x%08" PRIx32, __FUNCTION__, (pressed ? "pressed" : "released"), keysym);
        serverRecvKeyEvent(pressed, keysym);
    }

    void RFB::ServerEncoder::recvPointer(void) {
        // RFB: 6.4.5
        // left 0x01, middle 0x02, right 0x04, scrollUp: 0x08, scrollDn: 0x10, scrollLf: 0x20, scrollRt: 0x40, back: 0x80
        uint8_t buttons = recvInt8();
        uint16_t posx = recvIntBE16();
        uint16_t posy = recvIntBE16();
        Application::debug(DebugType::Rfb, "%s: mask: 0x%02" PRIx8 ", pos: [ %" PRId16 ", %" PRId16 "]", __FUNCTION__, buttons, posx, posy);
        serverRecvPointerEvent(buttons, posx, posy);
    }

    void RFB::ServerEncoder::recvCutText(void) {
        // RFB: 6.4.6
        // skip padding
        recvSkip(3);

        // A negative value of length indicates that the extended message format is used and abs(length) is the total number of following bytes.
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        int32_t length = recvIntBE32();

        if(0 < length) {
            Application::debug(DebugType::Rfb, "%s: text length: %" PRId32 ", limit: %" PRId32, __FUNCTION__, length, localExtClipTypeTextSz);
            size_t recv = localExtClipTypeTextSz ?
                          std::min(static_cast<uint32_t>(length), localExtClipTypeTextSz) : length;
            auto buffer = recvData(recv);
            recvSkip(length - recv);
            serverRecvCutTextEvent(std::move(buffer));
        } else if(length < 0) {
            if(0 == extClipboardLocalCaps()) {
                Application::error("%s: invalid format, failed `%s'", __FUNCTION__, "ext clipboard");
                throw rfb_error(NS_FuncName);
            }

            auto buffer = recvData(std::abs(length));
            recvExtClipboardCaps(StreamBuf(std::move(buffer)));
        }
    }

    void RFB::ServerEncoder::recvSetContinuousUpdates(void) {
        int enable = recvInt8();
        int16_t regx = recvIntBE16();
        int16_t regy = recvIntBE16();
        uint16_t regw = recvIntBE16();
        uint16_t regh = recvIntBE16();
        Application::info("%s: region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], enabled: %d", __FUNCTION__, regx,
                          regy, regw, regh, enable);
        continueUpdatesProcessed = enable;
        serverRecvSetContinuousUpdatesEvent(enable, XCB::Region(regx, regy, regw, regh));
    }

    void RFB::ServerEncoder::recvSetDesktopSize(void) {
        // skip padding (one byte!)
        recvSkip(1);
        uint16_t width = recvIntBE16();
        uint16_t height = recvIntBE16();
        int numOfScreens = recvInt8();
        recvSkip(1);
        Application::info("%s: size [%" PRIu16 ", %" PRIu16 "], screens: %d", __FUNCTION__, width, height, numOfScreens);
        // screens array
        std::vector<RFB::ScreenInfo> screens;

        for(int it = 0; it < numOfScreens; it++) {
            uint32_t id = recvIntBE32();
            uint16_t posx = recvIntBE16();
            uint16_t posy = recvIntBE16();
            uint16_t width = recvIntBE16();
            uint16_t height = recvIntBE16();
            uint32_t flags = recvIntBE32();
            screens.push_back({ .id = id, .posx = posx, .posy = posy, .width = width, .height = height, .flags = flags });
        }

        serverRecvDesktopSizeEvent(screens);
    }

    void RFB::ServerEncoder::displayResizeEvent(const XCB::Size & dsz) {
        Application::info("%s: display resized, new size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, dsz.width, dsz.height);
#ifdef LTSM_ENCODING_FFMPEG
        // event background
        std::thread([this, sz = dsz]() {
            if(this->encoder &&
               (this->encoder->getType() == RFB::ENCODING_FFMPEG_H264 ||
                this->encoder->getType() == RFB::ENCODING_FFMPEG_AV1 || this->encoder->getType() == RFB::ENCODING_FFMPEG_VP8)) {
                this->encoder->resizedEvent(sz);
            }
        }).detach();

#endif
    }

    void RFB::ServerEncoder::clientDisconnectedEvent(int display) {
        Application::warning("%s: display: %d", __FUNCTION__, display);
    }

    void RFB::ServerEncoder::sendColourMap(int first) {
        Application::info("%s: first: %d, colour map length: %lu", __FUNCTION__, first, colourMap.size());
        std::scoped_lock guard{ sendLock };
        // RFB: 6.5.2
        sendInt8(RFB::SERVER_SET_COLOURMAP);
        sendInt8(0); // padding
        sendIntBE16(first); // first color
        sendIntBE16(colourMap.size());

        for(const auto & col : colourMap) {
            sendIntBE16(col.r);
            sendIntBE16(col.g);
            sendIntBE16(col.b);
        }

        sendFlush();
    }

    void RFB::ServerEncoder::sendBellEvent(void) {
        Application::info("%s: process", __FUNCTION__);
        std::scoped_lock guard{ sendLock };
        // RFB: 6.5.3
        sendInt8(RFB::SERVER_BELL);
        sendFlush();
    }

    void RFB::ServerEncoder::sendCutTextEvent(const uint8_t* buf, uint32_t len, bool ext) {
        std::scoped_lock guard{ sendLock };

        // RFB: 6.5.4
        sendInt8(RFB::SERVER_CUT_TEXT);
        sendInt8(0); // padding
        sendInt8(0); // padding
        sendInt8(0); // padding

        if(ext) {
            // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
            if(0 == extClipboardRemoteCaps()) {
                Application::error("%s: invalid format, failed `%s'", __FUNCTION__, "ext clipboard");
                throw rfb_error(NS_FuncName);
            }

            // A negative value of length indicates that the extended message format
            // is used and abs(length) is the total number of following bytes.
            sendIntBE32(static_cast<uint32_t>(0xFFFFFFFF) - len + 1);
        } else {
            Application::debug(DebugType::Rfb, "%s: length text: %" PRIu32, __FUNCTION__, len);
            sendIntBE32(len);
        }

        sendRaw(buf, len);
        sendFlush();
    }

    void RFB::ServerEncoder::sendContinuousUpdates(bool enable) {
        // RFB: 6.5.5
        Application::info("%s: status: %s", __FUNCTION__, (enable ? "enable" : "disable"));
        std::scoped_lock guard{ sendLock };
        sendInt8(RFB::SERVER_CONTINUOUS_UPDATES).sendFlush();
        continueUpdatesProcessed = enable;
    }

    bool RFB::ServerEncoder::sendFrameBufferUpdate(const FrameBuffer & fb) {
        if(! encoder) {
            Application::warning("%s: encoder null", __FUNCTION__);
            return false;
        }

        auto & reg = fb.region();
        Application::debug(DebugType::Rfb, "%s: region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y,
                           reg.width, reg.height);
        std::scoped_lock guard{ sendLock };
        // RFB: 6.5.1
        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);
        // send encodings
        encoder->sendFrameBuffer(this, fb);
        sendFlush();
        return true;
    }


    std::string RFB::ServerEncoder::serverEncryptionInfo(void) const {
#ifdef LTSM_WITH_GNUTLS
        return tls ? tls->sessionDescription() : "none";
#else
        return "unsupported";
#endif
    }

    bool RFB::ServerEncoder::isContinueUpdatesProcessed(void) const {
        return continueUpdatesProcessed;
    }

    bool RFB::ServerEncoder::isClientSupportedEncoding(int enc) const {
        return clientEncodings.isPresent(enc);
    }

    void RFB::ServerEncoder::setEncodingDebug(int v) {
        if(encoder) {
            // FIXME
            // encoder->setDebug(v);
        }
    }

    void RFB::ServerEncoder::setEncodingThreads(int threads) {
        if(threads < 1) {
            threads = 1;
        } else if(std::thread::hardware_concurrency() < threads) {
            threads = std::thread::hardware_concurrency();
            Application::error("%s: encoding threads incorrect, fixed to hardware concurrency: %d", __FUNCTION__, threads);
        }

        if(encoder) {
            Application::info("%s: using encoding threads: %d", __FUNCTION__, threads);
            encoder->setThreads(threads);
        }
    }

    int RFB::serverSelectCompatibleEncoding(const ClientEncodings & clientEncodings) {
        // server priority
        std::initializer_list<int> encs = {
#ifdef LTSM_ENCODING_FFMPEG
            RFB::ENCODING_FFMPEG_H264,
            RFB::ENCODING_FFMPEG_AV1,
            RFB::ENCODING_FFMPEG_VP8,
#endif
#ifdef LTSM_ENCODING
            RFB::ENCODING_LTSM_QOI,
            RFB::ENCODING_LTSM_LZ4,
            RFB::ENCODING_LTSM_TJPG,
#endif
            RFB::ENCODING_ZRLE, RFB::ENCODING_TRLE, RFB::ENCODING_ZLIB, RFB::ENCODING_HEXTILE,
            RFB::ENCODING_CORRE, RFB::ENCODING_RRE, RFB::ENCODING_RAW
        };

        return clientEncodings.findPriorityFrom(encs);
    }

    void RFB::ServerEncoder::serverSelectClientEncoding(void) {
        int compatible = serverSelectCompatibleEncoding(clientEncodings);

        if(encoder && encoder->getType() == compatible) {
            return;
        }

        switch(compatible) {
            case RFB::ENCODING_RAW:
                encoder = std::make_unique<EncodingRaw>();
                break;

            case RFB::ENCODING_ZLIB: {
                auto clevels = { ENCODING_COMPRESS1, ENCODING_COMPRESS2, ENCODING_COMPRESS3, ENCODING_COMPRESS4, ENCODING_COMPRESS5, ENCODING_COMPRESS6, ENCODING_COMPRESS7, ENCODING_COMPRESS8, ENCODING_COMPRESS9 };
                int zlevel = Z_BEST_SPEED;

                if(auto it = std::find_if(clevels.begin(), clevels.end(),
                [this](auto & enc) {
                return this->isClientSupportedEncoding(enc);
                }); it != clevels.end()) {
                    zlevel = ENCODING_COMPRESS1 - *it + Z_BEST_SPEED;
                }

                encoder = std::make_unique<EncodingZlib>(zlevel);
                break;
            }

            case RFB::ENCODING_HEXTILE:
                encoder = std::make_unique<EncodingHexTile>();
                break;

            case RFB::ENCODING_CORRE:
                encoder = std::make_unique<EncodingRRE>(true);
                break;

            case RFB::ENCODING_RRE:
                encoder = std::make_unique<EncodingRRE>(false);
                break;

            case RFB::ENCODING_TRLE:
                encoder = std::make_unique<EncodingTRLE>(false);
                break;

            case RFB::ENCODING_ZRLE:
                encoder = std::make_unique<EncodingTRLE>(true);
                break;
#ifdef LTSM_ENCODING_FFMPEG

            case RFB::ENCODING_FFMPEG_H264:
            case RFB::ENCODING_FFMPEG_VP8:
            case RFB::ENCODING_FFMPEG_AV1:
                encoder = std::make_unique<EncodingFFmpeg>(compatible);
                break;
#endif
#ifdef LTSM_ENCODING

            case RFB::ENCODING_LTSM_QOI:
                encoder = std::make_unique<EncodingQOI>();
                break;

            case RFB::ENCODING_LTSM_LZ4:
                encoder = std::make_unique<EncodingLZ4>();
                break;

            case RFB::ENCODING_LTSM_TJPG:
                encoder = std::make_unique<EncodingTJPG>();
                break;
#endif

            default:
                encoder = std::make_unique<EncodingRaw>();
                break;
        }

        encoderInitEvent(encoder.get());
    }

    void RFB::ServerEncoder::serverSelectEncodings(void) {
        serverSelectClientEncoding();
        Application::notice("%s: select encoding: %s", __FUNCTION__, RFB::encodingName(encoder->getType()));
        serverEncodingSelectedEvent();
    }

    /* pseudo encodings DesktopSize/Extended */
    void RFB::ServerEncoder::sendEncodingDesktopResize(const DesktopResizeStatus & status, const DesktopResizeError & error,
            const XCB::Size & desktopSize) {
        int statusCode = desktopResizeStatusCode(status);
        int errorCode = desktopResizeErrorCode(error);
        Application::info("%s: status: %d, error: %d, size [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, statusCode, errorCode,
                          desktopSize.width, desktopSize.height);

        if(! isClientSupportedEncoding(RFB::ENCODING_EXT_DESKTOP_SIZE)) {
            Application::error("%s: %s", __FUNCTION__, "client not supported ExtDesktopResize encoding");
            throw rfb_error(NS_FuncName);
        }

        // send
        std::scoped_lock guard{ sendLock };
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

    void RFB::ServerEncoder::sendEncodingRichCursor(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot) {
        // priority LTSM cursors
        if(isClientSupportedEncoding(RFB::ENCODING_LTSM_CURSOR)) {
            return sendEncodingLtsmCursor(fb, xhot, yhot);
        }

        auto & reg = fb.region();
        Application::debug(DebugType::Rfb, "%s: region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], hot: [%" PRIu16 ", %" PRIu16 "]",
                           __FUNCTION__, reg.x, reg.y, reg.width, reg.height, xhot, yhot);

        Tools::StreamBitsPack bitmask;

        const uint32_t clientAMask = ~(clientFormat().rmask() | clientFormat().gmask() | clientFormat().bmask());
        const PixelFormat clientFormatAlpha(clientFormat().bitsPerPixel(),
                                            clientFormat().rmask(), clientFormat().gmask(), clientFormat().bmask(), clientAMask);

        std::scoped_lock guard{ sendLock };
        // RFB: 6.5.1
        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);
        // regions counts
        sendIntBE16(1);
        // region size
        sendIntBE16(xhot);
        sendIntBE16(yhot);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        // region type
        sendIntBE32(RFB::ENCODING_RICH_CURSOR);

        for(int oy = 0; oy < reg.height; ++oy) {
            for(int ox = 0; ox < reg.width; ++ox) {
                auto pixel = fb.pixel(XCB::Point(ox, oy));
                auto pixel2 = fb.pixelFormat().convertTo(pixel, clientFormatAlpha);
                // part1: send pixels buf
                sendPixelRaw(pixel2, clientFormat().bytePerPixel(), clientIsBigEndian());
                bitmask.pushBit(fb.pixelFormat().alpha(pixel) == fb.pixelFormat().amax());
            }

            bitmask.pushAlign();
        }

        // The bitmask consists of left-to-right, top-to-bottom scanlines,
        // where each scanline is padded to a whole number of bytes floor((width + 7) / 8
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#cursor-pseudo-encoding
        size_t bitmaskSize = std::floor((reg.width + 7) / 8) * reg.height;
        const std::vector<uint8_t> & bitmaskBuf = bitmask.toVector();

        if(bitmaskSize != bitmaskBuf.size()) {
            Application::error("%s: bitmask missmatch, buf size: %lu, bitmask size: %lu", __FUNCTION__, bitmaskBuf.size(),
                               bitmaskSize);
            throw rfb_error(NS_FuncName);
        }

        // part2: send bitmask buf
        sendData(bitmaskBuf);
        sendFlush();
    }

    void RFB::ServerEncoder::sendEncodingLtsmCursor(const FrameBuffer & fb, uint16_t xhot, uint16_t yhot) {
        auto & reg = fb.region();
        Application::debug(DebugType::Rfb, "%s: region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], hot: [%" PRIu16 ", %" PRIu16 "]",
                           __FUNCTION__, reg.x, reg.y, reg.width, reg.height, xhot, yhot);

        std::scoped_lock guard{ sendLock };
        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);
        // rects
        sendIntBE16(1);
        sendIntBE16(xhot);
        sendIntBE16(yhot);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        sendIntBE32(ENCODING_LTSM_CURSOR);
        // cursor id
        auto rawPtr = fb.rawPtr();
        auto cursorId = rawPtr.crc32b();
        sendIntBE32(cursorId);

        // cursor rgba data
        if(std::none_of(cursorSended.begin(), cursorSended.end(),
            [&cursorId](auto & curid) { return curid == cursorId; })) {
            auto zlib = Tools::zlibCompress(rawPtr);
            // raw size
            sendIntBE32(rawPtr.size());
            // compress size
            sendIntBE32(zlib.size());
            sendData(zlib);
            cursorSended.push_front(cursorId);
        } else {
            sendIntBE32(0);
        }

        sendFlush();
    }

    void RFB::ServerEncoder::sendEncodingLtsmSupported(void) {
        Application::info("%s: server supported", __FUNCTION__);
        std::scoped_lock guard{ sendLock };
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
        sendIntBE32(0);
        sendIntBE32(LTSM::service_version);
        sendFlush();
    }

    void RFB::ServerEncoder::sendEncodingLtsmData(const uint8_t* ptr, size_t len) {
        std::scoped_lock guard{ sendLock };
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
        // raw data
        sendIntBE32(1);
        sendIntBE32(len);
        sendRaw(ptr, len);
        sendFlush();
    }

    bool RFB::ServerEncoder::isClientLtsmSupported(void) const {
        return clientLtsmSupported;
    }

    bool RFB::ServerEncoder::isClientVideoSupported(void) const {
        return clientVideoSupported;
    }

    void RFB::ServerEncoder::sendLtsmChannelData(uint8_t channel, const uint8_t* buf, size_t len) {
        if(clientLtsmSupported) {
            sendLtsmProto(*this, sendLock, channel, buf, len);
        }
    }

    void RFB::ServerEncoder::recvChannelSystem(const std::vector<uint8_t> & buf) {
        JsonContent jc;
        jc.parseBinary(reinterpret_cast<const char*>(buf.data()), buf.size());

        if(! jc.isObject()) {
            Application::error("%s: %s", __FUNCTION__, "json broken");
            throw std::invalid_argument(NS_FuncName);
        }

        auto jo = jc.toObject();
        auto cmd = jo.getString("cmd");

        if(cmd.empty()) {
            Application::error("%s: %s", __FUNCTION__, "format message broken");
            throw std::invalid_argument(NS_FuncName);
        }

        Application::debug(DebugType::Rfb, "%s: cmd: %s", __FUNCTION__, cmd.c_str());

        if(cmd == SystemCommand::ClientVariables) {
            systemClientVariables(jo);
        } else if(cmd == SystemCommand::KeyboardChange) {
            systemKeyboardChange(jo);
        } else if(cmd == SystemCommand::KeyboardEvent) {
            systemKeyboardEvent(jo);
        } else if(cmd == SystemCommand::CursorFailed) {
            systemCursorFailed(jo);
        } else if(cmd == SystemCommand::TransferFiles) {
            systemTransferFiles(jo);
        } else if(cmd == SystemCommand::ChannelClose) {
            systemChannelClose(jo);
        } else if(cmd == SystemCommand::ChannelConnected) {
            systemChannelConnected(jo);
        } else if(cmd == SystemCommand::ChannelError) {
            systemChannelError(jo);
        } else if(cmd == SystemCommand::LoginSuccess) {
            systemLoginSuccess(jo);
        } else {
            Application::error("%s: %s", __FUNCTION__, "unknown cmd");
            throw std::invalid_argument(NS_FuncName);
        }
    }

    std::pair<std::string, std::string> RFB::ServerEncoder::authInfo(void) const {
        return std::make_pair(clientAuthName, clientAuthDomain);
    }

    void RFB::ServerEncoder::setEncodingOptions(const std::forward_list<std::string> & opts) {
        if(encoder) {
            // apply opts: need full update
            if(encoder->setEncodingOptions(opts)) {
                serverScreenUpdateRequest();
            }
        }
    }

    void RFB::ServerEncoder::cursorFailed(uint32_t cursorId) {
        Application::info("%s: cursorId: 0x%08" PRIx32, __FUNCTION__, cursorId);
        cursorSended.remove(cursorId);
    }
}
