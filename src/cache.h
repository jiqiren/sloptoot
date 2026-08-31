#pragma once

#include <sqlite3.h>

#include <stdbool.h>
#include <stddef.h>

#include "json_helpers.h"

typedef struct {
  sqlite3 *db;
  char *path;
} cache;

/* Open (creating if needed) the per-user cache database. Returns false on
 * failure. */
[[nodiscard]] bool cache_open(cache *c);

/* Close the cache, freeing resources. Safe to call multiple times. */
void cache_close(cache *c);

/* Store a list of status objects (JSON array) under a timeline type. */
void cache_store_statuses(cache *c, const cJSON *timeline, const char *type);

/* Store a single status object (a boost wrapper also stores its inner post),
 * without associating it with a timeline. */
void cache_store_status(cache *c, const cJSON *status);

/* Print a detailed view of one cached status, mirroring the online view as
 * far as stored fields allow (handle, text, posted time, id). Returns false
 * if the id is not in the cache. */
[[nodiscard]] bool cache_view(const cache *c, const char *id);

/* Print cached statuses for a timeline type as plain text lines. When `wrap`
 * is true the full text is printed; otherwise each line is truncated to the
 * terminal width. */
void cache_list(const cache *c, const char *type, bool wrap);

/* Reconstruct the cached timeline of `type` as a cJSON array of status
 * objects (same shape `ls` renders from), including each account's avatar URL
 * and the media_attachments URLs, so `ls --mobile` can draw avatars/images
 * from the local media cache. Caller frees with json_free(). Returns nullptr
 * on no rows or OOM. */
[[nodiscard]] cJSON *cache_list_json(const cache *c, const char *type);

/* Print a cJSON array of statuses (newest-first). Boost wrappers render with
 * their inner post on an indented line. */
void cache_list_items(const cache *c, const cJSON *arr, bool wrap);

/* Number of terminal rows, or a default when not a TTY / unknown. */
int cache_terminal_rows(void);

/* Strip HTML tags and decode common entities from a status's content into
 * `out` (plain text for terminal display). */
void cache_plain_text(const char *src, char *out, size_t cap);

/* Word-wrap `text` to at most `cols` display columns (wide/emoji-aware),
 * inserting '\n' at word boundaries. `out` receives the wrapped text
 * (NUL-terminated; safe to call with cap 0). Returns the number of display
 * lines the wrapped text occupies. */
size_t cache_wrap(const char *text, size_t cols, char *out, size_t cap);

/* Fill `out` with `s` truncated to `cols` display columns without splitting a
 * multibyte sequence (appends an ellipsis when cut). Returns columns used. */
size_t cache_truncate(const char *s, size_t cols, char *out, size_t cap);

/* Format an ISO-8601 status timestamp as a relative time ("5m", "3h", ...). */
void cache_relative_time(const char *iso, char *out, size_t cap);

/* Remove a status (and its timeline associations) from the cache. If the id
 * matches the chain anchor, that anchor is cleared too. */
void cache_delete_status(cache *c, const char *id);

/* Record which timeline type was most recently loaded. */
void cache_set_last_type(cache *c, const char *type);

/* The timeline type most recently loaded, or nullptr if none. Fresh
 * allocation; caller frees. */
[[nodiscard]] char *cache_last_type(const cache *c);

/* The id of the most recent successfully posted status, or nullptr if none.
 * Returns a fresh allocation; caller frees. */
[[nodiscard]] char *cache_last_post_id(const cache *c);

/* Persist the id of a just-posted status as the chain anchor. */
void cache_set_last_post_id(cache *c, const char *id);