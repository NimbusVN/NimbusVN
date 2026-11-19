/* -*- C++ -*-
 *
 *  image.cpp - PNG / JPEG / BMP decoder for the NimbusVN ONScripter engine
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

/*
 *  Everything here produces an engine Bitmap (ARGB8888, bytes B,G,R,A).
 *
 *  SDL's original image surfaces carried the "does this surface have an
 *  alpha channel?" answer in SDL_PixelFormat::Amask.  ONScripter tests that
 *  field (ONScripterLabel::loadImage / Layer.cpp) to decide whether the file
 *  supplied its own transparency or whether the ":a" mask / colorkey paths
 *  have to build one.  We reproduce the semantics exactly:
 *
 *    - PNG without alpha and without tRNS  -> Amask = 0
 *    - PNG with alpha or tRNS              -> Amask = 0xff000000
 *    - JPEG                                -> Amask = 0
 *    - BMP 24/8bpp                         -> Amask = 0
 *    - BMP 32bpp with a real alpha channel -> Amask = 0xff000000
 *    - BMP 32bpp with an all-zero alpha    -> treated as opaque, Amask = 0
 *
 *  libpng and libjpeg keep all of their state in the per-call structures, so
 *  decoding is already re-entrant.  The mutex below additionally serialises
 *  decodes; animation and preload threads may call this concurrently and the
 *  cost of a serialised decode is negligible next to I/O.
 */

#include "image.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <jpeglib.h>
#include <png.h>

namespace ons {

namespace {

std::mutex g_decode_mutex;

/* ------------------------------------------------------------------ */
/* diagnostics                                                        */
/* ------------------------------------------------------------------ */

bool image_debug_enabled()
{
    /* Function-local statics are initialised exactly once (C++11). */
    static const bool enabled = []() {
        const char *env = std::getenv("ONS_IMAGE_DEBUG");
        return env && *env;
    }();
    return enabled;
}

void print_magic(const char *tag, const uint8_t *data, size_t len)
{
    char hex[32];
    size_t n = len < 8 ? len : 8;
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 4 < sizeof(hex); ++i)
        pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, i ? " %02x" : "%02x",
                                (unsigned)data[i]);
    hex[pos] = '\0';
    fprintf(stderr, "ons: image decode failed (%s): magic=%s\n", tag, hex);
}

void decode_fail(const char *tag, const char *reason, const uint8_t *data, size_t len)
{
    fprintf(stderr, "ons: image decode failed (%s): %s\n", tag, reason);
    if (data && len > 0)
        print_magic(tag, data, len);
}

/* Sample pixel logging, enabled with ONS_IMAGE_DEBUG=1. */
void debug_summary(const char *tag, const Bitmap *bitmap, bool has_alpha,
                   long opaque_count)
{
    if (!image_debug_enabled() || !bitmap || !bitmap->pixels)
        return;
    const uint32_t first = bitmap->pixels[0];
    fprintf(stderr,
            "ons: image %s %dx%d alpha=%d amask=0x%08x first=0x%08x "
            "(b=%u g=%u r=%u a=%u) opaque=%ld/%d\n",
            tag, bitmap->w, bitmap->h, has_alpha ? 1 : 0,
            (unsigned)bitmap->format->Amask, (unsigned)first,
            (unsigned)(first & 0xff), (unsigned)((first >> 8) & 0xff),
            (unsigned)((first >> 16) & 0xff), (unsigned)((first >> 24) & 0xff),
            opaque_count, bitmap->w * bitmap->h);
}

/* Counts non-zero / partially transparent alpha bytes for ONS_IMAGE_DEBUG. */
void count_opaque(const Bitmap *bitmap, long *opaque, long *semi)
{
    long o = 0, s = 0;
    if (!bitmap || !bitmap->pixels)
        return;
    const size_t count = (size_t)bitmap->w * (size_t)bitmap->h;
    for (size_t i = 0; i < count; ++i) {
        const unsigned a = (unsigned)(bitmap->pixels[i] >> 24);
        if (a != 0) ++o;
        if (a != 0 && a != 255) ++s;
    }
    if (opaque) *opaque = o;
    if (semi) *semi = s;
}

