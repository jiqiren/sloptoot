#include "timeline.h"

#include "cache.h"
#include "http.h"
#include "image.h"
#include "json_helpers.h"
#include "kitty.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PAGE_LIMIT 40

/* Fetch one page of a timeline. `url` must be the endpoint base without any
 * query string. When `max_id` is non-null the older-than page is requested.
 * Returns a parsed cJSON array (caller frees) or nullptr on error. */
static cJSON *fetch_page(const config *c, const char *base, const char *max_id) {
  /* Build endpoint + query. */
  size_t n = strlen(base) + 256;
  char *url = malloc(n);
  if (url == nullptr) return nullptr;
  if (max_id != nullptr) {
    snprintf(url, n, "%s?limit=%d&max_id=%s", base, PAGE_LIMIT, max_id);
  } else {
    snprintf(url, n, "%s?limit=%d", base, PAGE_LIMIT);
  }

  http_response resp = {0};
  bool ok = http_get(url, c->access_token, &resp);
  free(url);
  if (!ok) {
    fprintf(stderr, "error: network request failed\n");
    http_response_free(&resp);
    return nullptr;
  }
  if (resp.code < 200 || resp.code >= 300) {
    fprintf(stderr, "error: HTTP %ld\n", resp.code);
    if (resp.body != nullptr) fprintf(stderr, "%s\n", resp.body);
    http_response_free(&resp);
    return nullptr;
  }

  cJSON *root = json_parse(resp.body, resp.len);
  http_response_free(&resp);
  return root;
}

/* Concatenate the items of `page` onto `all`. Returns the id of the last item
 * in the page (a fresh allocation), or nullptr. */
static char *append_page(cJSON *all, const cJSON *page, const char **last_id) {
  if (page == nullptr || !cJSON_IsArray(page)) return nullptr;
  *last_id = nullptr;
  for (const cJSON *it = page->child; it != nullptr; it = it->next) {
    cJSON *copy = cJSON_Duplicate(it, true);
    if (copy != nullptr) cJSON_AddItemToArray(all, copy);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
    if (id != nullptr && cJSON_IsString(id)) *last_id = id->valuestring;
  }
  return (char *)*last_id;
}

/* Number of terminal lines one status occupies when printed: 1 for a normal
 * post, 2 for a boost wrapper (its inner post is the indented second line). */
static int status_lines(const cJSON *it) {
  const cJSON *reblog = cJSON_GetObjectItemCaseSensitive(it, "reblog");
  return (reblog != nullptr && cJSON_IsObject(reblog)) ? 2 : 1;
}

/* Total display lines of a cJSON array of statuses. */
static size_t array_lines(const cJSON *arr) {
  size_t n = 0;
  for (const cJSON *it = arr->child; it != nullptr; it = it->next)
    n += (size_t)status_lines(it);
  return n;
}

/* ---- kitty-graphics timeline rendering -------------------------------- */

/* Terminal columns, or 80 when not a TTY / unknown. */
static unsigned tl_cols(void) {
  struct winsize ws = {0};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return (unsigned)ws.ws_col;
  return 80;
}

/* Is this attachment one of the formats we can decode and draw? */
static bool attachment_is_image(const cJSON *att) {
  if (att == nullptr) return false;
  const char *mime = json_get_string(att, "mime_type");
  if (mime != nullptr) {
    return strcmp(mime, "image/png") == 0 || strcmp(mime, "image/jpeg") == 0 ||
           strcmp(mime, "image/jpg") == 0 || strcmp(mime, "image/gif") == 0 ||
           strcmp(mime, "image/webp") == 0;
  }
  /* No mime_type (older instances): sniff the URL's extension. */
  const char *url = json_get_string(att, "url");
  if (url == nullptr) return false;
  const char *ext = strrchr(url, '.');
  if (ext == nullptr) return false;
  return strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 ||
         strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".gif") == 0 ||
         strcasecmp(ext, ".webp") == 0;
}

/* Fetch and decode an image from a URL. Media and avatars are public, so no
 * auth is sent (they often live on a CDN other than the instance). */
