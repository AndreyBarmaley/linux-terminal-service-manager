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

#include <array>
#include <algorithm>

#include "ltsm_application.h"
#include "librfb_ffmpeg.h"
#include "ffmpeg_tools.h"

namespace LTSM
{
    namespace FFMPEG
    {
        std::array<char, 1024> errbuf;
        std::array<char, 1024> logbuf;

        const char* error(int errnum)
        {
            std::fill(errbuf.begin(), errbuf.end(), 0);
            return 0 > av_strerror(errnum, errbuf.data(), errbuf.size() - 1) ? "error not found" : errbuf.data();
        }

        void logCallback(void* avcl, int lvl, const char* fmt, va_list vl)
        {
            if(av_log_get_level() < lvl)
            {
                return;
            }

            if(int len = vsnprintf(logbuf.data(), logbuf.size(), fmt, vl); 0 < len)
            {
                AVClass* avc = avcl ? *(AVClass**) avcl : nullptr;
                const char* name = avc ? avc->item_name(avcl) : nullptr;
                const char* type = "ffmpeg debug";

                // remove endl
                if(1 < len)
                {
                    len--;
                }

                switch(lvl)
                {
                    case AV_LOG_PANIC:
                    case AV_LOG_FATAL:
                    case AV_LOG_ERROR:
                        Application::error("%s: [%s] %.*s", type, name, len, logbuf.data());
                        break;

                    case AV_LOG_WARNING:
                        Application::warning("%s: [%s] %.*s", type, name, len, logbuf.data());
                        break;

                    case AV_LOG_INFO:
                    case AV_LOG_VERBOSE:
                        Application::notice("%s: [%s] %.*s", type, name, len, logbuf.data());
                        break;

                    case AV_LOG_DEBUG:
                    case AV_LOG_TRACE:
                        Application::info("%s: [%s] %.*s", type, name, len, logbuf.data());
                        break;

                    default:
                        break;
                }
            }
        }
    }

#ifdef LTSM_ENCODING_FFMPEG
    // EncodingFFmpeg
    RFB::EncodingFFmpeg::EncodingFFmpeg(int type) : EncodingBase(type)
    {
        av_log_set_level(AV_LOG_QUIET);
        av_log_set_callback(FFMPEG::logCallback);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
        avcodec_register_all();
#endif
        updatePoint = std::chrono::steady_clock::now();

        switch(type)
        {
            case RFB::ENCODING_FFMPEG_H264:
                codec = avcodec_find_encoder(AV_CODEC_ID_H264);
                break;

            case RFB::ENCODING_FFMPEG_VP8:
                codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
                break;

            case RFB::ENCODING_FFMPEG_AV1:
                codec = avcodec_find_encoder(AV_CODEC_ID_AV1);
                break;

            default:
                break;
        }

        if(! codec)
        {
            Application::error("%s: %s failed, type: %d, encoding: %s", __FUNCTION__, "avcodec_find_encoder", type,
                               encodingName(type));
            throw ffmpeg_error(NS_FuncName);
        }
    }

    void RFB::EncodingFFmpeg::setDebug(int val)
    {
        // encoding:debug
        switch(val)
        {
            case 0:
                av_log_set_level(AV_LOG_QUIET);
                break;

            case 1:
                av_log_set_level(AV_LOG_ERROR);
                break;

            case 2:
                av_log_set_level(AV_LOG_WARNING);
                break;

            case 3:
                av_log_set_level(AV_LOG_INFO);
                break;

            case 4:
                av_log_set_level(AV_LOG_VERBOSE);
                break;

            case 5:
                av_log_set_level(AV_LOG_DEBUG);
                break;

            default:
                av_log_set_level(AV_LOG_TRACE);
                break;
        }

        EncodingBase::setDebug(val);
    }

    void RFB::EncodingFFmpeg::resizedEvent(const XCB::Size & nsz)
    {
        std::scoped_lock guard{ lockUpdate };

        if(avcctx && (avcctx->width != nsz.width || avcctx->height != nsz.height))
        {
            initContext(nsz);
        }
    }

