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
#include <string>
#include <chrono>
#include <thread>
#include <future>
#include <numeric>
#include <iomanip>
#include <iostream>
#include <iterator>

#include "ltsm_tools.h"
#include "ltsm_librfb.h"
#include "ltsm_application.h"

using namespace std::chrono_literals;

namespace LTSM
{
    namespace RFB
    {
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

                case ENCODING_DESKTOP_SIZE:
                    return "DesktopSize";

                case ENCODING_EXT_DESKTOP_SIZE:
                    return "ExtendedDesktopSize";

                case ENCODING_LAST_RECT:
                    return "ExtendedLastRect";

                case ENCODING_COMPRESS9:
                    return "ExtendedCompress9";

                case ENCODING_COMPRESS8:
                    return "ExtendedCompress8";

                case ENCODING_COMPRESS7:
                    return "ExtendedCompress7";

                case ENCODING_COMPRESS6:
                    return "ExtendedCompress6";

                case ENCODING_COMPRESS5:
                    return "ExtendedCompress5";

                case ENCODING_COMPRESS4:
                    return "ExtendedCompress4";

                case ENCODING_COMPRESS3:
                    return "ExtendedCompress3";

                case ENCODING_COMPRESS2:
                    return "ExtendedCompress2";

                case ENCODING_COMPRESS1:
                    return "ExtendedCompress1";

                case ENCODING_CONTINUOUS_UPDATES:
                    return "ExtendedContinuousUpdates";

                default:
                    break;
            }

