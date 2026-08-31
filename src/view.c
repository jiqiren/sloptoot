#include "view.h"

#include "cache.h"
#include "config.h"
#include "http.h"
#include "image.h"
#include "json_helpers.h"
#include "kitty.h"
#include "toot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* A width this wide earns the side-by-side layout (text left / image right);
 * narrower terminals stack the image under the text. */
#define SPLIT_MIN_COLS 92u

/* Minimum columns for the text pane in the side-by-side layout, and a floor
 * for the image pane so a very wide image cannot starve the text completely. */
#define TEXT_MIN_COLS 40u
#define IMAGE_MIN_COLS 30u

typedef struct {
  const cJSON *att; /* the media_attachments entry */
  img_image im;     /* decoded frames (0 = failed) */
  bool ok;
  unsigned px_w, px_h;
} img_ent;

/* Is this attachment one of the formats we can decode and draw? */
static bool attachment_is_image(const cJSON *att) {
  const char *mime =
      att != nullptr ? json_get_string(att, "mime_type") : nullptr;
  if (mime != nullptr) {
    if (strcmp(mime, "image/png") == 0 || strcmp(mime, "image/jpeg") == 0 ||
        strcmp(mime, "image/jpg") == 0 || strcmp(mime, "image/gif") == 0 ||
        strcmp(mime, "image/webp") == 0)
      return true;
    return false;
  }
  /* No mime_type (older instances): sniff the URL's extension. */
  const char *url = att != nullptr ? json_get_string(att, "url") : nullptr;
  if (url == nullptr) return false;
  const char *ext = strrchr(url, '.');
  if (ext == nullptr) return false;
  return strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 ||
         strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".gif") == 0 ||
         strcasecmp(ext, ".webp") == 0;
}

/* Fetch and decode a single image attachment. Media rarely needs auth and is
 * often served from a different host than the instance; the bearer token is
 * never sent to a CDN. */
static bool load_attachment(const cJSON *att, img_image *out) {
  if (!attachment_is_image(att)) return false;
  const char *url = att != nullptr ? json_get_string(att, "url") : nullptr;
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

static void free_imgents(img_ent *ents, size_t n) {
  for (size_t i = 0; i < n; i++) img_free(&ents[i].im);
  free(ents);
}

static void terminal_size(unsigned *cols, unsigned *rows) {
  *cols = 80;
  *rows = 24;
  struct winsize ws = {0};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_col > 0) *cols = ws.ws_col;
    if (ws.ws_row > 0) *rows = ws.ws_row;
  }
}

static void print_trunc(const char *s, unsigned width) {
  char line[4096];
  size_t n = cache_truncate(s, width, line, sizeof(line));
  (void)n;
  printf("%s\n", line);
}

