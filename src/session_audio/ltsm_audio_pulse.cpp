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

#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <iostream>

#include "pulse/error.h"

#include "ltsm_tools.h"
#include "ltsm_audio.h"
#include "ltsm_audio_pulse.h"
#include "ltsm_application.h"

using namespace std::chrono_literals;

namespace LTSM
{
    /// WaitNotify
    const void* WaitNotify::wait(int id)
    {
        std::unique_lock<std::mutex> lk{ lock };
        cv.wait(lk, [&] { return waitId < 0 || waitId == id; });
        return waitData;
    }

    void WaitNotify::reset(void)
    {
        std::lock_guard<std::mutex> lk{ lock };
        waitId = -1;
        waitData = nullptr;
    }

    void WaitNotify::notify(int id, const void* ptr)
    {
        std::lock_guard<std::mutex> lk{ lock };
        waitId = id;
        waitData = ptr;
        cv.notify_all();
    }

    /// PulseAudio
    namespace PulseAudio
    {
        const size_t pcmReserveSize = 32 * 1024;

        uint16_t formatBits(const pa_sample_format_t & fmt)
        {
            switch(fmt)
            {
                case PA_SAMPLE_S16LE:
                    return 16;

                case PA_SAMPLE_S24LE:
                    return 24;

                case PA_SAMPLE_S32LE:
                    return 32;

                default:
                    break;
            }

            return 0;
        }

        const char* streamStateName(const pa_stream_state_t & st)
        {
            switch(st)
            {
                case PA_STREAM_UNCONNECTED:
                    return "UNCONNECTED";

                case PA_STREAM_CREATING:
                    return "CREATING";

                case PA_STREAM_READY:
                    return "READY";

                case PA_STREAM_FAILED:
                    return "FAILED";

                case PA_STREAM_TERMINATED:
                    return "TERMINATED";

                default:
                    break;
            }

            return "UNKNOWN";
        }

        const char* contextStateName(const pa_context_state_t & st)
        {
            switch(st)
            {
                case PA_CONTEXT_UNCONNECTED:
                    return "UNCONNECTED";

                case PA_CONTEXT_READY:
                    return "READY";

                case PA_CONTEXT_FAILED:
                    return "FAILED";

                case PA_CONTEXT_TERMINATED:
                    return "TERMINATED";

                case PA_CONTEXT_CONNECTING:
                    return "CONNECTING";

                case PA_CONTEXT_AUTHORIZING:
                    return "AUTHORIZING";

                case PA_CONTEXT_SETTING_NAME:
                    return "SETTING_NAME";

                default:
                    break;
            }

            return "UNKNOWN";
        }

        void PulseAudio::BaseStream::contextStateCallback(pa_context* ctx, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->contextStateEvent(pa_context_get_state(ctx));
            }
        }

        void PulseAudio::BaseStream::streamStateCallback(pa_stream* stream, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->streamStateEvent(pa_stream_get_state(stream));
            }
        }

        void PulseAudio::BaseStream::streamSuspendedCallback(pa_stream* stream, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->streamSuspendedEvent(pa_stream_is_suspended(stream));
            }
        }

        // notify
        void PulseAudio::BaseStream::contextServerInfoCallback(pa_context* ctx, const pa_server_info* info, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->contextServerInfoNotify(info);
            }
        }

        void PulseAudio::BaseStream::contextDrainCallback(pa_context* ctx, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->contextDrainNotify();
            }
        }

        void PulseAudio::BaseStream::streamCorkCallback(pa_stream* stream, int success, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->streamCorkNotify(success);
            }
        }

        void PulseAudio::BaseStream::streamTriggerCallback(pa_stream* stream, int success, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->streamTriggerNotify(success);
            }
        }

        void PulseAudio::BaseStream::streamFlushCallback(pa_stream* stream, int success, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->streamFlushNotify(success);
            }
        }

        void PulseAudio::BaseStream::streamDrainCallback(pa_stream* stream, int success, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->streamDrainNotify(success);
            }
        }

        void PulseAudio::BaseStream::streamOverflowCallback(pa_stream* stream, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->streamOverflowEvent();
            }
        }

        void PulseAudio::BaseStream::streamUnderflowCallback(pa_stream* stream, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<BaseStream*>(userData))
            {
                pulseAudio->streamUnderflowEvent(pa_stream_get_underflow_index(stream));
            }
        }

