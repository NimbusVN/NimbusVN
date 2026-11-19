/* -*- C++ -*-
 *
 *  types.h - basic value types shared by the NimbusVN ONScripter engine
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

#ifndef ONS_CORE_TYPES_H
#define ONS_CORE_TYPES_H

#include <cstddef>
#include <cstdint>

/*
 * Engine-wide integer aliases.  ONScripter's pixel code uses these names
 * everywhere, so they are kept verbatim; they are plain fixed width types,
 * *not* a platform abstraction layer.
 */
typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int8_t   Sint8;
typedef int16_t  Sint16;
typedef int32_t  Sint32;

/* The engine assumes a little-endian host (Windows / Android / wasm). */
#define ONS_LITTLE_ENDIAN 1

namespace ons {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

/*
 * Fixed ARGB8888 layout: in memory the bytes are B,G,R,A and the 32-bit
 * word is (A<<24)|(R<<16)|(G<<8)|B.  This matches the R/G/B shift macros in
 * ons/graphics_common.h (RSHIFT 16, GSHIFT 8, BSHIFT 0, ASHIFT 24), which the
 * original ONScripter pixel routines rely on.
 */
struct PixelFormat {
    int BitsPerPixel = 32;
    int BytesPerPixel = 4;
    uint32_t Rmask = 0x00ff0000;
    uint32_t Gmask = 0x0000ff00;
    uint32_t Bmask = 0x000000ff;
    uint32_t Amask = 0xff000000;
    uint8_t Rshift = 16;
    uint8_t Gshift = 8;
    uint8_t Bshift = 0;
    uint8_t Ashift = 24;
    /* Precision loss of each channel; always 0 for the fixed 32bpp format,
     * kept because the ported effect code reads them. */
    uint8_t Rloss = 0;
    uint8_t Gloss = 0;
    uint8_t Bloss = 0;
    uint8_t Aloss = 0;
    uint32_t colorkey = 0;
    uint8_t alpha = 255;
    bool colorkey_flag = false;
    bool alpha_flag = false;
};

inline uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

inline Rect make_rect(int x, int y, int w, int h)
{
    Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

} // namespace ons

/* The engine sources use these names unqualified. */
using ons::Color;
using ons::PixelFormat;
using ons::Rect;

#endif /* ONS_CORE_TYPES_H */
