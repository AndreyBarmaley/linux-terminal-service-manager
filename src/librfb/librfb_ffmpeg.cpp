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

namespace LTSM
{
    namespace FFMPEG
    {
        std::array<char,1024> errbuf;

        const char* error(int errnum)
        {
            std::fill(errbuf.begin(), errbuf.end(), 0);
            return 0 > av_strerror(errnum, errbuf.data(), errbuf.size() - 1) ? "error not found" : errbuf.data();
        }
    }

#ifdef LTSM_ENCODING_FFMPEG
    // EncodingFFmpeg
    RFB::EncodingFFmpeg::EncodingFFmpeg() : EncodingBase(ENCODING_FFMPEG_X264)
    {
        av_log_set_level(AV_LOG_QUIET);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
        avcodec_register_all();
#endif

        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if(! codec)
        {
            Application::error("%s: %s failed", __FUNCTION__, "avcodec_find_encoder");
            throw ffmpeg_error(NS_FuncName);
        }
    }

    void RFB::EncodingFFmpeg::initContext(size_t width, size_t height)
    {
	avcctx.reset(avcodec_alloc_context3(codec));
    	if(! avcctx)
    	{
    	    Application::error("%s: %s failed", __FUNCTION__, "avcodec_alloc_context3");
    	    throw ffmpeg_error(NS_FuncName);
    	}

	avcctx->delay = 0;
	//avcctx->bit_rate = xxxx;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 13, 100)
    	avcctx->framerate = (AVRational){ fps, 1 };
#endif
    	avcctx->time_base = (AVRational){ 1, fps };

    	av_opt_set(avcctx.get(), "preset", "medium", AV_OPT_SEARCH_CHILDREN);
    	av_opt_set(avcctx.get(), "tune", "zerolatency", AV_OPT_SEARCH_CHILDREN);

    	avcctx->pix_fmt = AV_PIX_FMT_YUV420P;
        avcctx->width = width;
        avcctx->height = height;
    	avcctx->codec_type = AVMEDIA_TYPE_VIDEO;
    	avcctx->flags |= AV_CODEC_FLAG_LOOP_FILTER;

	if(codec->capabilities & AV_CODEC_CAP_TRUNCATED)
    	    avcctx->flags |= AV_CODEC_FLAG_TRUNCATED;

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

        ret = av_frame_get_buffer(frame.get(), 1);
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

    }

    void RFB::EncodingFFmpeg::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        if(! avcctx)
	    initContext(fb.width(), fb.height());

	if(fb.width() != avcctx->width || fb.height() != avcctx->height)
	{
            Application::error("%s: %s, new sz: %dx%x, old sz: %dx%d", __FUNCTION__, "context resized", fb.width(), fb.height(), avcctx->width, avcctx->height);
            throw ffmpeg_error(NS_FuncName);
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
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_receive_packet", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        // send region
        if(ret == 0)
	{
	    st->sendIntBE16(1);
    	    st->sendHeader(getType(), fb.region());
    	    st->sendIntBE32(packet->size);

    	    Application::info("%s: packet size: %d", __FUNCTION__, packet->size);
	    st->sendRaw(packet->data, packet->size);

    	    st->sendFlush();
	}
    }
#endif

#ifdef LTSM_DECODING_FFMPEG
    // DecodingFFmpeg
    RFB::DecodingFFmpeg::DecodingFFmpeg() : DecodingBase(ENCODING_FFMPEG_X264)
    {
        av_log_set_level(AV_LOG_QUIET);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
        avcodec_register_all();
#endif

        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if(! codec)
        {
            Application::error("%s: %s failed", __FUNCTION__, "avcodec_find_encoder");
            throw ffmpeg_error(NS_FuncName);
        }
    }

    void RFB::DecodingFFmpeg::initContext(size_t width, size_t height)
    {
    	avcctx.reset(avcodec_alloc_context3(codec));
    	if(! avcctx)
    	{
    	    Application::error("%s: %s failed", __FUNCTION__, "avcodec_alloc_context3");
    	    throw ffmpeg_error(NS_FuncName);
    	}

	if(codec->capabilities & AV_CODEC_CAP_TRUNCATED)
    	    avcctx->flags |= AV_CODEC_FLAG_TRUNCATED;

    	avcctx->time_base = (AVRational){ 1, 25 };
    	avcctx->pix_fmt = AV_PIX_FMT_YUV420P;
    	avcctx->width = width;
    	avcctx->height = height;
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

        ret = av_frame_get_buffer(frame.get(), 1);
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
        swsctx.reset(sws_getContext(avcctx->width, avcctx->height, avcctx->pix_fmt,
                        frame->width, frame->height, avPixelFormat, SWS_BILINEAR, nullptr, nullptr, nullptr));

	packet.reset(av_packet_alloc());

        if(ret = av_image_get_buffer_size(avPixelFormat, avcctx->width, avcctx->height, 1); 0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "av_image_get_buffer_size", FFMPEG::error(ret), ret);
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
    }

    void RFB::DecodingFFmpeg::updateRegion(DecoderStream & cli, const XCB::Region & reg)
    {
        if(debug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        auto len = cli.recvIntBE32();
        auto buf  = cli.recvData(len);

	if(reg.toSize() != cli.clientSize())
	{
	    if(avcctx)
	    {
    		Application::warning("%s: incorrect size: [%d, %d], decoder reset", __FUNCTION__, reg.width, reg.height);
	        rgbdata.reset();
	        rgb.reset();
	        packet.reset();
	        frame.reset();
	        swsctx.reset();
	        avcctx.reset();
	    }
    	    // client resized, wait correct geometry
	    return;
	}

	// The input buffer, avpkt->data must be AV_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes
	if(len % AV_INPUT_BUFFER_PADDING_SIZE)
	    buf.resize(buf.size() + AV_INPUT_BUFFER_PADDING_SIZE - (len % AV_INPUT_BUFFER_PADDING_SIZE), 0);

	if(! avcctx)
	    initContext(reg.width, reg.height);

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
        } while (ret == AVERROR(EAGAIN));

        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_receive_frame", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        haveFrame = (ret == 0);
#else
	int gotFrame = 0;

	if(ret = avcodec_decode_video2(avcctx.get(), frame.get(), & gotFrame, packet.get()); 0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_decode_video2", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        haveFrame = (gotFrame != 0);
#endif

	if(haveFrame)
	{
	    sws_scale(swsctx.get(), (uint8_t const* const*) frame->data,
		    frame->linesize, 0, avcctx->height, rgb->data, rgb->linesize);

	    int bpp; uint32_t rmask, gmask, bmask, amask;

	    if(Tools::AV_PixelFormatEnumToMasks((AVPixelFormat) rgb->format, & bpp, & rmask, & gmask, & bmask, & amask, false))
		cli.updateRawPixels(rgb->data[0], avcctx->width, avcctx->height, rgb->linesize[0], bpp, rmask, gmask, bmask, amask);
	}
    }
#endif
}