/* ------------------------------------------------------------------ */
/* PNG                                                                */
/* ------------------------------------------------------------------ */

struct PngSource {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

struct PngContext {
    PngSource src;
    jmp_buf jump;
    char message[256];
};

void png_read_callback(png_structp png, png_bytep out, png_size_t count)
{
    PngContext *ctx = (PngContext *)png_get_io_ptr(png);
    if (!ctx || ctx->src.pos >= ctx->src.len) {
        png_error(png, "unexpected end of PNG data");
        return;
    }
    size_t avail = ctx->src.len - ctx->src.pos;
    size_t n = count < avail ? count : avail;
    memcpy(out, ctx->src.data + ctx->src.pos, n);
    if (n < count)
        memset(out + n, 0, count - n);
    ctx->src.pos += n;
    if (n < count)
        png_error(png, "truncated PNG scanline data");
}

void png_error_callback(png_structp png, png_const_charp message)
{
    PngContext *ctx = (PngContext *)png_get_error_ptr(png);
    if (ctx) {
        snprintf(ctx->message, sizeof(ctx->message), "%s", message ? message : "unknown");
        longjmp(ctx->jump, 1);
    }
    /* No context: still abort, libpng requires the function not to return. */
    longjmp(png_jmpbuf(png), 1);
}

void png_warning_callback(png_structp png, png_const_charp message)
{
    (void)png;
    if (image_debug_enabled())
        fprintf(stderr, "ons: libpng warning: %s\n", message ? message : "");
}

Bitmap *decode_png(const uint8_t *data, size_t len)
{
    PngContext ctx;
    ctx.src.data = data;
    ctx.src.len = len;
    ctx.src.pos = 0;
    ctx.message[0] = '\0';

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &ctx,
                                             png_error_callback,
                                             png_warning_callback);
    if (!png) {
        decode_fail("png", "png_create_read_struct failed", data, len);
        return nullptr;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        decode_fail("png", "png_create_info_struct failed", data, len);
        return nullptr;
    }

    Bitmap *bitmap = nullptr;
    png_bytep *rows = nullptr;

    if (setjmp(ctx.jump)) {
        if (rows) free(rows);
        delete bitmap;
        png_destroy_read_struct(&png, &info, nullptr);
        decode_fail("png", ctx.message[0] ? ctx.message : "libpng error", data, len);
        return nullptr;
    }

    png_set_read_fn(png, &ctx, png_read_callback);
    png_read_info(png, info);

    const png_uint_32 width = png_get_image_width(png, info);
    const png_uint_32 height = png_get_image_height(png, info);
    const png_byte color_type = png_get_color_type(png, info);
    const png_byte bit_depth = png_get_bit_depth(png, info);
    const bool has_trns = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
    const bool has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 || has_trns;

    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        png_destroy_read_struct(&png, &info, nullptr);
        decode_fail("png", "invalid image dimensions", data, len);
        return nullptr;
    }

    /* Normalise every supported combination to 8-bit BGR(A). */
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
#if PNG_LIBPNG_VER >= 10600
        png_set_expand_gray_1_2_4_to_8(png);
#else
        png_set_gray_1_2_4_to_8(png);
