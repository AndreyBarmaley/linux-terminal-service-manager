/***********************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_AUDIO_PULSE_
#define _LTSM_AUDIO_PULSE_

#include <mutex>
#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include <thread>
#include <condition_variable>

#ifdef LTSM_CLIENT
#include "pulse/simple.h"
#endif

#include "pulse/stream.h"
#include "pulse/context.h"
#include "pulse/mainloop.h"
#include "pulse/introspect.h"

namespace LTSM
{
    class WaitNotify
    {
        std::mutex lock;
        std::condition_variable cv;

        const void* waitData = nullptr;
        int waitId = 0;

    public:
        WaitNotify() = default;

        void reset(void);
        const void* wait(int id);
        void notify(int id, const void*);
    };

    namespace PulseAudio
    {
        enum WaitOp { ContextServerInfo = 0xAB01, ContextDrain = 0xAB02, ContextLoadModule = 0xAB03, ContextSourceInfo = 0xAB04,
                      StreamCork = 0xAB12, StreamTrigger = 0xAB13, StreamFlush = 0xAB14, StreamDrain = 0xAB15
                    };

        struct MainLoopDeleter
        {
            void operator()(pa_mainloop* loop)
            {
                pa_mainloop_free(loop);
            }
        };

        struct ContextDeleter
        {
            void operator()(pa_context* ctx)
            {
                pa_context_unref(ctx);
            }
        };

        struct StreamDeleter
        {
            void operator()(pa_stream* st)
            {
                pa_stream_unref(st);
            }
        };

        uint16_t formatBits(const pa_sample_format_t &);

        class BaseStream
        {
        protected:
            pa_sample_spec audioSpec = { .format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 2 };

            WaitNotify waitNotify;

            std::unique_ptr<pa_mainloop, MainLoopDeleter> loop;
            std::unique_ptr<pa_context, ContextDeleter> ctx;
            std::unique_ptr<pa_stream, StreamDeleter> stream;

            std::atomic<int> contextState{PA_CONTEXT_UNCONNECTED};
            std::atomic<int> streamState{PA_STREAM_UNCONNECTED};

            const pa_server_info* serverInfo = nullptr;

        protected:
            static void contextStateCallback(pa_context* ctx, void* userData);
            static void streamStateCallback(pa_stream* stream, void* userData);
            static void streamSuspendedCallback(pa_stream* stream, void* userData);
            static void streamOverflowCallback(pa_stream* stream, void* userData);
            static void streamUnderflowCallback(pa_stream* stream, void* userData);
            static void sourceInfoCallback(pa_context* ctx, const pa_source_info* info, int eol, void *userData);

            static void contextServerInfoCallback(pa_context* ctx, const pa_server_info* info, void* userData);
            static void contextLoadModuleCallback(pa_context* ctx, uint32_t idx, void* userData);
            static void contextSourceInfoCallback(pa_context* ctx, const pa_source_info* info, int eol, void *userData);
            static void contextDrainCallback(pa_context* ctx, void* userData);


            static void streamCorkCallback(pa_stream* stream, int success, void* userData);
            static void streamTriggerCallback(pa_stream* stream, int success, void* userData);
            static void streamFlushCallback(pa_stream* stream, int success, void* userData);
            static void streamDrainCallback(pa_stream* stream, int success, void* userData);

            void contextDrainNotify(void);
            void contextDrainWait(void);

            void contextServerInfoNotify(const pa_server_info*);
            const pa_server_info* contextServerInfoWait(void);

            void contextLoadModuleNotify(uint32_t idx);
            uint32_t contextLoadModuleWait(const std::string & name, const std::string & args);

            void contextSourceInfoNotify(const pa_source_info* info, int eol);
            const pa_source_info* contextSourceInfoWait(const std::string & name);

            void streamCorkNotify(int success);
            bool streamCorkWait(bool);

            void streamTriggerNotify(int success);
            bool streamTriggerWait(void);

            void streamFlushNotify(int success);
            bool streamFlushWait(void);

            void streamDrainNotify(int success);
            bool streamDrainWait(void);

            virtual void contextStateEvent(const pa_context_state_t &);
            virtual void sourceInfoEvent(const pa_source_info* info, int eol);
            virtual void streamStateEvent(const pa_stream_state_t &);
            virtual void streamSuspendedEvent(int state);
            virtual void streamOverflowEvent(void);
            virtual void streamUnderflowEvent(int64_t index);

        public:
            BaseStream() = default;
            virtual ~BaseStream();

            BaseStream(const std::string &, const pa_sample_format_t &, uint32_t rate, uint8_t channels);

            bool initContext(void);
            void contextDisconnect(void);

            virtual const char* streamName(void) const = 0;
            virtual bool streamConnect(bool paused, const pa_buffer_attr* attr = nullptr) = 0;

            void streamDisconnect(void);

            bool sourceInfo(const std::string & name);

            bool streamPaused(void) const;
            bool streamSuspended(void) const;

            void streamPause(void);
            void streamUnPause(void);

            void streamFlush(void);
            void streamDrain(void);
        };

#ifdef LTSM_CLIENT
        class InputStream : public BaseStream
        {
            std::thread thread;

            std::vector<uint8_t> pcm;
            mutable std::mutex lock;

        protected:
            static void streamWriteCallback(pa_stream* stream, const size_t nbytes, void* userData);
            virtual void streamWriteEvent(const size_t &);

        public:
            InputStream(const pa_sample_format_t &, uint32_t rate, uint8_t channels);
            ~InputStream();


            const char* streamName(void) const override
            {
                return "LTSM Audio Input";
            }

            bool streamConnect(bool paused, const pa_buffer_attr* attr = nullptr) override;

            void streamPlayImmediatly(void);

            void streamWriteData(const uint8_t*, size_t);
            void streamWriteSilent(size_t);

            size_t streamWriteableSize(void) const;
            size_t streamBufferSize(void) const;

            void setLatencyMs(uint32_t ms);
        };

#else
        class OutputStream : public BaseStream
        {
            std::thread thread;
            std::string monitorName;

            std::vector<uint8_t> pcm;
            mutable std::mutex lock;

        protected:
            static void streamReadCallback(pa_stream* stream, const size_t nbytes, void* userData);
            virtual void streamReadEvent(const size_t &);

        public:
            OutputStream(const pa_sample_format_t &, uint32_t rate, uint8_t channels);
            ~OutputStream();

            const char* streamName(void) const override
            {
                return "LTSM Audio Output";
            }

            bool streamConnect(bool paused, const pa_buffer_attr* attr = nullptr) override;

            void setFragSize(uint32_t fragsz);

            bool pcmEmpty(void) const;
            std::vector<uint8_t> pcmData(void);
        };

#endif

#ifdef LTSM_CLIENT
        struct SimpleDeleter
        {
            void operator()(pa_simple* ctx)
            {
                pa_simple_free(ctx);
            }
        };

        class Simple
        {
        protected:
            pa_sample_spec audioSpec = { .format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 2 };
            std::unique_ptr<pa_simple, SimpleDeleter> ctx;

        public:
            Simple() = default;
            virtual ~Simple() = default;

            bool streamFlush(void) const;
            pa_usec_t getLatency(void) const;
        };

        class Playback : public Simple
        {
        public:
            Playback(const std::string & appName, const std::string & streamName, const pa_sample_format_t &,
                     uint32_t rate, uint8_t channels, const pa_buffer_attr* attr = nullptr);

            bool streamWrite(const uint8_t*, size_t) const;
            bool streamDrain(void) const;
        };

        class Record : public Simple
        {
        public:
            Record(const std::string & appName, const std::string & streamName, const pa_sample_format_t &,
                   uint32_t rate, uint8_t channels, const pa_buffer_attr* attr = nullptr);

            std::vector<uint8_t> streamRead(size_t) const;
        };
#endif // LTSM_CLIENT
    }
}

#endif // _LTSM_AUDIO_PULSE_