int view_status(const char *ref, bool mobile) {
  char *id = normalize_status_id(ref);
  if (id == nullptr) {
    fprintf(stderr, "error: invalid status reference: %s\n",
            ref != nullptr ? ref : "(null)");
    return 1;
  }

  /* A failing cache is not fatal: we simply skip the offline view and the
   * cache store. */
  cache db;
  bool have_db = cache_open(&db);
  if (!have_db) db = (cache){0};

  /* Mobile: already cached? Serve from the cache, no network. */
  if (mobile && have_db && cache_view(&db, id)) {
    cache_close(&db);
    free(id);
    return 0;
  }

  config c;
  config_init(&c);
  if (!config_load(&c) || c.access_token == nullptr || c.instance == nullptr) {
    fprintf(stderr, "error: not logged in. Run `sloptoot login <instance>`.\n");
    config_free(&c);
    cache_close(&db);
    free(id);
    return 2;
  }

  size_t n = strlen(c.instance) + strlen("/api/v1/statuses/") + strlen(id) + 1;
  char *url = malloc(n);
  if (url == nullptr) {
    config_free(&c);
    cache_close(&db);
    free(id);
    return 3;
  }
  snprintf(url, n, "%s/api/v1/statuses/%s", c.instance, id);

  http_response resp = {0};
  bool ok = http_get(url, c.access_token, &resp);
  free(url);
  if (!ok) {
    fprintf(stderr, "error: network request failed\n");
    http_response_free(&resp);
    config_free(&c);
    cache_close(&db);
    free(id);
    return 3;
  }
  if (resp.code < 200 || resp.code >= 300) {
    fprintf(stderr, "error: HTTP %ld\n", resp.code);
    if (resp.body != nullptr) fprintf(stderr, "%s\n", resp.body);
    http_response_free(&resp);
    config_free(&c);
    cache_close(&db);
    free(id);
    return 3;
  }

  cJSON *root = json_parse(resp.body, resp.len);
  http_response_free(&resp);
  if (root == nullptr) {
    fprintf(stderr, "error: could not parse response\n");
    config_free(&c);
    cache_close(&db);
    free(id);
    return 3;
  }

  /* Cache it (a boost wrapper stores its inner post too) so that later
   * `view --mobile` can serve it without network. */
  if (have_db) cache_store_status(&db, root);

  /* A boost wrapper's real content is on the inner `reblog` status. */
  const cJSON *inner = cJSON_GetObjectItemCaseSensitive(root, "reblog");
  bool is_boost = inner != nullptr && cJSON_IsObject(inner);
  const cJSON *detail = is_boost ? inner : root;

  /* Author (of the underlying post). */
  const cJSON *account = cJSON_GetObjectItemCaseSensitive(detail, "account");
  const char *display = "";
  const char *handle = "";
  if (account != nullptr && cJSON_IsObject(account)) {
    display = json_get_string(account, "display_name");
    handle = json_get_string(account, "acct");
  }
  if (display == nullptr || display[0] == '\0') display = handle;

  /* Body text. */
  char text[8192];
  cache_plain_text(json_get_string(detail, "content"), text, sizeof(text));

  /* Timestamps. */
  const char *created = json_get_string(detail, "created_at");
  char rel[16] = "";
  if (created != nullptr) cache_relative_time(created, rel, sizeof(rel));

  /* Attachment list. */
  const cJSON *media =
      cJSON_GetObjectItemCaseSensitive(detail, "media_attachments");
  int nmedia =
      (media != nullptr && cJSON_IsArray(media)) ? cJSON_GetArraySize(media) : 0;

  /* Pre-load the images we may render, so the layout can be decided up front
   * and no attachment is fetched twice. */
  img_ent *ents = nullptr;
  size_t nents = 0, nok = 0;
  bool gfx = false;
  if (nmedia > 0) {
    bool any_img = false;
    for (int i = 0; i < nmedia; i++)
      if (attachment_is_image(cJSON_GetArrayItem(media, i))) {
        any_img = true;
        break;
      }
    gfx = any_img && kgraphics_supported();
    if (gfx) {
      ents = calloc((size_t)nmedia, sizeof(*ents));
      if (ents == nullptr) gfx = false;
    }
    if (gfx) {
      for (int i = 0; i < nmedia; i++) {
        const cJSON *att = cJSON_GetArrayItem(media, i);
        if (!attachment_is_image(att)) continue;
        ents[nents].att = att;
        if (load_attachment(att, &ents[nents].im)) {
          ents[nents].ok = true;
          ents[nents].px_w = ents[nents].im.frames[0].width;
          ents[nents].px_h = ents[nents].im.frames[0].height;
          nok++;
        }
        nents++;
      }
    }
  }

  /* Terminal geometry. */
  unsigned tcols = 80, trows = 24;
  terminal_size(&tcols, &trows);

  /* Layout: side-by-side needs exactly one attachment that decoded and a
   * wide-enough terminal; anything else stacks the image(s) below the text. */
  bool side = gfx && nmedia == 1 && nok == 1 && tcols >= SPLIT_MIN_COLS;
  unsigned tw = tcols; /* text pane width */
  unsigned img_w = 1, img_h = 1;
  if (side) {
    /* Adaptive split: size the image so it fills the terminal height (its
     * natural full-screen height in columns), leaving at least a 40-column
     * text pane and at least ~30 columns of image. A fixed 58% left a lot of
     * the wide terminal empty for wide images. */
    size_t e = 0;
    while (e < nents && !ents[e].ok) e++;
    img_w = ents[e].px_w > 0 ? ents[e].px_w : 1;
    img_h = ents[e].px_h > 0 ? ents[e].px_h : 1;
    unsigned cw = 8, ch = 16;
    kgraphics_cell_size(&cw, &ch);
    double full_h = (double)trows * ((double)img_w * (double)ch) /
                    ((double)img_h * (double)cw);
    unsigned right = (unsigned)(full_h + 0.5);
    unsigned right_max = tcols > TEXT_MIN_COLS ? tcols - TEXT_MIN_COLS
                                               : (tcols > 6 ? tcols - 6 : 1);
    if (right > right_max) right = right_max;
    if (right < IMAGE_MIN_COLS) right = IMAGE_MIN_COLS;
    tw = tcols - right;
  }

  unsigned used = 0; /* display rows consumed to place the stacked images */
  char wrapped[16384];

  /* When viewing a boost, first show the boost line (booster, when, id). */
  if (is_boost) {
    const cJSON *bacc = cJSON_GetObjectItemCaseSensitive(root, "account");
    const char *bhandle = "";
    if (bacc != nullptr && cJSON_IsObject(bacc))
      bhandle = json_get_string(bacc, "acct");
    const char *bcreated = json_get_string(root, "created_at");
    char brel[16] = "";
    if (bcreated != nullptr) cache_relative_time(bcreated, brel, sizeof(brel));
    printf("\xF0\x9F\x9A\x80 boosted %s\n", bhandle != nullptr ? bhandle : "");
    if (bcreated != nullptr) printf("Boosted: %s (%s)\n", bcreated, brel);
    printf("\n");
    used += 3;
  }

  printf("%s @%s\n", display, handle != nullptr ? handle : "");
  used++;

  {
    size_t nw = cache_wrap(text, tw, wrapped, sizeof(wrapped));
    fputs(wrapped, stdout);
    printf("\n");
    used += (unsigned)(nw > 0 ? nw : 1);
  }

  printf("\n");
  used++;

  char line[4096];
  if (created != nullptr) {
    snprintf(line, sizeof(line), "Posted: %s (%s)", created, rel);
    print_trunc(line, tw);
    used++;
  }
  snprintf(line, sizeof(line), "ID:     %s", json_get_string(detail, "id"));
  print_trunc(line, tw);
  used++;
  const char *post_url = json_get_string(detail, "url");
  if (post_url != nullptr) {
    snprintf(line, sizeof(line), "URL:    %s", post_url);
    print_trunc(line, tw);
    used++;
  }
  const char *visibility = json_get_string(detail, "visibility");
  snprintf(line, sizeof(line), "Visibility: %s",
           visibility != nullptr ? visibility : "?");
  print_trunc(line, tw);
  used++;
  const cJSON *app = cJSON_GetObjectItemCaseSensitive(detail, "application");
  const char *client = app != nullptr ? json_get_string(app, "name") : nullptr;
  snprintf(line, sizeof(line), "Client:   %s",
           client != nullptr && client[0] != '\0' ? client : "-");
  print_trunc(line, tw);
  used++;
  snprintf(line, sizeof(line), "Boosts: %d  Likes: %d  Replies: %d",
           json_get_int(detail, "reblogs_count"),
           json_get_int(detail, "favourites_count"),
           json_get_int(detail, "replies_count"));
  print_trunc(line, tw);
  used++;

  /* Attachments: rendered in the right pane when side-by-side, otherwise
   * listed as captions above the stacked images. */
  if (nmedia > 0) {
    snprintf(line, sizeof(line), "Attachments: %d", nmedia);
    print_trunc(line, tw);
    used++;
    if (!side || !gfx) {
      for (int i = 0; i < nmedia; i++) {
        const cJSON *att = cJSON_GetArrayItem(media, i);
        const char *desc = att != nullptr
                               ? json_get_string(att, "description")
                               : nullptr;
        char alt[1024] = "";
        if (desc != nullptr && desc[0] != '\0')
          cache_plain_text(desc, alt, sizeof(alt));
        if (gfx && attachment_is_image(att) &&
            /* an in-sequence entry should be "rendered below" */
            ents != nullptr) {
          /* find the ent index for this attachment */
          size_t e = 0;
          while (e < nents && ents[e].att != att) e++;
          if (e < nents && ents[e].ok) {
            snprintf(line, sizeof(line), "  [%d] %s", i + 1,
                     alt[0] != '\0' ? alt : "(no description)");
            print_trunc(line, tw);
            used++;
            continue;
          }
        }
        const char *type = att != nullptr ? json_get_string(att, "type") : nullptr;
        if (alt[0] != '\0')
          snprintf(line, sizeof(line), "  [%d] %s", i + 1, alt);
        else
          snprintf(line, sizeof(line), "  [%d] <%s>", i + 1,
                   type != nullptr ? type : "?");
        print_trunc(line, tw);
        used++;
      }
    }
  }

  /* Render the images. */
  if (gfx && nok > 0) {
    unsigned cw = 8, ch = 16;
    kgraphics_cell_size(&cw, &ch);
    if (side && nok == 1) {
      /* Find the single decoded image, pin it to the top-right pane sized
       * preserving aspect ratio (height at most the full terminal), then park
       * the cursor at the bottom-left so the shell prompt appears below the
       * whole layout instead of overwriting the text. */
      size_t e = 0;
      while (e < nents && !ents[e].ok) e++;
      unsigned c = 0, r = 0;
      img_fit_aspect(&ents[e].im, tcols - tw, trows, cw, ch, &c, &r);
      printf("\x1b[1;%uH", tw + 1);
      kgraphics_render(&ents[e].im, c, r, true);
      /* Park the cursor just below the taller of the text pane and the image
       * so the shell prompt appears below the whole layout (pushing the
       * result up) instead of overwriting the left-column text. */
      unsigned erow = used + 1;
      if (erow < r + 1) erow = r + 1;
      if (erow > trows) erow = trows;
      printf("\x1b[%u;1H", erow);
      fflush(stdout);
    } else {
      /* Stack below the text: split the leftover rows among the images,
       * weighted by aspect ratio so tall images get more height, then fit
       * each image within (full width, its allocation) preserving aspect. */
      int budget = (int)trows - (int)used - 2;
      if (budget < 6) budget = 6;
      double sum = 0;
      for (size_t e = 0; e < nents; e++)
        if (ents[e].ok && ents[e].px_w > 0)
          sum += (double)ents[e].px_h / (double)ents[e].px_w;
      for (size_t e = 0; e < nents; e++) {
        if (!ents[e].ok) continue;
        unsigned alloc = (unsigned)budget;
        if (sum > 0 && ents[e].px_w > 0)
          alloc = (unsigned)((double)budget *
                             ((double)ents[e].px_h / (double)ents[e].px_w) /
                             sum);
        if (alloc < 6) alloc = 6;
        unsigned c = 0, r = 0;
        img_fit_aspect(&ents[e].im, tcols - 1, alloc, cw, ch, &c, &r);
        kgraphics_render(&ents[e].im, c, r, false);
      }
    }
  }

  free_imgents(ents, nents);
  json_free(root);
  config_free(&c);
  cache_close(&db);
  free(id);
  return 0;
}