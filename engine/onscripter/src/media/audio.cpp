/* -*- C++ -*-
 *
 *  audio.cpp - FFmpeg decoder + software mixer + SDL3 playback device
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  ---------------------------------------------------------------------
 *  Decoding happens on the calling thread (whole file -> S16 stereo 44100).
 *  The SDL3 device callback only sums pre-decoded buffers; no allocation,
 *  no file I/O and no decoder work happens there.  When no audio device can
 *  be opened an internal tick thread drives the same mixer at real time so
 *  the WAV dump stays usable in headless runs.
 *  ---------------------------------------------------------------------
 */

#include "audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(ONS_HAVE_FFMPEG) && ONS_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace ons {

namespace {

const int kOutRate = 44100;
const int kOutChannels = 2;
const int kOutFrameBytes = kOutChannels * (int)sizeof(int16_t);
const int kMaxMixFrames = 8192;
const int kMaxDumpSeconds = 600;
/* Output headroom: summed buses can exceed full scale (BGM + voice + SE).
 * Scaling before the clamp keeps the mix below 0 dBFS instead of clipping. */
const float kMixHeadroom = 0.8f;
/* Guard against pathological files: 10 min of decoded PCM at most. */
const size_t kMaxDecodedSamples =
    (size_t)kOutRate * (size_t)kOutChannels * 60u * 10u;

inline float clamp01(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

void wav_put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

void wav_put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

/* Writes/refreshes the canonical 44-byte PCM WAV header. */
void wav_write_header(FILE *f, uint64_t data_bytes)
{
    unsigned char h[44];
    if (data_bytes > 0xffffffffull - 36ull)
        data_bytes = 0xffffffffull - 36ull;
    memcpy(h, "RIFF", 4);
    wav_put32(h + 4, (uint32_t)(36u + data_bytes));
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    wav_put32(h + 16, 16);
    wav_put16(h + 20, 1); /* PCM */
    wav_put16(h + 22, (uint16_t)kOutChannels);
    wav_put32(h + 24, (uint32_t)kOutRate);
    wav_put32(h + 28, (uint32_t)(kOutRate * kOutFrameBytes));
    wav_put16(h + 32, (uint16_t)kOutFrameBytes);
    wav_put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    wav_put32(h + 40, (uint32_t)data_bytes);
    if (fseek(f, 0, SEEK_SET) == 0)
        fwrite(h, 1, sizeof(h), f);
    fflush(f);
}

#if defined(ONS_HAVE_FFMPEG) && ONS_HAVE_FFMPEG

struct MemReader {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

int mem_read_cb(void *opaque, uint8_t *buf, int size)
{
    MemReader *r = static_cast<MemReader *>(opaque);
    if (r->pos >= r->len)
        return AVERROR_EOF;
    const size_t n = std::min((size_t)size, r->len - r->pos);
    memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    return (int)n;
}

int64_t mem_seek_cb(void *opaque, int64_t offset, int whence)
{
    MemReader *r = static_cast<MemReader *>(opaque);
    if (whence == AVSEEK_SIZE)
        return (int64_t)r->len;
    int64_t base = 0;
    if (whence == SEEK_CUR)
        base = (int64_t)r->pos;
    else if (whence == SEEK_END)
        base = (int64_t)r->len;
    const int64_t np = base + offset;
    if (np < 0 || (uint64_t)np > r->len)
        return -1;
    r->pos = (size_t)np;
    return np;
}

/*
 * Whole-file decoder: demuxes from memory, decodes with libavcodec and
 * resamples through libswresample to interleaved S16 stereo 44100 Hz.
 */
struct MemoryAudioDecoder {
    MemReader io = {nullptr, 0, 0};
    AVIOContext *avio = nullptr;
    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    SwrContext *swr = nullptr;
    int astream = -1;

    ~MemoryAudioDecoder()
    {
        if (swr)
            swr_free(&swr);
        if (frame)
            av_frame_free(&frame);
        if (pkt)
            av_packet_free(&pkt);
        if (dec)
            avcodec_free_context(&dec);
        if (fmt)
            avformat_close_input(&fmt);
        if (avio) {
            /* avio_alloc_context() takes ownership of the caller's buffer
             * only for reads; avio_context_free() does not release it, so
             * the 64 KiB demux buffer has to be freed here.  Leaking it
             * cost one block per sound effect / BGM decode. */
            av_freep(reinterpret_cast<void **>(&avio->buffer));
            avio_context_free(&avio);
        }
    }

    bool open(const uint8_t *data, size_t len)
    {
        static std::once_flag log_once;
        std::call_once(log_once, [] { av_log_set_level(AV_LOG_ERROR); });

        io.data = data;
        io.len = len;
        io.pos = 0;

        constexpr int kIoBufSize = 1 << 16;
        unsigned char *iobuf =
            static_cast<unsigned char *>(av_malloc(kIoBufSize));
        if (!iobuf)
            return false;
        avio = avio_alloc_context(iobuf, kIoBufSize, 0, &io, mem_read_cb,
                                  nullptr, mem_seek_cb);
        if (!avio) {
            av_free(iobuf);
            return false;
        }

        fmt = avformat_alloc_context();
        if (!fmt)
            return false;
        fmt->pb = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0)
            return false; /* fmt is freed by avformat_open_input */
        if (avformat_find_stream_info(fmt, nullptr) < 0)
            return false;

        const AVCodec *codec = nullptr;
        astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (astream < 0 || !codec)
            return false;

        AVStream *st = fmt->streams[astream];
        if (!st->codecpar)
            return false;
        dec = avcodec_alloc_context3(codec);
        if (!dec)
            return false;
        if (avcodec_parameters_to_context(dec, st->codecpar) < 0)
            return false;
        dec->thread_count = 1; /* deterministic, in-order frames */
        if (avcodec_open2(dec, codec, nullptr) < 0)
            return false;
        if (dec->sample_rate <= 0 || dec->sample_fmt == AV_SAMPLE_FMT_NONE)
            return false;

        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!frame || !pkt)
            return false;

        AVChannelLayout in_layout;
        memset(&in_layout, 0, sizeof(in_layout));
        if (dec->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&in_layout, &dec->ch_layout);
        else
            av_channel_layout_default(&in_layout, 1);
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        const int rc = swr_alloc_set_opts2(
            &swr, &out_layout, AV_SAMPLE_FMT_S16, kOutRate, &in_layout,
            dec->sample_fmt, dec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        if (rc < 0 || !swr)
            return false;
        if (swr_init(swr) < 0)
            return false;
        return true;
    }

    bool convert_frame(const AVFrame *f, std::vector<int16_t> &out,
                       std::vector<int16_t> &scratch)
    {
        int out_max = (int)av_rescale_rnd(
            swr_get_delay(swr, dec->sample_rate) + f->nb_samples, kOutRate,
            dec->sample_rate, AV_ROUND_UP);
        if (out_max <= 0)
            return true;
        if (out.size() + (size_t)out_max * 2u > kMaxDecodedSamples)
            return false;

        scratch.resize((size_t)out_max * 2u);
        uint8_t *dst = reinterpret_cast<uint8_t *>(scratch.data());
        const int got = swr_convert(swr, &dst, out_max,
                                    (const uint8_t **)f->extended_data,
                                    f->nb_samples);
        if (got > 0)
            out.insert(out.end(), scratch.begin(),
                       scratch.begin() + (size_t)got * 2u);
        return true;
    }

    bool decode_all(std::vector<int16_t> &out)
    {
        /* Reserve from the container duration so the decoded buffer is not
         * grown by repeated reallocation.  Vector doubling kept almost twice
         * the PCM size per track alive, which is what pushed the process
         * working set up every time a new BGM was started.  Half a second of
         * headroom covers decoder priming/padding, which otherwise makes the
         * last insert reallocate and double the capacity again. */
        if (fmt && fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE) {
            const double seconds = (double)fmt->duration / (double)AV_TIME_BASE;
            size_t samples = (size_t)(seconds * (double)kOutRate) *
                             (size_t)kOutChannels;
            samples += (size_t)kOutRate * (size_t)kOutChannels / 2u;
            if (samples > kMaxDecodedSamples)
                samples = kMaxDecodedSamples;
            if (samples > out.size())
                out.reserve(samples);
        }

        std::vector<int16_t> scratch;
        for (;;) {
            const int rr = av_read_frame(fmt, pkt);
            if (rr == AVERROR_EOF)
                break;
            if (rr < 0)
                break; /* tolerate a truncated tail: keep what we decoded */
            if (pkt->stream_index == astream) {
                if (avcodec_send_packet(dec, pkt) == 0) {
                    for (;;) {
                        const int gr = avcodec_receive_frame(dec, frame);
                        if (gr == AVERROR(EAGAIN) || gr == AVERROR_EOF)
                            break;
                        if (gr < 0) {
                            av_packet_unref(pkt);
                            return !out.empty();
                        }
                        const bool ok = convert_frame(frame, out, scratch);
                        av_frame_unref(frame);
                        if (!ok) {
                            av_packet_unref(pkt);
                            return !out.empty();
                        }
                    }
                }
            }
            av_packet_unref(pkt);
        }

        /* Drain the decoder. */
        if (avcodec_send_packet(dec, nullptr) >= 0) {
            for (;;) {
                const int gr = avcodec_receive_frame(dec, frame);
                if (gr == AVERROR(EAGAIN) || gr == AVERROR_EOF)
                    break;
                if (gr < 0)
                    break;
                convert_frame(frame, out, scratch);
                av_frame_unref(frame);
            }
        }

        /* Drain the resampler. */
        for (;;) {
            scratch.resize(2048 * 2);
            uint8_t *dst = reinterpret_cast<uint8_t *>(scratch.data());
            const int got = swr_convert(swr, &dst, 2048, nullptr, 0);
            if (got <= 0)
                break;
            out.insert(out.end(), scratch.begin(),
                       scratch.begin() + (size_t)got * 2u);
        }
        return !out.empty();
    }
};

#endif /* ONS_HAVE_FFMPEG */

bool decode_audio_memory(const uint8_t *data, size_t len,
                         std::vector<int16_t> &out)
{
#if defined(ONS_HAVE_FFMPEG) && ONS_HAVE_FFMPEG
    if (!data || len < 16)
        return false;
    MemoryAudioDecoder decoder;
    if (!decoder.open(data, len))
        return false;
    return decoder.decode_all(out);
#else
    (void)data;
    (void)len;
    (void)out;
    return false;
#endif
}

} // namespace

struct AudioEngine::Impl {
    struct Channel {
        std::vector<int16_t> pcm; /* interleaved stereo */
        size_t pos = 0;           /* in samples, not frames */
        bool playing = false;
        bool loop = false;
        bool preloaded = false;
        float volume = 1.0f;
        float pan = 0.0f;
        int fade_total = 0; /* in frames */
        int fade_left = 0;
    };

