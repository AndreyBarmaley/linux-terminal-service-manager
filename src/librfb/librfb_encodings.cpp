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

#include <cmath>
#include <numeric>

#include "ltsm_tools.h"
#include "librfb_server.h"
#include "ltsm_application.h"
#include "librfb_encodings.h"

using namespace std::chrono_literals;

namespace LTSM
{
    RFB::EncodingBase::EncodingBase(int v) : type(v)
    {
        Application::info("%s: init encoding: %s", __FUNCTION__, encodingName(type));
    }

    bool RFB::EncodingBase::jobsEmpty(void) const
    {
        return jobs.empty();
    }

    int RFB::EncodingBase::getType(void) const
    {
        return type;
    }

    void RFB::EncodingBase::setDebug(int v)
    {
        debug = v;
    }

    void RFB::EncodingBase::setThreads(int v)
    {
        threads = v;
    }

    void RFB::EncodingBase::sendHeader(ServerEncoder* srv, const XCB::Region & reg)
    {
        // region size
        srv->sendIntBE16(reg.x);
        srv->sendIntBE16(reg.y);
        srv->sendIntBE16(reg.width);
        srv->sendIntBE16(reg.height);
        // region type
        srv->sendIntBE32(getType());
    }

    void RFB::EncodingBase::sendRawRegion(ServerEncoder* srv, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        std::scoped_lock guard{ busy };

        if(debug)
            Application::debug("%s: job id: %d, [%d, %d, %d, %d]", __FUNCTION__, jobId, reg.x, reg.y, reg.width, reg.height);

        sendHeader(srv, reg + top);
        sendRawRegionPixels(srv, reg, fb);
    }

    void RFB::EncodingBase::sendRawRegionPixels(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb)
    {
        if(srv->serverFormat() != srv->clientFormat())
        {
            for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
                srv->sendPixel(fb.pixel(reg.topLeft() + coord));
        }
        else
        {
            for(int yy = 0; yy < reg.height; ++yy)
            {
                size_t line = reg.width * fb.bytePerPixel();
                srv->sendRaw(fb.pitchData(reg.y + yy) + reg.x * fb.bytePerPixel(), line);
            }
        }
    }

    std::list<XCB::RegionPixel> RFB::EncodingBase::rreProcessing(const XCB::Region & badreg, const FrameBuffer & fb, int skipPixel)
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

    // EncodingRaw
    void RFB::EncodingRaw::sendFrameBuffer(ServerEncoder & srv, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(debug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        // regions counts
        srv.sendIntBE16(1);
        sendRawRegion(& srv, XCB::Point(0, 0), reg0, fb, 1);
    }

    // EncodingRRE
    void RFB::EncodingRRE::sendFrameBuffer(ServerEncoder & srv, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(debug)
            Application::debug("%s: type: %s, region: [%d, %d, %d, %d]", __FUNCTION__, (isCoRRE() ? "CoRRE" : "RRE"), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz = isCoRRE() ? XCB::Size(64, 64) : XCB::Size(128, 128);
        auto regions = reg0.divideBlocks(bsz);

        // regions counts
        srv.sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.push_back(std::async(std::launch::async, & EncodingRRE::sendRegion, this, & srv, top, regions.front() - top, fb, jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : jobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(250us) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & EncodingRRE::sendRegion, this, & srv, top, regions.front() - top, fb, jobId);
                    regions.pop_front();
                    jobId++;
                }
            }
        }
            
        // wait jobs
        for(auto & job : jobs)
            job.wait();

        jobs.clear();
    }