static bool load_image_url(const char *url, img_image *out) {
  if (url == nullptr || url[0] == '\0') return false;
  http_response resp = {0};
  if (!http_get(url, nullptr, &resp)) {
    http_response_free(&resp);
    return false;
  }
  if (resp.code < 200 || resp.code >= 300 || resp.len == 0 ||
      resp.body == nullptr) {
    http_response_free(&resp);
    return false;
  }
  bool ok = img_decode((const uint8_t *)resp.body, resp.len, out) == 0;
  http_response_free(&resp);
  return ok;
}

/* The account that owns this timeline entry: for a boost the reblogger, else
 * the author. */
static const cJSON *entry_account(const cJSON *it) {
  const cJSON *account = cJSON_GetObjectItemCaseSensitive(it, "account");
  return (account != nullptr && cJSON_IsObject(account)) ? account : nullptr;
}

/* The non-animated avatar URL of `account`, or its animated one as a fallback. */
static const char *avatar_url(const cJSON *account) {
  if (account == nullptr) return nullptr;
  const char *av = json_get_string(account, "avatar_static");
  if (av == nullptr || av[0] == '\0') av = json_get_string(account, "avatar");
  return (av != nullptr && av[0] != '\0') ? av : nullptr;
}

/* One decoded image attachment waiting to be placed. */
typedef struct {
  img_image im;
  bool ok;
  unsigned cols; /* placement rectangle (cells, aspect-preserved) */
  unsigned rows;
} tl_att;

/* Wrap `prefix` + `text` so the whole line fits `width` display columns.
 * `out` receives the (possibly multi-line) result; returns its line count. */
static unsigned make_line(const char *prefix, const char *text, size_t width,
                          char *out, size_t cap) {
  size_t pl = strlen(prefix);
  size_t w = width > pl ? width - pl : 1;
  char tw[8192];
  cache_wrap(text != nullptr ? text : "", w, tw, sizeof(tw));
  snprintf(out, cap, "%s%s", prefix, tw);
  unsigned n = 1;
  for (const char *p = out; *p != '\0'; p++)
    if (*p == '\n') n++;
  return n;
}

/* Print a (possibly multi-line) string with `gutter` blank columns on the
 * left of every line. */
static void print_line_gutter(const char *s, size_t gutter) {
  size_t n = strlen(s);
  size_t line_start = 0;
  while (line_start <= n) {
    for (size_t g = 0; g < gutter; g++) putchar(' ');
    size_t end = n;
    for (size_t i = line_start; i < n; i++) {
      if (s[i] == '\n') {
        end = i;
        break;
      }
    }
    fwrite(s + line_start, 1, end - line_start, stdout);
    putchar('\n');
    if (end >= n) break;
    line_start = end + 1;
  }
}

/* Render a timeline with the kitty graphics protocol: each post gets its
 * author's avatar (aspect-preserved) beside the text and any image attachments
 * stacked below. Only called when `kgraphics_supported()`. */
