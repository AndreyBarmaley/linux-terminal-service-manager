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
#include "ltsm_tools.h"
#include "librfb_ffmpeg.h"
#include "ffmpeg_tools.h"

namespace LTSM {
    namespace FFMPEG {
        std::array<char, 1024> errbuf;
        std::array<char, 1024> logbuf;

        const char* error(int errnum) {
            std::ranges::fill(errbuf, 0);
            return 0 > av_strerror(errnum, errbuf.data(), errbuf.size() - 1) ? "error not found" : errbuf.data();
        }

        void logCallback(void* avcl, int lvl, const char* fmt, va_list vl) {
            if(av_log_get_level() < lvl) {
                return;
            }

            if(int len = vsnprintf(logbuf.data(), logbuf.size(), fmt, vl); 0 < len) {
                AVClass* avc = avcl ? *(AVClass**) avcl : nullptr;
                const char* name = avc ? avc->item_name(avcl) : nullptr;
                const char* type = "ffmpeg debug";

                // remove endl
                if(1 < len) {
                    len--;
                }

                switch(lvl) {
                    case AV_LOG_PANIC:
                    case AV_LOG_FATAL:
                    case AV_LOG_ERROR:
                        Application::error("{}: [{}] {:.{}}", type, name, logbuf.data(), len);
                        break;

                    case AV_LOG_WARNING:
                        Application::warning("{}: [{}] {:.{}}", type, name, logbuf.data(), len);
                        break;

                    case AV_LOG_INFO:
                    case AV_LOG_VERBOSE:
                        Application::notice("{}: [{}] {:.{}}", type, name, logbuf.data(), len);
                        break;

                    case AV_LOG_DEBUG:
                    case AV_LOG_TRACE:
                        Application::info("{}: [{}] {:.{}}", type, name, logbuf.data(), len);
                        break;

                    default:
                        break;
                }
            }
        }
    }

#ifdef LTSM_ENCODING_FFMPEG
    // EncodingFFmpeg
    RFB::EncodingFFmpeg::EncodingFFmpeg(int type, int fps_) : EncodingBase(type), fps(fps_) {
        av_log_set_level(AV_LOG_QUIET);
        av_log_set_callback(FFMPEG::logCallback);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
        avcodec_register_all();
#endif

        switch(type) {
            case RFB::ENCODING_LTSM_H264:
                codec = avcodec_find_encoder(AV_CODEC_ID_H264);
                break;

            case RFB::ENCODING_LTSM_VP8:
                codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
                break;

            case RFB::ENCODING_LTSM_AV1:
                codec = avcodec_find_encoder(AV_CODEC_ID_AV1);
                break;

            default:
                break;
        }

        if(! codec) {
            Application::error("{}: {} failed, type: {}, encoding: {}", __FUNCTION__, "avcodec_find_encoder", type,
                               encodingName(type));
            throw ffmpeg_error(NS_FuncNameS);
        }

        Application::info("{}: set FPS: {}", __FUNCTION__, fps);
    }

    const char* RFB::EncodingFFmpeg::getTypeName(void) const {
        switch(getType()) {
            case RFB::ENCODING_LTSM_H264:
                return "FFMPEG_H264";

            case RFB::ENCODING_LTSM_VP8:
                return "FFMPEG_VP8";

            case RFB::ENCODING_LTSM_AV1:
                return "FFMPEG_AV1";

            default:
                break;
        }

        return "FFMPEG_UNKNOWN";
    }

    /*
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
        }
    */

    void RFB::EncodingFFmpeg::setFps(uint32_t val) {
        std::scoped_lock guard{ lockUpdate };
        fps = val;
        Application::info("{}: set FPS: {}", __FUNCTION__, fps);
        if(avcctx) {
            initContext(XCB::Size(avcctx->width, avcctx->height));
        }
    }

    void RFB::EncodingFFmpeg::resizedEvent(const XCB::Size & nsz) {
        std::scoped_lock guard{ lockUpdate };

        if(avcctx && (avcctx->width != nsz.width || avcctx->height != nsz.height)) {
            initContext(nsz);
        }
    }

