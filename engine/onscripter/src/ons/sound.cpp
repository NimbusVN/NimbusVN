/* -*- C++ -*-
 *
 *  sound.cpp - Methods for playing sound for ONScripter
 *
 *  Copyright (c) 2001-2011 Ogapee. All rights reserved.
 *  (original ONScripter, of which this is a fork).
 *
 *  ogapee@aqua.dti2.ne.jp
 *
 *  Copyright (c) 2008-2011 "Uncle" Mion Sonozaki
 *
 *  UncleMion@gmail.com
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
 *  S4: playback is delegated to ons::AudioEngine (FFmpeg decode + software
 *  mixer + SDL3 device).  The legacy external mixer / MPEG / VorbisFile
 *  backends are gone; the remaining members only keep the engine-side
 *  bookkeeping the rest of ONScripter relies on (wave_sample /
 *  channel_preloaded / loop flags).
 *  ---------------------------------------------------------------------
 */

#include "ONScripterLabel.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

/* ASF (WMA) container magic; the original engine refused these. */
static bool is_asf_header(const unsigned char *buf)
{
    return buf[0] == 0x30 && buf[1] == 0x26 && buf[2] == 0xb2 &&
           buf[3] == 0x75 && buf[4] == 0x8e && buf[5] == 0x66 &&
           buf[6] == 0xcf && buf[7] == 0x11;
}

/* RIFF/AVI magic; AVI audio tracks are not played as sound effects. */
static bool is_avi_header(const unsigned char *buf)
{
    return buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
           buf[8] == 'A' && buf[9] == 'V' && buf[10] == 'I';
}

int ONScripterLabel::playSound(const char *filename, int format, bool loop_flag,
                               int channel)
{
    if (!audio_open_flag || !filename || !*filename)
        return SOUND_NONE;

    const int channel_count = ONS_MIX_CHANNELS + ONS_MIX_EXTRA_CHANNELS;
    if (channel < 0 || channel >= channel_count)
        return SOUND_NONE;

    const size_t length = script_h.cBR->getFileLength(filename);
    if (length == 0)
        return SOUND_NONE;

    /* In skip mode non-BGM wave/ogg playback is suppressed (original rule). */
    if (!mode_wave_demo_flag &&
        ((skip_mode & SKIP_NORMAL) || ctrl_pressed_status)) {
        if ((format & (SOUND_OGG | SOUND_WAVE)) &&
            ((channel < ONS_MIX_CHANNELS) || (channel == MIX_WAVE_CHANNEL) ||
             (channel == MIX_CLICKVOICE_CHANNEL)))
            return SOUND_NONE;
    }

    std::vector<unsigned char> buffer(length);
    if (script_h.cBR->getFile(filename, buffer.data()) != length)
        return SOUND_NONE;

    if (length >= 11 && is_asf_header(buffer.data())) {
        fprintf(stderr, "ons: '%s' is WMA, skipped\n", filename);
        return SOUND_OTHER;
    }
    if (length >= 12 && is_avi_header(buffer.data())) {
        fprintf(stderr, "ons: '%s' is AVI, skipped\n", filename);
        return SOUND_OTHER;
    }

    int ret = SOUND_OTHER;
    if (format & SOUND_OGG_STREAMING)
        ret = SOUND_OGG_STREAMING;
    else if (format & SOUND_OGG)
        ret = SOUND_OGG;
    else if (format & SOUND_WAVE)
        ret = SOUND_WAVE;
    else if (format & SOUND_MP3)
        ret = SOUND_MP3;
    else if (format & SOUND_SEQMUSIC)
        ret = SOUND_SEQMUSIC;

    const bool preload = (format & SOUND_PRELOAD) != 0;
    const bool bgm_channel = (channel == MIX_BGM_CHANNEL ||
                              channel == MIX_LOOPBGM_CHANNEL0 ||
                              channel == MIX_LOOPBGM_CHANNEL1);

    if (bgm_channel) {
        /* Background music is streamed on the dedicated BGM bus. */
        if (preload)
            return ret; /* nothing to start; decode-only requests are moot */
        int mv = music_volume;
        if (bgmdownmode_flag && audio_engine.se_active(0))
            mv /= 2;
        const float vol = (float)mv / 100.0f;
        if (!audio_engine.play_bgm(buffer.data(), length, loop_flag, vol))
            return SOUND_OTHER;
        wave_sample[channel] = (void *)(intptr_t)1;
        fprintf(stderr, "ons: bgm '%s' (%zu bytes, loop=%d, vol=%.2f)\n",
                filename, length, loop_flag ? 1 : 0, (double)vol);
        return ret;
    }

    float vol = (float)DEFAULT_VOLUME / 100.0f;
    if (channel >= 0 && channel < ONS_MIX_CHANNELS)
        vol = (float)channelvolumes[channel] / 100.0f;
    else if (channel == MIX_CLICKVOICE_CHANNEL)
        vol = (float)se_volume / 100.0f;

    if (!audio_engine.play_se(buffer.data(), length, loop_flag, channel, vol,
                              preload))
        return SOUND_OTHER;

    wave_sample[channel] = (void *)(intptr_t)1;
    if (channel == 0 || channel == MIX_CLICKVOICE_CHANNEL || debug_level > 0)
        fprintf(stderr, "ons: se[%d] '%s' (%zu bytes, loop=%d%s)\n", channel,
                filename, length, loop_flag ? 1 : 0,
                preload ? ", preload" : "");
    return ret;
}

