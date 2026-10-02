// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/gfx/klogo.h */
/* Aurora Tejeda / CATX Systems */
/* Vector CX logo renderer (even-odd scanline fill, integer-only). */

#pragma once

#include <stdint.h>

/* Rasterize the logo into a `size` x `size` px box at (ox, oy). Transparent
   background (composites over existing framebuffer contents). */
void klogo_draw(uint32_t ox, uint32_t oy, uint32_t size);

/* Centered convenience: clamps so an oversized logo can't underflow, and
   size 0 auto-fits ~60% of the shorter screen dimension. */
void klogo_draw_centered(uint32_t size);