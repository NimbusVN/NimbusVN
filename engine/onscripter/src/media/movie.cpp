/* -*- C++ -*-
 *
 *  movie.cpp - FFmpeg software movie playback (video + movie audio bus)
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
 *  FFmpeg only: avformat/avcodec for demux/decode, swscale YUV->BGRA,
 *  swresample -> S16 stereo 44100 for the AudioEngine movie bus.  No
 *  system codecs and no legacy movie wrappers.
 *
 *  The audio track is decoded up-front into memory (ONScripter movie files
 *  are short); when a track would exceed the 200 MB budget it is decoded
 *  lazily into a bounded ring buffer instead, pumped from update().
 *  ---------------------------------------------------------------------
 */

#include "movie.h"

#include "clock.h"
#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(ONS_HAVE_FFMPEG) && ONS_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace ons {

#if defined(ONS_HAVE_FFMPEG) && ONS_HAVE_FFMPEG

namespace {

const int kOutRate = 44100;
const int kOutChannels = 2;
/* Hard cap for the up-front decoded track (interleaved samples). */
const size_t kMaxPcmBytes = (size_t)200 * 1024 * 1024;
/* Ring buffer size used by the streaming fallback: 4 seconds. */
const size_t kRingFrames = (size_t)kOutRate * 4;

struct MemIo {
    const uint8_t *data = nullptr;
    size_t len = 0;
    size_t pos = 0;
};

int mem_read_cb(void *opaque, uint8_t *buf, int size)
{
    MemIo *r = static_cast<MemIo *>(opaque);
    if (r->pos >= r->len)
        return AVERROR_EOF;
    const size_t n = std::min((size_t)size, r->len - r->pos);
    memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    return (int)n;
}

int64_t mem_seek_cb(void *opaque, int64_t offset, int whence)
{
    MemIo *r = static_cast<MemIo *>(opaque);
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

void log_av_error(const char *what, int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    fprintf(stderr, "ons: movie: %s failed: %s (%d)\n", what, buf, code);
}

} // namespace

/* ------------------------------------------------------------------------ */

struct MoviePlayer::Impl {
    bool opened = false;

    MemIo io;
    std::vector<uint8_t> bytes;
    AVIOContext *avio = nullptr;
    unsigned char *iobuf = nullptr;
    AVFormatContext *fmt = nullptr;

    AVCodecContext *vdec = nullptr;
    AVFrame *vframe = nullptr;
    AVPacket *pkt = nullptr;
    SwsContext *sws = nullptr;
    int vstream = -1;
    int vw = 0;
    int vh = 0;
    int vfmt = AV_PIX_FMT_NONE;
    AVRational vtb{1, 1000};
    double first_pts = 0.0;
    bool have_first_pts = false;
    bool demux_eof = false;
    bool flush_sent = false;
    double fallback_fps = 25.0;
    double fallback_pts = 0.0;

    AVCodecContext *adec = nullptr;
    AVFrame *aframe = nullptr;
    SwrContext *swr = nullptr;
    int astream = -1;

    std::string vcodec_name;
    std::string acodec_name;
    std::string pixname;
    double duration = 0.0;

    Bitmap frame;
    double pending_pts = 0.0;
    bool has_pending = false;
    int64_t decoded = 0;
    int64_t presented = 0;
    int64_t dropped = 0;

    /* ---- audio buffers ------------------------------------------------ */
    bool streaming = false;
    std::vector<int16_t> pcm;       /* pre-decoded track, interleaved */
    size_t pcm_pos = 0;             /* read position in samples */
    bool pcm_truncated = false;

    AVFormatContext *afmt = nullptr;
    AVIOContext *aavio = nullptr;
    unsigned char *aiobuf = nullptr;
    MemIo aio;
    AVCodecContext *adec2 = nullptr;
    AVFrame *aframe2 = nullptr;
    SwrContext *swr2 = nullptr;
    int astream2 = -1;
    bool audio_eof = false;

    std::vector<int16_t> ring;
    size_t ring_cap_frames = 0;
    size_t ring_read = 0;
    size_t ring_write = 0;
    size_t ring_count = 0;
    bool ring_eof = false;

    std::vector<int16_t> conv;      /* swr output scratch */
    std::vector<int16_t> sink;      /* pre-decode accumulation */
    std::mutex audio_mu;

    ~Impl() { close(); }