void ONScripterLabel::playCDAudio()
{
    if (!audio_open_flag)
        return;

    /* No CD device: fall back to cd/trackNN.{mp3,ogg,wav} (original path). */
    char filename[256];
    sprintf(filename, "cd%ctrack%2.2d.mp3", DELIMITER, current_cd_track);
    int ret = playSound(filename, SOUND_MP3, cd_play_loop_flag,
                        MIX_BGM_CHANNEL);
    if (ret == SOUND_MP3)
        return;

    sprintf(filename, "cd%ctrack%2.2d.ogg", DELIMITER, current_cd_track);
    ret = playSound(filename, SOUND_OGG_STREAMING, cd_play_loop_flag,
                    MIX_BGM_CHANNEL);
    if (ret == SOUND_OGG_STREAMING)
        return;

    sprintf(filename, "cd%ctrack%2.2d.wav", DELIMITER, current_cd_track);
    playSound(filename, SOUND_WAVE, cd_play_loop_flag, MIX_BGM_CHANNEL);
}

int ONScripterLabel::playingMusic()
{
    if (!audio_open_flag)
        return 0;
    return (audio_engine.bgm_playing() || audio_engine.bgm_fading()) ? 1 : 0;
}

int ONScripterLabel::setCurMusicVolume(int volume)
{
    if (!audio_open_flag)
        return 0;

    int mv = volume;
    if (bgmdownmode_flag && audio_engine.se_active(0))
        mv /= 2;
    if (mv < 0)
        mv = 0;
    else if (mv > DEFAULT_VOLUME)
        mv = DEFAULT_VOLUME;
    audio_engine.set_bgm_volume((float)mv / 100.0f);
    return 0;
}

int ONScripterLabel::setVolumeMute(bool do_mute)
{
    if (!audio_open_flag)
        return 0;
    audio_engine.set_muted(do_mute);
    return 0;
}

void ONScripterLabel::stopBGM(bool continue_flag)
{
    audio_engine.stop_bgm(0);

    wave_sample[MIX_BGM_CHANNEL] = NULL;
    wave_sample[MIX_LOOPBGM_CHANNEL0] = NULL;
    wave_sample[MIX_LOOPBGM_CHANNEL1] = NULL;

    if (!continue_flag) {
        setStr(&music_file_name, NULL);
        music_play_loop_flag = false;
        setStr(&seqmusic_file_name, NULL);
        seqmusic_play_loop_flag = false;
        current_cd_track = -1;
    }
}

void ONScripterLabel::stopDWAVE(int channel)
{
    if (!audio_open_flag)
        return;

    if (channel < 0)
        channel = 0;
    else if (channel >= ONS_MIX_CHANNELS)
        channel = ONS_MIX_CHANNELS - 1;

    audio_engine.stop_se(channel);
    if (wave_sample[channel]) {
        /* Preloaded samples are kept for a later dwaveplay.  The voice
         * channel is always released (bgm-down bookkeeping relies on it). */
        if (!channel_preloaded[channel] || channel == 0) {
            wave_sample[channel] = NULL;
            channel_preloaded[channel] = false;
        }
    }
    if (channel == 0 && bgmdownmode_flag)
        setCurMusicVolume(music_volume);
}

