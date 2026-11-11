#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "config.h"
#include "edit.h"
#include "http.h"
#include "json_helpers.h"
#include "oauth.h"
#include "timeline.h"
#include "toot.h"
#include "version.h"
#include "view.h"

static void print_usage(const char *prog) {
  fprintf(stderr,
          "usage: %s <command> [args]\n"
          "\n"
          "commands:\n"
          "  login <instance> [--oob]\n"
          "                     authenticate with a Mastodon instance\n"
          "                     --oob skips the loopback server and uses\n"
          "                     the out-of-band code paste flow\n"
          "  toot [\"<text>\"]    post a new status; if no text is given,\n"
          "                     $EDITOR opens and the post is written there\n"
          "                     flags:\n"
          "                       -r, --reply[=<id-or-url>]\n"
          "                           reply to a status; bare --reply chains\n"
          "                           to your most recent post\n"
          "  ls [profile|timeline|home|#tag] [-m] [-l]\n"
          "                     list posts. default: your posts\n"
          "                     profile   posts/replies from your account\n"
          "                     timeline  your home timeline (alias: home)\n"
          "                     #tag      posts matching a hashtag\n"
          "                     -m, --mobile: serve from the local cache\n"
          "                     instead of fetching fresh data; with no\n"
          "                     type, uses the last loaded timeline\n"
          "                     -l, --long: wrap full text instead of\n"
          "                     truncating to the terminal width\n"
           "  delete <id-or-url> remove a post from your account\n"
           "  view <id-or-url>   show a detailed view of a post\n"
           "                     -m, --mobile: serve cached posts from the\n"
           "                     local cache; uncached posts are fetched\n"
           "                     and stored in the cache\n"
           "  boost <id-or-url>  boost (reblog) a post\n"
           "  like <id-or-url>   favourite a post\n"
           "  bookmark <id-or-url>  bookmark a post\n"
          "  whoami             show the logged-in account\n"
          "  help               show this help\n"
          "  version            show version\n",
          prog);
}

static int cmd_whoami(void) {
  config c;
  config_init(&c);
  if (!config_load(&c) || c.access_token == nullptr || c.instance == nullptr ||
      c.username == nullptr) {
    fprintf(stderr, "error: not logged in. Run `sloptoot login <instance>`.\n");
    config_free(&c);
    return 2;
  }
  printf("@%s@%s\n", c.username,
         c.instance + (strncmp(c.instance, "https://", 8) == 0 ? 8
                        : strncmp(c.instance, "http://", 7) == 0 ? 7
                                                                  : 0));
  config_free(&c);
  return 0;
}

static bool open_cache(cache *db) {
  bool ok = cache_open(db);
  if (!ok) {
    fprintf(stderr, "warning: could not open cache; continuing without it\n");
    *db = (cache){0};
  }
  return ok;
}

static bool looks_like_status_ref(const char *s) {
  if (s == nullptr || s[0] == '\0') return false;
  /* An HTTP(S) status URL. */
  if (strncmp(s, "http", 4) == 0) return true;
  /* A bare numeric legacy Mastodon snowflake id. */
  bool numeric = true, upper_alnum = true;
  size_t len = 0;
  for (const char *p = s; *p != '\0'; p++) {
    len++;
    if (!isdigit((unsigned char)*p)) numeric = false;
    if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z'))) upper_alnum = false;
  }
  if (numeric) return true;
  /* A new-format Mastodon id or GoToSocial ULID: 26 uppercase base32-ish
   * characters (the new-format ids start with '0'). */
  return len == 26 && upper_alnum;
}

