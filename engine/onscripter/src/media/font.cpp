/* -*- C++ -*-
 *
 *  font.cpp - FreeType backed font and glyph implementation
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
 *
 *  ---------------------------------------------------------------------
 *  Only FreeType is used here; there is no SDL_ttf compatibility layer.
 *  Glyphs are rasterised on demand and returned as 8-bit coverage maps.
 *  ---------------------------------------------------------------------
 */

#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_BITMAP_H

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ons {
namespace {

FT_Library g_library = NULL;
bool g_library_checked = false;

long g_font_count = 0;
long g_glyph_count = 0;
long g_retired_count = 0;

bool ensure_library()
{
    if (g_library_checked)
        return g_library != NULL;
    g_library_checked = true;

    FT_Error err = FT_Init_FreeType(&g_library);
    if (err) {
        fprintf(stderr, "ons: FT_Init_FreeType failed (FreeType error %d)\n", err);
        g_library = NULL;
        return false;
    }
    return true;
}

const char *ft_error_text(FT_Error err)
{
    /* FreeType does not expose a public string table; the numeric code is
     * enough to identify the failure in the log. */
    static char buf[64];
    snprintf(buf, sizeof(buf), "FreeType error %d", err);
    return buf;
}

/* 26.6 fixed point -> pixel, rounding towards -inf / +inf. */
inline int pix_floor(FT_Pos v)
{
    return v >= 0 ? (int)(v >> 6) : -(int)((-v + 63) >> 6);
}

inline int pix_ceil(FT_Pos v)
{
    return v >= 0 ? (int)((v + 63) >> 6) : -(int)((-v) >> 6);
}

void make_blank(Glyph &glyph, int advance)
{
    glyph.w = 1;
    glyph.h = 1;
    glyph.pitch = 1;
    glyph.pixels.assign(1, 0);
    glyph.minx = glyph.maxx = glyph.miny = glyph.maxy = 0;
    glyph.advance = advance;
}

/* Converts the FreeType slot bitmap into a tightly packed coverage map. */
void copy_bitmap(const FT_Bitmap &bitmap, Glyph &glyph)
{
    const int w = (int)bitmap.width;
    const int h = (int)bitmap.rows;

    glyph.w = w;
    glyph.h = h;
    glyph.pitch = w;
    glyph.pixels.assign((size_t)w * (size_t)h, 0);

    const unsigned char *src = bitmap.buffer;
    if (src == NULL)
        return;

    switch (bitmap.pixel_mode) {
    case FT_PIXEL_MODE_GRAY:
        for (int y = 0; y < h; ++y) {
            const unsigned char *row = src + (ptrdiff_t)y * bitmap.pitch;
            /* Rows are pitch bytes wide but only the first w are pixels. */
            memcpy(&glyph.pixels[(size_t)y * w], row, (size_t)w);
        }
        break;

    case FT_PIXEL_MODE_MONO:
        for (int y = 0; y < h; ++y) {
            const unsigned char *row = src + (ptrdiff_t)y * bitmap.pitch;
            for (int x = 0; x < w; ++x) {
                if (row[x >> 3] & (0x80 >> (x & 7)))
                    glyph.pixels[(size_t)y * w + x] = 255;
            }
        }
        break;

    case FT_PIXEL_MODE_LCD:
    case FT_PIXEL_MODE_LCD_V: {
        const int channels = 3;
        const int width_px = (bitmap.pixel_mode == FT_PIXEL_MODE_LCD) ? w : w;
        for (int y = 0; y < h; ++y) {
            const unsigned char *row = src + (ptrdiff_t)y * bitmap.pitch;
            for (int x = 0; x < width_px; ++x) {
                int sum = 0;
                for (int c = 0; c < channels; ++c)
                    sum += row[x * channels + c];
                glyph.pixels[(size_t)y * w + x] = (unsigned char)(sum / channels);
            }
        }
        break;
    }

    case FT_PIXEL_MODE_BGRA:
        for (int y = 0; y < h; ++y) {
            const unsigned char *row = src + (ptrdiff_t)y * bitmap.pitch;
            for (int x = 0; x < w; ++x)
                glyph.pixels[(size_t)y * w + x] = row[x * 4 + 3];
        }
        break;

    default:
        /* Unknown mode: leave the coverage map empty. */
        break;
    }
}

/* Draws a horizontal 1px rule at baseline relative height y. */
void draw_rule(Glyph &glyph, int y, int x0, int x1)
{
    if (glyph.h <= 0 || glyph.w <= 0)
        return;
    const int row = glyph.maxy - y;
    if (row < 0 || row >= glyph.h)
        return;

    x0 = std::max(0, x0);
    x1 = std::min(glyph.w, x1);
    if (x1 <= x0)
        return;

    unsigned char *dst = &glyph.pixels[(size_t)row * glyph.w];
    for (int x = x0; x < x1; ++x)
        dst[x] = 255;
}

/* Adds underline/strike decorations, extending the bitmap downwards when the
 * requested line sits below the glyph's bounding box. */
void add_decorations(Glyph &glyph, bool underline, bool strike, int ascent,
                     int advance)
{
    if (!underline && !strike)
        return;

    /* Rows are indexed from the top; the top row corresponds to glyph.maxy. */
    int needed_min = glyph.miny;
    if (underline)
        needed_min = std::min(needed_min, -3);
    if (needed_min < glyph.miny) {
        const int extra = glyph.miny - needed_min;
        const size_t old_size = glyph.pixels.size();
        glyph.pixels.resize(old_size + (size_t)glyph.w * (size_t)extra, 0);
        glyph.h += extra;
        glyph.miny = needed_min;
    }

    const int span = advance > 0 ? advance : glyph.w;
    const int x0 = 0;
    const int x1 = std::min(glyph.w, span);

    if (underline) {
        draw_rule(glyph, -1, x0, x1);
        draw_rule(glyph, -2, x0, x1);
    }
    if (strike) {
        int sy = ascent / 2;
        if (sy < 1)
            sy = glyph.maxy / 2;
        draw_rule(glyph, sy, x0, x1);
    }
}

/* Rasterises one codepoint; g is always left in a usable state. */
void rasterize(FT_Face face, uint32_t unicode, int font_size,
               bool bold, bool italic, bool underline, bool strike,
               int ascent, Glyph &g)
{
    const int default_advance = std::max(1, font_size / 2);
    const FT_UInt index = FT_Get_Char_Index(face, unicode);
    if (index == 0) {
        make_blank(g, default_advance);
        return;
    }

    /* Load the outline first so bold/italic transforms can be applied before
     * the bitmap is produced.  Bitmap-only fonts fall back below. */
    FT_Error err = FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP);
    FT_GlyphSlot slot = face->glyph;

    bool have_outline = (err == 0 && slot->format == FT_GLYPH_FORMAT_OUTLINE);

    if (have_outline) {
        FT_Pos strength = 0;
        if (bold) {
            strength = (FT_Pos)font_size * 64 / 24;
            if (strength < 32)
                strength = 32;
            FT_Outline_EmboldenXY(&slot->outline, strength, 0);
        }
        if (italic) {
            FT_Matrix shear;
            shear.xx = 0x10000L;
            shear.xy = (FT_Fixed)(0.21 * 65536.0);
            shear.yx = 0;
            shear.yy = 0x10000L;
            FT_Outline_Transform(&slot->outline, &shear);
        }

        FT_BBox box;
        FT_Outline_Get_CBox(&slot->outline, &box);
        g.minx = pix_floor(box.xMin);
        g.maxx = pix_ceil(box.xMax);
        g.miny = pix_floor(box.yMin);
        g.maxy = pix_ceil(box.yMax);

        err = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
        if (err)
            fprintf(stderr, "ons: FT_Render_Glyph(U+%04X) failed: %s\n",
                    unicode, ft_error_text(err));

        copy_bitmap(slot->bitmap, g);

        int advance = (int)(slot->advance.x >> 6);
        if (bold)
            advance += pix_ceil(strength);
        if (advance <= 0)
            advance = std::max(g.w, default_advance);
        g.advance = advance;
    } else {
        err = FT_Load_Glyph(face, index, FT_LOAD_DEFAULT | FT_LOAD_RENDER);
        if (err) {
            fprintf(stderr, "ons: FT_Load_Glyph(U+%04X) failed: %s\n",
                    unicode, ft_error_text(err));
            make_blank(g, default_advance);
            return;
        }

        copy_bitmap(slot->bitmap, g);
        g.minx = slot->bitmap_left;
        g.maxx = slot->bitmap_left + (int)slot->bitmap.width;
        g.maxy = slot->bitmap_top;
        g.miny = slot->bitmap_top - (int)slot->bitmap.rows;

        int advance = (int)(slot->advance.x >> 6);
        if (advance <= 0)
            advance = std::max(g.w, default_advance);
        g.advance = advance;
    }

    if (g.w <= 0 || g.h <= 0) {
        /* Whitespace and other empty glyphs: keep the advance only. */
        const int advance = g.advance;
        make_blank(g, advance > 0 ? advance : default_advance);
        return;
    }

    if (underline || strike)
        add_decorations(g, underline, strike, ascent, g.advance);
}

} // namespace

