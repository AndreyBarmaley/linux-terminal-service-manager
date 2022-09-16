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

#ifndef _LIBRFB_SERVER_
#define _LIBRFB_SERVER_

#include <list>
#include <mutex>
#include <future>
#include <tuple>

#include "ltsm_librfb.h"
#include "ltsm_sockets.h"
#include "ltsm_framebuffer.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_json_wrapper.h"

namespace LTSM
{
    namespace RFB
    {
        /// ServerEncoder
        class ServerEncoder : protected NetworkStream
        {
            std::list< std::future<void> >
                                encodingJobs;
            std::vector<int>    clientEncodings;
            std::list<std::string> disabledEncodings;
            std::list<std::string> prefferedEncodings;

            std::unique_ptr<NetworkStream> socket;      /// socket layer
            std::unique_ptr<TLS::Stream> tls;           /// tls layer
            std::unique_ptr<ZLib::DeflateStream> zlib;  /// zlib layer

            PixelFormat         serverFormat;
            PixelFormat         clientFormat;
            ColorMap            colourMap;
            std::mutex		encodingBusy;
            std::mutex          networkBusy;
            std::atomic<int>    pressedMask{0};
            std::atomic<bool>   fbUpdateProcessing{false};
	    std::atomic<DesktopResizeMode>
                                desktopMode{DesktopResizeMode::Undefined};
	    RFB::ScreenInfo     desktopScreenInfo;
            mutable size_t      netStatRx = 0;
            mutable size_t      netStatTx = 0;
            int                 encodingDebug = 0;
            int                 encodingThreads = 2;
            NetworkStream*      streamIn = nullptr;
            NetworkStream*      streamOut = nullptr;

            std::pair<RFB::sendEncodingFunc, int>
                                prefEncodingsPair;

            bool                clientTrueColor = true;
            bool                clientBigEndian = false;

        protected:
            // librfb interface
            virtual XCB::RootDisplayExt* xcbDisplay(void) const = 0;
            virtual bool        serviceAlive(void) const = 0;
            virtual void        serviceStop(void) = 0;
            virtual void        serverPostProcessingFrameBuffer(FrameBuffer &) {}

           // network stream interface
            void                sendFlush(void) override;
            void                sendRaw(const void* ptr, size_t len) override;
            void                recvRaw(void* ptr, size_t len) const override;
            bool                hasInput(void) const override;
            size_t              hasData(void) const override;
            uint8_t             peekInt8(void) const override;

	    void		zlibDeflateStart(size_t);
	    void                zlibDeflateStop(bool uint16sz = false);

            void                setDisabledEncodings(std::list<std::string>);
            void                setPrefferedEncodings(std::list<std::string>);

            std::string         serverEncryptionInfo(void) const;

            void                setEncodingDebug(int v);
            void                setEncodingThreads(int v);
            bool                isClientEncodings(int) const;

            bool                isUpdateProcessed(void) const;
            void                waitUpdateProcess(void);

            bool                serverSelectClientEncoding(void);
            void                serverSetPixelFormat(const PixelFormat &);

            bool                desktopResizeModeInit(void);
            void                desktopResizeModeDisable(void);
            void                desktopResizeModeSet(const DesktopResizeMode &, const std::vector<RFB::ScreenInfo> &);
            bool                desktopResizeModeChange(const XCB::Size &);
            DesktopResizeMode   desktopResizeMode(void) const;

            bool                serverAuthVncInit(const std::string &);
            bool                serverAuthVenCryptInit(const SecurityInfo &);
            void                serverSendUpdateBackground(const XCB::Region &);

            void                clientSetPixelFormat(void);
            bool                clientSetEncodings(void);

        public:
            ServerEncoder(int sockfd = 0);

            int                 serverHandshakeVersion(void);
            bool                serverSecurityInit(int protover, const SecurityInfo &);
            void                serverClientInit(std::string_view);

            bool                serverSendFrameBufferUpdate(const XCB::Region &);
            void                serverSendColourMap(int first);
            void                serverSendBell(void);
            void                serverSendCutText(const std::vector<uint8_t> &);
            void                serverSendEndContinuousUpdates(void);

            void                serverSelectEncodings(void);

            void                sendEncodingRaw(const FrameBuffer &);
            void                sendEncodingRawSubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);
            void                sendEncodingRawSubRegionRaw(const XCB::Region &, const FrameBuffer &);

            void                sendEncodingRRE(const FrameBuffer &, bool corre);
            void                sendEncodingRRESubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId, bool corre);
            void                sendEncodingRRESubRects(const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &, bool corre);

            void                sendEncodingHextile(const FrameBuffer &, bool zlibver);
            void                sendEncodingHextileSubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId, bool zlibver);
            void                sendEncodingHextileSubForeground(const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &);
            void                sendEncodingHextileSubColored(const XCB::Region &, const FrameBuffer &, int jobId, int back, const std::list<XCB::RegionPixel> &);
            void                sendEncodingHextileSubRaw(const XCB::Region &, const FrameBuffer &, int jobId, bool zlibver);

            void                sendEncodingZLib(const FrameBuffer &);
            void                sendEncodingZLibSubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId);

            void                sendEncodingTRLE(const FrameBuffer &, bool zrle);
            void                sendEncodingTRLESubRegion(const XCB::Point &, const XCB::Region &, const FrameBuffer &, int jobId, bool zrle);
            void                sendEncodingTRLESubPacked(const XCB::Region &, const FrameBuffer &, int jobId, size_t field, const PixelMapWeight &, bool zrle);
	    void                sendEncodingTRLESubPlain(const XCB::Region &, const FrameBuffer &, const std::list<PixelLength> &);
	    void                sendEncodingTRLESubPalette(const XCB::Region &, const FrameBuffer &, const PixelMapWeight &, const std::list<PixelLength> &);
	    void                sendEncodingTRLESubRaw(const XCB::Region &, const FrameBuffer &);

	    bool		sendEncodingDesktopSize(bool xcbAllow);

            std::pair<bool, XCB::Region>
                                clientFramebufferUpdate(void);
            void                clientKeyEvent(bool xcbAllow, const JsonObject* keymap = nullptr);
            void                clientPointerEvent(bool xcbAllow);
            void                clientCutTextEvent(bool xcbAllow, bool clipboardEnable);
            void                clientSetDesktopSizeEvent(void);
            void                clientEnableContinuousUpdates(void);
            void                clientDisconnectedEvent(int display);

            int                 sendPixel(uint32_t pixel);
            int                 sendCPixel(uint32_t pixel);
            int                 sendRunLength(size_t length);
        };
    }
}

#endif // _LTSM_LIBRFB_
