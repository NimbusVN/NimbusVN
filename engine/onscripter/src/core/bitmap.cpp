/* -*- C++ -*-
 *
 *  bitmap.cpp - CPU ARGB8888 bitmap implementation
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "bitmap.h"
#include "memdiag.h"

#include <cstdlib>
#include <cstring>

namespace ons {

namespace {

inline int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

Bitmap::Bitmap()
{
    format = &fmt_;
}

Bitmap::Bitmap(const Bitmap &other)
{
    format = &fmt_;
    copy_from(other);
}

Bitmap &Bitmap::operator=(const Bitmap &other)
{
    if (this != &other)
        copy_from(other);
    return *this;
}

Bitmap::~Bitmap()
{
    release();
}

void Bitmap::release()
{
    if (pixels) {
        memdiag::bitmap_free((std::size_t)w * (std::size_t)h * 4u);
        std::free(pixels);
        pixels = nullptr;
    }
    w = h = pitch = 0;
}

bool Bitmap::create(int width, int height)
{
    release();
    if (width <= 0 || height <= 0)
        return false;

    const size_t bytes = (size_t)width * (size_t)height * 4u;
    pixels = (uint32_t *)std::malloc(bytes);
    if (!pixels)
        return false;

    std::memset(pixels, 0, bytes);
    w = width;
    h = height;
    pitch = width * 4;
    memdiag::bitmap_alloc(bytes);
    return true;
}

void Bitmap::free()
{
    release();
}

void Bitmap::copy_from(const Bitmap &other)
{
    release();
    fmt_ = other.fmt_;
    format = &fmt_;

    if (!other.pixels || other.w <= 0 || other.h <= 0)
        return;

    if (!create(other.w, other.h))
        return;

    const size_t bytes = (size_t)w * (size_t)h * 4u;
    std::memcpy(pixels, other.pixels, bytes);
}

Bitmap *Bitmap::from_rgba(const uint8_t *data, int width, int height)
{
    if (!data || width <= 0 || height <= 0)
        return nullptr;

    Bitmap *bmp = new Bitmap();
    if (!bmp->create(width, height)) {
        delete bmp;
        return nullptr;
    }

    for (int y = 0; y < height; ++y) {
        uint32_t *dst = bmp->pixels + (size_t)y * (size_t)width;
        const uint8_t *src = data + (size_t)y * (size_t)width * 4u;
        for (int x = 0; x < width; ++x, src += 4) {
            dst[x] = pack_rgba(src[0], src[1], src[2], src[3]);
        }
    }
    return bmp;
}

Bitmap *Bitmap::from_argb(const uint32_t *data, int width, int height)
{
    if (!data || width <= 0 || height <= 0)
        return nullptr;

    Bitmap *bmp = new Bitmap();
    if (!bmp->create(width, height)) {
        delete bmp;
        return nullptr;
    }
    std::memcpy(bmp->pixels, data, (size_t)width * (size_t)height * 4u);
    return bmp;
}

Bitmap *Bitmap::alloc(int width, int height)
{
    Bitmap *bmp = new Bitmap();
    if (!bmp->create(width, height)) {
        delete bmp;
        return nullptr;
    }
    return bmp;
}

Bitmap *Bitmap::clone() const
{
    Bitmap *bmp = new Bitmap(*this);
    return bmp;
}

Bitmap *Bitmap::convert() const
{
    return clone();
}

void Bitmap::resize(int width, int height)
{
    if (width == w && height == h)
        return;
    if (width <= 0 || height <= 0) {
        release();
        return;
    }

    Bitmap old(*this);
    if (!create(width, height))
        return;

    Rect dst;
    dst.x = 0;
    dst.y = 0;
    dst.w = width;
    dst.h = height;
    blit_scaled(old, nullptr, &dst);
}

void Bitmap::fill(const Color &color)
{
    fill(map_rgba(color));
}

void Bitmap::fill(uint32_t pixel)
{
    if (!pixels)
        return;
    const size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; ++i)
        pixels[i] = pixel;
}

void Bitmap::fill(const Rect &rect, const Color &color)
{
    fill(rect, map_rgba(color));
}

void Bitmap::fill(const Rect &rect, uint32_t pixel)
{
    if (!pixels)
        return;
    int x0 = clamp_int(rect.x, 0, w);
    int y0 = clamp_int(rect.y, 0, h);
    int x1 = clamp_int(rect.x + rect.w, 0, w);
    int y1 = clamp_int(rect.y + rect.h, 0, h);
    for (int y = y0; y < y1; ++y) {
        uint32_t *row = pixels + (size_t)y * (size_t)(pitch / 4);
        for (int x = x0; x < x1; ++x)
            row[x] = pixel;
    }
}

void Bitmap::blit(const Bitmap &src, const Rect *src_rect, const Rect *dst_rect)
{
    if (!pixels || !src.pixels)
        return;

    int dw = w;
    int dh = h;
    int dx0 = 0;
    int dy0 = 0;
    Rect sr;
    sr.x = 0;
    sr.y = 0;
    sr.w = src.w;
    sr.h = src.h;

    if (src_rect)
        sr = *src_rect;
    if (dst_rect) {
        dx0 = dst_rect->x;
        dy0 = dst_rect->y;
        dw = dst_rect->w > 0 ? dst_rect->w : sr.w;
        dh = dst_rect->h > 0 ? dst_rect->h : sr.h;
    } else {
        dw = sr.w;
        dh = sr.h;
    }

    /* Clip against the source extent. */
    if (sr.x < 0) { dx0 -= sr.x; dw += sr.x; sr.w += sr.x; sr.x = 0; }
    if (sr.y < 0) { dy0 -= sr.y; dh += sr.y; sr.h += sr.y; sr.y = 0; }
    if (sr.x + sr.w > src.w) { dw -= sr.x + sr.w - src.w; sr.w = src.w - sr.x; }
    if (sr.y + sr.h > src.h) { dh -= sr.y + sr.h - src.h; sr.h = src.h - sr.y; }

    /* Clip against the destination extent. */
    if (dw > sr.w) dw = sr.w;
    if (dh > sr.h) dh = sr.h;
    if (dx0 < 0) { sr.x -= dx0; dw += dx0; sr.w += dx0; dx0 = 0; }
    if (dy0 < 0) { sr.y -= dy0; dh += dy0; sr.h += dy0; dy0 = 0; }
    if (dx0 + dw > w) { dw = w - dx0; sr.w = dw; }
    if (dy0 + dh > h) { dh = h - dy0; sr.h = dh; }

    if (dw <= 0 || dh <= 0)
        return;

    const bool use_colorkey = src.fmt_.colorkey_flag;
    const uint32_t colorkey = src.fmt_.colorkey;
    const bool use_alpha = src.fmt_.alpha_flag;
    const int surface_alpha = use_alpha ? (int)src.fmt_.alpha : 255;
    const int dst_stride = pitch / 4;
    const int src_stride = src.pitch / 4;

    for (int y = 0; y < dh; ++y) {
        const uint32_t *s = src.pixels + (size_t)(sr.y + y) * (size_t)src_stride + (size_t)sr.x;
        uint32_t *d = pixels + (size_t)(dy0 + y) * (size_t)dst_stride + (size_t)dx0;
        if (!use_alpha) {
            for (int x = 0; x < dw; ++x) {
                uint32_t p = s[x];
                if (use_colorkey && p == colorkey)
                    continue;
                d[x] = p;
            }
        } else {
            for (int x = 0; x < dw; ++x) {
                uint32_t p = s[x];
                if (use_colorkey && p == colorkey)
                    continue;
                int sa = (int)(p >> 24);
                if (surface_alpha != 255)
                    sa = sa * surface_alpha / 255;
                if (sa >= 255) {
                    d[x] = p;
                } else if (sa > 0) {
                    uint32_t dp = d[x];
                    int da = (int)(dp >> 24);
                    int sr8 = (int)((p >> 16) & 0xff);
                    int sg8 = (int)((p >> 8) & 0xff);
                    int sb8 = (int)(p & 0xff);
                    int dr8 = (int)((dp >> 16) & 0xff);
                    int dg8 = (int)((dp >> 8) & 0xff);
                    int db8 = (int)(dp & 0xff);
                    int ia = 255 - sa;
                    int or8 = (sr8 * sa + dr8 * ia) / 255;
                    int og8 = (sg8 * sa + dg8 * ia) / 255;
                    int ob8 = (sb8 * sa + db8 * ia) / 255;
                    (void)da;
                    d[x] = pack_rgba((uint8_t)or8, (uint8_t)og8, (uint8_t)ob8, 255);
                }
            }
        }
    }
}

