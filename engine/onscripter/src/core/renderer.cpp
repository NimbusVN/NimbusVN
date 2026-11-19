/* -*- C++ -*-
 *
 *  renderer.cpp - SDL3 window / renderer / streaming texture presenter
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "renderer.h"

#include "clock.h"
#include "input.h"
#include "memdiag.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace ons {

namespace {

/* Process working set / RSS, polled every 1000 frames by long-run tests.
 * The Windows entry point is resolved dynamically so the build keeps its
 * current link list (no psapi.lib requirement). */
size_t query_working_set()
{
#if defined(_WIN32)
    typedef BOOL (WINAPI *GetProcessMemoryInfoFn)(HANDLE,
                                                  PROCESS_MEMORY_COUNTERS *,
                                                  DWORD);
    static GetProcessMemoryInfoFn fn = NULL;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE psapi = LoadLibraryA("psapi.dll");
        if (psapi) {
            fn = (GetProcessMemoryInfoFn)GetProcAddress(psapi,
                                                        "GetProcessMemoryInfo");
        }
        if (!fn) {
            HMODULE k32 = GetModuleHandleA("kernel32.dll");
            if (k32) {
                fn = (GetProcessMemoryInfoFn)GetProcAddress(k32,
                                                            "K32GetProcessMemoryInfo");
            }
        }
    }
    if (fn) {
        PROCESS_MEMORY_COUNTERS pmc;
        if (fn(GetCurrentProcess(), &pmc, sizeof(pmc)))
            return (size_t)pmc.WorkingSetSize;
    }
    return 0;
#elif defined(__linux__)
    FILE *fp = fopen("/proc/self/statm", "r");
    if (!fp)
        return 0;
    unsigned long total = 0, resident = 0;
    if (fscanf(fp, "%lu %lu", &total, &resident) != 2)
        resident = 0;
    fclose(fp);
    return (size_t)resident * (size_t)sysconf(_SC_PAGESIZE);
#else
    return 0;
#endif
}

} // namespace

Renderer::Renderer()
    : window_(nullptr), renderer_(nullptr), texture_(nullptr), icon_surface_(nullptr),
      logical_w_(0), logical_h_(0), window_w_(0), window_h_(0),
      offscreen_(false), fullscreen_(false), toggle_fullscreen_(false),
      quit_requested_(false), quit_pushed_(false), dumped_(false),
      cursor_visible_(true), frame_count_(0), frames_limit_(0), dump_at_(0),
      auto_click_ms_(0), click_x_(0), click_y_(0), click_set_(false),
      hold_frames_(false), pad_frames_(false), menu_open_(false),
      right_click_pending_(false), right_click_next_frame_(0),
      auto_click_until_(0), dump_path_(nullptr), next_test_event_(0),
      start_ms_(0), last_present_ms_(0),
      last_click_ms_(0), last_frame_(nullptr), snapshot_(nullptr), input_(nullptr)
{
    error_[0] = '\0';
}

Renderer::~Renderer()
{
    shutdown();
    if (dump_path_) {
        delete[] dump_path_;
        dump_path_ = nullptr;
    }
}

