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
#include "librfb_client.h"
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
    RFB::EncodingFFmpeg::EncodingFFmpeg() : EncodingBase(ENCODING_FFMP)
    {
        av_log_set_level(AV_LOG_QUIET);

#if LIBAVFORMAT_VERSION_MAJOR < 58
        av_register_all();
        avcodec_register_all();
#endif
        oformat = av_guess_format("mp4", nullptr, nullptr);
        if(! oformat)
        {
            Application::error("%s: %s failed", __FUNCTION__, "av_guess_format");
            throw ffmpeg_error(NS_FuncName);
        }

        AVFormatContext* avfctx2 = nullptr;
        int ret = avformat_alloc_output_context2(& avfctx2, oformat, nullptr, nullptr);

        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avformat_alloc_output_context2", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        avfctx.reset(avfctx2);

        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if(! codec)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_find_encoder", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        stream = avformat_new_stream(avfctx.get(), codec);
        if(! stream)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avformat_new_stream", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        avcctx.reset(avcodec_alloc_context3(codec));
        if(! avcctx)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_alloc_context3", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        stream->id = avfctx->nb_streams - 1;
        stream->time_base = (AVRational){1, fps};
        stream->avg_frame_rate = (AVRational){fps, 1};

        auto codecpar = stream->codecpar;
        codecpar->codec_id = AV_CODEC_ID_H264;
        codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        codecpar->format = AV_PIX_FMT_YUV420P;
        codecpar->bit_rate = bitrate * 1024;

        ret = avcodec_parameters_to_context(avcctx.get(), codecpar);
        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_parameters_to_context", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        avcctx->time_base = (AVRational){1, fps};
        avcctx->framerate = (AVRational){fps, 1};
        avcctx->gop_size = 12;

        av_opt_set(avcctx.get(), "preset", "veryfast", 0);
    }

    RFB::EncodingFFmpeg::~EncodingFFmpeg()
    {
        uint8_t* buf = nullptr;
        int ret = avio_close_dyn_buf(avfctx->pb, & buf);
        if(buf)
            av_free(buf);
    }

    void RFB::EncodingFFmpeg::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb)
    {
        if(! swsctx || fb.width() != avcctx->width || fb.height() != avcctx->height)
        {
            if(swsctx)
            {
                Application::warning("%s: context resized, width: %d, height: %d", __FUNCTION__, fb.width(), fb.height());
            }

            avcctx->pix_fmt = AV_PIX_FMT_YUV420P;
            avcctx->width = fb.width();
            avcctx->height = fb.height();
        
            int ret = avcodec_parameters_from_context(stream->codecpar, avcctx.get());
            if(0 > ret)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_parameters_from_context", FFMPEG::error(ret), ret);
                throw ffmpeg_error(NS_FuncName);
            }
 
            ret = avcodec_open2(avcctx.get(), codec, nullptr);
            if(0 > ret)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_open2", FFMPEG::error(ret), ret);
                throw ffmpeg_error(NS_FuncName);
            }

            // write header
            if(! swsctx)
            {
                ret = avio_open_dyn_buf(& avfctx->pb);
                if(0 > ret)
                {
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avio_open_dyn_buf", FFMPEG::error(ret), ret);
                    throw ffmpeg_error(NS_FuncName);
                }
            }

            ret = avformat_write_header(avfctx.get(), nullptr);
            if(0 > ret)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avformat_write_header", FFMPEG::error(ret), ret);
                throw ffmpeg_error(NS_FuncName);
            }

            // init frame
            frame.reset(av_frame_alloc());
            if(! frame)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "av_frame_alloc", FFMPEG::error(ret), ret);
                throw ffmpeg_error(NS_FuncName);
            }

            frame->width = avcctx->width;
            frame->height = avcctx->height;
            frame->format = avcctx->pix_fmt;

            ret = av_frame_get_buffer(frame.get(), 32);
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
                        frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
        }

        const uint8_t* data[1] = { fb.pitchData(0) };
        int lines[1] = { (int) fb.pitchSize() };

        sws_scale(swsctx.get(), data, lines, 0, fb.height(), frame->data, frame->linesize);
        frame->pts = pts++;

        // write frame
        int ret = avcodec_send_frame(avcctx.get(), frame.get());
        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_send_frame", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }
            
        while(true)
        {
            std::unique_ptr<AVPacket, AVPacketDeleter> pkt(av_packet_alloc());
     
            int ret = avcodec_receive_packet(avcctx.get(), pkt.get());
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;

            if(0 > ret)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avcodec_receive_packet", FFMPEG::error(ret), ret);
                throw ffmpeg_error(NS_FuncName);
            }

            av_packet_rescale_ts(pkt.get(), avcctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;

            ret = av_write_frame(avfctx.get(), pkt.get());
            if(0 > ret)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "av_write_frame", FFMPEG::error(ret), ret);
                throw ffmpeg_error(NS_FuncName);
            }
        }

        uint8_t* buf = nullptr;
        ret = avio_get_dyn_buf(avfctx->pb, & buf);
        if(0 > ret)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avio_get_dyn_buf", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncName);
        }

        // send region
        st->sendIntBE16(1);
        st->sendHeader(getType(), fb.region());
        st->sendIntBE32(ret);
        st->sendRaw(buf, ret);
        st->sendFlush();
    }