void ONScripterLabel::stopAllDWAVE()
{
    if (!audio_open_flag)
        return;

    for (int ch = 0; ch < ONS_MIX_CHANNELS; ch++) {
        audio_engine.stop_se(ch);
        if (wave_sample[ch] && (!channel_preloaded[ch] || ch == 0))
            wave_sample[ch] = NULL;
    }
    if (bgmdownmode_flag)
        setCurMusicVolume(music_volume);
}

void ONScripterLabel::playClickVoice()
{
    if (clickstr_state == CLICK_NEWPAGE) {
        if (clickvoice_file_name[CLICKVOICE_NEWPAGE])
            playSound(clickvoice_file_name[CLICKVOICE_NEWPAGE],
                      SOUND_WAVE | SOUND_OGG, false, MIX_CLICKVOICE_CHANNEL);
    } else if (clickstr_state == CLICK_WAIT) {
        if (clickvoice_file_name[CLICKVOICE_NORMAL])
            playSound(clickvoice_file_name[CLICKVOICE_NORMAL],
                      SOUND_WAVE | SOUND_OGG, false, MIX_CLICKVOICE_CHANNEL);
    }
}

/* Movies are restored in S5 on top of ons::MoviePlayer (FFmpeg soft decode). */
void ONScripterLabel::setMovieSurround(const Rect &movie_rect)
{
    if (!screen_surface)
        return;

    const int sw = screen_surface->w;
    const int sh = screen_surface->h;
    int x0 = movie_rect.x;
    int y0 = movie_rect.y;
    int x1 = movie_rect.x + movie_rect.w;
    int y1 = movie_rect.y + movie_rect.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;

    if (!surround_rects)
        surround_rects = new Rect[4];

    /* Bands above, below, left and right of the movie rectangle. */
    surround_rects[0] = Rect{0, 0, sw, y0};
    surround_rects[1] = Rect{0, y1, sw, sh - y1};
    surround_rects[2] = Rect{0, y0, x0, y1 - y0};
    surround_rects[3] = Rect{x1, y0, sw - x1, y1 - y0};
}

void ONScripterLabel::clearMovieSurround()
{
    if (surround_rects) {
        delete[] surround_rects;
        surround_rects = NULL;
    }
}

/* The movie decoded straight into screen_surface.  Clear it explicitly and
 * present once: a later flush() may be REFRESH_NONE_MODE, and then the last
 * video frame would stay frozen until the script happens to redraw. */
void ONScripterLabel::clearMovieFrame()
{
    if (!screen_surface) return;
    Rect full = {0, 0, (Uint16)screen_surface->w, (Uint16)screen_surface->h};
    screen_surface->fill(full, 0xff000000u);
    renderer_.present(*screen_surface, nullptr);
}

bool ONScripterLabel::runMovieSyncLoop(bool clickable)
{
    bool quit = false;
    const uint64_t started_ms = ons::ticks_ms();
    const double duration_ms = movie_player.duration() * 1000.0;

    while (movie_player.playing()) {
        /* Safety net: if a decoder/audio clock stalls at EOS, do not leave
         * the last frame on screen forever. */
        if (duration_ms > 0.0 &&
            (double)(ons::ticks_ms() - started_ms) > duration_ms + 1000.0) {
            fprintf(stderr, "ons: movie: duration guard stopped playback\n");
            break;
        }
        renderer_.pump_test_input();
        InputEvent event;
        while (input_.poll(event)) {
            if (event.type == EventType::Quit) {
                renderer_.request_quit();
                quit = true;
                break;
            }
            const bool skip_key =
                event.type == EventType::KeyDown &&
                (event.key.keysym.sym == KEY_ESCAPE ||
                 event.key.keysym.sym == KEY_RETURN ||
                 event.key.keysym.sym == KEY_SPACE);
            if (clickable && (event.type == EventType::MouseButtonDown ||
                              skip_key)) {
                movie_player.stop();
                break;
            }
            /* Keep timers / audio notifications flowing while the movie
             * blocks the script (BGM fade events and the like). */
            flushEventSub(event);
        }
        if (quit || !movie_player.playing())
            break;
        if (!movie_player.update(true))
            break;
    }

    movie_player.stop();
    clearMovieSurround();
    clearMovieFrame();
    /* The movie drew straight into the screen; force the next flush to
     * repaint everything from the engine's own layers.  Do it here rather
     * than waiting for the script's next draw: a following wait command
     * would otherwise leave the last video frame frozen on screen. */
    dirty_rect.fill(screen_width, screen_height);
    flush(refreshMode());
    if (quit) {
        /* The Quit event was consumed here; terminate the same way the
         * event loop would have. */
        endCommand();
    }
    return !quit;
}

