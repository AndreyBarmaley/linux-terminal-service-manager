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
#include <cstring>
#include <utility>

#include "ltsm_tools.h"
#include "librfb_server.h"
#include "ltsm_application.h"
#include "librfb_encodings.h"

#ifdef LTSM_ENCODING
#include "lz4.h"
#include "turbojpeg.h"
#endif

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

    int RFB::EncoderStream::sendPixelRaw(uint32_t pixel, uint8_t bpp, bool be)
    {
        switch(bpp)
        {
            case 4:
                if(be)
                {
                    sendIntBE32(pixel);
                }
                else
                {
                    sendIntLE32(pixel);
                }

                return 4;

            case 2:
                if(be)
                {
                    sendIntBE16(pixel);
                }
                else
                {
                    sendIntLE16(pixel);
                }

                return 2;

            case 1:
                sendInt8(pixel);
                return 1;

            default:
                Application::error("%s: %s", __FUNCTION__, "unknown pixel format");
                break;
        }

        throw rfb_error(NS_FuncName);
    }

    int RFB::EncoderStream::sendPixel(uint32_t pixel)
    {
        return sendPixelRaw(clientFormat().convertFrom(serverFormat(), pixel), clientFormat().bytePerPixel(), clientIsBigEndian());
    }

    int RFB::EncoderStream::sendCPixel(uint32_t pixel)
    {
        if(clientFormat().bitsPerPixel() == 32)
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

    int RFB::EncoderStream::sendRunLength(uint32_t length)
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
        {
            buffer->append(static_cast<const uint8_t*>(ptr), len);
        }
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

    int RFB::EncodingBase::getType(void) const
    {
        return type;
    }

    void RFB::EncodingBase::setThreads(int v)
    {
        threads = v;
    }

    void RFB::EncodingBase::sendRawRegionPixels(EncoderStream* ns, EncoderStream* st, const XCB::Region & reg,
            const FrameBuffer & fb)
    {
#ifdef FB_FAST_CYCLE

        for(uint16_t py = 0; py < reg.height; ++py)
        {
            const uint8_t* pitch = fb.pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px)
            {
                auto ptr = pitch + ((reg.x + px) * fb.bytePerPixel());
                auto pix = FrameBuffer::rawPixel(ptr, fb.bitsPerPixel(), BigEndian);
                ns->sendPixel(pix);
            }
        }

#else

        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
        {
            ns->sendPixel(fb.pixel(reg.topLeft() + coord));
        }

#endif
    }

    std::list<XCB::RegionPixel> RFB::EncodingBase::rreProcessing(const XCB::Region & badreg, const FrameBuffer & fb,
            int skipPixel)
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
                            {
                                goods.back().first.width += subreg.width;
                            }
                            else
                            {
                                goods.emplace_back(subreg, pixel);
                            }
                        }
                    }
                    else
                    {
                        bads2.push_back(subreg);
                    }
                }

                bads1.pop_front();
            }

            if(bads2.empty())
            {
                break;
            }

            bads2.swap(bads1);
            bads2.clear();
        }
        while(! bads1.empty());

        return goods;
    }

    // EncodingRaw
    void RFB::EncodingRaw::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               getTypeName(), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        // regions counts
        st->sendIntBE16(1);
        int jobId = 1;
        // single thread: stream spec
        auto job = sendRegion(st, top, reg0 - top, fb, jobId);
        st->sendHeader(getType(), job.first);
        st->sendData(buf);
        st->sendFlush();
        jobs.clear();
    }

    RFB::EncodingRet RFB::EncodingRaw::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg,
            const FrameBuffer & fb, int jobId)
    {
        Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, jobId, reg.x,
                               reg.y, reg.width, reg.height);

        buf.clear();
        EncoderWrapper wrap(& buf, st);
        sendRawRegionPixels(& wrap, st, reg, fb);
        return std::make_pair(reg + top, BinaryBuf{});
    }

    // EncodingRRE
    void RFB::EncodingRRE::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               getTypeName(), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz = isCoRRE() ? XCB::Size(64, 64) : XCB::Size(128, 128);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingRRE::sendRegion, this, st, top, regions.front() - top, fb,
                                         jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            // busy
            auto busy = std::count_if(jobs.begin(), jobs.end(), [](auto & job)
            {
                return job.wait_for(1us) != std::future_status::ready;
            });

            if(busy < threads)
            {
                jobs.emplace_back(std::async(std::launch::async, & EncodingRRE::sendRegion, this, st, top, regions.front() - top, fb,
                                             jobId));
                regions.pop_front();
                jobId++;
            }

            std::this_thread::sleep_for(100us);
        }

        // wait jobs
        for(auto & job : jobs)
        {
            job.wait();
            auto ret = job.get();
            st->sendHeader(getType(), ret.first);
            st->sendData(ret.second);
        }

        st->sendFlush();
        jobs.clear();
    }

    RFB::EncodingRet RFB::EncodingRRE::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg,
            const FrameBuffer & fb, int jobId)
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

            Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16
                                   "], back pixel 0x%08x, sub rects: %u",
                                   __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size());

            sendRects(& wrap, reg, fb, jobId, back, goods);
        }
        // if(map.size() == 1)
        else
        {
            int back = fb.pixel(reg.topLeft());

            Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel 0x%08x, %s",
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

        return std::make_pair(reg + top, std::move(bb));
    }

    void RFB::EncodingRRE::sendRects(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, int jobId,
                                     int back, const std::list<XCB::RegionPixel> & rreList)
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

            Application::trace(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel 0x%08x",
                                   __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    // EncodingHexTile
    void RFB::EncodingHexTile::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               getTypeName(), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        const XCB::Size bsz(16, 16);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingHexTile::sendRegion, this, st, top, regions.front() - top,
                                         fb, jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            // busy
            auto busy = std::count_if(jobs.begin(), jobs.end(), [](auto & job)
            {
                return job.wait_for(1us) != std::future_status::ready;
            });

            if(busy < threads)
            {
                jobs.emplace_back(std::async(std::launch::async, & EncodingHexTile::sendRegion, this, st, top, regions.front() - top,
                                             fb, jobId));
                regions.pop_front();
                jobId++;
            }

            std::this_thread::sleep_for(100us);
        }

        // wait jobs
        for(auto & job : jobs)
        {
            job.wait();
            auto ret = job.get();
            st->sendHeader(getType(), ret.first);
            st->sendData(ret.second);
        }

        st->sendFlush();
        jobs.clear();
    }

    RFB::EncodingRet RFB::EncodingHexTile::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg,
            const FrameBuffer & fb, int jobId)
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

            Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel: 0x%08x, %s",
                                   __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // hextile flags
            wrap.sendInt8(RFB::HEXTILE_BACKGROUND);
            wrap.sendPixel(back);
        }
        else if(map.size() > 1)
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

            if(foreground)
            {
                const size_t hextileForegroundLength = 2 + 2 * fb.bytePerPixel() + goods.size() * 2;

                // compare with raw
                if(hextileRawLength < hextileForegroundLength)
                {
                    Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], %s",
                                           __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");

                    sendRegionRaw(& wrap, reg, fb, jobId);
                }
                else
                {
                    Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16
                                           "], back pixel: 0x%08x, sub rects: %u, %s",
                                           __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "foreground");

                    sendRegionForeground(& wrap, reg, fb, jobId, back, goods);
                }
            }
            else
            {
                const size_t hextileColoredLength = 2 + fb.bytePerPixel() + goods.size() * (2 + fb.bytePerPixel());

                // compare with raw
                if(hextileRawLength < hextileColoredLength)
                {
                    Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], %s",
                                           __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, "raw");

                    sendRegionRaw(& wrap, reg, fb, jobId);
                }
                else
                {
                    Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16
                                           "], back pixel: 0x%08x, sub rects: %u, %s",
                                           __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, goods.size(), "colored");

                    sendRegionColored(& wrap, reg, fb, jobId, back, goods);
                }
            }
        }

        return std::make_pair(reg + top, std::move(bb));
    }

    void RFB::EncodingHexTile::sendRegionColored(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
            int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
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

            Application::trace(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel: 0x%08x",
                                   __FUNCTION__, jobId, region.x - reg.x, region.y - reg.y, region.width, region.height, pair.pixel());
        }
    }

    void RFB::EncodingHexTile::sendRegionForeground(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
            int jobId, int back, const std::list<XCB::RegionPixel> & rreList)
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

            Application::trace(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]",
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
        {
            zlib = std::make_unique<ZLib::DeflateStream>(Z_BEST_SPEED);
        }
    }

    void RFB::EncodingTRLE::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               getTypeName(), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Size bsz(64, 64);
        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingTRLE::sendRegion, this, st, top, regions.front() - top, fb,
                                         jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            // busy
            auto busy = std::count_if(jobs.begin(), jobs.end(), [](auto & job)
            {
                return job.wait_for(1us) != std::future_status::ready;
            });

            if(busy < threads)
            {
                jobs.emplace_back(std::async(std::launch::async, & EncodingTRLE::sendRegion, this, st, top, regions.front() - top, fb,
                                             jobId));
                regions.pop_front();
                jobId++;
            }

            std::this_thread::sleep_for(100us);
        }

        // wait jobs
        for(auto & job : jobs)
        {
            job.wait();
            auto ret = job.get();
            st->sendHeader(getType(), ret.first);

            if(zlib)
            {
                zlib->sendData(ret.second);
                st->sendZlibData(zlib.get());
            }
            else
            {
                st->sendData(ret.second);
            }
        }

        st->sendFlush();
        jobs.clear();
    }

    RFB::EncodingRet RFB::EncodingTRLE::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg,
            const FrameBuffer & fb, int jobId)
    {
        auto map = fb.pixelMapPalette(reg);
        // thread buffer
        BinaryBuf bb;
        bb.reserve(reg.width* reg.height* fb.bytePerPixel());
        EncoderWrapper wrap(& bb, st);

        if(map.size() == 1)
        {
            int back = fb.pixel(reg.topLeft());

            Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], back pixel: 0x%08x, %s",
                                   __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, back, "solid");

            // subencoding type: solid tile
            wrap.sendInt8(1);
            wrap.sendCPixel(back);
        }
        else if(2 <= map.size() && map.size() <= 16)
        {
            size_t fieldWidth = 1;

            if(4 < map.size())
            {
                fieldWidth = 4;
            }
            else if(2 < map.size())
            {
                fieldWidth = 2;
            }

            Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], palsz: %u, packed: %u",
                                   __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), fieldWidth);

            sendRegionPacked(& wrap, reg, fb, jobId, fieldWidth, map);
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
            const size_t rlePaletteLength = 1 < rleList.size() &&
                                            rleList.size() < 128 ? std::accumulate(rleList.begin(), rleList.end(), 1 + 3 * map.size(),
                                                [](int v, auto & pair)
            {
                return v + 1 + std::floor((pair.second - 1) / 255.0) + 1;
            }) : 0xFFFF;

            // raw length
            const size_t rawLength = 1 + 3 * reg.width * reg.height;

            if(rlePlainLength < rlePaletteLength && rlePlainLength < rawLength)
            {
                Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], length: %u, rle plain",
                                       __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, rleList.size());

                sendRegionPlain(& wrap, reg, fb, rleList);
            }
            else if(rlePaletteLength < rlePlainLength && rlePaletteLength < rawLength)
            {
                Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16
                                       "], pal size: %u, length: %u, rle palette",
                                       __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height, map.size(), rleList.size());

                sendRegionPalette(& wrap, reg, fb, map, rleList);
            }
            else
            {
                Application::debug(DebugType::Enc, "%s: job id: %d, [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "], raw",
                                       __FUNCTION__, jobId, top.x + reg.x, top.y + reg.y, reg.width, reg.height);

                sendRegionRaw(& wrap, reg, fb);
            }
        }

        return std::make_pair(reg + top, std::move(bb));
    }

    void RFB::EncodingTRLE::sendRegionPacked(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb, int jobId,
            size_t field, const PixelMapPalette & pal)
    {
        // subencoding type: packed palette
        st->sendInt8(pal.size());

        // send palette
        for(auto & pair : pal)
        {
            st->sendCPixel(pair.first);
        }

        const size_t rez = (reg.width* reg.height) >> 2;
        Tools::StreamBitsPack sb(rez ? rez : 32);
        // send packed rows
#ifdef FB_FAST_CYCLE

        for(uint16_t py = 0; py < reg.height; ++py)
        {
            const uint8_t* pitch = fb.pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px)
            {
                auto ptr = pitch + ((reg.x + px) * fb.bytePerPixel());
                auto pix = FrameBuffer::rawPixel(ptr, fb.bitsPerPixel(), BigEndian);
                auto index = pal.findColorIndex(pix);
                assertm(0 <= index, "palette color not found");
                sb.pushValue(index, field);
            }

            sb.pushAlign();
        }

