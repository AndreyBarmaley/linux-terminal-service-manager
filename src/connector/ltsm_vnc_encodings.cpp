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

#include<iomanip>
#include<sstream>

#include <string>
#include <chrono>
#include <thread>
#include <future>
#include <numeric>
#include <iostream>
#include <iterator>

#include "zlib.h"

#include "libdeflate.h"
#include "ltsm_tools.h"
#include "ltsm_connector_vnc.h"

using namespace std::chrono_literals;

namespace LTSM
{
    namespace RFB
    {
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

        const char* encodingName(int type)
        {
            switch(type)
            {
                case ENCODING_RAW:
                    return "Raw";

                case ENCODING_COPYRECT:
                    return "CopyRect";

                case ENCODING_RRE:
                    return "RRE";

                case ENCODING_CORRE:
                    return "CoRRE";

                case ENCODING_HEXTILE:
                    return "HexTile";

                case ENCODING_ZLIB:
                    return "ZLib";

                case ENCODING_TIGHT:
                    return "Tight";

                case ENCODING_ZLIBHEX:
                    return "ZLibHex";

                case ENCODING_TRLE:
                    return "TRLE";

                case ENCODING_ZRLE:
                    return "ZRLE";

                default:
                    break;
            }

            return "unknown";
        }
    }

    std::pair<Connector::sendEncodingFunc, std::string> Connector::VNC::selectEncodings(void)
    {
        for(int type : clientEncodings)
        {
            switch(type)
            {
                case RFB::ENCODING_ZLIB:
                    Application::info("server select encoding: %s", RFB::encodingName(type));
                    return std::make_pair([=](const RFB::Region & a, const RFB::FrameBuffer & b)
                    {
                        return this->sendEncodingZLib(a, b);
                    }, RFB::encodingName(type));

                case RFB::ENCODING_HEXTILE:
                    Application::info("server select encoding: %s", RFB::encodingName(type));
                    return std::make_pair([=](const RFB::Region & a, const RFB::FrameBuffer & b)
                    {
                        return this->sendEncodingHextile(a, b);
                    }, RFB::encodingName(type));

                case RFB::ENCODING_CORRE:
                    Application::info("server select encoding: %s", RFB::encodingName(type));
                    return std::make_pair([=](const RFB::Region & a, const RFB::FrameBuffer & b)
                    {
                        return this->sendEncodingCoRRE(a, b);
                    }, RFB::encodingName(type));

                case RFB::ENCODING_RRE:
                    Application::info("server select encoding: %s", RFB::encodingName(type));
                    return std::make_pair([=](const RFB::Region & a, const RFB::FrameBuffer & b)
                    {
                        return this->sendEncodingRRE(a, b);
                    }, RFB::encodingName(type));

                case RFB::ENCODING_TRLE:
                    Application::info("server select encoding: %s", RFB::encodingName(type));
                    return std::make_pair([=](const RFB::Region & a, const RFB::FrameBuffer & b)
                    {
                        return this->sendEncodingTRLE(a, b);
                    }, RFB::encodingName(type));

                default:
                    break;
            }
        }

        Application::info("server select encoding: %s", RFB::encodingName(RFB::ENCODING_RAW));
        return std::make_pair([=](const RFB::Region & a, const RFB::FrameBuffer & b)
    	{
	    return this->sendEncodingRaw(a, b);
	}, "Raw");
    }

    int Connector::VNC::sendEncodingSmall(const RFB::Region & reg, const RFB::FrameBuffer & fb)
    {
        for(auto & type : clientEncodings)
        {
            if(type == RFB::ENCODING_CORRE)
                return sendEncodingCoRRE(reg, fb);
            else if(type == RFB::ENCODING_RRE)
                return sendEncodingRRE(reg, fb);
        }

        return sendEncodingRaw(reg, fb);
    }

    int Connector::VNC::sendEncodingRaw(const RFB::Region & reg0, const RFB::FrameBuffer & fb)
    {
        Application::debug("encoding: %s, region: [%d, %d, %d, %d]", "Raw", reg0.x, reg0.y, reg0.w, reg0.h);

        // regions counts
        sendIntBE16(1);
        return 2 + sendEncodingRawSubRegion(reg0, fb, 1);
    }