bool Renderer::init(int width, int height, const char *title,
                    bool toggle_fullscreen, bool offscreen)
{
    logical_w_ = width;
    logical_h_ = height;
    offscreen_ = offscreen || offscreen_;
    toggle_fullscreen_ = toggle_fullscreen;
    start_ms_ = ticks_ms();
    last_click_ms_ = start_ms_;

    if (offscreen_) {
        fprintf(stderr, "ons: offscreen mode, logical %dx%d\n", width, height);
        if (frames_limit_ >= 1000)
            report_memory("start");
        return true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        snprintf(error_, sizeof(error_), "SDL_Init failed: %s", SDL_GetError());
        fprintf(stderr, "ons: %s\n", error_);
        return false;
    }

    int dw = 0, dh = 0;
    desktop_size(&dw, &dh);

    int scale = 2;
    if (width * scale > dw * 9 / 10 || height * scale > dh * 9 / 10) {
        scale = 1;
        if (width > dw * 9 / 10 || height > dh * 9 / 10) {
            float sx = (float)(dw * 9 / 10) / (float)width;
            float sy = (float)(dh * 9 / 10) / (float)height;
            float s = std::max(1.0f, std::min(sx, sy));
            scale = (int)s;
        }
    }

    window_w_ = width * scale;
    window_h_ = height * scale;

    window_ = SDL_CreateWindow(title ? title : "ONScripter",
                               window_w_, window_h_, SDL_WINDOW_RESIZABLE);
    if (!window_) {
        snprintf(error_, sizeof(error_), "SDL_CreateWindow failed: %s", SDL_GetError());
        fprintf(stderr, "ons: %s\n", error_);
        return false;
    }

    if (toggle_fullscreen_)
        SDL_SetWindowFullscreen(window_, true);

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        snprintf(error_, sizeof(error_), "SDL_CreateRenderer failed: %s", SDL_GetError());
        fprintf(stderr, "ons: %s\n", error_);
        destroy_video();
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);

    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture_) {
        snprintf(error_, sizeof(error_), "SDL_CreateTexture failed: %s", SDL_GetError());
        fprintf(stderr, "ons: %s\n", error_);
        destroy_video();
        return false;
    }
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);

#if !defined(__ANDROID__)
    /* Desktop keeps SDL text input enabled for keyboard input.  On Android
     * SDL_StartTextInput raises the soft keyboard immediately, and the
     * engine's `input` command is not implemented yet, so do not start it. */
    SDL_StartTextInput(window_);
#endif
    sync_viewport();

    fprintf(stderr, "ons: window %dx%d (x%d), logical %dx%d\n",
            window_w_, window_h_, scale, width, height);
    if (frames_limit_ >= 1000)
        report_memory("start");
    return true;
}