    /* -- demuxer ------------------------------------------------------- */
    bool attach(MemIo *mem, AVIOContext **out_avio, unsigned char **out_buf,
                AVFormatContext **out_fmt)
    {
        constexpr int kIoBufSize = 1 << 16;
        unsigned char *buf =
            static_cast<unsigned char *>(av_malloc(kIoBufSize));
        if (!buf)
            return false;
        AVIOContext *a = avio_alloc_context(buf, kIoBufSize, 0, mem,
                                            mem_read_cb, nullptr, mem_seek_cb);
        if (!a) {
            av_free(buf);
            return false;
        }
        AVFormatContext *f = avformat_alloc_context();
        if (!f) {
            av_freep(reinterpret_cast<void **>(&a->buffer));
            avio_context_free(&a);
            return false;
        }
        f->pb = a;
        f->flags |= AVFMT_FLAG_CUSTOM_IO;
        int rc = avformat_open_input(&f, nullptr, nullptr, nullptr);
        if (rc < 0) {
            log_av_error("avformat_open_input", rc);
            av_freep(reinterpret_cast<void **>(&a->buffer));
            avio_context_free(&a);
            return false;
        }
        rc = avformat_find_stream_info(f, nullptr);
        if (rc < 0) {
            log_av_error("avformat_find_stream_info", rc);
            avformat_close_input(&f);
            av_freep(reinterpret_cast<void **>(&a->buffer));
            avio_context_free(&a);
            return false;
        }
        *out_avio = a;
        *out_buf = buf;
        *out_fmt = f;
        return true;
    }

    bool open_video()
    {
        const AVCodec *codec = nullptr;
        vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (vstream < 0 || !codec) {
            fprintf(stderr, "ons: movie: no video stream\n");
            return false;
        }
        AVStream *st = fmt->streams[vstream];
        vdec = avcodec_alloc_context3(codec);
        if (!vdec)
            return false;
        int rc = avcodec_parameters_to_context(vdec, st->codecpar);
        if (rc < 0) {
            log_av_error("avcodec_parameters_to_context(video)", rc);
            return false;
        }
        vdec->thread_count = 1; /* deterministic, in-order frames */
        rc = avcodec_open2(vdec, codec, nullptr);
        if (rc < 0) {
            log_av_error("avcodec_open2(video)", rc);
            return false;
        }
        vframe = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!vframe || !pkt)
            return false;

        vw = vdec->width > 0 ? vdec->width : st->codecpar->width;
        vh = vdec->height > 0 ? vdec->height : st->codecpar->height;
        if (vw <= 0 || vh <= 0)
            return false;

        vtb = st->time_base;
        AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
        if (fr.num > 0 && fr.den > 0)
            fallback_fps = av_q2d(fr);
        if (!(fallback_fps > 0.0))
            fallback_fps = 25.0;

        vcodec_name = avcodec_get_name(codec->id);
        pixname = av_get_pix_fmt_name((AVPixelFormat)vdec->pix_fmt)
                      ? av_get_pix_fmt_name((AVPixelFormat)vdec->pix_fmt)
                      : "unknown";
        return true;
    }

    bool open_audio_decoder(const AVCodec *codec, AVStream *st,
                            AVCodecContext **out_dec, AVFrame **out_frame,
                            SwrContext **out_swr)
    {
        AVCodecContext *dec = avcodec_alloc_context3(codec);
        if (!dec)
            return false;
        int rc = avcodec_parameters_to_context(dec, st->codecpar);
        if (rc < 0) {
            log_av_error("avcodec_parameters_to_context(audio)", rc);
            avcodec_free_context(&dec);
            return false;
        }
        dec->thread_count = 1;
        rc = avcodec_open2(dec, codec, nullptr);
        if (rc < 0) {
            log_av_error("avcodec_open2(audio)", rc);
            avcodec_free_context(&dec);
            return false;
        }
        if (dec->sample_rate <= 0 || dec->sample_fmt == AV_SAMPLE_FMT_NONE) {
            avcodec_free_context(&dec);
            return false;
        }

        AVFrame *fr = av_frame_alloc();
        if (!fr) {
            avcodec_free_context(&dec);
            return false;
        }

        AVChannelLayout in_layout;
        memset(&in_layout, 0, sizeof(in_layout));
        if (dec->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&in_layout, &dec->ch_layout);
        else
            av_channel_layout_default(&in_layout, 1);
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;

        SwrContext *swr = nullptr;
        rc = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, kOutRate,
                                 &in_layout, dec->sample_fmt, dec->sample_rate,
                                 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        if (rc < 0 || !swr) {
            av_frame_free(&fr);
            avcodec_free_context(&dec);
            return false;
        }
        if (swr_init(swr) < 0) {
            swr_free(&swr);
            av_frame_free(&fr);
            avcodec_free_context(&dec);
            return false;
        }

        *out_dec = dec;
        *out_frame = fr;
        *out_swr = swr;
        return true;
    }

    bool open_audio()
    {
        const AVCodec *codec = nullptr;
        astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (astream < 0 || !codec)
            return true; /* silent movie */

        AVStream *st = fmt->streams[astream];
        if (!open_audio_decoder(codec, st, &adec, &aframe, &swr)) {
            fprintf(stderr, "ons: movie: audio decoder unavailable, "
                            "playing without sound\n");
            astream = -1;
            return true;
        }
        acodec_name = avcodec_get_name(codec->id);
        fprintf(stderr,
                "ons: movie: audio %s %d Hz, %d ch -> S16 stereo %d Hz\n",
                acodec_name.c_str(), adec->sample_rate,
                adec->ch_layout.nb_channels, kOutRate);
        return true;
    }