struct Font::Impl {
    FT_Face face = NULL;
    std::unordered_map<uint32_t, std::unique_ptr<Glyph>> glyphs;
    /* Glyphs evicted by a style change stay alive so the engine's own glyph
     * cache never holds a dangling pointer. */
    std::vector<std::unique_ptr<Glyph>> retired;
};

Font::Font(FontEngine *owner, const std::string &path, int size)
    : owner_(owner), path_(path), size_(size), height_(0), ascent_(0),
      descent_(0), line_skip_(0), loaded_(false), bold_(false),
      italic_(false), underline_(false), strike_(false), impl_(new Impl())
{
    fallback_.w = 1;
    fallback_.h = 1;
    fallback_.pitch = 1;
    fallback_.pixels.assign(1, 0);
    (void)owner_;
    ++g_font_count;

    if (size_ < 1)
        size_ = 12;

    if (!ensure_library())
        return;

    if (path_.empty()) {
        fprintf(stderr, "ons: cannot open font: empty path\n");
        return;
    }

    /* TTC/OTC collections always use face index 0. */
    FT_Error err = FT_New_Face(g_library, path_.c_str(), 0, &impl_->face);
    if (err) {
        fprintf(stderr, "ons: FT_New_Face('%s') failed: %s\n",
                path_.c_str(), ft_error_text(err));
        impl_->face = NULL;
        return;
    }

    err = FT_Set_Pixel_Sizes(impl_->face, 0, (FT_UInt)size_);
    if (err) {
        /* Some bitmap-only faces reject pixel sizes; try a char size. */
        err = FT_Set_Char_Size(impl_->face, 0, (FT_F26Dot6)size_ * 64, 72, 72);
        if (err) {
            fprintf(stderr, "ons: cannot set size %d on '%s': %s\n",
                    size_, path_.c_str(), ft_error_text(err));
            FT_Done_Face(impl_->face);
            impl_->face = NULL;
            return;
        }
    }

    const FT_Size_Metrics &metrics = impl_->face->size->metrics;
    ascent_ = pix_ceil(metrics.ascender);
    descent_ = pix_floor(metrics.descender);
    height_ = pix_ceil(metrics.height);
    if (height_ <= 0)
        height_ = ascent_ - descent_;
    if (height_ <= 0)
        height_ = size_;
    line_skip_ = height_ + 2;

    loaded_ = true;
    fprintf(stderr, "ons: FreeType font '%s' size %d (h=%d asc=%d desc=%d)\n",
            path_.c_str(), size_, height_, ascent_, descent_);
}

