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
#include <chrono>
#include <numeric>

#include "ltsm_tools.h"
#include "librfb_server.h"
#include "ltsm_application.h"
#include "librfb_encodings.h"

using namespace std::chrono_literals;

namespace LTSM
{
    // EncoderStream
    int RFB::EncoderStream::sendHeader(int type, const XCB::Region & reg)
    {
        // region size
        sendIntBE16(reg.x);
        sendIntBE16(reg.y);
        sendIntBE16(reg.width);
        sendIntBE16(reg.height);
        // region type
        sendIntBE32(type);
        return 12;
    }

    int RFB::EncoderStream::sendPixel(uint32_t pixel)
    {
        switch(clientFormat().bytePerPixel())
        {
            case 4:
                if(clientIsBigEndian())
                    sendIntBE32(clientFormat().convertFrom(serverFormat(), pixel));
                else
                    sendIntLE32(clientFormat().convertFrom(serverFormat(), pixel));

                return 4;

            case 2:
                if(clientIsBigEndian())
                    sendIntBE16(clientFormat().convertFrom(serverFormat(), pixel));
                else
                    sendIntLE16(clientFormat().convertFrom(serverFormat(), pixel));

                return 2;

            case 1:
                sendInt8(clientFormat().convertFrom(serverFormat(), pixel));
                return 1;

            default:
                Application::error("%s: %s", __FUNCTION__, "unknown pixel format");
                break;
        }

        throw rfb_error(NS_FuncName);
    }

    int RFB::EncoderStream::sendCPixel(uint32_t pixel)
    {
        if(clientFormat().bitsPerPixel == 32)
        {
            auto pixel2 = clientFormat().convertFrom(serverFormat(), pixel);
            auto red = clientFormat().red(pixel2);
            auto green = clientFormat().green(pixel2);
            auto blue = clientFormat().blue(pixel2);
            std::swap(red, blue);
            sendInt8(red);
            sendInt8(green);
            sendInt8(blue);
            return 3;
        }
        
        return sendPixel(pixel);
    }

    int RFB::EncoderStream::sendRunLength(size_t length)
    {
        if(0 == length)
        {
            Application::error("%s: %s", __FUNCTION__, "length is zero");
            throw rfb_error(NS_FuncName);
        }

        int res = 0;

        while(255 < length)
        {
            sendInt8(255);
            res += 1;
            length -= 255;
        }

        sendInt8((length - 1) % 255);
        return res + 1;
    }

    int RFB::EncoderStream::sendZlibData(ZLib::DeflateStream* zlib, bool uint16sz)
    {
        auto zip = zlib->deflateFlush();

        if(uint16sz)
        {
            if(0xFFFF < zip.size())
            {
                Application::error("%s: %s", __FUNCTION__, "size is large");
                throw rfb_error(NS_FuncName);
            }
            sendIntBE16(zip.size());
        }
        else
        {
            sendIntBE32(zip.size());
        }
        
        sendRaw(zip.data(), zip.size());
        return zip.size() + (uint16sz ? 2 : 4);
    }

    // EncoderWrapper
    void RFB::EncoderWrapper::sendRaw(const void* ptr, size_t len)
    {
        if(ptr && len)
            buffer->append(static_cast<const uint8_t*>(ptr), len);
    }
    
    bool RFB::EncoderWrapper::hasInput(void) const
    {
        LTSM::Application::error("%s: disabled", __FUNCTION__);
        throw network_error(NS_FuncName);
    }

    size_t RFB::EncoderWrapper::hasData(void) const
    {
        LTSM::Application::error("%s: disabled", __FUNCTION__);
        throw network_error(NS_FuncName);
    }

    void RFB::EncoderWrapper::recvRaw(void* ptr, size_t len) const
    {
        LTSM::Application::error("%s: disabled", __FUNCTION__);
        throw network_error(NS_FuncName);
    }

    uint8_t RFB::EncoderWrapper::peekInt8(void) const
    {
        LTSM::Application::error("%s: disabled", __FUNCTION__);
        throw network_error(NS_FuncName);
    }

    // EncodingBase
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

    void RFB::EncodingBase::sendRawRegionPixels(EncoderStream* ns, EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb)
    {
        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
            ns->sendPixel(fb.pixel(reg.topLeft() + coord));
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
    void RFB::EncodingRaw::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        std::scoped_lock guard{ busy };

        if(debug)
            Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, jobId, reg.x, reg.y, reg.width, reg.height);

