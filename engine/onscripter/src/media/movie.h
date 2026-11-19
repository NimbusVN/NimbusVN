/* -*- C++ -*-
 *
 *  movie.h - FFmpeg movie playback interface
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
 */

#ifndef ONS_MEDIA_MOVIE_H
#define ONS_MEDIA_MOVIE_H

#include "audio.h"
#include "bitmap.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ons {

class Renderer;

enum class MovieStatus {
    Stopped,
    Playing,
    Paused,
    Finished
};

/*
 * Container movie playback: FFmpeg demux/decode for the picture, swscale to
 * BGRA, swresample to S16 stereo 44100 for the sound track.  The decoded
 * frames are scaled into the output bitmap and pushed to the renderer; the
 * audio track is pulled by AudioEngine through MovieAudioSource.
 *
 * Everything except MovieAudioSource::read() runs on the caller's thread.
 * read() is called by the mixer and only copies from a pre-decoded PCM buffer
 * (or a bounded ring in streaming mode), never from FFmpeg.
 */
class MoviePlayer : public MovieAudioSource {
public:
    MoviePlayer();
    ~MoviePlayer() override;

    MoviePlayer(const MoviePlayer &) = delete;
    MoviePlayer &operator=(const MoviePlayer &) = delete;

    bool open(const std::string &path);
    bool open(const uint8_t *data, size_t len);
    void close();
    bool is_open() const;

    /* Destination bitmap (usually screen_surface) and the movie area in it. */
    void set_output(Bitmap *bitmap, const Rect &rect);
    void set_renderer(Renderer *renderer) { renderer_ = renderer; }
    void set_audio_engine(AudioEngine *audio) { audio_ = audio; }
    void set_volume(float volume);

    /*
     * Starts playback.  When the player is currently paused (the engine
     * pauses it across fullscreen/window switches) the position is kept;
     * otherwise playback restarts from the beginning.
     */
    bool play(bool loop, bool skippable);
    void stop();
    void pause(bool paused);

    MovieStatus status() const { return status_; }
    bool playing() const { return status_ == MovieStatus::Playing; }
    bool paused() const { return status_ == MovieStatus::Paused; }

    void set_loop(bool loop) { loop_ = loop; }
    bool has_audio() const { return has_audio_; }
    int width() const { return width_; }
    int height() const { return height_; }

    /* Container metadata used by diagnostics. */
    double duration() const;
    const char *video_codec_name() const;
    const char *audio_codec_name() const;
    const char *pixel_format_name() const;
    int64_t decoded_frames() const;
    int64_t presented_frames() const;
    int64_t played_audio_frames() const;
    int64_t dropped_frames() const;

    /*
     * Decodes and presents at most one frame.  Returns false once the movie
     * has ended (Finished/Stopped).  When `synchronous` is true the call
     * sleeps briefly while waiting for the next presentation time; the
     * asynchronous engine path passes false and returns immediately.
     */
    bool update(bool synchronous = false);

    /* ---- MovieAudioSource -------------------------------------------- */
    int sample_rate() const override;
    int read(int16_t *buffer, int frames) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void present_pending_frame();
    bool advance_video(double *pts);
    bool handle_end_of_stream();
    bool seek_to_start();
    double audio_clock_seconds() const;

    MovieStatus status_;
    Bitmap *output_;
    Rect output_rect_;
    Renderer *renderer_;
    AudioEngine *audio_;
    bool has_audio_;
    int width_;
    int height_;
    bool loop_;
    bool skippable_;

    std::atomic<bool> audio_running_;
    std::atomic<int64_t> played_frames_;
    std::atomic<float> volume_;

    uint64_t play_start_us_;
    bool first_frame_presented_;
};

} // namespace ons


using ons::MoviePlayer;
using ons::MovieStatus;

#endif /* ONS_MEDIA_MOVIE_H */