    void RFB::EncodingFFmpeg::initContext(const XCB::Size & csz)
    {
        packet.reset();
        frame.reset();
        swsctx.reset();
        avcctx.reset();
        avcctx.reset(avcodec_alloc_context3(codec));

        if(! avcctx)
        {
            Application::error("%s: %s failed", __FUNCTION__, "avcodec_alloc_context3");
            throw ffmpeg_error(NS_FuncName);
        }

        avcctx->delay = 0;
        //avcctx->bit_rate = xxxx;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 13, 100)
        avcctx->framerate = (AVRational)
        {
            fps, 1
        };

#endif
        avcctx->time_base = (AVRational)
        {
            1, fps
        };

        switch(codec->id)
        {
            case AV_CODEC_ID_H264:
                av_opt_set(avcctx.get(), "preset", "veryfast", AV_OPT_SEARCH_CHILDREN);
                av_opt_set(avcctx.get(), "tune", "zerolatency", AV_OPT_SEARCH_CHILDREN);
                break;

            case AV_CODEC_ID_AV1:
                // In versions prior to 0.9.0, valid presets are 0 to 8.
                // higher numbers providing a higher encoding speed.
                av_opt_set(avcctx.get(), "preset", "7", AV_OPT_SEARCH_CHILDREN);
                break;

            case AV_CODEC_ID_VP8:
                av_opt_set(avcctx.get(), "quality", "realtime", AV_OPT_SEARCH_CHILDREN);
                av_opt_set_int(avcctx.get(), "speed", 6, AV_OPT_SEARCH_CHILDREN);
                break;

            default:
                break;
        }

        avcctx->pix_fmt = AV_PIX_FMT_YUV420P;
        avcctx->width = csz.width;
        avcctx->height = csz.height;
        avcctx->codec_type = AVMEDIA_TYPE_VIDEO;
        avcctx->flags |= AV_CODEC_FLAG_LOOP_FILTER;

        if(codec->capabilities & AV_CODEC_CAP_TRUNCATED)
        {
            avcctx->flags |= AV_CODEC_FLAG_TRUNCATED;
        }

        int ret = avcodec_open2(avcctx.get(), codec, nullptr);

        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_open2", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        frame.reset(av_frame_alloc());

        if(! frame)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "av_frame_alloc", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        frame->width = avcctx->width;
        frame->height = avcctx->height;
        frame->format = avcctx->pix_fmt;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 48, 100)
        frame->colorspace = AVCOL_SPC_BT709;
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 92, 100)
        frame->chroma_location = AVCHROMA_LOC_LEFT;
#endif
        frame->pts = 0;
        ret = av_frame_get_buffer(frame.get(), 0 /* align auto*/);

        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "av_frame_get_buffer", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        AVPixelFormat avPixelFormat = AV_PIX_FMT_BGR0;
#else
        AVPixelFormat avPixelFormat = AV_PIX_FMT_0RGB;
#endif
        swsctx.reset(sws_getContext(avcctx->width, avcctx->height, avPixelFormat,
                                    frame->width, frame->height, (AVPixelFormat) frame->format, SWS_BILINEAR, nullptr, nullptr, nullptr));
        packet.reset(av_packet_alloc());
        Application::info("%s: %s, size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, RFB::encodingName(getType()), csz.width,
                          csz.height);
    }

    void RFB::EncodingFFmpeg::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        std::scoped_lock guard{ lockUpdate };

        if(! avcctx)
        {
            initContext(fb.region().toSize());
        }
        else if(fb.width() != avcctx->width || fb.height() != avcctx->height)
        {
            Application::warning("%s: incorrect region size: [%u, %u]", __FUNCTION__, fb.width(), fb.height());
            initContext(fb.region().toSize());
        }

        const uint8_t* data[1] = { fb.pitchData(0) };
        int lines[1] = { (int) fb.pitchSize() };
        sws_scale(swsctx.get(), data, lines, 0, fb.height(), frame->data, frame->linesize);
        frame->pts = pts++;
        int ret = 0;

        if(ret = avcodec_send_frame(avcctx.get(), frame.get()); 0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_send_frame", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        if(ret = avcodec_receive_packet(avcctx.get(), packet.get()); 0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_receive_packet", FFMPEG::error(ret),
                               ret);

            if(ret != EAGAIN)
            {
                throw ffmpeg_error(NS_FuncName);
            }
        }

        st->sendIntBE16(1);
        st->sendHeader(getType(), fb.region());

        // send region
        if(ret == 0)
        {
            st->sendIntBE32(packet->size);
            Application::trace("%s: packet size: %d", __FUNCTION__, packet->size);
            st->sendRaw(packet->data, packet->size);
            updatePoint = std::chrono::steady_clock::now();
        }
        else
        {
            st->sendIntBE32(0);
        }

        st->sendFlush();
    }

    size_t RFB::EncodingFFmpeg::updateTimeMS(void) const
    {
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - updatePoint);
        return dt.count();
    }

