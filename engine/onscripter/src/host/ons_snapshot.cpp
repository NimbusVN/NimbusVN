/* -*- C++ -*-
 *
 *  ons_snapshot.cpp - write an engine Bitmap as PNG (libpng)
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "snapshot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <png.h>

namespace ons {

bool save_png(const Bitmap &bitmap, const char *path)
{
    if (!bitmap.pixels || bitmap.w <= 0 || bitmap.h <= 0 || !path || !*path)
        return false;

    FILE *fp = fopen(path, "wb");
    if (!fp)
        return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)bitmap.w, (png_uint_32)bitmap.h,
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    png_bytep row = (png_bytep)malloc((size_t)bitmap.w * 4u);
    if (!row) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    for (int y = 0; y < bitmap.h; ++y) {
        const uint32_t *src = bitmap.pixels + (size_t)y * (size_t)(bitmap.pitch / 4);
        png_bytep dst = row;
        for (int x = 0; x < bitmap.w; ++x) {
            uint32_t pixel = src[x];
            /* memory layout is B,G,R,A; PNG wants R,G,B,A */
            *dst++ = (png_byte)((pixel >> 16) & 0xff);
            *dst++ = (png_byte)((pixel >> 8) & 0xff);
            *dst++ = (png_byte)(pixel & 0xff);
            *dst++ = (png_byte)((pixel >> 24) & 0xff);
        }
        png_write_row(png, row);
    }

    free(row);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

} // namespace ons