    mutable std::mutex mu;
    bool open = false;
    int freq = kOutRate;
    int buffer_ms = 46;
    float master = 1.0f;
    bool muted = false;

    Channel se[kMaxChannels];
    Channel bgm;
    bool bgm_paused = false;

    MovieAudioSource *movie = nullptr;

    SDL_AudioStream *stream = nullptr;
    std::thread tick;
    std::atomic<bool> running{false};

    std::vector<int16_t> scratch;   /* mixer output, preallocated */
    std::vector<int32_t> accum;     /* int32 accumulation */
    std::vector<int16_t> movie_buf; /* movie source scratch */

    FILE *dump = nullptr;
    uint64_t dump_bytes = 0;
    uint64_t dump_limit = 0;

    uint64_t mixed_frames = 0;
    int peak_abs = 0;

    std::function<void(int)> se_done;
    std::function<void()> bgm_done;

    void allocate_scratch()
    {
        scratch.resize((size_t)kMaxMixFrames * 2u);
        accum.resize((size_t)kMaxMixFrames * 2u);
        movie_buf.resize((size_t)kMaxMixFrames * 2u);
    }

    static void SDLCALL stream_callback(void *userdata, SDL_AudioStream *s,
                                        int additional, int /*total*/)
    {
        static_cast<Impl *>(userdata)->pump(s, additional);
    }

