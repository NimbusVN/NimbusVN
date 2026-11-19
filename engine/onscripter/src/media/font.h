/* -*- C++ -*-
 *
 *  font.h - font and glyph interface for the NimbusVN ONScripter engine
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

#ifndef ONS_MEDIA_FONT_H
#define ONS_MEDIA_FONT_H

#include <cstdint>
#include <string>
#include <vector>

namespace ons {

/*
 * One rasterised glyph.  pixels holds 8-bit coverage values (0 = transparent,
 * 255 = fully covered), pitch bytes per row.
 */
struct Glyph {
    int w = 0;
    int h = 0;
    int pitch = 0;
    std::vector<uint8_t> pixels;
    int minx = 0;
    int maxx = 0;
    int miny = 0;
    int maxy = 0;
    int advance = 0;
};

class FontEngine;

class Font {
public:
    Font(FontEngine *owner, const std::string &path, int size);
    ~Font();

    /* Returns a cached glyph; never NULL for a valid font. */
    const Glyph *glyph(uint32_t unicode);

    int height() const { return height_; }
    int ascent() const { return ascent_; }
    int descent() const { return descent_; }
    int line_skip() const { return line_skip_; }
    int size() const { return size_; }

    bool has_glyph(uint32_t unicode) const;
    void set_style(bool bold, bool italic, bool underline, bool strike);
    int kerning(uint32_t first, uint32_t second);

    bool loaded() const { return loaded_; }
    bool bold() const { return bold_; }
    const std::string &path() const { return path_; }

private:
    struct Impl;

    FontEngine *owner_;
    std::string path_;
    int size_;
    int height_;
    int ascent_;
    int descent_;
    int line_skip_;
    bool loaded_;
    bool bold_;
    bool italic_;
    bool underline_;
    bool strike_;
    Glyph fallback_;
    Impl *impl_;
};

class FontEngine {
public:
    FontEngine();
    ~FontEngine();

    /* Returns an owning pointer; the engine keeps it until shutdown(). */
    Font *open(const std::string &path, int size);
    void shutdown();
    void drop(Font *font);

private:
    std::vector<Font *> fonts_;
};

} // namespace ons


using ons::Font;
using ons::FontEngine;
using ons::Glyph;

/* Diagnostic snapshot of every live Font: cached and retired glyph counts. */
void ons_font_stats(long *fonts, long *glyphs, long *retired);

#endif /* ONS_MEDIA_FONT_H */