void Renderer::destroy_video()
{
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

void Renderer::shutdown()
{
    dump_if_needed();
    destroy_video();
    if (icon_surface_) {
        SDL_DestroySurface(icon_surface_);
        icon_surface_ = nullptr;
    }
    if (window_ == nullptr && renderer_ == nullptr && !offscreen_)
        SDL_Quit();
}

bool Renderer::set_mode(int width, int height, bool fullscreen)
{
    if (width <= 0 || height <= 0)
        return false;

    if (width == logical_w_ && height == logical_h_ && texture_) {
        fullscreen_ = fullscreen;
        if (window_)
            SDL_SetWindowFullscreen(window_, fullscreen);
        sync_viewport();
        return true;
    }

    logical_w_ = width;
    logical_h_ = height;
    fullscreen_ = fullscreen;

    if (offscreen_)
        return true;

    if (!renderer_) {
        return init(width, height, "ONScripter", toggle_fullscreen_, false);
    }

    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }

    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture_) {
        snprintf(error_, sizeof(error_), "SDL_CreateTexture failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);

    if (window_) {
        SDL_SetWindowSize(window_, width, height);
        SDL_SetWindowFullscreen(window_, fullscreen);
    }
    sync_viewport();
    return true;
}

void Renderer::sync_viewport()
{
    if (!window_)
        return;
    SDL_GetWindowSize(window_, &window_w_, &window_h_);
    if (input_)
        input_->set_viewport(window_w_, window_h_, logical_w_, logical_h_);
}

void Renderer::compute_present_rect(float *x, float *y, float *w, float *h) const
{
    int out_w = logical_w_ > 0 ? logical_w_ : 640;
    int out_h = logical_h_ > 0 ? logical_h_ : 480;

    *x = 0.0f;
    *y = 0.0f;
    *w = (float)out_w;
    *h = (float)out_h;

    int rw = 0, rh = 0;
    if (renderer_) {
        if (!SDL_GetRenderOutputSize(renderer_, &rw, &rh) || rw <= 0 || rh <= 0)
            return;
    } else {
        rw = window_w_ > 0 ? window_w_ : out_w;
        rh = window_h_ > 0 ? window_h_ : out_h;
    }

    float sx = (float)rw / (float)out_w;
    float sy = (float)rh / (float)out_h;
    float s = std::min(sx, sy);

    *w = std::round((float)out_w * s);
    *h = std::round((float)out_h * s);
    *x = std::floor(((float)rw - *w) * 0.5f);
    *y = std::floor(((float)rh - *h) * 0.5f);
}

void Renderer::present(const Bitmap &bitmap, const Rect *rect)
{
    if (!bitmap.pixels)
        return;

    last_frame_ = &bitmap;

    /* Headless runs have no vsync.  Pace presents so elapsed-time driven
     * logic (effects, text speed) behaves like a 60fps window. */
    if (offscreen_) {
        const uint32_t now = ticks_ms();
        if (last_present_ms_ != 0) {
            const uint32_t elapsed = now - last_present_ms_;
            if (elapsed < 15u)
                sleep_ms(15u - elapsed);
        }
    }

    ++frame_count_;
    last_present_ms_ = ticks_ms();

    /* Test-only scripted input: an event scheduled for frame N is queued
     * right after that frame is presented, so the following wait picks it up
     * deterministically regardless of wall-clock timing. */
    run_due_test_events();

    if (frames_limit_ >= 1000 && frame_count_ % 1000 == 0)
        report_memory("frame");
    /* Object accounting is compiled in but only runs with --debug. */
    if (frame_count_ % 100 == 0)
        memdiag::dump("frame");

    if (texture_ && renderer_) {
        if (!rect) {
            SDL_UpdateTexture(texture_, nullptr, bitmap.pixels, bitmap.pitch);
        } else {
            /* Clamp to the logical screen; effect code occasionally hands us
             * rectangles that extend past the edge. */
            int x0 = rect->x < 0 ? 0 : rect->x;
            int y0 = rect->y < 0 ? 0 : rect->y;
            int x1 = rect->x + rect->w;
            int y1 = rect->y + rect->h;
            if (x1 > bitmap.w) x1 = bitmap.w;
            if (y1 > bitmap.h) y1 = bitmap.h;
            if (x1 > x0 && y1 > y0) {
                SDL_Rect src;
                src.x = x0;
                src.y = y0;
                src.w = x1 - x0;
                src.h = y1 - y0;
                const uint8_t *pixels = (const uint8_t *)bitmap.pixels +
                                        (size_t)src.y * (size_t)bitmap.pitch +
                                        (size_t)src.x * 4u;
                SDL_UpdateTexture(texture_, &src, pixels, bitmap.pitch);
            }
        }

        float dx, dy, dw, dh;
        compute_present_rect(&dx, &dy, &dw, &dh);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_FRect dst;
        dst.x = dx;
        dst.y = dy;
        dst.w = dw;
        dst.h = dh;
        SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
        SDL_RenderPresent(renderer_);
    }

    if (dump_at_ > 0 && frame_count_ == dump_at_)
        dump_now();

    if (frames_limit_ > 0 && frame_count_ >= frames_limit_) {
        if (dump_at_ <= 0)
            dump_if_needed();
        request_quit();
    }
}

void Renderer::present_all(const Bitmap &bitmap)
{
    present(bitmap, nullptr);
}

bool Renderer::toggle_fullscreen()
{
    if (!window_)
        return false;
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_);
    sync_viewport();
    return true;
}

void Renderer::set_caption(const char *title)
{
    if (window_ && title)
        SDL_SetWindowTitle(window_, title);
}

void Renderer::set_icon(const Bitmap &icon)
{
    if (!window_ || !icon.pixels)
        return;

    if (icon_surface_) {
        SDL_DestroySurface(icon_surface_);
        icon_surface_ = nullptr;
    }

    icon_surface_ = SDL_CreateSurface(icon.w, icon.h, SDL_PIXELFORMAT_ARGB8888);
    if (!icon_surface_)
        return;

    const int dst_pitch = icon_surface_->pitch;
    uint8_t *dst = (uint8_t *)icon_surface_->pixels;
    for (int y = 0; y < icon.h; ++y) {
        memcpy(dst + (size_t)y * (size_t)dst_pitch,
               (const uint8_t *)icon.pixels + (size_t)y * (size_t)icon.pitch,
               (size_t)icon.w * 4u);
    }
    SDL_SetWindowIcon(window_, icon_surface_);
}

