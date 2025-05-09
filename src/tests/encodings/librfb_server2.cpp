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

#include <string>
#include <chrono>
#include <thread>
#include <cstring>
#include <fstream>
#include <algorithm>

#include "ltsm_application.h"
#include "ltsm_tools.h"
#include "librfb_server2.h"

using namespace std::chrono_literals;

namespace LTSM
{
    // ServerEncoder
    RFB::ServerEncoderBuf::ServerEncoderBuf(const PixelFormat & pf) : clientPf(pf), serverPf(pf)
    {
        bufData.reserve(30 * 1024 * 1024);
        socket.reset(new EncoderWrapper(&bufData, this));

        streamIn = streamOut = socket.get();
    }

    void RFB::ServerEncoderBuf::sendFlush(void)
    {   
        try
        {
            streamOut->sendFlush();
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }
    }

    void RFB::ServerEncoderBuf::sendRaw(const void* ptr, size_t len)
    {   
        try
        {
            streamOut->sendRaw(ptr, len);
            netStatTx += len;
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }
    }

    void RFB::ServerEncoderBuf::recvRaw(void* ptr, size_t len) const
    {   
        try
        {
            streamIn->recvRaw(ptr, len);
            netStatRx += len;
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }
    }

    bool RFB::ServerEncoderBuf::hasInput(void) const
    {
        try
        {
            return streamIn->hasInput();
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

        return false;
    }

    size_t RFB::ServerEncoderBuf::hasData(void) const
    {
        try
        {
            return streamIn->hasData();
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

        return 0;
    }

    uint8_t RFB::ServerEncoderBuf::peekInt8(void) const
    {
        try
        {
            return streamIn->peekInt8();
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

        return 0;
    }

    XCB::Size RFB::ServerEncoderBuf::displaySize(void) const
    {
        return socket->displaySize();
    }

    const std::vector<uint8_t> & RFB::ServerEncoderBuf::getBuffer(void) const
    {
        return bufData;
    }

    void RFB::ServerEncoderBuf::resetBuffer(void)
    {
        bufData.clear();
    }

/*
    bool RFB::ServerEncoderBuf::isUpdateProcessed(void) const
    {
        return encoder;
    }

    void RFB::ServerEncoderBuf::waitUpdateProcess(void)
    {
        while(isUpdateProcessed())
            std::this_thread::sleep_for(5ms);
    }
*/

    void RFB::ServerEncoderBuf::sendFrameBufferUpdate(const FrameBuffer & fb)
    {
        auto & reg = fb.region();

        Application::debug(DebugType::App, "%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        std::scoped_lock guard{ sendLock };

        // RFB: 6.5.1
        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);

        // send encodings
        encoder->sendFrameBuffer(this, fb);

        sendFlush();
    }

    void RFB::ServerEncoderBuf::setEncodingDebug(int v)
    {
        //if(encoder)
        //    encoder->setDebug(v);
    }

    void RFB::ServerEncoderBuf::setEncodingThreads(int threads)
    {
        if(threads < 1)
            threads = 1;
        else
        if(std::thread::hardware_concurrency() < threads)
        {
            threads = std::thread::hardware_concurrency();
            Application::error("%s: encoding threads incorrect, fixed to hardware concurrency: %d", __FUNCTION__, threads);
        }

        if(encoder)
        {
            Application::info("%s: using encoding threads: %d", __FUNCTION__, threads);
            encoder->setThreads(threads);
        }
    }

    bool RFB::ServerEncoderBuf::serverSetClientEncoding(int type)
    {
        switch(type)
        {
            case RFB::ENCODING_ZLIB:
                encoder = std::make_unique<EncodingZlib>();
                return true;

            case RFB::ENCODING_HEXTILE:
                encoder = std::make_unique<EncodingHexTile>();
                return true;

            case RFB::ENCODING_CORRE:
                encoder = std::make_unique<EncodingRRE>(true);
                return true;

            case RFB::ENCODING_RRE:
                encoder = std::make_unique<EncodingRRE>(false);
                return true;

            case RFB::ENCODING_TRLE:
                encoder = std::make_unique<EncodingTRLE>(false);
                return true;

            case RFB::ENCODING_ZRLE:
                encoder = std::make_unique<EncodingTRLE>(true);
                return true;

            default:
                break;
        }

        encoder = std::make_unique<EncodingRaw>();
        return true;
    }
}
