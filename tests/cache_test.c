#include "cache.h"
#include "json_helpers.h"

#include <cjson/cJSON.h>
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The cache honours XDG_CONFIG_HOME and XDG_CACHE_HOME; point both at a
 * scratch dir so the test never touches real credentials or the real cache. */
static int run(void) {
  char dir[] = "/tmp/sloptoot-cache-test-XXXXXX";
  if (mkdtemp(dir) == nullptr) return 1;
  if (setenv("XDG_CONFIG_HOME", dir, 1) != 0) return 1;
  if (setenv("XDG_CACHE_HOME", dir, 1) != 0) return 1;

  cache c;
  if (!cache_open(&c)) return 1;

  char *last = cache_last_post_id(&c);
  if (last != nullptr) return 1; /* expect nothing initially */

  cJSON *arr = cJSON_CreateArray();

  /* Status 1 carries an avatar URL and one image attachment, so the cached
   * listing can render it offline. */
  cJSON *s1 = cJSON_CreateObject();
  cJSON_AddStringToObject(s1, "id", "1");
  cJSON_AddStringToObject(s1, "created_at", "2024-01-01T00:00:00.000Z");
  cJSON_AddStringToObject(s1, "content", "<p>hello &amp; goodbye</p>");
  cJSON *acct1 = cJSON_CreateObject();
  cJSON_AddStringToObject(acct1, "acct", "alice");
  cJSON_AddStringToObject(acct1, "avatar_static", "https://cdn/a.png");
  cJSON_AddItemToObject(s1, "account", acct1);
  cJSON *media1 = cJSON_CreateArray();
  cJSON *att1 = cJSON_CreateObject();
  cJSON_AddStringToObject(att1, "type", "image");
  cJSON_AddStringToObject(att1, "mime_type", "image/png");
  cJSON_AddStringToObject(att1, "url", "https://cdn/1.png");
  cJSON_AddStringToObject(att1, "preview_url", "https://cdn/1_small.png");
  cJSON_AddStringToObject(att1, "description", "a picture");
  cJSON_AddItemToArray(media1, att1);
  cJSON_AddItemToObject(s1, "media_attachments", media1);
  cJSON_AddItemToArray(arr, s1);

  cJSON *s2 = cJSON_CreateObject();
  cJSON_AddStringToObject(s2, "id", "2");
  cJSON_AddStringToObject(s2, "created_at", "2024-01-02T00:00:00.000Z");
  cJSON_AddStringToObject(s2, "content", "<p>second</p>");
  cJSON *acct2 = cJSON_CreateObject();
  cJSON_AddStringToObject(acct2, "acct", "bob");
  cJSON_AddItemToObject(s2, "account", acct2);
  cJSON_AddItemToArray(arr, s2);

  cache_store_statuses(&c, arr, "profile");
  json_free(arr);

  cache_set_last_post_id(&c, "42");

  /* A boost wrapper stores the inner post and marks the wrapper. */
  cJSON *boost_arr = cJSON_CreateArray();
  cJSON *inner = cJSON_CreateObject();
  cJSON_AddStringToObject(inner, "id", "10");
  cJSON_AddStringToObject(inner, "created_at", "2024-01-03T00:00:00.000Z");
  cJSON_AddStringToObject(inner, "content", "<p>original</p>");
  cJSON *inner_acct = cJSON_CreateObject();
  cJSON_AddStringToObject(inner_acct, "acct", "carol");
  cJSON_AddItemToObject(inner, "account", inner_acct);
  cJSON *wrap = cJSON_CreateObject();
  cJSON_AddStringToObject(wrap, "id", "50");
  cJSON_AddStringToObject(wrap, "created_at", "2024-01-04T00:00:00.000Z");
  cJSON_AddStringToObject(wrap, "content", "");
  cJSON *boost_acct = cJSON_CreateObject();
  cJSON_AddStringToObject(boost_acct, "acct", "bob");
  cJSON_AddItemToObject(wrap, "account", boost_acct);
  cJSON_AddItemToObject(wrap, "reblog", inner);
  cJSON_AddItemToArray(boost_arr, wrap);
  cache_store_statuses(&c, boost_arr, "profile");
  json_free(boost_arr);

  cache_close(&c);

  cache c2;
  if (!cache_open(&c2)) return 1;
  last = cache_last_post_id(&c2);
  if (last == nullptr || strcmp(last, "42") != 0) return 1;
  free(last);

  /* Verify a stored row round-trips to the database. */
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT account, content FROM statuses WHERE id='2';";
  int found = 0;
  if (sqlite3_prepare_v2(c2.db, q, -1, &st, nullptr) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW) {
    const char *account = (const char *)sqlite3_column_text(st, 0);
    const char *content = (const char *)sqlite3_column_text(st, 1);
    if (account != nullptr && strcmp(account, "bob") == 0 && content != nullptr)
      found = 1;
  }
  if (st != nullptr) sqlite3_finalize(st);

  /* The wrapper (id 50) is a timeline boost of the inner post (id 10). */
  int boost_ok = 0;
  st = nullptr;
  const char *qb =
      "SELECT account FROM statuses WHERE id='50' AND reblog_of='10';";
  if (sqlite3_prepare_v2(c2.db, qb, -1, &st, nullptr) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW) {
    const char *account = (const char *)sqlite3_column_text(st, 0);
    if (account != nullptr && strcmp(account, "bob") == 0) boost_ok = 1;
  }
  if (st != nullptr) sqlite3_finalize(st);

  int inner_ok = 0;
  st = nullptr;
  /* Inner boosted post present but NOT a timeline member (no duplicate). */
  const char *qi = "SELECT COUNT(*) FROM timeline_statuses WHERE id='10';";
  if (sqlite3_prepare_v2(c2.db, qi, -1, &st, nullptr) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW)
    inner_ok = (sqlite3_column_int(st, 0) == 0);
  if (st != nullptr) sqlite3_finalize(st);

  /* The avatar URL and media attachments round-trip into the schema. */
  int media_ok = 0;
  st = nullptr;
  const char *qm =
      "SELECT s.avatar, m.url, m.preview, m.description"
      " FROM statuses s JOIN status_media m ON s.id = m.status_id"
      " WHERE s.id='1' AND m.pos=0;";
  if (sqlite3_prepare_v2(c2.db, qm, -1, &st, nullptr) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW) {
    const char *avatar = (const char *)sqlite3_column_text(st, 0);
    const char *url = (const char *)sqlite3_column_text(st, 1);
    const char *preview = (const char *)sqlite3_column_text(st, 2);
    const char *desc = (const char *)sqlite3_column_text(st, 3);
    if (avatar != nullptr && strcmp(avatar, "https://cdn/a.png") == 0 &&
        url != nullptr && strcmp(url, "https://cdn/1.png") == 0 &&
        preview != nullptr && strcmp(preview, "https://cdn/1_small.png") == 0 &&
        desc != nullptr && strcmp(desc, "a picture") == 0)
      media_ok = 1;
  }
  if (st != nullptr) sqlite3_finalize(st);

  /* cache_list_json rebuilds the renderable timeline (avatar + media URLs). */
  int json_ok = 0;
  cJSON *rebuilt = cache_list_json(&c2, "profile");
  if (rebuilt != nullptr && cJSON_IsArray(rebuilt)) {
    const cJSON *first = rebuilt->child;
    while (first != nullptr &&
           strcmp(json_get_string(first, "id"), "1") != 0)
      first = first->next;
    if (first != nullptr) {
      const cJSON *account = cJSON_GetObjectItemCaseSensitive(first, "account");
      const char *av = account != nullptr ? json_get_string(account, "avatar")
                                          : nullptr;
      const cJSON *media =
          cJSON_GetObjectItemCaseSensitive(first, "media_attachments");
      const cJSON *att = (media != nullptr && cJSON_IsArray(media))
                             ? media->child
                             : nullptr;
      const char *u = att != nullptr ? json_get_string(att, "url") : nullptr;
      if (av != nullptr && strcmp(av, "https://cdn/a.png") == 0 && u != nullptr &&
          strcmp(u, "https://cdn/1.png") == 0)
        json_ok = 1;
    }
    json_free(rebuilt);
  }

  cache_close(&c2);

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  (void)system(cmd);
  return (found && boost_ok && inner_ok && media_ok && json_ok) ? 0 : 1;
}

