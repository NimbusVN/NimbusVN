/* -*- C++ -*-
 *
 *  input.cpp - SDL3 event pump translated to the engine's input events
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "input.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>

namespace ons {

namespace {

std::mutex g_queue_mutex;
std::deque<InputEvent> g_queue;
std::condition_variable g_queue_cv;

int32_t map_keycode(SDL_Keycode kc)
{
    /* Printable ASCII and the control keys the engine uses keep their values. */
    if (kc >= 0x20 && kc < 0x7f)
        return (int32_t)kc;
    if (kc < 0x80)
        return (int32_t)kc; /* backspace/tab/return/escape/delete */

    switch (kc) {
    case SDLK_KP_0: return KEY_KP0;
    case SDLK_KP_1: return KEY_KP1;
    case SDLK_KP_2: return KEY_KP2;
    case SDLK_KP_3: return KEY_KP3;
    case SDLK_KP_4: return KEY_KP4;
    case SDLK_KP_5: return KEY_KP5;
    case SDLK_KP_6: return KEY_KP6;
    case SDLK_KP_7: return KEY_KP7;
    case SDLK_KP_8: return KEY_KP8;
    case SDLK_KP_9: return KEY_KP9;
    case SDLK_KP_PERIOD: return KEY_KP_PERIOD;
    case SDLK_KP_DIVIDE: return KEY_KP_DIVIDE;
    case SDLK_KP_MULTIPLY: return KEY_KP_MULTIPLY;
    case SDLK_KP_MINUS: return KEY_KP_MINUS;
    case SDLK_KP_PLUS: return KEY_KP_PLUS;
    case SDLK_KP_ENTER: return KEY_KP_ENTER;
    case SDLK_KP_EQUALS: return KEY_KP_EQUALS;
    case SDLK_UP: return KEY_UP;
    case SDLK_DOWN: return KEY_DOWN;
    case SDLK_RIGHT: return KEY_RIGHT;
    case SDLK_LEFT: return KEY_LEFT;
    case SDLK_INSERT: return KEY_INSERT;
    case SDLK_HOME: return KEY_HOME;
    case SDLK_END: return KEY_END;
    case SDLK_PAGEUP: return KEY_PAGEUP;
    case SDLK_PAGEDOWN: return KEY_PAGEDOWN;
    case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4:
    case SDLK_F5: case SDLK_F6: case SDLK_F7: case SDLK_F8:
    case SDLK_F9: case SDLK_F10: case SDLK_F11: case SDLK_F12:
    case SDLK_F13: case SDLK_F14: case SDLK_F15:
        return KEY_F1 + (int32_t)(kc - SDLK_F1);
    case SDLK_NUMLOCKCLEAR: return KEY_NUMLOCK;
    case SDLK_CAPSLOCK: return KEY_CAPSLOCK;
    case SDLK_SCROLLLOCK: return KEY_SCROLLOCK;
    case SDLK_RSHIFT: return KEY_RSHIFT;
    case SDLK_LSHIFT: return KEY_LSHIFT;
    case SDLK_RCTRL: return KEY_RCTRL;
    case SDLK_LCTRL: return KEY_LCTRL;
    case SDLK_RALT: return KEY_RALT;
    case SDLK_LALT: return KEY_LALT;
    case SDLK_RGUI: return KEY_RMETA;
    case SDLK_LGUI: return KEY_LMETA;
    case SDLK_MODE: return KEY_MODE;
    case SDLK_HELP: return KEY_HELP;
    case SDLK_PRINTSCREEN: return KEY_PRINT;
    case SDLK_SYSREQ: return KEY_SYSREQ;
    case SDLK_PAUSE: return KEY_BREAK;
    case SDLK_MENU: return KEY_MENU;
    case SDLK_POWER: return KEY_POWER;
    default: break;
    }
    return KEY_UNKNOWN;
}