static void ls_render_items(const cJSON *all, bool wrap) {
  (void)wrap; /* in graphics mode the text is always wrapped to its column. */
  unsigned tcols = tl_cols();
  if (tcols < 8) tcols = 8;

  int rows_target = cache_terminal_rows() - 2;
  if (rows_target < 1) rows_target = 1;
  unsigned used = 0;

  unsigned cw = 8, ch = 16;
  kgraphics_cell_size(&cw, &ch);

  /* The avatar is two terminal rows tall; leave a gutter at least as wide so
   * no text ever runs under it. */
  unsigned gutter = (2 * ch + cw - 1) / cw + 1;
  if (gutter < 3) gutter = 3;
  if (gutter > tcols - 1) gutter = tcols - 1;
  unsigned text_w = tcols - gutter;

  for (const cJSON *it = all->child; it != nullptr; it = it->next) {
    const cJSON *reblog = cJSON_GetObjectItemCaseSensitive(it, "reblog");
    bool is_boost = reblog != nullptr && cJSON_IsObject(reblog);
    const cJSON *detail = is_boost ? reblog : it;

    /* Text of the two possible lines (boost line + underlying post). */
    char prefix1[256], prefix2[256];
    char text1[8192], text2[8192];
    char rel[16];
    const char *created = json_get_string(it, "created_at");
    rel[0] = '\0';
    if (created != nullptr) cache_relative_time(created, rel, sizeof(rel));
    if (is_boost) {
      const cJSON *bacc = entry_account(it);
      const char *bhandle = bacc != nullptr ? json_get_string(bacc, "acct") : "";
      snprintf(prefix1, sizeof(prefix1), "%s  %s  \xF0\x9F\x9A\x80 @%s",
               json_get_string(it, "id"), rel, bhandle);
      text1[0] = '\0';
      const char *icreated = json_get_string(detail, "created_at");
      char irel[16] = "";
      if (icreated != nullptr)
        cache_relative_time(icreated, irel, sizeof(irel));
      snprintf(prefix2, sizeof(prefix2), "  \xE2\x8E\xA3  [%s] %s  ",
               json_get_string(detail, "id"), irel);
      const char *content = json_get_string(detail, "content");
      cache_plain_text(content != nullptr ? content : "", text2,
                       sizeof(text2));
    } else {
      snprintf(prefix1, sizeof(prefix1), "%s  %s  ",
               json_get_string(it, "id"), rel);
      const char *content = json_get_string(it, "content");
      cache_plain_text(content != nullptr ? content : "", text1, sizeof(text1));
      prefix2[0] = '\0';
      text2[0] = '\0';
    }

    char line1[10240], line2[10240];
    unsigned l1 = make_line(prefix1, text1, text_w, line1, sizeof(line1));
    unsigned l2 = is_boost ? make_line(prefix2, text2, text_w, line2,
                                       sizeof(line2))
                           : 0;
    unsigned text_rows = l1 + l2;

    /* Avatar (of the poster, or the reblogger for a boost). */
    img_image av = {0};
    const char *avurl = avatar_url(entry_account(it));
    bool av_ok = avurl != nullptr && load_image_url(avurl, &av);
    unsigned av_rows = av_ok ? 2u : 0u;

    /* Image attachments of the underlying post. */
    const cJSON *media =
        cJSON_GetObjectItemCaseSensitive(detail, "media_attachments");
    int nmedia =
        (media != nullptr && cJSON_IsArray(media)) ? cJSON_GetArraySize(media) : 0;
    int nimg = 0;
    for (int i = 0; i < nmedia; i++)
      if (attachment_is_image(cJSON_GetArrayItem(media, i))) nimg++;
    tl_att *atts = nimg > 0 ? calloc((size_t)nimg, sizeof(*atts)) : nullptr;
    size_t natt = 0;
    unsigned att_rows = 0;
    /* Cap attachment height so a wide feed image never monopolizes the
     * listing (the full image is one `view` away). */
    unsigned att_rmax = (unsigned)((rows_target * 2) / 5);
    if (att_rmax < 4) att_rmax = 4;
    for (int i = 0; i < nmedia && atts != nullptr; i++) {
      const cJSON *att = cJSON_GetArrayItem(media, i);
      if (!attachment_is_image(att)) continue;
      const char *u = att != nullptr ? json_get_string(att, "url") : nullptr;
      if (u != nullptr && load_image_url(u, &atts[natt].im)) {
        img_fit_aspect(&atts[natt].im, tcols - 1, att_rmax, cw, ch,
                       &atts[natt].cols, &atts[natt].rows);
        atts[natt].ok = true;
        if (atts[natt].cols < 1) atts[natt].cols = 1;
        if (atts[natt].rows < 1) atts[natt].rows = 1;
        att_rows += atts[natt].rows;
      }
      natt++;
    }

    /* A status consumes at most its text rows (avatar shares them) plus the
     * attachment rows plus one separator line. Skipping once the screen is
     * full keeps the shell prompt below the listing. */
    unsigned block =
        (text_rows > av_rows ? text_rows : av_rows) + att_rows + 1;
    /* Always show the newest post; stop once the screen is full so the shell
     * prompt stays below the listing. */
    if (used > 0 && used + block > (unsigned)rows_target) {
      img_free(&av);
      for (size_t e = 0; e < natt; e++) img_free(&atts[e].im);
      free(atts);
      break;
    }

    /* Avatar, placed with the cursor held so the text prints beside it. */
    if (av_ok) {
      unsigned ac = 0, ar = 0;
      img_fit_aspect(&av, gutter > 0 ? gutter - 1 : 1, av_rows, cw, ch, &ac,
                     &ar);
      if (ac < 1) ac = 1;
      kgraphics_render(&av, ac, ar, true);
    }
    print_line_gutter(line1, gutter);
    if (is_boost) print_line_gutter(line2, gutter);
    /* Clear the rows under a wide avatar before stacking attachments. */
    if (av_rows > text_rows) {
      for (unsigned k = text_rows; k < av_rows; k++) putchar('\n');
    }
    for (size_t e = 0; e < natt; e++) {
      if (!atts[e].ok) continue;
      kgraphics_render(&atts[e].im, atts[e].cols, atts[e].rows, false);
    }
    putchar('\n'); /* blank line between posts */

    used += block;
    img_free(&av);
    for (size_t e = 0; e < natt; e++) img_free(&atts[e].im);
    free(atts);
  }
  fflush(stdout);
}

