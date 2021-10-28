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

#include "ltsm_tools.h"
#include "ltsm_font_psf.h"
#include "ltsm_connector.h"
#include "ltsm_connector_vnc.h"

using namespace std::chrono_literals;

namespace LTSM
{
    const char* desktopResizeModeString(const DesktopResizeMode & mode)
    {
	switch(mode)
	{
	    case DesktopResizeMode::Disabled: 		return "Disabled";
	    case DesktopResizeMode::Success:		return "Success";
	    case DesktopResizeMode::ServerInform:	return "ServerInform";
	    case DesktopResizeMode::ClientRequest:	return "ClientRequest";
	    default: break;
	}

	return "Undefined";
    }

    /* Connector::VNC */
    Connector::VNC::VNC(sdbus::IConnection* conn, const JsonObject & jo)
        : SignalProxy(conn, jo, "vnc"), streamIn(nullptr), streamOut(nullptr), loopMessage(false), encodingDebug(0),
	    encodingThreads(2), pressedMask(0), fbUpdateProcessing(false), sendBellFlag(false), desktopResizeMode(DesktopResizeMode::Undefined)
    {
	socket.reset(new InetStream());
	streamIn = streamOut = socket.get();
        registerProxy();
    }

    Connector::VNC::~VNC()
    {
        if(0 < _display) busConnectorTerminated(_display);
        unregisterProxy();
        clientDisconnectedEvent();
    }

    bool Connector::VNC::clientVenCryptHandshake(void)
    {
        const std::string tlsPriority = _config->getString("vnc:gnutls:priority", "NORMAL:+ANON-ECDH:+ANON-DH");
        bool tlsAnonMode = _config->getBoolean("vnc:gnutls:anonmode", true);
        int tlsDebug = _config->getInteger("vnc:gnutls:debug", 3);

        const std::string tlsCAFile = checkFileOption("vnc:gnutls:cafile");
        const std::string tlsCertFile = checkFileOption("vnc:gnutls:certfile");
        const std::string tlsKeyFile = checkFileOption("vnc:gnutls:keyfile");
        const std::string tlsCRLFile = checkFileOption("vnc:gnutls:crlfile");

        const std::string keymapFile = _config->getString("vnc:keymap:file");
        if(keymapFile.size())
        {
            JsonContentFile jc(keymapFile);
            if(jc.isValid() && jc.isObject())
            {
                keymap.reset(new JsonObject());
                auto jo = jc.toObject();

                for(auto & key : jo.keys())
                    if(auto map = jo.getObject(key))
                        keymap->join(*map);

                Application::notice("keymap loaded: %s, items: %d", keymapFile.c_str(), keymap->size());
            }
        }

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
	bool x509Mode = false;

	if(minorVer == 1)
	{
	    if(tlsAnonMode)
	        sendInt8(1).sendInt8(RFB::SECURITY_VENCRYPT01_TLSNONE).sendFlush();
	    else
	        sendInt8(2).sendInt8(RFB::SECURITY_VENCRYPT01_TLSNONE).sendInt8(RFB::SECURITY_VENCRYPT01_X509NONE).sendFlush();

	    int res = recvInt8();
    	    Application::debug("RFB 6.2.19.0.1, client choice vencrypt security: 0x%02x", res);

	    switch(res)
	    {
		case RFB::SECURITY_VENCRYPT01_TLSNONE:
		    break;

		case RFB::SECURITY_VENCRYPT01_X509NONE:
		    if(tlsAnonMode)
		    {
			Application::error("error: %s", "unsupported vencrypt security");
        		return false;
		    }
		    x509Mode = true;
		    break;

		default:
		    Application::error("error: %s", "unsupported vencrypt security");
        	    return false;
	    }
	}
	else
	// if(minorVer == 2)
	{
	    if(tlsAnonMode)
		sendInt8(1).sendIntBE32(RFB::SECURITY_VENCRYPT02_TLSNONE).sendFlush();
	    else
	        sendInt8(2).sendIntBE32(RFB::SECURITY_VENCRYPT02_TLSNONE).sendIntBE32(RFB::SECURITY_VENCRYPT02_X509NONE).sendFlush();

	    int res = recvIntBE32();
    	    Application::debug("RFB 6.2.19.0.2, client choice vencrypt security: 0x%08x", res);

	    switch(res)
	    {
		case RFB::SECURITY_VENCRYPT02_TLSNONE:
		    break;

		case RFB::SECURITY_VENCRYPT02_X509NONE:
		    if(tlsAnonMode)
		    {
			Application::error("error: %s", "unsupported vencrypt security");
        		return false;
		    }
		    x509Mode = true;
		    break;

		default:
		    Application::error("error: %s", "unsupported vencrypt security");
        	    return false;
	    }
	}

        sendInt8(1).sendFlush();

	// init hasdshake
	tls.reset(new TLS::Stream(socket.get()));

	bool tlsInitHandshake = x509Mode ? 
		tls->initX509Handshake(tlsPriority, tlsCAFile, tlsCertFile, tlsKeyFile, tlsCRLFile, tlsDebug) :
		tls->initAnonHandshake(tlsPriority, tlsDebug);

	if(tlsInitHandshake)
	{
	    streamIn = streamOut = tls.get();
	}

	return tlsInitHandshake;
    }