/* Rough UTF-8 to UCS-2 conversion of the first code point. */
uint16_t first_unicode(const char *text)
{
    if (!text || !*text)
        return 0;
    const unsigned char *p = (const unsigned char *)text;
    if (p[0] < 0x80)
        return p[0];
    if ((p[0] & 0xe0) == 0xc0)
        return (uint16_t)(((p[0] & 0x1f) << 6) | (p[1] & 0x3f));
    if ((p[0] & 0xf0) == 0xe0)
        return (uint16_t)(((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f));
    return 0; /* outside the BMP; the engine only handles UCS-2 */
}

uint16_t ascii_unicode(int32_t sym, uint16_t mod)
{
    if (sym >= 'a' && sym <= 'z') {
        if (mod & SDL_KMOD_SHIFT)
            return (uint16_t)(sym - 'a' + 'A');
        return (uint16_t)sym;
    }
    if (sym >= 0x20 && sym < 0x7f) {
        static const char unshifted[] = "0123456789`-=[]\\;',./";
        static const char shifted[] = ")!@#$%^&*(~_+{}|:\"<>?";
        if ((mod & SDL_KMOD_SHIFT) && sym < 0x7f) {
            for (int i = 0; unshifted[i]; ++i) {
                if (unshifted[i] == (char)sym)
                    return (uint16_t)shifted[i];
            }
        }
        return (uint16_t)sym;
    }
    return 0;
}

enum TranslateKind { T_NONE, T_EVENT, T_KEYDOWN, T_TEXT };

/* ------------------------------------------------------------------ *
 * Android touch gestures (modeled on krkrsdl3's sdl3_app.cpp):
 *   single tap            -> left click
 *   two-finger tap        -> right click (script menu / engine rmenu)
 *   two-finger vertical   -> wheel (lookback / page)
 *   three-finger tap      -> right click
 *   long-press + drag     -> left button drag (sliders)
 *   single-finger drag    -> pointer motion only (no click)
 * ------------------------------------------------------------------ */
struct TouchFingerState {
    float x = 0.0f;
    float y = 0.0f;
    float start_x = 0.0f;
    float start_y = 0.0f;
    uint64_t down_ms = 0;
    bool moved = false;
};

std::map<SDL_FingerID, TouchFingerState> g_touch_fingers;
bool g_touch_multi = false;        /* two/three-finger gesture active */
bool g_touch_multi_clicked = false;/* right click already emitted */
bool g_touch_scrolled = false;     /* two-finger drag became a wheel */
bool g_touch_dragging = false;     /* long-press held left button */
float g_touch_anchor_y = 0.0f;
float g_touch_scroll_accum = 0.0f;

constexpr float kTouchTapSlop = 12.0f;      /* pixels */
constexpr float kTouchDragStart = 6.0f;     /* pixels */
constexpr float kTouchScrollStep = 40.0f;   /* pixels per wheel notch */
constexpr uint64_t kTouchLongPressMs = 350;

void reset_touch_gesture()
{
    g_touch_multi = false;
    g_touch_multi_clicked = false;
    g_touch_scrolled = false;
    g_touch_dragging = false;
    g_touch_anchor_y = 0.0f;
    g_touch_scroll_accum = 0.0f;
}

} // namespace

void push_event(const InputEvent &event)
{
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_queue.push_back(event);
    }
    g_queue_cv.notify_all();
}

void push_user(int code, int code2, void *data1, void *data2)
{
    InputEvent event;
    event.type = EventType::User;
    event.user.code = code;
    event.user.code2 = code2;
    event.user.data1 = data1;
    event.user.data2 = data2;
    push_event(event);
}

void push_quit()
{
    InputEvent event;
    event.type = EventType::Quit;
    push_event(event);
}

void push_mouse_button(int x, int y, int button, bool down)
{
    InputEvent event;
    event.type = down ? EventType::MouseButtonDown : EventType::MouseButtonUp;
    event.button.x = x;
    event.button.y = y;
    event.button.button = (uint8_t)button;
    event.button.down = down;
    push_event(event);
}

void push_mouse_motion(int x, int y)
{
    InputEvent event;
    event.type = EventType::MouseMotion;
    event.motion.type = EventType::MouseMotion;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.xrel = 0;
    event.motion.yrel = 0;
    event.motion.buttons = 0;
    push_event(event);
}