Font::~Font()
{
    if (impl_) {
        g_glyph_count -= (long)impl_->glyphs.size();
        g_retired_count -= (long)impl_->retired.size();
        if (impl_->face)
            FT_Done_Face(impl_->face);
        delete impl_;
        impl_ = NULL;
    }
    --g_font_count;
}

const Glyph *Font::glyph(uint32_t unicode)
{
    if (!loaded_ || !impl_ || !impl_->face)
        return &fallback_;

    std::unordered_map<uint32_t, std::unique_ptr<Glyph>>::iterator it =
        impl_->glyphs.find(unicode);
    if (it != impl_->glyphs.end())
        return it->second.get();

    std::unique_ptr<Glyph> created(new Glyph());
    rasterize(impl_->face, unicode, size_, bold_, italic_, underline_,
              strike_, ascent_, *created);

    const Glyph *result = created.get();
    impl_->glyphs.emplace(unicode, std::move(created));
    ++g_glyph_count;
    return result;
}

bool Font::has_glyph(uint32_t unicode) const
{
    if (!loaded_ || !impl_ || !impl_->face)
        return false;
    return FT_Get_Char_Index(impl_->face, unicode) != 0;
}

void Font::set_style(bool bold, bool italic, bool underline, bool strike)
{
    /* drawGlyph() calls this every frame with the same values; keep it a
     * no-op in that case so no face or cache work happens per frame. */
    if (bold == bold_ && italic == italic_ &&
        underline == underline_ && strike == strike_)
        return;

    const bool raster_changed = (bold != bold_) || (italic != italic_) ||
                                (underline != underline_) || (strike != strike_);

    bold_ = bold;
    italic_ = italic;
    underline_ = underline;
    strike_ = strike;

    if (!raster_changed || !impl_)
        return;

    /* Retire instead of destroying: the engine keeps raw Glyph pointers in
     * its own cache and they must stay valid. */
    for (std::unordered_map<uint32_t, std::unique_ptr<Glyph>>::iterator it =
             impl_->glyphs.begin(); it != impl_->glyphs.end(); ++it) {
        impl_->retired.push_back(std::move(it->second));
        --g_glyph_count;
        ++g_retired_count;
    }
    impl_->glyphs.clear();
}