#endif
    }
    if (has_trns)
        png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (bit_depth == 16) {
#if PNG_LIBPNG_VER >= 10600
        png_set_scale_16(png);
#else
        png_set_strip_16(png);
#endif
    }
    /* Three channel images get an opaque alpha so the writer loop is uniform. */
    if (!has_alpha)
        png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    /* Engine memory order is B,G,R,A. */
    png_set_bgr(png);
    png_set_interlace_handling(png);

    png_read_update_info(png, info);

    if (png_get_channels(png, info) != 4 || png_get_bit_depth(png, info) != 8) {
        png_destroy_read_struct(&png, &info, nullptr);
        decode_fail("png", "unsupported channel/bit-depth combination", data, len);
        return nullptr;
    }

    bitmap = new Bitmap();
    if (!bitmap->create((int)width, (int)height)) {
        delete bitmap;
        png_destroy_read_struct(&png, &info, nullptr);
        decode_fail("png", "out of memory for bitmap", data, len);
        return nullptr;
    }

    rows = (png_bytep *)malloc(sizeof(png_bytep) * (size_t)height);
    if (!rows) {
        delete bitmap;
        png_destroy_read_struct(&png, &info, nullptr);
        decode_fail("png", "out of memory for row pointers", data, len);
        return nullptr;
    }
    for (png_uint_32 y = 0; y < height; ++y)
        rows[y] = (png_bytep)((uint8_t *)bitmap->pixels + (size_t)y * (size_t)bitmap->pitch);

    png_read_image(png, rows);
    png_read_end(png, nullptr);
    free(rows);
    rows = nullptr;
    png_destroy_read_struct(&png, &info, nullptr);

    bitmap->format->Amask = has_alpha ? 0xff000000u : 0u;

    long opaque = 0, semi = 0;
    if (image_debug_enabled())
        count_opaque(bitmap, &opaque, &semi);
    debug_summary("png", bitmap, has_alpha, opaque);
    if (image_debug_enabled() && semi > 0)
        fprintf(stderr, "ons: image png semi-transparent pixels=%ld\n", semi);

    return bitmap;
}

/* ------------------------------------------------------------------ */
/* JPEG                                                               */
/* ------------------------------------------------------------------ */

struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void jpeg_error_exit(j_common_ptr cinfo)
{
    JpegError *err = (JpegError *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump, 1);
}

void jpeg_output_message(j_common_ptr cinfo)
{
    if (!image_debug_enabled())
        return;
    char buffer[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buffer);
    fprintf(stderr, "ons: libjpeg: %s\n", buffer);
}

Bitmap *decode_jpeg(const uint8_t *data, size_t len)
{
    jpeg_decompress_struct cinfo;
    JpegError jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    jerr.pub.output_message = jpeg_output_message;
    jerr.message[0] = '\0';

    Bitmap *bitmap = nullptr;
    JSAMPARRAY buffer = nullptr;

    if (setjmp(jerr.jump)) {
        delete bitmap;
        jpeg_destroy_decompress(&cinfo);
        decode_fail("jpeg", jerr.message[0] ? jerr.message : "libjpeg error", data, len);
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)data, (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.out_color_space = JCS_RGB;
    cinfo.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&cinfo);

    const int width = (int)cinfo.output_width;
    const int height = (int)cinfo.output_height;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        jpeg_destroy_decompress(&cinfo);
        decode_fail("jpeg", "invalid image dimensions", data, len);
        return nullptr;
    }

    bitmap = new Bitmap();
    if (!bitmap->create(width, height)) {
        delete bitmap;
        jpeg_destroy_decompress(&cinfo);
        decode_fail("jpeg", "out of memory for bitmap", data, len);
        return nullptr;
    }

    const int row_stride = width * 3;
    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                        (JDIMENSION)row_stride, 1);
    if (!buffer || !buffer[0]) {
        delete bitmap;
        jpeg_destroy_decompress(&cinfo);
        decode_fail("jpeg", "out of memory for scanline buffer", data, len);
        return nullptr;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW dest = buffer[0];
        if (jpeg_read_scanlines(&cinfo, &dest, 1) != 1)
            break;
        const int y = (int)cinfo.output_scanline - 1;
        uint32_t *dst = bitmap->pixels + (size_t)y * (size_t)(bitmap->pitch / 4);
        const uint8_t *src = (const uint8_t *)buffer[0];
        for (int x = 0; x < width; ++x, src += 3)
            dst[x] = pack_rgba(src[0], src[1], src[2], 255);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    bitmap->format->Amask = 0;
    debug_summary("jpeg", bitmap, false, (long)width * height);
    return bitmap;
}

/* ------------------------------------------------------------------ */
/* BMP                                                                */
/* ------------------------------------------------------------------ */

uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