#endif

#ifdef LTSM_DECODING_FFMPEG
    // DecodingFFmpeg
    RFB::DecodingFFmpeg::DecodingFFmpeg(int type) : DecodingBase(type)
    {
        av_log_set_level(AV_LOG_QUIET);
        av_log_set_callback(FFMPEG::logCallback);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
        avcodec_register_all();
#endif

        switch(type)
        {
            case RFB::ENCODING_FFMPEG_H264:
                codec = avcodec_find_decoder(AV_CODEC_ID_H264);
                break;

            case RFB::ENCODING_FFMPEG_VP8:
                codec = avcodec_find_decoder(AV_CODEC_ID_VP8);
                break;

            case RFB::ENCODING_FFMPEG_AV1:
                codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
                break;

            default:
                break;
        }

        if(! codec)
        {
            Application::error("%s: %s failed", __FUNCTION__, "avcodec_find_encoder");
            throw ffmpeg_error(NS_FuncName);
        }
    }

    void RFB::DecodingFFmpeg::setDebug(int val)
    {
        // encoding:debug
        switch(val)
        {
            case 0:
                av_log_set_level(AV_LOG_QUIET);
                break;

            case 1:
                av_log_set_level(AV_LOG_ERROR);
                break;

            case 2:
                av_log_set_level(AV_LOG_WARNING);
                break;

            case 3:
                av_log_set_level(AV_LOG_INFO);
                break;

            case 4:
                av_log_set_level(AV_LOG_VERBOSE);
                break;

            case 5:
                av_log_set_level(AV_LOG_DEBUG);
                break;

            default:
                av_log_set_level(AV_LOG_TRACE);
                break;
        }

        DecodingBase::setDebug(val);
    }

    void RFB::DecodingFFmpeg::resizedEvent(const XCB::Size & nsz)
    {
        std::scoped_lock guard{ lockUpdate };

        if(avcctx && (avcctx->width != nsz.width || avcctx->height != nsz.height))
        {
            initContext(nsz);
        }
    }

    void RFB::DecodingFFmpeg::initContext(const XCB::Size & csz)
    {
        rgbdata.reset();
        rgb.reset();
        packet.reset();
        frame.reset();
        swsctx.reset();
        avcctx.reset();
        avcctx.reset(avcodec_alloc_context3(codec));

        if(! avcctx)
        {
            Application::error("%s: %s failed", __FUNCTION__, "avcodec_alloc_context3");
            throw ffmpeg_error(NS_FuncName);
        }

        if(codec->capabilities & AV_CODEC_CAP_TRUNCATED)
        {
            avcctx->flags |= AV_CODEC_FLAG_TRUNCATED;
        }

        avcctx->time_base = (AVRational)
        {
            1, 25
        };

        avcctx->pix_fmt = AV_PIX_FMT_YUV420P;
        avcctx->width = csz.width;
        avcctx->height = csz.height;
        avcctx->codec_type = AVMEDIA_TYPE_VIDEO;
        avcctx->extradata = nullptr;
        int ret = avcodec_open2(avcctx.get(), codec, nullptr);

        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_open2", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        frame.reset(av_frame_alloc());

        if(! frame)
        {
            Application::error("%s: %s failed", __FUNCTION__, "av_frame_alloc");
            throw ffmpeg_error(NS_FuncName);
        }

        frame->width = avcctx->width;
        frame->height = avcctx->height;
        frame->format = avcctx->pix_fmt;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 48, 100)
        frame->colorspace = AVCOL_SPC_BT709;
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 92, 100)
        frame->chroma_location = AVCHROMA_LOC_LEFT;
#endif
        frame->pts = 0;
        ret = av_frame_get_buffer(frame.get(), 0 /* align auto*/);

        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "av_frame_get_buffer", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        AVPixelFormat avPixelFormat = AV_PIX_FMT_BGR0;