int ONScripterLabel::playMPEG(const char *filename, bool async_flag, bool use_pos,
                              int xpos, int ypos, int width, int height)
{
    if (!filename || !*filename)
        return 0;

    if (async_movie) {
        stopMovie(async_movie);
        async_movie = NULL;
    }

    const size_t length = script_h.cBR->getFileLength(filename);
    if (length == 0) {
        fprintf(stderr, "ons: mpeg '%s' not found\n", filename);
        return 0;
    }
    std::vector<unsigned char> buffer(length);
    if (script_h.cBR->getFile(filename, buffer.data()) != length) {
        fprintf(stderr, "ons: mpeg '%s' could not be read\n", filename);
        return 0;
    }

    movie_player.set_audio_engine(&audio_engine);
    movie_player.set_renderer(&renderer_);
    movie_player.set_volume(volume_on_flag ? 1.0f : 0.0f);

    if (!movie_player.open(buffer.data(), length)) {
        fprintf(stderr, "ons: movie: cannot play '%s'\n", filename);
        return 0;
    }

    Rect rect;
    if (use_pos) {
        rect.x = xpos;
        rect.y = ypos;
        rect.w = width;
        rect.h = height;
    } else {
        rect.x = 0;
        rect.y = 0;
        rect.w = screen_surface->w;
        rect.h = screen_surface->h;
    }
    movie_player.set_output(screen_surface, rect);
    setMovieSurround(rect);

    if (!movie_player.play(movie_loop_flag, movie_click_flag)) {
        fprintf(stderr, "ons: movie: '%s' failed to start\n", filename);
        return 0;
    }

    fprintf(stderr,
            "ons: movie '%s' %s %dx%d %.2fs audio=%d%s%s\n", filename,
            async_flag ? "async" : "sync", movie_player.width(),
            movie_player.height(), movie_player.duration(),
            movie_player.has_audio() ? 1 : 0, movie_loop_flag ? " loop" : "",
            movie_click_flag ? " click" : "");

    if (async_flag) {
        async_movie = &movie_player;
        async_movie_rect = rect;
        return 0;
    }

    return runMovieSyncLoop(movie_click_flag) ? 0 : 1;
}

int ONScripterLabel::playAVI(const char *filename, bool click_flag)
{
    if (!filename || !*filename)
        return 0;

    if (async_movie) {
        stopMovie(async_movie);
        async_movie = NULL;
    }

    const size_t length = script_h.cBR->getFileLength(filename);
    if (length == 0) {
        fprintf(stderr, "ons: avi '%s' not found\n", filename);
        return 0;
    }
    std::vector<unsigned char> buffer(length);
    if (script_h.cBR->getFile(filename, buffer.data()) != length) {
        fprintf(stderr, "ons: avi '%s' could not be read\n", filename);
        return 0;
    }

    movie_player.set_audio_engine(&audio_engine);
    movie_player.set_renderer(&renderer_);
    movie_player.set_volume(volume_on_flag ? 1.0f : 0.0f);

    if (!movie_player.open(buffer.data(), length)) {
        fprintf(stderr, "ons: avi: cannot play '%s'\n", filename);
        return 0;
    }

    Rect rect = {0, 0, screen_surface->w, screen_surface->h};
    movie_player.set_output(screen_surface, rect);
    setMovieSurround(rect);

    if (!movie_player.play(false, click_flag)) {
        fprintf(stderr, "ons: avi: '%s' failed to start\n", filename);
        return 0;
    }

    fprintf(stderr, "ons: avi '%s' %dx%d %.2fs audio=%d%s\n", filename,
            movie_player.width(), movie_player.height(),
            movie_player.duration(), movie_player.has_audio() ? 1 : 0,
            click_flag ? " click" : "");

    return runMovieSyncLoop(click_flag) ? 0 : 1;
}

void ONScripterLabel::stopMovie(void *mpeg)
{
    if (!mpeg)
        return;

    if (mpeg == static_cast<void *>(&movie_player)) {
        movie_player.stop();
        clearMovieSurround();
        clearMovieFrame();
        if (async_movie == mpeg) {
            async_movie = NULL;
            fprintf(stderr, "ons: movie: stopped (%lld frames shown)\n",
                    (long long)movie_player.presented_frames());
        }
        dirty_rect.fill(screen_width, screen_height);
        return;
    }

    /* No other movie backends exist in this port. */
}