    int Connector::VNC::sendEncodingRawSubRegion(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId)
    {
        const std::lock_guard<std::mutex> lock(sendEncoding);

        if(encodingDebug)
            Application::debug("send RAW region, job id: %d, [%d, %d, %d, %d]", jobId, reg.x, reg.y, reg.w, reg.h);

        // region size
        sendIntBE16(reg.x);
        sendIntBE16(reg.y);
        sendIntBE16(reg.w);
        sendIntBE16(reg.h);
        // region type
        sendIntBE32(RFB::ENCODING_RAW);
        return 12 + sendEncodingRawSubRegionRaw(reg, fb);
    }

    int Connector::VNC::sendEncodingRawSubRegionRaw(const RFB::Region & reg, const RFB::FrameBuffer & fb)
    {
        int res = 0;

        if(serverFormat != clientFormat)
        {
            for(int yy = 0; yy < reg.h; ++yy)
                for(int xx = 0; xx < reg.w; ++xx)
                    res += sendPixel(fb.pixel(reg.x + xx, reg.y + yy));
        }
        else
        {
            for(int yy = 0; yy < reg.h; ++yy)
            {
		size_t line = reg.w * serverFormat.bytePerPixel();
                sendRaw(fb.pitchData(reg.y + yy) + reg.x * serverFormat.bytePerPixel(), line);
                res += line;
            }
        }

        return res;
    }

    std::list<RRE::Region> processingRRE(const RFB::Region & badreg, const RFB::FrameBuffer & fb, int skipPixel)
    {
        std::list<RRE::Region> goods;
        std::list<RFB::Region> bads1 = { badreg };
        std::list<RFB::Region> bads2;

        do
        {
            while(! bads1.empty())
            {
                for(auto & subreg : bads1.front().divideCounts(2, 2))
                {
                    int pixel = fb.pixel(subreg.x, subreg.y);

                    if((subreg.w == 1 && subreg.h == 1) || fb.allOfPixel(pixel, subreg))
                    {
                        if(pixel != skipPixel)
                        {
                            // maybe join prev
                            if(! goods.empty() && goods.back().first.y == subreg.y && goods.back().first.h == subreg.h &&
                               goods.back().first.x + goods.back().first.w == subreg.x && goods.back().second == pixel)
                                goods.back().first.w += subreg.w;
                            else
                                goods.emplace_back(subreg, pixel);
                        }
                    }
                    else
                        bads2.push_back(subreg);
                }

                bads1.pop_front();
            }

            if(bads2.empty())
                break;

            bads2.swap(bads1);
            bads2.clear();
        }
        while(! bads1.empty());

        return goods;
    }