#else
        AVPixelFormat avPixelFormat = AV_PIX_FMT_0RGB;
#endif
        int bpp;
        uint32_t rmask, gmask, bmask, amask;

        if(Tools::AV_PixelFormatEnumToMasks(avPixelFormat, & bpp, & rmask, & gmask, & bmask, & amask, false))
        {
            pf = PixelFormat(bpp, rmask, gmask, bmask, amask);
        }
        else
        {
            Application::error("%s: unknown pixel format: %s, id: %d", __FUNCTION__, av_get_pix_fmt_name(avPixelFormat),
                               (int) avPixelFormat);
            throw ffmpeg_error(NS_FuncName);
        }

        swsctx.reset(sws_getContext(avcctx->width, avcctx->height, avcctx->pix_fmt,
                                    frame->width, frame->height, avPixelFormat, SWS_BILINEAR, nullptr, nullptr, nullptr));
        packet.reset(av_packet_alloc());

        if(ret = av_image_get_buffer_size(avPixelFormat, avcctx->width, avcctx->height, 1); 0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "av_image_get_buffer_size", FFMPEG::error(ret),
                               ret);
            throw ffmpeg_error(NS_FuncName);
        }

        rgb.reset(av_frame_alloc());
        rgb->width = avcctx->width;
        rgb->height = avcctx->height;
        rgb->format = avPixelFormat;
        rgbdata.reset((uint8_t*) av_malloc(ret));

        if(ret = av_image_fill_arrays(rgb->data, rgb->linesize, rgbdata.get(),
                                      (AVPixelFormat) rgb->format, rgb->width, rgb->height, 1); 0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "av_image_fill_arrays", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        Application::info("%s: %s, size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, RFB::encodingName(getType()), csz.width,
                          csz.height);
    }

    void RFB::DecodingFFmpeg::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        if(debug)
        {
            Application::debug("%s: decoding region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x,
                               reg.y, reg.width, reg.height);
        }

        auto len = cli.recvIntBE32();
        auto buf = cli.recvData(len);

        if(0 == len)
        {
            return;
        }

        if(reg.toSize() != cli.clientSize())
        {
            Application::warning("%s: incorrect region size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.width, reg.height);
            return;
        }

        // The input buffer, avpkt->data must be AV_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes
        if(len % AV_INPUT_BUFFER_PADDING_SIZE)
        {
            buf.resize(buf.size() + AV_INPUT_BUFFER_PADDING_SIZE - (len % AV_INPUT_BUFFER_PADDING_SIZE), 0);
        }

        std::scoped_lock guard{ lockUpdate };

        if(! avcctx)
        {
            initContext(reg);
        }

        packet->data = buf.data();
        packet->size = len;
        int ret = 0;
        bool haveFrame = false;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)

        if(ret = avcodec_send_packet(avcctx.get(), packet.get()); 0 > ret)
        {
            Application::error("%s: %d %d", __FUNCTION__, AV_INPUT_BUFFER_PADDING_SIZE, packet->size);
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_send_packet", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        do
        {
            ret = avcodec_receive_frame(avcctx.get(), frame.get());
        }
        while(ret == AVERROR(EAGAIN));

        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_receive_frame", FFMPEG::error(ret),
                               ret);
            throw ffmpeg_error(NS_FuncName);
        }

        haveFrame = (ret == 0);
#else
        int gotFrame = 0;

        if(ret = avcodec_decode_video2(avcctx.get(), frame.get(), & gotFrame, packet.get()); 0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_decode_video2", FFMPEG::error(ret),
                               ret);
            throw ffmpeg_error(NS_FuncName);
        }

        haveFrame = (gotFrame != 0);
#endif

        if(haveFrame)
        {
            int heightResult = sws_scale(swsctx.get(), (uint8_t const* const*) frame->data,
                                         frame->linesize, 0, avcctx->height, rgb->data, rgb->linesize);

            if(heightResult < 0)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "sws_scale", FFMPEG::error(heightResult),
                                   heightResult);
                throw ffmpeg_error(NS_FuncName);
            }

            if(heightResult == avcctx->height)
            {
                cli.updateRawPixels(rgb->data[0], XCB::Size(avcctx->width, avcctx->height), rgb->linesize[0], pf);
            }

            av_frame_unref(frame.get());
        }
    }

#endif
}