#ifdef LTSM_CLIENT
        void PulseAudio::InputStream::streamWriteCallback(pa_stream* stream, const size_t nbytes, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<InputStream*>(userData))
            {
                pulseAudio->streamWriteEvent(nbytes);
            }
        }

#else
        void PulseAudio::OutputStream::streamReadCallback(pa_stream* stream, const size_t nbytes, void* userData)
        {
            Application::debug("%s", __FUNCTION__);

            if(auto pulseAudio = static_cast<OutputStream*>(userData))
            {
                pulseAudio->streamReadEvent(nbytes);
            }
        }

#endif
    }

    // BaseStream
    PulseAudio::BaseStream::BaseStream(std::string_view contextName, const pa_sample_format_t & fmt, uint32_t rate,
                                       uint8_t channels)
    {
        audioSpec.format = fmt;
        audioSpec.rate = rate;
        audioSpec.channels = channels;

        if(0 == pa_sample_spec_valid(& audioSpec))
        {
            Application::error("%s: %s failed, format: `%s', rate: %" PRIu32 ", channels: %" PRIu8,
                               __FUNCTION__, "pa_sample_spec_valid", pa_sample_format_to_string(audioSpec.format), audioSpec.rate, audioSpec.channels);
            throw audio_error(NS_FuncName);
        }

        loop.reset(pa_mainloop_new());

        if(! loop)
        {
            Application::error("%s: %s failed", __FUNCTION__, "pa_mainloop_new");
            throw audio_error(NS_FuncName);
        }

        pa_mainloop_api* api = pa_mainloop_get_api(loop.get());

        if(! api)
        {
            Application::error("%s: %s failed", __FUNCTION__, "pa_mainloop_get_api");
            throw audio_error(NS_FuncName);
        }

        ctx.reset(pa_context_new(api, contextName.data()));

        if(! ctx)
        {
            Application::error("%s: %s failed", __FUNCTION__, "pa_context_new");
            throw audio_error(NS_FuncName);
        }
    }

    PulseAudio::BaseStream::~BaseStream()
    {
        waitNotify.reset();
    }

    void PulseAudio::BaseStream::contextDisconnect(void)
    {
        if(pa_context_get_state(ctx.get()) != PA_CONTEXT_UNCONNECTED)
        {
            Application::debug("%s", __FUNCTION__);
            pa_context_disconnect(ctx.get());
        }

        pa_context_set_state_callback(ctx.get(), nullptr, nullptr);
        pa_context_set_event_callback(ctx.get(), nullptr, nullptr);
    }

    void PulseAudio::BaseStream::streamDisconnect(void)
    {
        if(! stream)
        {
            return;
        }

        if(pa_stream_get_state(stream.get()) != PA_STREAM_UNCONNECTED)
        {
            Application::info("%s", __FUNCTION__);
            pa_stream_drop(stream.get());
            pa_stream_disconnect(stream.get());
        }

        pa_stream_set_suspended_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_overflow_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_underflow_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_state_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_write_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_read_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_started_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_latency_update_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_moved_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_suspended_callback(stream.get(), nullptr, nullptr);
        pa_stream_set_event_callback(stream.get(), nullptr, nullptr);
    }

    bool PulseAudio::BaseStream::initContext(void)
    {
        pa_context_set_state_callback(ctx.get(), & contextStateCallback, this);

        if(0 > pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr))
        {
            Application::warning("%s: %s failed", __FUNCTION__, "pa_context_connect");
            return false;
        }

        while(contextState != PA_CONTEXT_READY)
        {
            if(PA_CONTEXT_FAILED == contextState)
            {
                Application::error("%s: %s failed", __FUNCTION__, "context state");
                return false;
            }

            std::this_thread::sleep_for(50ms);
        }

        serverInfo = contextServerInfoWait();

        if(! serverInfo)
        {
            Application::error("%s: %s failed", __FUNCTION__, "server info");
            return false;
        }

        Application::info("%s: server version: %s", __FUNCTION__, serverInfo->server_version);
        // init stream
        stream.reset(pa_stream_new(ctx.get(), streamName(), & audioSpec, nullptr));

        if(! stream)
        {
            Application::error("%s: %s failed", __FUNCTION__, "pa_stream_new");
            return false;
        }

        pa_stream_set_state_callback(stream.get(), & streamStateCallback, this);
        pa_stream_set_suspended_callback(stream.get(), & streamSuspendedCallback, this);
        pa_stream_set_overflow_callback(stream.get(), & streamOverflowCallback, this);
        pa_stream_set_underflow_callback(stream.get(), & streamUnderflowCallback, this);
        return true;
    }

    void PulseAudio::BaseStream::contextServerInfoNotify(const pa_server_info* info)
    {
        Application::debug("%s", __FUNCTION__);
        waitNotify.notify(WaitOp::ContextServerInfo, info);
    }

    const pa_server_info* PulseAudio::BaseStream::contextServerInfoWait(void)
    {
        Application::debug("%s", __FUNCTION__);

        if(auto op = pa_context_get_server_info(ctx.get(), & contextServerInfoCallback, this))
        {
            auto ret = waitNotify.wait(WaitOp::ContextServerInfo);
            pa_operation_unref(op);
            return (const pa_server_info*) ret;
        }

        return nullptr;
    }

    void PulseAudio::BaseStream::contextDrainNotify(void)
    {
        Application::debug("%s", __FUNCTION__);
        waitNotify.notify(WaitOp::ContextDrain, nullptr);
    }

    void PulseAudio::BaseStream::contextDrainWait(void)
    {
        Application::debug("%s", __FUNCTION__);

        if(auto op = pa_context_drain(ctx.get(), & contextDrainCallback, this))
        {
            waitNotify.wait(WaitOp::ContextDrain);
            pa_operation_unref(op);
        }
    }

    void PulseAudio::BaseStream::streamCorkNotify(int success)
    {
        Application::debug("%s: success: %d", __FUNCTION__, success);
        waitNotify.notify(WaitOp::StreamCork, success ? & success : nullptr);
    }

    bool PulseAudio::BaseStream::streamCorkWait(bool pause)
    {
        Application::debug("%s: pause %s", __FUNCTION__, (pause ? "true" : "false"));

        if(pa_stream_is_corked(stream.get()) == pause)
        {
            return true;
        }

        if(auto op = pa_stream_cork(stream.get(), pause ? 1 : 0, & streamCorkCallback, this))
        {
            auto ret = waitNotify.wait(WaitOp::StreamCork);
            pa_operation_unref(op);
            return ret;
        }

        return false;
    }

    void PulseAudio::BaseStream::streamTriggerNotify(int success)
    {
        Application::debug("%s: success: %d", __FUNCTION__, success);
        waitNotify.notify(WaitOp::StreamTrigger, success ? & success : nullptr);
    }

    bool PulseAudio::BaseStream::streamTriggerWait(void)
    {
        Application::debug("%s", __FUNCTION__);

        if(auto op = pa_stream_trigger(stream.get(), & streamTriggerCallback, this))
        {
            auto ret = waitNotify.wait(WaitOp::StreamTrigger);
            pa_operation_unref(op);
            return ret;
        }

        return false;
    }

    void PulseAudio::BaseStream::streamFlushNotify(int success)
    {
        Application::debug("%s: success: %d", __FUNCTION__, success);
        waitNotify.notify(WaitOp::StreamFlush, success ? & success : nullptr);
    }

    bool PulseAudio::BaseStream::streamFlushWait(void)
    {
        Application::debug("%s", __FUNCTION__);

        if(auto op = pa_stream_flush(stream.get(), & streamFlushCallback, this))
        {
            auto ret = waitNotify.wait(WaitOp::StreamFlush);
            pa_operation_unref(op);
            return ret;
        }

        return false;
    }

    void PulseAudio::BaseStream::streamDrainNotify(int success)
    {
        Application::debug("%s: success: %d", __FUNCTION__, success);
        waitNotify.notify(WaitOp::StreamDrain, success ? & success : nullptr);
    }

    bool PulseAudio::BaseStream::streamDrainWait(void)
    {
        Application::debug("%s", __FUNCTION__);

        if(auto op = pa_stream_drain(stream.get(), & streamDrainCallback, this))
        {
            auto ret = waitNotify.wait(WaitOp::StreamDrain);
            pa_operation_unref(op);
            return ret;
        }

        return false;
    }

    void PulseAudio::BaseStream::contextStateEvent(const pa_context_state_t & state)
    {
        if(PA_CONTEXT_FAILED == state)
        {
            Application::error("%s: state: %s", __FUNCTION__, contextStateName(state));
        }
        else
        {
            Application::info("%s: state: %s", __FUNCTION__, contextStateName(state));
        }

        contextState = state;
    }

    void PulseAudio::BaseStream::streamStateEvent(const pa_stream_state_t & state)
    {
        Application::info("%s: state: %s", __FUNCTION__, streamStateName(state));
        streamState = state;
    }

    void PulseAudio::BaseStream::streamSuspendedEvent(int state)
    {
        Application::info("%s: state: %d", __FUNCTION__, state);
    }

    void PulseAudio::BaseStream::streamOverflowEvent(void)
    {
        Application::info("%s: ", __FUNCTION__);
    }

    void PulseAudio::BaseStream::streamUnderflowEvent(int64_t index)
    {
        Application::info("%s: index: %" PRId64, __FUNCTION__, index);
    }

    bool PulseAudio::BaseStream::streamSuspended(void) const
    {
        return 1 == pa_stream_is_suspended(stream.get());
    }

    bool PulseAudio::BaseStream::streamPaused(void) const
    {
        return 1 == pa_stream_is_corked(stream.get());
    }

    void PulseAudio::BaseStream::streamPause(void)
    {
        streamCorkWait(true);
    }

    void PulseAudio::BaseStream::streamUnPause(void)
    {
        streamCorkWait(false);
    }

    void PulseAudio::BaseStream::streamDrain(void)
    {
        streamDrainWait();
    }

    void PulseAudio::BaseStream::streamFlush(void)
    {
        streamFlushWait();
    }