    void pump(SDL_AudioStream *s, int additional_bytes)
    {
        int frames = additional_bytes / kOutFrameBytes;
        while (frames > 0) {
            const int n = std::min(frames, kMaxMixFrames);
            mix(scratch.data(), n, true);
            SDL_PutAudioStreamData(s, scratch.data(), n * kOutFrameBytes);
            frames -= n;
        }
    }

    void tick_loop(int frames, int interval_ms)
    {
        std::vector<int16_t> buf((size_t)std::min(frames, kMaxMixFrames) * 2u);
        auto next = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_relaxed)) {
            next += std::chrono::milliseconds(interval_ms);
            std::this_thread::sleep_until(next);
            int left = frames;
            while (left > 0 && running.load(std::memory_order_relaxed)) {
                const int n = std::min(left, kMaxMixFrames);
                mix(buf.data(), n, true);
                left -= n;
            }
        }
    }

    void mix_channel(Channel &c, int32_t *acc, int frames, bool is_bgm,
                     int channel_index, int *done, int &ndone,
                     bool &bgm_ended)
    {
        const size_t total = c.pcm.size();
        if (total < 2)
            return;

        float left_gain = 1.0f;
        float right_gain = 1.0f;
        if (c.pan < 0.0f)
            right_gain = 1.0f + c.pan;
        else if (c.pan > 0.0f)
            left_gain = 1.0f - c.pan;

        size_t pos = c.pos;
        for (int i = 0; i < frames; ++i) {
            if (pos + 1 >= total) {
                if (c.loop) {
                    pos = 0;
                } else {
                    c.playing = false;
                    c.fade_total = c.fade_left = 0;
                    if (is_bgm)
                        bgm_ended = true;
                    else if (ndone < kMaxChannels)
                        done[ndone++] = channel_index;
                    break;
                }
            }

            float gain = c.volume;
            if (c.fade_total > 0) {
                gain *= (float)c.fade_left / (float)c.fade_total;
                if (--c.fade_left <= 0) {
                    /* A requested fade-out is a stop, not a natural end:
                     * do not raise the BGM-finished event. */
                    c.playing = false;
                    c.fade_total = c.fade_left = 0;
                    break;
                }
            }

            const float l = (float)c.pcm[pos] * gain * left_gain;
            const float r = (float)c.pcm[pos + 1] * gain * right_gain;
            acc[i * 2] += (int32_t)l;
            acc[i * 2 + 1] += (int32_t)r;
            pos += 2;
        }
        c.pos = pos;
    }

    void mix(int16_t *out, int frames, bool record)
    {
        int done[kMaxChannels];
        int ndone = 0;
        bool bgm_ended = false;
        std::function<void(int)> se_cb;
        std::function<void()> bgm_cb;

        {
            std::lock_guard<std::mutex> lk(mu);
            const size_t n = (size_t)frames * 2u;
            if (accum.size() < n) /* only on an unexpected large request */
                accum.resize(n);
            std::fill(accum.begin(), accum.begin() + n, 0);

            if (bgm.playing && !bgm_paused && !bgm.pcm.empty())
                mix_channel(bgm, accum.data(), frames, true, -1, done, ndone,
                            bgm_ended);
            for (int ch = 0; ch < kMaxChannels; ++ch) {
                Channel &c = se[ch];
                if (c.playing && !c.pcm.empty())
                    mix_channel(c, accum.data(), frames, false, ch, done, ndone,
                                bgm_ended);
            }

            if (movie) {
                const int got = movie->read(movie_buf.data(), frames);
                const int use = std::max(0, std::min(got, frames));
                for (int i = 0; i < use * 2; ++i)
                    accum[i] += (int32_t)movie_buf[i];
            }

            const float g = (muted ? 0.0f : master) * kMixHeadroom;
            int peak = 0;
            for (int i = 0; i < frames * 2; ++i) {
                const int v = (int)((float)accum[i] * g);
                accum[i] = v;
                const int a = v < 0 ? -v : v;
                if (a > peak)
                    peak = a;
            }
            /* Soft per-buffer gain trim: keeps overloaded buses inside
             * full scale instead of hard-clipping them. */
            constexpr int kOutputCeiling = 32700;
            const float norm =
                peak > kOutputCeiling ? (float)kOutputCeiling / (float)peak
                                      : 1.0f;
            if (peak > kOutputCeiling)
                peak = kOutputCeiling;
            int out_peak = peak_abs;
            for (int i = 0; i < frames * 2; ++i) {
                const int v = (int)((float)accum[i] * norm);
                out[i] = (int16_t)v;
                const int a = v < 0 ? -v : v;
                if (a > out_peak)
                    out_peak = a;
            }
            peak_abs = out_peak;

            mixed_frames += (uint64_t)frames;
            if (record && dump && dump_bytes < dump_limit) {
                uint64_t room = dump_limit - dump_bytes;
                uint64_t bytes = (uint64_t)frames * kOutFrameBytes;
                if (bytes > room)
                    bytes = room;
                fwrite(out, 1, (size_t)bytes, dump);
                dump_bytes += bytes;
            }

            se_cb = se_done;
            bgm_cb = bgm_done;
        }

        for (int i = 0; i < ndone; ++i) {
            if (se_cb)
                se_cb(done[i]);
        }
        if (bgm_ended && bgm_cb)
            bgm_cb();
    }

    void finalize_dump_locked()
    {
        if (!dump)
            return;
        wav_write_header(dump, dump_bytes);
        fclose(dump);
        dump = nullptr;
        fprintf(stderr,
                "ons: audio mixed %.2f s (peak %.3f), wav data %.2f s\n",
                (double)mixed_frames / (double)kOutRate,
                (double)peak_abs / 32768.0,
                (double)dump_bytes / (double)(kOutRate * kOutFrameBytes));
    }
};