void Renderer::minimize()
{
    if (window_)
        SDL_MinimizeWindow(window_);
}

void Renderer::show_cursor(bool show)
{
    if (show == cursor_visible_)
        return;
    cursor_visible_ = show;
    if (show)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
}

void Renderer::warp_mouse(int x, int y)
{
    if (!window_ || logical_w_ <= 0 || logical_h_ <= 0)
        return;

    float dx, dy, dw, dh;
    compute_present_rect(&dx, &dy, &dw, &dh);
    if (dw <= 0.0f || dh <= 0.0f)
        return;

    float wx = dx + (float)x * dw / (float)logical_w_;
    float wy = dy + (float)y * dh / (float)logical_h_;
    SDL_WarpMouseInWindow(window_, wx, wy);
}

void Renderer::get_mouse(int *x, int *y) const
{
    float mx = 0.0f, my = 0.0f;
    if (window_)
        SDL_GetMouseState(&mx, &my);
    int lx = 0, ly = 0;
    window_to_logical(mx, my, &lx, &ly);
    if (x) *x = lx;
    if (y) *y = ly;
}

void Renderer::desktop_size(int *w, int *h) const
{
    int dw = 640, dh = 480;
    if (!offscreen_) {
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display);
        if (mode) {
            dw = mode->w;
            dh = mode->h;
        }
    }
    if (w) *w = dw;
    if (h) *h = dh;
}

void Renderer::window_to_logical(float wx, float wy, int *lx, int *ly) const
{
    float dx, dy, dw, dh;
    compute_present_rect(&dx, &dy, &dw, &dh);

    int x = 0, y = 0;
    if (dw > 0.0f && dh > 0.0f && logical_w_ > 0 && logical_h_ > 0) {
        x = (int)std::lround((wx - dx) * (float)logical_w_ / dw);
        y = (int)std::lround((wy - dy) * (float)logical_h_ / dh);
    }
    x = std::max(0, std::min(x, logical_w_ > 0 ? logical_w_ - 1 : 0));
    y = std::max(0, std::min(y, logical_h_ > 0 ? logical_h_ - 1 : 0));
    if (lx) *lx = x;
    if (ly) *ly = y;
}

void Renderer::logical_size(int *w, int *h) const
{
    if (w) *w = logical_w_;
    if (h) *h = logical_h_;
}

void Renderer::request_quit()
{
    if (quit_requested_)
        return;
    quit_requested_ = true;

    /* Frame limits and the idle watchdog terminate through the engine's
     * Quit event, which calls endCommand() -> exit().  Save the requested
     * screenshot first so --ons-dump-frame works for those paths too. */
    dump_if_needed();

    if (!quit_pushed_) {
        quit_pushed_ = true;
        push_quit();
    }
}

void Renderer::push_click()
{
    if (quit_pushed_)
        return;

    /* Injected clicks carry a motion event first: the engine only resolves
     * current_over_button in mouseMoveEvent(), and mousePressEvent() reads
     * that cached value.  Without the motion a scripted click would always
     * hit button 0 no matter where it was aimed. */
    int x = click_set_ ? click_x_ : (logical_w_ > 0 ? logical_w_ / 2 : 320);
    int y = click_set_ ? click_y_ : (logical_h_ > 0 ? logical_h_ / 2 : 240);
    push_mouse_motion(x, y);
    push_mouse_button(x, y, BUTTON_LEFT, true);
    push_mouse_button(x, y, BUTTON_LEFT, false);
}

