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

#ifndef _LTSM_CONNECTOR_VNC_
#define _LTSM_CONNECTOR_VNC_

#include <list>
#include <mutex>
#include <tuple>
#include <memory>
#include <future>
#include <atomic>
#include <functional>

#include "ltsm_sockets.h"
#include "ltsm_connector.h"
#include "ltsm_vnc_zlib.h"

namespace LTSM
{
    namespace RFB
    {
        // RFB protocol constant
        const int VERSION_MAJOR = 3;
        const int VERSION_MINOR = 8;

        const int SECURITY_TYPE_NONE = 1;
        const int SECURITY_TYPE_VNC = 2;
        const int SECURITY_TYPE_TLS = 18;
        const int SECURITY_TYPE_VENCRYPT = 19;
        const int SECURITY_VENCRYPT01_PLAIN = 19;
        const int SECURITY_VENCRYPT01_TLSNONE = 20;
        const int SECURITY_VENCRYPT01_TLSVNC = 21;
        const int SECURITY_VENCRYPT01_TLSPLAIN = 22;
        const int SECURITY_VENCRYPT01_X509NONE = 23;
        const int SECURITY_VENCRYPT01_X509VNC = 24;
        const int SECURITY_VENCRYPT01_X509PLAIN = 25;
        const int SECURITY_VENCRYPT02_PLAIN = 256;
        const int SECURITY_VENCRYPT02_TLSNONE = 257;
        const int SECURITY_VENCRYPT02_TLSVNC = 258;
        const int SECURITY_VENCRYPT02_TLSPLAIN = 259;
        const int SECURITY_VENCRYPT02_X509NONE = 260;
        const int SECURITY_VENCRYPT02_X509VNC = 261;
        const int SECURITY_VENCRYPT02_X509PLAIN = 261;

        const int SECURITY_RESULT_OK = 0;
        const int SECURITY_RESULT_ERR = 1;

        const int CLIENT_SET_PIXEL_FORMAT = 0;
        const int CLIENT_SET_ENCODINGS = 2;
        const int CLIENT_REQUEST_FB_UPDATE = 3;
        const int CLIENT_EVENT_KEY = 4;
        const int CLIENT_EVENT_POINTER = 5;
        const int CLIENT_CUT_TEXT = 6;
        const int CLIENT_SET_DESKTOP_SIZE = 251;
            
        const int SERVER_FB_UPDATE = 0;
        const int SERVER_SET_COLOURMAP = 1;
        const int SERVER_BELL = 2;
        const int SERVER_CUT_TEXT = 3;

        // RFB protocol constants
        const int ENCODING_RAW = 0;
        const int ENCODING_COPYRECT = 1;
        const int ENCODING_RRE = 2;
        const int ENCODING_CORRE = 4;
        const int ENCODING_HEXTILE = 5;
        const int ENCODING_ZLIB = 6;
        const int ENCODING_TIGHT = 7;
        const int ENCODING_ZLIBHEX = 8;
        const int ENCODING_TRLE = 15;
        const int ENCODING_ZRLE = 16;
 
        // hextile constants
        const int HEXTILE_RAW = 1;
        const int HEXTILE_BACKGROUND = 2;
        const int HEXTILE_FOREGROUND = 4;
        const int HEXTILE_SUBRECTS = 8;
        const int HEXTILE_COLOURED = 16;
        const int HEXTILE_ZLIBRAW = 32;
        const int HEXTILE_ZLIB = 64;
 
        // pseudo encodings
        const int ENCODING_DESKTOP_SIZE = -223;
        const int ENCODING_EXT_DESKTOP_SIZE = -308;

	struct ScreenInfo
	{
            uint32_t		id = 0;
            uint16_t		xpos = 0;
            uint16_t		ypos = 0;
            uint16_t		width = 0;
            uint16_t		height = 0;
            uint32_t		flags = 0;
	};

        const char* encodingName(int type);
    }

    namespace RRE
    {
        struct Region : std::pair<XCB::Region, uint32_t>
        {
            Region(const XCB::Region & reg, uint32_t pixel) : std::pair<XCB::Region, uint32_t>(reg, pixel) {}
            Region() {}

            const uint32_t &    pixel(void) const { return second; }
            const XCB::Region & region(void) const { return first; }
        };
    }

    enum class DesktopResizeMode { Undefined, Disabled, Success, ServerInform, ClientRequest };
    const char* desktopResizeModeString(const DesktopResizeMode &);

    namespace Connector
    {
        typedef std::function<int(const FrameBuffer &)> sendEncodingFunc;

        /* Connector::VNC */
        class VNC : public SignalProxy, protected NetworkStream
        {
	    std::unique_ptr<NetworkStream> socket;	/// socket layer
	    std::unique_ptr<TLS::Stream> tls;		/// tls layer
	    std::unique_ptr<ZLib::DeflateStream> zlib;	/// zlib layer
            std::unique_ptr<JsonObject> keymap;

