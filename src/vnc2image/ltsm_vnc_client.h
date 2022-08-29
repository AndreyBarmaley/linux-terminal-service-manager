/***************************************************************************
 *   Copyright (C) 2022 by MultiCapture team <public.irkutsk@gmail.com>    *
 *                                                                         *
 *   Part of the MultiCapture engine:                                      *
 *   https://github.com/AndreyBarmaley/multi-capture                       *
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
 ***************************************************************************/

#ifndef _LTSM_VNC_CLIENT_
#define _LTSM_VNC_CLIENT_

#include <mutex>
#include <memory>
#include <atomic>

namespace LTSM
{
    /* Connector::VNC */
    class ClientConnector : protected Network::BaseStream
    {
        std::unique_ptr<SocketStream> socket;
        std::unique_ptr<Zlib::InflateStream> zlib;       /// zlib layer

        NetworkStream* streamIn;
        NetworkStream* streamOut;

        int             debug;
        int             sdlFormat;
        std::atomic<bool> loopMessage;
        const JsonObject* config;
        std::unique_ptr<FrameBuffer> fbPtr;
        std::mutex      fbChange;

        // network stream interface
        void            sendFlush(void) override;
        void            sendRaw(const void* ptr, size_t len) override;
        void            recvRaw(void* ptr, size_t len) const override;
        void            recvRaw(void* ptr, size_t len, size_t timeout /* ms */) const override;
        bool            hasInput(void) const override;
        size_t          hasData(void) const override;

        // zlib wrapper
        void            zlibInflateStart(bool uint16sz = false);
        void            zlibInflateStop(void);

    protected:
        void            clientPixelFormat(void);
        void            clientSetEncodings(std::initializer_list<int>);
        void            clientFrameBufferUpdateReq(bool incr);
        void            clientFrameBufferUpdateReq(const XCB::Region &, bool incr);

        virtual void    serverFBUpdateEvent(void);
        virtual void    serverSetColorMapEvent(void);
        virtual void    serverBellEvent(void);
        virtual void    serverCutTextEvent(void);

        void            recvDecodingRaw(const XCB::Region &);
        void            recvDecodingRRE(const XCB::Region &, bool corre);
        void            recvDecodingHexTile(const XCB::Region &);
        void            recvDecodingHexTileRegion(const XCB::Region &, int & bgColor, int & fgColor);
        void            recvDecodingTRLE(const XCB::Region &, bool zrle);
        void            recvDecodingTRLERegion(const XCB::Region &, bool zrle);
        void            recvDecodingZlib(const XCB::Region &);
        void            recvDecodingLastRect(const XCB::Region &);

        int             recvPixel(void);
        int             recvCPixel(void);
        size_t          recvRunLength(void);

    public:
        ClientConnector(const SWE::JsonObject &);
        virtual ~ClientConnector(){}

        bool            communication(const std::string &, int, const std::string & pass = "");
        void            messages(void);
        void            shutdown(void);

        //void            syncFrameBuffer(Surface &);
    };
}

#endif