int ls_statuses(const config *c, cache *db, const char *type, bool mobile,
                bool wrap) {
  if (c == nullptr || c->instance == nullptr || c->access_token == nullptr) {
    fprintf(stderr,
            "error: not logged in. Run `sloptoot login <instance>`.\n");
    return 2;
  }

  bool is_tag = type != nullptr && type[0] == '#';
  bool is_home = type != nullptr && strcmp(type, "home") == 0;
  const char *cache_type = is_tag ? type + 1
                           : (is_home ? "home"
                                      : "profile");

  /* Mobile: serve from the cache only, no network. */
  if (mobile) {
    cache_list(db, cache_type, wrap);
    return 0;
  }

  /* Build the endpoint base URL. */
  size_t n = strlen(c->instance) + 256;
  char *base = malloc(n);
  if (base == nullptr) return 3;
  if (is_home) {
    snprintf(base, n, "%s/api/v1/timelines/home", c->instance);
  } else if (is_tag) {
    char *enc = urlencode(type + 1);
    if (enc == nullptr) {
      free(base);
      return 3;
    }
    snprintf(base, n, "%s/api/v1/timelines/tag/%s", c->instance, enc);
    free(enc);
  } else {
    if (c->account_id == nullptr) {
      fprintf(stderr, "error: account id missing from config\n");
      free(base);
      return 2;
    }
    snprintf(base, n, "%s/api/v1/accounts/%s/statuses", c->instance,
             c->account_id);
  }

  /* How many display lines to aim for: the whole screen minus two rows (so the
   * shell prompt after `ls` isn't pushed off / doesn't lose a line). Boosts
   * render as two lines, so the budget is counted in lines, not statuses. */
  int target_rows = cache_terminal_rows();
  size_t target_lines = (size_t)((target_rows - 2) > 0 ? (target_rows - 2) : 1);

  cJSON *all = cJSON_CreateArray();
  if (all == nullptr) {
    free(base);
    return 3;
  }

  char *max_id = nullptr;
  int guard = 0;
  while (array_lines(all) < target_lines) {
    cJSON *page = fetch_page(c, base, max_id);
    if (page == nullptr) {
      json_free(all);
      free(max_id);
      free(base);
      return 3;
    }
    if (!cJSON_IsArray(page) || cJSON_GetArraySize(page) == 0) {
      json_free(page);
      break;
    }
    const char *last_id = nullptr;
    append_page(all, page, &last_id);
    /* Copy the id now; `page` is freed below and last_id points into it. */
    char *next_id = last_id != nullptr ? strdup(last_id) : nullptr;
    json_free(page);
    if (next_id == nullptr) {
      /* No id to page on (or OOM); can't fetch more. */
      break;
    }
    free(max_id);
    max_id = next_id;
    if (++guard > 100) break; /* sanity: never loop forever */
  }

  free(max_id);
  free(base);

  if (cJSON_GetArraySize(all) == 0) {
    json_free(all);
    fprintf(stderr, "no posts found\n");
    return 0;
  }

  /* We fetched at least a screenful; drop the oldest posts (whole items, each
   * one or two lines) until the display fits the terminal. */
  while (array_lines(all) > target_lines) {
    cJSON *drop = cJSON_DetachItemFromArray(all, cJSON_GetArraySize(all) - 1);
    if (drop != nullptr) cJSON_Delete(drop);
  }

  cache_store_statuses(db, all, cache_type);
  cache_set_last_type(db, cache_type);
  /* Inline avatars + attachments when the terminal supports kitty graphics,
   * otherwise the plain text listings. */
  if (kgraphics_supported()) {
    ls_render_items(all, wrap);
  } else {
    cache_list_items(db, all, wrap);
  }
  json_free(all);
  return 0;
}