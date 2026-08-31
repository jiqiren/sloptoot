#include "media_cache.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  const char *url = "https://example.test/avatar.png";

  /* The cache key is a 64-char lowercase hex digest of the URL — the O(1)
   * deterministic file name. */
  char *key = media_cache_key(url);
  if (key == nullptr) return 1;
  if (strlen(key) != 64) {
    printf("key has %zu chars (expected 64)\n", strlen(key));
    free(key);
    return 2;
  }
  free(key);

  /* Isolate the cache in a throwaway directory. */
  const char *base = getenv("TMPDIR");
  if (base == nullptr || base[0] == '\0') base = "/tmp";
  char root[512];
  snprintf(root, sizeof(root), "%s/sloptoot_media_test_XXXXXX", base);
  char *dir = mkdtemp(root);
  if (dir == nullptr) return 3;
  setenv("XDG_CACHE_HOME", dir, 1);

  /* A miss before anything was stored. */
  uint8_t *data = nullptr;
  size_t len = 0;
  if (media_cache_load(url, &data, &len)) return 4;

  /* Store then load round-trips the exact bytes. */
  const uint8_t payload[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3};
  if (!media_cache_store(url, payload, sizeof(payload))) return 5;
  if (!media_cache_load(url, &data, &len)) return 6;
  if (len != sizeof(payload) || memcmp(data, payload, sizeof(payload)) != 0) {
    free(data);
    return 7;
  }
  free(data);

  /* A different URL has its own key and is still a miss. */
  if (media_cache_load("https://example.test/other.png", &data, &len)) return 8;

  /* Clean up the media dir, its parent, and the temp root. */
  char media[600], parent[600];
  snprintf(media, sizeof(media), "%s/sloptoot/media", dir);
  snprintf(parent, sizeof(parent), "%s/sloptoot", dir);
  key = media_cache_key(url);
  char file[700];
  if (key != nullptr) {
    snprintf(file, sizeof(file), "%s/%s", media, key);
    unlink(file);
    free(key);
  }
  rmdir(media);
  rmdir(parent);
  rmdir(dir);
  return 0;
}