/* -*- C++ -*-
 *
 *  memdiag.h - object accounting used by the --debug long-run diagnostics
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef ONS_CORE_MEMDIAG_H
#define ONS_CORE_MEMDIAG_H

#include <cstddef>

namespace ons {
namespace memdiag {

/* Enabled by ONScripterLabel::setDebugLevel(); counters are free when off. */
void set_enabled(bool on);
bool enabled();

void bitmap_alloc(std::size_t bytes);
void bitmap_free(std::size_t bytes);
void anim_created();
void anim_destroyed();
void anim_surface();
void button_created();
void button_destroyed();
void txtbtn_created();
void txtbtn_destroyed();

/* Writes one line with the current counters. */
void dump(const char *tag);

} // namespace memdiag
} // namespace ons

#endif /* ONS_CORE_MEMDIAG_H */
