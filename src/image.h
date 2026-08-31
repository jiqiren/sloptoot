#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One decoded frame: an RGBA (4 bytes/pixel, row-major) canvas. */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t delay_ms; /* time to show before advancing to the next frame */
  uint8_t *rgba;
} img_frame;

/* A decoded image: one or more frames (GIF/WebP animations), plus the loop
 * count the file declares (0 means loop forever). */
typedef struct {
  img_frame *frames;
  size_t nframes;
  uint32_t loop_count;
} img_image;

/* Decode a JPEG/PNG/GIF/WebP buffer into RGBA frames. Format is detected
 * from the magic bytes, so `data` does not need an accompanying MIME type.
 * Returns 0 on success, nonzero on decode/unsupported-format errors.
 * Caller must call img_free() on `out`. */
[[nodiscard]] int img_decode(const uint8_t *data, size_t len, img_image *out);

/* Encode a single RGBA frame as a PNG (deflate-compressed, 8-bit RGBA).
 * `*out` is a fresh allocation the caller frees with free(). Returns 0 on
 * success. */
[[nodiscard]] int img_encode_png(const img_frame *f, uint8_t **out,
                                 size_t *outlen);

/* Compute the largest `cols` x `rows` cell rectangle that displays the image
 * within `cmax` x `rmax` cells while preserving its pixel aspect ratio, using
 * the terminal's character cell size (`cell_w` x `cell_h` pixels). Both
 * outputs are at least 1. */
void img_fit_aspect(const img_image *im, unsigned cmax, unsigned rmax,
                    unsigned cell_w, unsigned cell_h, unsigned *cols,
                    unsigned *rows);

void img_free(img_image *img);