void push_key(int sym, bool down, uint16_t unicode)
{
    InputEvent event;
    event.type = down ? EventType::KeyDown : EventType::KeyUp;
    event.key.type = event.type;
    event.key.keysym.sym = sym;
    event.key.keysym.scancode = 0;
    event.key.keysym.mod = 0;
    event.key.keysym.unicode = unicode;
    event.key.down = down;
    push_event(event);
}

InputSystem::InputSystem()
    : stash_valid_(false), offscreen_(false), window_w_(0), window_h_(0),
      logical_w_(0), logical_h_(0), initialised_(false)
{
    for (int i = 0; i <= KEY_LAST; ++i)
        keys_[i] = false;
}

InputSystem::~InputSystem()
{
}

bool InputSystem::init()
{
#if defined(__ANDROID__)
    /* We translate touch ourselves: two-finger tap -> right click.  Letting
     * SDL also synthesize mouse events would produce a stray left click. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    initialised_ = true;
    return true;
}

void InputSystem::shutdown()
{
    initialised_ = false;
}

void InputSystem::set_viewport(int window_w, int window_h, int logical_w, int logical_h)
{
    window_w_ = window_w;
    window_h_ = window_h;
    logical_w_ = logical_w;
    logical_h_ = logical_h;
}

void InputSystem::map_mouse(float wx, float wy, int *lx, int *ly) const
{
    if (window_w_ <= 0 || window_h_ <= 0 || logical_w_ <= 0 || logical_h_ <= 0) {
        *lx = (int)wx;
        *ly = (int)wy;
        return;
    }

    float sx = (float)window_w_ / (float)logical_w_;
    float sy = (float)window_h_ / (float)logical_h_;
    float s = sx < sy ? sx : sy;
    if (s <= 0.0f)
        s = 1.0f;

    float dw = (float)logical_w_ * s;
    float dh = (float)logical_h_ * s;
    float dx = ((float)window_w_ - dw) * 0.5f;
    float dy = ((float)window_h_ - dh) * 0.5f;

    int x = (int)((wx - dx) / s + 0.5f);
    int y = (int)((wy - dy) / s + 0.5f);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= logical_w_) x = logical_w_ - 1;
    if (y >= logical_h_) y = logical_h_ - 1;
    *lx = x;
    *ly = y;
}

void InputSystem::handle_touch_event(const void *opaque)
{
    const SDL_Event &e = *static_cast<const SDL_Event *>(opaque);
    const SDL_TouchFingerEvent &tf = e.tfinger;

    auto to_logical = [this](float nx, float ny, int *x, int *y) {
        float w = window_w_ > 0 ? (float)window_w_ : (float)logical_w_;
        float h = window_h_ > 0 ? (float)window_h_ : (float)logical_h_;
        if (w <= 0.0f) w = 1.0f;
        if (h <= 0.0f) h = 1.0f;
        map_mouse(nx * w, ny * h, x, y);
    };

    int x = 0, y = 0;
    to_logical(tf.x, tf.y, &x, &y);

    auto it = g_touch_fingers.find(tf.fingerID);

    if (e.type == SDL_EVENT_FINGER_DOWN) {
        if (g_touch_fingers.empty())
            reset_touch_gesture();

        TouchFingerState f;
        f.x = f.start_x = (float)x;
        f.y = f.start_y = (float)y;
        f.down_ms = SDL_GetTicks();
        g_touch_fingers[tf.fingerID] = f;

        if (g_touch_fingers.size() == 1) {
            /* Hover immediately: Android has no real pointer, and ONScripter
             * menus highlight on motion. */
            push_mouse_motion(x, y);
        } else if (g_touch_fingers.size() == 2) {
            g_touch_multi = true;
            g_touch_multi_clicked = false;
            g_touch_scrolled = false;
            float sx = 0.0f, sy = 0.0f;
            for (const auto &kv : g_touch_fingers) {
                sx += kv.second.x;
                sy += kv.second.y;
            }
            const float n = (float)g_touch_fingers.size();
            g_touch_anchor_y = sy / n;
            g_touch_scroll_accum = 0.0f;
            push_mouse_motion((int)(sx / n), (int)(sy / n));
        } else {
            /* Three fingers: menu (right click). */
            if (!g_touch_multi_clicked) {
                push_mouse_button(x, y, BUTTON_RIGHT, true);
                push_mouse_button(x, y, BUTTON_RIGHT, false);
                g_touch_multi_clicked = true;
            }
        }
        return;
    }

    if (it == g_touch_fingers.end())
        return;

    TouchFingerState &f = it->second;

    if (e.type == SDL_EVENT_FINGER_MOTION) {
        f.x = (float)x;
        f.y = (float)y;
        const float dx = f.x - f.start_x;
        const float dy = f.y - f.start_y;
        if (dx * dx + dy * dy > kTouchTapSlop * kTouchTapSlop)
            f.moved = true;

        if (g_touch_fingers.size() == 1) {
            const uint64_t held = SDL_GetTicks() - f.down_ms;
            if (!g_touch_dragging && held >= kTouchLongPressMs &&
                dx * dx + dy * dy > kTouchDragStart * kTouchDragStart) {
                /* Long press then move: hold the left button so sliders and
                 * scrollbars can be dragged, like krkrsdl3. */
                g_touch_dragging = true;
                push_mouse_button((int)f.start_x, (int)f.start_y, BUTTON_LEFT, true);
            }
            push_mouse_motion(x, y);
        } else {
            float sx = 0.0f, sy = 0.0f;
            for (const auto &kv : g_touch_fingers) {
                sx += kv.second.x;
                sy += kv.second.y;
            }
            const float n = (float)g_touch_fingers.size();
            const float avg_y = sy / n;

            if (g_touch_multi && !g_touch_dragging) {
                g_touch_scroll_accum += avg_y - g_touch_anchor_y;
                g_touch_anchor_y = avg_y;
                while (g_touch_scroll_accum >= kTouchScrollStep) {
                    push_mouse_button(x, y, BUTTON_WHEELDOWN, true);
                    push_mouse_button(x, y, BUTTON_WHEELDOWN, false);
                    g_touch_scroll_accum -= kTouchScrollStep;
                    g_touch_scrolled = true;
                }
                while (g_touch_scroll_accum <= -kTouchScrollStep) {
                    push_mouse_button(x, y, BUTTON_WHEELUP, true);
                    push_mouse_button(x, y, BUTTON_WHEELUP, false);
                    g_touch_scroll_accum += kTouchScrollStep;
                    g_touch_scrolled = true;
                }
            }
            push_mouse_motion((int)(sx / n), (int)(sy / n));
        }
        return;
    }

    if (e.type == SDL_EVENT_FINGER_UP) {
        f.x = (float)x;
        f.y = (float)y;
        const size_t before = g_touch_fingers.size();

        if (g_touch_multi) {
            /* Two-finger tap -> right click.  A scroll or a moved finger is
             * not a tap.  Remaining fingers must not fall through to the
             * single-tap left click. */
            if (before >= 2 && !g_touch_scrolled &&
                !g_touch_multi_clicked && !f.moved) {
                push_mouse_button(x, y, BUTTON_RIGHT, true);
                push_mouse_button(x, y, BUTTON_RIGHT, false);
                g_touch_multi_clicked = true;
            }
        } else if (before == 1) {
            if (g_touch_dragging) {
                push_mouse_button(x, y, BUTTON_LEFT, false);
                g_touch_dragging = false;
            } else if (!f.moved &&
                       SDL_GetTicks() - f.down_ms < 650) {
                push_mouse_motion(x, y);
                push_mouse_button(x, y, BUTTON_LEFT, true);
                push_mouse_button(x, y, BUTTON_LEFT, false);
            } else {
                push_mouse_motion(x, y);
            }
        }

        g_touch_fingers.erase(it);
        if (g_touch_fingers.empty())
            reset_touch_gesture();
        return;
    }

    if (e.type == SDL_EVENT_FINGER_CANCELED) {
        g_touch_fingers.erase(it);
        if (g_touch_fingers.empty())
            reset_touch_gesture();
    }
}

