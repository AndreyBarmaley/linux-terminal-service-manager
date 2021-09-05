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
#include <winpr/wtsapi.h>
#include <winpr/version.h>

#include <freerdp/freerdp.h>
#include <freerdp/version.h>
#include <freerdp/constants.h>
#include <freerdp/codec/region.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/channels.h>

#include "ltsm_tools.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_connector_rdp.h"

using namespace std::chrono_literals;

namespace LTSM
{
    struct ClientContext : rdp_context
    {
        Connector::RDP*	rdp;
        wStream*	st;
        RFX_CONTEXT*	rfx;
        UINT32		frameId;
        BOOL		activated;
	HANDLE		vcm;
	REGION16	invalidRegion;

	std::mutex	lock;
	int	        test1;
	int	        test2;
    };

    // FreeRDP
    int clientContextNew(rdp_freerdp_peer* client, ClientContext* context)
    {
        context->rfx = rfx_context_new(TRUE);
        if(! context->rfx)
	{
	    Application::error("%s: failed", "rfx_context");
	    return FALSE;
	}

	context->st = Stream_New(NULL, 0xFFFF);
        if(! context->st)
	{
	    Application::error("%s: failed", "Stream_New");
	    return FALSE;
	}

        context->vcm = WTSOpenServerA((LPSTR) client->context);
	if(!context->vcm || context->vcm == INVALID_HANDLE_VALUE)
	{
	    Application::error("%s: failed", "WTSOpenServer");
            return FALSE;
	}

	context->rfx->mode = RLGR3;
    	context->rfx->width = 1024; //client->settings->DesktopWidth;
    	context->rfx->height = 7680; //client->settings->DesktopHeight;
    	rfx_context_set_pixel_format(context->rfx, PIXEL_FORMAT_BGRA32);
	region16_init(& context->invalidRegion);
        Application::info("%s: success", __FUNCTION__);
	return TRUE;
    }

    void clientContextFree(rdp_freerdp_peer* client, ClientContext* context)
    {
	if(context)
	{
	    region16_uninit(& context->invalidRegion);
	    Stream_Free(context->st, TRUE);
            rfx_context_free(context->rfx);
	    WTSCloseServer(context->vcm);
    	    Application::info("%s: success", __FUNCTION__);
	}
    }

    class FreeRdpClient
    {
	freerdp_peer*		peer;
	ClientContext*		context;
        std::atomic<HANDLE>	stopEvent;

    public:
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
		auto str = Tools::lower(config.getString("rdp:freerdp:wlog"));
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
	    peer->ContextFree = (psPeerContextFree) clientContextFree;

            if(! freerdp_peer_context_new(peer))
            {
                Application::error("%s: failed", "freerdp_peer_context_new");
                throw EXIT_FAILURE;
            }

	    Application::error("peer context: %p", peer);
	    Application::error("client context: %p", peer->context);

	    context = static_cast<ClientContext*>(peer->context);
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

            peer->settings->RdpSecurity = TRUE;
            peer->settings->TlsSecurity = FALSE;
            peer->settings->NlaSecurity = FALSE;
            peer->settings->ExtSecurity = FALSE;
            peer->settings->UseRdpSecurityLayer = TRUE;
	    // ENCRYPTION_LEVEL_NONE ENCRYPTION_LEVEL_CLIENT_COMPATIBLE ENCRYPTION_LEVEL_HIGH, ENCRYPTION_LEVEL_LOW, ENCRYPTION_LEVEL_FIPS
            peer->settings->EncryptionLevel = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;

            peer->settings->ColorDepth = 32;
	    peer->settings->NSCodec = TRUE;
	    peer->settings->RemoteFxCodec = TRUE;
            peer->settings->RefreshRect = TRUE;
	    peer->settings->SuppressOutput = TRUE;
	    peer->settings->FrameMarkerCommandEnabled = TRUE;
	    peer->settings->SurfaceFrameMarkerEnabled = TRUE;

	    peer->PostConnect = Connector::RDP::clientPostConnect;
	    peer->Activate = Connector::RDP::clientActivate;
            peer->input->KeyboardEvent = Connector::RDP::clientKeyboardEvent;
	    peer->input->UnicodeKeyboardEvent = Connector::RDP::clientUnicodeKeyboardEvent;
            peer->input->MouseEvent = Connector::RDP::clientMouseEvent;
            peer->input->ExtendedMouseEvent = Connector::RDP::clientExtendedMouseEvent;
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
            {
                stopEventLoop();
                std::this_thread::sleep_for(200ms);
            }

    	    if(peer)
	    {
		peer->Disconnect(peer);
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
                CloseHandle(stopEvent);
                stopEvent = nullptr;
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

                if(WTSVirtualChannelManagerCheckFileDescriptor(client->context->vcm) != TRUE)
                    break;

		if(WaitForSingleObject(client->stopEvent, 1) == WAIT_OBJECT_0)
		    break;

    		if(peer->activated == TRUE)
		{
    		    //event = mf_event_peek(info_event_queue);
            	    // mf_peer_rfx_update(client);
                }
/*
                handles[count++] = WTSVirtualChannelManagerGetEventHandle(context->vcm);
                status = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
        
                if (status == WAIT_FAILED)
                {
                        WLog_ERR(TAG, "WaitForMultipleObjects failed (errno: %d)", errno);
                        break;
                }
                        
                if (peer->CheckFileDescriptor(client) != TRUE)
                        break;
*/
                // wait
                std::this_thread::sleep_for(1ms);
            }

