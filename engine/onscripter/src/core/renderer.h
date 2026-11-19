/* -*- C++ -*-
 *
 *  renderer.h - window, renderer and streaming texture presentation
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

#ifndef ONS_CORE_RENDERER_H
#define ONS_CORE_RENDERER_H

#include "bitmap.h"
#include "types.h"

#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Surface;

namespace ons {

class InputSystem;

/* Writes the bitmap as a PNG; implemented by the host (ons_snapshot.cpp). */
typedef bool (*SnapshotFn)(const Bitmap &bitmap, const char *path);

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(int width, int height, const char *title,
              bool toggle_fullscreen, bool offscreen);
    void shutdown();

    bool set_mode(int width, int height, bool fullscreen);

    void present(const Bitmap &bitmap, const Rect *rect = nullptr);
    void present_all(const Bitmap &bitmap);

    bool toggle_fullscreen();
    void set_caption(const char *title);
    void set_icon(const Bitmap &icon);
    void minimize();

    void show_cursor(bool show);
    void warp_mouse(int x, int y);
    void get_mouse(int *x, int *y) const;

    void desktop_size(int *w, int *h) const;
    void window_to_logical(float wx, float wy, int *lx, int *ly) const;
    void logical_size(int *w, int *h) const;

    int frame_count() const { return frame_count_; }
    bool offscreen() const { return offscreen_; }
    void request_quit();
    bool quit_requested() const { return quit_requested_; }

    /* Frame limiting, auto-click injection and the hang watchdog. */
    void tick();

    void set_input(InputSystem *input) { input_ = input; }
    void set_snapshot_hook(SnapshotFn fn) { snapshot_ = fn; }
    void set_offscreen(bool offscreen) { offscreen_ = offscreen; }

    /* Test host options (see --ons-*). */
    void set_test_options(int frames, const char *dump_path, int dump_at,
                          int auto_click_ms);
    /* Stop the auto clicker once this frame has been presented (0 = never). */
    void set_auto_click_until(int frame);
    /* Injects scripted input while a blocking movie owns the main loop
     * (the movie itself does not call tick()). */
    void pump_test_input();
    /* Logical screen position used by injected clicks (--ons-click X,Y). */
    void set_click_point(int x, int y);
    void dump_if_needed();

    /* Frame-scheduled input injection (test only, see --ons-*-at). */
    void add_click_at(int frame, int x, int y);
    void add_right_click_at(int frame);
    /* The script consumed a right click (-1 from textbtnwait) and will open
     * its own menu; stop the headless test harness from retrying. */
    void notify_right_click_consumed() { right_click_pending_ = false; hold_frames_ = true; }
    void add_key_at(int frame, int key);
    /* Called by the engine when the script rmenu has been entered. */
    void notify_menu_opened() { menu_open_ = true; right_click_pending_ = false; }

    bool valid() const { return offscreen_ || texture_ != nullptr; }
    const char *error() const { return error_; }

private:
    void compute_present_rect(float *x, float *y, float *w, float *h) const;
    void destroy_video();
    void dump_now();
    void push_click();
    void sync_viewport();

    enum class TestEventKind { Click, RightClick, Key };
    struct TestEvent {
        int frame = 0;
        TestEventKind kind = TestEventKind::Click;
        int x = 0;
        int y = 0;
        int key = 0;
    };

    void add_test_event(const TestEvent &event);
    void run_due_test_events();
    void report_memory(const char *tag);

    SDL_Window *window_;
    SDL_Renderer *renderer_;
    SDL_Texture *texture_;
    SDL_Surface *icon_surface_;

    int logical_w_;
    int logical_h_;
    int window_w_;
    int window_h_;
    bool offscreen_;
    bool fullscreen_;
    bool toggle_fullscreen_;
    bool quit_requested_;
    bool quit_pushed_;
    bool dumped_;
    bool cursor_visible_;

    int frame_count_;
    int frames_limit_;
    int dump_at_;
    int auto_click_ms_;
    int click_x_;
    int click_y_;
    bool click_set_;
    bool hold_frames_;
    bool pad_frames_;
    bool menu_open_;
    bool right_click_pending_;
    int right_click_next_frame_;
    int auto_click_until_;
    char *dump_path_;

    std::vector<TestEvent> test_events_;
    size_t next_test_event_;

    uint32_t start_ms_;
    uint32_t last_present_ms_;
    uint32_t last_click_ms_;

    const Bitmap *last_frame_;
    SnapshotFn snapshot_;
    InputSystem *input_;

    char error_[256];
};

} // namespace ons


using ons::Renderer;

#endif /* ONS_CORE_RENDERER_H */
