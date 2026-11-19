/* -*- C++ -*-
 *
 *  memdiag.cpp - object accounting used by the --debug long-run diagnostics
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "memdiag.h"

#include "font.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace ons {
namespace memdiag {
namespace {

std::atomic<long> g_bitmap_live(0);
std::atomic<long long> g_bitmap_bytes(0);
std::atomic<long> g_anim_live(0);
std::atomic<long> g_anim_surfaces(0);
std::atomic<long> g_button_live(0);
std::atomic<long> g_txtbtn_live(0);

int g_enabled = 0;

} // namespace

void set_enabled(bool on)
{
    g_enabled = on ? 1 : 0;
}

bool enabled()
{
    return g_enabled != 0;
}

void bitmap_alloc(std::size_t bytes)
{
    if (!enabled())
        return;
    ++g_bitmap_live;
    g_bitmap_bytes += (long long)bytes;
}

void bitmap_free(std::size_t bytes)
{
    if (!enabled())
        return;
    --g_bitmap_live;
    g_bitmap_bytes -= (long long)bytes;
}

void anim_created()
{
    if (enabled())
        ++g_anim_live;
}

void anim_destroyed()
{
    if (enabled())
        --g_anim_live;
}

void anim_surface()
{
    if (enabled())
        ++g_anim_surfaces;
}

void button_created()
{
    if (enabled())
        ++g_button_live;
}

void button_destroyed()
{
    if (enabled())
        --g_button_live;
}

void txtbtn_created()
{
    if (enabled())
        ++g_txtbtn_live;
}

void txtbtn_destroyed()
{
    if (enabled())
        --g_txtbtn_live;
}

void dump(const char *tag)
{
    if (!enabled())
        return;
    long font_count = 0, glyph_count = 0, retired_count = 0;
    ons_font_stats(&font_count, &glyph_count, &retired_count);
    fprintf(stderr,
            "memdiag %s bitmap=%ld bitmap_bytes=%.2fMiB anim=%ld "
            "allocSurface=%ld button=%ld txtbtn=%ld fonts=%ld glyphs=%ld "
            "retired=%ld\n",
            tag, g_bitmap_live.load(),
            (double)g_bitmap_bytes.load() / (1024.0 * 1024.0),
            g_anim_live.load(), g_anim_surfaces.load(), g_button_live.load(),
            g_txtbtn_live.load(), font_count, glyph_count, retired_count);
}

} // namespace memdiag
} // namespace ons
