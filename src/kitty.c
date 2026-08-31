#include "kitty.h"

#include "base64.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define PROBE_TIMEOUT_MS 800u
#define QUERY_TIMEOUT_MS 200u
#define MAX_REPLY 4096
#define CHUNK 4092 /* multiple of 4, stays under the 4096 byte limit */

static int g_support = -1; /* -1 unknown, 0 unsupported, 1 supported */
static uint32_t g_next_id = 1;
static unsigned g_cell_w = 0;
static unsigned g_cell_h = 0;

/* Send `cmd` to the terminal (may be empty to just listen), put stdin in raw
 * mode, and read the terminal's reply within `timeout_ms`. Reads until the
 * response settles, then flushes any residual input so stray terminal
 * control sequences never reach the shell or the screen afterwards. Restores
 * termios before returning. Returns bytes read. */
static size_t tty_query(const char *cmd, size_t cmdlen, int timeout_ms,
                        char *buf, size_t cap) {
  struct termios old, raw;
  if (tcgetattr(STDIN_FILENO, &old) != 0) return 0;
  raw = old;
  raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IXON);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  if (cmdlen > 0) {
    fwrite(cmd, 1, cmdlen, stdout);
    fflush(stdout);
  }

  struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
  size_t n = 0;
  int remaining_ms = timeout_ms;
  while ((int)n < (int)cap - 1 && remaining_ms > 0) {
    int pr = poll(&pfd, 1, remaining_ms);
    if (pr <= 0 || !(pfd.revents & POLLIN)) break;
    ssize_t r = read(STDIN_FILENO, buf + n, cap - 1 - n);
    if (r <= 0) break;
    n += (size_t)r;
    remaining_ms = 40; /* more reply bytes may follow; keep reading briefly */
  }

  tcsetattr(STDIN_FILENO, TCSANOW, &old);
  tcflush(STDIN_FILENO, TCIFLUSH);
  buf[n] = '\0';
  return n;
}

static bool terminal_usable(void) {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
  const char *term = getenv("TERM");
  if (term == nullptr || term[0] == '\0' || strcmp(term, "dumb") == 0)
    return false;
  return true;
}

static bool probe_support(void) {
  /* Debug/testing escape hatch: with SLOPTOOT_FORCE_GFX set the probe is
   * skipped so the control sequences can be captured and inspected (for
   * example under `script`). */
  if (getenv("SLOPTOOT_FORCE_GFX") != nullptr) return true;

  /* The recommended probe: a query action with an id. A graphics-capable
   * terminal replies `i=31;OK` immediately; silence means unsupported.
   * Note: no DA1 (`CSI c`) is appended — the response to that would be
   * echoed onto the screen/shifted into the image and is not needed. */
  static const char probe[] =
      "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";
  char buf[MAX_REPLY];
  tty_query(probe, sizeof(probe) - 1, (int)PROBE_TIMEOUT_MS, buf, sizeof(buf));
  return strstr(buf, "i=31;OK") != nullptr;
}

bool kgraphics_supported(void) {
  if (g_support == -1)
    g_support = terminal_usable() && probe_support() ? 1 : 0;
  return g_support == 1;
}

/* Best-effort character cell size in pixels: XTWINOPS 16 (CSI 16 t) first,
 * else window pixel size (CSI 14 t) divided by the cell grid, else a
 * conventional 8x16. Cached after the first call. */
void kgraphics_cell_size(unsigned *w, unsigned *h) {
  if (g_cell_w == 0) {
    char buf[MAX_REPLY];

    tty_query("\x1b[16t", 6, (int)QUERY_TIMEOUT_MS, buf, sizeof(buf));
    unsigned ch = 0, cw = 0;
    if (sscanf(buf, "\x1b[6;%u;%ut", &ch, &cw) == 2 && ch > 0 && cw > 0) {
      g_cell_w = cw;
      g_cell_h = ch;
    }
    if (g_cell_w == 0) {
      tty_query("\x1b[14t", 6, (int)QUERY_TIMEOUT_MS, buf, sizeof(buf));
      unsigned ph = 0, pw = 0;
      if (sscanf(buf, "\x1b[4;%u;%ut", &ph, &pw) == 2 && ph > 0 && pw > 0) {
        unsigned cols = 80, rows = 24;
        struct winsize ws = {0};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
          if (ws.ws_col > 0) cols = ws.ws_col;
          if (ws.ws_row > 0) rows = ws.ws_row;
        }
        g_cell_w = cols > 0 ? pw / cols : 0;
        g_cell_h = rows > 0 ? ph / rows : 0;
      }
    }
    if (g_cell_w == 0) g_cell_w = 8;
    if (g_cell_h == 0) g_cell_h = 16;
  }
  if (w != nullptr) *w = g_cell_w;
  if (h != nullptr) *h = g_cell_h;
}