    /* -- container ----------------------------------------------------- */
    bool open_memory(const uint8_t *data, size_t len)
    {
        if (!data || len < 16) {
            fprintf(stderr, "ons: movie: input too small (%zu bytes)\n", len);
            return false;
        }
        static std::once_flag log_once;
        std::call_once(log_once, [] { av_log_set_level(AV_LOG_ERROR); });

        io.data = data;
        io.len = len;
        io.pos = 0;
        if (!attach(&io, &avio, &iobuf, &fmt))
            return false;

        if (!open_video())
            return false;
        if (!open_audio())
            return false;

        if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE)
            duration = (double)fmt->duration / (double)AV_TIME_BASE;
        else if (fmt->streams[vstream]->duration > 0)
            duration = (double)fmt->streams[vstream]->duration *
                       av_q2d(fmt->streams[vstream]->time_base);
        if (!(duration > 0.0))
            duration = 0.0;

        fprintf(stderr,
                "ons: movie: %dx%d %s (%s) %.2fs, audio=%s%s\n", vw, vh,
                vcodec_name.c_str(), pixname.c_str(), duration,
                astream >= 0 ? acodec_name.c_str() : "none",
                astream >= 0 ? "" : "");

        /* Decide between an up-front decode and the bounded ring. */
        const double estimated_bytes =
            duration > 0.0 ? duration * (double)kOutRate * 4.0 : -1.0;
        streaming = (estimated_bytes < 0.0 ||
                     estimated_bytes > (double)kMaxPcmBytes);
        if (streaming) {
            fprintf(stderr,
                    "ons: movie: audio streamed through a %zu-frame ring "
                    "(track too large to pre-decode)\n",
                    kRingFrames);
            if (!open_streaming_audio()) {
                fprintf(stderr,
                        "ons: movie: streaming audio unavailable, "
                        "playing silently\n");
                streaming = false;
                astream = -1;
            }
        } else if (astream >= 0) {
            if (!decode_all_audio()) {
                fprintf(stderr, "ons: movie: audio decode failed, "
                                "playing silently\n");
                pcm.clear();
                astream = -1;
            }
        }

        if (streaming) {
            ring_cap_frames = kRingFrames;
            ring.assign(ring_cap_frames * 2u, 0);
        }