int Font::kerning(uint32_t first, uint32_t second)
{
    if (!loaded_ || !impl_ || !impl_->face)
        return 0;

    const FT_UInt a = FT_Get_Char_Index(impl_->face, first);
    const FT_UInt b = FT_Get_Char_Index(impl_->face, second);
    if (a == 0 || b == 0)
        return 0;

    FT_Vector delta;
    if (FT_Get_Kerning(impl_->face, a, b, FT_KERNING_DEFAULT, &delta))
        return 0;

    return (int)(delta.x >> 6);
}

FontEngine::FontEngine()
{
}

FontEngine::~FontEngine()
{
    shutdown();
}

Font *FontEngine::open(const std::string &path, int size)
{
    if (size < 1)
        size = 12;

    for (size_t i = 0; i < fonts_.size(); ++i) {
        Font *existing = fonts_[i];
        if (existing && existing->path() == path && existing->size() == size)
            return existing;
    }

    Font *font = new Font(this, path, size);
    if (!font->loaded()) {
        fprintf(stderr, "ons: font open failed for '%s' (size %d)\n",
                path.c_str(), size);
        delete font;
        return NULL;
    }

    fonts_.push_back(font);
    return font;
}

void FontEngine::drop(Font *font)
{
    for (size_t i = 0; i < fonts_.size(); ++i) {
        if (fonts_[i] == font) {
            fonts_.erase(fonts_.begin() + i);
            delete font;
            return;
        }
    }
}

void FontEngine::shutdown()
{
    for (size_t i = 0; i < fonts_.size(); ++i)
        delete fonts_[i];
    fonts_.clear();
}

} // namespace ons

void ons_font_stats(long *fonts, long *glyphs, long *retired)
{
    if (fonts) *fonts = ons::g_font_count;
    if (glyphs) *glyphs = ons::g_glyph_count;
    if (retired) *retired = ons::g_retired_count;
}
