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

#include <list>
#include <cmath>
#include <tuple>
#include <cstdio>
#include <string>
#include <thread>
#include <future>
#include <memory>
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
#include <freerdp/codec/interleaved.h>
#include <freerdp/codec/region.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/cliprdr.h>
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
        BITMAP_PLANAR_CONTEXT* 	planar;
	BITMAP_INTERLEAVED_CONTEXT* interleaved;
        HANDLE			vcm;

        bool			activated;
        size_t			frameId;

	const JsonObject*	config;
	Connector::RDP*		rdp;
	XCB::RootDisplayExt*	x11display;
        JsonObject*             keymap;
    };

    int clientContextNew(rdp_freerdp_peer* client, ClientContext* context)
    {
        context->planar = nullptr;
        context->interleaved = nullptr;

        context->vcm = WTSOpenServerA((LPSTR) client->context);
	if(! context->vcm || context->vcm == INVALID_HANDLE_VALUE)
	{
	    Application::error("%s: failed", "WTSOpenServer");
            return FALSE;
	}

        context->activated = false;
        context->frameId = 0;

	context->config = nullptr;
	context->rdp = nullptr;
	context->x11display = nullptr;
        context->keymap = nullptr;

        Application::info("%s: success", __FUNCTION__);
	return TRUE;
    }

    void clientContextFree(ClientContext* context)
    {
	if(context->planar)
	{
	    freerdp_bitmap_planar_context_free(context->planar);
	    context->planar = nullptr;
	}

	if(context->interleaved)
	{
	    bitmap_interleaved_context_free(context->interleaved);
	    context->interleaved = nullptr;
	}

	if(context->vcm)
	{
	    WTSCloseServer(context->vcm);
	    context->vcm = nullptr;
	}

        if(context->keymap)
        {
            delete context->keymap;
            context->keymap = nullptr;
        }
    }

    // FreeRdp
    struct FreeRdpCallback
    {
	freerdp_peer*		peer;
	ClientContext*		context;
        std::atomic<HANDLE>	stopEvent;

	FreeRdpCallback(int fd, const std::string & remoteaddr, const JsonObject & config, Connector::RDP* connector) : peer(nullptr), context(nullptr)
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
	    peer->ContextFree = (psPeerContextFree) clientContextFree;

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

            const std::string keymapFile = config.getString("rdp:keymap:file");
            if(keymapFile.size())
            {
                JsonContentFile jc(keymapFile);
                if(jc.isValid() && jc.isObject())
                {
		    context->keymap = new JsonObject(jc.toObject());
		    Application::notice("keymap loaded: %s", keymapFile.c_str());
		}
            }

            auto certfile = connector->checkFileOption("rdp:server:certfile");
            if(certfile.size())
	    {
                peer->settings->CertificateFile = strdup(certfile.c_str());
		Application::info("server cert: %s", peer->settings->CertificateFile);
	    }

            auto keyfile = connector->checkFileOption("rdp:server:keyfile");
            if(keyfile.size())
            {
                peer->settings->PrivateKeyFile = strdup(keyfile.c_str());
                peer->settings->RdpKeyFile = strdup(keyfile.c_str());
		Application::info("server key: %s", peer->settings->RdpKeyFile);
            }

	    int encryptionLevel = ENCRYPTION_LEVEL_NONE;
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
            peer->settings->TlsSecLevel = config.getInteger("rdp:tls:level", 1);
            peer->settings->ExtSecurity = FALSE;
            peer->settings->UseRdpSecurityLayer = FALSE;
            peer->settings->EncryptionLevel = encryptionLevel;

	    peer->settings->NSCodec = FALSE;
	    peer->settings->RemoteFxCodec = FALSE;
            peer->settings->RefreshRect = TRUE;
	    peer->settings->SuppressOutput = TRUE;
	    peer->settings->FrameMarkerCommandEnabled = TRUE;
	    peer->settings->SurfaceFrameMarkerEnabled = TRUE;

/*
            if(WTSVirtualChannelManagerIsChannelJoined(context->vcm, CLIPRDR_SVC_CHANNEL_NAME))
                peer->settings->RedirectClipboard = TRUE;
*/

	    peer->PostConnect = Connector::RDP::cbClientPostConnect;
	    peer->Activate = Connector::RDP::cbClientActivate;
            peer->input->KeyboardEvent = Connector::RDP::cbClientKeyboardEvent;
            peer->input->MouseEvent = Connector::RDP::cbClientMouseEvent;
            peer->update->RefreshRect = Connector::RDP::cbClientRefreshRect;
	    peer->update->SuppressOutput = Connector::RDP::cbClientSuppressOutput;

            if(1 != peer->Initialize(peer))
            {
		Application::error("%s: failed", "peer->Initialize");
                throw EXIT_FAILURE;
	    }
	}

	~FreeRdpCallback()
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
	        Application::info("%s: stop event", "FreeRdpCallback");
                SetEvent(stopEvent);
            }
        }

        bool isShutdown(void) const
        {
            return ! stopEvent;
        }

        static bool enterEventLoop(FreeRdpCallback* client)
        {
	    auto error = CHANNEL_RC_OK;
	    HANDLE handles[32] = {0};
	    DWORD count = 0;

	    Application::info("%s: enter event loop", "FreeRdpCallback");
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

	    Application::info("%s: loop shutdown", "FreeRdpCallback");
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

        // create FreeRdpCallback
    	Application::notice("%s", "create freerdp client");
        freeRdp.reset(new FreeRdpCallback(clientSocket(), _remoteaddr, *_config, this));
        auto freeRdpThread = std::thread([ptr = freeRdp.get()]{ FreeRdpCallback::enterEventLoop(ptr); });
	XCB::Region damageRegion(0, 0, 0, 0);

	// all ok
	while(! loopShutdownFlag)
	{
	    if(freeRdp->isShutdown())
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
                            clientDesktopResizeEvent(*freeRdp->peer, cc.width, cc.height);
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

                auto & context = freeRdp->context;
                if(! damageRegion.empty() && context && context->activated)
                {
		    clientUpdatePartFlag = true;

                    try
		    {
			if(clientUpdateEvent(*freeRdp->peer, damageRegion))
                	{
                    	    _xcbDisplay->damageSubtrack(damageRegion);
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

        freeRdp->stopEventLoop();
        if(freeRdpThread.joinable()) freeRdpThread.join();

	proxyShutdown();

        return EXIT_SUCCESS;
    }

    void Connector::RDP::setEncryptionInfo(const std::string & info)
    {
	busSetEncryptionInfo(_display, info);
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

	std::this_thread::sleep_for(50ms);
	return true;
    }

    void Connector::RDP::onLoginSuccess(const int32_t & display, const std::string & userName)
    {
        if(0 < _display && display == _display)
        {
	    // disable xcb messages processing 
            setEnableXcbMessages(false);

	    // wait client update canceled, 1000ms, 10 ms pause
	    if(clientUpdatePartFlag)
    		Tools::waitCallable<std::chrono::milliseconds>(1000, 10, [=](){ return !!this->clientUpdatePartFlag; });

	    // switch display
            SignalProxy::onLoginSuccess(display, userName);

	    // update context
	    freeRdp->context->x11display = _xcbDisplay.get();
            setEnableXcbMessages(true);

	    // fix new session size
            auto wsz = _xcbDisplay->size();
	    if(wsz.width != freeRdp->peer->settings->DesktopWidth || wsz.height != freeRdp->peer->settings->DesktopHeight)
	    {
		if(_xcbDisplay->setScreenSize(freeRdp->peer->settings->DesktopWidth, freeRdp->peer->settings->DesktopHeight))
        	{
		    wsz = _xcbDisplay->size();
		    Application::notice("change session size %dx%d, display: %d", wsz.width, wsz.height, display);
		}
	    }

            // full update
            _xcbDisplay->damageAdd(_xcbDisplay->region());
	    Application::notice("dbus signal: login success, display: %d, username: %s", display, userName.c_str());
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
            helperStartedFlag = true;
	    Application::info("dbus signal: helper started, display: %d", display);
        }
    }

    // client events
    void Connector::RDP::clientDisconnectedEvent(void)
    {
        Application::warning("RDP disconnected, display: %d", _display);
    }

    void Connector::RDP::clientDesktopResizeEvent(freerdp_peer & peer, uint16_t width, uint16_t height)
    {
        Application::debug("%s: [%d,%d]", __FUNCTION__, width, height);
	auto context = static_cast<ClientContext*>(peer.context);

        context->activated = false;
        peer.settings->DesktopWidth = width;
        peer.settings->DesktopHeight = height;

        if(peer.update->DesktopResize(peer.update->context))
            Application::error("%s: [%d,%d] failed", __FUNCTION__, width, height);
    }

    bool Connector::RDP::clientUpdateEvent(freerdp_peer & peer, const XCB::Region & damage)
    {
	auto context = static_cast<ClientContext*>(peer.context);
        auto reply = context->x11display->copyRootImageRegion(damage);

        // reply info dump
        if(Application::isDebugLevel(DebugLevel::SyslogDebug))
        {
            if(const xcb_visualtype_t* visual = reply->visual())
            {
                Application::info("get_image: request size [%d, %d], reply length: %d, depth: %d, bits per rgb value: %d, red: %08x, green: %08x, blue: %08x, color entries: %d",
                        damage.width, damage.height, reply->size(), reply->depth(), visual->bits_per_rgb_value, visual->red_mask, visual->green_mask, visual->blue_mask, visual->colormap_entries);
            }
        }

	return 24 == reply->depth() ?
		clientUpdateBitmapPlanar(peer, damage, reply) : clientUpdateBitmapInterleaved(peer, damage, reply);
    }

    bool Connector::RDP::clientUpdateBitmapPlanar(freerdp_peer & peer, const XCB::Region & reg, const XCB::PixmapInfoReply & reply)
    {
	auto context = static_cast<ClientContext*>(peer.context);

	const int bytePerPixel = context->x11display->bitsPerPixel(reply->depth()) >> 3;
        const size_t scanLineBytes = reg.width * bytePerPixel;
        const size_t tileSize = 64;
	const size_t pixelFormat = PIXEL_FORMAT_BGRX32;

    	if(reply->size() != reg.height * reg.width * bytePerPixel)
	{
	    Application::error("%s: %s failed, length:%d, size:%dx%d, bpp:%d", __FUNCTION__, "align region", reply->size(), reg.height, reg.width, bytePerPixel);
            throw CodecFailed("clientUpdateBitmapPlanar");
	}

        // planar activate
	if(! context->planar)
	{
    	    DWORD planarFlags = PLANAR_FORMAT_HEADER_RLE;

    	    if(peer.settings->DrawAllowSkipAlpha)
        	    planarFlags |= PLANAR_FORMAT_HEADER_NA;

	    context->planar = freerdp_bitmap_planar_context_new(planarFlags, tileSize, tileSize);
	    if(! context->planar)
	    {
		Application::error("%s: %s failed", __FUNCTION__, "bitmap_planar_context_new");
                throw CodecFailed("clientUpdateBitmapPlanar");
	    }
	}

        if(! freerdp_bitmap_planar_context_reset(context->planar, tileSize, tileSize))
        {
	    Application::error("%s: %s failed", __FUNCTION__, "bitmap_planar_context_reset");
            throw CodecFailed("clientUpdateBitmapPlanar");
        }

        Application::debug("%s: area [%d,%d,%d,%d], depth:%d, scanline: %d, bpp:%d", __FUNCTION__, reg.x, reg.y, reg.width, reg.height, reply->depth(), scanLineBytes, bytePerPixel);
	auto blocks = reg.divideBlocks(tileSize, tileSize);

        // Compressed header of bitmap
        // http://msdn.microsoft.com/en-us/library/cc240644.aspx
 
	const size_t hdrsz = 34;
	std::vector<BITMAP_DATA> vec;
	vec.reserve(blocks.size());

        for(auto & subreg : blocks)
        {
	    const int16_t localX = subreg.x - reg.x;
	    const int16_t localY = subreg.y - reg.y;
            const size_t offset = localY * scanLineBytes + localX * bytePerPixel;
	    BITMAP_DATA st = {0};

            // Bitmap data here the screen capture
            // https://msdn.microsoft.com/en-us/library/cc240612.aspx
            st.destLeft = subreg.x;
            st.destRight = subreg.x + subreg.width - 1;
            st.width = subreg.width;
            st.bitsPerPixel = peer.settings->ColorDepth;

            st.compressed = TRUE;
    	    st.height = subreg.height;
    	    st.destTop = subreg.y;
    	    st.destBottom = subreg.y + subreg.height - 1;

    	    st.cbScanWidth = subreg.width * bytePerPixel;
    	    st.cbUncompressedSize = subreg.height * subreg.width * bytePerPixel;
    	    st.bitmapDataStream = freerdp_bitmap_compress_planar(context->planar, reply->data() + offset,
                                        pixelFormat, subreg.width, subreg.height, scanLineBytes, NULL, & st.bitmapLength);
    	    st.cbCompMainBodySize = st.bitmapLength;

	    if(peer.settings->MultifragMaxRequestSize < st.cbCompMainBodySize + hdrsz)
            {
        	Application::error("%s: %s failed", __FUNCTION__, "MultifragMaxRequestSize");
                throw CodecFailed("clientUpdateBitmapPlanar");
            }

	    vec.emplace_back(st);
	}

	auto it1 = vec.begin();
	while(it1 != vec.end())
	{
	    // calc blocks
	    size_t totalSize = 0;
	    auto it2 = std::find_if(it1, vec.end(), [&](auto & st)
	    {
		if(totalSize + (st.cbCompMainBodySize + hdrsz) > peer.settings->MultifragMaxRequestSize)
		    return true;
		totalSize += (st.cbCompMainBodySize + hdrsz);
		return false;
	    });

    	    BITMAP_UPDATE bitmapUpdate = {0};
    	    bitmapUpdate.count = bitmapUpdate.number = std::distance(it1, it2);
    	    bitmapUpdate.rectangles = & (*it1);

    	    if(! peer.update->BitmapUpdate(peer.context, &bitmapUpdate))
    	    {
            	Application::error("%s: %s failed, length: %d", __FUNCTION__, "BitmapUpdate", totalSize);
                throw CodecFailed("clientUpdateBitmapPlanar");
    	    }

	    it1 = it2;
	}

	for(auto & st: vec)
	    std::free(st.bitmapDataStream);

	return true;
    }

    bool Connector::RDP::clientUpdateBitmapInterleaved(freerdp_peer & peer, const XCB::Region & reg, const XCB::PixmapInfoReply & reply)
    {
	auto context = static_cast<ClientContext*>(peer.context);

	const int bytePerPixel = context->x11display->bitsPerPixel(reply->depth()) >> 3;
        const size_t scanLineBytes = reg.width * bytePerPixel;
	// size fixed: libfreerdp/codec/interleaved.c
        const size_t tileSize = 64;

    	if(reply->size() != reg.height * reg.width * bytePerPixel)
	{
	    Application::error("%s: %s failed, length:%d, size:%dx%d, bpp:%d", __FUNCTION__, "align region", reply->size(), reg.height, reg.width, bytePerPixel);
    	    throw CodecFailed("clientUpdateBitmapInterleaved");
    	}

	size_t pixelFormat = 0;
	switch(reply->depth())
	{
#ifdef __ORDER_LITTLE_ENDIAN__
	    case 16:	pixelFormat = PIXEL_FORMAT_RGB16; break;
	    case 24:	pixelFormat = PIXEL_FORMAT_RGB24; break;
#else
	    case 16:	pixelFormat = PIXEL_FORMAT_BGR16; break;
	    case 24:	pixelFormat = PIXEL_FORMAT_BGR24; break;
#endif
	    default:
		Application::error("%s: %s failed", __FUNCTION__, "pixel format");
        	throw CodecFailed("clientUpdateBitmapInterleaved");
		break;
	}

        // planar activate
	if(! context->interleaved)
	{
	    BOOL compressor = TRUE;
	    context->interleaved = bitmap_interleaved_context_new(compressor);
	    if(! context->interleaved)
	    {
		Application::error("%s: %s failed", __FUNCTION__, "bitmap_interleaved_context_new");
        	throw CodecFailed("clientUpdateBitmapInterleaved");
	    }
	}

        if(! bitmap_interleaved_context_reset(context->interleaved))
        {
	    Application::error("%s: %s failed", __FUNCTION__, "bitmap_interleaved_context_reset");
            throw CodecFailed("clientUpdateBitmapInterleaved");
        }

        Application::debug("%s: area [%d,%d,%d,%d], depth:%d, scanline: %d, bpp:%d", __FUNCTION__, reg.x, reg.y, reg.width, reg.height, reply->depth(), scanLineBytes, bytePerPixel);
	auto blocks = reg.divideBlocks(tileSize, tileSize);

        // Compressed header of bitmap
        // http://msdn.microsoft.com/en-us/library/cc240644.aspx
 
        BITMAP_DATA st = {0};
	// full size reserved
	auto data = std::make_unique<uint8_t[]>(tileSize * tileSize * 4);

        for(auto & subreg : blocks)
        {
	    const int16_t localX = subreg.x - reg.x;
	    const int16_t localY = subreg.y - reg.y;
            const size_t offset = localY * scanLineBytes + localX * bytePerPixel;

            // Bitmap data here the screen capture
            // https://msdn.microsoft.com/en-us/library/cc240612.aspx
            st.destLeft = subreg.x;
            st.destTop = subreg.y;
            st.destRight = subreg.x + subreg.width - 1;
            st.destBottom = subreg.y + subreg.height - 1;
            st.width = subreg.width;
            st.height = subreg.height;
            st.bitsPerPixel = bytePerPixel << 3;
            st.compressed = TRUE;

            st.cbScanWidth = subreg.width * bytePerPixel;
            st.cbUncompressedSize = subreg.height * subreg.width * bytePerPixel;

            if(! interleaved_compress(context->interleaved, data.get(), & st.bitmapLength, st.width, st.height,
		reply->data() + offset, pixelFormat, scanLineBytes, 0, 0, NULL, peer.settings->ColorDepth))
	    {
                Application::error("%s: %s failed", __FUNCTION__, "freerdp_bitmap_compress_interleaved");
                throw CodecFailed("clientUpdateBitmapInterleaved");
	    }

	    st.bitmapDataStream = data.get();
            st.cbCompMainBodySize = st.bitmapLength;

	    if(peer.settings->MultifragMaxRequestSize < st.bitmapLength + 22)
            {
        	Application::error("%s: %s failed", __FUNCTION__, "MultifragMaxRequestSize");
                throw CodecFailed("clientUpdateBitmapInterleaved");
            }

    	    BITMAP_UPDATE bitmapUpdate = {0};
            bitmapUpdate.count = bitmapUpdate.number = 1;
            bitmapUpdate.rectangles = & st;

            auto ret = peer.update->BitmapUpdate(peer.context, &bitmapUpdate);
            if(! ret)
            {
                Application::error("%s: %s failed", __FUNCTION__, "BitmapUpdate");
                throw CodecFailed("clientUpdateBitmapInterleaved");
            }
        }

	return true;
    }

    // freerdp callback func
    BOOL Connector::RDP::cbClientAuthenticate(freerdp_peer* client, const char** user, const char** domain, const char** password)
    {
        Application::notice("%s: client:%p", __FUNCTION__, client);
	return TRUE;
    }

    BOOL Connector::RDP::cbClientPostConnect(freerdp_peer* client)
    {
        Application::notice("%s: client:%p, desktop:%dx%d, client depth: %d", __FUNCTION__, client, client->settings->DesktopWidth, client->settings->DesktopHeight, client->settings->ColorDepth);

	auto context = static_cast<ClientContext*>(client->context);
        auto connector = context->rdp;

	if(! connector->createX11Session())
	    return FALSE;

	context->x11display = connector->_xcbDisplay.get();
        client->settings->ColorDepth = context->x11display->bitsPerPixel();

	auto wsz = context->x11display->size();
	if(wsz.width != client->settings->DesktopWidth || wsz.height != client->settings->DesktopHeight)
	{
            if(! context->x11display->setScreenSize(client->settings->DesktopWidth, client->settings->DesktopHeight))
    	        Application::error("%s: x11display set size: failed", __FUNCTION__);

	    auto wsz = context->x11display->size();
	    if(wsz.width != client->settings->DesktopWidth || wsz.height != client->settings->DesktopHeight)
    	        Application::warning("%s: x11display size: %dx%d", __FUNCTION__, wsz.width, wsz.height);

            client->settings->DesktopWidth = wsz.width;
            client->settings->DesktopHeight = wsz.height;
            client->update->DesktopResize(client->update->context);
        }

        return TRUE;
    }

    BOOL Connector::RDP::cbClientActivate(freerdp_peer* client)
    {
        Application::notice("%s: client:%p", __FUNCTION__, client);

	auto context = static_cast<ClientContext*>(client->context);
        auto connector = context->rdp;

	if(1)
	{
    	    Application::info("client: %s: %s", "Username", client->settings->Username);
    	    Application::info("client: %s: %s", "Domain", client->settings->Domain);
    	    Application::info("client: %s: %d", "DesktopWidth", client->settings->DesktopWidth);
    	    Application::info("client: %s: %d", "DesktopHeight", client->settings->DesktopHeight);
    	    Application::info("client: %s: %d", "DesktopColorDepth", client->settings->ColorDepth);
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
    	    Application::info("client: %s: %s", "FastPathOutput", (client->settings->FastPathOutput ? "true" : "false"));
    	    Application::info("client: %s: %s", "UnicodeInput", (client->settings->UnicodeInput ? "true" : "false"));
    	    Application::info("client: %s: %s", "BitmapCacheEnabled", (client->settings->BitmapCacheEnabled ? "true" : "false"));
    	    Application::info("client: %s: %s", "DesktopResize", (client->settings->DesktopResize ? "true" : "false"));
    	    Application::info("client: %s: %s", "RefreshRect", (client->settings->RefreshRect ? "true" : "false"));
    	    Application::info("client: %s: %s", "SuppressOutput", (client->settings->SuppressOutput ? "true" : "false"));
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

	std::string encryptionInfo;

	if(0 < client->settings->TlsSecLevel)
	    encryptionInfo.append("TLS security level: ").append(std::to_string(client->settings->TlsSecLevel));

	const char* method = nullptr;
	switch(client->settings->EncryptionMethods)
	{
	    case ENCRYPTION_METHOD_40BIT:  method = "40bit"; break;
	    case ENCRYPTION_METHOD_56BIT:  method = "56bit"; break;
	    case ENCRYPTION_METHOD_128BIT: method = "128bit"; break;
	    case ENCRYPTION_METHOD_FIPS:   method = "fips"; break;
	    default: break;
	}

	if(method)
	{
	    if(encryptionInfo.size()) encryptionInfo.append(", ");
	    encryptionInfo.append("RDP method: ").append(method);
	}

	if(encryptionInfo.size())
	    connector->setEncryptionInfo(encryptionInfo);
 
        context->activated = TRUE;
	connector->setEnableXcbMessages(true);

        const XCB::Region damage(0, 0, client->settings->DesktopWidth, client->settings->DesktopHeight);
	context->x11display->damageAdd(damage);

        return TRUE;
    }

    /// @param flags: KBD_FLAGS_EXTENDED(0x0100), KBD_FLAGS_EXTENDED1(0x0200), KBD_FLAGS_DOWN(0x4000), KBD_FLAGS_RELEASE(0x8000)
    /// @see:  freerdp/input.h
    BOOL Connector::RDP::cbClientKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code)
    {
        Application::notice("%s: flags:0x%04X, code:0x%04X, input:%p, context:%p", __FUNCTION__, flags, code, input, input->context);
	auto context = static_cast<ClientContext*>(input->context);

	if(flags == 0x8000 && code == 0x000F)
	    return TRUE;

	if(context->rdp->isAllowXcbMessages())
        {
            uint32_t keysym = static_cast<uint32_t>(flags) << 16 | code;

            // local keymap priority "vnc:keymap:file"
            if(auto value = (context->keymap ? context->keymap->getValue(Tools::hex(keysym, 8)) : nullptr))
            {
                // no wait xcb replies
                if(value->isArray())
                {
                    auto ja = static_cast<const JsonArray*>(value);
                    for(auto & val : ja->toStdVector<int>())
                        context->x11display->fakeInputKeycode(flags & KBD_FLAGS_DOWN ? XCB_KEY_PRESS : XCB_KEY_RELEASE, val);
                }
                else
                    context->x11display->fakeInputKeycode(flags & KBD_FLAGS_DOWN ? XCB_KEY_PRESS : XCB_KEY_RELEASE, value->getInteger());
            }
            else
            {
		// see winpr/input.h
		// KBDEXT(0x0100), KBDMULTIVK(0x0200), KBDSPECIAL(0x0400), KBDNUMPAD(0x0800),
	        // KBDUNICODE(0x1000), KBDINJECTEDVK(0x2000), KBDMAPPEDVK(0x4000), KBDBREAK(0x8000)
	        if(flags & KBD_FLAGS_EXTENDED)
                    code |= KBDEXT;

	        // winpr: input
    	        auto vkcode = GetVirtualKeyCodeFromVirtualScanCode(code, 4);
    	        auto keycode = GetKeycodeFromVirtualKeyCode((flags & KBD_FLAGS_EXTENDED ? vkcode | KBDEXT : vkcode), KEYCODE_TYPE_EVDEV);

                context->x11display->fakeInputKeycode(flags & KBD_FLAGS_DOWN ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode);
	    }
        }

        return TRUE;
    }

    /// @param flags: PTR_FLAGS_BUTTON1(0x1000), PTR_FLAGS_BUTTON2(0x2000), PTR_FLAGS_BUTTON3(0x4000), PTR_FLAGS_HWHEEL(0x0400),
    ///               PTR_FLAGS_WHEEL(0x0200), PTR_FLAGS_WHEEL_NEGATIVE(0x0100), PTR_FLAGS_MOVE(0x0800), PTR_FLAGS_DOWN(0x8000)
    /// @see:  freerdp/input.h
    BOOL Connector::RDP::cbClientMouseEvent(rdpInput* input, UINT16 flags, UINT16 posx, UINT16 posy)
    {
        Application::debug("%s: flags:0x%04X, pos:%d,%d, input:%p, context:%p", __FUNCTION__, flags, posx, posy, input, input->context);
	auto context = static_cast<ClientContext*>(input->context);

	if(context->rdp->isAllowXcbMessages())
        {
	    // left button
	    if(flags & PTR_FLAGS_BUTTON1)
        	context->x11display->fakeInputMouse(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, XCB_BUTTON_INDEX_1, posx, posy);
	    else
	    // right button
	    if(flags & PTR_FLAGS_BUTTON2)
        	context->x11display->fakeInputMouse(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, XCB_BUTTON_INDEX_3, posx, posy);
	    else
	    // middle button
	    if(flags & PTR_FLAGS_BUTTON3)
        	context->x11display->fakeInputMouse(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, XCB_BUTTON_INDEX_2, posx, posy);
	    else
	    if(flags & PTR_FLAGS_WHEEL)
        	context->x11display->fakeInputMouse(flags & PTR_FLAGS_DOWN ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, flags & PTR_FLAGS_WHEEL_NEGATIVE ? XCB_BUTTON_INDEX_5 : XCB_BUTTON_INDEX_4, posx, posy);

	    if(flags & PTR_FLAGS_MOVE)
        	context->x11display->fakeInputMouse(XCB_MOTION_NOTIFY, 0, posx, posy);
	}

        return TRUE;
    }

    BOOL Connector::RDP::cbClientRefreshRect(rdpContext* rdpctx, BYTE count, const RECTANGLE_16* areas)
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

    BOOL Connector::RDP::cbClientSuppressOutput(rdpContext* rdpctx, BYTE allow, const RECTANGLE_16* area)
    {
        Application::notice("%s: allow:0x%02X, context:%p", __FUNCTION__, (int) allow, rdpctx);
	auto context = static_cast<ClientContext*>(rdpctx);

	if(area && 0 < allow)
        {
    	    Application::debug("%s: client restore output(left:%d,top:%d,right:%d,bottom:%d)", __FUNCTION__, area->left, area->top, area->right, area->bottom);
	    context->rdp->setEnableXcbMessages(true);
    	    context->x11display->damageAdd(context->x11display->region());
        }
        else
        {
            Application::debug("%s: client minimized and suppress output", __FUNCTION__);
	    context->rdp->setEnableXcbMessages(false);
        }

        return TRUE;
    }
}