void Renderer::dump_now()
{
    if (dumped_)
        return;
    dumped_ = true;
    if (!dump_path_ || !*dump_path_ || !last_frame_)
        return;

    /* The engine's screen surface has no meaningful alpha channel: sprites
     * are composited into it with the classic ONScripter routines, which
     * leave the alpha byte at zero on some paths.  SDL ignored it because the
     * display surface is opaque; a PNG dump must do the same, otherwise the
     * screenshot looks empty. */
    Bitmap opaque_frame(*last_frame_);
    if (!opaque_frame.pixels) {
        fprintf(stderr, "ons: failed to copy frame for %s\n", dump_path_);
        return;
    }
    const size_t count = (size_t)opaque_frame.w * (size_t)opaque_frame.h;
    for (size_t i = 0; i < count; ++i)
        opaque_frame.pixels[i] |= 0xff000000u;

    if (snapshot_ && snapshot_(opaque_frame, dump_path_))
        fprintf(stderr, "ons: dumped frame %d to %s (%dx%d)\n",
                frame_count_, dump_path_, opaque_frame.w, opaque_frame.h);
    else
        fprintf(stderr, "ons: failed to write %s\n", dump_path_);
}

void Renderer::dump_if_needed()
{
    if (dump_path_ && !dumped_)
        dump_now();
}

void Renderer::set_test_options(int frames, const char *dump_path, int dump_at,
                                int auto_click_ms)
{
    frames_limit_ = frames > 0 ? frames : 0;
    dump_at_ = dump_at > 0 ? dump_at : 0;
    auto_click_ms_ = auto_click_ms > 0 ? auto_click_ms : 0;

    if (dump_path_) {
        delete[] dump_path_;
        dump_path_ = nullptr;
    }
    if (dump_path && *dump_path) {
        size_t len = strlen(dump_path);
        dump_path_ = new char[len + 1];
        memcpy(dump_path_, dump_path, len + 1);
    }
}

void Renderer::set_click_point(int x, int y)
{
    click_x_ = x;
    click_y_ = y;
    click_set_ = true;
}

void Renderer::add_test_event(const TestEvent &event)
{
    /* Keep insertions stable for events sharing a frame. */
    std::vector<TestEvent>::iterator it = test_events_.begin();
    while (it != test_events_.end() && it->frame <= event.frame)
        ++it;
    test_events_.insert(it, event);
    pad_frames_ = true;
}

void Renderer::set_auto_click_until(int frame)
{
    auto_click_until_ = frame > 0 ? frame : 0;
}

void Renderer::pump_test_input()
{
    if (quit_pushed_)
        return;
    const uint32_t now = ticks_ms();
    if (auto_click_until_ > 0 && frame_count_ >= auto_click_until_)
        auto_click_ms_ = 0;
    if (auto_click_ms_ > 0 &&
        now - last_click_ms_ >= (uint32_t)auto_click_ms_) {
        last_click_ms_ = now;
        push_click();
    }
}

void Renderer::add_click_at(int frame, int x, int y)
{
    TestEvent event;
    event.frame = frame;
    event.kind = TestEventKind::Click;
    event.x = x;
    event.y = y;
    add_test_event(event);
}

void Renderer::add_right_click_at(int frame)
{
    TestEvent event;
    event.frame = frame;
    event.kind = TestEventKind::RightClick;
    add_test_event(event);
}

void Renderer::add_key_at(int frame, int key)
{
    TestEvent event;
    event.frame = frame;
    event.kind = TestEventKind::Key;
    event.key = key;
    add_test_event(event);
}