Bitmap *decode_bmp(const uint8_t *data, size_t len)
{
    if (len < 26) {
        decode_fail("bmp", "file shorter than BITMAPFILEHEADER", data, len);
        return nullptr;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        decode_fail("bmp", "missing BM signature", data, len);
        return nullptr;
    }

    const uint32_t data_offset = read_le32(data + 10);
    const uint32_t header_size = read_le32(data + 14);
    if (header_size < 40 || (size_t)header_size > len - 14u) {
        decode_fail("bmp", "unsupported header size", data, len);
        return nullptr;
    }

    const int32_t raw_width = (int32_t)read_le32(data + 18);
    const int32_t raw_height = (int32_t)read_le32(data + 22);
    const uint16_t bpp = read_le16(data + 28);
    const uint32_t compression = read_le32(data + 30);
    const uint32_t colors_used = read_le32(data + 46);

    if (compression != 0) {
        decode_fail("bmp", "compressed BMP is not supported", data, len);
        return nullptr;
    }
    if (raw_width <= 0 || raw_height == 0 || raw_width > 16384 ||
        raw_height == INT32_MIN || (raw_height < 0 ? -raw_height : raw_height) > 16384) {
        decode_fail("bmp", "invalid image dimensions", data, len);
        return nullptr;
    }
    if (bpp != 8 && bpp != 24 && bpp != 32) {
        decode_fail("bmp", "unsupported bits per pixel", data, len);
        return nullptr;
    }

    const bool top_down = raw_height < 0;
    const int width = (int)raw_width;
    const int height = top_down ? (int)(-raw_height) : (int)raw_height;
    const int bytes_per_pixel = bpp / 8;
    const size_t row_stride = (((size_t)width * (size_t)bpp + 31u) / 32u) * 4u;

    if ((size_t)data_offset > len || row_stride > len) {
        decode_fail("bmp", "pixel data offset outside file", data, len);
        return nullptr;
    }
    if (data_offset + row_stride * (size_t)height > len) {
        /* Some writers leave off trailing padding of the last row; only
         * reject when the start of the last row itself is missing. */
        if (data_offset + row_stride * (size_t)(height - 1) + (size_t)width * bytes_per_pixel > len) {
            decode_fail("bmp", "truncated pixel data", data, len);
            return nullptr;
        }
    }

    /* Palette for 8bpp images. */
    const uint8_t *palette = nullptr;
    uint32_t palette_entries = 0;
    if (bpp == 8) {
        palette = data + 14 + header_size;
        palette_entries = colors_used ? colors_used : 256;
        if (palette_entries > 256)
            palette_entries = 256;
        if ((size_t)(palette - data) + (size_t)palette_entries * 4u > len) {
            decode_fail("bmp", "truncated palette", data, len);
            return nullptr;
        }
    }

    Bitmap *bitmap = new Bitmap();
    if (!bitmap->create(width, height)) {
        delete bitmap;
        decode_fail("bmp", "out of memory for bitmap", data, len);
        return nullptr;
    }

    bool has_alpha = false;
    if (bpp == 32) {
        for (int y = 0; y < height && !has_alpha; ++y) {
            const uint8_t *src = data + data_offset + row_stride * (size_t)y;
            for (int x = 0; x < width; ++x) {
                if (src[(size_t)x * 4u + 3u] != 0) {
                    has_alpha = true;
                    break;
                }
            }
        }
    }

    for (int y = 0; y < height; ++y) {
        const int sy = top_down ? y : (height - 1 - y);
        const uint8_t *src = data + data_offset + row_stride * (size_t)sy;
        uint32_t *dst = bitmap->pixels + (size_t)y * (size_t)(bitmap->pitch / 4);
        if (bpp == 8) {
            for (int x = 0; x < width; ++x) {
                const uint32_t index = src[x];
                if (index >= palette_entries) {
                    dst[x] = 0xff000000u; /* black, opaque */
                    continue;
                }
                const uint8_t *entry = palette + (size_t)index * 4u;
                dst[x] = pack_rgba(entry[2], entry[1], entry[0], 255);
            }
        } else if (bpp == 24) {
            for (int x = 0; x < width; ++x) {
                const uint8_t *p = src + (size_t)x * 3u;
                dst[x] = pack_rgba(p[2], p[1], p[0], 255);
            }
        } else {
            if (has_alpha) {
                /* B,G,R,A already matches the engine byte order. */
                memcpy(dst, src, (size_t)width * 4u);
            } else {
                for (int x = 0; x < width; ++x) {
                    const uint8_t *p = src + (size_t)x * 4u;
                    dst[x] = pack_rgba(p[2], p[1], p[0], 255);
                }
            }
        }
    }

    bitmap->format->Amask = has_alpha ? 0xff000000u : 0u;

    long opaque = (long)width * height;
    if (image_debug_enabled())
        count_opaque(bitmap, &opaque, nullptr);
    debug_summary("bmp", bitmap, has_alpha, opaque);
    return bitmap;
}