#ifdef LTSM_CLIENT
    PulseAudio::InputStream::InputStream(const pa_sample_format_t & fmt, uint32_t rate, uint8_t channels)
        : BaseStream("ltsm_client", fmt, rate, channels)
    {
        pcm.reserve(pcmReserveSize);
        // loop
        thread = std::thread([loop = loop.get()]()
        {
            std::this_thread::sleep_for(5ms);
            pa_mainloop_run(loop, nullptr);
        });
    }

    PulseAudio::InputStream::~InputStream()
    {
        streamDisconnect();
        contextDisconnect();

        if(loop)
        {
            pa_mainloop_quit(loop.get(), 0);
        }

        if(thread.joinable())
        {
            thread.join();
        }
    }

    /*
        ///  pa_buffer_attr
        @details
            maxLength - Абсолютное максимальное количество байт, которое может быть сохранено в буфере.
                Если это значение будет превышено, данные будут потеряны. Здесь рекомендуется передать (uint32_t) -1, что приведет к заполнению сервером максимально возможного значения.

            tLength - целевой уровень заполнения буфера воспроизведения. Сервер будет отправлять запросы на дополнительные данные только до тех пор, пока в буфере будет меньше этого количества байт данных.
                Если вы передадите здесь значение (uint32_t) -1 (что рекомендуется), сервер выберет максимально возможный уровень заполнения целевого буфера, чтобы минимизировать количество необходимых пробуждений
                и максимально повысить безопасность выпадения. Это может превышать 2 секунды буферизации. Для приложений с низкой задержкой или приложений, где задержка имеет значение, вы должны указать здесь правильное значение.

            preBuf - количество байт, которые должны быть в буфере перед началом воспроизведения. Запуск воспроизведения можно принудительно запустить с помощью pa_stream_trigger(), даже если размер предварительного буфера не достигнут.
                Если произойдет переполнение буфера, эта предварительная буферизация будет снова включена. Если воспроизведение никогда не должно прекращаться в случае переполнения буфера, это значение должно быть установлено равным 0.
                В этом случае индекс чтения выходного буфера превышает индекс записи, и, следовательно, уровень заполнения буфера отрицательный. Если вы передадите (uint32_t) -1 здесь (что рекомендуется), сервер выберет то же значение,
                что и tlength здесь.

            minReq - минимальное количество свободных байт в буфере воспроизведения, прежде чем сервер запросит дополнительные данные. Здесь рекомендуется заполнить (uint32_t) -1.
                Это значение влияет на то, сколько времени требуется звуковому серверу для перемещения данных из буфера воспроизведения на стороне сервера для каждого потока в буфер аппаратного воспроизведения.

            fragSize - максимальное количество байт, которое сервер будет помещать в один блок для потоков записей. Если выпередадите здесь значение (uint32_t) -1 (что рекомендуется),
                сервер выберет максимально возможную настройку фрагмента, чтобы минимизировать количество необходимых пробуждений и максимально повысить безопасность выпадения.
                Это может превышать 2 секунды буферизации. Для приложений с низкой задержкой или приложений, где задержка имеет значение, вы должны указать здесь правильное значение.
    */
    void PulseAudio::InputStream::setLatencyMs(uint32_t ms)
    {
        pa_buffer_attr bufferAttr = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
        const pa_usec_t latency = 1000 * ms;
        bufferAttr.maxlength = pa_usec_to_bytes(latency, & audioSpec);
        bufferAttr.tlength = pa_usec_to_bytes(latency, & audioSpec);
        bufferAttr.minreq = pa_usec_to_bytes(0, & audioSpec);
        Application::debug("%s: latency: %" PRIu32 "ms, buffer max length: %" PRIu32 ", target length: %" PRIu32,
                           __FUNCTION__, ms, bufferAttr.maxlength, bufferAttr.tlength);

        if(auto op = pa_stream_set_buffer_attr(stream.get(), & bufferAttr, nullptr, nullptr))
        {
            pa_operation_unref(op);
        }
    }

    bool PulseAudio::InputStream::streamConnect(bool paused, const pa_buffer_attr* attr)
    {
        pa_stream_set_write_callback(stream.get(), & streamWriteCallback, this);
        Application::info("%s: connect to: `%s'", __FUNCTION__, serverInfo->default_sink_name);
        const pa_buffer_attr bufferAttr = { .maxlength = UINT32_MAX, .tlength = 2048, .prebuf = UINT32_MAX, .minreq = UINT32_MAX, .fragsize = UINT32_MAX };

        if(0 != pa_stream_connect_playback(stream.get(), serverInfo->default_sink_name, attr ? attr : & bufferAttr,
                                           pa_stream_flags_t(PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE), nullptr,
                                           nullptr))
        {
            // old pulse audio servers don't like the ADJUST_LATENCY flag
            if(0 != pa_stream_connect_playback(stream.get(), serverInfo->default_sink_name, attr ? attr : & bufferAttr,
                                               pa_stream_flags_t(PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE), nullptr, nullptr))
            {
                Application::error("%s: %s failed", __FUNCTION__, "pa_stream_connect_playback");
                return false;
            }
        }

        while(PA_STREAM_READY != streamState)
        {
            if(PA_STREAM_FAILED == streamState)
            {
                Application::error("%s: %s failed", __FUNCTION__, "stream state");
                return false;
            }

            std::this_thread::sleep_for(50ms);
        }

        if(paused)
        {
            if(auto op = pa_stream_cork(stream.get(), 1, nullptr, nullptr))
            {
                pa_operation_unref(op);
            }
        }

        return true;
    }

    void PulseAudio::InputStream::streamWriteSilent(size_t len)
    {
        Application::debug("%s: data size: %u", __FUNCTION__, len);
        std::vector<uint8_t> buf(len, 0);
        streamWriteData(buf.data(), buf.size());
    }

    size_t PulseAudio::InputStream::streamWriteableSize(void) const
    {
        return pa_stream_writable_size(stream.get());
    }

    void PulseAudio::InputStream::streamWriteData(const uint8_t* ptr, size_t len)
    {
        Application::info("%s: data size: %u", __FUNCTION__, len);
        std::scoped_lock guard{ lock };
        pcm.insert(pcm.end(), ptr, ptr + len);
        auto writableSize = pa_stream_writable_size(stream.get());

        if((writableSize << 2) < pcm.size())
        {
            // unpaused
            if(auto op = pa_stream_cork(stream.get(), 0, nullptr, nullptr))
            {
                pa_operation_unref(op);
            }

            // triggered
            if(auto op = pa_stream_trigger(stream.get(), nullptr, nullptr))
            {
                pa_operation_unref(op);
            }
        }
    }

    void PulseAudio::InputStream::streamWriteEvent(const size_t & nbytes)
    {
        pa_usec_t usec = 0;
        int neg = 0;

        if(0 != pa_stream_get_latency(stream.get(), & usec, & neg))
        {
            Application::warning("%s: %s failed", __FUNCTION__, "pa_stream_get_latency");
        }

        std::scoped_lock guard{ lock };

        if(pcm.empty())
        {
            if(! streamPaused())
            {
                if(auto op = pa_stream_cork(stream.get(), 1, nullptr, nullptr))
                {
                    pa_operation_unref(op);
                }
            }
        }
        else if(auto len = std::min(nbytes, pcm.size()))
        {
            Application::info("%s: request: %u, last: %u, write: %u, latency: %8d, neg: %d", __FUNCTION__, nbytes, pcm.size(), len,
                              usec, neg);

            if(0 != pa_stream_write(stream.get(), pcm.data(), len, nullptr, 0, PA_SEEK_RELATIVE))
            {
                Application::error("%s: %s failed", __FUNCTION__, "pa_stream_write");
                throw audio_error(NS_FuncName);
            }

            pcm.erase(pcm.begin(), pcm.begin() + len);
        }
    }

    size_t PulseAudio::InputStream::streamBufferSize(void) const
    {
        std::scoped_lock guard{ lock };
        return pcm.size();
    }

    void PulseAudio::InputStream::streamPlayImmediatly(void)
    {
        streamTriggerWait();
    }

