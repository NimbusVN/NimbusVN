/* -*- C++ -*-
 *
 *  input.h - keyboard/mouse event queue for the NimbusVN ONScripter engine
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

#ifndef ONS_CORE_INPUT_H
#define ONS_CORE_INPUT_H

#include "types.h"

/*
 * Key codes use the classic ONScripter numbering: printable ASCII keeps its
 * ASCII value, the function keys start at 273.  These constants are defined by
 * this engine, they are not provided by any SDL header.
 */
enum {
    KEY_UNKNOWN = 0,
    KEY_BACKSPACE = 8,
    KEY_TAB = 9,
    KEY_CLEAR = 12,
    KEY_RETURN = 13,
    KEY_PAUSE = 19,
    KEY_ESCAPE = 27,
    KEY_SPACE = 32,
    KEY_DELETE = 127,
    KEY_0 = '0', KEY_1 = '1', KEY_2 = '2', KEY_3 = '3', KEY_4 = '4',
    KEY_5 = '5', KEY_6 = '6', KEY_7 = '7', KEY_8 = '8', KEY_9 = '9',
    KEY_a = 'a', KEY_b = 'b', KEY_c = 'c', KEY_d = 'd', KEY_e = 'e',
    KEY_f = 'f', KEY_g = 'g', KEY_h = 'h', KEY_i = 'i', KEY_j = 'j',
    KEY_k = 'k', KEY_l = 'l', KEY_m = 'm', KEY_n = 'n', KEY_o = 'o',
    KEY_p = 'p', KEY_q = 'q', KEY_r = 'r', KEY_s = 's', KEY_t = 't',
    KEY_u = 'u', KEY_v = 'v', KEY_w = 'w', KEY_x = 'x', KEY_y = 'y',
    KEY_z = 'z',
    KEY_COMMA = ',',
    KEY_PERIOD = '.',
    KEY_SLASH = '/',
    KEY_MINUS = '-',
    KEY_EQUALS = '=',
    KEY_KP0 = 256, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4,
    KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9,
    KEY_KP_PERIOD, KEY_KP_DIVIDE, KEY_KP_MULTIPLY, KEY_KP_MINUS,
    KEY_KP_PLUS, KEY_KP_ENTER, KEY_KP_EQUALS,
    KEY_UP = 273, KEY_DOWN, KEY_RIGHT, KEY_LEFT,
    KEY_INSERT, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_F1 = 282, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7,
    KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_F13, KEY_F14, KEY_F15,
    KEY_NUMLOCK = 300, KEY_CAPSLOCK, KEY_SCROLLOCK,
    KEY_RSHIFT, KEY_LSHIFT, KEY_RCTRL, KEY_LCTRL,
    KEY_RALT, KEY_LALT, KEY_RMETA, KEY_LMETA,
    KEY_LSUPER, KEY_RSUPER,
    KEY_MODE, KEY_COMPOSE, KEY_HELP, KEY_PRINT, KEY_SYSREQ, KEY_BREAK,
    KEY_MENU, KEY_POWER, KEY_EURO, KEY_UNDO,
    KEY_LAST
};

/* Mouse button numbering follows the classic 1=left 2=middle 3=right. */
enum {
    BUTTON_LEFT = 1,
    BUTTON_MIDDLE = 2,
    BUTTON_RIGHT = 3,
    BUTTON_WHEELUP = 4,
    BUTTON_WHEELDOWN = 5
};

namespace ons {

/* Symbolic key value (classic ONScripter numbering, see the KEY_* list). */
typedef int32_t KeyCode;

enum class EventType {
    KeyDown,
    KeyUp,
    TextInput,
    MouseMotion,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    Quit,
    Expose,
    FocusGained,
    FocusLost,
    User
};

struct Keysym {
    int32_t sym = 0;
    uint16_t unicode = 0;
    uint8_t scancode = 0;
    uint16_t mod = 0;
};

struct KeyEvent {
    /* Mirrors InputEvent::type so handlers can look at a sub-event alone. */
    EventType type = EventType::KeyDown;
    Keysym keysym;
    bool down = false;
};

struct MouseMotionEvent {
    EventType type = EventType::MouseMotion;
    int x = 0;
    int y = 0;
    int xrel = 0;
    int yrel = 0;
    uint8_t buttons = 0;
};

struct MouseButtonEvent {
    EventType type = EventType::MouseButtonUp;
    int x = 0;
    int y = 0;
    uint8_t button = 0;
    bool down = false;
};

struct MouseWheelEvent {
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;
};

struct UserEvent {
    int code = 0;
    int code2 = 0;
    void *data1 = nullptr;
    void *data2 = nullptr;
};

struct InputEvent {
    EventType type = EventType::User;
    KeyEvent key;
    MouseMotionEvent motion;
    MouseButtonEvent button;
    MouseWheelEvent wheel;
    UserEvent user;
    uint8_t gain = 0;
};

/*
 * The engine keeps a single process wide queue so that timer callbacks can
 * inject events without holding a pointer to the InputSystem instance.
 */
void push_event(const InputEvent &event);
void push_user(int code, int code2 = 0, void *data1 = nullptr, void *data2 = nullptr);
void push_quit();
void push_mouse_button(int x, int y, int button, bool down);
void push_mouse_motion(int x, int y);
/* Injects a key stroke (test host only); sym uses the KEY_* numbering. */
void push_key(int sym, bool down, uint16_t unicode = 0);

class InputSystem {
public:
    InputSystem();
    ~InputSystem();

    bool init();
    void shutdown();

    /* Non blocking: returns false when no event is pending. */
    bool poll(InputEvent &event);

    /* Blocking: waits until an event is available. */
    bool wait(InputEvent &event);

    /* Lookahead used by the engine to coalesce mouse motion events. */
    bool peek(InputEvent &event);
    bool pop(InputEvent &event);

    void set_viewport(int window_w, int window_h, int logical_w, int logical_h);
    void set_offscreen(bool offscreen) { offscreen_ = offscreen; }

    int mouse_x = 0;
    int mouse_y = 0;

    bool key_down(int sym) const;
    void reset_keys();

    /* Queues a user event (used by the engine's timer callbacks). */
    void push_user(int code, int code2 = 0) { ons::push_user(code, code2); }

    /* True when a quit request is already queued (safety check). */
    bool quit_pending();

    /* Drains pending window events without dispatching them to the engine. */
    void pump();

private:
    bool next_raw(InputEvent &event);
    void map_mouse(float wx, float wy, int *lx, int *ly) const;
    /* Android touch gesture translation (single tap / two-finger right
     * click / two-finger scroll / three-finger menu).  The argument is an
     * SDL_Event*, kept opaque here so core headers stay SDL-free. */
    void handle_touch_event(const void *event);

    InputEvent stash_;
    bool stash_valid_;
    bool keys_[KEY_LAST + 1];
    bool offscreen_;
    int window_w_;
    int window_h_;
    int logical_w_;
    int logical_h_;
    bool initialised_;
};

} // namespace ons


using ons::EventType;
using ons::InputEvent;
using ons::InputSystem;
using ons::KeyCode;
using ons::KeyEvent;
using ons::Keysym;
using ons::MouseButtonEvent;
using ons::MouseMotionEvent;
using ons::MouseWheelEvent;
using ons::UserEvent;

#endif /* ONS_CORE_INPUT_H */