    void RFB::EncodingFFmpeg::initContext(const XCB::Size & csz) {
        packet.reset();
        frame.reset();
        swsctx.reset();
        avcctx.reset();
        avcctx.reset(avcodec_alloc_context3(codec));

        if(! avcctx) {
            Application::error("{}: {} failed", __FUNCTION__, "avcodec_alloc_context3");
            throw ffmpeg_error(NS_FuncNameS);
        }

        avcctx->delay = 0;
        //avcctx->bit_rate = xxxx;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 13, 100)
        avcctx->framerate = (AVRational) {
            fps, 1
        };

#endif
        avcctx->time_base = (AVRational) {
            1, fps
        };

        switch(codec->id) {
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

        avcctx->thread_count = threads;
        avcctx->pix_fmt = localFormat;
        avcctx->width = csz.width;
        avcctx->height = csz.height;
        avcctx->codec_type = AVMEDIA_TYPE_VIDEO;
        avcctx->flags |= AV_CODEC_FLAG_LOOP_FILTER;

        // if(codec->capabilities & AV_CODEC_CAP_TRUNCATED)
        // {
        // avcctx->flags |= AV_CODEC_FLAG_TRUNCATED;
        // }

        int ret = avcodec_open2(avcctx.get(), codec, nullptr);

        if(0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "avcodec_open2", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        frame.reset(av_frame_alloc());

        if(! frame) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "av_frame_alloc", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        frame->width = avcctx->width;
        frame->height = avcctx->height;
        frame->format = localFormat;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 48, 100)
        frame->colorspace = AVCOL_SPC_BT709;
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 92, 100)
        frame->chroma_location = AVCHROMA_LOC_LEFT;
#endif
        frame->pts = 0;
        ret = av_frame_get_buffer(frame.get(), 0 /* align auto*/);

        if(0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "av_frame_get_buffer", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        swsctx.reset(sws_getContext(avcctx->width, avcctx->height, remoteFormat,
                                    frame->width, frame->height, localFormat, SWS_BILINEAR, nullptr, nullptr, nullptr));
        packet.reset(av_packet_alloc());
        Application::info("{}: {}, size: {}", __FUNCTION__, RFB::encodingName(getType()), csz);
    }

    void RFB::EncodingFFmpeg::sendFrameBuffer(EncoderStream* st, const FrameBuffer & fb) {
        std::scoped_lock guard{ lockUpdate };

        if(! avcctx) {
            initContext(fb.region().toSize());
        } else if(fb.width() != avcctx->width || fb.height() != avcctx->height) {
            Application::warning("{}: incorrect region size: {}", __FUNCTION__, fb.region().toSize());
            initContext(fb.region().toSize());
        }

        const uint8_t* data[1] = { fb.pitchData(0) };
        int lines[1] = { (int) fb.pitchSize() };
        sws_scale(swsctx.get(), data, lines, 0, fb.height(), frame->data, frame->linesize);
        frame->pts = pts++;
        int ret = 0;

        // ref: https://ffmpeg.org/doxygen/7.0/encode_video_8c-example.html
        if(ret = avcodec_send_frame(avcctx.get(), frame.get()); 0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "avcodec_send_frame", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        while(ret >= 0) {
            ret = avcodec_receive_packet(avcctx.get(), packet.get());
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if(ret < 0) {
                Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "avcodec_receive_packet", FFMPEG::error(ret),
                               ret);
                throw ffmpeg_error(NS_FuncNameS);
            }

            st->sendIntBE16(1);
            st->sendHeader(getType(), fb.region());

            // send region
            st->sendIntBE32(packet->size);
            Application::trace(DebugType::Enc, "{}: packet size: {}", __FUNCTION__, packet->size);
            st->sendRaw(packet->data, packet->size);
        }

        st->sendFlush();
    }

#endif

#ifdef LTSM_DECODING_FFMPEG
    // DecodingFFmpeg
    RFB::DecodingFFmpeg::DecodingFFmpeg(int type, int fps_) : DecodingBase(type), fps(fps_) {
        av_log_set_level(AV_LOG_QUIET);
        av_log_set_callback(FFMPEG::logCallback);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
        avcodec_register_all();
#endif

        switch(type) {
#ifdef LTSM_DECODING_H264

            case RFB::ENCODING_LTSM_H264:
                codec = avcodec_find_decoder(AV_CODEC_ID_H264);
                break;
#endif
#ifdef LTSM_DECODING_VP8

            case RFB::ENCODING_LTSM_VP8:
                codec = avcodec_find_decoder(AV_CODEC_ID_VP8);
                break;
#endif
#ifdef LTSM_DECODING_AV1

            case RFB::ENCODING_LTSM_AV1:
                codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
                break;
#endif

            default:
                break;
        }

        if(! codec) {
            Application::error("{}: {} failed", __FUNCTION__, "avcodec_find_encoder");
            throw ffmpeg_error(NS_FuncNameS);
        }

        Application::info("{}: set FPS: {}", __FUNCTION__, fps);
    }

    /*
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
        }
    */

    void RFB::DecodingFFmpeg::resizedEvent(const XCB::Size & nsz) {
        std::scoped_lock guard{ lockUpdate };

        Application::debug(DebugType::Enc, "{}: received", __FUNCTION__);

        if(! localFrame || XCB::Size(localFrame->width, localFrame->height) != nsz) {
            initLocalContext(nsz);
        }
    }

    void RFB::DecodingFFmpeg::initLocalContext(const XCB::Size & csz) {
        Application::debug(DebugType::Enc, "{}: size: {}", __FUNCTION__, csz);

        // init local frame
        localFrame.reset(av_frame_alloc());

        if(! localFrame) {
            Application::error("{}: {} failed", __FUNCTION__, "av_frame_alloc");
            throw ffmpeg_error(NS_FuncNameS);
        }

        localFrame->width = csz.width;
        localFrame->height = csz.height;
        localFrame->format = localFormat;

        int ret = av_image_get_buffer_size(localFormat, localFrame->width, localFrame->height, 1);
        if(0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "av_image_get_buffer_size", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        localData.reset((uint8_t*) av_malloc(ret));

        if(! localData) {
            Application::error("{}: {} failed", __FUNCTION__, "av_malloc");
            throw ffmpeg_error(NS_FuncNameS);
        }

        if(int ret = av_image_fill_arrays(localFrame->data, localFrame->linesize, localData.get(),
                                      localFormat, localFrame->width, localFrame->height, 1); 0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "av_image_fill_arrays", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        // init pixel format
        int bpp;
        uint32_t rmask, gmask, bmask, amask;

        if(Tools::AV_PixelFormatEnumToMasks(localFormat, & bpp, & rmask, & gmask, & bmask, & amask, false)) {
            pf = PixelFormat(bpp, rmask, gmask, bmask, amask);
        } else {
            Application::error("{}: unknown pixel format: {}, id: {}", __FUNCTION__, av_get_pix_fmt_name(localFormat), localFrame->format);
            throw ffmpeg_error(NS_FuncNameS);
        }

        initSwScaller();
    }

    void RFB::DecodingFFmpeg::initRemoteContext(const XCB::Size & fsz) {
        Application::debug(DebugType::Enc, "{}: size: {}", __FUNCTION__, fsz);

        avcctx.reset(avcodec_alloc_context3(codec));

        if(! avcctx) {
            Application::error("{}: {} failed", __FUNCTION__, "avcodec_alloc_context3");
            throw ffmpeg_error(NS_FuncNameS);
        }

        // if(codec->capabilities & AV_CODEC_CAP_TRUNCATED)
        // {
        // avcctx->flags |= AV_CODEC_FLAG_TRUNCATED;
        // }

        avcctx->framerate = (AVRational) {
            fps, 1
        };

        avcctx->time_base = (AVRational) {
            1, fps
        };

        avcctx->pix_fmt = remoteFormat;
        avcctx->width = fsz.width;
        avcctx->height = fsz.height;
        avcctx->codec_type = AVMEDIA_TYPE_VIDEO;
        avcctx->extradata = nullptr;
        int ret = avcodec_open2(avcctx.get(), codec, nullptr);

        if(0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "avcodec_open2", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        // init av frame with frame size
        remoteFrame.reset(av_frame_alloc());

        if(! remoteFrame) {
            Application::error("{}: {} failed", __FUNCTION__, "av_frame_alloc");
            throw ffmpeg_error(NS_FuncNameS);
        }

        remoteFrame->width = avcctx->width;
        remoteFrame->height = avcctx->height;
        remoteFrame->format = remoteFormat;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 48, 100)
        remoteFrame->colorspace = AVCOL_SPC_BT709;
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 92, 100)
        remoteFrame->chroma_location = AVCHROMA_LOC_LEFT;
#endif
        remoteFrame->pts = 0;

        if(int ret = av_frame_get_buffer(remoteFrame.get(), 0 /* align auto*/); 0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "av_frame_get_buffer", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        remotePacket.reset(av_packet_alloc());

        if(! remotePacket) {
            Application::error("{}: {} failed", __FUNCTION__, "av_packet_alloc");
            throw ffmpeg_error(NS_FuncNameS);
        }

        initSwScaller();
    }

    void RFB::DecodingFFmpeg::initSwScaller(void) {
        // init scaller (src frame, dst frame)
        if(remoteFrame && localFrame) {
            swsctx.reset(sws_getContext(avcctx->width, avcctx->height, remoteFormat,
                                    localFrame->width, localFrame->height, localFormat, SWS_BILINEAR, nullptr, nullptr, nullptr));
        }
        Application::debug(DebugType::Enc, "{}: success", __FUNCTION__);
    }

    void RFB::DecodingFFmpeg::updateRegion(DecoderStream & cli, const XCB::Region & reg) {
        Application::trace(DebugType::Enc, "{}: decoding region {}", __FUNCTION__, reg);

        auto len = cli.recvIntBE32();
        auto buf = cli.recvData(len);

        if(0 == len) {
            return;
        }

        std::scoped_lock guard{ lockUpdate };

        if(! localFrame || XCB::Size(localFrame->width, localFrame->height) != cli.clientSize()) {
            initLocalContext(cli.clientSize());
        }

        if(! remoteFrame) {
            initRemoteContext(reg.toSize());
        } else if(reg.toSize() != XCB::Size(avcctx->width, avcctx->height)) {
            initRemoteContext(reg.toSize());
        }

        // The input buffer, avpkt->data must be AV_INPUT_BUFFER_PADDING_SIZE larger than the actual read bytes
        if(len % AV_INPUT_BUFFER_PADDING_SIZE) {
            buf.resize(buf.size() + AV_INPUT_BUFFER_PADDING_SIZE - (len % AV_INPUT_BUFFER_PADDING_SIZE), 0);
        }

        remotePacket->data = buf.data();
        remotePacket->size = len;
        int ret = 0;

        Application::trace(DebugType::Enc, "{}: padding size: {}, packet size: {}, buf size: {}", __FUNCTION__, AV_INPUT_BUFFER_PADDING_SIZE, remotePacket->size, buf.size());

        // ref: https://ffmpeg.org/doxygen/7.0/decode_video_8c-example.html
        if(ret = avcodec_send_packet(avcctx.get(), remotePacket.get()); 0 > ret) {
            Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "avcodec_send_packet", FFMPEG::error(ret), ret);
            throw ffmpeg_error(NS_FuncNameS);
        }

        while(ret >= 0) {
            ret = avcodec_receive_frame(avcctx.get(), remoteFrame.get());

            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if(ret < 0) {
                Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "avcodec_receive_frame", FFMPEG::error(ret), ret);
                throw ffmpeg_error(NS_FuncNameS);
            }

            if(int err = sws_scale_frame(swsctx.get(), localFrame.get(), remoteFrame.get()); 0 > err) {
                Application::error("{}: {} failed, error: {}, code: {}", __FUNCTION__, "sws_scale", FFMPEG::error(err), err);
                throw ffmpeg_error(NS_FuncNameS);
            }

            cli.updateRawPixels(XCB::Region(0, 0, localFrame->width, localFrame->height), localFrame->data[0], localFrame->linesize[0], pf);
            av_frame_unref(remoteFrame.get());
        }
    }

#endif
}
