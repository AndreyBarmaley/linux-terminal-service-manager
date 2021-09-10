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

#include <cmath>
#include <tuple>
#include <cstdio>
#include <string>
#include <thread>
#include <future>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <filesystem>

#include <unistd.h>
#include <winpr/crt.h>
#include <winpr/ssl.h>
#include <winpr/input.h>
#include <winpr/wtsapi.h>
#include <winpr/version.h>

#include <freerdp/freerdp.h>
#include <freerdp/version.h>
#include <freerdp/constants.h>
#include <freerdp/codec/planar.h>
#include <freerdp/codec/region.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/channels/channels.h>

#include "ltsm_tools.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_connector_vnc.h"
#include "ltsm_connector_rdp.h"

using namespace std::chrono_literals;

#define FREERDP_VERSION_NUMBER ((FREERDP_VERSION_MAJOR << 16) | (FREERDP_VERSION_MINOR << 8) | FREERDP_VERSION_REVISION)

namespace LTSM
{
    void stream_free(wStream* st)
    {
        Stream_Free(st, TRUE);
    }

    struct ClientContext : rdpContext
    {
	const JsonObject* config;
	Connector::RDP*	rdp;

        XCB::SharedDisplay x11display;

        std::unique_ptr<RFX_CONTEXT, decltype(rfx_context_free)*> rfx;
        std::unique_ptr<BITMAP_PLANAR_CONTEXT, decltype(freerdp_bitmap_planar_context_free)*> planar;

        std::unique_ptr<wStream, decltype(stream_free)*> st;
        std::unique_ptr<void, decltype(WTSCloseServer)*> vcm;

        size_t		frameId;
        int             bytePerPixel;
	int		pressedMask;
        std::atomic<bool> activated;
    };

    // FreeRDP
    int clientContextNew(rdp_freerdp_peer* client, ClientContext* context)
    {
	context->st = { Stream_New(NULL, 0xFFFF), stream_free };
        if(! context->st)
	{
	    Application::error("%s: failed", "Stream_New");
	    return FALSE;
	}

        context->vcm = { WTSOpenServerA((LPSTR) client->context), WTSCloseServer };
	if(!context->vcm || context->vcm.get() == INVALID_HANDLE_VALUE)
	{
	    Application::error("%s: failed", "WTSOpenServer");
            return FALSE;
	}

        context->frameId = 0;
        context->activated = false;
        context->bytePerPixel = 0;
	context->pressedMask = 0;

        Application::info("%s: success", __FUNCTION__);
	return TRUE;
    }

    struct FreeRdpClient
    {
	freerdp_peer*		peer;
	ClientContext*		context;
        std::atomic<HANDLE>	stopEvent;

	FreeRdpClient(int fd, const std::string & remoteaddr, const JsonObject & config, Connector::RDP* connector) : peer(nullptr), context(nullptr)
	{
	    Application::info("freerdp version usage: %s, winpr: %s", FREERDP_VERSION_FULL, WINPR_VERSION_FULL);
	    winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
	    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());

	    // init freerdp log system
	    auto log = WLog_GetRoot();
            if(log)
	    {
	        WLog_SetLogAppenderType(log, WLOG_APPENDER_SYSLOG);
		auto str = Tools::lower(config.getString("rdp:wlog:level"));
		int type = WLOG_ERROR;

		if(str == "trace")
		    type = WLOG_TRACE;
		else
		if(str == "debug")
		    type = WLOG_DEBUG;
		else
		if(str == "info")
		    type = WLOG_INFO;
		else
		if(str == "warn")
		    type = WLOG_WARN;
		else
		if(str == "error")
		    type = WLOG_ERROR;
		else
		if(str == "fatal")
		    type = WLOG_FATAL;
		else
		if(str == "off")
		    type = WLOG_OFF;

	        WLog_SetLogLevel(log, type);
	    }

	    peer = freerdp_peer_new(fd);
	    peer->local = TRUE;
            std::copy_n(remoteaddr.begin(), std::min(sizeof(peer->hostname), remoteaddr.size()), peer->hostname);

            stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	    // init context
	    peer->ContextSize = sizeof(ClientContext);
	    peer->ContextNew = (psPeerContextNew) clientContextNew;
	    // peer->ContextFree = (psPeerContextFree) clientContextFree;