        /* Rewind for playback: the pre-decode consumed the container. */
        seek_av(0.0);
        opened = true;
        return true;
    }

    bool open_streaming_audio()
    {
        aio.data = io.data;
        aio.len = io.len;
        aio.pos = 0;
        if (!attach(&aio, &aavio, &aiobuf, &afmt))
            return false;
        const AVCodec *codec = nullptr;
        astream2 = av_find_best_stream(afmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (astream2 < 0 || !codec)
            return false;
        AVStream *st = afmt->streams[astream2];
        if (!open_audio_decoder(codec, st, &adec2, &aframe2, &swr2))
            return false;
        return true;
    }

    /* -- audio conversion ---------------------------------------------- */
    bool convert_track(AVCodecContext *dec, SwrContext *swr_ctx,
                       const AVFrame *f, std::vector<int16_t> *out)
    {
        int out_max = swr_get_out_samples(swr_ctx, f->nb_samples);
        if (out_max <= 0)
            return true;
        conv.resize((size_t)out_max * 2u);
        uint8_t *dst = reinterpret_cast<uint8_t *>(conv.data());
        const int got =
            swr_convert(swr_ctx, &dst, out_max,
                        (const uint8_t **)f->extended_data, f->nb_samples);
        if (got > 0 && out) {
            const size_t n = (size_t)got * 2u;
            if (out->size() + n <= kMaxPcmBytes / sizeof(int16_t))
                out->insert(out->end(), conv.begin(), conv.begin() + n);
            else
                pcm_truncated = true;
        }
        (void)dec;
        return true;
    }

    void drain_audio_decoder(AVCodecContext *dec, AVFrame *frame,
                             SwrContext *swr_ctx, std::vector<int16_t> *out)
    {
        for (;;) {
            const int rr = avcodec_receive_frame(dec, frame);
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF)
                break;
            if (rr < 0)
                break;
            convert_track(dec, swr_ctx, frame, out);
            av_frame_unref(frame);
        }
    }

    void flush_resampler(SwrContext *swr_ctx, std::vector<int16_t> *out)
    {
        for (;;) {
            conv.resize(2048u * 2u);
            uint8_t *dst = reinterpret_cast<uint8_t *>(conv.data());
            const int got = swr_convert(swr_ctx, &dst, 2048, nullptr, 0);
            if (got <= 0)
                break;
            if (out)
                out->insert(out->end(), conv.begin(),
                            conv.begin() + (size_t)got * 2u);
        }
    }

    bool decode_all_audio()
    {
        sink.clear();
        if (duration > 0.0) {
            const size_t est =
                (size_t)(duration * (double)kOutRate * 2.0);
            if (est > 16)
                sink.reserve(est);
        }

        for (;;) {
            if (pcm_truncated)
                break;
            const int ar = av_read_frame(fmt, pkt);
            if (ar == AVERROR_EOF)
                break;
            if (ar < 0)
                break; /* keep whatever was decoded from a truncated tail */
            if (pkt->stream_index == astream) {
                if (avcodec_send_packet(adec, pkt) == 0)
                    drain_audio_decoder(adec, aframe, swr, &sink);
            }
            av_packet_unref(pkt);
        }
        if (!pcm_truncated) {
            if (avcodec_send_packet(adec, nullptr) >= 0)
                drain_audio_decoder(adec, aframe, swr, &sink);
            flush_resampler(swr, &sink);
        }
        if (pcm_truncated)
            fprintf(stderr,
                    "ons: movie: audio track exceeds the %zu MB budget, "
                    "tail dropped\n",
                    kMaxPcmBytes / (1024u * 1024u));
        pcm.swap(sink);
        pcm_pos = 0;
        return !pcm.empty();
    }

    /* -- streaming ring ------------------------------------------------ */
    void push_ring(const std::vector<int16_t> &src)
    {
        if (src.empty() || ring_cap_frames == 0)
            return;
        const size_t want_frames = src.size() / 2u;
        size_t frames = want_frames;
        {
            std::lock_guard<std::mutex> lk(audio_mu);
            const size_t free_frames = ring_cap_frames - ring_count;
            frames = std::min(frames, free_frames);
            size_t w = ring_write;
            for (size_t i = 0; i < frames; ++i) {
                ring[w * 2u] = src[i * 2u];
                ring[w * 2u + 1u] = src[i * 2u + 1u];
                w = (w + 1u) % ring_cap_frames;
            }
            ring_write = w;
            ring_count += frames;
        }
    }

    void drain_audio_stream()
    {
        for (;;) {
            const int rr = avcodec_receive_frame(adec2, aframe2);
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF)
                break;
            if (rr < 0)
                break;
            conv.clear();
            convert_track(adec2, swr2, aframe2, &conv);
            push_ring(conv);
            av_frame_unref(aframe2);
        }
    }

    void pump_audio_stream()
    {
        if (!streaming || !afmt)
            return;
        for (int guard = 0; guard < 64; ++guard) {
            size_t count;
            {
                std::lock_guard<std::mutex> lk(audio_mu);
                count = ring_count;
            }
            if (count > ring_cap_frames / 2u)
                return; /* keep the producer ahead, not unbounded */
            if (audio_eof) {
                if (!pcm_should_loop_hint())
                    return;
                {
                    std::lock_guard<std::mutex> lk(audio_mu);
                    ring_read = ring_write = ring_count = 0;
                    ring_eof = false;
                }
                reset_stream_audio();
                continue;
            }
            const int ar = av_read_frame(afmt, pkt);
            if (ar == AVERROR_EOF) {
                audio_eof = true;
                if (avcodec_send_packet(adec2, nullptr) >= 0)
                    drain_audio_stream();
                conv.clear();
                flush_resampler(swr2, &conv);
                push_ring(conv);
                {
                    std::lock_guard<std::mutex> lk(audio_mu);
                    ring_eof = true;
                }
                return;
            }
            if (ar < 0) {
                audio_eof = true;
                std::lock_guard<std::mutex> lk(audio_mu);
                ring_eof = true;
                return;
            }
            if (pkt->stream_index == astream2 &&
                avcodec_send_packet(adec2, pkt) == 0)
                drain_audio_stream();
            av_packet_unref(pkt);
        }
    }

    bool pcm_should_loop_hint() const { return loop_hint; }
    bool loop_hint = false;

    void reset_stream_audio()
    {
        if (!afmt)
            return;
        if (av_seek_frame(afmt, -1, 0, AVSEEK_FLAG_BACKWARD) < 0)
            return;
        avcodec_flush_buffers(adec2);
        if (swr2) {
            swr_close(swr2);
            swr_init(swr2);
        }
        audio_eof = false;
        {
            std::lock_guard<std::mutex> lk(audio_mu);
            ring_eof = false;
        }
    }

    /* -- seeking ------------------------------------------------------- */
    bool seek_av(double seconds)
    {
        if (!fmt)
            return false;
        const int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
        const int rc = av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        if (rc < 0) {
            log_av_error("av_seek_frame", rc);
            return false;
        }
        if (vdec)
            avcodec_flush_buffers(vdec);
        if (adec)
            avcodec_flush_buffers(adec);
        if (swr) {
            swr_close(swr);
            swr_init(swr);
        }
        demux_eof = false;
        flush_sent = false;
        have_first_pts = false;
        first_pts = 0.0;
        fallback_pts = 0.0;
        has_pending = false;
        if (afmt) {
            av_seek_frame(afmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(adec2);
            if (swr2) {
                swr_close(swr2);
                swr_init(swr2);
            }
            audio_eof = false;
            std::lock_guard<std::mutex> lk(audio_mu);
            ring_read = ring_write = ring_count = 0;
            ring_eof = false;
        }
        {
            std::lock_guard<std::mutex> lk(audio_mu);
            pcm_pos = 0;
        }
        return true;
    }

    /* -- video decoding ------------------------------------------------ */
    bool decode_next_frame(double *pts_out)
    {
        if (!vdec)
            return false;
        for (;;) {
            const int rr = avcodec_receive_frame(vdec, vframe);
            if (rr == 0) {
                const int fw = vframe->width;
                const int fh = vframe->height;
                if (fw <= 0 || fh <= 0) {
                    av_frame_unref(vframe);
                    continue;
                }

                if (fw != vw || fh != vh || vframe->format != vfmt ||
                    !frame.valid()) {
                    if (!frame.create(fw, fh)) {
                        av_frame_unref(vframe);
                        return false;
                    }
                    vw = fw;
                    vh = fh;
                    vfmt = vframe->format;
                    if (sws)
                        sws_freeContext(sws);
                    sws = sws_getContext(vw, vh,
                                         (AVPixelFormat)vframe->format, vw, vh,
                                         AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                                         nullptr, nullptr);
                    if (!sws) {
                        fprintf(stderr,
                                "ons: movie: sws_getContext failed for %s\n",
                                av_get_pix_fmt_name(
                                    (AVPixelFormat)vframe->format));
                        av_frame_unref(vframe);
                        return false;
                    }
                    fprintf(stderr,
                            "ons: movie: scale %dx%d %s -> BGRA\n", vw, vh,
                            av_get_pix_fmt_name(
                                (AVPixelFormat)vframe->format));
                }

                uint8_t *dst[4] = {(uint8_t *)frame.pixels, nullptr, nullptr,
                                   nullptr};
                const int dst_stride[4] = {frame.pitch, 0, 0, 0};
                const int scaled = sws_scale(sws, vframe->data,
                                             vframe->linesize, 0, vh, dst,
                                             dst_stride);
                if (scaled <= 0) {
                    av_frame_unref(vframe);
                    return false;
                }
                /* Opaque movie frames: force alpha even for formats that
                 * carry a dummy alpha plane. */
                const size_t count = (size_t)frame.w * (size_t)frame.h;
                for (size_t i = 0; i < count; ++i)
                    frame.pixels[i] |= 0xff000000u;

                int64_t ts = vframe->best_effort_timestamp;
                if (ts == AV_NOPTS_VALUE)
                    ts = vframe->pts;
                double rel = 0.0;
                if (ts != AV_NOPTS_VALUE && vtb.num > 0 && vtb.den > 0) {
                    const double sec = (double)ts * av_q2d(vtb);
                    if (!have_first_pts) {
                        first_pts = sec;
                        have_first_pts = true;
                    }
                    rel = sec - first_pts;
                    fallback_pts = rel;
                } else {
                    rel = fallback_pts;
                    fallback_pts += 1.0 / fallback_fps;
                }
                if (rel < 0.0)
                    rel = 0.0;
                *pts_out = rel;
                ++decoded;
                av_frame_unref(vframe);
                return true;
            }
            if (rr == AVERROR_EOF)
                return false;
            if (rr != AVERROR(EAGAIN)) {
                log_av_error("avcodec_receive_frame(video)", rr);
                return false;
            }
            if (demux_eof) {
                if (!flush_sent) {
                    avcodec_send_packet(vdec, nullptr);
                    flush_sent = true;
                    continue;
                }
                return false;
            }
            const int ar = av_read_frame(fmt, pkt);
            if (ar == AVERROR_EOF) {
                demux_eof = true;
                avcodec_send_packet(vdec, nullptr);
                flush_sent = true;
                continue;
            }
            if (ar < 0) {
                log_av_error("av_read_frame", ar);
                demux_eof = true;
                return false;
            }
            if (pkt->stream_index == vstream) {
                const int sret = avcodec_send_packet(vdec, pkt);
                av_packet_unref(pkt);
                if (sret == AVERROR(EAGAIN))
                    continue; /* receive the queued frames first */
                if (sret < 0) {
                    log_av_error("avcodec_send_packet(video)", sret);
                    return false;
                }
                continue;
            }
            av_packet_unref(pkt);
        }
    }

    void close()
    {
        if (sws)
            sws_freeContext(sws);
        sws = nullptr;
        if (swr)
            swr_free(&swr);
        if (swr2)
            swr_free(&swr2);
        if (vframe)
            av_frame_free(&vframe);
        if (aframe)
            av_frame_free(&aframe);
        if (aframe2)
            av_frame_free(&aframe2);
        if (pkt)
            av_packet_free(&pkt);
        if (vdec)
            avcodec_free_context(&vdec);
        if (adec)
            avcodec_free_context(&adec);
        if (adec2)
            avcodec_free_context(&adec2);
        if (fmt)
            avformat_close_input(&fmt);
        if (avio) {
            /* The demux buffer belongs to the caller, not to the AVIOContext. */
            av_freep(reinterpret_cast<void **>(&avio->buffer));
            avio_context_free(&avio);
        }
        if (afmt)
            avformat_close_input(&afmt);
        if (aavio) {
            av_freep(reinterpret_cast<void **>(&aavio->buffer));
            avio_context_free(&aavio);
        }
        iobuf = nullptr;
        aiobuf = nullptr;
        frame.free();
        opened = false;
    }
};

