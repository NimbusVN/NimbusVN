/* -*- C++ -*-
 *
 *  image.h - image decoding interface
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef ONS_MEDIA_IMAGE_H
#define ONS_MEDIA_IMAGE_H

#include "bitmap.h"

#include <cstddef>
#include <cstdint>

namespace ons {

/*
 * Decodes PNG/JPEG/BMP bytes into a new ARGB8888 bitmap.  The caller owns the
 * result.  Returns NULL when the data cannot be decoded.
 */
Bitmap *load_image(const uint8_t *data, size_t len);

/* Guesses the image size without decoding, when the format allows it. */
bool probe_image_size(const uint8_t *data, size_t len, int *w, int *h);

} // namespace ons

#endif /* ONS_MEDIA_IMAGE_H */