#endif

#ifdef LTSM_DECODING_FFMPEG
    // DecodingFFmpeg
    RFB::DecodingFFmpeg::DecodingFFmpeg() : DecodingBase(ENCODING_FFMP)
    {
        av_log_set_level(AV_LOG_QUIET);

#if LIBAVFORMAT_VERSION_MAJOR < 58
        av_register_all();
        avcodec_register_all();
#endif

        avfctx.reset(avformat_alloc_context());

        if(! avfctx)
        {
            Application::error("%s: %s failed", __FUNCTION__, "avformat_alloc_context");
            throw ffmpeg_error(NS_FuncName);
        }
    }

    RFB::DecodingFFmpeg::~DecodingFFmpeg()
    {
        AVFormatContext* avfptr = avfctx.get();
        avformat_close_input(& avfptr);
    }

    int readPacketFromStreamBuf(void* opaque, uint8_t* buf, int sz)
    {
        if(auto sb = reinterpret_cast<StreamBuf*>(opaque))
        {
            size_t len = std::min(sb->last(), (size_t) sz);
            sb->readTo(buf, len);
            return len;
        }
        return 0;
    }

    void RFB::DecodingFFmpeg::updateRegion(ClientDecoder & cli, const XCB::Region & reg)
    {
        if(debug)
            Application::debug("%s: decoding region [%d,%d,%d,%d]", __FUNCTION__, reg.x, reg.y, reg.width, reg.height);

        auto len = cli.recvIntBE32();
        aviobuf.write(cli.recvData(len));

        avioctx.reset(avio_alloc_context(aviobuf.rawbuf().data(), aviobuf.last(), 0, & aviobuf, readPacketFromStreamBuf, nullptr, nullptr));

        if(! avioctx)
        {
            Application::error("%s: %s failed", __FUNCTION__, "avio_alloc_context");
            throw ffmpeg_error(NS_FuncName);
        }

        avfctx->pb = avioctx.get();

        if(0 == (avfctx->flags & AVFMT_FLAG_CUSTOM_IO))
        {
            avfctx->flags |= AVFMT_FLAG_CUSTOM_IO;
            AVFormatContext* avfptr = avfctx.get();

            int ret = avformat_open_input(& avfptr, "", nullptr, nullptr);
            if(0 > ret)
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "avformat_open_input", FFMPEG::error(ret), ret);
                throw ffmpeg_error(NS_FuncName);
            }

            if(avfctx.get() != avfptr)
                avfctx.reset(avfptr);

            // std::pair<AVStream*, int> findVideoStreamIndex
        }
        //
    }
#endif
}
