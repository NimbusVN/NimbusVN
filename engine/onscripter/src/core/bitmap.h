/* -*- C++ -*-
 *
 *  bitmap.h - CPU ARGB8888 bitmap used by every part of the engine
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

#ifndef ONS_CORE_BITMAP_H
#define ONS_CORE_BITMAP_H

#include "types.h"

namespace ons {

/*
 * A CPU side RGBA8 image.  w/h/pitch/pixels/format are public because the
 * original ONScripter pixel routines address them directly.
 *
 * Memory layout is fixed to ARGB8888: 32bpp, bytes B,G,R,A per pixel.
 */
class Bitmap {
public:
    int w = 0;
    int h = 0;
    int pitch = 0;
    uint32_t *pixels = nullptr;
    PixelFormat *format = nullptr;

    Bitmap();
    Bitmap(const Bitmap &other);
    Bitmap &operator=(const Bitmap &other);
    ~Bitmap();

    /* (Re)allocates the pixel buffer; contents become undefined. */
    bool create(int width, int height);

    /* Frees the pixel buffer and resets the geometry. */
    void free();

    /* Builds a bitmap from tightly packed RGBA8 bytes (R,G,B,A order). */
    static Bitmap *from_rgba(const uint8_t *data, int width, int height);

    /* Builds a bitmap from tightly packed ARGB words. */
    static Bitmap *from_argb(const uint32_t *data, int width, int height);

    /* Convenience allocator used by AnimationInfo::allocSurface(). */
    static Bitmap *alloc(int width, int height);

    Bitmap *clone() const;

    /* All bitmaps already use the engine format, so this is a deep copy. */
    Bitmap *convert() const;

    /* Nearest neighbour rescale in place. */
    void resize(int width, int height);

    void fill(const Color &color);
    void fill(const Rect &rect, const Color &color);
    void fill(uint32_t pixel);
    void fill(const Rect &rect, uint32_t pixel);

    /*
     * Copies src into this bitmap.  src_rect/dst_rect may be NULL (whole
     * source / destination origin).  Only the position of dst_rect is used,
     * the area is clipped against the destination bounds, exactly like the
     * classic blit semantics the engine was written against.
     */
    void blit(const Bitmap &src, const Rect *src_rect = nullptr,
              const Rect *dst_rect = nullptr);

    /* Nearest neighbour scaled blit (dst_rect gives the target extent). */
    void blit_scaled(const Bitmap &src, const Rect *src_rect = nullptr,
                     const Rect *dst_rect = nullptr);

    void set_alpha(uint8_t alpha, bool enabled);
    void set_colorkey(uint32_t key, bool enabled);

    uint32_t map_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) const;
    uint32_t map_rgba(const Color &color) const;
    uint32_t map_rgb(uint8_t r, uint8_t g, uint8_t b) const;

    uint32_t get_pixel(int x, int y) const;
    void set_pixel(int x, int y, uint32_t value);

    /* CPU surfaces never need locking; kept so call sites stay readable. */
    void lock() {}
    void unlock() {}

    bool valid() const { return pixels != nullptr; }

private:
    void copy_from(const Bitmap &other);
    void release();

    PixelFormat fmt_;
};

} // namespace ons


/* The engine sources use Bitmap unqualified. */
using ons::Bitmap;

#endif /* ONS_CORE_BITMAP_H */