    int Connector::VNC::communication(void)
    {
	if(0 >= busGetServiceVersion())
	{
            Application::error("%s", "bus service failure");
            return EXIT_FAILURE;
	}

        Application::info("%s: remote addr: %s", __FUNCTION__, _remoteaddr.c_str());

        encodingThreads = _config->getInteger("vnc:encoding:threads", 2);
        if(encodingThreads < 1)
        {
            encodingThreads = 1;
        }
        else
        if(std::thread::hardware_concurrency() < encodingThreads)
        {
            encodingThreads = std::thread::hardware_concurrency();
            Application::error("encoding threads incorrect, fixed to hardware concurrency: %d", encodingThreads);
        }
        Application::info("using encoding threads: %d", encodingThreads);

        encodingDebug = _config->getInteger("vnc:encoding:debug", 0);
        prefEncodings = selectEncodings();
        disabledEncodings = _config->getStdList<std::string>("vnc:encoding:blacklist");

        std::string encryptionInfo = "none";

        if(! disabledEncodings.empty())
            disabledEncodings.remove_if([](auto & str){ return 0 == Tools::lower(str).compare("raw"); });

        // RFB 6.1.1 version
        const std::string version = Tools::StringFormat("RFB 00%1.00%2\n").arg(RFB::VERSION_MAJOR).arg(RFB::VERSION_MINOR);
        sendString(version).sendFlush();
        std::string magick = recvString(12);
        Application::debug("RFB 6.1.1, handshake version: %s", magick.c_str());

        if(magick != version)
        {
            Application::error("%s", "handshake failure");
            return EXIT_FAILURE;
        }

        // Xvfb: session request
        int screen = busStartLoginSession(_remoteaddr, "vnc");
        if(screen <= 0)
        {
            Application::error("%s", "login session request failure");
            return EXIT_FAILURE;
        }

        Application::debug("login session request success, display: %d", screen);
	Application::info("server default encoding: %s", RFB::encodingName(prefEncodings.second));

        if(! xcbConnect(screen))
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

        // init server format
        serverFormat = PixelFormat(_xcbDisplay->bitsPerPixel(), _xcbDisplay->depth(), big_endian, true,
                                            visual->red_mask, visual->green_mask, visual->blue_mask);

        bool tlsDisable = _config->getBoolean("vnc:gnutls:disable", false);
        // RFB 6.1.2 security
	if(tlsDisable)
	{
	    sendInt8(1);
	    sendInt8(RFB::SECURITY_TYPE_NONE);
	    sendFlush();
        }
	else
	{
    	    sendInt8(2);
	    sendInt8(RFB::SECURITY_TYPE_VENCRYPT);
	    sendInt8(RFB::SECURITY_TYPE_NONE);
	    sendFlush();
	}
        int clientSecurity = recvInt8();
        Application::debug("RFB 6.1.2, client security: 0x%02x", clientSecurity);

        if(clientSecurity == RFB::SECURITY_TYPE_NONE)
            sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
        else
        if(clientSecurity == RFB::SECURITY_TYPE_VENCRYPT)
	{
	    if(! clientVenCryptHandshake())
        	return EXIT_FAILURE;

            encryptionInfo = tls->sessionDescription();
            sendIntBE32(RFB::SECURITY_RESULT_OK).sendFlush();
	}
	else
        {
            const std::string err("no matching security types");
            sendIntBE32(RFB::SECURITY_RESULT_ERR).sendString(err).sendFlush();

            Application::error("error: %s", err.c_str());
            return EXIT_FAILURE;
        }

    	busSetEncryptionInfo(screen, encryptionInfo);

        // RFB 6.3.1 client init
    	int clientSharedFlag = recvInt8();
    	Application::debug("RFB 6.3.1, client shared: 0x%02x", clientSharedFlag);
    	// RFB 6.3.2 server init
	auto wsz = _xcbDisplay->size();
    	sendIntBE16(wsz.width);
    	sendIntBE16(wsz.height);
    	Application::debug("server send: pixel format, bpp: %d, depth: %d, be: %d, truecol: %d, red(%d,%d), green(%d,%d), blue(%d,%d)",
                           serverFormat.bitsPerPixel, serverFormat.depth, serverFormat.bigEndian(), serverFormat.trueColor(),
                           serverFormat.redMax, serverFormat.redShift, serverFormat.greenMax, serverFormat.greenShift, serverFormat.blueMax, serverFormat.blueShift);
    	clientFormat = serverFormat;
    	// send pixel format
    	sendInt8(serverFormat.bitsPerPixel);
    	sendInt8(serverFormat.depth);
    	sendInt8(serverFormat.bigEndian());
    	sendInt8(serverFormat.trueColor());
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
    	sendIntBE32(desktopName.size());
    	sendString(desktopName).sendFlush();

        // wait widget started signal(onHelperWidgetStarted), 3000ms, 10 ms pause
        if(! Tools::waitCallable<std::chrono::milliseconds>(3000, 10,
            [=](){ _conn->enterEventLoopAsync(); return ! this->loopMessage; }))
        {
            Application::info("connector starting: %s", "something went wrong...");
            return EXIT_FAILURE;
        }

        Application::info("connector starting: %s", "wait RFB messages...");

        // xcb on
        setEnableXcbMessages(true);
        serverRegion.assign(0, 0, wsz.width, wsz.height);
        XCB::Region damageRegion(0, 0, 0, 0);
	bool clientUpdateReq = false;
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
                        damageRegion.join(serverRegion);
			clientUpdateReq = true;
                        break;

                    case RFB::CLIENT_SET_ENCODINGS:
                        if(clientSetEncodings())
                        {
                            // full update
                            damageRegion.join(serverRegion);
			    clientUpdateReq = true;
                        }
                        break;

                    case RFB::CLIENT_REQUEST_FB_UPDATE:
                        // full update
                        if(clientFramebufferUpdate())
                            damageRegion.join(serverRegion);
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

                    default:
                        Application::error("RFB unknown message: 0x%02x", msgType);
                        throw std::runtime_error("RFB unknown message");
                        break;
                }
            }

            if( isAllowXcbMessages())
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
                    else
                    if(_xcbDisplay->isRandrCRTCNotify(ev))
                    {
                        auto notify = reinterpret_cast<xcb_randr_notify_event_t*>(ev.get());

                        xcb_randr_crtc_change_t cc = notify->u.cc;
                        if(0 < cc.width && 0 < cc.height)
                        {
			    busDisplayResized(_display, cc.width, cc.height);

			    if(DesktopResizeMode::Undefined != desktopResizeMode &&
				DesktopResizeMode::Disabled != desktopResizeMode &&
				(screensInfo.empty() || (screensInfo.front().width != cc.width || screensInfo.front().height != cc.height)))
			    {
				screensInfo.push_back({ .width = cc.width, .height = cc.height });
				desktopResizeMode = DesktopResizeMode::ServerInform;
			    }
                        }
                    }
                    else
                    if(_xcbDisplay->isSelectionNotify(ev))
            	    {
                        auto notify = reinterpret_cast<xcb_selection_notify_event_t*>(ev.get());
			if(_xcbDisplay->selectionNotifyAction(notify))
                	    selbuf = _xcbDisplay->getSelectionData();
		    }
                }

		if(! damageRegion.empty())
                    // fix out of screen
                    damageRegion = _xcbDisplay->region().intersected(damageRegion.align(4));

                // server action
		if(! isUpdateProcessed())
		{
		    if(DesktopResizeMode::Undefined != desktopResizeMode &&
			DesktopResizeMode::Disabled != desktopResizeMode && DesktopResizeMode::Success != desktopResizeMode)
		    {
			serverSendDesktopSize(desktopResizeMode);
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
                            std::thread([=](){ this->serverSendFrameBufferUpdate(res); }).detach();
		        }
                        damageRegion.reset();
		        clientUpdateReq = false;
                    }
                }
    	    }

            // dbus processing
            _conn->enterEventLoopAsync();
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
        {
            std::this_thread::sleep_for(1ms);
        }
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
                throw std::runtime_error("unknown client pixel format");
        }

	if(trueColor == 0 || redMax == 0 || greenMax == 0 || blueMax == 0)
            throw std::runtime_error("unsupported pixel format");

        clientFormat = PixelFormat(bitsPerPixel, depth, bigEndian, trueColor, redMax, greenMax, blueMax, redShift, greenShift, blueShift);
        if(colourMap.size()) colourMap.clear();
    }

    bool Connector::VNC::clientSetEncodings(void)
    {
        waitSendingFBUpdate();

        // RFB: 6.4.2
        // skip padding
        recvSkip(1);

	int previousType = prefEncodings.second;
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

        prefEncodings = selectEncodings();
	Application::notice("server select encoding: %s", RFB::encodingName(prefEncodings.second));

	return previousType != prefEncodings.second;
    }

    bool Connector::VNC::clientFramebufferUpdate(void)
    {
        // RFB: 6.4.3
        int incremental = recvInt8();
        clientRegion.x = recvIntBE16();
        clientRegion.y = recvIntBE16();
        clientRegion.width = recvIntBE16();
        clientRegion.height = recvIntBE16();
        Application::debug("RFB 6.4.3, request update fb, region [%d, %d, %d, %d], incremental: %d",
                           clientRegion.x, clientRegion.y, clientRegion.width, clientRegion.height, incremental);
        bool fullUpdate = incremental == 0;

        if(fullUpdate)
	{
            clientRegion = serverRegion;

	    if(desktopResizeMode == DesktopResizeMode::Undefined &&
		std::any_of(clientEncodings.begin(), clientEncodings.end(),
                    [=](auto & val){ return  val == RFB::ENCODING_EXT_DESKTOP_SIZE; }))
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
        Application::debug("RFB 6.4.4, key event (%s), keysym: 0x%04x", (pressed ? "pressed" : "released"), keysym);

        if(isAllowXcbMessages())
        {
            // local keymap priority "vnc:keymap:file"
            if(auto value = (keymap ? keymap->getValue(Tools::hex(keysym, 8)) : nullptr))
            {
                // no wait xcb replies
                std::thread([=]()
                {
                    if(value->isArray())
                    {
                        auto ja = static_cast<const JsonArray*>(value);
                        for(auto & val : ja->toStdVector<int>())
                            _xcbDisplay->fakeInputKeycode(0 < pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, val);
                    }
                    else
                    {
                        _xcbDisplay->fakeInputKeycode(0 < pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, value->getInteger());
                    }
                }).detach();
            }
            else
            if(auto keyCodes = _xcbDisplay->keysymToKeycodes(keysym))
            {
                // no wait xcb replies
                std::thread([=]()
                {
                    _xcbDisplay->fakeInputKeysym(0 < pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keyCodes);
                }).detach();
            }
        }
    }

    void Connector::VNC::clientPointerEvent(void)
    {
        // RFB: 6.4.5
        int mask = recvInt8(); // button1 0x01, button2 0x02, button3 0x04
        int posx = recvIntBE16();
        int posy = recvIntBE16();
        Application::debug("RFB 6.4.5, pointer event, mask: 0x%02x, posx: %d, posy: %d", mask, posx, posy);

        if(isAllowXcbMessages())
        {
            // no wait xcb replies
            std::thread([=]()
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
                            _xcbDisplay->fakeInputMouse(XCB_BUTTON_PRESS, num + 1, posx, posy);
                            this->pressedMask |= bit;
                        }
                        else if(bit & pressedMask)
                        {
			    if(1 < encodingDebug)
                        	Application::debug("xfb fake input released: %d", num + 1);
                            _xcbDisplay->fakeInputMouse(XCB_BUTTON_RELEASE, num + 1, posx, posy);
                            this->pressedMask &= ~bit;
                        }
                    }
                }
                else
                {
		    if(1 < encodingDebug)
                	Application::debug("xfb fake input move, posx: %d, posy: %d", posx, posy);
                    _xcbDisplay->fakeInputMouse(XCB_MOTION_NOTIFY, 0, posx, posy);
                }
            }).detach();
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
	{
    	    recvSkip(length);
	}
    }

    void Connector::VNC::clientSetDesktopSizeEvent(void)
    {
        // skip padding (one byte!)
        recvSkip(1);
        int width = recvIntBE16();
        int height = recvIntBE16();
	int numOfScreens = recvInt8();
        recvSkip(1);

        Application::notice("RFB 6.4.x, set desktop size event, size: %dx%d, screens: %d", width, height, numOfScreens);
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

    void Connector::VNC::clientDisconnectedEvent(void)
    {
        Application::warning("RFB disconnected, display: %d", _display);
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

            Application::debug("server send fb update: [%d, %d, %d, %d]", reg.x, reg.y, reg.width, reg.height);

            // fix align
            if(reply->size() != reg.width * reg.height * bytePerPixel)
	    {
        	Application::error("%s: %s failed", __FUNCTION__, "align region");
        	throw CodecFailed("region not aligned");
	    }

            // RFB: 6.5.1
            sendInt8(RFB::SERVER_FB_UPDATE);
            // padding
            sendInt8(0);

            FrameBuffer frameBuffer(reply->data(), reg, serverFormat);

            // apply render primitives
            renderPrimitivesToFB(frameBuffer);
	    int encodingLength = 0;

            try
	    {
		// send encodings
        	encodingLength = prefEncodings.first(frameBuffer);
	    }
	    catch(const CodecFailed & ex)
	    {
		Application::error("codec exception: %s", ex.err.c_str());
		loopMessage = false;
		return false;
	    }
	    catch(const SocketFailed & ex)
	    {
		Application::error("socket exception: %s", ex.err.c_str());
		loopMessage = false;
		return false;
	    }

            if(encodingDebug)
            {
                int rawLength = 14 /* raw header for one region */ + reg.width * reg.height * clientFormat.bytePerPixel();
                float optimize = 100.0f - encodingLength * 100 / static_cast<float>(rawLength);
                Application::debug("encoding %s optimize: %.*f%% (send: %d, raw: %d), region(%d, %d)", RFB::encodingName(prefEncodings.second), 2, optimize, encodingLength, rawLength, reg.width, reg.height);
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
        // break connection
        if(! loopMessage)
            return 0;

        if(clientFormat.trueColor())
        {
            switch(clientFormat.bytePerPixel())
            {
                case 4:
                    if(clientFormat.bigEndian())
			sendIntBE32(clientFormat.convertFrom(serverFormat, pixel));
		    else
			sendIntLE32(clientFormat.convertFrom(serverFormat, pixel));
                    return 4;

                case 2:
                    if(clientFormat.bigEndian())
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

        throw std::runtime_error("send pixel: unknown format");
        return 0;
    }

    int Connector::VNC::sendCPixel(uint32_t pixel)
    {
        // break connection
        if(! loopMessage)
            return 0;

        if(clientFormat.trueColor() && clientFormat.bitsPerPixel == 32)
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

    void Connector::VNC::onLoginSuccess(const int32_t & display, const std::string & userName)
    {
        if(0 < _display && display == _display)
        {
            setEnableXcbMessages(false);
            waitSendingFBUpdate();

            SignalProxy::onLoginSuccess(display, userName);
            setEnableXcbMessages(true);

	    // fix new session size
	    auto wsz = _xcbDisplay->size();
	    if(wsz != serverRegion.toSize())
	    {
                if(_xcbDisplay->setScreenSize(serverRegion.width, serverRegion.height))
                {
                    wsz = _xcbDisplay->size();
                    Application::notice("change session size %dx%d, display: %d", wsz.width, wsz.height, _display);
                }
	    }

            // full update
            _xcbDisplay->damageAdd(serverRegion);

            Application::notice("dbus signal: login success, display: %d, username: %s", _display, userName.c_str());
        }
    }

    void Connector::VNC::onShutdownConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            setEnableXcbMessages(false);
            waitSendingFBUpdate();

            loopMessage = false;
            Application::notice("dbus signal: shutdown connector, display: %d", display);
        }
    }

    void Connector::VNC::onHelperWidgetStarted(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: helper started, display: %d", display);
            loopMessage = true;
        }
    }

    void Connector::VNC::onSendBellSignal(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: send bell, display: %d", display);
            sendBellFlag = true;
        }
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