    /* RRE */
    int Connector::VNC::sendEncodingRRE(const RFB::Region & reg0, const RFB::FrameBuffer & fb)
    {
        Application::debug("encoding: %s, region: [%d, %d, %d, %d]", "RRE", reg0.x, reg0.y, reg0.w, reg0.h);

        auto regions = reg0.divideBlocks(128, 128);
        // regions counts
        sendIntBE16(regions.size());
        int res = 2;
        int jobId = 1;

        // make pool jobs
        while(jobId <= _encodingThreads && ! regions.empty())
        {
            jobsEncodings.push_back(std::async(std::launch::async, & Connector::VNC::sendEncodingRRESubRegion, this, std::move(regions.front()), fb, jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : jobsEncodings)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::nanoseconds(200)) == std::future_status::ready)
                {
                    res += job.get();
                    job = std::async(std::launch::async, & Connector::VNC::sendEncodingRRESubRegion, this, std::move(regions.front()), fb, jobId);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // get results
        for(auto & job : jobsEncodings)
            res += job.get();

        jobsEncodings.clear();
        return res;
    }

    int Connector::VNC::sendEncodingRRESubRegion(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapWeight(reg);

        auto sendHeaderRRE = [this](const RFB::Region & reg)
        {
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.w);
            this->sendIntBE16(reg.h);
            // region type
            this->sendIntBE32(RFB::ENCODING_RRE);
            return 12;
        };

        if(map.size() > 1)
        {
            int back = map.maxWeightPixel();
            std::list<RRE::Region> goods = processingRRE(reg, fb, back);

            const size_t rawLength = reg.w * reg.h * fb.bytePerPixel();
            const size_t rreLength = 4 + fb.bytePerPixel() + goods.size() * (fb.bytePerPixel() + 8);

            // compare with raw
            if(rawLength < rreLength)
                return sendEncodingRawSubRegion(reg, fb, jobId);

            const std::lock_guard<std::mutex> lock(sendEncoding);

            if(encodingDebug)
                Application::debug("send RRE region, job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, sub rects: %d",
                                   jobId, reg.x, reg.y, reg.w, reg.h, back, goods.size());

            int res = sendHeaderRRE(reg);
            res += sendEncodingRRESubRects(reg, fb, jobId, back, goods);

            return res;
        }
        else if(map.size() == 1)
        {
            int back = fb.pixel(reg.x, reg.y);
            const std::lock_guard<std::mutex> lock(sendEncoding);

            if(encodingDebug)
                Application::debug("send RRE region, job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, %s",
                                   jobId, reg.x, reg.y, reg.w, reg.h, back, "solid");

            int res = sendHeaderRRE(reg);

            // num sub rects
            sendIntBE32(1);
            res += 4;

            // back pixel
            res += sendPixel(back);

            /* one fake sub region : RRE requires */
            // subrect pixel
            res += sendPixel(back);

            // subrect region (relative coords)
            sendIntBE16(0);
            sendIntBE16(0);
            sendIntBE16(1);
            sendIntBE16(1);
            res += 8;

            return res;
        }

        throw std::string("send RRE encoding: pixel map is empty");
        return 0;
    }

    int Connector::VNC::sendEncodingRRESubRects(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId, int back, const std::list<RRE::Region> & rreList)
    {
        // num sub rects
        sendIntBE32(rreList.size());
        int res = 4;

        // back pixel
        res += sendPixel(back);

        for(auto & pair : rreList)
        {
            // subrect pixel
            res += sendPixel(pair.second);

            // subrect region (relative coords)
            sendIntBE16(pair.first.x - reg.x);
            sendIntBE16(pair.first.y - reg.y);
            sendIntBE16(pair.first.w);
            sendIntBE16(pair.first.h);
            res += 8;

            if(1 < encodingDebug)
                Application::debug("send RRE sub region, job id: %d, [%d, %d, %d, %d], back pixel 0x%08x",
                                       jobId, pair.first.x - reg.x, pair.first.y - reg.y, pair.first.w, pair.first.h, pair.second);
        }

        return res;
    }

    /* CoRRE */
    int Connector::VNC::sendEncodingCoRRE(const RFB::Region & reg0, const RFB::FrameBuffer & fb)
    {
        Application::debug("encoding: %s, region: [%d, %d, %d, %d]", "CoRRE", reg0.x, reg0.y, reg0.w, reg0.h);

        auto regions = reg0.divideBlocks(64, 64);
        // regions counts
        sendIntBE16(regions.size());
        int res = 2;
        int jobId = 1;

        // make pool jobs
        while(jobId <= _encodingThreads && ! regions.empty())
        {
            jobsEncodings.push_back(std::async(std::launch::async, & Connector::VNC::sendEncodingCoRRESubRegion, this, std::move(regions.front()), fb, jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : jobsEncodings)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::nanoseconds(200)) == std::future_status::ready)
                {
                    res += job.get();
                    job = std::async(std::launch::async, & Connector::VNC::sendEncodingCoRRESubRegion, this, std::move(regions.front()), fb, jobId);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // get results
        for(auto & job : jobsEncodings)
            res += job.get();

        jobsEncodings.clear();
        return res;
    }

    int Connector::VNC::sendEncodingCoRRESubRegion(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapWeight(reg);

        auto sendHeaderCoRRE = [this](const RFB::Region & reg)
        {
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.w);
            this->sendIntBE16(reg.h);
            // region type
            this->sendIntBE32(RFB::ENCODING_CORRE);
            return 12;
        };

        if(map.size() > 1)
        {
            int back = map.maxWeightPixel();
            std::list<RRE::Region> goods = processingRRE(reg, fb, back);

            const size_t rawLength = reg.w * reg.h * fb.bytePerPixel();
            const size_t rreLength = 4 + fb.bytePerPixel() + goods.size() * (fb.bytePerPixel() + 4);

            // compare with raw
            if(rawLength < rreLength)
                return sendEncodingRawSubRegion(reg, fb, jobId);

            const std::lock_guard<std::mutex> lock(sendEncoding);

            if(encodingDebug)
                Application::debug("send CoRRE region, job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, sub rects: %d",
                                   jobId, reg.x, reg.y, reg.w, reg.h, back, goods.size());

            int res = sendHeaderCoRRE(reg);
            res += sendEncodingCoRRESubRects(reg, fb, jobId, back, goods);

            return res;
        }
        else if(map.size() == 1)
        {
            int back = fb.pixel(reg.x, reg.y);
            const std::lock_guard<std::mutex> lock(sendEncoding);

            if(encodingDebug)
                Application::debug("send CoRRE region, job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, %s",
                                   jobId, reg.x, reg.y, reg.w, reg.h, back, "solid");

            int res = sendHeaderCoRRE(reg);

            // num sub rects
            sendIntBE32(1);
            res += 4;

            // back pixel
            res += sendPixel(back);

            /* one fake sub region : RRE requires */
            // subrect pixel
            res += sendPixel(back);

            // subrect region (relative coords)
            sendInt8(0);
            sendInt8(0);
            sendInt8(1);
            sendInt8(1);
            res += 4;

            return res;
        }

        throw std::string("send CoRRE encoding: pixel map is empty");
        return 0;
    }