	    NetworkStream* 	streamIn;
	    NetworkStream* 	streamOut;

            std::atomic<bool>   loopMessage;
            int                 encodingDebug;
            int                 encodingThreads;
            std::atomic<int>    pressedMask;
	    std::atomic<bool>   fbUpdateProcessing;
	    std::atomic<bool>	sendBellFlag;
	    std::atomic<DesktopResizeMode>
                                desktopResizeMode;
            PixelFormat         serverFormat;
            PixelFormat         clientFormat;
            XCB::Region         clientRegion;
            XCB::Region         serverRegion;
            std::mutex          sendGlobal;
            std::mutex		sendEncoding;
            ColorMap            colourMap;
            std::vector<int>    clientEncodings;
            std::list<std::string>
                                disabledEncodings;
            std::list< std::future<int> >
                                jobsEncodings;
            std::pair<sendEncodingFunc, int>
                                prefEncodings;
	    std::vector<RFB::ScreenInfo>
                                screensInfo;

	    // network stream interface
	    void		  sendFlush(void) override { return streamOut->sendFlush(); }
	    void		  sendRaw(const void* ptr, size_t len) override { streamOut->sendRaw(ptr, len); }
	    void                  recvRaw(void* ptr, size_t len) const override { streamIn->recvRaw(ptr, len); }
	    bool		  hasInput(void) const override { return streamIn->hasInput(); }
	    uint8_t		  peekInt8(void) const override { return streamIn->peekInt8(); }

	    // zlib wrapper
	    void		 zlibDeflateStart(size_t);
	    std::vector<uint8_t> zlibDeflateStop(void);

        protected:
            // dbus virtual signals
            void                onLoginSuccess(const int32_t & display, const std::string & userName) override;
            void                onShutdownConnector(const int32_t & display) override;
            void                onHelperWidgetStarted(const int32_t & display) override;
            void                onSendBellSignal(const int32_t & display) override;

        protected:
            void                clientSetPixelFormat(void);
	    bool		clientVenCryptHandshake(void);
            bool                clientSetEncodings(void);
            bool                clientFramebufferUpdate(void);
            void                clientKeyEvent(void);
            void                clientPointerEvent(void);
            void                clientCutTextEvent(void);
	    void		clientSetDesktopSizeEvent(void);
            void                clientDisconnectedEvent(void);

            bool                serverSendFrameBufferUpdate(const XCB::Region &);
            void                serverSendColourMap(int first);
            void                serverSendBell(void);
            void                serverSendCutText(const std::vector<uint8_t> &);
	    int			serverSendDesktopSize(const DesktopResizeMode &);

            int                 sendPixel(uint32_t pixel);
            int                 sendCPixel(uint32_t pixel);

            bool                isUpdateProcessed(void) const;
            void                waitSendingFBUpdate(void) const;

            int                 sendEncodingRaw(const FrameBuffer &);
            int                 sendEncodingRawSubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            int                 sendEncodingRawSubRegionRaw(const XCB::Region &, const FrameBuffer &);

            int                 sendEncodingRRE(const FrameBuffer &, bool corre);
            int			sendEncodingRRESubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId, bool corre);
            int                 sendEncodingRRESubRects(const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<RRE::Region> &, bool corre);

            int                 sendEncodingHextile(const FrameBuffer &, bool zlibver);
            int			sendEncodingHextileSubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId, bool zlibver);
            int			sendEncodingHextileSubForeground(const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<RRE::Region> &);
            int			sendEncodingHextileSubColored(const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<RRE::Region> &);
            int			sendEncodingHextileSubRaw(const XCB::Region &, const FrameBuffer &, int jobId, bool zlibver);

            int                 sendEncodingZLib(const FrameBuffer &);
            int			sendEncodingZLibSubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);

            int                 sendEncodingTRLE(const FrameBuffer &, bool zrle);
            int			sendEncodingTRLESubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId, bool zrle);
            int                 sendEncodingTRLESubPacked(const XCB::Region &, const FrameBuffer &, int jobId, size_t field, size_t rowsz, const PixelMapWeight &, bool zrle);
	    int			sendEncodingTRLESubPlain(const XCB::Region &, const FrameBuffer &, const std::list<PixelLength> &);
	    int			sendEncodingTRLESubPalette(const XCB::Region &, const FrameBuffer &, const PixelMapWeight &, const std::list<PixelLength> &);
	    int			sendEncodingTRLESubRaw(const XCB::Region &, const FrameBuffer &);

            std::pair<sendEncodingFunc, int> selectEncodings(void);

        public:
            VNC(sdbus::IConnection* conn, const JsonObject & jo);
            ~VNC();

            int		        communication(void) override;
        };
    }
}

#endif // _LTSM_CONNECTOR_VNC_