static int cmd_toot(int argc, char **argv) {
  const char *text = nullptr;
  const char *reply_ref = nullptr;
  bool reply_last = false;

  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    if (strncmp(a, "--reply=", 8) == 0) {
      reply_ref = a + 8;
    } else if (strcmp(a, "--reply") == 0) {
      if (i + 1 < argc && looks_like_status_ref(argv[i + 1])) {
        reply_ref = argv[++i];
      } else {
        reply_last = true;
      }
    } else if (strncmp(a, "-r=", 3) == 0) {
      reply_ref = a + 3;
    } else if (strncmp(a, "-r", 2) == 0 && a[2] != '\0') {
      reply_ref = a + 2;
    } else if (strcmp(a, "-r") == 0) {
      if (i + 1 < argc && looks_like_status_ref(argv[i + 1])) {
        reply_ref = argv[++i];
      } else {
        reply_last = true;
      }
    } else if (a[0] == '-') {
      fprintf(stderr, "error: unknown toot option: %s\n", a);
      return 1;
    } else if (text == nullptr) {
      text = a;
    } else {
      fprintf(stderr, "error: unexpected argument: %s\n", a);
      return 1;
    }
  }

  cache db;
  open_cache(&db);

  char *reply_id = nullptr;
  if (reply_ref != nullptr) {
    reply_id = resolve_status_id(reply_ref);
    if (reply_id == nullptr) {
      fprintf(stderr, "error: invalid reply reference: %s\n", reply_ref);
      cache_close(&db);
      return 1;
    }
  } else if (reply_last) {
    reply_id = cache_last_post_id(&db);
    if (reply_id == nullptr) {
      fprintf(stderr, "error: no previous post to chain to\n");
      cache_close(&db);
      return 1;
    }
  }

  /* No quoted text: compose in $EDITOR. An untouched buffer cancels. */
  char *editor_text = nullptr;
  if (text == nullptr || text[0] == '\0') {
    editor_result ed = {0};
    int rc = edit_open(&ed);
    if (rc != 0) {
      fprintf(stderr, "error: could not run editor\n");
      free(reply_id);
      cache_close(&db);
      return 1;
    }
    if (ed.cancelled) {
      fprintf(stderr, "cancelled: nothing written\n");
      free(reply_id);
      cache_close(&db);
      return 0;
    }
    editor_text = ed.text;
    text = editor_text;
  }

  int rc = post_status(text, reply_id, &db);
  free(editor_text);
  free(reply_id);
  cache_close(&db);
  return rc;
}

static int cmd_ls(int argc, char **argv) {
  const char *type = nullptr;
  bool mobile = false;
  bool wrap = false;

  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--mobile") == 0 || strcmp(a, "-m") == 0) {
      mobile = true;
    } else if (strcmp(a, "--long") == 0 || strcmp(a, "-l") == 0) {
      wrap = true;
    } else if (a[0] == '-') {
      fprintf(stderr, "error: unknown ls option: %s\n", a);
      return 1;
    } else if (type == nullptr) {
      type = a;
    } else {
      fprintf(stderr, "error: unexpected argument: %s\n", a);
      return 1;
    }
  }

  const char *norm = type;

  config c;
  config_init(&c);
  if (!config_load(&c) || c.access_token == nullptr || c.instance == nullptr) {
    fprintf(stderr, "error: not logged in. Run `sloptoot login <instance>`.\n");
    config_free(&c);
    return 2;
  }
  cache db;
  open_cache(&db);

  /* Resolve the listing type. With no argument, `--mobile` serves whatever
   * timeline was loaded last (the cache stores a tag without its '#'), and
   * otherwise defaults to the profile. */
  char *last_type = nullptr;
  char *tag_type = nullptr;
  if (type == nullptr) {
    if (mobile) {
      last_type = cache_last_type(&db);
      if (last_type != nullptr && strcmp(last_type, "home") != 0 &&
          strcmp(last_type, "profile") != 0) {
        size_t n = strlen(last_type) + 2;
        tag_type = malloc(n);
        if (tag_type != nullptr) snprintf(tag_type, n, "#%s", last_type);
      }
      norm = tag_type != nullptr ? tag_type
                                 : (last_type != nullptr ? last_type
                                                         : "profile");
    } else {
      norm = "profile";
    }
  } else if (strcmp(type, "profile") == 0) {
    norm = "profile";
  } else if (strcmp(type, "timeline") == 0) {
    norm = "home";
  }

  int rc = ls_statuses(&c, &db, norm, mobile, wrap);
  free(tag_type);
  free(last_type);
  config_free(&c);
  cache_close(&db);
  return rc;
}