/* ------------------------------------------------------------------ */
/* size probing                                                       */
/* ------------------------------------------------------------------ */

bool probe_png(const uint8_t *data, size_t len, int *w, int *h)
{
    if (len < 24 || png_sig_cmp((png_const_bytep)data, 0, 8) != 0)
        return false;
    if (memcmp(data + 12, "IHDR", 4) != 0)
        return false;
    const uint32_t width = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                           ((uint32_t)data[18] << 8) | (uint32_t)data[19];
    const uint32_t height = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                            ((uint32_t)data[22] << 8) | (uint32_t)data[23];
    if (width == 0 || height == 0)
        return false;
    *w = (int)width;
    *h = (int)height;
    return true;
}

bool probe_bmp(const uint8_t *data, size_t len, int *w, int *h)
{
    if (len < 26 || data[0] != 'B' || data[1] != 'M')
        return false;
    const int32_t width = (int32_t)read_le32(data + 18);
    const int32_t height = (int32_t)read_le32(data + 22);
    if (width <= 0 || height == 0)
        return false;
    *w = (int)width;
    *h = height < 0 ? (int)(-height) : (int)height;
    return true;
}

bool probe_jpeg(const uint8_t *data, size_t len, int *w, int *h)
{
    if (len < 4 || data[0] != 0xff || data[1] != 0xd8)
        return false;

    size_t pos = 2;
    while (pos + 4 <= len) {
        if (data[pos] != 0xff) {
            ++pos;
            continue;
        }
        const uint8_t marker = data[pos + 1];
        pos += 2;
        if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (pos + 2 > len)
            return false;
        const size_t segment_len = ((size_t)data[pos] << 8) | data[pos + 1];
        if (segment_len < 2 || pos + segment_len > len)
            return false;
        /* SOF0..SOF3, SOF5..SOF7, SOF9..SOF11, SOF13..SOF15 */
        const bool is_sof = (marker >= 0xc0 && marker <= 0xc3) ||
                            (marker >= 0xc5 && marker <= 0xc7) ||
                            (marker >= 0xc9 && marker <= 0xcb) ||
                            (marker >= 0xcd && marker <= 0xcf);
        if (is_sof && segment_len >= 7) {
            const uint32_t height = ((uint32_t)data[pos + 3] << 8) | data[pos + 4];
            const uint32_t width = ((uint32_t)data[pos + 5] << 8) | data[pos + 6];
            if (width == 0 || height == 0)
                return false;
            *w = (int)width;
            *h = (int)height;
            return true;
        }
        pos += segment_len;
    }
    return false;
}

} // namespace

Bitmap *load_image(const uint8_t *data, size_t len)
{
    if (!data) {
        fprintf(stderr, "ons: image decode failed: null data pointer\n");
        return nullptr;
    }
    if (len < 4) {
        decode_fail("unknown", "image data too short", data, len);
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(g_decode_mutex);

    if (len >= 8 && png_sig_cmp((png_const_bytep)data, 0, 8) == 0)
        return decode_png(data, len);
    if (data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return decode_jpeg(data, len);
    if (data[0] == 'B' && data[1] == 'M')
        return decode_bmp(data, len);

    decode_fail("unknown", "unrecognised image signature", data, len);
    return nullptr;
}

bool probe_image_size(const uint8_t *data, size_t len, int *w, int *h)
{
    if (!data || !w || !h)
        return false;

    std::lock_guard<std::mutex> guard(g_decode_mutex);

    if (probe_png(data, len, w, h))
        return true;
    if (probe_jpeg(data, len, w, h))
        return true;
    if (probe_bmp(data, len, w, h))
        return true;
    return false;
}

} // namespace ons