AudioEngine::AudioEngine() : impl_(new Impl())
{
    impl_->allocate_scratch();
}

AudioEngine::~AudioEngine()
{
    close();
}

bool AudioEngine::open(int freq, int channels, int buffer_ms)
{
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->open)
            return true;
    }

    /* The mixer and the device stream are fixed at S16/stereo/44100; the
     * engine always requests this rate, other values are folded in. */
    (void)freq;
    (void)channels;
    int bms = buffer_ms > 0 ? buffer_ms : 46;
    bms = std::max(10, std::min(200, bms));

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        fprintf(stderr, "ons: SDL audio init failed: %s\n", SDL_GetError());

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = kOutChannels;
    spec.freq = kOutRate;

    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::stream_callback,
        impl_.get());

    if (stream)
        SDL_ResumeAudioStreamDevice(stream);

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->freq = kOutRate;
        impl_->buffer_ms = bms;
        impl_->stream = stream;
        impl_->open = true;
        impl_->running.store(true, std::memory_order_relaxed);
    }

    if (stream) {
        fprintf(stderr, "ons: audio device opened (%d Hz, %d ch, %d ms)\n",
                kOutRate, kOutChannels, bms);
    } else {
        fprintf(stderr,
                "ons: audio device unavailable (%s); mixing on silent tick\n",
                SDL_GetError());
        const int frames = kOutRate * bms / 1000;
        impl_->tick = std::thread(&Impl::tick_loop, impl_.get(), frames, bms);
    }
    return true;
}