#else

    /// PulseAudio::OutputStream
    PulseAudio::OutputStream::OutputStream(const pa_sample_format_t & fmt, uint32_t rate, uint8_t channels)
        : BaseStream("ltsm_audio_session", fmt, rate, channels)
    {
        pcm.reserve(pcmReserveSize);
        // loop
        thread = std::thread([loop = loop.get()]()
        {
            std::this_thread::sleep_for(5ms);
            pa_mainloop_run(loop, nullptr);
        });
    }

    PulseAudio::OutputStream::~OutputStream()
    {
        streamDisconnect();
        contextDisconnect();

        if(loop)
        {
            pa_mainloop_quit(loop.get(), 0);
        }

        if(thread.joinable())
        {
            thread.join();
        }
    }

    bool PulseAudio::OutputStream::streamConnect(bool paused, const pa_buffer_attr* attr)
    {
        pa_stream_set_read_callback(stream.get(), & streamReadCallback, this);
        monitorName = std::string(serverInfo->default_sink_name).append(".monitor");
        Application::info("%s: connect to: `%s'", __FUNCTION__, serverInfo->default_sink_name);
        const uint32_t fragsz = 1024;
        const pa_buffer_attr bufferAttr = { .maxlength = fragsz, .tlength = UINT32_MAX, .prebuf = UINT32_MAX, .minreq = UINT32_MAX, .fragsize = fragsz };

        if(0 != pa_stream_connect_record(stream.get(), monitorName.c_str(), attr ? attr : & bufferAttr,
                                         PA_STREAM_ADJUST_LATENCY))
        {
            Application::error("%s: %s failed", __FUNCTION__, "pa_stream_connect_record");
            return false;
        }

        while(PA_STREAM_READY != streamState)
        {
            if(PA_STREAM_FAILED == streamState)
            {
                Application::error("%s: %s failed", __FUNCTION__, "stream state");
                return false;
            }

            std::this_thread::sleep_for(50ms);
        }

        if(paused)
        {
            if(auto op = pa_stream_cork(stream.get(), 1, nullptr, nullptr))
            {
                pa_operation_unref(op);
            }
        }

        return true;
    }

    void PulseAudio::OutputStream::setFragSize(uint32_t fragsize)
    {
        const pa_buffer_attr bufferAttr = { fragsize, UINT32_MAX, UINT32_MAX, UINT32_MAX, fragsize };

        if(auto op = pa_stream_set_buffer_attr(stream.get(), & bufferAttr, nullptr, nullptr))
        {
            pa_operation_unref(op);
        }
    }

    void PulseAudio::OutputStream::streamReadEvent(const size_t & nbytes)
    {
        Application::debug("%s: bytes: %u", __FUNCTION__, nbytes);
        const uint8_t* streamData = nullptr;
        size_t streamBytes = 0;

        if(0 == pa_stream_peek(stream.get(), (const void**) & streamData, & streamBytes))
        {
            std::scoped_lock guard{ lock };

            if(pcm.capacity() < pcmReserveSize)
            {
                pcm.reserve(pcmReserveSize);
            }

            if(pcm.size() + streamBytes > pcmReserveSize)
            {
                Application::warning("%s: pcm overflow, size: %u, block: %u, limit: %u", __FUNCTION__, pcm.size(), streamBytes,
                                     pcmReserveSize);
                pcm.assign(streamData, streamData + streamBytes);
            }
            else
            {
                pcm.insert(pcm.end(), streamData, streamData + streamBytes);
            }

            if(streamBytes)
            {
                pa_stream_drop(stream.get());
            }
        }
        else
        {
            Application::error("%s: %s failed", __FUNCTION__, "pa_stream_peek");
        }
    }

    bool PulseAudio::OutputStream::pcmEmpty(void) const
    {
        std::scoped_lock guard{ lock };
        return pcm.empty();
    }

    std::vector<uint8_t> PulseAudio::OutputStream::pcmData(void)
    {
        std::scoped_lock guard{ lock };
        return std::move(pcm);
    }