/* ------------------------------------------------------------------------ */

MoviePlayer::MoviePlayer()
    : status_(MovieStatus::Stopped), output_(nullptr), renderer_(nullptr),
      audio_(nullptr), has_audio_(false), width_(0), height_(0), loop_(false),
      skippable_(false), audio_running_(false), played_frames_(0),
      volume_(1.0f), play_start_us_(0), first_frame_presented_(false)
{
    output_rect_.x = 0;
    output_rect_.y = 0;
    output_rect_.w = 0;
    output_rect_.h = 0;
}

MoviePlayer::~MoviePlayer()
{
    close();
}

bool MoviePlayer::open(const std::string &path)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "ons: movie: cannot open '%s'\n", path.c_str());
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    const long length = ftell(fp);
    if (length <= 0) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    std::vector<uint8_t> bytes((size_t)length);
    const size_t got = fread(bytes.data(), 1, bytes.size(), fp);
    fclose(fp);
    if (got != bytes.size())
        return false;
    return open(bytes.data(), bytes.size());
}

bool MoviePlayer::open(const uint8_t *data, size_t len)
{
    close();
    impl_.reset(new Impl());
    /* Impl demuxes straight from the caller's buffer.  The engine keeps the
     * script archive buffer alive for the whole command, but copy it so a
     * short-lived stack vector can never dangle. */
    impl_->bytes.assign(data, data + len);
    if (!impl_->open_memory(impl_->bytes.data(), impl_->bytes.size())) {
        fprintf(stderr, "ons: movie: unsupported or corrupt container\n");
        impl_.reset();
        return false;
    }
    width_ = impl_->vw;
    height_ = impl_->vh;
    has_audio_ = impl_->astream >= 0;
    status_ = MovieStatus::Stopped;
    return true;
}

