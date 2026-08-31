#include "cache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Defined below; used by the media-attachment store. */
static void exec_bind(sqlite3 *db, const char *sql, const char *v);

/* Make the parent directories of `path` exist (like config.c). */
static void ensure_dir(const char *path) {
  char *copy = strdup(path);
  if (copy == nullptr) return;
  char *slash = strrchr(copy, '/');
  if (slash != nullptr && slash != copy) {
    *slash = '\0';
    for (char *p = copy + 1; *p != '\0'; p++) {
      if (*p == '/') {
        *p = '\0';
        mkdir(copy, 0700);
        *p = '/';
      }
    }
    mkdir(copy, 0700);
  }
  free(copy);
}

/* Cache database lives alongside the credentials file so both honor XDG. */
static char *cache_path(void) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home;
  if (xdg != nullptr && xdg[0] != '\0') {
    size_t n = strlen(xdg) + strlen("/sloptoot/cache.db") + 1;
    char *p = malloc(n);
    if (p != nullptr) snprintf(p, n, "%s/sloptoot/cache.db", xdg);
    return p;
  }
  home = getenv("HOME");
  if (home == nullptr || home[0] == '\0') return nullptr;
  size_t n = strlen(home) + strlen("/.config/sloptoot/cache.db") + 1;
  char *p = malloc(n);
  if (p != nullptr) snprintf(p, n, "%s/.config/sloptoot/cache.db", home);
  return p;
}

bool cache_open(cache *c) {
  c->db = nullptr;
  c->path = cache_path();
  if (c->path == nullptr) return false;
  ensure_dir(c->path);
  sqlite3 *db = nullptr;
  if (sqlite3_open(c->path, &db) != SQLITE_OK || db == nullptr) {
    if (db != nullptr) sqlite3_close(db);
    return false;
  }
  c->db = db;

  /* Schema: statuses are deduplicated by id; a many-to-many table records
   * which timeline type surfaced each one. `status_media` holds each post's
   * `media_attachments` (URL + `preview_url` thumbnail + caption), so a
   * cached `ls --mobile` can render images from the local media cache without
   * the network. `meta` stores small scalars. A boost wrapper row carries the
   * boosted (inner) status id in `reblog_of`, the booster's handle in
   * `account`, and the booster's avatar in `avatar`. */
  const char *schema =
      "CREATE TABLE IF NOT EXISTS statuses("
      " id TEXT PRIMARY KEY,"
      " created_at TEXT, account TEXT, content TEXT,"
      " reblog_of TEXT);"
      "CREATE TABLE IF NOT EXISTS status_media("
      " status_id TEXT NOT NULL, pos INTEGER NOT NULL,"
      " mime TEXT, url TEXT, preview TEXT, description TEXT, type TEXT,"
      " PRIMARY KEY(status_id, pos));"
      "CREATE TABLE IF NOT EXISTS timeline_statuses("
      " type TEXT NOT NULL, id TEXT NOT NULL,"
      " PRIMARY KEY(type, id));"
      "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);";
  if (sqlite3_exec(db, schema, nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    c->db = nullptr;
    return false;
  }

  /* Older databases predate the `avatar` column; migrate them on open. */
  bool has_avatar = false;
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA table_info(statuses);", -1, &st,
                         nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *name = (const char *)sqlite3_column_text(st, 1);
      if (name != nullptr && strcmp(name, "avatar") == 0) has_avatar = true;
    }
  }
  if (st != nullptr) sqlite3_finalize(st);
  if (!has_avatar)
    sqlite3_exec(db, "ALTER TABLE statuses ADD COLUMN avatar TEXT;", nullptr,
                 nullptr, nullptr);
  return true;
}

void cache_close(cache *c) {
  if (c->db != nullptr) sqlite3_close(c->db);
  c->db = nullptr;
  free(c->path);
  c->path = nullptr;
}

static const char *status_field(const cJSON *status, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(status, key);
  if (item == nullptr || !cJSON_IsString(item)) return "";
  return item->valuestring;
}

/* cJSON array children; cJSON exposes them as a linked list. */
static const cJSON *first_array_item(const cJSON *arr) {
  if (arr == nullptr || !cJSON_IsArray(arr)) return nullptr;
  return arr->child;
}

static const cJSON *next_array_item(const cJSON *item) {
  return item == nullptr ? nullptr : item->next;
}

