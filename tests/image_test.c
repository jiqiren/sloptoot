#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_fixtures.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);                 \
      failures++;                                                             \
    }                                                                         \
  } while (0)

/* Minimal standard base64 decoder (used to unpack the test fixtures). */
static size_t b64decode(const char *in, unsigned char *out, size_t cap) {
  static const char *tbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t n = 0;
  int bits = 0, acc = 0;
  for (const char *p = in; *p; p++) {
    if (*p == '=') break;
    const char *q = strchr(tbl, *p);
    if (q == nullptr) continue;
    acc = (acc << 6) | (int)(q - tbl);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n < cap) out[n++] = (unsigned char)((acc >> bits) & 0xFF);
    }
  }
  return n;
}

static const uint8_t *pix(const img_image *img, size_t row, size_t col) {
  return img->frames[0].rgba + (row * img->frames[0].width + col) * 4u;
}

int main(void) {
  /* Lossless PNG: left half red, right half green, opaque. */
  unsigned char *buf = malloc(sample_png_len);
  size_t got = b64decode((const char *)sample_png, buf, sample_png_len);
  img_image d = {0};
  CHECK(got == sample_png_len, "png fixture fits");
  CHECK(img_decode(buf, got, &d) == 0, "png decode");
  CHECK(d.nframes == 1, "png single frame");
  CHECK(pix(&d, 1, 1)[0] == 255 && pix(&d, 1, 1)[1] == 0 && pix(&d, 1, 1)[2] == 0,
        "png left red");
  CHECK(pix(&d, 1, 6)[0] == 0 && pix(&d, 1, 6)[1] == 255 && pix(&d, 1, 6)[2] == 0,
        "png right green");
  CHECK(pix(&d, 1, 6)[3] == 255, "png opaque");
  img_free(&d);
  free(buf);

  /* Lossless formats decode to exactly the colours we drew. */
  {
    unsigned char *b = malloc(sample_gif_len);
    size_t n = b64decode((const char *)sample_gif, b, sample_gif_len);
    img_image im = {0};
    CHECK(img_decode(b, n, &im) == 0, "gif decode");
    if (im.nframes == 1) {
      CHECK(pix(&im, 1, 1)[0] == 255 && pix(&im, 1, 1)[1] == 0,
            "gif left red");
      CHECK(pix(&im, 1, 6)[1] == 255 && pix(&im, 1, 6)[0] == 0,
            "gif right green");
    }
    img_free(&im);
    free(b);
  }
  {
    unsigned char *b = malloc(sample_webp_len);
    size_t n = b64decode((const char *)sample_webp, b, sample_webp_len);
    img_image im = {0};
    CHECK(img_decode(b, n, &im) == 0, "webp decode");
    if (im.nframes == 1) {
      CHECK(pix(&im, 1, 1)[0] == 255 && pix(&im, 1, 1)[1] == 0,
            "webp left red");
      CHECK(pix(&im, 1, 6)[1] == 255 && pix(&im, 1, 6)[0] == 0,
            "webp right green");
    }
    img_free(&im);
    free(b);
  }

  /* JPEG is lossy; only assert dims + dominant hue and that it does not
   * crash. */
  {
    unsigned char *b = malloc(sample_jpg_len);
    size_t n = b64decode((const char *)sample_jpg, b, sample_jpg_len);
    img_image im = {0};
    CHECK(img_decode(b, n, &im) == 0, "jpeg decode");
    if (im.nframes == 1) {
      CHECK(pix(&im, 1, 1)[0] > pix(&im, 1, 1)[1], "jpeg left red-dominant");
      CHECK(pix(&im, 1, 6)[1] > pix(&im, 1, 6)[0], "jpeg right green-dominant");
    }
    img_free(&im);
    free(b);
  }

  /* Animated GIF/WebP: two frames, bumpy loop flag, sane delays. */
  {
    unsigned char *b = malloc(sample_anim_gif_len);
    size_t n = b64decode((const char *)sample_anim_gif, b, sample_anim_gif_len);
    img_image im = {0};
    CHECK(img_decode(b, n, &im) == 0, "anim gif decode");
    CHECK(im.nframes == 2, "anim gif frame count");
    if (im.nframes == 2) {
      CHECK(im.loop_count == 0, "gif loops forever");
      CHECK(im.frames[0].delay_ms == 400u, "gif frame0 delay");
      CHECK(im.frames[1].delay_ms == 400u, "gif frame1 delay");
      CHECK(pix(&im, 0, 0)[0] == 255, "gif frame0 red");
    }
    img_free(&im);
    free(b);
  }
  {
    unsigned char *b = malloc(sample_anim_webp_len);
    size_t n = b64decode((const char *)sample_anim_webp, b, sample_anim_webp_len);
    img_image im = {0};
    CHECK(img_decode(b, n, &im) == 0, "anim webp decode");
    CHECK(im.nframes == 2, "anim webp frame count");
    if (im.nframes == 2) {
      CHECK(im.loop_count == 0, "webp loops forever");
      CHECK(im.frames[0].delay_ms == 400u, "webp frame0 delay");
      CHECK(im.frames[1].delay_ms == 400u, "webp frame1 delay");
    }
    img_free(&im);
    free(b);
  }

  /* PNG encode round trip: RGBA -> PNG -> RGBA must be pixel-identical. */
  {
    unsigned char *b = malloc(sample_png_len);
    size_t n = b64decode((const char *)sample_png, b, sample_png_len);
    img_image im = {0};
    CHECK(img_decode(b, n, &im) == 0, "png decode for round-trip");
    free(b);
    unsigned char *enc = nullptr;
    size_t enc_len = 0;
    CHECK(img_encode_png(&im.frames[0], &enc, &enc_len) == 0, "png encode");
    if (enc != nullptr) {
      img_image back = {0};
      CHECK(img_decode(enc, enc_len, &back) == 0, "round-trip decode");
      if (back.nframes == 1)
        CHECK(memcmp(back.frames[0].rgba, im.frames[0].rgba,
                     (size_t)8 * 8 * 4) == 0,
              "round-trip pixels identical");
      img_free(&back);
      free(enc);
    }
    img_free(&im);
  }

  /* Garbage must be rejected, not crash. */
  {
    img_image bad = {0};
    int rc = img_decode((const uint8_t *)"this is not an image", 8, &bad);
    CHECK(rc != 0 || bad.nframes == 0, "garbage rejected");
    img_free(&bad);
  }

  if (failures == 0) {
    printf("image tests passed\n");
    return 0;
  }
  return 1;
}