/*
 * Raw event translation.  One key-down is held back so that a following
 * SDL_EVENT_TEXT_INPUT can be merged into keysym.unicode, which is how the
 * engine expects text input to arrive.
 */
bool InputSystem::next_raw(InputEvent &event)
{
    static InputEvent held;
    static bool has_held = false;
    static SDL_Event stash;
    static bool has_stash = false;

    if (stash_valid_) {
        event = stash_;
        stash_valid_ = false;
        return true;
    }

    for (;;) {
        SDL_Event e;
        bool got = false;
        if (has_stash) {
            e = stash;
            has_stash = false;
            got = true;
        } else {
            got = SDL_PollEvent(&e);
        }

        if (!got) {
            if (has_held) {
                event = held;
                has_held = false;
                return true;
            }
            return false;
        }

        InputEvent translated;
        TranslateKind kind = T_NONE;

        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            translated.type = (e.type == SDL_EVENT_KEY_DOWN) ? EventType::KeyDown
                                                            : EventType::KeyUp;
            translated.key.type = translated.type;
            translated.key.keysym.sym = map_keycode(e.key.key);
            translated.key.keysym.scancode = (uint8_t)e.key.scancode;
            translated.key.keysym.mod = (uint16_t)e.key.mod;
            translated.key.keysym.unicode = ascii_unicode(translated.key.keysym.sym,
                                                         (uint16_t)e.key.mod);
            translated.key.down = (e.type == SDL_EVENT_KEY_DOWN);
            kind = (e.type == SDL_EVENT_KEY_DOWN) ? T_KEYDOWN : T_EVENT;

            int sym = translated.key.keysym.sym;
            if (sym >= 0 && sym <= KEY_LAST)
                keys_[sym] = translated.key.down;
            if (e.key.repeat)
                kind = T_EVENT; /* never hold a repeat, the text already matched */
            break;
        }
        case SDL_EVENT_TEXT_INPUT: {
            translated.type = EventType::TextInput;
            translated.key.type = translated.type;
            translated.key.keysym.unicode = first_unicode(e.text.text);
            translated.key.keysym.sym = translated.key.keysym.unicode;
            translated.key.down = true;
            if (has_held) {
                /* Merge into the pending key-down. */
                has_held = false;
                InputEvent key = held;
                key.key.keysym.unicode = translated.key.keysym.unicode;
                if (key.key.keysym.unicode)
                    key.key.keysym.sym = key.key.keysym.sym;
                event = key;
                return true;
            }
            kind = T_TEXT;
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            translated.type = EventType::MouseMotion;
            translated.motion.type = translated.type;
            map_mouse(e.motion.x, e.motion.y, &translated.motion.x, &translated.motion.y);
            translated.motion.xrel = (int)e.motion.xrel;
            translated.motion.yrel = (int)e.motion.yrel;
            translated.motion.buttons = (uint8_t)e.motion.state;
            mouse_x = translated.motion.x;
            mouse_y = translated.motion.y;
            kind = T_EVENT;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            translated.type = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                                  ? EventType::MouseButtonDown
                                  : EventType::MouseButtonUp;
            translated.button.type = translated.type;
            translated.button.button = e.button.button;
            translated.button.down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            map_mouse(e.button.x, e.button.y, &translated.button.x, &translated.button.y);
            mouse_x = translated.button.x;
            mouse_y = translated.button.y;
            kind = T_EVENT;
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            /* The engine expects wheel input as classic button 4/5 presses. */
            int button = (e.wheel.y >= 0) ? BUTTON_WHEELUP : BUTTON_WHEELDOWN;
            push_mouse_button(mouse_x, mouse_y, button, true);
            push_mouse_button(mouse_x, mouse_y, button, false);
            translated.type = EventType::MouseWheel;
            translated.wheel.dx = (int)e.wheel.x;
            translated.wheel.dy = (int)e.wheel.y;
            translated.wheel.x = mouse_x;
            translated.wheel.y = mouse_y;
            kind = T_NONE; /* the queued button events carry the action */
            break;
        }
        case SDL_EVENT_QUIT:
            translated.type = EventType::Quit;
            kind = T_EVENT;
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            translated.type = EventType::Expose;
            kind = T_EVENT;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            translated.type = EventType::FocusGained;
            translated.gain = 1;
            kind = T_EVENT;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            translated.type = EventType::FocusLost;
            translated.gain = 0;
            kind = T_EVENT;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            window_w_ = e.window.data1;
            window_h_ = e.window.data2;
            kind = T_NONE;
            break;
#if defined(__ANDROID__)
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            handle_touch_event(&e);
            kind = T_NONE;
            break;
#endif
        default:
            kind = T_NONE;
            break;
        }

        if (has_held) {
            if (kind == T_TEXT) {
                InputEvent key = held;
                has_held = false;
                key.key.keysym.unicode = translated.key.keysym.unicode;
                event = key;
                return true;
            }
            has_stash = true;
            stash = e;
            event = held;
            has_held = false;
            return true;
        }

        if (kind == T_KEYDOWN) {
            held = translated;
            has_held = true;
            continue;
        }

        if (kind == T_EVENT || kind == T_TEXT) {
            event = translated;
            return true;
        }
    }
}

