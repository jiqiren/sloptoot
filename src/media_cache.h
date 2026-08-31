#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The on-disk media cache directory
 * (`$XDG_CACHE_HOME/sloptoot/media`, falling back to
 * `$HOME/.cache/sloptoot/media`). Fresh allocation, caller frees. */
char *media_cache_dir(void);

/* Hex SHA-256 of `url` — the deterministic cache file name for it, so a lookup
 * is a single open() by URL. Fresh allocation, caller frees. */
char *media_cache_key(const char *url);

/* Best-effort store of `data` under `url`'s cache key. Creates the cache
 * directory as needed. Returns false without touching anything required for
 * rendering (a storage failure is never fatal). */
bool media_cache_store(const char *url, const uint8_t *data, size_t len);

/* Load `url`'s cached bytes. On a hit `*out`/`*outlen` are set to a fresh
 * buffer the caller frees. Returns false on a miss. */
bool media_cache_load(const char *url, uint8_t **out, size_t *outlen);

/* Fetch `url`'s raw bytes: a cache hit returns the stored file without any
 * network; a miss downloads the (public, unauthenticated) media and stores it
 * for next time. `*out`/`*outlen` are set on success (caller frees). */
bool media_cache_fetch(const char *url, uint8_t **out, size_t *outlen);