void AudioEngine::close()
{
    SDL_AudioStream *stream = nullptr;
    std::thread tick;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->open && !impl_->stream && !impl_->tick.joinable()) {
            impl_->finalize_dump_locked();
            return;
        }
        impl_->running.store(false, std::memory_order_relaxed);
        stream = impl_->stream;
        impl_->stream = nullptr;
        if (impl_->tick.joinable())
            tick = std::move(impl_->tick);
        impl_->open = false;
    }

    if (stream)
        SDL_DestroyAudioStream(stream);
    if (tick.joinable())
        tick.join();

    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->finalize_dump_locked();
    impl_->bgm = Impl::Channel();
    for (int i = 0; i < kMaxChannels; ++i)
        impl_->se[i] = Impl::Channel();
}

bool AudioEngine::is_open() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->open;
}

void AudioEngine::set_master_volume(float volume)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->master = clamp01(volume);
}

float AudioEngine::master_volume() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->master;
}

void AudioEngine::set_muted(bool muted)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->muted = muted;
}

bool AudioEngine::muted() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->muted;
}

bool AudioEngine::play_se(const uint8_t *data, size_t len, bool loop,
                          int channel, float volume, bool preload, float pan)
{
    if (channel < 0 || channel >= kMaxChannels)
        return false;

    std::vector<int16_t> pcm;
    if (!decode_audio_memory(data, len, pcm)) {
        fprintf(stderr, "ons: se[%d] decode failed\n", channel);
        return false;
    }

    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl::Channel &c = impl_->se[channel];
    c.pcm = std::move(pcm);
    c.pos = 0;
    c.loop = loop;
    c.volume = clamp01(volume);
    c.pan = std::max(-1.0f, std::min(1.0f, pan));
    c.preloaded = preload;
    c.playing = !preload;
    c.fade_total = c.fade_left = 0;
    return true;
}

void AudioEngine::play_preloaded(int channel, bool loop)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl::Channel &c = impl_->se[channel];
    if (c.pcm.empty())
        return;
    c.pos = 0;
    c.loop = loop;
    c.playing = true;
    c.fade_total = c.fade_left = 0;
}

