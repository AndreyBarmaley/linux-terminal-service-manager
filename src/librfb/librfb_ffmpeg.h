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

#ifndef _LIBRFB_FFMPEG_
#define _LIBRFB_FFMPEG_

#include <mutex>
#include <chrono>
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
#include "libavutil/imgutils.h"

#ifdef __cplusplus
}

#endif

#include "librfb_encodings.h"
#include "librfb_decodings.h"

namespace LTSM
{
    struct ffmpeg_error : public std::runtime_error
    {
        explicit ffmpeg_error(std::string_view what) : std::runtime_error(what.data()) {}
    };

    struct AVCodecContextDeleter
    {
        void operator()(AVCodecContext* ctx)
        {
            avcodec_close(ctx);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 69, 100)
            avcodec_free_context(& ctx);
#else
            avcodec_free(ctx);
#endif
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
#ifdef LTSM_ENCODING_FFMPEG
        /// EncodingFFmpeg
        class EncodingFFmpeg : public EncodingBase
        {
            std::unique_ptr<AVCodecContext, AVCodecContextDeleter> avcctx;
            std::unique_ptr<SwsContext, SwsContextDeleter> swsctx;
            std::unique_ptr<AVFrame, AVFrameDeleter> frame;
            std::unique_ptr<AVPacket, AVPacketDeleter> packet;

#if LIBAVFORMAT_VERSION_MAJOR < 59
            AVCodec* codec = nullptr;
#else
            const AVCodec* codec = nullptr;
#endif

            std::mutex lockUpdate;
            std::chrono::steady_clock::time_point updatePoint;

            //int bitrate = 1024;
            int fps = 25;
            int pts = 0;

        protected:
            void initContext(const XCB::Size &);

        public:
            void resizedEvent(const XCB::Size &) override;
            void sendFrameBuffer(EncoderStream*, const FrameBuffer &) override;
            void setDebug(int) override;

            size_t updateTimeMS(void) const;

            EncodingFFmpeg(int type);
            ~EncodingFFmpeg() = default;
        };

#endif // ENCODING_FFMPEG

#ifdef LTSM_DECODING_FFMPEG
        /// DecodingFFmpeg
        class DecodingFFmpeg : public DecodingBase
        {
            PixelFormat pf;

            std::unique_ptr<AVCodecContext, AVCodecContextDeleter> avcctx;
            std::unique_ptr<SwsContext, SwsContextDeleter> swsctx;
            std::unique_ptr<AVFrame, AVFrameDeleter> frame;
            std::unique_ptr<AVPacket, AVPacketDeleter> packet;
            std::unique_ptr<AVFrame, AVFrameDeleter> rgb;
            std::unique_ptr<uint8_t, decltype(av_free)*> rgbdata{nullptr, av_free};

#if LIBAVFORMAT_VERSION_MAJOR < 59
            AVCodec* codec = nullptr;
#else
            const AVCodec* codec = nullptr;
#endif

            std::mutex lockUpdate;

        protected:
            void initContext(const XCB::Size &);

        public:
            void resizedEvent(const XCB::Size &) override;
            void updateRegion(DecoderStream &, const XCB::Region &) override;
            void setDebug(int) override;

            DecodingFFmpeg(int type);
            ~DecodingFFmpeg() = default;
        };

#endif //  DECODING_FFMPEG
    }
}

#endif // _LIBRFB_FFMPEG_
