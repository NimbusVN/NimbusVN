/* -*- C++ -*-
 *
 *  clock.h - monotonic clock, sleep and engine timers (no SDL dependency)
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef ONS_CORE_CLOCK_H
#define ONS_CORE_CLOCK_H

#include <cstdint>

namespace ons {

/* Milliseconds since the process started (monotonic). */
uint32_t ticks_ms();

/* Sleep for the given number of milliseconds. */
void sleep_ms(uint32_t ms);

/*
 * Simple repeating timer facility, replacing SDL_AddTimer.  The callback is
 * invoked on a dedicated timer thread; returning 0 cancels the timer, any
 * other value is used as the next interval in milliseconds.
 */
typedef uint32_t (*TimerCallback)(uint32_t interval, void *param);

/* Returns 0 on failure (a valid id is always > 0). */
uint32_t add_timer(uint32_t interval, TimerCallback callback, void *param);
void remove_timer(uint32_t id);

/* Stops the timer thread; called at shutdown. */
void shutdown_timers();

} // namespace ons

#endif /* ONS_CORE_CLOCK_H */