void Renderer::run_due_test_events()
{
    while (next_test_event_ < test_events_.size() &&
           test_events_[next_test_event_].frame <= frame_count_) {
        const TestEvent &event = test_events_[next_test_event_++];
        switch (event.kind) {
        case TestEventKind::Click: {
            fprintf(stderr, "ons: injected click at frame %d (%d,%d)\n",
                    event.frame, event.x, event.y);
            push_mouse_motion(event.x, event.y);
            push_mouse_button(event.x, event.y, BUTTON_LEFT, true);
            push_mouse_button(event.x, event.y, BUTTON_LEFT, false);
            break;
        }
        case TestEventKind::RightClick: {
            if (menu_open_) {
                right_click_pending_ = false;
                break;
            }
            int x = click_set_ ? click_x_
                               : (logical_w_ > 0 ? logical_w_ / 2 : 320);
            int y = click_set_ ? click_y_
                               : (logical_h_ > 0 ? logical_h_ / 2 : 240);
            fprintf(stderr, "ons: injected right click at frame %d (%d,%d)\n",
                    event.frame, x, y);
            push_mouse_motion(x, y);
            push_mouse_button(x, y, BUTTON_RIGHT, true);
            push_mouse_button(x, y, BUTTON_RIGHT, false);
            /* The right-click menu is modal.  Stop the auto clicker and
             * retry periodically until the engine opens its menu or the
             * script consumes -1 and opens its own (notify_right_click_-
             * consumed).  Without the retry a click during text output is
             * simply lost. */
            auto_click_ms_ = 0;
            hold_frames_ = true;
            right_click_pending_ = true;
            right_click_next_frame_ = frame_count_ + 20;
            break;
        }
        case TestEventKind::Key: {
            fprintf(stderr, "ons: injected key %d at frame %d\n",
                    event.key, event.frame);
            uint16_t unicode = 0;
            if (event.key >= 0x20 && event.key < 0x7f)
                unicode = (uint16_t)event.key;
            push_key(event.key, true, unicode);
            push_key(event.key, false, unicode);
            break;
        }
        }
    }

    /* Retry an ignored right click (text output / effect / normal mode)
     * every 20 frames until the engine opens its menu, the script consumes
     * -1, or the run ends. */
    if (right_click_pending_ && !menu_open_ &&
        frame_count_ >= right_click_next_frame_) {
        int x = click_set_ ? click_x_ : (logical_w_ > 0 ? logical_w_ / 2 : 320);
        int y = click_set_ ? click_y_ : (logical_h_ > 0 ? logical_h_ / 2 : 240);
        fprintf(stderr, "ons: retrying right click at frame %d\n", frame_count_);
        push_mouse_motion(x, y);
        push_mouse_button(x, y, BUTTON_RIGHT, true);
        push_mouse_button(x, y, BUTTON_RIGHT, false);
        right_click_next_frame_ = frame_count_ + 20;
    }

    /* Padding only continues past the last scripted event for the modal
     * right-click menu (which never repaints by itself) or while a later
     * scheduled event still needs its frame to arrive. */
    pad_frames_ = hold_frames_ || right_click_pending_ ||
                  next_test_event_ < test_events_.size();
}

void Renderer::report_memory(const char *tag)
{
    const size_t bytes = query_working_set();
    if (bytes == 0) {
        fprintf(stderr, "ons: %s frame=%d working_set=unavailable\n",
                tag, frame_count_);
        return;
    }
    fprintf(stderr, "ons: %s frame=%d working_set=%.1f MiB (%llu bytes)\n",
            tag, frame_count_, (double)bytes / (1024.0 * 1024.0),
            (unsigned long long)bytes);
}

void Renderer::tick()
{
    if (quit_pushed_)
        return;

    uint32_t now = ticks_ms();

    if (auto_click_until_ > 0 && frame_count_ >= auto_click_until_)
        auto_click_ms_ = 0;

    if (auto_click_ms_ > 0) {
        if (now - last_click_ms_ >= (uint32_t)auto_click_ms_) {
            last_click_ms_ = now;
            push_click();
        }
    } else if ((pad_frames_ || hold_frames_) && frames_limit_ > 0 &&
               frame_count_ < frames_limit_ && last_frame_ && !quit_pushed_) {
        /* No clicker: keep the test frame clock advancing so a scheduled
         * event's frame is actually reached while the engine waits (it does
         * not repaint by itself in a clickwait or in the modal menu).  Only
         * presentation is driven here; engine timers still fire through the
         * event queue. */
        if (now - last_present_ms_ >= 1u)
            present(*last_frame_);
        return;
    } else if (frames_limit_ > 0) {
        /* Watchdog for frame limited runs: a modal wait with no input and no
         * animation would otherwise never reach the frame limit. */
        if (frame_count_ > 0 && now - last_present_ms_ > 12000) {
            fprintf(stderr, "ons: no frame for 12s, quitting\n");
            request_quit();
        }
        if (frame_count_ == 0 && now - start_ms_ > 30000) {
            fprintf(stderr, "ons: no frame within 30s of startup, quitting\n");
            request_quit();
        }
    }

    if (frames_limit_ > 0 && frame_count_ >= frames_limit_)
        request_quit();
}

} // namespace ons
