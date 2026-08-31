#include "media_cache.h"

#include "http.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *media_cache_dir(void) {
  const char *xdg = getenv("XDG_CACHE_HOME");
  const char *home = getenv("HOME");
  if (xdg != nullptr && xdg[0] != '\0') {
    size_t n = strlen(xdg) + strlen("/sloptoot/media") + 1;
    char *p = malloc(n);
    if (p != nullptr) snprintf(p, n, "%s/sloptoot/media", xdg);
    return p;
  }
  if (home == nullptr || home[0] == '\0') return nullptr;
  size_t n = strlen(home) + strlen("/.cache/sloptoot/media") + 1;
  char *p = malloc(n);
  if (p != nullptr) snprintf(p, n, "%s/.cache/sloptoot/media", home);
  return p;
}

char *media_cache_key(const char *url) {
  if (url == nullptr) return nullptr;
  uint8_t digest[32];
  sha256_once((const uint8_t *)url, strlen(url), digest);
  static const char hex[] = "0123456789abcdef";
  char *key = malloc(sizeof(digest) * 2 + 1);
  if (key == nullptr) return nullptr;
  for (size_t i = 0; i < sizeof(digest); i++) {
    key[i * 2] = hex[digest[i] >> 4];
    key[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  key[sizeof(digest) * 2] = '\0';
  return key;
}

/* Create the cache directory (and its parents), mirroring config.c. */
static bool ensure_dir(const char *path) {
  char *copy = strdup(path);
  if (copy == nullptr) return false;
  for (char *p = copy + 1; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
        free(copy);
        return false;
      }
      *p = '/';
    }
  }
  bool ok = mkdir(copy, 0700) == 0 || errno == EEXIST;
  free(copy);
  return ok;
}

/* Cache file path for `url` (directory + hex key). Fresh allocation, or null. */
static char *cache_path(const char *url) {
  char *dir = media_cache_dir();
  char *key = media_cache_key(url);
  if (dir == nullptr || key == nullptr) {
    free(dir);
    free(key);
    return nullptr;
  }
  size_t n = strlen(dir) + 1 + strlen(key) + 1;
  char *path = malloc(n);
  if (path != nullptr) snprintf(path, n, "%s/%s", dir, key);
  free(dir);
  free(key);
  return path;
}

/* Read a whole file into a fresh buffer. */
static bool read_file(const char *path, uint8_t **out, size_t *outlen) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return false;
  }
  size_t len = (size_t)st.st_size;
  uint8_t *buf = malloc(len);
  if (buf == nullptr) {
    close(fd);
    return false;
  }
  size_t off = 0;
  while (off < len) {
    ssize_t got = read(fd, buf + off, len - off);
    if (got <= 0) {
      close(fd);
      free(buf);
      return false;
    }
    off += (size_t)got;
  }
  close(fd);
  *out = buf;
  *outlen = len;
  return true;
}

/* Write `data` to `path` atomically (temp file + rename) so a concurrent
 * render never observes a partially written cache file. */
static bool write_file_atomic(const char *path, const uint8_t *data,
                              size_t len) {
  size_t n = strlen(path) + strlen(".tmp") + 1;
  char *tmp = malloc(n);
  if (tmp == nullptr) return false;
  snprintf(tmp, n, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    free(tmp);
    return false;
  }
  if (fchmod(fd, 0600) != 0) {
    close(fd);
    unlink(tmp);
    free(tmp);
    return false;
  }
  size_t off = 0;
  bool bad = false;
  while (off < len && !bad) {
    ssize_t w = write(fd, data + off, len - off);
    if (w < 0) {
      bad = true;
    } else {
      off += (size_t)w;
    }
  }
  if (close(fd) != 0) bad = true;
  if (bad || rename(tmp, path) != 0) {
    unlink(tmp);
    free(tmp);
    return false;
  }
  free(tmp);
  return true;
}

bool media_cache_load(const char *url, uint8_t **out, size_t *outlen) {
  if (url == nullptr || out == nullptr || outlen == nullptr) return false;
  *out = nullptr;
  *outlen = 0;
  char *path = cache_path(url);
  if (path == nullptr) return false;
  bool ok = read_file(path, out, outlen);
  free(path);
  return ok;
}

bool media_cache_store(const char *url, const uint8_t *data, size_t len) {
  if (url == nullptr || data == nullptr || len == 0) return false;
  char *dir = media_cache_dir();
  if (dir == nullptr || !ensure_dir(dir)) {
    free(dir);
    return false;
  }
  free(dir);
  char *path = cache_path(url);
  if (path == nullptr) return false;
  bool ok = write_file_atomic(path, data, len);
  free(path);
  return ok;
}

bool media_cache_fetch(const char *url, uint8_t **out, size_t *outlen) {
  if (url == nullptr || out == nullptr || outlen == nullptr) return false;

  /* Cache hit: return the stored bytes without touching the network. */
  if (media_cache_load(url, out, outlen)) return true;

  /* Media and avatars are public (often served from a CDN other than the
   * instance), so no auth is sent. */
  http_response resp = {0};
  if (!http_get(url, nullptr, &resp) || resp.code < 200 || resp.code >= 300 ||
      resp.len == 0 || resp.body == nullptr) {
    http_response_free(&resp);
    return false;
  }

  /* Miss: hand back the fresh bytes and persist them for next time. A storage
   * failure is never fatal — the caller still gets the image. */
  *out = (uint8_t *)resp.body;
  *outlen = resp.len;
  resp.body = nullptr; /* ownership moved to the caller */
  media_cache_store(url, *out, *outlen);
  return true;
}