bool InputSystem::poll(InputEvent &event)
{
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (!g_queue.empty()) {
            event = g_queue.front();
            g_queue.pop_front();
            return true;
        }
    }

    if (!initialised_ || offscreen_)
        return false;

    /* Coalesce consecutive mouse motion, as the original engine did. */
    if (!next_raw(event)) {
        /* A touch gesture may have queued synthesized mouse events while
         * translating SDL touch input. */
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (!g_queue.empty()) {
            event = g_queue.front();
            g_queue.pop_front();
            return true;
        }
        return false;
    }

    if (event.type == EventType::MouseMotion) {
        InputEvent more;
        while (next_raw(more)) {
            if (more.type != EventType::MouseMotion) {
                stash_ = more;
                stash_valid_ = true;
                break;
            }
            event = more;
        }
    }
    return true;
}

bool InputSystem::wait(InputEvent &event)
{
    if (poll(event))
        return true;

    /* Wait for at most one tick interval.  Returning false on timeout is what
     * lets the host run its per-frame work (injected clicks, frame limits,
     * hang watchdog) while the script is blocked in a wait command.  Looping
     * here forever would stall that work as soon as a script waits without
     * any timer running. */
    std::unique_lock<std::mutex> lock(g_queue_mutex);
    if (g_queue.empty())
        g_queue_cv.wait_for(lock, std::chrono::milliseconds(10));
    if (g_queue.empty())
        return false;

    event = g_queue.front();
    g_queue.pop_front();
    return true;
}