#else

        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
        {
            auto pix = fb.pixel(reg.topLeft() + coord);
            auto index = pal.findColorIndex(pix);
            assertm(0 <= index, "palette color not found");
            sb.pushValue(index, field);

            if(coord.isEndLine())
            {
                sb.pushAlign();
            }
        }

#endif
        st->sendData(sb.toVector());

        if(Application::isDebugLevel(DebugLevel::Trace))
        {
            auto & vec = sb.toVector();
            std::string str = Tools::buffer2hexstring(vec.begin(), vec.end(), 2);
            Application::debug(DebugType::Enc, "%s: job id: %d, packed stream: %s", __FUNCTION__, jobId, str.c_str());
        }
    }

    void RFB::EncodingTRLE::sendRegionPlain(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
                                            const PixelLengthList & rle)
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

    void RFB::EncodingTRLE::sendRegionPalette(EncoderStream* st, const XCB::Region & reg, const FrameBuffer & fb,
            const PixelMapPalette & pal, const PixelLengthList & rle)
    {
        // subencoding type: rle palette
        st->sendInt8(pal.size() + 128);

        // send palette
        for(auto & pair : pal)
        {
            st->sendCPixel(pair.first);
        }

        // send rle indexes
        for(auto & pair : rle)
        {
            int index = pal.findColorIndex(pair.pixel());
            assertm(0 <= index, "palette color not found");

            if(1 == pair.length())
            {
                st->sendInt8(index);
            }
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
#ifdef FB_FAST_CYCLE

        for(uint16_t py = 0; py < reg.height; ++py)
        {
            const uint8_t* pitch = fb.pitchData(reg.y + py);

            for(uint16_t px = 0; px < reg.width; ++px)
            {
                auto ptr = pitch + ((reg.x + px) * fb.bytePerPixel());
                auto pix = FrameBuffer::rawPixel(ptr, fb.bitsPerPixel(), BigEndian);
                st->sendCPixel(pix);
            }
        }

#else

        for(auto coord = reg.coordBegin(); coord.isValid(); ++coord)
        {
            st->sendCPixel(fb.pixel(reg.topLeft() + coord));
        }

#endif
    }

    // EncodingZlib
    RFB::EncodingZlib::EncodingZlib(int lev) : EncodingBase(ENCODING_ZLIB), zlevel(lev)
    {
        if(zlevel < Z_BEST_SPEED || zlevel > Z_BEST_COMPRESSION)
            zlevel = Z_BEST_SPEED;

        zlib.reset(new ZLib::DeflateStream(zlevel));
        buf.reserve(64 * 1024);
    }

    void RFB::EncodingZlib::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               getTypeName(), reg0.x, reg0.y, reg0.width, reg0.height);

        const XCB::Point top(reg0.x, reg0.y);
        // regions counts
        st->sendIntBE16(1);
        int jobId = 1;
        // single thread: zlib stream spec
        auto job = sendRegion(st, top, reg0 - top, fb, jobId);
        st->sendHeader(getType(), job.first);
        st->sendZlibData(zlib.get());
        st->sendFlush();
        jobs.clear();
    }

    RFB::EncodingRet RFB::EncodingZlib::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg,
            const FrameBuffer & fb, int jobId)
    {
        buf.clear();
        EncoderWrapper wrap(& buf, st);
        sendRawRegionPixels(& wrap, st, reg, fb);
        zlib->sendData(buf);
        return std::make_pair(reg + top, BinaryBuf{});
    }

    bool RFB::EncodingZlib::setEncodingOptions(const std::forward_list<std::string> & encopts)
    {
        bool fullscreenUpdate = false;

        for(auto & str: encopts)
        {
            // parce zlevel
            if(0 == std::strncmp(str.c_str(), "zlev", 4))
            {
                if(auto it = str.find(':'); it++ != str.npos && it != str.npos)
                {
                    try
                    {
                        zlevel = std::stoi(str.substr(it));

                        if(zlevel < Z_BEST_SPEED || zlevel > Z_BEST_COMPRESSION)
                        {
                            Application::warning("%s: incorrect value, zlevel: %d", __FUNCTION__, zlevel);
                            zlevel = Z_BEST_SPEED;
                        }
                    }
                    catch(...)
                    {
                    }

                    Application::info("%s: set zlevel: %d", __FUNCTION__, zlevel);
                    zlib.reset(new ZLib::DeflateStream(zlevel));
                }
            }
        }

        return fullscreenUpdate;
    }

