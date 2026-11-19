/* -*- C++ -*-
 *
 *  snapshot.h - PNG output shared by the host and the save-screenshot command
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef ONS_CORE_SNAPSHOT_H
#define ONS_CORE_SNAPSHOT_H

#include "bitmap.h"

namespace ons {

/* Writes the bitmap as a PNG (RGBA8).  Implemented in host/ons_snapshot.cpp. */
bool save_png(const Bitmap &bitmap, const char *path);

} // namespace ons

#endif /* ONS_CORE_SNAPSHOT_H */