/* Emit one base64 payload across the required graphics escape codes.
 * `first_kv` holds the control keys for the first (header) chunk; trailing
 * chunks carry only `m` (plus `a=f` for animation frame data). */
static void chunked_send(bool anim, const char *first_kv, const char *b64) {
  size_t n = strlen(b64);
  size_t off = 0;
  bool first = true;
  while (off < n || first) {
    size_t take = (n - off) < CHUNK ? (n - off) : CHUNK;
    int more = (off + take < n) ? 1 : 0;
    if (first) {
      printf("\x1b_G%s,m=%d;%.*s\x1b\\", first_kv, more, (int)take, b64 + off);
    } else if (anim) {
      printf("\x1b_Ga=f,m=%d;%.*s\x1b\\", more, (int)take, b64 + off);
    } else {
      printf("\x1b_Gm=%d;%.*s\x1b\\", more, (int)take, b64 + off);
    }
    first = false;
    off += take;
    if (more == 0) break;
  }
}

/* Transmit the base image (a=t: transmit only, the placement comes later),
 * or one animation frame (a=f). */
static bool send_png(uint32_t id, bool anim, uint32_t delay_ms,
                     const uint8_t *png, size_t png_len) {
  char *b64 = base64_encode(png, png_len);
  if (b64 == nullptr) return false;

  char kv[80];
  if (anim)
    snprintf(kv, sizeof(kv), "a=f,i=%u,f=100,q=2,z=%u", id, delay_ms);
  else
    snprintf(kv, sizeof(kv), "a=t,i=%u,f=100,q=2", id);
  chunked_send(anim, kv, b64);
  free(b64);
  return true;
}

bool kgraphics_render(const img_image *img, unsigned cols, unsigned rows,
                      bool hold_cursor) {
  if (img == nullptr || img->nframes == 0) return false;
  if (img->frames[0].width == 0 || img->frames[0].height == 0) return false;
  if (!kgraphics_supported()) return false;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;

  uint32_t id = g_next_id++;
  uint8_t *png = nullptr;
  size_t png_len = 0;
  if (img_encode_png(&img->frames[0], &png, &png_len) != 0) return false;
  bool ok = send_png(id, false, 0, png, png_len);
  free(png);
  if (!ok) return false;

  /* Animated: transmit the remaining frames with their gaps, then start the
   * animation in looping mode. */
  if (img->nframes > 1) {
    ok = true;
    for (size_t i = 1; i < img->nframes; i++) {
      uint8_t *fp = nullptr;
      size_t fl = 0;
      if (img_encode_png(&img->frames[i], &fp, &fl) != 0) {
        ok = false;
        break;
      }
      ok = send_png(id, true, img->frames[i].delay_ms, fp, fl);
      free(fp);
      if (!ok) break;
    }
    if (ok) {
      /* Root-frame gap (the base image has no z of its own). */
      if (img->frames[0].delay_ms > 0)
        printf("\x1b_Ga=a,i=%u,r=1,z=%u\x1b\\", id, img->frames[0].delay_ms);
      uint32_t loops = img->loop_count > 0 ? img->loop_count : 1;
      printf("\x1b_Ga=a,i=%u,s=3,v=%u\x1b\\", id, loops);
    }
    if (!ok) return true; /* degrade to a still frame */
  }

  if (hold_cursor)
    printf("\x1b_Ga=p,i=%u,c=%u,r=%u,q=2,C=1\x1b\\", id, cols, rows);
  else
    printf("\x1b_Ga=p,i=%u,c=%u,r=%u,q=2\x1b\\", id, cols, rows);
  fflush(stdout);
  return true;
}