#endif

    // PulseAudio::Simple
    bool PulseAudio::Simple::streamFlush(void) const
    {
        int error = 0;

        if(0 != pa_simple_flush(ctx.get(), & error))
        {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_flush", pa_strerror(error));
            return false;
        }

        return true;
    }

    pa_usec_t PulseAudio::Simple::getLatency(void) const
    {
        int error = 0;
        auto res = pa_simple_get_latency(ctx.get(), & error);

        if(error)
        {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_get_latency", pa_strerror(error));
            return 0;
        }

        return res;
    }

    PulseAudio::Playback::Playback(std::string_view appName, std::string_view streamName,
                                   const pa_sample_format_t & fmt, uint32_t rate, uint8_t channels, const pa_buffer_attr* attr)
    {
        audioSpec.format = fmt;
        audioSpec.rate = rate;
        audioSpec.channels = channels;

        if(0 == pa_sample_spec_valid(& audioSpec))
        {
            Application::error("%s: %s failed, format: `%s', rate: %" PRIu32 ", channels: %" PRIu8,
                               __FUNCTION__, "pa_sample_spec_valid", pa_sample_format_to_string(audioSpec.format), audioSpec.rate, audioSpec.channels);
            throw audio_error(NS_FuncName);
        }

        int error = 0;
        ctx.reset(pa_simple_new(nullptr, appName.data(), PA_STREAM_PLAYBACK,
                                nullptr, streamName.data(), & audioSpec, nullptr, attr, & error));

        if(! ctx)
        {
            Application::error("%s: %s failed, error: `%s'", __FUNCTION__, "pa_simple_new", pa_strerror(error));
            throw audio_error(NS_FuncName);
        }
    }

    bool PulseAudio::Playback::streamWrite(const uint8_t* ptr, size_t len) const
    {
        int error = 0;

        if(0 != pa_simple_write(ctx.get(), ptr, len, & error))
        {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_write", pa_strerror(error));
            return false;
        }

        return true;
    }

    bool PulseAudio::Playback::streamDrain(void) const
    {
        int error = 0;

        if(0 != pa_simple_drain(ctx.get(), & error))
        {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_drain", pa_strerror(error));
            return false;
        }

        return true;
    }

    PulseAudio::Record::Record(std::string_view appName, std::string_view streamName,
                               const pa_sample_format_t & fmt, uint32_t rate, uint8_t channels, const pa_buffer_attr* attr)
    {
        audioSpec.format = fmt;
        audioSpec.rate = rate;
        audioSpec.channels = channels;

        if(0 == pa_sample_spec_valid(& audioSpec))
        {
            Application::error("%s: %s failed, format: `%s', rate: %" PRIu32 ", channels: %" PRIu8,
                               __FUNCTION__, "pa_sample_spec_valid", pa_sample_format_to_string(audioSpec.format), audioSpec.rate, audioSpec.channels);
            throw audio_error(NS_FuncName);
        }

        int error = 0;
        ctx.reset(pa_simple_new(nullptr, appName.data(), PA_STREAM_RECORD,
                                nullptr, streamName.data(), & audioSpec, nullptr, attr, & error));

        if(! ctx)
        {
            Application::error("%s: %s failed, error: `%s'", __FUNCTION__, "pa_simple_new", pa_strerror(error));
            throw audio_error(NS_FuncName);
        }
    }

    std::vector<uint8_t> PulseAudio::Record::streamRead(size_t len) const
    {
        int error = 0;
        std::vector<uint8_t> buf(len, 0);

        if(0 != pa_simple_read(ctx.get(), buf.data(), buf.size(), & error))
        {
            Application::error("%s: %s failed, error: `%d'", __FUNCTION__, "pa_simple_read", pa_strerror(error));
            return {};
        }

        return buf;
    }
}