/* Delete removes a cached status and clears the chain anchor when it matches. */
static int run_delete(void) {
  char dir[] = "/tmp/sloptoot-cache-del-XXXXXX";
  if (mkdtemp(dir) == nullptr) return 1;
  if (setenv("XDG_CONFIG_HOME", dir, 1) != 0) return 1;
  if (setenv("XDG_CACHE_HOME", dir, 1) != 0) return 1;

  cache c;
  if (!cache_open(&c)) return 1;

  cJSON *arr = cJSON_CreateArray();
  cJSON *s = cJSON_CreateObject();
  cJSON_AddStringToObject(s, "id", "99");
  cJSON_AddStringToObject(s, "created_at", "2024-03-01T00:00:00.000Z");
  cJSON_AddStringToObject(s, "content", "<p>obsolete</p>");
  cJSON *acct = cJSON_CreateObject();
  cJSON_AddStringToObject(acct, "acct", "alice");
  cJSON_AddItemToObject(s, "account", acct);
  cJSON_AddItemToArray(arr, s);
  cache_store_statuses(&c, arr, "profile");
  json_free(arr);

  cache_set_last_post_id(&c, "99");
  cache_delete_status(&c, "99");

  /* Status gone, anchor cleared. */
  sqlite3_stmt *st = nullptr;
  const char *q = "SELECT COUNT(*) FROM statuses WHERE id='99';";
  int n = -1;
  if (sqlite3_prepare_v2(c.db, q, -1, &st, nullptr) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW)
    n = sqlite3_column_int(st, 0);
  if (st != nullptr) sqlite3_finalize(st);

  char *last = cache_last_post_id(&c);
  int ok = (n == 0) && (last == nullptr);
  free(last);
  cache_close(&c);

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  (void)system(cmd);
  return ok ? 0 : 1;
}