bool InputSystem::peek(InputEvent &event)
{
    if (stash_valid_) {
        event = stash_;
        return true;
    }
    if (!initialised_ || offscreen_)
        return false;
    if (!next_raw(event))
        return false;
    stash_ = event;
    stash_valid_ = true;
    return true;
}

bool InputSystem::pop(InputEvent &event)
{
    if (stash_valid_) {
        event = stash_;
        stash_valid_ = false;
        return true;
    }
    return poll(event);
}

void InputSystem::pump()
{
    if (!initialised_ || offscreen_)
        return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        /* keep the window size in sync, ignore everything else */
        if (e.type == SDL_EVENT_WINDOW_RESIZED) {
            window_w_ = e.window.data1;
            window_h_ = e.window.data2;
        }
    }
}

bool InputSystem::key_down(int sym) const
{
    if (sym < 0 || sym > KEY_LAST)
        return false;
    if (!initialised_)
        return false;
    return keys_[sym];
}

bool InputSystem::quit_pending()
{
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    if (!g_queue.empty() && g_queue.back().type == EventType::Quit)
        return true;
    for (size_t i = 0; i < g_queue.size(); ++i) {
        if (g_queue[i].type == EventType::Quit)
            return true;
    }
    return false;
}

void InputSystem::reset_keys()
{
    for (int i = 0; i <= KEY_LAST; ++i)
        keys_[i] = false;
}

} // namespace ons
