#pragma once

#include <stdbool.h>

#include "image.h"

/* Whether the terminal supports the kitty graphics protocol (probed once,
 * cached). */
bool kgraphics_supported(void);

/* Best-effort size in pixels of one terminal character cell (queried once via
 * CSI 16 t / 14 t, else the conventional 8x16). Always fills in sane
 * non-zero values. */
void kgraphics_cell_size(unsigned *w, unsigned *h);

/* Transmit a decoded image and place it at the current cursor position as a
 * `cols` x `rows` cell rectangle. The caller must supply a `cols`/`rows` pair
 * that matches the image's aspect ratio in cells (use kgraphics_cell_size
 * plus the image dimensions); the terminal scales to exactly that rectangle,
 * so a mismatched pair distorts the image. Animated images (2+ frames) are
 * transmitted and started. When `hold_cursor` is set the cursor is left where
 * it was (C=1), otherwise it advances past the image (the protocol default). */
bool kgraphics_render(const img_image *img, unsigned cols, unsigned rows,
                      bool hold_cursor);