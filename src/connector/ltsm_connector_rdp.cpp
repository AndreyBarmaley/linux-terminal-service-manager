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
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <winpr/crt.h>
#include <winpr/ssl.h>
#include <winpr/wtsapi.h>

#include <freerdp/freerdp.h>
#include <freerdp/constants.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/channels.h>

#include "ltsm_tools.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_connector_rdp.h"

using namespace std::chrono_literals;

namespace LTSM
{
    Connector::ProxySocket::~ProxySocket()
    {
	loopTransmission = false;
	if(loopThread.joinable()) loopThread.join();

        std::filesystem::remove(socketPath);
        if(0 < bridgeSock) close(bridgeSock);
        if(0 < clientSock) close(clientSock);
    }

    int Connector::ProxySocket::clientSocket(void) const
    {
	return clientSock;
    }

    void Connector::ProxySocket::stopEventLoop(void)
    {
	loopTransmission = false;
    }

    void Connector::ProxySocket::startEventLoopBackground(void)
    {
	loopThread = std::thread([this]{
	    while(this->loopTransmission)
	    {
		if(! this->enterEventLoopAsync())
		    break;

		std::this_thread::sleep_for(1ms);
	    }
	    this->loopTransmission = false;
	});
    }

    bool Connector::ProxySocket::enterEventLoopAsync(void)
    {
	// read all data
	while(hasInput())
	{
	    uint8_t ch = recvInt8();
	    buf.push_back(ch);

	}

	if(buf.size())
	{
	    if(buf.size() != send(bridgeSock, buf.data(), buf.size(), 0))
	    {
		Application::error("unix send error: %s", strerror(errno));
		return false;
	    }

#ifdef LTSM_DEBUG
	    if(! checkError())
	    {
		std::string str = Tools::vector2hexstring<uint8_t>(buf, 2);
		Application::info("from rdesktop: [%s]", str.c_str());
	    }
#endif
	    buf.clear();
	}

	if(checkError())
	    return false;

	// write all data
	while(hasInput(bridgeSock))
	{
	    uint8_t ch;
	    if(1 != recv(bridgeSock, & ch, 1, 0))
	    {
		Application::error("unix recv error: %s", strerror(errno));
		return false;
	    }

	    buf.push_back(ch);
	}

	if(buf.size())
	{
	    sendRaw(buf.data(), buf.size());
	    sendFlush();

#ifdef LTSM_DEBUG
	    if(! checkError())
	    {
		std::string str = Tools::vector2hexstring<uint8_t>(buf, 2);
		Application::info("from freerdp: [%s]", str.c_str());
	    }
#endif
	    buf.clear();
	}

	return ! checkError();
    }

    int Connector::ProxySocket::connectUnixSocket(const char* path)
    {
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if(0 > sock)
	{
	    Application::error("socket failed: %s", strerror(errno));
	    return -1;
	}

	struct sockaddr_un sockaddr;
	memset(& sockaddr, 0, sizeof(struct sockaddr_un));
	sockaddr.sun_family = AF_UNIX;   
	std::strcpy(sockaddr.sun_path, path);

        if(0 != connect(sock, (struct sockaddr*) &sockaddr,  sizeof(struct sockaddr_un)))
	    Application::error("connect failed: %s, socket: %s", strerror(errno), path);
        else
	    Application::info("connect unix sock fd: %d", sock);

        return sock;
    }

    int Connector::ProxySocket::listenUnixSocket(const char* path)
    {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(0 > fd)
	{
	    Application::error("socket failed: %s", strerror(errno));
	    return -1;
	}

	struct sockaddr_un sockaddr;
	memset(& sockaddr, 0, sizeof(struct sockaddr_un));
	sockaddr.sun_family = AF_UNIX;   
	std::strcpy(sockaddr.sun_path, path);

	std::filesystem::remove(path);
	if(0 != bind(fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_un)))
	{
	    Application::error("bind failed: %s, socket: %s", strerror(errno), path);
	    return -1;
	}

	if(0 != listen(fd, 5))
	{
	    Application::error("listen failed: %s", strerror(errno));
	    return -1;
	}
	Application::info("listen unix sock: %s", path);

	int sock = accept(fd, nullptr, nullptr);
	if(0 > sock)
	    Application::error("accept failed: %s", strerror(errno));
        else
	    Application::info("accept unix sock: %s", path);