    int Connector::VNC::sendEncodingCoRRESubRects(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId, int back, const std::list<RRE::Region> & rreList)
    {
        // num sub rects
        sendIntBE32(rreList.size());
        int res = 4;

        // back pixel
        res += sendPixel(back);

        for(auto & pair : rreList)
        {
            // subrect pixel
            res += sendPixel(pair.second);

            // subrect region (relative coords)
            sendInt8(pair.first.x - reg.x);
            sendInt8(pair.first.y - reg.y);
            sendInt8(pair.first.w);
            sendInt8(pair.first.h);
            res += 4;

            if(1 < encodingDebug)
                Application::debug("send CoRRE sub region, job id: %d, [%d, %d, %d, %d], back pixel 0x%08x",
                                       jobId, pair.first.x - reg.x, pair.first.y - reg.y, pair.first.w, pair.first.h, pair.second);
        }

        return res;
    }

    /* HexTile */
    int Connector::VNC::sendEncodingHextile(const RFB::Region & reg0, const RFB::FrameBuffer & fb)
    {
        Application::debug("encoding: %s, region: [%d, %d, %d, %d]", "HexTile", reg0.x, reg0.y, reg0.w, reg0.h);

        auto regions = reg0.divideBlocks(16, 16);
        // regions counts
        sendIntBE16(regions.size());
        int res = 2;
        int jobId = 1;

        // make pool jobs
        while(jobId <= _encodingThreads && ! regions.empty())
        {
            jobsEncodings.push_back(std::async(std::launch::async, & Connector::VNC::sendEncodingHextileSubRegion, this, std::move(regions.front()), fb, jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : jobsEncodings)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::nanoseconds(100)) == std::future_status::ready)
                {
                    res += job.get();
                    job = std::async(std::launch::async, & Connector::VNC::sendEncodingHextileSubRegion, this, std::move(regions.front()), fb, jobId);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // get results
        for(auto & job : jobsEncodings)
            res += job.get();

        jobsEncodings.clear();
        return res;
    }

    int Connector::VNC::sendEncodingHextileSubRegion(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapWeight(reg);

	auto sendHeaderHexTile = [this](const RFB::Region & reg)
	{
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.w);
            this->sendIntBE16(reg.h);
            // region type
            this->sendIntBE32(RFB::ENCODING_HEXTILE);
            return 12;
        };

        if(map.size() == 1)
        {
            // wait thread
            const std::lock_guard<std::mutex> lock(sendEncoding);

            int res = sendHeaderHexTile(reg);
            int back = fb.pixel(reg.x, reg.y);

            if(encodingDebug)
                Application::debug("send HexTile region, job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, %s",
                                   jobId, reg.x, reg.y, reg.w, reg.h, back, "solid");

            // hextile flags
            sendInt8(RFB::HEXTILE_BACKGROUND);
            res += 1 + sendPixel(back);

            return res;
        }
        else if(map.size() > 1)
        {
            // no wait, worked
            int back = map.maxWeightPixel();
            std::list<RRE::Region> goods = processingRRE(reg, fb, back);

            // all other color
            bool foreground = std::all_of(goods.begin(), goods.end(),
                    [col = goods.front().second](auto & pair) { return pair.second == col; });

            const size_t hextileRawLength = 1 + reg.w * reg.h * fb.bytePerPixel();

            // wait thread
            const std::lock_guard<std::mutex> lock(sendEncoding);
            int res = sendHeaderHexTile(reg);

            if(foreground)
            {
                const size_t hextileForegroundLength = 2 + 2 * fb.bytePerPixel() + goods.size() * 2;

                // compare with raw
                if(hextileRawLength < hextileForegroundLength)
                {
                    if(encodingDebug)
                        Application::debug("send HexTile region, job id: %d, [%d, %d, %d, %d], %s",
                                      jobId, reg.x, reg.y, reg.w, reg.h, "raw");

                    res = sendEncodingHextileSubRaw(reg, fb, jobId);
                }
                else
                {
                    if(encodingDebug)
                        Application::debug("send HexTile region, job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, sub rects: %d, %s",
                                       jobId, reg.x, reg.y, reg.w, reg.h, back, goods.size(), "foreground");

                    res = sendEncodingHextileSubForeground(reg, fb, jobId, back, goods);
                }
            }
            else
            {
                const size_t hextileColoredLength = 2 + fb.bytePerPixel() + goods.size() * (2 + fb.bytePerPixel());

                // compare with raw
                if(hextileRawLength < hextileColoredLength)
                {
                    if(encodingDebug)
                        Application::debug("send HexTile region, job id: %d, [%d, %d, %d, %d], %s",
                                      jobId, reg.x, reg.y, reg.w, reg.h, "raw");

                    res = sendEncodingHextileSubRaw(reg, fb, jobId);
                }
                else
                {
                    if(encodingDebug)
                        Application::debug("send HexTile region, job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, sub rects: %d, %s",
                                       jobId, reg.x, reg.y, reg.w, reg.h, back, goods.size(), "colored");

                    res = sendEncodingHextileSubColored(reg, fb, jobId, back, goods);
                }
            }

            return res;
        }

        throw std::string("send Hextile encoding: pixel map is empty");
        return 0;
    }