#ifdef LTSM_ENCODING
    /// EncodingLZ4
    void RFB::EncodingLZ4::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               getTypeName(), reg0.x, reg0.y, reg0.width, reg0.height);

        // calculate block size
        XCB::Size bsz;
        const size_t blocksz = 256 * 256;

        if(st->displaySize() == fb.region().toSize())
        {
            bsz = 1 < threads ?
                XCB::Size(fb.width(), fb.height() / threads) :
                fb.region().toSize();
        }
        else if(fb.width() * fb.height() < blocksz)
        {
            // one rect
            bsz = fb.region().toSize();
        }
        else
        {
            bsz = XCB::Size(fb.width(), blocksz / fb.height());
        }

        assertm(! bsz.isEmpty(), "block size empty");

        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingLZ4::sendRegion, this, st, top, regions.front() - top, fb,
                                         jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            // busy
            auto busy = std::count_if(jobs.begin(), jobs.end(), [](auto & job)
            {
                return job.wait_for(1us) != std::future_status::ready;
            });

            if(busy < threads)
            {
                jobs.emplace_back(std::async(std::launch::async, & EncodingLZ4::sendRegion, this, st, top, regions.front() - top, fb,
                                             jobId));
                regions.pop_front();
                jobId++;
            }

            std::this_thread::sleep_for(100us);
        }

        // wait jobs
        for(auto & job : jobs)
        {
            job.wait();
            auto ret = job.get();
            st->sendHeader(getType(), ret.first);
            // ltsm lz4 format
            st->sendIntBE32(ret.second.size());
            st->sendData(ret.second);
        }

        st->sendFlush();
        jobs.clear();
    }

    RFB::EncodingRet RFB::EncodingLZ4::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg,
            const FrameBuffer & fb, int jobId)
    {
        // thread buffer
        BinaryBuf bb;
        int ret = 0;

        if(fb.width() == reg.width)
        {
            auto inbuf = fb.pitchData(reg.y);
            auto inlen = fb.pitchSize() * reg.height;
            bb.resize(LZ4_compressBound(inlen));

            ret = LZ4_compress_fast((const char*) inbuf, (char*) bb.data(), inlen, bb.size(), 1);
        }
        else
        {
            auto fb2 = fb.copyRegion(reg);
            auto inbuf = fb2.pitchData(0);
            auto inlen = fb2.pitchSize() * reg.height;
            bb.resize(LZ4_compressBound(inlen));

            ret = LZ4_compress_fast((const char*) inbuf, (char*) bb.data(), inlen, bb.size(), 1);
        }

        if(ret < 0)
        {
            Application::error("%s: %s failed, ret: %d", __FUNCTION__, "LZ4_compress_fast_continue", ret);
            throw rfb_error(NS_FuncName);
        }

        bb.resize(ret);
        return std::make_pair(reg + top, std::move(bb));
    }

    /// EncodingTJPG
    bool RFB::EncodingTJPG::setEncodingOptions(const std::forward_list<std::string> & encopts)
    {
        bool fullscreenUpdate = false;

        for(auto & str: encopts)
        {
            // parce quality
            if(0 == std::strncmp(str.c_str(), "qual", 4))
            {
                if(auto it = str.find(':'); it++ != str.npos && it != str.npos)
                {
                    try
                    {
                        jpegQuality = std::stoi(str.substr(it));

                        if(10 > jpegQuality || 100 < jpegQuality)
                        {
                            Application::warning("%s: incorrect value, quality: %d", __FUNCTION__, jpegQuality);
                            jpegQuality = 85;
                        }
                        else
                        {
                            fullscreenUpdate = true;
                        }
                    }
                    catch(...)
                    {
                    }

                    Application::info("%s: set quality: %d", __FUNCTION__, jpegQuality);
                }
            }
            else
            // parce sample
            if(0 == std::strncmp(str.c_str(), "samp", 4))
            {
                if(auto it = str.find(':'); it++ != str.npos && it != str.npos)
                {
                    if(0 == str.compare(it, -1, "420"))
                    {
                        jpegSamp = TJSAMP_420;
                        fullscreenUpdate = true;
                    }
                    else
                    if(0 == str.compare(it, -1, "422"))
                    {
                        jpegSamp = TJSAMP_422;
                        fullscreenUpdate = true;
                    }
                    else
                    if(0 == str.compare(it, -1, "440"))
                    {
                        jpegSamp = TJSAMP_440;
                        fullscreenUpdate = true;
                    }
                    else
                    if(0 == str.compare(it, -1, "444"))
                    {
                        jpegSamp = TJSAMP_444;
                        fullscreenUpdate = true;
                    }
                    else
                    if(0 == str.compare(it, -1, "411"))
                    {
                        jpegSamp = TJSAMP_411;
                        fullscreenUpdate = true;
                    }
                    else
                    if(0 == str.compare(it, -1, "gray"))
                    {
                        jpegSamp = TJSAMP_GRAY;
                        fullscreenUpdate = true;
                    }

                    Application::info("%s: set sample: %s", __FUNCTION__, str.substr(it).c_str());
                }
            }
        }

        return fullscreenUpdate;
    }
    
    void RFB::EncodingTJPG::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               getTypeName(), reg0.x, reg0.y, reg0.width, reg0.height);

        // calculate block size
        XCB::Size bsz;
        const size_t blocksz = 256 * 256;

        if(st->displaySize() == fb.region().toSize())
        {
            bsz = 1 < threads ?
                XCB::Size(fb.width(), fb.height() / threads) :
                fb.region().toSize();
        }
        else if(fb.width() * fb.height() < blocksz)
        {
            // one rect
            bsz = fb.region().toSize();
        }
        else
        {
            bsz = XCB::Size(fb.width(), blocksz / fb.height());
        }

        assertm(! bsz.isEmpty(), "block size empty");

        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingTJPG::sendRegion, this, st, top, regions.front() - top, fb,
                                         jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            // busy
            auto busy = std::count_if(jobs.begin(), jobs.end(), [](auto & job)
            {
                return job.wait_for(1us) != std::future_status::ready;
            });

            if(busy < threads)
            {
                jobs.emplace_back(std::async(std::launch::async, & EncodingTJPG::sendRegion, this, st, top, regions.front() - top, fb,
                                             jobId));
                regions.pop_front();
                jobId++;
            }

            std::this_thread::sleep_for(100us);
        }

        // wait jobs
        for(auto & job : jobs)
        {
            job.wait();
            auto ret = job.get();
            st->sendHeader(getType(), ret.first);
            // pixels
            st->sendIntBE32(ret.second.size());
            st->sendData(ret.second);
        }

        st->sendFlush();
        jobs.clear();
    }

    RFB::EncodingRet RFB::EncodingTJPG::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg,
            const FrameBuffer & fb, int jobId)
    {
        std::unique_ptr<void, int(*)(void*)> jpeg{ tjInitCompress(), tjDestroy };

        if(! jpeg)
        {
            Application::error("%s: %s failed", __FUNCTION__, "tjInitCompress");
            throw rfb_error(NS_FuncName);
        }

#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        const int pixFmt = TJPF_RGBX;
#else
        const int pixFmt = TJPF_BGRX;
#endif

        long unsigned int jpegSize = tjBufSize(reg.width, reg.height, jpegSamp);
        // thread buffer
        auto bb = BinaryBuf(jpegSize);
        unsigned char* jpegBuf = bb.data();
        int ret = 0;

        if(fb.width() == reg.width)
        {
            ret = tjCompress2(jpeg.get(), fb.pitchData(reg.y), reg.width, fb.pitchSize(), reg.height, pixFmt,
                              & jpegBuf, & jpegSize, jpegSamp, jpegQuality, TJFLAG_FASTDCT|TJFLAG_NOREALLOC);
        }
        else
        {
            auto fb2 = fb.copyRegion(reg);
            ret = tjCompress2(jpeg.get(), fb2.pitchData(0), reg.width, fb2.pitchSize(), reg.height, pixFmt,
                              & jpegBuf, & jpegSize, jpegSamp, jpegQuality, TJFLAG_FASTDCT|TJFLAG_NOREALLOC);
        }

        if(0 > ret)
        {
            int err = tjGetErrorCode(jpeg.get());
            const char* str = tjGetErrorStr2(jpeg.get());
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "tjCompress", str, err);
            throw rfb_error(NS_FuncName);
        }

        bb.resize(jpegSize);
        return std::make_pair(reg + top, std::move(bb));
    }

    /// EncodingQOI
    void RFB::EncodingQOI::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        const XCB::Region & reg0 = fb.region();

        Application::debug(DebugType::Enc, "%s: type: %s, region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               getTypeName(), reg0.x, reg0.y, reg0.width, reg0.height);

        // calculate block size
        XCB::Size bsz;
        const size_t blocksz = 256 * 256;

        if(st->displaySize() == fb.region().toSize())
        {
            bsz = 1 < threads ?
                XCB::Size(fb.width(), fb.height() / threads) :
                fb.region().toSize();
        }
        else if(fb.width() * fb.height() < blocksz)
        {
            // one rect
            bsz = fb.region().toSize();
        }
        else
        {
            bsz = XCB::Size(fb.width(), blocksz / fb.height());
        }

        assertm(! bsz.isEmpty(), "block size empty");

        const XCB::Point top(reg0.x, reg0.y);
        auto regions = reg0.divideBlocks(bsz);
        // regions counts
        st->sendIntBE16(regions.size());
        int jobId = 1;

        // make pool jobs
        while(jobId <= threads && ! regions.empty())
        {
            jobs.emplace_back(std::async(std::launch::async, & EncodingQOI::sendRegion, this, st, top, regions.front() - top, fb,
                                         jobId));
            regions.pop_front();
            jobId++;
        }

        // renew completed job
        while(! regions.empty())
        {
            // busy
            auto busy = std::count_if(jobs.begin(), jobs.end(), [](auto & job)
            {
                return job.wait_for(1us) != std::future_status::ready;
            });

            if(busy < threads)
            {
                jobs.emplace_back(std::async(std::launch::async, & EncodingQOI::sendRegion, this, st, top, regions.front() - top, fb,
                                             jobId));
                regions.pop_front();
                jobId++;
            }

            std::this_thread::sleep_for(100us);
        }

        // wait jobs
        for(auto & job : jobs)
        {
            job.wait();
            auto ret = job.get();
            st->sendHeader(getType(), ret.first);
            // encode buf
            st->sendIntBE32(ret.second.size());
            st->sendData(ret.second);
        }

        st->sendFlush();
        jobs.clear();
    }

    RFB::EncodingRet RFB::EncodingQOI::sendRegion(EncoderStream* st, const XCB::Point & top, const XCB::Region & reg,
            const FrameBuffer & fb, int jobId)
    {
        BinaryBuf bb = encodeBGRx(fb, reg, st->clientFormat());
        return std::make_pair(reg + top, std::move(bb));
    }

    namespace QOI
    {
        enum Tag
        {
            INDEX = 0x00,
            DIFF = 0x40,
            LUMA = 0x80,
            RUN = 0xC0,
            RGB = 0xFE,
            RGBA = 0xFF
        };

        inline uint8_t hashIndex64RGB(const Color & col)
        {
            return (col.r * 3 + col.g * 5 + col.b * 7) % 64;
        }
    }

    BinaryBuf RFB::EncodingQOI::encodeBGRx(const FrameBuffer & fb, const XCB::Region & reg, const PixelFormat & clientPf) const
    {
        StreamBuf sb(reg.height * reg.width * 8 / 3);

        std::array<int64_t, 64> hashes;
        hashes.fill(-1);

        int64_t prevPixel = -1;
        std::uint8_t run = 0;

        for(int16_t py = 0; py < reg.height; ++py)
        {
            for(int16_t px = 0; px < reg.width; ++px)
            {
                const bool pixelLast = (py == reg.height - 1) && (px == reg.width - 1);
                const uint32_t pixel = clientPf.pixel(fb.color(reg.topLeft() + XCB::Point{px, py}));

                // QOI::Tag::RUN
                if(pixel == prevPixel)
                {
                    run++;

                    if(run == 62 || pixelLast)
                    {
                        sb.writeInt8(QOI::Tag::RUN | (run - 1));
                        run = 0;
                    }

                    continue;
                }

                if(run)
                {
                    sb.writeInt8(QOI::Tag::RUN | (run - 1));
                    run = 0;
                }

                auto col = clientPf.color(pixel);

                // QOI::Tag::INDEX
                const uint8_t index = QOI::hashIndex64RGB(col);

                if(hashes[index] == pixel)
                {
                    sb.writeInt8(QOI::Tag::INDEX | index);
                    prevPixel = pixel;
                    continue;
                }

                hashes[index] = pixel;

                if(prevPixel < 0)
                {
                    sb.writeInt8(QOI::Tag::RGB);
                    sb.writeInt8(col.r);
                    sb.writeInt8(col.g);
                    sb.writeInt8(col.b);
                    prevPixel = pixel;
                    continue;
                }

                auto pcol = clientPf.color(prevPixel);

                const int8_t vr = col.r - pcol.r;
                const int8_t vg = col.g - pcol.g;
                const int8_t vb = col.b - pcol.b;

                // QOI::Tag::DIFF
                if(vr > static_cast<int8_t>(-3) && vr < static_cast<int8_t>(2) &&
                        vg > static_cast<int8_t>(-3) && vg < static_cast<int8_t>(2) &&
                        vb > static_cast<int8_t>(-3) && vb < static_cast<int8_t>(2))
                {
                    sb.writeInt8(QOI::Tag::DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2));
                    prevPixel = pixel;
                    continue;
                }

                const int8_t vg_r = vr - vg;
                const int8_t vg_b = vb - vg;

                // QOI::Tag::LUMA
                if(vg_r > static_cast<int8_t>(-9) && vg_r < static_cast<int8_t>(8) &&
                        vg > static_cast<int8_t>(-33) && vg < static_cast<int8_t>(32) &&
                        vg_b > static_cast<int8_t>(-9) && vg_b < static_cast<int8_t>(8))
                {
                    sb.writeInt8(QOI::Tag::LUMA | (vg + 32));
                    sb.writeInt8((vg_r + 8) << 4 | (vg_b + 8));
                    prevPixel = pixel;
                    continue;
                }

                // QOI::Tag::RGB
                sb.writeInt8(QOI::Tag::RGB);
                sb.writeInt8(col.r);
                sb.writeInt8(col.g);
                sb.writeInt8(col.b);

                prevPixel = pixel;
            }
        }

        // padding
        const uint8_t qoiPadding[] = {0,0,0,0,0,0,0,1};

        for(auto & pad: qoiPadding)
        {
            sb.writeInt8(pad);
        }

        return sb.rawbuf();
    }

#endif
}