static const char *haccount(const cJSON *status) {
  const cJSON *account = cJSON_GetObjectItemCaseSensitive(status, "account");
  if (account != nullptr && cJSON_IsObject(account))
    return status_field(account, "acct");
  return "";
}

/* The account's avatar URL (static when available, else the animated one). */
static const char *havatar(const cJSON *status) {
  const cJSON *account = cJSON_GetObjectItemCaseSensitive(status, "account");
  if (account != nullptr && cJSON_IsObject(account)) {
    const char *av = status_field(account, "avatar_static");
    if (av[0] == '\0') av = status_field(account, "avatar");
    return av;
  }
  return "";
}

/* Insert one status row (and note it appeared in `type`). `reblog_of` is the
 * boosted inner status id when this row is a boost wrapper, else null. */
/* Upsert the status row itself (account/avatar/created/content/reblog_of). This
 * is shared by normal posts and inner boosted posts. */
static void store_status_row(cache *c, const cJSON *status,
                             const char *reblog_of, const char *avatar) {
  sqlite3_stmt *st = nullptr;
  const char *upsert = "INSERT OR REPLACE INTO statuses"
                       "(id, created_at, account, content, reblog_of, avatar)"
                       " VALUES(?1,?2,?3,?4,?5,?6);";
  if (sqlite3_prepare_v2(c->db, upsert, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, status_field(status, "id"), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, status_field(status, "created_at"), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, haccount(status), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, status_field(status, "content"), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, reblog_of, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, avatar, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
  }
  if (st != nullptr) sqlite3_finalize(st);
}

/* Replace the media_attachments rows for one status id. */
static void store_media(cache *c, const cJSON *status) {
  if (c == nullptr || c->db == nullptr || status == nullptr) return;
  const char *id = status_field(status, "id");
  if (id[0] == '\0') return;
  exec_bind(c->db, "DELETE FROM status_media WHERE status_id=?1;", id);
  const cJSON *media =
      cJSON_GetObjectItemCaseSensitive(status, "media_attachments");
  if (media == nullptr || !cJSON_IsArray(media)) return;
  int n = cJSON_GetArraySize(media);
  for (int i = 0; i < n; i++) {
    const cJSON *att = cJSON_GetArrayItem(media, i);
    const char *mime = json_get_string(att, "mime_type");
    const char *url = json_get_string(att, "url");
    const char *preview = json_get_string(att, "preview_url");
    const char *desc = json_get_string(att, "description");
    const char *type = json_get_string(att, "type");
    sqlite3_stmt *st = nullptr;
    const char *ins = "INSERT OR REPLACE INTO status_media"
                      "(status_id, pos, mime, url, preview, description, type)"
                      " VALUES(?1,?2,?3,?4,?5,?6,?7);";
    if (sqlite3_prepare_v2(c->db, ins, -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(st, 2, i);
      sqlite3_bind_text(st, 3, mime != nullptr ? mime : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 4, url != nullptr ? url : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 5, preview != nullptr ? preview : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 6, desc != nullptr ? desc : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 7, type != nullptr ? type : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_step(st);
    }
    if (st != nullptr) sqlite3_finalize(st);
  }
}

/* Record that a status appears in a timeline. */
static void write_timeline_member(cache *c, const char *id, const char *type) {
  sqlite3_stmt *st = nullptr;
  const char *ins = "INSERT OR IGNORE INTO timeline_statuses(type,id)"
                    " VALUES(?1,?2);";
  if (sqlite3_prepare_v2(c->db, ins, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
  }
  if (st != nullptr) sqlite3_finalize(st);
}

/* Write a timeline-associated row: the status (with avatar + media) plus its
 * membership in `type`. Inner boosted posts are stored via store_status_row
 * only. */
static void write_status(cache *c, const cJSON *status, const char *type) {
  store_status_row(c, status, nullptr, havatar(status));
  store_media(c, status);
  write_timeline_member(c, status_field(status, "id"), type);
}

static void status_to_db(cache *c, const cJSON *status, const char *type) {
  const cJSON *reblog = cJSON_GetObjectItemCaseSensitive(status, "reblog");
  if (reblog != nullptr && cJSON_IsObject(reblog)) {
    /* Store the boosted (inner) post for lookup; the wrapper marks it. */
    store_status_row(c, reblog, nullptr, havatar(reblog));
    store_media(c, reblog);
    const char *inner_id = status_field(reblog, "id");
    if (inner_id[0] == '\0') inner_id = nullptr;
    /* The wrapper carries its own id/account(booster)/avatar/content and
     * reblog_of. */
    store_status_row(c, status, inner_id, havatar(status));
    write_timeline_member(c, status_field(status, "id"), type);
  } else {
    write_status(c, status, type);
  }
}

void cache_store_statuses(cache *c, const cJSON *timeline, const char *type) {
  for (const cJSON *it = first_array_item(timeline); it != nullptr;
       it = next_array_item(it)) {
    status_to_db(c, it, type);
  }
}

/* Store a single status object — a boost wrapper plus its inner post —
 * without tying either to a timeline (used after fetching one status). */
void cache_store_status(cache *c, const cJSON *status) {
  if (c == nullptr || c->db == nullptr || status == nullptr) return;
  const cJSON *reblog = cJSON_GetObjectItemCaseSensitive(status, "reblog");
  if (reblog != nullptr && cJSON_IsObject(reblog)) {
    store_status_row(c, reblog, nullptr, havatar(reblog));
    store_media(c, reblog);
    const char *inner_id = status_field(reblog, "id");
    if (inner_id[0] == '\0') inner_id = nullptr;
    store_status_row(c, status, inner_id, havatar(status));
  } else {
    store_status_row(c, status, nullptr, havatar(status));
    store_media(c, status);
  }
}

/* Strip HTML tags and unescape common entities for plain-text display. */
static void plain_text(const char *src, char *out, size_t cap) {  size_t j = 0;
  bool in_tag = false;
  for (size_t i = 0; src[i] != '\0' && j + 1 < cap; i++) {
    char ch = src[i];
    if (ch == '<') {
      in_tag = true;
      continue;
    }
    if (ch == '>') {
      in_tag = false;
      continue;
    }
    if (in_tag) continue;
    if (ch == '&') {
      if (strncmp(&src[i], "&amp;", 5) == 0) {
        out[j++] = '&';
        i += 4;
        continue;
      }
      if (strncmp(&src[i], "&lt;", 4) == 0) {
        out[j++] = '<';
        i += 3;
        continue;
      }
      if (strncmp(&src[i], "&gt;", 4) == 0) {
        out[j++] = '>';
        i += 3;
        continue;
      }
      if (strncmp(&src[i], "&quot;", 6) == 0) {
        out[j++] = '"';
        i += 5;
        continue;
      }
      if (strncmp(&src[i], "&apos;", 6) == 0) {
        out[j++] = '\'';
        i += 5;
        continue;
      }
      if (strncmp(&src[i], "&#39;", 5) == 0) {
        out[j++] = '\'';
        i += 4;
        continue;
      }
    }
    out[j++] = ch;
  }
  out[j] = '\0';
}

void cache_plain_text(const char *src, char *out, size_t cap) {
  plain_text(src, out, cap);
}

static size_t terminal_cols(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return (size_t)ws.ws_col;
  return 80;
}

int cache_terminal_rows(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    return (int)ws.ws_row;
  return 24;
}

/* Decode one UTF-8 code point starting at `s`. Returns the code point and sets
 * `*len` to the number of bytes consumed (1 for ASCII or on invalid input). */
static unsigned long utf8_decode(const char *s, size_t *len) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) {
    *len = 1;
    return c;
  }
  size_t n;
  unsigned long cp = 0;
  if ((c & 0xE0) == 0xC0) {
    n = 2;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    n = 3;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    n = 4;
    cp = c & 0x07;
  } else {
    *len = 1;
    return c; /* invalid lead byte; treat as one column */
  }
  for (size_t i = 1; i < n; i++) {
    unsigned char cont = (unsigned char)s[i];
    if ((cont & 0xC0) != 0x80) {
      *len = 1;
      return c; /* malformed continuation; bail */
    }
    cp = (cp << 6) | (cont & 0x3F);
  }
  *len = n;
  return cp;
}

/* Approximate terminal display width of a code point: 0 for combining marks,
 * 2 for East Asian wide/fullwidth and emoji, 1 otherwise. */
static int char_disp_width(unsigned long cp) {
  if (cp == 0) return 0;
  /* Combining / zero-width marks (roughly U+0300..U+036F and friends). */
  if (cp >= 0x0300 && cp <= 0x036F) return 0;
  if (cp >= 0x1AB0 && cp <= 0x1AFF) return 0;
  if (cp >= 0x1DC0 && cp <= 0x1DFF) return 0;
  if (cp >= 0x20D0 && cp <= 0x20FF) return 0;
  if (cp >= 0xFE20 && cp <= 0xFE2F) return 0;
  /* East Asian Wide / Fullwidth — display as 2 columns. */
  if ((cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul jamo */
      (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK radicals, punctuation */
      (cp >= 0x3041 && cp <= 0x33FF) ||   /* Hiragana..CJK compat */
      (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK ext A */
      (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK unified */
      (cp >= 0xA000 && cp <= 0xA4CF) ||   /* Yi */
      (cp >= 0xA960 && cp <= 0xA97F) ||   /* Hangul jamo ext A */
      (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul syllables */
      (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compat */
      (cp == 0x2026) ||                   /* … is East Asian Wide */
      (cp >= 0xFE30 && cp <= 0xFE4F) ||   /* CJK compat forms */
      (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms */
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||   /* fullwidth signs */
      (cp >= 0x1F000 && cp <= 0x1FAFF) || /* emoji */
      (cp >= 0x20000 && cp <= 0x3FFFD))   /* CJK ext B+ */
    return 2;
  return 1;
}

/* Ellipsis shown when a line is truncated. */
#define ELLIPSIS "\xE2\x80\xA6" /* U+2026 … */
#define ELLIPSIS_W 2

/* Fill `out` with `s` truncated to fit `cols` display columns without splitting
 * a multibyte sequence; a trailing ellipsis is appended when anything is cut.
 * Returns the number of display columns used. */
static size_t truncate_to_cols(const char *s, size_t cols, char *out,
                               size_t outcap) {
  /* Total display width of the whole string. */
  size_t total = 0;
  size_t i = 0;
  while (s[i] != '\0') {
    size_t n;
    unsigned long cp = utf8_decode(s + i, &n);
    total += (size_t)char_disp_width(cp);
    i += n;
  }

  bool cut = total > cols;
  size_t budget = cut ? cols - ELLIPSIS_W : cols;

  /* Fill up to `budget` displaying columns, tracking byte end. */
  size_t col = 0;
  size_t end = 0;
  i = 0;
  while (s[i] != '\0') {
    size_t n;
    unsigned long cp = utf8_decode(s + i, &n);
    int w = char_disp_width(cp);
    if (col + (size_t)w > budget) break;
    col += (size_t)w;
    i += n;
    end = i;
  }
  cut = cut || (col < total);

  if (outcap == 0) return col;
  if (end + sizeof(ELLIPSIS) > outcap - 1) {
    while (end + sizeof(ELLIPSIS) > outcap - 1 && end > 0) {
      /* Back up one code point. */
      size_t k = 0;
      while (k + 1 < end && ((unsigned char)s[end - k - 1] & 0xC0) == 0x80)
        k++;
      end -= k + 1;
    }
  }
  memcpy(out, s, end);
  if (cut) {
    memcpy(out + end, ELLIPSIS, sizeof(ELLIPSIS) - 1);
    end += sizeof(ELLIPSIS) - 1;
  }
  out[end] = '\0';
  return col;
}

size_t cache_truncate(const char *s, size_t cols, char *out, size_t cap) {
  return truncate_to_cols(s, cols, out, cap);
}

/* Wrap `text` to at most `cols` display columns without splitting words. */
size_t cache_wrap(const char *text, size_t cols, char *out, size_t cap) {
  if (text == nullptr || out == nullptr || cap == 0 || cols == 0) return 0;

  size_t o = 0;  /* output cursor */
  size_t wl = 0; /* display width of the line so far */
  size_t lines = 1;
  bool any = false;

  const char *ws = nullptr; /* start of pending word, or null */
  size_t ww = 0;            /* display width of pending word */

  size_t i = 0;
  while (text[i] != '\0') {
    size_t n;
    unsigned long cp = utf8_decode(text + i, &n);
    if (cp == ' ' || cp == '\t' || cp == '\n') {
      if (ws != nullptr) { /* commit the pending word */
        size_t wb = (size_t)(text + i - ws);
        if (wl > 0) {
          if (wl + 1 + ww > cols) {
            if (o < cap) out[o++] = '\n';
            wl = 0;
            lines++;
          } else {
            if (o < cap) out[o++] = ' ';
            wl++;
          }
        }
        for (size_t k = 0; k < wb && o < cap; k++) out[o++] = ws[k];
        wl += ww;
        any = true;
        ws = nullptr;
        ww = 0;
      }
      if (cp == '\n') {
        if (o < cap) out[o++] = '\n';
        wl = 0;
        lines++;
      }
      i += n;
      continue;
    }
    if (ws == nullptr) ws = text + i;
    ww += (size_t)char_disp_width(cp);
    i += n;
  }

  /* Trailing word. */
  if (ws != nullptr) {
    size_t wb = (size_t)(text + i - ws);
    if (wl > 0) {
      if (wl + 1 + ww > cols) {
        if (o < cap) out[o++] = '\n';
        wl = 0;
        lines++;
      } else {
        if (o < cap) out[o++] = ' ';
        wl++;
      }
    }
    for (size_t k = 0; k < wb && o < cap; k++) out[o++] = ws[k];
    any = true;
  }

  if (!any) return 0;
  out[o] = '\0';
  return lines;
}
/* Parse a Mastodon ISO-8601 timestamp ("2026-08-19T09:30:00.000Z") as UTC and
 * return it as a Unix epoch. On parse failure, returns 0. */
static long parse_iso8601(const char *s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
    return 0;
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  return (long)timegm(&tm);
}

/* Format `epoch` relative to now into `out` as e.g. "now", "5m", "3h", "2d",
 * "4w". */
static void relative_time(long epoch, char *out, size_t cap) {
  long now = (long)time(nullptr);
  long diff = now - epoch;
  if (diff < 0) diff = 0;
  if (diff < 60) {
    snprintf(out, cap, "now");
  } else if (diff < 3600) {
    snprintf(out, cap, "%ldm", diff / 60);
  } else if (diff < 86400) {
    snprintf(out, cap, "%ldh", diff / 3600);
  } else if (diff < 604800) {
    snprintf(out, cap, "%ldd", diff / 86400);
  } else {
    snprintf(out, cap, "%ldw", diff / 604800);
  }
}

/* Public helper: format an ISO-8601 status timestamp as a relative time. */
void cache_relative_time(const char *iso, char *out, size_t cap) {
  relative_time(parse_iso8601(iso != nullptr ? iso : ""), out, cap);
}

/* Print a single row, truncating to terminal width unless `wrap`. */
static void emit_row(const char *row, bool wrap) {
  if (wrap) {
    puts(row);
    return;
  }
  size_t cols = terminal_cols();
  char out[5120];
  truncate_to_cols(row, cols, out, sizeof(out));
  puts(out);
}

static void print_row(const char *id, const char *created, const char *content,
                      bool wrap) {
  char plain[4096];
  plain_text(content != nullptr ? content : "", plain, sizeof(plain));

  char rel[16];
  relative_time(parse_iso8601(created != nullptr ? created : ""), rel,
                sizeof(rel));

  char row[4600];
  snprintf(row, sizeof(row), "%s  %s  %s",
           id != nullptr ? id : "", rel, plain);
  emit_row(row, wrap);
}

/* Look up a stored status's relative time and plain text by id, filling both
 * out buffers. Returns false if not present. */
static bool fetch_row(const cache *c, const char *id, char *rel_out,
                      char *text_out, size_t relcap, size_t textcap) {
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT created_at, content FROM statuses WHERE id=?1;";
  if (sqlite3_prepare_v2(c->db, q, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  bool ok = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char *created = (const char *)sqlite3_column_text(st, 0);
    const char *content = (const char *)sqlite3_column_text(st, 1);
    relative_time(parse_iso8601(created != nullptr ? created : ""), rel_out,
                  relcap);
    plain_text(content != nullptr ? content : "", text_out, textcap);
    ok = true;
  }
  sqlite3_finalize(st);
  return ok;
}

/* A boost wrapper: show the booster on the first line and the original post on
 * an indented line below. */
static void print_boost(const cache *c, const char *id, const char *created,
                        const char *account, const char *reblog_of,
                        bool wrap) {
  char rel[16];
  relative_time(parse_iso8601(created != nullptr ? created : ""), rel,
                sizeof(rel));

  char row[4600];
  snprintf(row, sizeof(row), "%s  %s  \xF0\x9F\x9A\x80 @%s",
           id != nullptr ? id : "", rel, account != nullptr ? account : "");
  emit_row(row, wrap);

  char relb[16], text[4096];
  if (reblog_of != nullptr && fetch_row(c, reblog_of, relb, text,
                                        sizeof(relb), sizeof(text))) {
    char sub[4700];
    snprintf(sub, sizeof(sub), "  \xE2\x8E\xA3  [%s] %s  %s", reblog_of, relb,
             text);
    emit_row(sub, wrap);
  }
}

/* Print a timeline from a cJSON array of status objects, newest-first. Boost
 * wrappers render with their inner post on an indented line. */
void cache_list_items(const cache *c, const cJSON *arr, bool wrap) {
  for (const cJSON *it = first_array_item(arr); it != nullptr;
       it = next_array_item(it)) {
    const char *id = status_field(it, "id");
    const char *created = status_field(it, "created_at");
    const char *content = status_field(it, "content");
    const cJSON *reblog = cJSON_GetObjectItemCaseSensitive(it, "reblog");
    if (reblog != nullptr && cJSON_IsObject(reblog)) {
      /* Wrapper is a boost: show booster + inner post. */
      const char *inner_id = status_field(reblog, "id");
      print_boost(c, id, created, haccount(it), inner_id, wrap);
    } else {
      print_row(id, created, content, wrap);
    }
  }
}

/* One cached status, with its stored fields copied out of the database into
 * fixed-size buffers (sqlite column pointers only live until finalize). */
typedef struct {
  char created[64];
  char account[256];
  char reblog_of[128];
  char text[8192]; /* content, already converted to plain text */
} cached_status;

/* Copy the stored row for `id` into `row`. Returns false if not present. */
static bool cached_row(const cache *c, const char *id, cached_status *row) {
  memset(row, 0, sizeof(*row));
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT created_at, account, content, reblog_of"
                  " FROM statuses WHERE id=?1;";
  if (sqlite3_prepare_v2(c->db, q, -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  bool found = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    snprintf(row->created, sizeof(row->created), "%s",
             (const char *)sqlite3_column_text(st, 0) != nullptr
                 ? (const char *)sqlite3_column_text(st, 0)
                 : "");
    snprintf(row->account, sizeof(row->account), "%s",
             (const char *)sqlite3_column_text(st, 1) != nullptr
                 ? (const char *)sqlite3_column_text(st, 1)
                 : "");
    const char *content = (const char *)sqlite3_column_text(st, 2);
    snprintf(row->reblog_of, sizeof(row->reblog_of), "%s",
             (const char *)sqlite3_column_text(st, 3) != nullptr
                 ? (const char *)sqlite3_column_text(st, 3)
                 : "");
    plain_text(content != nullptr ? content : "", row->text, sizeof(row->text));
    found = true;
  }
  sqlite3_finalize(st);
  return found;
}

/* Detailed single-status view from the cache (mobile mode): the boost line
 * first for boost wrappers, then handle, full text, posted time and id. The
 * online-only fields (URL, visibility, client, counts, attachments) are not
 * stored, so a cached view omits them. */
bool cache_view(const cache *c, const char *id) {
  if (c == nullptr || c->db == nullptr || id == nullptr) return false;
  cached_status row;
  if (!cached_row(c, id, &row)) return false;

  char rel[16] = "";
  relative_time(parse_iso8601(row.created), rel, sizeof(rel));

  if (row.reblog_of[0] != '\0') {
    /* Boost wrapper: booster line, then the underlying post. */
    printf("\xF0\x9F\x9A\x80 @%s boosted\n", row.account);
    if (row.created[0] != '\0')
      printf("Boosted: %s (%s)\n", row.created, rel);
    printf("\n");
    cached_status inner;
    if (!cached_row(c, row.reblog_of, &inner)) {
      printf("(the boosted post isn't in the cache)\n");
      return true;
    }
    char irel[16] = "";
    relative_time(parse_iso8601(inner.created), irel, sizeof(irel));
    printf("@%s\n", inner.account);
    printf("%s\n", inner.text);
    printf("\n");
    if (inner.created[0] != '\0')
      printf("Posted: %s (%s)\n", inner.created, irel);
    printf("ID:     %s\n", row.reblog_of);
    return true;
  }

  printf("@%s\n", row.account);
  printf("%s\n", row.text);
  printf("\n");
  if (row.created[0] != '\0') printf("Posted: %s (%s)\n", row.created, rel);
  printf("ID:     %s\n", id);
  return true;
}

void cache_list(const cache *c, const char *type, bool wrap) {
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT s.id, s.created_at, s.account, s.reblog_of, s.content"
                  " FROM timeline_statuses t JOIN statuses s ON t.id = s.id"
                  " WHERE t.type = ?1 ORDER BY s.created_at DESC;";
  if (sqlite3_prepare_v2(c->db, q, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *id = (const char *)sqlite3_column_text(st, 0);
    const char *created = (const char *)sqlite3_column_text(st, 1);
    const char *account = (const char *)sqlite3_column_text(st, 2);
    const char *reblog_of = (const char *)sqlite3_column_text(st, 3);
    const char *content = (const char *)sqlite3_column_text(st, 4);
    if (reblog_of != nullptr && reblog_of[0] != '\0') {
      print_boost(c, id, created, account, reblog_of, wrap);
    } else {
      print_row(id, created, content, wrap);
    }
  }
  sqlite3_finalize(st);
}

/* ---- cached-listing reconstruction (for `ls --mobile` graphics) --------- */

static cJSON *account_to_json(const char *acct, const char *avatar) {
  cJSON *a = cJSON_CreateObject();
  if (a == nullptr) return nullptr;
  cJSON_AddStringToObject(a, "acct", acct != nullptr ? acct : "");
  cJSON_AddStringToObject(a, "avatar", avatar != nullptr ? avatar : "");
  return a;
}

/* Rebuild a status's media_attachments array from the status_media table. */
static cJSON *media_to_json(const cache *c, const char *id) {
  cJSON *arr = cJSON_CreateArray();
  if (arr == nullptr) return nullptr;
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT mime, url, preview, description, type"
                  " FROM status_media WHERE status_id=?1 ORDER BY pos;";
  if (sqlite3_prepare_v2(c->db, q, -1, &st, nullptr) != SQLITE_OK) return arr;
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *mime = (const char *)sqlite3_column_text(st, 0);
    const char *url = (const char *)sqlite3_column_text(st, 1);
    const char *preview = (const char *)sqlite3_column_text(st, 2);
    const char *desc = (const char *)sqlite3_column_text(st, 3);
    const char *type = (const char *)sqlite3_column_text(st, 4);
    cJSON *att = cJSON_CreateObject();
    if (att == nullptr) break;
    cJSON_AddStringToObject(att, "mime_type", mime != nullptr ? mime : "");
    cJSON_AddStringToObject(att, "url", url != nullptr ? url : "");
    cJSON_AddStringToObject(att, "preview_url",
                            preview != nullptr ? preview : "");
    cJSON_AddStringToObject(att, "description", desc != nullptr ? desc : "");
    cJSON_AddStringToObject(att, "type", type != nullptr ? type : "");
    cJSON_AddItemToArray(arr, att);
  }
  sqlite3_finalize(st);
  return arr;
}

/* A minimal status object with the fields the ls renderer reads: id,
 * created_at, content, account (acct + avatar) and, for a boost, `reblog`
 * (the inner post, itself carrying its media). */
static cJSON *status_base(const char *id, const char *created,
                          const char *content, const char *acct,
                          const char *avatar) {
  cJSON *o = cJSON_CreateObject();
  if (o == nullptr) return nullptr;
  cJSON_AddStringToObject(o, "id", id != nullptr ? id : "");
  cJSON_AddStringToObject(o, "created_at", created != nullptr ? created : "");
  cJSON_AddStringToObject(o, "content", content != nullptr ? content : "");
  cJSON *account = account_to_json(acct, avatar);
  if (account == nullptr) {
    cJSON_Delete(o);
    return nullptr;
  }
  cJSON_AddItemToObject(o, "account", account);
  return o;
}

/* The inner (boosted) post for a boost wrapper id. */
static cJSON *inner_to_json(const cache *c, const char *id) {
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT created_at, content, account, avatar"
                  " FROM statuses WHERE id=?1;";
  if (sqlite3_prepare_v2(c->db, q, -1, &st, nullptr) != SQLITE_OK) return nullptr;
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  cJSON *o = nullptr;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char *created = (const char *)sqlite3_column_text(st, 0);
    const char *content = (const char *)sqlite3_column_text(st, 1);
    const char *acct = (const char *)sqlite3_column_text(st, 2);
    const char *avatar = (const char *)sqlite3_column_text(st, 3);
    o = status_base(id, created, content, acct, avatar);
    if (o != nullptr) {
      cJSON *media = media_to_json(c, id);
      if (media != nullptr) cJSON_AddItemToObject(o, "media_attachments", media);
    }
  }
  sqlite3_finalize(st);
  return o;
}

/* Reconstruct the cached timeline of `type` as a cJSON array of status
 * objects (same shape `ls` renders from), so `ls --mobile` can draw the
 * avatars and images from the local media cache. Caller frees with
 * json_free(). Returns nullptr on no rows or OOM. */
cJSON *cache_list_json(const cache *c, const char *type) {
  if (c == nullptr || c->db == nullptr || type == nullptr) return nullptr;
  cJSON *arr = cJSON_CreateArray();
  if (arr == nullptr) return nullptr;
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT s.id, s.created_at, s.content, s.reblog_of,"
                  " s.account, s.avatar"
                  " FROM timeline_statuses t JOIN statuses s ON t.id = s.id"
                  " WHERE t.type=?1 ORDER BY s.created_at DESC;";
  if (sqlite3_prepare_v2(c->db, q, -1, &st, nullptr) != SQLITE_OK) return arr;
  sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *id = (const char *)sqlite3_column_text(st, 0);
    const char *created = (const char *)sqlite3_column_text(st, 1);
    const char *content = (const char *)sqlite3_column_text(st, 2);
    const char *reblog_of = (const char *)sqlite3_column_text(st, 3);
    const char *acct = (const char *)sqlite3_column_text(st, 4);
    const char *avatar = (const char *)sqlite3_column_text(st, 5);
    cJSON *o = status_base(id, created, content, acct, avatar);
    if (o == nullptr) break;
    if (reblog_of != nullptr && reblog_of[0] != '\0') {
      cJSON *inner = inner_to_json(c, reblog_of);
      if (inner != nullptr) cJSON_AddItemToObject(o, "reblog", inner);
    } else {
      cJSON *media = media_to_json(c, id);
      if (media != nullptr) cJSON_AddItemToObject(o, "media_attachments", media);
    }
    cJSON_AddItemToArray(arr, o);
  }
  sqlite3_finalize(st);
  return arr;
}

/* Run a statement with a single text bound parameter and no result rows. */
static void exec_bind(sqlite3 *db, const char *sql, const char *v) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_text(st, 1, v != nullptr ? v : "", -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

void cache_delete_status(cache *c, const char *id) {
  exec_bind(c->db,
            "DELETE FROM timeline_statuses WHERE id=?1;", id);
  exec_bind(c->db, "DELETE FROM statuses WHERE id=?1;", id);
  /* If the deleted post was the thread anchor, drop it too. */
  char *last = cache_last_post_id(c);
  if (last != nullptr && id != nullptr && strcmp(last, id) == 0) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(c->db, "DELETE FROM meta WHERE k='last_post_id';",
                           -1, &st, nullptr) == SQLITE_OK)
      sqlite3_step(st);
    if (st != nullptr) sqlite3_finalize(st);
  }
  free(last);
}

static void meta_set(cache *c, const char *k, const char *v) {
  sqlite3_stmt *st = nullptr;
  const char *q = "REPLACE INTO meta(k,v) VALUES(?1,?2);";
  if (sqlite3_prepare_v2(c->db, q, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, v, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

static char *meta_get(const cache *c, const char *k) {
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT v FROM meta WHERE k=?1;";
  if (sqlite3_prepare_v2(c->db, q, -1, &st, nullptr) != SQLITE_OK) return nullptr;
  sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
  char *out = nullptr;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(st, 0);
    if (v != nullptr) out = strdup(v);
  }
  sqlite3_finalize(st);
  return out;
}

void cache_set_last_type(cache *c, const char *type) {
  meta_set(c, "last_type", type);
}

void cache_set_last_post_id(cache *c, const char *id) {
  meta_set(c, "last_post_id", id);
}

char *cache_last_post_id(const cache *c) { return meta_get(c, "last_post_id"); }

/* The timeline type most recently loaded, or nullptr if none. Fresh allocation. */
[[nodiscard]] char *cache_last_type(const cache *c) {
  return meta_get(c, "last_type");
}