    int Connector::VNC::sendEncodingHextileSubColored(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId, int back, const std::list<RRE::Region> & rreList)
    {
        // hextile flags
        sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_COLOURED | RFB::HEXTILE_SUBRECTS);
        int res = 1;

        // hextile background
        res += sendPixel(back);

        // hextile subrects
        sendInt8(rreList.size());
        res += 1;

        for(auto & pair : rreList)
        {
            res += sendPixel(pair.second);
            sendInt8(0xFF & ((pair.first.x - reg.x) << 4 | (pair.first.y - reg.y)));
            sendInt8(0xFF & ((pair.first.w - 1) << 4 | (pair.first.h - 1)));
            res += 2;

            if(1 < encodingDebug)
                Application::debug("send HexTile sub region, job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x",
                                   jobId, pair.first.x - reg.x, pair.first.y - reg.y, pair.first.w, pair.first.h, pair.second);
        }

        return res;
    }

    int Connector::VNC::sendEncodingHextileSubForeground(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId, int back, const std::list<RRE::Region> & rreList)
    {
        // hextile flags
        sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_FOREGROUND | RFB::HEXTILE_SUBRECTS);
        int res = 1;

        // hextile background
        res += sendPixel(back);

        // hextile foreground
        res += sendPixel(rreList.front().second);

        // hextile subrects
        sendInt8(rreList.size());
        res += 1;

        for(auto & pair : rreList)
        {
            sendInt8(0xFF & ((pair.first.x - reg.x) << 4 | (pair.first.y - reg.y)));
            sendInt8(0xFF & ((pair.first.w - 1) << 4 | (pair.first.h - 1)));
            res += 2;

            if(1 < encodingDebug)
                Application::debug("send HexTile sub region, job id: %d, [%d, %d, %d, %d]",
                                   jobId, pair.first.x - reg.x, pair.first.y - reg.y, pair.first.w, pair.first.h);
        }

        return res;
    }

    int Connector::VNC::sendEncodingHextileSubRaw(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId)
    {
        // hextile flags
        sendInt8(RFB::HEXTILE_RAW);
        int res = 1;

        res += sendEncodingRawSubRegionRaw(reg, fb);
        return res;
    }

    /* ZLib */
    int Connector::VNC::sendEncodingZLib(const RFB::Region & reg0, const RFB::FrameBuffer & fb)
    {
        Application::debug("encoding: %s, region: [%d, %d, %d, %d]", "ZLib", reg0.x, reg0.y, reg0.w, reg0.h);

        std::list<RFB::Region> regions = reg0.divideBlocks(64, 64);
        // regions counts
        sendIntBE16(regions.size());
        int res = 2;
        int jobId = 1;

        // make pool jobs
        while(jobId <= _encodingThreads && ! regions.empty())
        {
            jobsEncodings.push_back(std::async(std::launch::async, & Connector::VNC::sendEncodingZLibSubRegion, this, std::move(regions.front()), fb, jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : jobsEncodings)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::nanoseconds(300)) == std::future_status::ready)
                {
                    res += job.get();
                    job = std::async(std::launch::async, & Connector::VNC::sendEncodingZLibSubRegion, this, std::move(regions.front()), fb, jobId);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // get results
        for(auto & job : jobsEncodings)
            res += job.get();

        jobsEncodings.clear();
        return res;
    }

    int Connector::VNC::sendEncodingZLibSubRegion(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId)
    {
        const std::lock_guard<std::mutex> lock(sendEncoding);

        if(encodingDebug)
            Application::debug("send ZLib region, job id: %d, [%d, %d, %d, %d]", jobId, reg.x, reg.y, reg.w, reg.h);

        // region size
        sendIntBE16(reg.x);
        sendIntBE16(reg.y);
        sendIntBE16(reg.w);
        sendIntBE16(reg.h);

        // region type
        sendIntBE32(RFB::ENCODING_ZLIB);
        int res = 12;

        RFB::FrameBuffer clientfb(reg.w, reg.h, clientFormat);

        if(serverFormat != clientFormat)
        {
            for(int yy = 0; yy < clientfb.height; ++yy)
                for(int xx = 0; xx < clientfb.width; ++xx)
                    clientfb.setPixel(xx, yy, fb.pixel(reg.x + xx, reg.y + yy), serverFormat);
        }
        else
        {
            for(int row = 0; row < reg.h; ++row)
            {
                auto ptr = fb.pitchData(reg.y + row) + reg.x * serverFormat.bytePerPixel();
                size_t length = reg.w * serverFormat.bytePerPixel();
                std::copy(ptr, ptr + length, clientfb.pitchData(row));
            }
        }

        auto ld = libdeflate_alloc_compressor(6);
        if(! ld) throw std::string("libdeflate_alloc failed");

        // deflate, zlib, or gzip?
        std::vector<uint8_t> data(libdeflate_zlib_compress_bound(ld, clientfb.size()));
        size_t zipsz = libdeflate_zlib_compress(ld, clientfb.data(), clientfb.size(), data.data(), data.size());

        sendIntBE32(zipsz);
        sendRaw(data.data(), zipsz);
        libdeflate_free_compressor(ld);

        return res;
    }

    /* TRLE */
    int Connector::VNC::sendEncodingTRLE(const RFB::Region & reg0, const RFB::FrameBuffer & fb)
    {
        Application::debug("encoding: %s, region: [%d, %d, %d, %d]", "TRLE", reg0.x, reg0.y, reg0.w, reg0.h);

        auto regions = reg0.divideBlocks(16, 16);
        // regions counts
        sendIntBE16(regions.size());
        int res = 2;
        int jobId = 1;

        // make pool jobs
        while(jobId <= _encodingThreads && ! regions.empty())
        {
            jobsEncodings.push_back(std::async(std::launch::async, & Connector::VNC::sendEncodingTRLESubRegion, this, std::move(regions.front()), fb, jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : jobsEncodings)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::nanoseconds(100)) == std::future_status::ready)
                {
                    res += job.get();
                    job = std::async(std::launch::async, & Connector::VNC::sendEncodingTRLESubRegion, this, std::move(regions.front()), fb, jobId);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // get results
        for(auto & job : jobsEncodings)
            res += job.get();

        jobsEncodings.clear();
        return res;
    }

    int Connector::VNC::sendEncodingTRLESubRegion(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapWeight(reg);

	// convert to palette
        int index = 0;
        for(auto & pair : map)
            pair.second = index++;

	auto sendHeaderTRLE = [this](const RFB::Region & reg)
	{
    	    // region size
    	    this->sendIntBE16(reg.x);
    	    this->sendIntBE16(reg.y);
    	    this->sendIntBE16(reg.w);
    	    this->sendIntBE16(reg.h);
    	    // region type
    	    this->sendIntBE32(RFB::ENCODING_TRLE);
	    return 12;
	};

        if(map.size() == 1)
        {
            int back = fb.pixel(reg.x, reg.y);

    	    // wait thread
    	    const std::lock_guard<std::mutex> lock(sendEncoding);
	    int res = sendHeaderTRLE(reg);

            if(encodingDebug)
                Application::debug("send TRLE region, job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, %s",
                                   jobId, reg.x, reg.y, reg.w, reg.h, back, "solid");

            // subencoding type: solid tile
            sendInt8(map.size());
            res += 1;

            res += sendCPixel(back);
            return res;
        }
        else
        if(2 <= map.size() && map.size() <= 16)
        {
	    size_t field = 1;

            if(4 < map.size())
		field = 4;
	    else
            if(2 < map.size())
		field = 2;

	    size_t bits = field * reg.w;
	    size_t rowsz = bits >> 3;
	    if((rowsz << 3) < bits) rowsz++;

    	    // wait thread
    	    const std::lock_guard<std::mutex> lock(sendEncoding);
	    int res = sendHeaderTRLE(reg);

            if(encodingDebug)
                Application::debug("send TRLE region, job id: %d, [%d, %d, %d, %d], palsz: %d, rowsz: %d, packed: %d",
                                   jobId, reg.x, reg.y, reg.w, reg.h, map.size(), field, rowsz, field);

	    res += sendEncodingTRLESubPacked(reg, fb, jobId, field, rowsz, map);

            return res;
        }
        else
	{
	    auto rleList = fb.FrameBuffer::toRLE(reg);

	    // rle plain size
	    const size_t rlePlainLength = std::accumulate(rleList.begin(), rleList.end(), 1,
				[](int v, auto & pair) { return v + 3 + std::floor((pair.second - 1) / 255.0) + 1; });

	    // rle palette size (2, 127)
	    const size_t rlePaletteLength = 1 < rleList.size() && rleList.size() < 128 ? std::accumulate(rleList.begin(), rleList.end(), 1 + 3 * map.size(),
				[](int v, auto & pair) { return v + 1 + std::floor((pair.second - 1) / 255.0) + 1; }) : 0xFFFF;

	    // raw length
	    const size_t rawLength = 1 + 3 * reg.w * reg.h;

    	    // wait thread
    	    const std::lock_guard<std::mutex> lock(sendEncoding);
	    int res = sendHeaderTRLE(reg);

	    if(rlePlainLength < rlePaletteLength && rlePlainLength < rawLength)
	    {
    		if(encodingDebug)
        	    Application::debug("send TRLE region, job id: %d, [%d, %d, %d, %d], length: %d, rle plain",
                                   jobId, reg.x, reg.y, reg.w, reg.h, rleList.size());
		res += sendEncodingTRLESubPlain(reg, fb, rleList);
	    }
	    else
	    if(rlePaletteLength < rlePlainLength && rlePaletteLength < rawLength)
	    {
        	if(encodingDebug)
            	    Application::debug("send TRLE region, job id: %d, [%d, %d, %d, %d], pal size: %d, length: %d, rle palette",
                                   jobId, reg.x, reg.y, reg.w, reg.h, map.size(), rleList.size());
		res += sendEncodingTRLESubPalette(reg, fb, map, rleList);
	    }
	    else
	    {
    		if(encodingDebug)
        	    Application::debug("send TRLE region, job id: %d, [%d, %d, %d, %d], raw",
                                   jobId, reg.x, reg.y, reg.w, reg.h);
		res += sendEncodingTRLESubRaw(reg, fb);
	    }

	    return res;
	}

        throw std::string("send TRLE encoding: pixel map is empty");
        return 0;
    }

    int Connector::VNC::sendEncodingTRLESubPacked(const RFB::Region & reg, const RFB::FrameBuffer & fb, int jobId, size_t field, size_t rowsz, const RFB::PixelMapWeight & pal)
    {
        // subencoding type: packed palette
        sendInt8(pal.size());
        int res = 1;

	// send palette
        for(auto & pair : pal)
            res += sendCPixel(pair.first);

        std::vector<uint8_t> packedRowIndexes(rowsz);

	// send packed rows
        for(int oy = 0; oy < reg.h; ++oy)
        {
            Tools::StreamBits sb(packedRowIndexes, 7);

            for(int ox = 0; ox < reg.w; ++ox)
            {
                int pixel = fb.pixel(reg.x + ox, reg.y + oy);
	        auto it = pal.find(pixel);
	        int index = it != pal.end() ? (*it).second : 0;

        	size_t mask = 1 << (field - 1);
        	while(mask)
        	{
            	    sb.pushBitBE(index & mask);
            	    mask >>= 1;
        	}
            }

            for(auto & v : packedRowIndexes)
		sendInt8(v);

            if(1 < encodingDebug)
	    {
		std::ostringstream os;
        	for(auto & v : packedRowIndexes)
		    os << "0x" << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(v) << " ";
            	Application::debug("send TRLE region, job id: %d, packed stream: %s", jobId, os.str().c_str());
	    }

            res += packedRowIndexes.size();
        }

        return res;
    }

    int Connector::VNC::sendEncodingTRLESubPlain(const RFB::Region & reg, const RFB::FrameBuffer & fb, const std::list<RFB::RLE> & rle)
    {
        // subencoding type: rle plain
        sendInt8(128);
	int res = 1;

	// send rle content
	for(auto & pair : rle)
	{
	    res += sendCPixel(pair.first);
	    size_t length = pair.second;

	    while(255 < length)
	    {
		sendInt8(255);
        	res += 1;
		length -= 255;
	    }

	    sendInt8((length - 1) % 255);
    	    res += 1;
	}

	return res;
    }

    int Connector::VNC::sendEncodingTRLESubPalette(const RFB::Region & reg, const RFB::FrameBuffer & fb, const RFB::PixelMapWeight & pal, const std::list<RFB::RLE> & rle)
    {
	// subencoding type: rle palette
        sendInt8(pal.size() + 128);
        int res = 1;

	// send palette
        for(auto & pair : pal)
            res += sendCPixel(pair.first);

	// send rle indexes
	for(auto & pair : rle)
	{
	    auto it = pal.find(pair.first);
	    int index = it != pal.end() ? (*it).second : 0;

	    if(1 == pair.second)
	    {
		sendInt8(index);
        	res += 1;
	    }
	    else
	    {
		sendInt8(index + 128);
        	res += 1;
		size_t length = pair.second;

		while(255 < length)
		{
		    sendInt8(255);
        	    res += 1;
		    length -= 255;
		}

		sendInt8((length - 1) % 255);
        	res += 1;
	    }
	}

        return res;
    }

    int Connector::VNC::sendEncodingTRLESubRaw(const RFB::Region & reg, const RFB::FrameBuffer & fb)
    {
        // subencoding type: raw
        sendInt8(0);
        int res = 1;

	// send pixels
        for(int oy = 0; oy < reg.h; ++oy)
        {
            for(int ox = 0; ox < reg.w; ++ox)
            {
                int pixel = fb.pixel(reg.x + ox, reg.y + oy);
                res += sendCPixel(pixel);
            }
        }

        return res;
    }
}