void Bitmap::blit_scaled(const Bitmap &src, const Rect *src_rect, const Rect *dst_rect)
{
    if (!pixels || !src.pixels)
        return;

    Rect sr;
    sr.x = 0;
    sr.y = 0;
    sr.w = src.w;
    sr.h = src.h;
    if (src_rect)
        sr = *src_rect;

    Rect dr;
    dr.x = 0;
    dr.y = 0;
    dr.w = sr.w;
    dr.h = sr.h;
    if (dst_rect) {
        dr.x = dst_rect->x;
        dr.y = dst_rect->y;
        if (dst_rect->w > 0) dr.w = dst_rect->w;
        if (dst_rect->h > 0) dr.h = dst_rect->h;
    }

    if (sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0)
        return;

    const int dst_stride = pitch / 4;
    const int src_stride = src.pitch / 4;
    const bool use_colorkey = src.fmt_.colorkey_flag;
    const uint32_t colorkey = src.fmt_.colorkey;
    const bool use_alpha = src.fmt_.alpha_flag;

    for (int dy = 0; dy < dr.h; ++dy) {
        int py = dr.y + dy;
        if (py < 0 || py >= h)
            continue;
        int sy = sr.y + (int)((int64_t)dy * sr.h / dr.h);
        if (sy < 0 || sy >= src.h)
            continue;
        const uint32_t *s = src.pixels + (size_t)sy * (size_t)src_stride;
        uint32_t *d = pixels + (size_t)py * (size_t)dst_stride;
        for (int dx = 0; dx < dr.w; ++dx) {
            int px = dr.x + dx;
            if (px < 0 || px >= w)
                continue;
            int sx = sr.x + (int)((int64_t)dx * sr.w / dr.w);
            if (sx < 0 || sx >= src.w)
                continue;
            uint32_t p = s[sx];
            if (use_colorkey && p == colorkey)
                continue;
            if (use_alpha && (p >> 24) < 255) {
                int sa = (int)(p >> 24);
                if (sa == 0)
                    continue;
                uint32_t dp = d[px];
                int ia = 255 - sa;
                int r = ((int)((p >> 16) & 0xff) * sa + (int)((dp >> 16) & 0xff) * ia) / 255;
                int g = ((int)((p >> 8) & 0xff) * sa + (int)((dp >> 8) & 0xff) * ia) / 255;
                int b = ((int)(p & 0xff) * sa + (int)(dp & 0xff) * ia) / 255;
                d[px] = pack_rgba((uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
            } else {
                d[px] = p;
            }
        }
    }
}

void Bitmap::set_alpha(uint8_t alpha, bool enabled)
{
    fmt_.alpha = alpha;
    fmt_.alpha_flag = enabled;
}

void Bitmap::set_colorkey(uint32_t key, bool enabled)
{
    fmt_.colorkey = key;
    fmt_.colorkey_flag = enabled;
}

uint32_t Bitmap::map_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) const
{
    return pack_rgba(r, g, b, a);
}

uint32_t Bitmap::map_rgba(const Color &color) const
{
    return pack_rgba(color.r, color.g, color.b, color.a);
}

uint32_t Bitmap::map_rgb(uint8_t r, uint8_t g, uint8_t b) const
{
    return pack_rgba(r, g, b, 255);
}

uint32_t Bitmap::get_pixel(int x, int y) const
{
    if (!pixels || x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    return pixels[(size_t)y * (size_t)(pitch / 4) + (size_t)x];
}

void Bitmap::set_pixel(int x, int y, uint32_t value)
{
    if (!pixels || x < 0 || y < 0 || x >= w || y >= h)
        return;
    pixels[(size_t)y * (size_t)(pitch / 4) + (size_t)x] = value;
}

} // namespace ons