            return "unknown";
        }
    }

    bool RFB::ServerEncoding::serverSelectClientEncoding(void)
    {
        for(int type : clientEncodings)
        {
            switch(type)
            {
                case RFB::ENCODING_ZLIB:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingZLib(fb); }, type);
                    return true;

                case RFB::ENCODING_HEXTILE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingHextile(fb, false); }, type);
                    return true;

                case RFB::ENCODING_ZLIBHEX:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingHextile(fb, true); }, type);
                    return true;

                case RFB::ENCODING_CORRE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingRRE(fb, true); }, type);
                    return true;

                case RFB::ENCODING_RRE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingRRE(fb, false); }, type);
                    return true;

                case RFB::ENCODING_TRLE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingTRLE(fb, false); }, type);
                    return true;

                case RFB::ENCODING_ZRLE:
                    prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingTRLE(fb, true); }, type);
                    return true;

                default:
                    break;
            }
        }

        prefEncodingsPair = std::make_pair([=](const FrameBuffer & fb) { return this->sendEncodingRaw(fb); }, RFB::ENCODING_RAW);
        return true;
    }

    void RFB::ServerEncoding::serverSelectEncodings(void)
    {
        serverSelectClientEncoding();
        Application::notice("%s: select encoding: %s", __FUNCTION__, RFB::encodingName(prefEncodingsPair.second));
    }

    void RFB::ServerEncoding::sendEncodingRaw(const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        // regions counts
        sendIntBE16(1);
        sendEncodingRawSubRegion(XCB::Point(0, 0), reg0, fb, 1);
    }

    void RFB::ServerEncoding::sendEncodingRawSubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        const std::lock_guard<std::mutex> lock(encodingBusy);

        if(encodingDebug)
            Application::debug("%s: job id: %d, [%d, %d, %d, %d]", __FUNCTION__, jobId, reg.x, reg.y, reg.width, reg.height);

        // region size
        sendIntBE16(top.x + reg.x);
        sendIntBE16(top.y + reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        // region type
        sendIntBE32(RFB::ENCODING_RAW);
        sendEncodingRawSubRegionRaw(reg, fb);
    }

    void RFB::ServerEncoding::sendEncodingRawSubRegionRaw(const XCB::Region & reg, const FrameBuffer & fb)
    {
        if(serverFormat != clientFormat)
        {
            for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
                sendPixel(fb.pixel(reg.topLeft() + coord));
        }
        else
        {
            for(int yy = 0; yy < reg.height; ++yy)
            {
                size_t line = reg.width * fb.bytePerPixel();
                sendRaw(fb.pitchData(reg.y + yy) + reg.x * fb.bytePerPixel(), line);
            }
        }
    }

    std::list<XCB::RegionPixel> processingRRE(const XCB::Region & badreg, const FrameBuffer & fb, int skipPixel)
    {
        std::list<XCB::RegionPixel> goods;
        std::list<XCB::Region> bads1 = { badreg };
        std::list<XCB::Region> bads2;

        do
        {
            while(! bads1.empty())
            {
                for(auto & subreg : bads1.front().divideCounts(2, 2))
                {
                    auto pixel = fb.pixel(subreg.topLeft());

                    if((subreg.width == 1 && subreg.height == 1) || fb.allOfPixel(pixel, subreg))
                    {
                        if(pixel != skipPixel)
                        {
                            // maybe join prev
                            if(! goods.empty() && goods.back().first.y == subreg.y && goods.back().first.height == subreg.height &&
                               goods.back().first.x + goods.back().first.width == subreg.x && goods.back().second == pixel)
                                goods.back().first.width += subreg.width;
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
    void RFB::ServerEncoding::sendEncodingRRE(const FrameBuffer & fb, bool corre)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: type: %s, region: [%d, %d, %d, %d]", __FUNCTION__, (corre ? "CoRRE" : "RRE"), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz = corre ? XCB::Size(64, 64) : XCB::Size(128, 128);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= encodingThreads && ! regions.empty())
        {
            encodingJobs.push_back(std::async(std::launch::async, & RFB::ServerEncoding::sendEncodingRRESubRegion, this, top, regions.front() - top, fb, jobId, corre));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : encodingJobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::microseconds(1)) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & RFB::ServerEncoding::sendEncodingRRESubRegion, this, top, regions.front() - top, fb, jobId, corre);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // wait jobs
        for(auto & job : encodingJobs)
            job.wait();

        encodingJobs.clear();
    }

    void RFB::ServerEncoding::sendEncodingRRESubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId, bool corre)
    {
        auto map = fb.pixelMapWeight(reg);
        auto sendHeaderRRE = [this](const XCB::Region & reg, bool corre)
        {
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.width);
            this->sendIntBE16(reg.height);
            // region type
            this->sendIntBE32(corre ? RFB::ENCODING_CORRE : RFB::ENCODING_RRE);
        };

        if(map.empty())
            throw std::runtime_error("VNC::sendEncodingRRESubRegion: pixel map is empty");

        if(map.size() > 1)
        {
            int back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = processingRRE(reg, fb, back);
            const size_t rawLength = reg.width * reg.height * fb.bytePerPixel();
            const size_t rreLength = 4 + fb.bytePerPixel() + goods.size() * (fb.bytePerPixel() + (corre ? 4 : 8));

            // compare with raw
            if(rawLength < rreLength)
                sendEncodingRawSubRegion(top, reg, fb, jobId);
            else
            {
                const std::lock_guard<std::mutex> lock(encodingBusy);

                if(encodingDebug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, sub rects: %d",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size());

                sendHeaderRRE(reg + top, corre);
                sendEncodingRRESubRects(reg, fb, jobId, back, goods, corre);
            }
        }
        // if(map.size() == 1)
        else
        {
            int back = fb.pixel(reg.topLeft());
            const std::lock_guard<std::mutex> lock(encodingBusy);

            if(encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            sendHeaderRRE(reg + top, corre);
            // num sub rects
            sendIntBE32(1);
            // back pixel
            sendPixel(back);
            /* one fake sub region : RRE requires */
            // subrect pixel
            sendPixel(back);

            // subrect region (relative coords)
            if(corre)
            {
                sendInt8(0);
                sendInt8(0);
                sendInt8(1);
                sendInt8(1);
            }
            else
            {
                sendIntBE16(0);
                sendIntBE16(0);
                sendIntBE16(1);
                sendIntBE16(1);
            }
        }
    }

    void RFB::ServerEncoding::sendEncodingRRESubRects(const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList, bool corre)
    {
        // num sub rects
        sendIntBE32(rreList.size());
        // back pixel
        sendPixel(back);

        for(auto & pair : rreList)
        {
            // subrect pixel
            sendPixel(pair.pixel());
            auto & region = pair.region();

            // subrect region (relative coords)
            if(corre)
            {
                sendInt8(region.x - reg.x);
                sendInt8(region.y - reg.y);
                sendInt8(region.width);
                sendInt8(region.height);
            }
            else
            {
                sendIntBE16(region.x - reg.x);
                sendIntBE16(region.y - reg.y);
                sendIntBE16(region.width);
                sendIntBE16(region.height);
            }

            if(1 < encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    /* HexTile */
    void RFB::ServerEncoding::sendEncodingHextile(const FrameBuffer & fb, bool zlibver)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz(16, 16);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= encodingThreads && ! regions.empty())
        {
            encodingJobs.push_back(std::async(std::launch::async, & RFB::ServerEncoding::sendEncodingHextileSubRegion, this, top, regions.front() - top, fb, jobId, zlibver));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : encodingJobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::microseconds(1)) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & RFB::ServerEncoding::sendEncodingHextileSubRegion, this, top, regions.front() - top, fb, jobId, zlibver);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // wait jobs
        for(auto & job : encodingJobs)
            job.wait();

        encodingJobs.clear();
    }

    void RFB::ServerEncoding::sendEncodingHextileSubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId, bool zlibver)
    {
        auto map = fb.pixelMapWeight(reg);
        auto sendHeaderHexTile = [this](const XCB::Region & reg, bool zlibver)
        {
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.width);
            this->sendIntBE16(reg.height);
            // region type
            this->sendIntBE32(zlibver ? RFB::ENCODING_ZLIBHEX : RFB::ENCODING_HEXTILE);
        };

        if(map.empty())
            throw std::runtime_error("VNC::sendEncodingHextileSubRegion: pixel map is empty");

        if(map.size() == 1)
        {
            // wait thread
            const std::lock_guard<std::mutex> lock(encodingBusy);
            sendHeaderHexTile(reg + top, zlibver);
            int back = fb.pixel(reg.topLeft());

            if(encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // hextile flags
            sendInt8(RFB::HEXTILE_BACKGROUND);
            sendPixel(back);
        }
        else if(map.size() > 1)
        {
            // no wait, worked
            int back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = processingRRE(reg, fb, back);
            // all other color
            bool foreground = std::all_of(goods.begin(), goods.end(),
                                          [col = goods.front().second](auto & pair)
            {
                return pair.pixel() == col;
            });
            const size_t hextileRawLength = 1 + reg.width * reg.height * fb.bytePerPixel();
            // wait thread
            const std::lock_guard<std::mutex> lock(encodingBusy);
            sendHeaderHexTile(reg + top, zlibver);

            if(foreground)
            {
                const size_t hextileForegroundLength = 2 + 2 * fb.bytePerPixel() + goods.size() * 2;

                // compare with raw
                if(hextileRawLength < hextileForegroundLength)
                {
                    if(encodingDebug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");

                    sendEncodingHextileSubRaw(reg, fb, jobId, zlibver);
                }
                else
                {
                    if(encodingDebug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, sub rects: %d, %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "foreground");

                    sendEncodingHextileSubForeground(reg, fb, jobId, back, goods);
                }
            }
            else
            {
                const size_t hextileColoredLength = 2 + fb.bytePerPixel() + goods.size() * (2 + fb.bytePerPixel());

                // compare with raw
                if(hextileRawLength < hextileColoredLength)
                {
                    if(encodingDebug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");

                    sendEncodingHextileSubRaw(reg, fb, jobId, zlibver);
                }
                else
                {
                    if(encodingDebug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, sub rects: %d, %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "colored");

                    sendEncodingHextileSubColored(reg, fb, jobId, back, goods);
                }
            }
        }
    }

    void RFB::ServerEncoding::sendEncodingHextileSubColored(const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {
        // hextile flags
        sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_COLOURED | RFB::HEXTILE_SUBRECTS);
        // hextile background
        sendPixel(back);
        // hextile subrects
        sendInt8(rreList.size());

        for(auto & pair : rreList)
        {
            auto & region = pair.region();
            sendPixel(pair.pixel());
            sendInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            sendInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            if(1 < encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    void RFB::ServerEncoding::sendEncodingHextileSubForeground(const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {
        // hextile flags
        sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_FOREGROUND | RFB::HEXTILE_SUBRECTS);
        // hextile background
        sendPixel(back);
        // hextile foreground
        sendPixel(rreList.front().second);
        // hextile subrects
        sendInt8(rreList.size());

        for(auto & pair : rreList)
        {
            auto & region = pair.region();
            sendInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            sendInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            if(1 < encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d]",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height);
        }
    }

    void RFB::ServerEncoding::sendEncodingHextileSubRaw(const XCB::Region & reg, const FrameBuffer & fb, int jobId, bool zlibver)
    {
        if(zlibver)
        {
            // hextile flags
            sendInt8(RFB::HEXTILE_ZLIBRAW);
            zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());
            sendEncodingRawSubRegionRaw(reg, fb);
            auto zip = zlibDeflateStop();
            sendIntBE16(zip.size());
            sendRaw(zip.data(), zip.size());
        }
        else
        {
            // hextile flags
            sendInt8(RFB::HEXTILE_RAW);
            sendEncodingRawSubRegionRaw(reg, fb);
        }
    }

    /* ZLib */
    void RFB::ServerEncoding::sendEncodingZLib(const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        // zlib specific: single thread only
        sendIntBE16(1);
        sendEncodingZLibSubRegion(XCB::Point(0, 0), reg0, fb, 1);
    }

    void RFB::ServerEncoding::sendEncodingZLibSubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        const std::lock_guard<std::mutex> lock(encodingBusy);

        if(encodingDebug)
            Application::debug("%s: job id: %d, [%d, %d, %d, %d]", __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

        // region size
        sendIntBE16(top.x + reg.x);
        sendIntBE16(top.y + reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        // region type
        sendIntBE32(RFB::ENCODING_ZLIB);
        zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());
        sendEncodingRawSubRegionRaw(reg, fb);
        auto zip = zlibDeflateStop();
        sendIntBE32(zip.size());
        sendRaw(zip.data(), zip.size());
    }

    /* TRLE */
    void RFB::ServerEncoding::sendEncodingTRLE(const FrameBuffer & fb, bool zrle)
    {
        const XCB::Region & reg0 = fb.region();

        if(encodingDebug)
            Application::debug("%s: type: %s, region: [%d, %d, %d, %d]", __FUNCTION__, (zrle ? "ZRLE" : "TRLE"), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Size bsz(64, 64);
        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= encodingThreads && ! regions.empty())
        {
            encodingJobs.push_back(std::async(std::launch::async, & RFB::ServerEncoding::sendEncodingTRLESubRegion, this, top, regions.front() - top, fb, jobId, zrle));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : encodingJobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(std::chrono::microseconds(1)) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & RFB::ServerEncoding::sendEncodingTRLESubRegion, this, top, regions.front() - top, fb, jobId, zrle);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // wait jobs
        for(auto & job : encodingJobs)
            job.wait();

        encodingJobs.clear();
    }

    void RFB::ServerEncoding::sendEncodingTRLESubRegion(const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId, bool zrle)
    {
        auto map = fb.pixelMapWeight(reg);
        // convert to palette
        int index = 0;

        for(auto & pair : map)
            pair.second = index++;

        auto sendHeaderTRLE = [this](const XCB::Region & reg, bool zrle)
        {
            // region size
            this->sendIntBE16(reg.x);
            this->sendIntBE16(reg.y);
            this->sendIntBE16(reg.width);
            this->sendIntBE16(reg.height);
            // region type
            this->sendIntBE32(zrle ? RFB::ENCODING_ZRLE : RFB::ENCODING_TRLE);
        };
        // wait thread
        const std::lock_guard<std::mutex> lock(encodingBusy);
        sendHeaderTRLE(reg + top, zrle);

        if(zrle)
            zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());

        if(map.size() == 1)
        {
            int back = fb.pixel(reg.topLeft());

            if(encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // subencoding type: solid tile
            sendInt8(1);
            sendCPixel(back);
        }
        else if(2 <= map.size() && map.size() <= 16)
        {
            size_t fieldWidth = 1;

            if(4 < map.size())
                fieldWidth = 4;
            else if(2 < map.size())
                fieldWidth = 2;

            if(encodingDebug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], palsz: %d, packed: %d",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), fieldWidth);

            sendEncodingTRLESubPacked(reg, fb, jobId, fieldWidth, map, zrle);
        }
        else
        {
            auto rleList = fb.FrameBuffer::toRLE(reg);
            // rle plain size
            const size_t rlePlainLength = std::accumulate(rleList.begin(), rleList.end(), 1,
                                          [](int v, auto & pair)
            {
                return v + 3 + std::floor((pair.second - 1) / 255.0) + 1;
            });
            // rle palette size (2, 127)
            const size_t rlePaletteLength = 1 < rleList.size() && rleList.size() < 128 ? std::accumulate(rleList.begin(), rleList.end(), 1 + 3 * map.size(),
                                            [](int v, auto & pair)
            {
                return v + 1 + std::floor((pair.second - 1) / 255.0) + 1;
            }) : 0xFFFF;
            // raw length
            const size_t rawLength = 1 + 3 * reg.width * reg.height;

            if(rlePlainLength < rlePaletteLength && rlePlainLength < rawLength)
            {
                if(encodingDebug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], length: %d, rle plain",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, rleList.size());

                sendEncodingTRLESubPlain(reg, fb, rleList);
            }
            else if(rlePaletteLength < rlePlainLength && rlePaletteLength < rawLength)
            {
                if(encodingDebug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], pal size: %d, length: %d, rle palette",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), rleList.size());

                sendEncodingTRLESubPalette(reg, fb, map, rleList);
            }
            else
            {
                if(encodingDebug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], raw",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

                sendEncodingTRLESubRaw(reg, fb);
            }
        }

        if(zrle)
        {
            auto zip = zlibDeflateStop();
            sendIntBE32(zip.size());
            sendRaw(zip.data(), zip.size());
        }
    }

    void RFB::ServerEncoding::sendEncodingTRLESubPacked(const XCB::Region & reg, const FrameBuffer & fb, int jobId, size_t field, const PixelMapWeight & pal, bool zrle)
    {
        // subencoding type: packed palette
        sendInt8(pal.size());

        // send palette
        for(auto & pair : pal)
            sendCPixel(pair.first);

        Tools::StreamBitsPack sb;

        // send packed rows
        for(int oy = 0; oy < reg.height; ++oy)
        {
            for(int ox = 0; ox < reg.width; ++ox)
            {
                auto pixel = fb.pixel(reg.topLeft() + XCB::Point(ox, oy));
                auto it = pal.find(pixel);
                auto index = it != pal.end() ? (*it).second : 0;
                sb.pushValue(index, field);
            }

            sb.pushAlign();
        }

        sendData(sb.toVector());

        if(1 < encodingDebug)
        {
            std::string str = Tools::buffer2hexstring<uint8_t>(sb.toVector().data(), sb.toVector().size(), 2);
            Application::debug("%s: job id: %d, packed stream: %s", __FUNCTION__, jobId, str.c_str());
        }
    }

    void RFB::ServerEncoding::sendEncodingTRLESubPlain(const XCB::Region & reg, const FrameBuffer & fb, const std::list<PixelLength> & rle)
    {
        // subencoding type: rle plain
        sendInt8(128);

        // send rle content
        for(auto & pair : rle)
        {
            sendCPixel(pair.pixel());
            sendRunLength(pair.length());
        }
    }

    void RFB::ServerEncoding::sendEncodingTRLESubPalette(const XCB::Region & reg, const FrameBuffer & fb, const PixelMapWeight & pal, const std::list<PixelLength> & rle)
    {
        // subencoding type: rle palette
        sendInt8(pal.size() + 128);

        // send palette
        for(auto & pair : pal)
            sendCPixel(pair.first);

        // send rle indexes
        for(auto & pair : rle)
        {
            auto it = pal.find(pair.pixel());
            auto index = it != pal.end() ? (*it).second : 0;

            if(1 == pair.length())
                sendInt8(index);
            else
            {
                sendInt8(index + 128);
                sendRunLength(pair.length());
            }
        }
    }

    void RFB::ServerEncoding::sendEncodingTRLESubRaw(const XCB::Region & reg, const FrameBuffer & fb)
    {
        // subencoding type: raw
        sendInt8(0);

        // send pixels
        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
            sendCPixel(fb.pixel(reg.topLeft() + coord));
    }

    /* pseudo encodings DesktopSize/Extended */
    bool RFB::ServerEncoding::sendEncodingDesktopSize(bool xcbAllow)
    {
        if(DesktopResizeMode::Undefined == desktopMode ||
            DesktopResizeMode::Disabled == desktopMode || DesktopResizeMode::Success == desktopMode)
            return false;

        int status = 0;
        int error = 0;
        int screenId = 0;
        int screenFlags = 0;
        int width = 0;
        int height = 0;

        if(xcbAllow)
        {
            auto wsz = xcbDisplay()->size();
            width = wsz.width;
            height = wsz.height;
        }

        bool extended = isClientEncodings(RFB::ENCODING_EXT_DESKTOP_SIZE);

        // reply type: initiator client/other
        if(desktopMode == DesktopResizeMode::ClientRequest)
        {
            status = 1;

            if(1 != screensInfo.size())
            {
                // invalid screen layout
                error = 3;
            }
            else
            {
                auto & info = screensInfo.front();
                screenId = info.id;
                screenFlags = info.flags;
                error = 0;

                if(info.width != width || info.height != height)
                {
                    // need resize
                    if(! xcbAllow)
                    {
                        // resize is administratively prohibited
                        error = 1;
                    }
                    else if(xcbDisplay()->setRandrScreenSize(info.width, info.height))
                    {
                        auto nsize = xcbDisplay()->size();
                        width = nsize.width;
                        height = nsize.height;
                        error = 0;
                    }
                    else
                        error = 3;
                }
            }
        }
        else
        // request: initiator server
        if(desktopMode == DesktopResizeMode::ServerInform)
        {
            status = 0;
            screensInfo.clear();
        }
        else
            Application::error("%s: unknown action for DesktopResizeMode::%s", __FUNCTION__, RFB::desktopResizeModeString(desktopMode));

        // send
        const std::lock_guard<std::mutex> lock(networkBusy);
        sendInt8(RFB::SERVER_FB_UPDATE);
        // padding
        sendInt8(0);
        // number of rects
        sendIntBE16(1);

        if(extended)
        {
            Application::notice("%s: ext desktop size [%dx%d], status: %d, error: %d", __FUNCTION__, width, height, status, error);
            sendIntBE16(status);
            sendIntBE16(error);
            sendIntBE16(width);
            sendIntBE16(height);
            sendIntBE32(RFB::ENCODING_EXT_DESKTOP_SIZE);
            // number of screens
            sendInt8(1);
            // padding
            sendZero(3);
            // id
            sendIntBE32(screenId);
            // xpos
            sendIntBE16(0);
            // ypos
            sendIntBE16(0);
            // width
            sendIntBE16(width);
            // height
            sendIntBE16(height);
            // flags
            sendIntBE32(screenFlags);
        }
        else
        {
            Application::notice("%s: desktop size [%dx%d], status: %d", __FUNCTION__, width, height, status);
            sendIntBE16(0);
            sendIntBE16(0);
            sendIntBE16(width);
            sendIntBE16(height);
            sendIntBE32(RFB::ENCODING_DESKTOP_SIZE);
        }

        sendFlush();

        if(0 == error)
        {
            // fix damage
            auto newreg = xcbDisplay()->region();
            xcbDisplay()->damageAdd(newreg);
            Application::debug("%s: added damage [%d,%d]", __FUNCTION__, newreg.width, newreg.height);
        }

        desktopMode = DesktopResizeMode::Success;
        return true;
    }
}