static int cmd_delete(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s delete <id-or-url>\n", "sloptoot");
    return 1;
  }
  if (argc > 3) {
    fprintf(stderr, "error: unexpected argument: %s\n", argv[3]);
    return 1;
  }
  cache db;
  open_cache(&db);
  int rc = delete_status(argv[2], &db);
  cache_close(&db);
  return rc;
}

static int cmd_view(int argc, char **argv) {
  const char *ref = nullptr;
  bool mobile = false;

  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--mobile") == 0 || strcmp(a, "-m") == 0) {
      mobile = true;
    } else if (a[0] == '-') {
      fprintf(stderr, "error: unknown view option: %s\n", a);
      return 1;
    } else if (ref == nullptr) {
      ref = a;
    } else {
      fprintf(stderr, "error: unexpected argument: %s\n", a);
      return 1;
    }
  }

  if (ref == nullptr) {
    fprintf(stderr, "usage: %s view <id-or-url> [-m]\n", "sloptoot");
    return 1;
  }
  return view_status(ref, mobile);
}

static const char *status_action_for(const char *label) {
  if (strcmp(label, "boost") == 0) return "reblog";
  if (strcmp(label, "like") == 0) return "favourite";
  if (strcmp(label, "bookmark") == 0) return "bookmark";
  return label;
}

static int cmd_action(int argc, char **argv, const char *label) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s %s <id-or-url>\n", "sloptoot", label);
    return 1;
  }
  if (argc > 3) {
    fprintf(stderr, "error: unexpected argument: %s\n", argv[3]);
    return 1;
  }
  return status_action(argv[2], status_action_for(label));
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  http_global_init();
  int rc = 0;

  if (strcmp(argv[1], "login") == 0) {
    if (argc < 3) {
      fprintf(stderr, "usage: %s login <instance> [--oob]\n", argv[0]);
      rc = 1;
    } else {
      bool force_oob = false;
      const char *instance_arg = nullptr;
      for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--oob") == 0) {
          force_oob = true;
        } else if (instance_arg == nullptr) {
          instance_arg = argv[i];
        } else {
          fprintf(stderr, "error: unexpected argument: %s\n", argv[i]);
          rc = 1;
          break;
        }
      }
      if (rc == 0 && instance_arg == nullptr) {
        fprintf(stderr, "usage: %s login <instance> [--oob]\n", argv[0]);
        rc = 1;
      } else if (rc == 0) {
        rc = login(instance_arg, force_oob);
      }
    }
  } else if (strcmp(argv[1], "toot") == 0) {
    rc = cmd_toot(argc, argv);
  } else if (strcmp(argv[1], "ls") == 0) {
    rc = cmd_ls(argc, argv);
  } else if (strcmp(argv[1], "delete") == 0) {
    rc = cmd_delete(argc, argv);
  } else if (strcmp(argv[1], "view") == 0) {
    rc = cmd_view(argc, argv);
  } else if (strcmp(argv[1], "boost") == 0) {
    rc = cmd_action(argc, argv, "boost");
  } else if (strcmp(argv[1], "like") == 0) {
    rc = cmd_action(argc, argv, "like");
  } else if (strcmp(argv[1], "bookmark") == 0) {
    rc = cmd_action(argc, argv, "bookmark");
  } else if (strcmp(argv[1], "whoami") == 0) {
    rc = cmd_whoami();
  } else if (strcmp(argv[1], "version") == 0 ||
             strcmp(argv[1], "--version") == 0 ||
             strcmp(argv[1], "-V") == 0) {
    printf("sloptoot %s\n", SLOPTOOT_VERSION);
    rc = 0;
  } else if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
             strcmp(argv[1], "-h") == 0) {
    print_usage(argv[0]);
    rc = 0;
  } else {
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    print_usage(argv[0]);
    rc = 1;
  }

  http_global_cleanup();
  return rc;
}