void MoviePlayer::close()
{
    if (audio_)
        audio_->set_movie_source(nullptr);
    audio_running_.store(false);
    played_frames_.store(0);
    status_ = MovieStatus::Stopped;
    impl_.reset();
    has_audio_ = false;
    width_ = height_ = 0;
    output_ = nullptr;
    output_rect_.x = output_rect_.y = output_rect_.w = output_rect_.h = 0;
}

bool MoviePlayer::is_open() const { return impl_ && impl_->opened; }

void MoviePlayer::set_output(Bitmap *bitmap, const Rect &rect)
{
    output_ = bitmap;
    output_rect_ = rect;
}

void MoviePlayer::set_volume(float volume)
{
    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;
    volume_.store(volume);
}

bool MoviePlayer::play(bool loop, bool skippable)
{
    if (!impl_ || !impl_->opened)
        return false;

    const bool resume = (status_ == MovieStatus::Paused);
    loop_ = loop;
    skippable_ = skippable;
    impl_->loop_hint = loop;

    if (!resume) {
        seek_to_start();
        play_start_us_ = (uint64_t)std::chrono::duration_cast<
                             std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        first_frame_presented_ = false;
        played_frames_.store(0);
        impl_->decoded = 0;
        impl_->presented = 0;
        impl_->dropped = 0;
    }

    status_ = MovieStatus::Playing;
    audio_running_.store(true);
    if (audio_)
        audio_->set_movie_source(has_audio_ ? this : nullptr);
    if (resume)
        fprintf(stderr, "ons: movie: resumed at %.2fs (shown=%lld)\n",
                impl_->pending_pts, (long long)impl_->presented);
    return true;
}

void MoviePlayer::stop()
{
    if (audio_)
        audio_->set_movie_source(nullptr);
    audio_running_.store(false);
    status_ = MovieStatus::Stopped;
    if (impl_) {
        impl_->has_pending = false;
    }
}

void MoviePlayer::pause(bool paused)
{
    if (!impl_)
        return;
    if (paused) {
        if (status_ == MovieStatus::Playing)
            status_ = MovieStatus::Paused;
        audio_running_.store(false);
    } else if (status_ == MovieStatus::Paused) {
        status_ = MovieStatus::Playing;
        audio_running_.store(true);
    }
}

double MoviePlayer::duration() const
{
    return impl_ ? impl_->duration : 0.0;
}

const char *MoviePlayer::video_codec_name() const
{
    return impl_ ? impl_->vcodec_name.c_str() : "";
}

const char *MoviePlayer::audio_codec_name() const
{
    return impl_ ? impl_->acodec_name.c_str() : "";
}