/* Text-wrap / truncate helpers used for the hybrid image layout. */
static int run_wrap(void) {
  char out[1024];

  /* Basic word wrap at a narrow width. */
  size_t n = cache_wrap("aaa bbb ccc ddd", 8, out, sizeof(out));
  if (n != 2) {
    fprintf(stderr, "wrap: expected 2 lines, got %zu: [%s]\n", n, out);
    return 1;
  }
  if (strcmp(out, "aaa bbb\nccc ddd") != 0) {
    fprintf(stderr, "wrap: bad output [%s]\n", out);
    return 1;
  }
  /* A wider word triggers a break. */
  n = cache_wrap("aa bb cccccc", 7, out, sizeof(out));
  if (n != 2 || strcmp(out, "aa bb\ncccccc") != 0) {
    fprintf(stderr, "wrap: wide-word break [%s]\n", out);
    return 1;
  }

  /* Line breaks in the source are preserved. */
  n = cache_wrap("one\ntwo", 80, out, sizeof(out));
  if (n != 2 || strcmp(out, "one\ntwo") != 0) {
    fprintf(stderr, "wrap: newline handling [%s]\n", out);
    return 1;
  }

  /* A single long word is not split but still counts as one line. */
  n = cache_wrap("supercalifragilistic", 4, out, sizeof(out));
  if (n != 1) {
    fprintf(stderr, "wrap: long word [%s]\n", out);
    return 1;
  }

  /* Truncation never splits a multibyte char and appends an ellipsis. */
  cache_truncate("ab\xEF\x80\x80 def", 80, out, sizeof(out));
  if (strcmp(out, "ab\xEF\x80\x80 def") != 0) {
    fprintf(stderr, "truncate: multibyte corruption [%s]\n", out);
    return 1;
  }
  cache_truncate("hello world", 6, out, sizeof(out));
  if (strcmp(out, "hell\xE2\x80\xA6") != 0) {
    fprintf(stderr, "truncate: bad cut [%s]\n", out);
    return 1;
  }
  cache_truncate("short", 80, out, sizeof(out));
  if (strcmp(out, "short") != 0) {
    fprintf(stderr, "truncate: long lines must be untouched [%s]\n", out);
    return 1;
  }
  return 0;
}

/* A pre-1.4.3 cache.db sitting in the config dir is moved to the cache dir
 * first time the cache is opened, and its data survives. */
static int run_migrate(void) {
  char dir[] = "/tmp/sloptoot-cache-migrate-XXXXXX";
  if (mkdtemp(dir) == nullptr) return 1;
  char cfg[400], cch[400];
  snprintf(cfg, sizeof(cfg), "%s/config", dir);
  snprintf(cch, sizeof(cch), "%s/cache", dir);
  if (mkdir(cfg, 0700) != 0 || mkdir(cch, 0700) != 0) return 1;
  if (setenv("XDG_CONFIG_HOME", cfg, 1) != 0) return 1;
  if (setenv("XDG_CACHE_HOME", cch, 1) != 0) return 1;

  /* Simulate an older release: a cache.db stored next to the credentials. */
  char olddir[512];
  snprintf(olddir, sizeof(olddir), "%s/sloptoot", cfg);
  char legacy[512];
  snprintf(legacy, sizeof(legacy), "%s/cache.db", olddir);
  if (mkdir(olddir, 0700) != 0) return 1;
  sqlite3 *db = nullptr;
  if (sqlite3_open(legacy, &db) != SQLITE_OK || db == nullptr) return 1;
  if (sqlite3_exec(db,
                   "CREATE TABLE meta(k TEXT PRIMARY KEY, v TEXT);"
                   "INSERT INTO meta VALUES('last_post_id','123');",
                   nullptr, nullptr, nullptr) != SQLITE_OK)
    return 1;
  sqlite3_close(db);

  /* Opening the cache relocates it before use; the chain anchor survives. */
  cache c;
  if (!cache_open(&c)) return 1;
  char *last = cache_last_post_id(&c);
  if (last == nullptr || strcmp(last, "123") != 0) return 1;
  free(last);
  cache_close(&c);

  /* The legacy file is gone; the data now lives in the cache dir. */
  char newpath[512];
  snprintf(newpath, sizeof(newpath), "%s/sloptoot/cache.db", cch);
  int ok = access(legacy, F_OK) != 0 && access(newpath, F_OK) == 0;

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  (void)system(cmd);
  return ok ? 0 : 1;
}

int main(void) {
  return run() || run_delete() || run_migrate() || run_wrap();
}