void AudioEngine::stop_se(int channel)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl::Channel &c = impl_->se[channel];
    c.playing = false;
    c.pos = 0;
    c.fade_total = c.fade_left = 0;
    if (!c.preloaded)
        c.pcm.clear();
}

void AudioEngine::stop_all_se()
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (int i = 0; i < kMaxChannels; ++i) {
        Impl::Channel &c = impl_->se[i];
        c.playing = false;
        c.pos = 0;
        c.fade_total = c.fade_left = 0;
        if (!c.preloaded)
            c.pcm.clear();
    }
}

void AudioEngine::set_se_volume(int channel, float volume)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->se[channel].volume = clamp01(volume);
}

void AudioEngine::set_se_pan(int channel, float pan)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->se[channel].pan = std::max(-1.0f, std::min(1.0f, pan));
}

bool AudioEngine::se_active(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    const Impl::Channel &c = impl_->se[channel];
    return c.playing && !c.pcm.empty();
}

bool AudioEngine::se_preloaded(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    const Impl::Channel &c = impl_->se[channel];
    return c.preloaded && !c.pcm.empty();
}

void AudioEngine::set_se_finished_callback(std::function<void(int)> callback)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->se_done = std::move(callback);
}

bool AudioEngine::play_bgm(const uint8_t *data, size_t len, bool loop,
                           float volume)
{
    /* Drop the previous track before decoding the new one.  Whole-file PCM
     * is tens of megabytes; keeping the old buffer alive while decoding its
     * replacement doubled the peak and made the working set ratchet upwards
     * on every BGM change (the pages stay resident after the free). */
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        Impl::Channel &old = impl_->bgm;
        old.playing = false;
        old.pos = 0;
        old.fade_total = old.fade_left = 0;
        std::vector<int16_t>().swap(old.pcm);
    }

    std::vector<int16_t> pcm;
    if (!decode_audio_memory(data, len, pcm)) {
        fprintf(stderr, "ons: bgm decode failed\n");
        return false;
    }

    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl::Channel &c = impl_->bgm;
    c.pcm = std::move(pcm);
    c.pos = 0;
    c.loop = loop;
    c.volume = clamp01(volume);
    c.pan = 0.0f;
    c.preloaded = false;
    c.playing = true;
    c.fade_total = c.fade_left = 0;
    impl_->bgm_paused = false;
    return true;
}

void AudioEngine::stop_bgm(int fade_ms)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl::Channel &c = impl_->bgm;
    if (!c.playing) {
        std::vector<int16_t>().swap(c.pcm);
        c.pos = 0;
        c.fade_total = c.fade_left = 0;
        return;
    }
    if (fade_ms > 0 && !c.pcm.empty()) {
        const int frames = std::max(1, fade_ms * kOutRate / 1000);
        c.fade_total = frames;
        c.fade_left = frames;
    } else {
        c.playing = false;
        c.pos = 0;
        c.fade_total = c.fade_left = 0;
        std::vector<int16_t>().swap(c.pcm);
    }
}

void AudioEngine::pause_bgm(bool paused)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->bgm_paused = paused;
}

void AudioEngine::set_bgm_volume(float volume)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->bgm.volume = clamp01(volume);
}

float AudioEngine::bgm_volume() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->bgm.volume;
}

bool AudioEngine::bgm_playing() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->bgm.playing && !impl_->bgm.pcm.empty();
}

bool AudioEngine::bgm_fading() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->bgm.playing && impl_->bgm.fade_total > 0;
}

void AudioEngine::set_bgm_finished_callback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->bgm_done = std::move(callback);
}

void AudioEngine::set_movie_source(MovieAudioSource *source)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->movie = source;
}

void AudioEngine::dump_wav(const std::string &path)
{
    if (path.empty())
        return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->dump)
        impl_->finalize_dump_locked();

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "ons: cannot open audio dump '%s'\n", path.c_str());
        return;
    }
    unsigned char zeros[44];
    memset(zeros, 0, sizeof(zeros));
    fwrite(zeros, 1, sizeof(zeros), f);
    impl_->dump = f;
    impl_->dump_bytes = 0;
    impl_->dump_limit =
        (uint64_t)kOutRate * kOutFrameBytes * (uint64_t)kMaxDumpSeconds;
    fprintf(stderr, "ons: audio dump -> %s\n", path.c_str());
}

} // namespace ons