            if(! freerdp_peer_context_new(peer))
            {
                Application::error("%s: failed", "freerdp_peer_context_new");
                throw EXIT_FAILURE;
            }

	    Application::error("peer context: %p", peer);
	    Application::error("client context: %p", peer->context);

	    context = static_cast<ClientContext*>(peer->context);
	    context->config = & config;
	    context->rdp = connector;

            auto certfile = connector->checkFileOption("rdp:server:certfile");
            auto keyfile = connector->checkFileOption("rdp:server:keyfile");

            if(keyfile.size())
	    {
                peer->settings->CertificateFile = strdup(certfile.c_str());
		Application::info("server cert: %s", peer->settings->CertificateFile);
	    }

            if(keyfile.size())
            {
                peer->settings->PrivateKeyFile = strdup(keyfile.c_str());
                peer->settings->RdpKeyFile = strdup(keyfile.c_str());
		Application::info("server key: %s", peer->settings->RdpKeyFile);
            }

	    int encryptionLevel = 0;
	    auto paramencryptionLevel = Tools::lower(config.getString("rdp:encription:level", "compatible"));

	    if(paramencryptionLevel == "high")
		encryptionLevel = ENCRYPTION_LEVEL_HIGH;
	    else
	    if(paramencryptionLevel == "low")
		encryptionLevel = ENCRYPTION_LEVEL_LOW;
	    else
	    if(paramencryptionLevel == "fips")
		encryptionLevel = ENCRYPTION_LEVEL_FIPS;
	    else
		encryptionLevel = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;

            peer->settings->RdpSecurity = config.getBoolean("rdp:security:rdp", true) ? TRUE : FALSE;
            peer->settings->TlsSecurity = config.getBoolean("rdp:security:tls", true) ? TRUE : FALSE;
            peer->settings->NlaSecurity = config.getBoolean("rdp:security:nla", false) ? TRUE : FALSE;
            peer->settings->ExtSecurity = FALSE;
            peer->settings->UseRdpSecurityLayer = FALSE;
            peer->settings->EncryptionLevel = encryptionLevel;

            peer->settings->ColorDepth = 32;
	    peer->settings->NSCodec = FALSE;
	    peer->settings->RemoteFxCodec = config.getBoolean("rdp:codec:remotefx", false) ? TRUE : FALSE;;
            peer->settings->RefreshRect = TRUE;
	    peer->settings->SuppressOutput = TRUE;
	    peer->settings->FrameMarkerCommandEnabled = TRUE;
	    peer->settings->SurfaceFrameMarkerEnabled = TRUE;

	    peer->PostConnect = Connector::RDP::clientPostConnect;
	    peer->Activate = Connector::RDP::clientActivate;
            peer->input->KeyboardEvent = Connector::RDP::clientKeyboardEvent;
	    //peer->input->UnicodeKeyboardEvent = Connector::RDP::clientUnicodeKeyboardEvent;
            peer->input->MouseEvent = Connector::RDP::clientMouseEvent;
            //peer->input->ExtendedMouseEvent = Connector::RDP::clientExtendedMouseEvent;
            peer->update->RefreshRect = Connector::RDP::clientRefreshRect;
	    peer->update->SuppressOutput = Connector::RDP::clientSuppressOutput;

            if(1 != peer->Initialize(peer))
            {
		Application::error("%s: failed", "peer->Initialize");
                throw EXIT_FAILURE;
	    }
	}

	~FreeRdpClient()
	{
            if(stopEvent)
                CloseHandle(stopEvent);

    	    if(peer)
	    {
    	        freerdp_peer_context_free(peer);
    		freerdp_peer_free(peer);
	    }
	}

        void stopEventLoop(void)
        {
            if(stopEvent)
            {
	        Application::info("%s: stop event", "FreeRdpClient");
                SetEvent(stopEvent);
            }
        }

        bool isShutdown(void) const
        {
            return ! stopEvent;
        }

