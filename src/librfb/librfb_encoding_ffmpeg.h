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

#ifndef _LIBRFB_ENCODING_FFMPEG_
#define _LIBRFB_ENCODING_FFMPEG_

#include <memory>
#include <stdexcept>

#ifdef __cplusplus
extern "C" {
#endif

#include "libavdevice/avdevice.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/timestamp.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"

#ifdef __cplusplus
}
#endif

#include "librfb_encodings.h"

namespace LTSM
{
    struct ffmpeg_error : public std::runtime_error
    {
        explicit ffmpeg_error(const std::string & what) : std::runtime_error(what){}
        explicit ffmpeg_error(const char* what) : std::runtime_error(what){}
    };

    struct AVCodecContextDeleter
    {
        void operator()(AVCodecContext* ctx)
        {
            avcodec_free_context(& ctx);
        }
    };
        
    struct AVFormatContextDeleter
    {   
        void operator()(AVFormatContext* ctx)
        {
            avformat_free_context(ctx);
        }
    };
        
    struct SwsContextDeleter
    {   
        void operator()(SwsContext* ctx)
        {
            sws_freeContext(ctx);
        }
    };
        
    struct SwrContextDeleter
    {   
        void operator()(SwrContext* ctx)
        {
            swr_free(& ctx);
        }
    };
        
    struct AVFrameDeleter
    {   
        void operator()(AVFrame* ptr)
        {
            av_frame_free(& ptr);
        }
    };
        
    struct AVPacketDeleter
    {   
        void operator()(AVPacket* ptr)
        {
            av_packet_free(& ptr);
        }
    };

    namespace RFB
    {
        /// EncodingFFmpeg
        class EncodingFFmpeg : public EncodingBase
        {
#if LIBAVFORMAT_VERSION_MAJOR < 59
            AVOutputFormat* oformat = nullptr;
#else
            const AVOutputFormat* oformat = nullptr;
#endif
            std::unique_ptr<AVFormatContext, AVFormatContextDeleter> avfctx;
            std::unique_ptr<AVCodecContext, AVCodecContextDeleter> avcctx;
            std::unique_ptr<SwsContext, SwsContextDeleter> swsctx;
            std::unique_ptr<AVFrame, AVFrameDeleter> frame;

#if LIBAVFORMAT_VERSION_MAJOR < 59
            AVCodec* codec = nullptr;
#else
            const AVCodec* codec = nullptr;
#endif
            AVStream* stream = nullptr;

            int bitrate = 1024;
            int fps = 25;
            int pts = 0;

        protected:

        public:
            void                sendFrameBuffer(EncoderStream*, const FrameBuffer &) override;

            EncodingFFmpeg();
            ~EncodingFFmpeg();
        };
    }
}

#endif // _LIBRFB_ENCODING_FFMPEG_
