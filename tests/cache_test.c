#include "cache.h"
#include "json_helpers.h"

#include <cjson/cJSON.h>
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The cache honours XDG_CONFIG_HOME; point it at a scratch dir so the test
 * never touches real credentials. */
static int run(void) {
  char dir[] = "/tmp/sloptoot-cache-test-XXXXXX";
  if (mkdtemp(dir) == nullptr) return 1;
  if (setenv("XDG_CONFIG_HOME", dir, 1) != 0) return 1;

  cache c;
  if (!cache_open(&c)) return 1;

  char *last = cache_last_post_id(&c);
  if (last != nullptr) return 1; /* expect nothing initially */

  cJSON *arr = cJSON_CreateArray();

  cJSON *s1 = cJSON_CreateObject();
  cJSON_AddStringToObject(s1, "id", "1");
  cJSON_AddStringToObject(s1, "created_at", "2024-01-01T00:00:00.000Z");
  cJSON_AddStringToObject(s1, "content", "<p>hello &amp; goodbye</p>");
  cJSON *acct1 = cJSON_CreateObject();
  cJSON_AddStringToObject(acct1, "acct", "alice");
  cJSON_AddItemToObject(s1, "account", acct1);
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

  cache_close(&c2);

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  (void)system(cmd);
  return (found && boost_ok && inner_ok) ? 0 : 1;
}

/* Delete removes a cached status and clears the chain anchor when it matches. */
static int run_delete(void) {
  char dir[] = "/tmp/sloptoot-cache-del-XXXXXX";
  if (mkdtemp(dir) == nullptr) return 1;
  if (setenv("XDG_CONFIG_HOME", dir, 1) != 0) return 1;

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

int main(void) { return run() || run_delete() || run_wrap(); }