        static bool enterEventLoop(FreeRdpClient* client)
        {
	    auto error = CHANNEL_RC_OK;
	    HANDLE handles[32] = { 0 };
	    DWORD count = 0;

	    Application::info("%s: enter event loop", "FreeRdpClient");
	    auto peer = client->peer;

	    // freerdp client events
    	    while(error == CHANNEL_RC_OK)
    	    {
                if(client->isShutdown())
                    break;

                if(peer->CheckFileDescriptor(peer) != TRUE)
                    break;

                if(WTSVirtualChannelManagerCheckFileDescriptor(client->context->vcm.get()) != TRUE)
                    break;

		if(WaitForSingleObject(client->stopEvent, 1) == WAIT_OBJECT_0)
		    break;

                // wait
                std::this_thread::sleep_for(1ms);
            }

            if(client->stopEvent)
            {
                // for shutdown flag
                CloseHandle(client->stopEvent);
                client->stopEvent = nullptr;
            }

	    peer->Disconnect(peer);

	    Application::info("%s: loop shutdown", "FreeRdpClient");
            return true;
        }
    };

    /* Connector::RDP */
    Connector::RDP::RDP(sdbus::IConnection* conn, const JsonObject & jo)
        : SignalProxy(conn, jo, "rdp"), helperStartedFlag(false), loopShutdownFlag(false), clientUpdatePartFlag(true)
    {
        registerProxy();
    }

    Connector::RDP::~RDP()
    {
	if(0 < _display) busConnectorTerminated(_display);
        unregisterProxy();
        clientDisconnectedEvent();
    }

    void Connector::RDP::clientDisconnectedEvent(void)
    {
        Application::warning("RDP disconnected, display: %d", _display);

        if(isAllowXcbMessages())
            xcbReleaseInputsEvent();
    }

    void Connector::RDP::xcbReleaseInputsEvent(void)
    {
	int buttons = 0;
        // send release pointer event
	if(buttons & PTR_FLAGS_BUTTON1)
    	    _xcbDisplay->fakeInputMouse(XCB_BUTTON_RELEASE, 1, 0, 0);

	if(buttons & PTR_FLAGS_BUTTON2)
    	    _xcbDisplay->fakeInputMouse(XCB_BUTTON_RELEASE, 2, 0, 0);

	if(buttons & PTR_FLAGS_BUTTON3)
    	    _xcbDisplay->fakeInputMouse(XCB_BUTTON_RELEASE, 3, 0, 0);

	// return buttons & ~(PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3);
        
        // send release key codes
/*
        if(! pressedKeys.empty())
        {
            for(auto & keyCodes : pressedKeys)
                _xcbDisplay->fakeInputKeysym(XCB_KEY_RELEASE, keyCodes);

            pressedKeys.clear();
        }
*/
    }

    int Connector::RDP::communication(void)
    {
	if(0 >= busGetServiceVersion())
        {
            Application::error("%s: failed", "bus service");
            return EXIT_FAILURE;
        }

        const std::string home = Tools::getenv("HOME", "/tmp");
	const auto socketFile = std::filesystem::path(home) / std::string("rdp_pid").append(std::to_string(getpid()));

        if(! initUnixSockets(socketFile.string()))
            return EXIT_FAILURE;

        // create FreeRdpClient
    	Application::notice("%s", "create freerdp client");
        freeRdpClient = std::unique_ptr<FreeRdpClient>(new FreeRdpClient(clientSocket(), _remoteaddr, *_config, this));
        auto freeRdpThread = std::thread([ptr = freeRdpClient.get()]{ FreeRdpClient::enterEventLoop(ptr); });
	XCB::Region damageRegion(0, 0, 0, 0);

	// all ok
	while(! loopShutdownFlag)
	{
	    if(freeRdpClient->isShutdown())
		loopShutdownFlag = true;

	    try
	    {
		if(! ProxySocket::enterEventLoopAsync())
		    loopShutdownFlag = true;
	    }
    	    catch(const SocketFailed &)
    	    {
		loopShutdownFlag = true;
		break;
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
                        loopShutdownFlag = true;
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
			    Application::error("LOOP: randr notify: %d,%d", cc.width, cc.height);
                            busDisplayResized(_display, cc.width, cc.height);
                    	    damageRegion.reset();
                        
                            // FIXME: rdp inform
                        }
                    }
                    else
                    if(_xcbDisplay->isSelectionNotify(ev))
                    {
                        // FIXME: rdp inform
                        //auto notify = reinterpret_cast<xcb_selection_notify_event_t*>(ev.get());
                        //if(_xcbDisplay->selectionNotifyAction(notify))
                        //    selbuf = _xcbDisplay->getSelectionData();
                    }
                }

                if(! damageRegion.empty())
                    // fix out of screen
		    damageRegion = _xcbDisplay->region().intersected(damageRegion.align(4));

                auto & context = freeRdpClient->context;
                if(! damageRegion.empty() && context && context->activated)
                {
                    auto reply = context->x11display->copyRootImageRegion(damageRegion);
		    clientUpdatePartFlag = true;
		
                    try
		    {
			if(clientUpdate(*freeRdpClient->peer, damageRegion, reply))
                	{
                    	    context->x11display->damageSubtrack(damageRegion);
                    	        damageRegion.reset();
                	}
		    }
		    catch(const LTSM::Connector::CodecFailed & ex)
		    {
			Application::error("exception: %s", ex.err.c_str());
			loopShutdownFlag = true;
		    }
		    clientUpdatePartFlag = false;
                }
            } // xcb not disabled messages

            // dbus processing
            _conn->enterEventLoopAsync();

            // wait
            std::this_thread::sleep_for(1ms);
	}

        freeRdpClient->stopEventLoop();
        if(freeRdpThread.joinable()) freeRdpThread.join();

	proxyShutdown();

        return EXIT_SUCCESS;
    }

    bool Connector::RDP::createX11Session(void)
    {
        int screen = busStartLoginSession(_remoteaddr, "rdp");
        if(screen <= 0)
        {
            Application::error("%s", "login session request failure");
            return false;
        }

        Application::debug("login session request success, display: %d", screen);

        if(! xcbConnect(screen))
        {
            Application::error("%s", "xcb connect failed");
            return false;
        }

        Application::info("xcb max request: %d", _xcbDisplay->getMaxRequest());

        // wait widget started signal(onHelperWidgetStarted), 3000ms, 10 ms pause
        if(! Tools::waitCallable<std::chrono::milliseconds>(3000, 10,
            [=](){ _conn->enterEventLoopAsync(); return ! this->helperStartedFlag; }))
        {
            Application::error("connector starting: %s", "something went wrong...");
            return false;
        }

	return true;
    }

    void Connector::RDP::onLoginSuccess(const int32_t & display, const std::string & userName)
    {
        if(0 < _display && display == _display)
        {
            xcbReleaseInputsEvent();

	    // disable xcb messages processing 
            setEnableXcbMessages(false);

	    // wait client update canceled, 1000ms, 10 ms pause
	    if(clientUpdatePartFlag)
    		Tools::waitCallable<std::chrono::milliseconds>(1000, 10, [=](){ return !!this->clientUpdatePartFlag; });

	    // switch display
            SignalProxy::onLoginSuccess(display, userName);

	    // update context
	    freeRdpClient->context->x11display = _xcbDisplay;
	    freeRdpClient->context->bytePerPixel = _xcbDisplay->bitsPerPixel() >> 3;
            setEnableXcbMessages(true);

            // full update
            _xcbDisplay->damageAdd(_xcbDisplay->region());
        }
    }

    void Connector::RDP::onShutdownConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            setEnableXcbMessages(false);
            loopShutdownFlag = true;
            Application::notice("dbus signal: shutdown connector, display: %d", display);
        }
    }

    void Connector::RDP::onHelperWidgetStarted(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::notice("dbus signal: helper started, display: %d", display);
            helperStartedFlag = true;
        }
    }

    BOOL Connector::RDP::clientAuthenticate(freerdp_peer* client, const char** user, const char** domain, const char** password)
    {
        Application::notice("%s: client:%p", __FUNCTION__, client);
	return TRUE;
    }

    BOOL Connector::RDP::clientPostConnect(freerdp_peer* client)
    {
        //Application::error("%s: client:%p", __FUNCTION__, client);
        Application::notice("%s: client:%p, desktop:%dx%d, colordepth: %d", __FUNCTION__, client, client->settings->DesktopWidth, client->settings->DesktopHeight, client->settings->ColorDepth);

	auto context = static_cast<ClientContext*>(client->context);
        auto connector = context->rdp;

	if(! connector->createX11Session())
	    return FALSE;

	context->x11display = connector->_xcbDisplay;
	if(! context->x11display->setScreenSize(client->settings->DesktopWidth, client->settings->DesktopHeight))
    	    Application::error("%s: x11display set size: failed", __FUNCTION__);

        context->bytePerPixel = context->x11display->bitsPerPixel() >> 3;
	auto wsz = context->x11display->size();
        Application::error("%s: x11display size: %dx%d", __FUNCTION__, wsz.width, wsz.height);

        client->settings->DesktopWidth = wsz.width;
        client->settings->DesktopHeight = wsz.height;
        client->settings->ColorDepth = context->x11display->bitsPerPixel();
        client->update->DesktopResize(client->update->context);

	// rfx activate
	if(client->settings->RemoteFxCodec)
	{
    	    context->rfx = { rfx_context_new(TRUE), rfx_context_free };
    	    if(! context->rfx)
	    {
		Application::error("%s: failed", "rfx_context");
		return FALSE;
	    }

	    // context->rfx->mode = RLGR1;
    	    //context->rfx->width = client->settings->DesktopWidth;
    	    //context->rfx->height = client->settings->DesktopHeight;
    	    rfx_context_set_pixel_format(context->rfx.get(), PIXEL_FORMAT_BGRX32);
	}

        // planar activate
        DWORD planarFlags = PLANAR_FORMAT_HEADER_RLE;

        if(client->settings->DrawAllowSkipAlpha)
            planarFlags |= PLANAR_FORMAT_HEADER_NA;

	context->planar = { freerdp_bitmap_planar_context_new(planarFlags, 64, 64), freerdp_bitmap_planar_context_free };
	if(! context->planar)
	{
	    Application::error("%s: failed", "bitmap_planar_context");
            return FALSE;
	}

        return TRUE;
    }

    BOOL Connector::RDP::clientActivate(freerdp_peer* client)
    {
        Application::notice("%s: client:%p, desktop:%dx%d, colordepth: %d", __FUNCTION__, client, client->settings->DesktopWidth, client->settings->DesktopHeight, client->settings->ColorDepth);

	auto context = static_cast<ClientContext*>(client->context);
        auto connector = context->rdp;

	// client->settings->CompressionLevel = PACKET_COMPR_TYPE_8K;
        // client->settings->CompressionLevel = PACKET_COMPR_TYPE_64K;
        // client->settings->CompressionLevel = PACKET_COMPR_TYPE_RDP6;
        // client->settings->CompressionLevel = PACKET_COMPR_TYPE_RDP61;

	if(client->settings)
	{
    	    Application::info("client: %s: 0x%08x", "RdpVersion", client->settings->RdpVersion);
    	    Application::info("client: %s: %s", "ClientProductId", client->settings->ClientProductId);
    	    Application::info("client: %s: %s", "AutoLogonEnabled", (client->settings->AutoLogonEnabled ? "true" : "false"));
    	    Application::info("client: %s: %s", "CompressionEnabled", (client->settings->CompressionEnabled ? "true" : "false"));
    	    Application::info("client: %s: %s", "RemoteFxCodec", (client->settings->RemoteFxCodec ? "true" : "false"));
    	    Application::info("client: %s: %s", "NSCodec", (client->settings->NSCodec ? "true" : "false"));
    	    Application::info("client: %s: %s", "JpegCodec", (client->settings->JpegCodec ? "true" : "false"));
    	    Application::info("client: %s: %s", "FrameMarkerCommandEnabled", (client->settings->FrameMarkerCommandEnabled ? "true" : "false"));
    	    Application::info("client: %s: %s", "SurfaceFrameMarkerEnabled", (client->settings->SurfaceFrameMarkerEnabled ? "true" : "false"));
    	    Application::info("client: %s: %s", "SurfaceCommandsEnabled", (client->settings->SurfaceCommandsEnabled ? "true" : "false"));
    	    Application::info("client: %s: %s", "FastPathInput", (client->settings->FastPathInput ? "true" : "false"));
    	    Application::info("client: %s: %s", "UnicodeInput", (client->settings->UnicodeInput ? "true" : "false"));
    	    Application::info("client: %s: %s", "BitmapCacheEnabled", (client->settings->BitmapCacheEnabled ? "true" : "false"));
    	    Application::info("client: %s: %s", "DesktopResize", (client->settings->DesktopResize ? "true" : "false"));
    	    Application::info("client: %s: %s", "RefreshRect", (client->settings->RefreshRect ? "true" : "false"));
    	    Application::info("client: %s: %s", "SuppressOutput", (client->settings->SuppressOutput ? "true" : "false"));
    	    Application::info("client: %s: %s", "FastPathOutput", (client->settings->FastPathOutput ? "true" : "false"));
    	    Application::info("client: %s: %s", "TlsSecurity", (client->settings->TlsSecurity ? "true" : "false"));
    	    Application::info("client: %s: %s", "NlaSecurity", (client->settings->NlaSecurity ? "true" : "false"));
    	    Application::info("client: %s: %s", "RdpSecurity", (client->settings->RdpSecurity ? "true" : "false"));
    	    Application::info("client: %s: %s", "SoundBeepsEnabled", (client->settings->SoundBeepsEnabled ? "true" : "false"));
    	    Application::info("client: %s: %d", "AuthenticationLevel", client->settings->AuthenticationLevel);
    	    Application::info("client: %s: %s", "AllowedTlsCiphers", client->settings->AllowedTlsCiphers);
    	    Application::info("client: %s: %d", "TlsSecLevel", client->settings->TlsSecLevel);
    	    Application::info("client: %s: %d", "EncryptionMethods", client->settings->EncryptionMethods);
    	    Application::info("client: %s: %d", "EncryptionLevel", client->settings->EncryptionLevel);
    	    Application::info("client: %s: %d", "CompressionLevel", client->settings->CompressionLevel);
    	    Application::info("client: %s: %d", "MultifragMaxRequestSize", client->settings->MultifragMaxRequestSize);
	}
 
        context->activated = TRUE;
	connector->setEnableXcbMessages(true);

        const XCB::Region damage(0, 0, client->settings->DesktopWidth, client->settings->DesktopHeight);
	context->x11display->damageAdd(damage);

        return TRUE;
    }

    BOOL Connector::RDP::clientKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code)
    {
        Application::notice("%s: flags:0x%04X, code:0x%04X, input:%p, context:%p", __FUNCTION__, flags, code, input, input->context);
	auto context = static_cast<ClientContext*>(input->context);

	if(context->rdp->isAllowXcbMessages())
        {
	    if(flags & KBD_FLAGS_EXTENDED)
                code |= KBDEXT;

	    // winpr: input
    	    auto vkcode = GetVirtualKeyCodeFromVirtualScanCode(code, 4);

	    if(flags & KBD_FLAGS_EXTENDED)
                vkcode |= KBDEXT;
        
	    // winpr: input
    	    //auto keycode = GetKeycodeFromVirtualKeyCode(vkcode, KEYCODE_TYPE_EVDEV);

	    auto keyCodes = context->x11display->keysymToKeycodes(vkcode);

	    if(flags & KBD_FLAGS_DOWN)
		context->x11display->fakeInputKeysym(XCB_KEY_PRESS, keyCodes);
	    else
	    if(flags & KBD_FLAGS_RELEASE)
		context->x11display->fakeInputKeysym(XCB_KEY_RELEASE, keyCodes);
	}

        return TRUE;
    }

    BOOL Connector::RDP::clientMouseEvent(rdpInput* input, UINT16 flags, UINT16 posx, UINT16 posy)
    {
        Application::notice("%s: flags:0x%04X, pos:%d,%d, input:%p, context:%p", __FUNCTION__, flags, posx, posy, input, input->context);
	auto context = static_cast<ClientContext*>(input->context);

	auto pressedMaskUpdate = [context](bool pressed, uint16_t button)
	{
	    if(pressed)
		context->pressedMask |= button;
	    else
		context->pressedMask &= ~button;
	};

	if(context->rdp->isAllowXcbMessages())
        {
	    if(flags & PTR_FLAGS_BUTTON1)
	    {
        	context->x11display->fakeInputMouse(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, 1, posx, posy);
		pressedMaskUpdate(flags & PTR_FLAGS_DOWN, PTR_FLAGS_BUTTON1);
	    }
	    else
	    if(flags & PTR_FLAGS_BUTTON2)
	    {
        	context->x11display->fakeInputMouse(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, 2, posx, posy);
		pressedMaskUpdate(flags & PTR_FLAGS_DOWN, PTR_FLAGS_BUTTON2);
	    }
	    else
	    if(flags & PTR_FLAGS_BUTTON3)
	    {
        	context->x11display->fakeInputMouse(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, 3, posx, posy);
		pressedMaskUpdate(flags & PTR_FLAGS_DOWN, PTR_FLAGS_BUTTON3);
	    }
	    else
	    if(flags & PTR_FLAGS_WHEEL)
        	context->x11display->fakeInputMouse(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, flags & PTR_FLAGS_WHEEL_NEGATIVE ? 5 : 4, posx, posy);

	    if(flags & PTR_FLAGS_MOVE)
        	context->x11display->fakeInputMouse(XCB_MOTION_NOTIFY, 0, posx, posy);
	}

        return TRUE;
    }

    BOOL Connector::RDP::clientRefreshRect(rdpContext* rdpctx, BYTE count, const RECTANGLE_16* areas)
    {
        Application::notice("%s: count rects:%d, context:%p", __FUNCTION__, (int) count, rdpctx);

	auto context = static_cast<ClientContext*>(rdpctx);
        std::vector<xcb_rectangle_t> rectangles(0 < count ? count : 1);

	if(count && areas)
        {
            for(int it = 0; it < count; ++it)
            {
		Application::info("client requested to refresh area[%d](left:%d,right:%d,top:%d,bottom:%d)", it, areas[it].left, areas[it].right, areas[it].top, areas[it].bottom);
                rectangles[it].x = areas[it].left;
                rectangles[it].y = areas[it].top;
                rectangles[it].width = areas[it].right - areas[it].left + 1;
                rectangles[it].height = areas[it].bottom - areas[it].top + 1;
            }
        }
        else
        {
            auto wsz = context->x11display->size();
            rectangles[0].x = 0;
            rectangles[0].y = 0;
            rectangles[0].width = wsz.width;
            rectangles[0].height = wsz.height;
        }

        return context->x11display->damageAdd(rectangles.data(), rectangles.size());
    }

    BOOL Connector::RDP::clientSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* area)
    {
        Application::notice("%s: allow:0x%02X, context:%p", __FUNCTION__, (int) allow, context);

	if(0 > area && allow)
        {
    	    Application::error("%s: client restore output(%d,%d,%d,%d)", __FUNCTION__, area->left, area->top, area->right, area->bottom);
        }
        else
        {
            Application::error("%s: client minimized and suppress output", __FUNCTION__);
        }

        return TRUE;
    }

    // client update wrapper
    bool Connector::RDP::clientUpdate(freerdp_peer & peer, const XCB::Region & reg, const XCB::PixmapInfoReply & reply)
    {
	if(peer.settings->RemoteFxCodec)
	    return clientUpdateRemoteFX(peer, reg, reply);
	
	return clientUpdateBitmap(peer, reg, reply);
    }

    bool Connector::RDP::clientUpdateRemoteFX(freerdp_peer & peer, const XCB::Region & reg, const XCB::PixmapInfoReply & reply)
    {
        Application::notice("%s: area [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

	auto context = static_cast<ClientContext*>(peer.context);

	const int alignRow = reply->size() > (reg.width * reg.height * context->bytePerPixel) ?
    		reply->size() / (reg.height * context->bytePerPixel) - reg.width : 0;

    	if(! rfx_context_reset(context->rfx.get(), reg.width, reg.height))
	{
	    Application::error("%s: failed", "rfx_context_reset");
	    throw CodecFailed("rfx_context_reset");
	}

	std::unique_ptr<wStream, decltype(stream_free)*> st = { Stream_New(NULL, 0xFFFF), stream_free };
        if(! st)
        {
            Application::error("%s: failed", "Stream_New");
	    throw CodecFailed("Stream_New");
        }

        const RFX_RECT rect{ uint16_t(reg.x), uint16_t(reg.y), reg.width, reg.height };
        if(! rfx_compose_message(context->rfx.get(), st.get(), &rect, 1,  reply->data(), rect.width, rect.height, context->bytePerPixel * rect.width + alignRow))
        {
            Application::error("%s: rfx_compose_message failed, align: %d", __FUNCTION__, alignRow);
	    throw CodecFailed("rfx_compose_message");
        }

        SURFACE_BITS_COMMAND cmd = { 0 };

        cmd.bmp.codecID = peer.settings->RemoteFxCodecId;
        cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
        cmd.destLeft = rect.x;
        cmd.destTop = rect.y;
        cmd.destRight = rect.x + rect.width - 1;
        cmd.destBottom = rect.y + rect.height - 1;
        cmd.bmp.bpp = context->bytePerPixel << 3;
        cmd.bmp.flags = 0;
        cmd.bmp.width = rect.width;
        cmd.bmp.height = rect.height;
        cmd.bmp.bitmapDataLength = Stream_GetPosition(st.get());
        cmd.bmp.bitmapData = Stream_Buffer(st.get());

        // begin_frame
        SURFACE_FRAME_MARKER fm = { 0 };
        fm.frameAction = SURFACECMD_FRAMEACTION_BEGIN;
        fm.frameId = context->frameId;
        peer.update->SurfaceFrameMarker(peer.update->context, &fm);

	peer.update->SurfaceBits(peer.update->context, &cmd);

        // end_frame
        fm.frameAction = SURFACECMD_FRAMEACTION_END;
        fm.frameId = context->frameId;
        peer.update->SurfaceFrameMarker(peer.update->context, &fm);
        context->frameId++;

	return true;
    }

    bool Connector::RDP::clientUpdateBitmap(freerdp_peer & peer, const XCB::Region & reg, const XCB::PixmapInfoReply & reply)
    {
        Application::notice("%s: area [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

	auto context = static_cast<ClientContext*>(peer.context);

    	if(reply->size() != reg.height * reg.width * context->bytePerPixel)
	{
	    Application::error("%s: %s failed", __FUNCTION__, "align region");
            throw CodecFailed("region not aligned");
	}

	// fixme color depth check
        if(peer.settings->ColorDepth != 32)
	{
	    Application::error("%s: %s failed", __FUNCTION__, "true colors");
            throw CodecFailed("true colors only");
	}

        const size_t scanLineBytes = reg.width * context->bytePerPixel;
        const size_t tileSize = 256;

        if(! freerdp_bitmap_planar_context_reset(context->planar.get(), tileSize, tileSize))
        {
	    Application::error("%s: %s failed", __FUNCTION__, "bitmap_planar_context_reset");
            throw CodecFailed("bitmap_planar_context_reset");
        }

	auto blocks = reg.divideBlocks(tileSize, tileSize);

        // Compressed header of bitmap
        // http://msdn.microsoft.com/en-us/library/cc240644.aspx
 
        BITMAP_DATA st = {0};
        BITMAP_UPDATE bitmapUpdate = {0};

        for(auto & subreg : blocks)
        {
	    const int16_t localX = subreg.x - reg.x;
	    const int16_t localY = subreg.y - reg.y;
            const size_t offset = localY * scanLineBytes + localX * context->bytePerPixel;

            // Bitmap data here the screen capture
            // https://msdn.microsoft.com/en-us/library/cc240612.aspx
            st.destLeft = subreg.x;
            st.destTop = subreg.y;
            st.destRight = subreg.x + subreg.width - 1;
            st.destBottom = subreg.y + subreg.height - 1;
            st.width = subreg.width;
            st.height = subreg.height;
            st.bitsPerPixel = context->bytePerPixel << 3;
            st.compressed = TRUE;

	    // FIXME COLORS
            if(peer.settings->ColorDepth == 32)
            {
                st.cbScanWidth = subreg.width * context->bytePerPixel;
                st.cbUncompressedSize = subreg.height * st.cbScanWidth;

                st.bitmapDataStream = freerdp_bitmap_compress_planar(context->planar.get(), reply->data() + offset,
                                        PIXEL_FORMAT_BGRX32, subreg.width, subreg.height, scanLineBytes, NULL, & st.bitmapLength);

                st.cbCompMainBodySize = st.bitmapLength;
            }

            bitmapUpdate.count = bitmapUpdate.number = 1;
            bitmapUpdate.rectangles = & st;

            auto ret = peer.update->BitmapUpdate(peer.context, &bitmapUpdate);
            if(! ret)
            {
                Application::error("%s: BitmapUpdate failed", __FUNCTION__);
                return false;
            }
        }

	return true;
    }
}