const char *MoviePlayer::pixel_format_name() const
{
    return impl_ ? impl_->pixname.c_str() : "";
}

int64_t MoviePlayer::decoded_frames() const
{
    return impl_ ? impl_->decoded : 0;
}

int64_t MoviePlayer::presented_frames() const
{
    return impl_ ? impl_->presented : 0;
}

int64_t MoviePlayer::dropped_frames() const
{
    return impl_ ? impl_->dropped : 0;
}

int64_t MoviePlayer::played_audio_frames() const
{
    return played_frames_.load();
}

double MoviePlayer::audio_clock_seconds() const
{
    if (!impl_ || !has_audio_ || !audio_ || !audio_->is_open())
        return -1.0;
    if (!audio_running_.load())
        return -1.0;
    if (impl_->streaming) {
        std::lock_guard<std::mutex> lk(impl_->audio_mu);
        if (impl_->ring_eof && impl_->ring_count == 0 && !loop_)
            return -1.0; /* track ended: hand the clock back to the wall */
    } else {
        std::lock_guard<std::mutex> lk(impl_->audio_mu);
        /* The video stream can outlive a slightly shorter audio track; once
         * the PCM buffer is exhausted (and we are not looping) the audio
         * clock would freeze, so fall back to wall-clock pacing. */
        if (!loop_ && !impl_->pcm.empty() &&
            impl_->pcm_pos >= impl_->pcm.size())
            return -1.0;
    }
    return (double)played_frames_.load() / (double)kOutRate;
}

bool MoviePlayer::advance_video(double *pts)
{
    return impl_ && impl_->decode_next_frame(pts);
}

bool MoviePlayer::handle_end_of_stream()
{
    if (loop_ && seek_to_start()) {
        play_start_us_ = (uint64_t)std::chrono::duration_cast<
                             std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        first_frame_presented_ = false;
        played_frames_.store(0);
        return true;
    }
    status_ = MovieStatus::Finished;
    audio_running_.store(false);
    if (audio_)
        audio_->set_movie_source(nullptr);
    fprintf(stderr, "ons: movie: finished (%lld frames)\n",
            (long long)impl_->decoded);
    return false;
}

void MoviePlayer::present_pending_frame()
{
    if (!impl_ || !impl_->has_pending)
        return;

    if (output_ && impl_->frame.valid()) {
        Rect area = output_rect_;
        if (area.w <= 0)
            area.w = output_->w - area.x;
        if (area.h <= 0)
            area.h = output_->h - area.y;

        /* Letterbox inside the requested area, keeping the movie's aspect. */
        int dw = area.w;
        int dh = area.h;
        if (impl_->vw > 0 && impl_->vh > 0) {
            dh = (int)((int64_t)area.w * impl_->vh / impl_->vw);
            if (dh > area.h) {
                dh = area.h;
                dw = (int)((int64_t)area.h * impl_->vw / impl_->vh);
            }
        }
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        Rect dst;
        dst.x = area.x + (area.w - dw) / 2;
        dst.y = area.y + (area.h - dh) / 2;
        dst.w = dw;
        dst.h = dh;

        output_->fill(area, 0xff000000u);
        output_->blit_scaled(impl_->frame, nullptr, &dst);
        if (renderer_)
            renderer_->present(*output_, nullptr);
    }

    impl_->has_pending = false;
    ++impl_->presented;
    first_frame_presented_ = true;
}

bool MoviePlayer::update(bool synchronous)
{
    if (!impl_ || !impl_->opened)
        return false;
    if (status_ == MovieStatus::Paused)
        return true;
    if (status_ != MovieStatus::Playing)
        return false;

    if (impl_->streaming)
        impl_->pump_audio_stream();

    for (int guard = 0; guard < 500; ++guard) {
        if (!impl_->has_pending) {
            double pts = 0.0;
            if (!advance_video(&pts))
                return handle_end_of_stream();
            impl_->pending_pts = pts;
            impl_->has_pending = true;
        }

        if (!first_frame_presented_) {
            present_pending_frame();
            return true;
        }

        double now = audio_clock_seconds();
        if (now < 0.0) {
            const uint64_t t = (uint64_t)std::chrono::duration_cast<
                                   std::chrono::microseconds>(
                                   std::chrono::steady_clock::now()
                                       .time_since_epoch())
                                   .count();
            now = (double)(t - play_start_us_) / 1000000.0;
        }

        const double wait = impl_->pending_pts - now;
        if (wait > 0.002) {
            if (synchronous) {
                int ms = (int)(wait * 1000.0);
                if (ms < 1)
                    ms = 1;
                if (ms > 5)
                    ms = 5;
                ons::sleep_ms((uint32_t)ms);
            }
            return true;
        }

        /* Video is behind the audio clock: drop frames until it catches up. */
        if (wait < -0.150 && impl_->decoded > 2) {
            impl_->has_pending = false;
            ++impl_->dropped;
            continue;
        }

        present_pending_frame();
        return true;
    }
    return true;
}

