/* -*- C++ -*-
 *
 *  audio.h - audio engine interface (BGM / SE / voice / movie bus)
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

#ifndef ONS_MEDIA_AUDIO_H
#define ONS_MEDIA_AUDIO_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ons {

/*
 * Pull interface for the movie audio track.  MoviePlayer implements it
 * and hands it to AudioEngine::set_movie_stream(); the mixer pumps it from the
 * device callback.  Frames are interleaved signed 16-bit stereo at 44100 Hz
 * (MoviePlayer resamples before handing the data over).
 */
class MovieAudioSource {
public:
    virtual ~MovieAudioSource() {}
    virtual int sample_rate() const = 0;
    /* Returns the number of frames actually written (0 at end of stream). */
    virtual int read(int16_t *buffer, int frames) = 0;
};

/*
 * Software mixer fed by FFmpeg-decoded in-memory PCM.
 *
 * All decoding happens on the caller's thread; the device callback only sums
 * pre-decoded S16 stereo 44100 Hz buffers, so it never allocates or blocks on
 * I/O.  Every public method is mutex-protected.
 */
class AudioEngine {
public:
    /* Highest SE channel index accepted by the mixer. */
    static const int kMaxChannels = 64;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine &) = delete;
    AudioEngine &operator=(const AudioEngine &) = delete;

    /*
     * Opens the default SDL3 playback device as an S16/stereo/44100 stream.
     * Returns true even when no device is available: the mixer then runs on an
     * internal tick thread so headless builds can still produce a WAV dump.
     */
    bool open(int freq = 44100, int channels = 2, int buffer_ms = 46);
    void close();
    bool is_open() const;

    void set_master_volume(float volume);
    float master_volume() const;
    void set_muted(bool muted);
    bool muted() const;

    /* ---- sound effects / voices ------------------------------------- */
    /* Returns false when the data could not be decoded. */
    bool play_se(const uint8_t *data, size_t len, bool loop, int channel,
                 float volume, bool preload, float pan = 0.0f);
    void play_preloaded(int channel, bool loop = false);
    void stop_se(int channel);
    void stop_all_se();
    void set_se_volume(int channel, float volume);
    void set_se_pan(int channel, float pan);
    bool se_active(int channel) const;
    bool se_preloaded(int channel) const;

    /* Invoked from the mixer thread when a non-looping SE reaches its end. */
    void set_se_finished_callback(std::function<void(int)> callback);

    /* ---- background music ------------------------------------------- */
    bool play_bgm(const uint8_t *data, size_t len, bool loop, float volume);
    void stop_bgm(int fade_ms);
    void pause_bgm(bool paused);
    void set_bgm_volume(float volume);
    float bgm_volume() const;
    bool bgm_playing() const;
    bool bgm_fading() const;

    /* Invoked from the mixer thread when a non-looping BGM reaches its end. */
    void set_bgm_finished_callback(std::function<void()> callback);

    /* ---- movie audio ------------------------------------------------- */
    void set_movie_source(MovieAudioSource *source);

    /* ---- diagnostics ------------------------------------------------- */
    /*
     * Starts recording the mixer output (including silence) to `path` as a
     * 16-bit stereo 44100 Hz WAV.  Recording stops at close(); at most ten
     * minutes are captured.
     */
    void dump_wav(const std::string &path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ons

using ons::AudioEngine;
using ons::MovieAudioSource;

#endif /* ONS_MEDIA_AUDIO_H */
