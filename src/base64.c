#include "base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static const char alphabet_std[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_common(const char *alpha, bool pad, const uint8_t *data,
                           size_t len) {
  size_t out_len = (len + 2) / 3 * 4;
  char *out = malloc(out_len + 1);
  if (out == nullptr) return nullptr;

  size_t i = 0, j = 0;
  while (i + 3 <= len) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                 (uint32_t)data[i + 2];
    out[j++] = alpha[(v >> 18) & 0x3F];
    out[j++] = alpha[(v >> 12) & 0x3F];
    out[j++] = alpha[(v >> 6) & 0x3F];
    out[j++] = alpha[v & 0x3F];
    i += 3;
  }

  size_t rem = len - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)data[i] << 16;
    out[j++] = alpha[(v >> 18) & 0x3F];
    out[j++] = alpha[(v >> 12) & 0x3F];
    if (pad) out[j++] = '=';
    if (pad) out[j++] = '=';
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out[j++] = alpha[(v >> 18) & 0x3F];
    out[j++] = alpha[(v >> 12) & 0x3F];
    out[j++] = alpha[(v >> 6) & 0x3F];
    if (pad) out[j++] = '=';
  }

  out[j] = '\0';
  return out;
}

char *base64_encode(const uint8_t *data, size_t len) {
  return base64_common(alphabet_std, true, data, len);
}

char *base64url_encode(const uint8_t *data, size_t len) {
  return base64_common(alphabet, false, data, len);
}

char *random_verifier(size_t bytes) {
  uint8_t *buf = malloc(bytes);
  if (buf == nullptr) return nullptr;

  FILE *f = fopen("/dev/urandom", "rb");
  if (f == nullptr) {
    free(buf);
    return nullptr;
  }
  size_t got = fread(buf, 1, bytes, f);
  fclose(f);
  if (got != bytes) {
    free(buf);
    return nullptr;
  }

  char *enc = base64url_encode(buf, bytes);
  free(buf);
  return enc;
}