bool MoviePlayer::seek_to_start()
{
    if (!impl_)
        return false;
    const bool ok = impl_->seek_av(0.0);
    impl_->has_pending = false;
    first_frame_presented_ = false;
    return ok;
}

int MoviePlayer::sample_rate() const { return kOutRate; }

int MoviePlayer::read(int16_t *buffer, int frames)
{
    if (!impl_ || !buffer || frames <= 0)
        return 0;
    if (!audio_running_.load() || !has_audio_)
        return 0;

    const float gain = volume_.load();
    int got = 0;

    if (impl_->streaming) {
        std::lock_guard<std::mutex> lk(impl_->audio_mu);
        if (impl_->ring_count == 0)
            return 0;
        const size_t want = std::min((size_t)frames, impl_->ring_count);
        size_t r = impl_->ring_read;
        for (size_t i = 0; i < want; ++i) {
            const float l = (float)impl_->ring[r * 2u] * gain;
            const float rr = (float)impl_->ring[r * 2u + 1u] * gain;
            buffer[i * 2u] = (int16_t)std::lround(l);
            buffer[i * 2u + 1u] = (int16_t)std::lround(rr);
            r = (r + 1u) % impl_->ring_cap_frames;
        }
        impl_->ring_read = r;
        impl_->ring_count -= want;
        got = (int)want;
    } else {
        std::lock_guard<std::mutex> lk(impl_->audio_mu);
        const size_t total = impl_->pcm.size();
        if (impl_->pcm_pos >= total) {
            if (loop_ && total > 0) {
                impl_->pcm_pos = 0;
            } else {
                return 0;
            }
        }
        const size_t avail = (total - impl_->pcm_pos) / 2u;
        got = (int)std::min((size_t)frames, avail);
        if (got > 0) {
            for (int i = 0; i < got * 2; ++i) {
                const float v = (float)impl_->pcm[impl_->pcm_pos + (size_t)i] *
                                gain;
                buffer[i] = (int16_t)std::lround(v);
            }
            impl_->pcm_pos += (size_t)got * 2u;
        }
        if (impl_->pcm_pos >= total && loop_ && total > 0)
            impl_->pcm_pos = 0;
    }

    if (got > 0)
        played_frames_.fetch_add(got);
    return got;
}

#else /* !ONS_HAVE_FFMPEG */

struct MoviePlayer::Impl {
    bool opened = false;
};

MoviePlayer::MoviePlayer()
    : status_(MovieStatus::Stopped), output_(nullptr), renderer_(nullptr),
      audio_(nullptr), has_audio_(false), width_(0), height_(0), loop_(false),
      skippable_(false), audio_running_(false), played_frames_(0),
      volume_(1.0f), play_start_us_(0), first_frame_presented_(false)
{
    output_rect_.x = output_rect_.y = output_rect_.w = output_rect_.h = 0;
}

MoviePlayer::~MoviePlayer() {}

bool MoviePlayer::open(const std::string &path)
{
    fprintf(stderr, "ons: movie: '%s' cannot play, FFmpeg support disabled\n",
            path.c_str());
    return false;
}

bool MoviePlayer::open(const uint8_t *, size_t) { return false; }
void MoviePlayer::close()
{
    impl_.reset();
    status_ = MovieStatus::Stopped;
    has_audio_ = false;
    width_ = height_ = 0;
    output_ = nullptr;
    output_rect_.x = output_rect_.y = output_rect_.w = output_rect_.h = 0;
}
bool MoviePlayer::is_open() const { return false; }
void MoviePlayer::set_output(Bitmap *bitmap, const Rect &rect)
{
    output_ = bitmap;
    output_rect_ = rect;
}
void MoviePlayer::set_volume(float volume) { volume_.store(volume); }
bool MoviePlayer::play(bool, bool) { return false; }
void MoviePlayer::stop() { status_ = MovieStatus::Stopped; }
void MoviePlayer::pause(bool) {}
double MoviePlayer::duration() const { return 0.0; }
const char *MoviePlayer::video_codec_name() const { return ""; }
const char *MoviePlayer::audio_codec_name() const { return ""; }
const char *MoviePlayer::pixel_format_name() const { return ""; }
int64_t MoviePlayer::decoded_frames() const { return 0; }
int64_t MoviePlayer::presented_frames() const { return 0; }
int64_t MoviePlayer::dropped_frames() const { return 0; }
int64_t MoviePlayer::played_audio_frames() const { return 0; }
double MoviePlayer::audio_clock_seconds() const { return -1.0; }
bool MoviePlayer::advance_video(double *) { return false; }
bool MoviePlayer::handle_end_of_stream() { return false; }
void MoviePlayer::present_pending_frame() {}
bool MoviePlayer::update(bool) { return false; }
bool MoviePlayer::seek_to_start() { return false; }
int MoviePlayer::sample_rate() const { return 44100; }
int MoviePlayer::read(int16_t *, int) { return 0; }

#endif /* ONS_HAVE_FFMPEG */

} // namespace ons