            CloseHandle(client->stopEvent);
            client->stopEvent = nullptr;
	    peer->Disconnect(client->peer);

	    Application::info("%s: loop shutdown", "FreeRdpClient");
            return true;
        }
    };

    /* Connector::RDP */
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
    	Application::info("%s", "create freerdp client");
        auto freeRdpClient = std::unique_ptr<FreeRdpClient>(new FreeRdpClient(clientSocket(), _remoteaddr, *_config, this));
        auto freeRdpThread = std::thread([ptr = freeRdpClient.get()]{ FreeRdpClient::enterEventLoop(ptr); });

	// all ok
	while(! loopShutdownFlag)
	{
            if(freeRdpClient->isShutdown())
		loopShutdownFlag = true;

	    if(! ProxySocket::enterEventLoopAsync())
		loopShutdownFlag = true;

            // dbus processing
            _conn->enterEventLoopAsync();

            // wait
            std::this_thread::sleep_for(1ms);
	}

	proxyShutdown();
        freeRdpClient->stopEventLoop();
        freeRdpThread.join();

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
            Application::info("connector starting: %s", "something went wrong...");
            return false;
        }

	return true;
    }

    void Connector::RDP::onShutdownConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            _xcbDisableMessages = true;
            loopShutdownFlag = true;
            Application::info("dbus signal: shutdown connector, display: %d", display);
        }
    }

    void Connector::RDP::onHelperWidgetStarted(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: helper started, display: %d", display);
            helperStartedFlag = true;
        }
    }

    BOOL Connector::RDP::clientAuthenticate(freerdp_peer* client, const char** user, const char** domain, const char** password)
    {
        Application::error("%s: client:%p", __FUNCTION__, client);
	return TRUE;
    }

    BOOL Connector::RDP::clientPostConnect(freerdp_peer* client)
    {
        Application::error("%s: client:%p", __FUNCTION__, client);
        Application::error("%s: client geometry:%dx%d", __FUNCTION__, client->settings->DesktopWidth, client->settings->DesktopHeight);

	if(client->settings->MultifragMaxRequestSize < 0x3F0000)
                client->settings->NSCodec = FALSE; /* NSCodec compressor does not support fragmentation yet */

	if(client->settings->ColorDepth == 24)
                client->settings->ColorDepth = 16; /* disable 24bpp */

	//auto context = static_cast<ClientContext*>(client->context);

        client->settings->DesktopWidth = 1024;
        client->settings->DesktopHeight = 768;
        client->settings->ColorDepth = 32;
        client->update->DesktopResize(client->update->context);

        return TRUE;
    }

    BOOL Connector::RDP::clientActivate(freerdp_peer* client)
    {
        Application::error("%s: client:%p", __FUNCTION__, client);

	auto context = static_cast<ClientContext*>(client->context);
        auto connector = context->rdp;

	if(! connector->createX11Session())
	    return FALSE;

        rfx_context_reset(context->rfx, client->settings->DesktopWidth, client->settings->DesktopHeight);
        context->activated = TRUE;

        // mark invalid
	const std::lock_guard<std::mutex> lock(context->lock);
	RECTANGLE_16 screenRegion;
	screenRegion.left = 0;
	screenRegion.top = 0;
	screenRegion.right = client->settings->DesktopWidth;
	screenRegion.bottom = client->settings->DesktopHeight;
        region16_union_rect(& context->invalidRegion, & context->invalidRegion, &screenRegion);

        return TRUE;
    }

    BOOL Connector::RDP::clientKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code)
    {
        Application::error("%s: flags:0x%04X, code:0x%04X, input:%p", __FUNCTION__, flags, code, input);
        return TRUE;
    }

    BOOL Connector::RDP::clientUnicodeKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code)
    {
        Application::error("%s: flags:0x%04X, code:0x%04X, input:%p", __FUNCTION__, flags, code, input);
        return FALSE;
    }

    BOOL Connector::RDP::clientMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
    {
        Application::error("%s: flags:0x%04X, pos:%d,%d, input:%p", __FUNCTION__, flags, x, y, input);
        return TRUE;
    }

    BOOL Connector::RDP::clientExtendedMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
    {
        Application::error("%s: flags:0x%04X, pos:%d,%d, input:%p, context:%p", __FUNCTION__, flags, x, y, input, input->context);
        return FALSE;
    }

    BOOL Connector::RDP::clientRefreshRequest(freerdp_peer* client)
    {
/*
	auto context = static_cast<ClientContext*>(client->context);
	if(client->subsystem)
	{
    	    wMessage message = { 0 };
    	    wMessagePipe* MsgPipe = client->subsystem->MsgPipe;
    	    message.id = SHADOW_MSG_IN_REFRESH_REQUEST_ID;
    	    message.wParam = NULL;
    	    message.lParam = NULL;
    	    message.context = (void*)client;
    	    message.Free = NULL;
    	    return MessageQueue_Dispatch(MsgPipe->In, &message);
	}
*/
	return FALSE;
    }

    BOOL Connector::RDP::clientRefreshRect(rdpContext* context, BYTE count, const RECTANGLE_16* areas)
    {
        Application::error("%s: count rects:%d, context:%p", __FUNCTION__, (int) count, context);
        return FALSE;
    }

    BOOL Connector::RDP::clientSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* area)
    {
        Application::error("%s: allow:0x%02X, context:%p", __FUNCTION__, (int) allow, context);
        return FALSE;
    }
}
