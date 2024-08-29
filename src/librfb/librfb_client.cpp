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

#include "ltsm_application.h"
#include "librfb_client.h"

#ifdef LTSM_DECODING_FFMPEG
#include "librfb_ffmpeg.h"
#endif

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
            const_cast<ClientDecoder*>(this)->rfbMessagesShutdown();
        }

        return 0;
    }

    bool RFB::ClientDecoder::authVncInit(std::string_view password)
    {
        // recv challenge 16 bytes
        auto challenge = recvData(16);
        if(Application::isDebugLevel(DebugLevel::Trace))
        {
            auto tmp = Tools::buffer2hexstring(challenge.begin(), challenge.end(), 2);
            Application::debug("%s: challenge: %s", __FUNCTION__, tmp.c_str());
        }
        auto crypt = TLS::encryptDES(challenge, password);
        if(Application::isDebugLevel(DebugLevel::Trace))
        {
            auto tmp = Tools::buffer2hexstring(crypt.begin(), crypt.end(), 2);
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

#ifdef LTSM_WITH_GSSAPI
    bool RFB::ClientDecoder::authGssApiInit(const SecurityInfo & sec)
    {
        try
        {
            auto krb = std::make_unique<GssApi::Client>(socket.get());
            // a remote peer asked for mutual authentication
            const bool mutual = true;

            if(krb->handshakeLayer(sec.krb5Service, mutual, sec.krb5Name))
            {
        	Application::info("%s: kerberos auth: %s", __FUNCTION__, "success");

                JsonObjectStream jo;
                jo.push("continue:tls", sec.authVenCrypt);
                auto json = jo.flush();

		// send security info json
                krb->sendIntBE32(json.size());
                krb->sendString(json);
                krb->sendFlush();

                // stop kerberos session
		krb.reset();

                // continue tls
                if(sec.authVenCrypt)
                    return authVenCryptInit(sec);

                return true;
            }
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }

        const std::string err("security kerberos failed");
	Application::error("%s: error: %s", __FUNCTION__, err.c_str());

	return false;
    }
#endif

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

#ifdef LTSM_WITH_GSSAPI
	Gss::CredentialPtr krb5Cred;

	if(sec.authKrb5 && std::any_of(security.begin(), security.end(), [=](auto & val){ return val == RFB::SECURITY_TYPE_GSSAPI; }))
	{
	    // check local ticket
	    if(krb5Cred = Gss::acquireUserCredential(sec.krb5Name); krb5Cred)
	    {
		auto canon = Gss::displayName(krb5Cred->name);
        	Application::info("%s: kerberos local ticket: %s", __FUNCTION__, canon.c_str());
	    }
	}

	if(krb5Cred)
        {
            Application::debug("%s: security: %s selected", __FUNCTION__, "gssapi");
            sendInt8(RFB::SECURITY_TYPE_GSSAPI).sendFlush();

            if(! authGssApiInit(sec))
		return false;
	}
        else
#endif
        if(sec.authVenCrypt && std::any_of(security.begin(), security.end(), [=](auto & val){ return val == RFB::SECURITY_TYPE_VENCRYPT; }))
        {
            Application::debug("%s: security: %s selected", __FUNCTION__, "vencrypt");
            sendInt8(RFB::SECURITY_TYPE_VENCRYPT).sendFlush();

            if(! authVenCryptInit(sec))
		return false;
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
        Application::debug("%s: remote framebuffer size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, fbWidth, fbHeight);

        // recv server pixel format
        int bpp = recvInt8();
        int depth = recvInt8();
        serverBigEndian = recvInt8();
        serverTrueColor = recvInt8();
        int rmax = recvIntBE16();
        int gmax = recvIntBE16();
        int bmax = recvIntBE16();
        int rshift = recvInt8();
        int gshift = recvInt8();
        int bshift = recvInt8();
        recvSkip(3);

        serverPf = PixelFormat(bpp, rmax, gmax, bmax, 0, rshift, gshift, bshift, 0);

        Application::debug("%s: remote pixel format: bpp: %" PRIu8 ", depth: %d, bigendian: %d, true color: %d, red(%" PRIu16 ",%" PRIu8 "), green(%" PRIu16 ",%" PRIu8 "), blue(%" PRIu16 ",%" PRIu8 ")",
                    __FUNCTION__, serverPf.bitsPerPixel(), depth, (int) serverBigEndian, (int) serverTrueColor,
                    serverPf.rmax(), serverPf.rshift(), serverPf.gmax(), serverPf.gshift(), serverPf.bmax(), serverPf.bshift());

        // check server format
        switch(serverPf.bitsPerPixel())
        {
            case 32: case 16: case 8:
                break;

            default:
                Application::error("%s: unknown pixel format, bpp: %" PRIu8, __FUNCTION__, serverPf.bitsPerPixel());
                return false;
        }

        if(! serverTrueColor || serverPf.rmax() == 0 || serverPf.gmax() == 0 || serverPf.bmax() == 0)
        {
            Application::error("%s: unsupported pixel format", __FUNCTION__);
            return false;
        }

        pixelFormatEvent(serverPf, XCB::Size(fbWidth, fbHeight));

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

    const PixelFormat & RFB::ClientDecoder::serverFormat(void) const
    {
        return serverPf;
    }

    void RFB::ClientDecoder::rfbMessagesShutdown(void)
    {
        channelsShutdown();
        std::this_thread::sleep_for(100ms);
        rfbMessages = false;
    }

    std::list<int> RFB::ClientDecoder::supportedEncodings(void) const
    {
        return { 
                    ENCODING_ZRLE, ENCODING_TRLE, ENCODING_HEXTILE,
                    ENCODING_ZLIB, ENCODING_CORRE, ENCODING_RRE,

#ifdef LTSM_DECODING_FFMPEG
	            ENCODING_FFMPEG_H264,
	            ENCODING_FFMPEG_AV1,
	            ENCODING_FFMPEG_VP8,
#endif

		     ENCODING_RAW };
    }

    void RFB::ClientDecoder::rfbMessagesLoop(void)
    {
        auto encodings = supportedEncodings();

        // added pseudo encodings
        encodings.push_front(ENCODING_LAST_RECT);
        encodings.push_front(ENCODING_RICH_CURSOR);
        encodings.push_front(ENCODING_EXT_DESKTOP_SIZE);
        encodings.push_front(ENCODING_CONTINUOUS_UPDATES);

        if(ltsmSupported())
            encodings.push_front(ENCODING_LTSM);

        auto prefferedEncoding = clientEncoding();

#ifdef LTSM_DECODING_FFMPEG
        // experimental feature remove and added from preffered
	if(prefferedEncoding != Tools::lower(encodingName(ENCODING_FFMPEG_H264)))
            encodings.remove(ENCODING_FFMPEG_H264);
	if(prefferedEncoding != Tools::lower(encodingName(ENCODING_FFMPEG_AV1)))
            encodings.remove(ENCODING_FFMPEG_AV1);
	if(prefferedEncoding != Tools::lower(encodingName(ENCODING_FFMPEG_VP8)))
            encodings.remove(ENCODING_FFMPEG_VP8);
#endif

        if( ! prefferedEncoding.empty())
        {
            // preffered set priority
            auto it = std::find_if(encodings.begin(), encodings.end(),
                            [&](auto & enc){ return prefferedEncoding == Tools::lower(RFB::encodingName(enc)); });
            if(it != encodings.end())
            {
                auto enc = *it;
                encodings.erase(it);
                it = std::find(encodings.begin(), encodings.end(), ENCODING_LTSM);
                encodings.insert(it == encodings.end() ? it : std::next(it), enc);
            }
        }

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

            if(ltsmServer && msgType == PROTOCOL_LTSM)
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
                case SERVER_FB_UPDATE:
                    try
                    {
                        recvFBUpdateEvent();
                    }
                    catch(const std::exception & err)
                    {
                        rfbMessagesShutdown();
                    }
                    break;

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

    void RFB::ClientDecoder::displayResizeEvent(const XCB::Size & dsz)
    {
	Application::info("%s: display resized, new size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, dsz.width, dsz.height);

#ifdef LTSM_DECODING_FFMPEG
	// event background
	std::thread([this, sz = dsz]()
	{
    	    if(this->decoder &&
		(this->decoder->getType() == RFB::ENCODING_FFMPEG_H264 ||
		this->decoder->getType() == RFB::ENCODING_FFMPEG_AV1 || this->decoder->getType() == RFB::ENCODING_FFMPEG_VP8))
    		this->decoder->resizedEvent(sz);
	}).detach();
#endif
    }

    void RFB::ClientDecoder::sendPixelFormat(void)
    {
        auto & pf = clientFormat();

        Application::debug("%s: local pixel format: bpp: %" PRIu8 ", bigendian: %d, red(%" PRIu16 ",%" PRIu8 "), green(%" PRIu16 ",%" PRIu8 "), blue(%" PRIu16 ",%" PRIu8 ")",
                    __FUNCTION__, pf.bitsPerPixel(), (int) BigEndian,
                    pf.rmax(), pf.rshift(), pf.gmax(), pf.gshift(), pf.bmax(), pf.bshift());

        std::scoped_lock guard{ sendLock };

        // send pixel format
        sendInt8(RFB::CLIENT_SET_PIXEL_FORMAT);
        sendZero(3); // padding
        sendInt8(pf.bitsPerPixel());
        sendInt8(24); // depth
        sendInt8(BigEndian);
        sendInt8(1); // trueColor
        sendIntBE16(pf.rmax());
        sendIntBE16(pf.gmax());
        sendIntBE16(pf.bmax());
        sendInt8(pf.rshift());
        sendInt8(pf.gshift());
        sendInt8(pf.bshift());
        sendZero(3); // padding
        sendFlush();
    }

    void RFB::ClientDecoder::sendEncodings(const std::list<int> & encodings)
    {
        for(auto type : encodings)
            Application::debug("%s: %s", __FUNCTION__, encodingName(type));

        std::scoped_lock guard{ sendLock };

        sendInt8(RFB::CLIENT_SET_ENCODINGS);
        sendZero(1); // padding
        sendIntBE16(encodings.size());
        for(auto val : encodings)
            sendIntBE32(val);
        sendFlush();
    }

    void RFB::ClientDecoder::sendKeyEvent(bool pressed, uint32_t keysym)
    {
        Application::debug("%s: keysym: 0x%08" PRIx32 ", pressed: %d", __FUNCTION__, keysym, (int) pressed);

        std::scoped_lock guard{ sendLock };

        sendInt8(RFB::CLIENT_EVENT_KEY);
        sendInt8(pressed ? 1 : 0);
        // padding
        sendZero(2);
        sendIntBE32(keysym);
        sendFlush();
    }

    void RFB::ClientDecoder::sendPointerEvent(uint8_t buttons, uint16_t posx, uint16_t posy)
    {
        Application::debug("%s: pointer: [%" PRIu16 ", %" PRIu16 "], buttons: 0x%02" PRIx8 , __FUNCTION__, posx, posy, buttons);

        std::scoped_lock guard{ sendLock };

        sendInt8(RFB::CLIENT_EVENT_POINTER);

        sendInt8(buttons);
        sendIntBE16(posx);
        sendIntBE16(posy);
        sendFlush();
    }

    void RFB::ClientDecoder::sendCutTextEvent(const char* buf, size_t len)
    {
        Application::debug("%s: buffer size: %u", __FUNCTION__, len);

        std::scoped_lock guard{ sendLock };

        sendInt8(RFB::CLIENT_CUT_TEXT);
        // padding
        sendZero(3);
        sendIntBE32(len);
        sendRaw(buf, len);
        sendFlush();
    }

    void RFB::ClientDecoder::sendContinuousUpdates(bool enable, const XCB::Region & reg)
    {
        Application::debug("%s: status: %s, region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, (enable ? "enable" : "disable"), reg.x, reg.y, reg.width, reg.height);

        std::scoped_lock guard{ sendLock };
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
        Application::debug("%s: region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        std::scoped_lock guard{ sendLock };

        // send framebuffer update request
        sendInt8(CLIENT_REQUEST_FB_UPDATE);
        sendInt8(incr ? 1 : 0);
        sendIntBE16(reg.x);
        sendIntBE16(reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        sendFlush();
    }


    void RFB::ClientDecoder::updateRegion(int type, const XCB::Region & reg)
    {
        if(! decoder || type != decoder->getType())
        {
            switch(type)
            {
                case ENCODING_RAW:      decoder = std::make_unique<DecodingRaw>(); break;
                case ENCODING_RRE:      decoder = std::make_unique<DecodingRRE>(false); break;
                case ENCODING_CORRE:    decoder = std::make_unique<DecodingRRE>(true); break;
                case ENCODING_HEXTILE:  decoder = std::make_unique<DecodingHexTile>(false); break;
                case ENCODING_TRLE:     decoder = std::make_unique<DecodingTRLE>(false); break;
                case ENCODING_ZRLE:     decoder = std::make_unique<DecodingTRLE>(true); break;
                case ENCODING_ZLIB:     decoder = std::make_unique<DecodingZlib>(); break;

#ifdef LTSM_DECODING_FFMPEG
        	case ENCODING_FFMPEG_H264:
        	case ENCODING_FFMPEG_AV1:
        	case ENCODING_FFMPEG_VP8:
		    decoder = std::make_unique<DecodingFFmpeg>(type);
		    decoder->setDebug(4 /* AV_LOG_VERBOSE */);
		    break;
#endif

                default:
                {
                    Application::error("%s: %s", __FUNCTION__, "unknown decoding");
                    throw rfb_error(NS_FuncName);
                }
            }
        }

        decoder->updateRegion(*this, reg);
    }

    void RFB::ClientDecoder::recvFBUpdateEvent(void)
    {
        auto start = std::chrono::steady_clock::now();

        // padding
        recvSkip(1);
        auto numRects = recvIntBE16();
        XCB::Region reg;

        Application::debug("%s: num rects: %" PRIu16, __FUNCTION__, numRects);

        while(0 < numRects--)
        {
            reg.x = recvIntBE16();
            reg.y = recvIntBE16();
            reg.width = recvIntBE16();
            reg.height = recvIntBE16();
            int encodingType = recvIntBE32();

            Application::debug("%s: region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], encodingType: %s",
                     __FUNCTION__, reg.x, reg.y, reg.width, reg.height, RFB::encodingName(encodingType));

            switch(encodingType)
            {
                case ENCODING_LTSM:
                    recvDecodingLtsm(reg);
                    break;

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
                    Application::warning("%s: %s", __FUNCTION__, "old desktop_size");
                    break;

                default:
                    updateRegion(encodingType, reg);
                    break;
            }

        }

        auto dt = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
        Application::debug("%s: update time: %uus", __FUNCTION__, dt.count());

        fbUpdateEvent();
    }

    void RFB::ClientDecoder::recvColorMapEvent(void)
    {
        // padding
        recvSkip(1);
        auto firstColor = recvIntBE16();
        auto numColors = recvIntBE16();
        Application::debug("%s: num colors: %" PRIu16 ", first color: %" PRIu16, __FUNCTION__, numColors, firstColor);

        std::vector<Color> colors(numColors);

        for(auto & col : colors)
        {
            col.r = recvInt8();
            col.g = recvInt8();
            col.b = recvInt8();

            if(Application::isDebugLevel(DebugLevel::Trace))
                Application::debug("%s: color [0x%02" PRIx8 ",0x%02" PRIx8 ",0x%02" PRIx8 "]", __FUNCTION__, col.r, col.g, col.b);
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

        Application::debug("%s: length: %" PRIu32, __FUNCTION__, length);

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

    void RFB::ClientDecoder::recvDecodingLastRect(const XCB::Region & reg)
    {
        Application::debug("%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);
    }

    void RFB::ClientDecoder::recvDecodingRichCursor(const XCB::Region & reg)
    {
        Application::debug("%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        auto buf = recvData(reg.width * reg.height * serverPf.bytePerPixel());
        auto mask = recvData(std::floor((reg.width + 7) / 8) * reg.height);

        richCursorEvent(reg, std::move(buf), std::move(mask));
    }

    void RFB::ClientDecoder::recvDecodingExtDesktopSize(int status, int err, const XCB::Size & sz)
    {
        Application::info("%s: status: %d, error: %d, size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, status, err, sz.width, sz.height);

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
            Application::debug("%s: screen: %" PRIu32 ", area: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], flags: 0x%08" PRIx32, __FUNCTION__, screen.id, posx, posy, screen.width, screen.height, flags);
        }

        decodingExtDesktopSizeEvent(status, err, sz, screens); 
    }

    void RFB::ClientDecoder::sendSetDesktopSize(const XCB::Size & wsz)
    {
        Application::info("%s: size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, wsz.width, wsz.height);

        std::scoped_lock guard{ sendLock };
        
        sendInt8(RFB::CLIENT_SET_DESKTOP_SIZE);
        sendZero(1);
        sendIntBE16(wsz.width);
        sendIntBE16(wsz.height);

        // num of screens
        sendInt8(1);
        sendZero(1);

        // screen id
        sendIntBE32(0);
        // posx, posy
        sendIntBE32(0);
        sendIntBE16(wsz.width);
        sendIntBE16(wsz.height);
        // flag
        sendIntBE32(0);

        sendFlush();
    }

    void RFB::ClientDecoder::recvDecodingLtsm(const XCB::Region & reg)
    {
        Application::info("%s: success", __FUNCTION__);

        ltsmServer = true;
        size_t type = recvIntBE32();

        // type 0: handshake part
        if(type == 0)
        {
            int flags = 0;
            ltsmHandshakeEvent(flags);
        }
        else
        {
            // auto data = recvData(len);

            Application::error("%s: %s", __FUNCTION__, "unknown decoding");
            throw rfb_error(NS_FuncName);
        }
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
        if(cmd == SystemCommand::ChannelError)
            systemChannelError(jo);
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