    void RFB::EncodingRRE::sendRegion(ServerEncoder* srv, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapWeight(reg);
        
        if(map.empty())
        {
            Application::error("%s: %s", __FUNCTION__, "pixels map is empty");
            throw rfb_error(NS_FuncName);
        }
        
        if(map.size() > 1)
        {
            int back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = rreProcessing(reg, fb, back);
            const size_t rawLength = reg.width * reg.height * fb.bytePerPixel();
            const size_t rreLength = 4 + fb.bytePerPixel() + goods.size() * (fb.bytePerPixel() + (isCoRRE() ? 4 : 8));
            
            // compare with raw
            if(rawLength < rreLength)
                sendRawRegion(srv, top, reg, fb, jobId);
            else
            {
                std::scoped_lock guard { busy  };
         
                if(debug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, sub rects: %d",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size());
         
                sendHeader(srv, reg + top);
                sendRects(srv, reg, fb, jobId, back, goods);
            }
        }
        // if(map.size() == 1)
        else
        {
            int back = fb.pixel(reg.topLeft());
            std::scoped_lock guard{ busy };
            
            if(debug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");
            
            sendHeader(srv, reg + top);
            // num sub rects
            srv->sendIntBE32(1);
            // back pixel
            srv->sendPixel(back);
            /* one fake sub region : RRE requires */
            // subrect pixel
            srv->sendPixel(back);
            
            // subrect region (relative coords)
            if(isCoRRE())
            {
                srv->sendInt8(0);
                srv->sendInt8(0);
                srv->sendInt8(1);
                srv->sendInt8(1);
            }
            else
            {
                srv->sendIntBE16(0);
                srv->sendIntBE16(0);
                srv->sendIntBE16(1);
                srv->sendIntBE16(1);
            }
        }
    }

    void RFB::EncodingRRE::sendRects(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {   
        // num sub rects
        srv->sendIntBE32(rreList.size());
        // back pixel
        srv->sendPixel(back);

        for(auto & pair : rreList)
        {
            // subrect pixel
            srv->sendPixel(pair.pixel());
            auto & region = pair.region();

            // subrect region (relative coords)
            if(isCoRRE())
            {
                srv->sendInt8(region.x - reg.x);
                srv->sendInt8(region.y - reg.y);
                srv->sendInt8(region.width);
                srv->sendInt8(region.height);
            }
            else
            {
                srv->sendIntBE16(region.x - reg.x);
                srv->sendIntBE16(region.y - reg.y);
                srv->sendIntBE16(region.width);
                srv->sendIntBE16(region.height);
            }

            if(1 < debug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel 0x%08x",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    // EncodingHexTile
    void RFB::EncodingHexTile::sendFrameBuffer(ServerEncoder & srv, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(debug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);
 
        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz(16, 16);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        srv.sendIntBE16(regions.size());
        int jobId = 1;
                
        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.push_back(std::async(std::launch::async, & EncodingHexTile::sendRegion, this, & srv, top, regions.front() - top, fb, jobId));
            regions.pop_front();
            jobId++;
        }
 
        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : jobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(250us) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & EncodingHexTile::sendRegion, this, & srv, top, regions.front() - top, fb, jobId);
                    regions.pop_front();
                    jobId++;
                }
            }
        }
            
        // wait jobs
        for(auto & job : jobs)
            job.wait();

        jobs.clear();
    }

    void RFB::EncodingHexTile::sendRegion(ServerEncoder* srv, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapWeight(reg);
        
        if(map.empty())
        {
            Application::error("%s: %s", __FUNCTION__, "pixels map is empty");
            throw rfb_error(NS_FuncName);
        }
            
        if(map.size() == 1)
        {   
            // wait thread
            std::scoped_lock guard { busy };
            sendHeader(srv, reg + top);
            int back = fb.pixel(reg.topLeft());

            if(debug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // hextile flags
            srv->sendInt8(RFB::HEXTILE_BACKGROUND);
            srv->sendPixel(back);
        }
        else
        if(map.size() > 1)
        {
            // no wait, worked
            int back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = rreProcessing(reg, fb, back);
            // all other color
            bool foreground = std::all_of(goods.begin(), goods.end(),
                                          [col = goods.front().second](auto & pair)
            {
                return pair.pixel() == col;
            });
            const size_t hextileRawLength = 1 + reg.width * reg.height * fb.bytePerPixel();
            // wait thread
            std::scoped_lock guard{ busy };
            sendHeader(srv, reg + top);
            
            if(foreground)
            {
                const size_t hextileForegroundLength = 2 + 2 * fb.bytePerPixel() + goods.size() * 2;
            
                // compare with raw
                if(hextileRawLength < hextileForegroundLength)
                {
                    if(debug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");

                    sendRegionRaw(srv, reg, fb, jobId);
                }
                else
                {
                    if(debug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, sub rects: %d, %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "foreground");

                    sendRegionForeground(srv, reg, fb, jobId, back, goods);
                }
            }
            else
            {
                const size_t hextileColoredLength = 2 + fb.bytePerPixel() + goods.size() * (2 + fb.bytePerPixel());

                // compare with raw
                if(hextileRawLength < hextileColoredLength)
                {
                    if(debug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");

                    sendRegionRaw(srv, reg, fb, jobId);
                }
                else
                {
                    if(debug)
                        Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, sub rects: %d, %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "colored");
                        
                    sendRegionColored(srv, reg, fb, jobId, back, goods);
                }
            }
        }
    }

    void RFB::EncodingHexTile::sendRegionColored(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {
        // hextile flags
        srv->sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_COLOURED | RFB::HEXTILE_SUBRECTS);
        // hextile background
        srv->sendPixel(back);
        // hextile subrects
        srv->sendInt8(rreList.size());

        for(auto & pair : rreList)
        {
            auto & region = pair.region();
            srv->sendPixel(pair.pixel());
            srv->sendInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            srv->sendInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            if(1 < debug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    void RFB::EncodingHexTile::sendRegionForeground(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {
        // hextile flags
        srv->sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_FOREGROUND | RFB::HEXTILE_SUBRECTS);
        // hextile background
        srv->sendPixel(back);
        // hextile foreground
        srv->sendPixel(rreList.front().second);
        // hextile subrects
        srv->sendInt8(rreList.size());

        for(auto & pair : rreList)
        {
            auto & region = pair.region();
            srv->sendInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            srv->sendInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            if(1 < debug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d]",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height);
        }
    }

    void RFB::EncodingHexTile::sendRegionRaw(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        if(isZlibHex())
        {
            // hextile flags
            srv->sendInt8(RFB::HEXTILE_ZLIBRAW);
            srv->zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());
            sendRawRegionPixels(srv, reg, fb);
            srv->zlibDeflateStop(true);
        }
        else
        {
            // hextile flags
            srv->sendInt8(RFB::HEXTILE_RAW);
            sendRawRegionPixels(srv, reg, fb);
        }
    }

    // EncodingTRLE
    void RFB::EncodingTRLE::sendFrameBuffer(ServerEncoder & srv, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(debug)
            Application::debug("%s: type: %s, region: [%d, %d, %d, %d]", __FUNCTION__, (isZRLE() ? "ZRLE" : "TRLE"), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Size bsz(64, 64);
        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        srv.sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.push_back(std::async(std::launch::async, & EncodingTRLE::sendRegion, this, & srv, top, regions.front() - top, fb, jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            for(auto & job : jobs)
            {
                if(regions.empty())
                    break;

                if(job.wait_for(250us) == std::future_status::ready)
                {
                    job = std::async(std::launch::async, & EncodingTRLE::sendRegion, this, & srv, top, regions.front() - top, fb, jobId);
                    regions.pop_front();
                    jobId++;
                }
            }
        }

        // wait jobs
        for(auto & job : jobs)
            job.wait();

        jobs.clear();
    }

    void RFB::EncodingTRLE::sendRegion(ServerEncoder* srv, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapWeight(reg);
        // convert to palette
        int index = 0;

        for(auto & pair : map)
            pair.second = index++;

        // wait thread
        std::scoped_lock guard{ busy };
        sendHeader(srv, reg + top);

        if(isZRLE())
            srv->zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());

        if(map.size() == 1)
        {
            int back = fb.pixel(reg.topLeft());

            if(debug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], back pixel: 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // subencoding type: solid tile
            srv->sendInt8(1);
            srv->sendCPixel(back);
        }
        else
        if(2 <= map.size() && map.size() <= 16)
        {
            size_t fieldWidth = 1;
        
            if(4 < map.size())
                fieldWidth = 4;
            else
            if(2 < map.size())
                fieldWidth = 2;

            if(debug)
                Application::debug("%s: job id: %d, [%d, %d, %d, %d], palsz: %d, packed: %d",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), fieldWidth);

            sendRegionPacked(srv, reg, fb, jobId, fieldWidth, map);
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
                if(debug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], length: %d, rle plain",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, rleList.size());

                sendRegionPlain(srv, reg, fb, rleList);
            }
            else if(rlePaletteLength < rlePlainLength && rlePaletteLength < rawLength)
            {
                if(debug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], pal size: %d, length: %d, rle palette",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), rleList.size());

                sendRegionPalette(srv, reg, fb, map, rleList);
            }
            else
            {
                if(debug)
                    Application::debug("%s: job id: %d, [%d, %d, %d, %d], raw",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

                sendRegionRaw(srv, reg, fb);
            }
        }

        if(isZRLE())
            srv->zlibDeflateStop();
    }

    void RFB::EncodingTRLE::sendRegionPacked(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb, int jobId, size_t field, const PixelMapWeight & pal)
    {       
        // subencoding type: packed palette
        srv->sendInt8(pal.size());
        
        // send palette
        for(auto & pair : pal)
            srv->sendCPixel(pair.first);

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

        srv->sendData(sb.toVector());

        if(1 < debug)
        {
            std::string str = Tools::buffer2hexstring<uint8_t>(sb.toVector().data(), sb.toVector().size(), 2);
            Application::debug("%s: job id: %d, packed stream: %s", __FUNCTION__, jobId, str.c_str());
        }
    }

    void RFB::EncodingTRLE::sendRegionPlain(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb, const std::list<PixelLength> & rle)
    {
        // subencoding type: rle plain
        srv->sendInt8(128);

        // send rle content
        for(auto & pair : rle)
        {
            srv->sendCPixel(pair.pixel());
            srv->sendRunLength(pair.length());
        }
    }

    void RFB::EncodingTRLE::sendRegionPalette(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb, const PixelMapWeight & pal, const std::list<PixelLength> & rle)
    {
        // subencoding type: rle palette
        srv->sendInt8(pal.size() + 128);

        // send palette
        for(auto & pair : pal)
            srv->sendCPixel(pair.first);
        
        // send rle indexes
        for(auto & pair : rle)
        {
            auto it = pal.find(pair.pixel());
            auto index = it != pal.end() ? (*it).second : 0;
            
            if(1 == pair.length())
                srv->sendInt8(index);
            else
            {
                srv->sendInt8(index + 128);
                srv->sendRunLength(pair.length());
            }
        }
    }

    void RFB::EncodingTRLE::sendRegionRaw(ServerEncoder* srv, const XCB::Region & reg, const FrameBuffer & fb)
    {
        // subencoding type: raw
        srv->sendInt8(0);

        // send pixels
        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
            srv->sendCPixel(fb.pixel(reg.topLeft() + coord));
    }

    // EncodingZlib
    void RFB::EncodingZlib::sendFrameBuffer(ServerEncoder & srv, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();
            
        if(debug)
            Application::debug("%s: region: [%d, %d, %d, %d]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        // zlib specific: single thread only
        srv.sendIntBE16(1);
        sendRegion(& srv, XCB::Point(0, 0), reg0, fb, 1);
    }

    void RFB::EncodingZlib::sendRegion(ServerEncoder* srv, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        std::scoped_lock guard{ busy };

        if(debug)
            Application::debug("%s: job id: %d, [%d, %d, %d, %d]", __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

        // region size
        sendHeader(srv, reg + top);
        // region type
        srv->zlibDeflateStart(reg.width * reg.height * fb.bytePerPixel());
        sendRawRegionPixels(srv, reg, fb);
        srv->zlibDeflateStop();
    }
}