	close(fd);
	return sock;
    }

    bool Connector::ProxySocket::initUnixSockets(const std::string & path)
    {
	socketPath = path;
	std::future<int> job = std::async(std::launch::async, Connector::ProxySocket::listenUnixSocket, socketPath.c_str());

	Application::info("wait server socket: %s", socketPath.c_str());
        while(! std::filesystem::is_socket(socketPath.c_str()))
            std::this_thread::sleep_for(1ms);

	bridgeSock = -1;
	// socket fd: client part
	clientSock = connectUnixSocket(socketPath.c_str());
	if(0 < clientSock)
	{
    	    while(job.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready);
	    // socket fd: server part
    	    bridgeSock = job.get();

    	    return 0 < bridgeSock;
        }

	Application::error("%s: failed", "init unix sockets");
        return false;
    }


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
	    winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
	    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());

	    // init freerdp log system
	    auto log = WLog_GetRoot();
            if(log)
	    {
	        WLog_SetLogAppenderType(log, WLOG_APPENDER_SYSLOG);
	        WLog_SetLogLevel(log, WLOG_DEBUG);
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

            auto certfile = config.getString("rdp:server:certfile");
            auto keyfile = config.getString("rdp:server:keyfile");

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
            peer->settings->TlsSecurity = TRUE;
            peer->settings->NlaSecurity = FALSE;
            peer->settings->ExtSecurity = FALSE;
            peer->settings->UseRdpSecurityLayer = FALSE;
	    // ENCRYPTION_LEVEL_NONE ENCRYPTION_LEVEL_CLIENT_COMPATIBLE ENCRYPTION_LEVEL_HIGH, ENCRYPTION_LEVEL_LOW, ENCRYPTION_LEVEL_FIPS
            peer->settings->EncryptionLevel = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;

            peer->settings->RemoteFxCodec = TRUE;
	    peer->settings->NSCodec = FALSE;
	    peer->settings->RemoteFxCodec = FALSE;
            peer->settings->ColorDepth = 32;
            peer->settings->RefreshRect = FALSE;
	    peer->settings->SuppressOutput = TRUE;

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
	const auto socket = std::filesystem::path(home) / std::string("rdp_pid").append(std::to_string(getpid()));

        if(! initUnixSockets(socket.string()))
            return EXIT_SUCCESS;

        // create X11 connect
        int screen = busStartLoginSession(_remoteaddr, "rdp");
        if(screen <= 0)
        {
            Application::error("%s", "login session request failure");
            return EXIT_FAILURE;
        }

        Application::debug("login session request success, display: %d", screen);

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

        // wait widget started signal
        while(! loopMessage)
        {
            // dbus processing
            _conn->enterEventLoopAsync();
            // wait
            std::this_thread::sleep_for(1ms);
        }
    
        // _xcbDisplay->bitsPerPixel(), _xcbDisplay->depth(),
        // visual->red_mask, visual->green_mask, visual->blue_mask

        // create FreeRdpClient
    	Application::info("create freerdp client");
        auto freeRdpClient = std::unique_ptr<FreeRdpClient>(new FreeRdpClient(clientSocket(), _remoteaddr, *_config, this));
        auto freeRdpThread = std::thread([ptr = freeRdpClient.get()]{ FreeRdpClient::enterEventLoop(ptr); });

	// all ok
	while(loopMessage)
	{
            if(freeRdpClient->isShutdown())
                loopMessage = false;

	    if(! ProxySocket::enterEventLoopAsync())
		loopMessage = false;

            // dbus processing
            _conn->enterEventLoopAsync();

            // wait
            std::this_thread::sleep_for(1ms);
	}

        freeRdpClient->stopEventLoop();
        freeRdpThread.join();

        return EXIT_SUCCESS;
    }

    void Connector::RDP::onShutdownConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            _xcbDisableMessages = true;
            loopMessage = false;
            Application::info("dbus signal: shutdown connector, display: %d", display);
        }
    }

    void Connector::RDP::onHelperWidgetStarted(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: helper started, display: %d", display);
            loopMessage = true;
        }
    }

    BOOL Connector::RDP::clientPostConnect(freerdp_peer* client)
    {
        Application::error("%s: client:%p", __FUNCTION__, client);

	//auto context = static_cast<ClientContext*>(client->context);
        UINT32 bitsPerPixel = 32;

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
        return FALSE;
    }

    BOOL Connector::RDP::clientUnicodeKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code)
    {
        Application::error("%s: flags:0x%04X, code:0x%04X, input:%p", __FUNCTION__, flags, code, input);
        return FALSE;
    }

    BOOL Connector::RDP::clientMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
    {
        Application::error("%s: flags:0x%04X, pos:%d,%d, input:%p", __FUNCTION__, flags, x, y, input);
        return FALSE;
    }

    BOOL Connector::RDP::clientExtendedMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
    {
        Application::error("%s: flags:0x%04X, pos:%d,%d, input: %p, context:%p", __FUNCTION__, flags, x, y, input, input->context);
        return FALSE;
    }

    BOOL Connector::RDP::clientRefreshRect(rdpContext* context, BYTE count, const RECTANGLE_16* areas)
    {
        Application::error("%s: count rects: %d, context: %p", __FUNCTION__, (int) count, context);
        return FALSE;
    }

    BOOL Connector::RDP::clientSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* area)
    {
        Application::error("%s: allow:0x%02X, context: %p", __FUNCTION__, (int) allow, context);
        return FALSE;
    }
}