        st->sendHeader(getType(), reg + top);
        sendRawRegionPixels(st, st, reg, fb);
        st->sendFlush();
    }

    void RFB::EncodingRaw::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(debug)
            Application::debug("%s: region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);

        // regions counts
        st->sendIntBE16(1);
        int jobId = 1;

        // single thread: stream spec
        jobs.emplace_back(std::async(std::launch::async, & EncodingRaw::sendRegion, this, st, top, reg0 - top, fb, jobId));

        // wait jobs
        for(auto & job : jobs)
            job.wait();

        jobs.clear();
    }

    // EncodingRRE
    void RFB::EncodingRRE::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(debug)
            Application::debug("%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, (isCoRRE() ? "CoRRE" : "RRE"), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz = isCoRRE() ? XCB::Size(64, 64) : XCB::Size(128, 128);
        auto regions = reg0.divideBlocks(bsz);

        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingRRE::sendRegion, this, st, top, regions.front() - top, fb, jobId));
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
                    job = std::async(std::launch::async, & EncodingRRE::sendRegion, this, st, top, regions.front() - top, fb, jobId);
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

    void RFB::EncodingRRE::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        // thread buffer
	BinaryBuf bb;
	bb.reserve(4096);

	EncoderWrapper wrap(& bb, st);
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
            //const size_t rawLength = reg.width * reg.height * fb.bytePerPixel();
            //const size_t rreLength = 4 + fb.bytePerPixel() + goods.size() * (fb.bytePerPixel() + (isCoRRE() ? 4 : 8));
            
            if(debug)
                Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel 0x%08x, sub rects: %u",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size());
         
            sendRects(& wrap, reg, fb, jobId, back, goods);
        }
        // if(map.size() == 1)
        else
        {
            int back = fb.pixel(reg.topLeft());
            
            if(debug)
                Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");
            
            // num sub rects
            wrap.sendIntBE32(1);
            // back pixel
            wrap.sendPixel(back);
            /* one fake sub region : RRE requires */
            // subrect pixel
            wrap.sendPixel(back);

            // subrect region (relative coords)
            if(isCoRRE())
            {
                wrap.sendInt8(0);
                wrap.sendInt8(0);
                wrap.sendInt8(1);
                wrap.sendInt8(1);
            }
            else
            {
                wrap.sendIntBE16(0);
                wrap.sendIntBE16(0);
                wrap.sendIntBE16(1);
                wrap.sendIntBE16(1);
            }
        }

        std::scoped_lock guard { busy };

        st->sendHeader(getType(), reg + top);
        st->sendData(bb);
        st->sendFlush();
    }

    void RFB::EncodingRRE::sendRects(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {   
        // num sub rects
        st->sendIntBE32(rreList.size());
        // back pixel
        st->sendPixel(back);

        for(auto & pair : rreList)
        {
            // subrect pixel
            st->sendPixel(pair.pixel());
            auto & region = pair.region();

            // subrect region (relative coords)
            if(isCoRRE())
            {
                st->sendInt8(region.x - reg.x);
                st->sendInt8(region.y - reg.y);
                st->sendInt8(region.width);
                st->sendInt8(region.height);
            }
            else
            {
                st->sendIntBE16(region.x - reg.x);
                st->sendIntBE16(region.y - reg.y);
                st->sendIntBE16(region.width);
                st->sendIntBE16(region.height);
            }

            if(1 < debug)
                Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel 0x%08x",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    // EncodingHexTile
    void RFB::EncodingHexTile::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(debug)
            Application::debug("%s: region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);
 
        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz(16, 16);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;
                
        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingHexTile::sendRegion, this, st, top, regions.front() - top, fb, jobId));
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
                    job = std::async(std::launch::async, & EncodingHexTile::sendRegion, this, st, top, regions.front() - top, fb, jobId);
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

    void RFB::EncodingHexTile::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        // thread buffer
        BinaryBuf bb;
	bb.reserve(4096);

	EncoderWrapper wrap(& bb, st);
        auto map = fb.pixelMapWeight(reg);
        
        if(map.empty())
        {
            Application::error("%s: %s", __FUNCTION__, "pixels map is empty");
            throw rfb_error(NS_FuncName);
        }
            
        if(map.size() == 1)
        {   
            int back = fb.pixel(reg.topLeft());

            if(debug)
            {
                Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel: 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");
            }

            // hextile flags
            wrap.sendInt8(RFB::HEXTILE_BACKGROUND);
            wrap.sendPixel(back);
        }
        else
        if(map.size() > 1)
        {
            // no wait, worked
            int back = map.maxWeightPixel();
            std::list<XCB::RegionPixel> goods = rreProcessing(reg, fb, back);
            // all other color
            bool foreground = std::all_of(goods.begin(), goods.end(),
                                          [col = goods.front().second](auto & pair) { return pair.pixel() == col; });
            const size_t hextileRawLength = 1 + reg.width * reg.height * fb.bytePerPixel();
            
            if(foreground)
            {
                const size_t hextileForegroundLength = 2 + 2 * fb.bytePerPixel() + goods.size() * 2;

                // compare with raw
                if(hextileRawLength < hextileForegroundLength)
                {
                    if(debug)
                    {
                        Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], %s",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");
                    }

                    sendRegionRaw(& wrap, reg, fb, jobId);
                }
                else
                {
                    if(debug)
                    {
                        Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel: 0x%08x, sub rects: %u, %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "foreground");
                    }

                    sendRegionForeground(& wrap, reg, fb, jobId, back, goods);
                }
            }
            else
            {
                const size_t hextileColoredLength = 2 + fb.bytePerPixel() + goods.size() * (2 + fb.bytePerPixel());

                // compare with raw
                if(hextileRawLength < hextileColoredLength)
                {
                    if(debug)
                    {
                        Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], %s",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");
                    }

                    sendRegionRaw(& wrap, reg, fb, jobId);
                }
                else
                {
                    if(debug)
                    {
                        Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel: 0x%08x, sub rects: %u, %s",
                                    __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "colored");
                    }

                    sendRegionColored(& wrap, reg, fb, jobId, back, goods);
                }
            }
        }

        // network send: wait thread
        std::scoped_lock guard { busy };

        st->sendHeader(getType(), reg + top);
        st->sendData(bb);
        st->sendFlush();
    }

    void RFB::EncodingHexTile::sendRegionColored(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {
        // hextile flags
        st->sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_COLOURED | RFB::HEXTILE_SUBRECTS);
        // hextile background
        st->sendPixel(back);
        // hextile subrects
        st->sendInt8(rreList.size());

        for(auto & pair : rreList)
        {
            auto & region = pair.region();
            st->sendPixel(pair.pixel());
            st->sendInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            st->sendInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            if(1 < debug)
                Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel: 0x%08x",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    void RFB::EncodingHexTile::sendRegionForeground(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
    {
        // hextile flags
        st->sendInt8(RFB::HEXTILE_BACKGROUND | RFB::HEXTILE_FOREGROUND | RFB::HEXTILE_SUBRECTS);
        // hextile background
        st->sendPixel(back);
        // hextile foreground
        st->sendPixel(rreList.front().second);
        // hextile subrects
        st->sendInt8(rreList.size());

        for(auto & pair : rreList)
        {
            auto & region = pair.region();
            st->sendInt8(0xFF & ((region.x - reg.x) << 4 | (region.y - reg.y)));
            st->sendInt8(0xFF & ((region.width - 1) << 4 | (region.height - 1)));

            if(1 < debug)
                Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]",
                            __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height);
        }
    }

    void RFB::EncodingHexTile::sendRegionRaw(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        // hextile flags
        st->sendInt8(RFB::HEXTILE_RAW);
        sendRawRegionPixels(st, st, reg, fb);
    }

    // EncodingTRLE
    RFB::EncodingTRLE::EncodingTRLE(bool zlibVer) : EncodingBase(zlibVer ? ENCODING_ZRLE : ENCODING_TRLE)
    {
        if(zlibVer)
            zlib = std::make_unique<ZLib::DeflateStream>(Z_BEST_SPEED);
    }

    void RFB::EncodingTRLE::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        if(debug)
            Application::debug("%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, (isZRLE() ? "ZRLE" : "TRLE"), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Size bsz(64, 64);
        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingTRLE::sendRegion, this, st, top, regions.front() - top, fb, jobId));
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
                    job = std::async(std::launch::async, & EncodingTRLE::sendRegion, this, st, top, regions.front() - top, fb, jobId);
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

    void RFB::EncodingTRLE::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapWeight(reg);
        // convert to palette
        int index = 0;

        // thread buffer
	BinaryBuf bb;
	bb.reserve(reg.width * reg.height * fb.bytePerPixel());

	EncoderWrapper wrap(& bb, st);

        for(auto & pair : map)
            pair.second = index++;

        if(map.size() == 1)
        {
            int back = fb.pixel(reg.topLeft());

            if(debug)
                Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel: 0x%08x, %s",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // subencoding type: solid tile
            wrap.sendInt8(1);
            wrap.sendCPixel(back);
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
                Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], palsz: %u, packed: %u",
                            __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), fieldWidth);

            sendRegionPacked(& wrap, reg, fb, jobId, fieldWidth, map);
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
            const size_t rawLength = 1 + 3 * reg.width * reg.height;

            if(rlePlainLength < rlePaletteLength && rlePlainLength < rawLength)
            {
                if(debug)
                    Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], length: %u, rle plain",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, rleList.size());

                sendRegionPlain(& wrap, reg, fb, rleList);
            }
            else
            if(rlePaletteLength < rlePlainLength && rlePaletteLength < rawLength)
            {
                if(debug)
                    Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], pal size: %u, length: %u, rle palette",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), rleList.size());

                sendRegionPalette(& wrap, reg, fb, map, rleList);
            }
            else
            {
                if(debug)
                    Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], raw",
                                __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

                sendRegionRaw(& wrap, reg, fb);
            }
        }

        // network send: wait thread
        std::scoped_lock guard{ busy };
        st->sendHeader(getType(), reg + top);

        if(zlib)
        {
            zlib->sendData(bb);
            st->sendZlibData(zlib.get());
        }
        else
        {
            st->sendData(bb);
        }

        st->sendFlush();
    }

    void RFB::EncodingTRLE::sendRegionPacked(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, int jobId, size_t field, const PixelMapWeight & pal)
    {       
        // subencoding type: packed palette
        st->sendInt8(pal.size());
        
        // send palette
        for(auto & pair : pal)
            st->sendCPixel(pair.first);

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

        st->sendData(sb.toVector());

        if(1 < debug)
        {
            std::string str = Tools::buffer2hexstring<uint8_t>(sb.toVector().data(), sb.toVector().size(), 2);
            Application::debug("%s: job id: %d, packed stream: %s", __FUNCTION__, jobId, str.c_str());
        }
    }

    void RFB::EncodingTRLE::sendRegionPlain(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, const std::list<PixelLength> & rle)
    {
        // subencoding type: rle plain
        st->sendInt8(128);

        // send rle content
        for(auto & pair : rle)
        {
            st->sendCPixel(pair.pixel());
            st->sendRunLength(pair.length());
        }
    }

    void RFB::EncodingTRLE::sendRegionPalette(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, const PixelMapWeight & pal, const std::list<PixelLength> & rle)
    {
        // subencoding type: rle palette
        st->sendInt8(pal.size() + 128);

        // send palette
        for(auto & pair : pal)
            st->sendCPixel(pair.first);

        // send rle indexes
        for(auto & pair : rle)
        {
            auto it = pal.find(pair.pixel());
            auto index = it != pal.end() ? (*it).second : 0;
            
            if(1 == pair.length())
                st->sendInt8(index);
            else
            {
                st->sendInt8(index + 128);
                st->sendRunLength(pair.length());
            }
        }
    }

    void RFB::EncodingTRLE::sendRegionRaw(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb)
    {
        // subencoding type: raw
        st->sendInt8(0);

        // send pixels
        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
            st->sendCPixel(fb.pixel(reg.topLeft() + coord));
    }

    // EncodingZlib
    RFB::EncodingZlib::EncodingZlib(int zlevel) : EncodingBase(ENCODING_ZLIB)
    {
	if(zlevel < Z_BEST_SPEED || zlevel > Z_BEST_COMPRESSION)
	{
            Application::debug("%s: incorrect value, zlevel: %d", __FUNCTION__, zlevel);
	    zlevel = Z_BEST_SPEED;
	}

        zlib.reset(new ZLib::DeflateStream(zlevel));
        buf.reserve(64 * 1024);
    }

    void RFB::EncodingZlib::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();
            
        if(debug)
            Application::debug("%s: region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);

        // regions counts
        st->sendIntBE16(1);
        int jobId = 1;

        // single thread: zlib stream spec
        jobs.emplace_back(std::async(std::launch::async, & EncodingZlib::sendRegion, this, st, top, reg0 - top, fb, jobId));

        // wait jobs
        for(auto & job : jobs)
            job.wait();

        jobs.clear();
    }

    void RFB::EncodingZlib::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg, const FrameBuffer & fb, int jobId)
    {
        if(debug)
            Application::debug("%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

        EncoderWrapper wrap(& buf, st);
        sendRawRegionPixels(& wrap, st, reg, fb);

        zlib->sendData(buf);
        buf.clear();

        // network send: wait thread
        std::scoped_lock guard{ busy };

        st->sendHeader(getType(), reg + top);
        st->sendZlibData(zlib.get());
        st->sendFlush();
    }
}
