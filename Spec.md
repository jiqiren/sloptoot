# sloptoot — Specification

A minimal command-line Mastodon client for sending quick toots, written in pure C23.

## Goals
- Fast, single-binary CLI for posting toots and authenticating with Mastodon.
- Cross-platform: macOS and Linux.
- Homebrew-friendly build system.
- Minimal external dependencies.

## Non-goals (v1)
- Streaming, notifications, media uploads.
- Multiple account profiles (single active account only).
- Windows support.

## Stack
- **Language:** C23 (`-std=c23`), Clang/GCC.
- **Build system:** Meson (accepted by the brew project). `meson.build` at repo root.
- **HTTP/TLS:** libcurl — hard system dependency, linked via pkg-config.
- **JSON:** cJSON via a Meson subproject wrap (`subprojects/cjson.wrap`). Not vendored. At build time, `dependency('libcjson', fallback : ['cjson', 'libcjson_dep'])` tries the system-installed cjson first (via pkg-config), falling back to the subproject only when not found. This lets Homebrew use its brewed `cjson` formula (`--wrap-mode=nofallback`) while local dev builds auto-fetch the wrap.
- **SQLite (cache):** system `sqlite3` via pkg-config (`dependency('sqlite3')`). On macOS the Homebrew `sqlite` formula (keg-only) provides the header, library, and `.pc`; on Linux the distro `sqlite3` provides them. Not vendored.
- **Image decoding (inline attachment viewing):** pkg-config deps, not vendored:
  - libpng (`dependency('libpng')`) — PNG decode, and PNG re-encode of decoded frames. The kitty protocol accepts only PNG or raw RGBA, so every format is decoded to RGBA and re-sent as PNG.
  - libjpeg-turbo (`dependency('libjpeg')`) — JPEG decode through the classic libjpeg API (`JCS_EXT_RGBA` output).
  - libnsgif (`dependency('libnsgif')`) — GIF decode including animations (frame delays + loop count).
  - libwebp / libwebpdemux (`dependency('libwebp')`, `dependency('libwebpdemux')`) — WebP decode; `WebPDemux`/`WebPAnimDecoder` for animated WebP.
- **SHA-256 (PKCE):** per-platform shim, no vendored crypto:
  - macOS: CommonCrypto `<CommonCrypto/CommonDigest.h>` (`CC_SHA256`). System framework, no link flag.
  - Linux: `<openssl/sha.h>` (`SHA256()`), linked against `-lcrypto` via pkg-config (`dependency('libcrypto')`).
- **Random bytes:** read 32 bytes from `/dev/urandom` (present on both macOS and Linux).
- **Feature macro (Linux):** `_GNU_SOURCE` is defined via `c_args` on non-darwin to expose POSIX functions (`fchmod`, `fdopen`) that glibc hides by default. macOS needs no such macro (defining `_POSIX_C_SOURCE` would hide BSD symbols like `INADDR_LOOPBACK` and `mkdtemp`).

## App identity
- `client_name` sent to Mastodon during app registration: **`slop TooT`** (exact casing/spelling).
- `redirect_uris`: registered dynamically to match the bound loopback port (see login flow); OOB URI is always included as a fallback. Multiple URIs are joined with a **newline** (`\n`), not a space — GoToSocial splits `redirect_uris` on newline only, while Mastodon accepts both. Newline works for both implementations.
- `scopes`: `read write`.

## Distribution
- **Homebrew tap:** `jiqiren/homebrew-tap` (https://github.com/jiqiren/homebrew-tap).
- Install with `brew install jiqiren/tap/sloptoot`.
- The formula builds from a tagged GitHub release archive (`https://github.com/jiqiren/sloptoot/archive/refs/tags/v<version>.tar.gz`), verified with SHA-256.
- Formula dependencies: `meson`, `ninja`, `pkgconf` (build); `curl`, `cjson`, `libpng`, `jpeg-turbo`, `libnsgif`, `webp` (runtime); `openssl@3` (runtime, Linux only).
- The formula passes `meson setup` with `--wrap-mode=nofallback` so the system `cjson` is used instead of building the subproject.

## Commands

### `sloptoot login <instance>`
Performs the full Mastodon OAuth login dance and persists credentials.

Flow (default PKCE + loopback, OOB fallback):
1. Validate `instance` arg (required). Normalize to `https://<instance>`.
2. **Bind loopback HTTP server** on `127.0.0.1:0` (ephemeral port) first. `redirect_uri = http://127.0.0.1:<port>/callback`.
   - If bind fails → fall back to OOB: `redirect_uri = urn:ietf:wg:oauth:2.0:oob`.
3. **Register app** — `POST /api/v1/apps` with `client_name="slop TooT"`, `redirect_uris` set to the exact bound `redirect_uri` plus `urn:ietf:wg:oauth:2.0:oob` (joined with `\n`; OOB only, on fallback), `scopes="read write"`. Parse `client_id`, `client_secret`. (Binding first lets us register the exact URI Mastodon will match at authorize time.)
4. **Generate PKCE:**
   - `code_verifier` = base64url(32 random bytes from `/dev/urandom`) → 43 chars.
   - `code_challenge` = base64url(SHA-256(verifier)), `code_challenge_method = S256`.
5. **Open browser** to `GET /oauth/authorize?response_type=code&client_id=...&redirect_uri=<enc>&scope=read+write&code_challenge=...&code_challenge_method=S256&state=<rand>`.
   - macOS: `open <url>`. Linux: `xdg-open <url>`.
6. **Capture code:** loopback server accepts exactly one request, validates `state`, extracts `code`, responds with a small HTML "you can close this" page, then shuts down. (OOB fallback: read code from stdin.)
7. **Exchange token** — `POST /oauth/token` form: `grant_type=authorization_code`, `code`, `client_id`, `client_secret`, `redirect_uri`, `code_verifier`. Parse `access_token`.
8. **Verify** — `GET /api/v1/accounts/verify_credentials` with `Authorization: Bearer <token>`. Print `@username@instance`.
9. **Save** `instance`, `client_id`, `client_secret`, `access_token`, `account_id`, `username` to config (mode 0600). Print success.

### `sloptoot toot "<text>"`
1. `config_load()` — require `instance` + `access_token` (exit 2 if not logged in).
2. URL-encode status text. `POST /api/v1/statuses` form `status=<enc>&visibility=public` with Bearer header.
3. Parse `Status` entity; print posted `url` field. On non-2xx, print error + response body.

### `sloptoot toot [flags]`
Flags (combined with the text form above):
- `--reply`, `-r` **without a value** reposts as a reply to your most recently posted status (the chain anchor is kept in the SQLite cache). Requires at least one prior post; otherwise exit 1.
- `--reply <id-or-url>`, `-r <id-or-url>`, `--reply=<id-or-url>`, `-r<id-or-url>` reply to a specific status. A bare numeric id is used directly; a status URL has its trailing id extracted (`normalize_status_id`). Invalid references exit 1.
- **Editor fallback:** when no positional `"<text>"` is given, `$EDITOR` (falls back to `VISUAL`, then `vi`) opens on a temp file. If the buffer is left empty/unchanged the post is cancelled (exit 0, "cancelled" printed). Otherwise the written text is posted.
- After any successful post the new status id is recorded as the next chain anchor.

### `sloptoot ls [profile|timeline|#tag] [-m] [-l]`
Lists posts as plain text lines (`<id>  <relative-time>  <text>` with HTML stripped). The id is the Mastodon status id, so a listed post can be replied to directly with `toot -r <id>` or deleted with `delete <id>`. Relative time is shown as `now`/`5m`/`3h`/`2d`/`4w`. Each line is truncated to the terminal column count (via `TIOCGWINSZ`); pass `-l`/`--long` to print full (wrapped) text instead.

By default `ls` fetches enough posts (paging via `max_id`, `limit=40`) to fill the terminal height minus two rows (via terminal rows from `TIOCGWINSZ`, fallback 24), so the shell prompt isn't pushed off. Only the posts that fit are printed.

**Inline images** (kitty-capable terminals only): when the terminal supports the kitty graphics protocol (`kgraphics_supported()`), `ls` renders each post's author avatar (the reblogger's for boosts) to the left of the text — a two-row-tall, aspect-preserving placement drawn behind a reserved gutter — and stacks any image `media_attachments` below the post (downloaded as the small `preview_url` thumbnail, capped at ~40% of the terminal height) so a wide feed image never monopolizes the listing (the full image is one `view` away). Text is word-wrapped to the remaining width; a `C=1` avatar placement never moves the cursor, and the listing stops once the screen is full so the prompt stays below. `--mobile` renders the same graphics layout, but the statuses come from the SQLite cache; each avatar/media image is read from the local media cache, and a cache miss is fetched (and stored) — so a fully-offline run still draws only what was previously cached. In a terminal without kitty support, or when `--mobile` images aren't cached, the affected rows fall back to the plain text rows described above.

**Boosts** are shown specially: the first line shows the boost id, relative time, a 🚀 emoji, and the boosting account's handle; a second, slightly indented line (introduced by `⎣`) shows the id, relative time, and text of the originally boosted post (its `reblog`).
- No argument or `profile` — your posts/replies: `GET /api/v1/accounts/{account_id}/statuses`.
- `timeline` — your home timeline: `GET /api/v1/timelines/home`.
- `#<tag>` — hashtag results: `GET /api/v1/timelines/tag/{tag}`.
- `-m`, `--mobile` — serve from the SQLite cache instead of fetching (no network). The cache stores each status's avatar URL and media attachment URLs (`status_media`), so a kitty-capable terminal renders the same graphic layout from the local media cache.

All loaded statuses are stored in the SQLite cache keyed by timeline type. The most recently used type is remembered so `--mobile` serves it too.

### `sloptoot delete <id-or-url>`
1. `normalize_status_id(ref)` — a bare numeric id is used directly; a status URL has its trailing id extracted.
2. `DELETE /api/v1/statuses/{id}` with Bearer header (exit 3 on network/non-2xx error, exit 1 on an invalid reference).
3. On success the id is removed from the SQLite cache (`cache_delete_status`); if it was the last-post chain anchor that anchor is cleared. Prints `deleted <id>`.

### `sloptoot whoami`
`GET /api/v1/accounts/verify_credentials`; print `@username@instance`. Cheap token-validation helper.

### `sloptoot version` / `--version` / `-V`
Print `sloptoot <version>`. The version is configured by Meson from `meson.build` into a generated `src/version.h` (via `src/version.h.in`).

### `sloptoot help` / bare invocation
Print usage.

### `sloptoot view <id-or-url>`
`GET /api/v1/statuses/{id}` and print a detailed view: author display name and handle, the post text (HTML stripped), posted timestamp with relative time, status id, canonical URL, and `reblogs_count` / `favourites_count` / `replies_count`. Accepts a bare id (Mastodon numeric snowflake or GoToSocial ULID) or a status URL. Exit 1 on an invalid reference, 3 on network/HTTP errors.

**When the id is a boost**, the view shows the boost line first (`🚀 @<booster> boosted`, when it was boosted), then the detailed view of the *underlying* post from its `reblog`.

**Image attachments** are rendered inline via the [kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol.html) when the terminal supports it:
1. The terminal environment (`TERM`, `TERM_PROGRAM`, vendor variables such as `KITTY_WINDOW_ID`) must identify a kitty-graphics-capable terminal family — the probe is an APC escape that sloppy parsers (Apple Terminal and friends) print as garbage, so it is never sent elsewhere. For marked terminals, graphics support is probed once per run with a query action (`a=q,i=31,t=d`) — a non-replying terminal (tmux without passthrough, `TERM=dumb`, piped stdout/stdin) silently disables rendering. `SLOPTOOT_FORCE_GFX=1` bypasses both the environment gate and the probe.
2. Each attachment with an image MIME type (`mime_type` field, falling back to the URL extension for `.png`/`.jpg`/`.jpeg`/`.gif`/`.webp`) is fetched (no auth — media often lives on a CDN), decoded to RGBA frames (`src/image.c`), and re-encoded as PNG.
3. **Hybrid layout** keeps the whole post on screen:
   - In a terminal ≥92 columns with exactly one decoded image, the text/metadata pane sits on the left and the image is pinned to the top-right (CUP to the split column, then `a=p,i=<id>,c=<cols>,r=<rows>,C=1` so the cursor doesn't move). The split is **adaptive**: the image is sized to fill the terminal height at its native aspect ratio (character cell size via `CSI 16 t`, falling back to `CSI 14 t`, then 8×16), with the text taking what remains (≥40 columns); a fixed split left wide terminals underused for wide images.
   - Otherwise (narrow terminal, no/tall/multiple images) the image(s) are stacked below the text, each `a=p,i=<id>,c=<cols>,r=<rows>` at the current cursor; the leftover rows are split among images by aspect ratio, and each image keeps its aspect ratio.
   - The current cursor is parked **below the taller of the text and the image** after the side-by-side placement so the shell prompt appears under the whole view instead of overwriting the left-column text.
   - Placement rectangles are always computed from the image's pixel dimensions and the cell size so **aspect ratio is preserved** — the kitty protocol distorts the image if `c`/`r` don't match its cell aspect.
4. Transmit is chunked base64 (`a=t`, ≤4096-byte chunks); animated GIFs/WebPs transmit the extra frames with `a=f,i=<id>,z=<gap-ms>` and start with `a=a,i=<id>,s=3,v=<loops>` (infinite loop when the file says "loop forever").

If rendering is impossible (unsupported format, decode failure, network error on the media URL, or unsupported terminal), that attachment falls back to a plain `<type>` / alt-text line. `SLOPTOOT_FORCE_GFX=1` skips the capability probe (for capturing/debugging the emitted escapes, e.g. under `script`); the tty checks still apply.

- `-m`, `--mobile` — if the post is already in the local cache, print the cached copy (handle, text, posted time, id; no network). A post not in the cache is fetched normally, and the fetched status (and its boosted inner, for boosts) is stored in the cache so later `--mobile` views are fully offline. If the cache cannot be opened the command proceeds with a plain online view.

### `sloptoot boost|like|bookmark <id-or-url>`
Status actions: `boost` (reblog), `like` (favourite), and `bookmark` each `POST /api/v1/statuses/{id}/{action}` (`reblog`, `favourite`, `bookmark`) with the Bearer header and no body. Accepts a bare id (numeric or ULID) or a status URL. **If the id is a boost, the action is applied to the underlying `reblog` post.** Prints the API action on success. Exit 1 on an invalid reference, 2 not logged in, 3 on network/HTTP errors.

The shared resolver `resolve_status_id(ref)` fetches the status and unwraps a boost before actions and replies; a reply to a boost (`toot --reply <boost-id>`) also targets the underlying post.

## Config storage
- Path: `$XDG_CONFIG_HOME/sloptoot/config` or `$HOME/.config/sloptoot/config`.
- Format: flat `key=value`, one per line.
- Fields: `instance`, `client_id`, `client_secret`, `access_token`, `account_id`, `username`.
- Created with mode 0600 (`open(O_CREAT, 0600)` + `fchmod`).

## SQLite cache
- Path: `$XDG_CACHE_HOME/sloptoot/cache.db` or `$HOME/.cache/sloptoot/cache.db` (the same cache dir as the media cache, so all cacheable data stays under one roof). A `cache.db` left in the config dir by a pre-1.4.3 build is moved here automatically on the first cache use (`cache_open` renames it before opening; failures are never fatal — the old cache is simply abandoned).
- Tables:
  - `statuses(id, created_at, account, content, reblog_of, avatar)` — deduplicated statuses; a boost wrapper row carries the boosted inner id in `reblog_of`, the booster's handle in `account`, and the booster's avatar URL in `avatar`. The inner boosted post is stored for lookup but is not itself a timeline entry.
  - `status_media(status_id, pos, mime, url, preview, description, type)` — each status's `media_attachments`, so `ls --mobile` can resolve the cached image files for offline rendering.
  - `timeline_statuses(type, id)` — which timeline type surfaced each status.
  - `meta(k, v)` — scalars: `last_post_id` (thread chain anchor), `last_type` (most recently loaded timeline).
- Contents are post metadata/ids only (plus avatar/media *URLs*, which are public) — never secrets. An existing `cache.db` from an older version is migrated on open (the `avatar` column and `status_media` table are added).

## Media cache
- Path: `$XDG_CACHE_HOME/sloptoot/media/` or `$HOME/.cache/sloptoot/media/`.
- One file per URL, named by the hex SHA-256 of the URL, so a lookup is a single deterministic `open()` — O(1) keyed by URL. `media_cache_fetch(url)` consults the cache first and only downloads (public, unauthenticated media) on a miss, then persists the raw bytes for next time. Cached entries never expire; Mastodon/GoToSocial URLs are content-addressed/versioned, so a changed image gets a new URL and a new cache key.
- Files are written atomically (temp + rename, mode 0600). Cache/disk failures are never fatal — the caller still gets a fresh download. This is only a performance cache: entries can be deleted freely (`rm -rf ~/.cache/sloptoot`).
- What is cached: avatar images (`avatar_static`/`avatar`) and image `media_attachments`. `view` caches the full-resolution media `url`; `ls` stacked listings fetch instead the small `preview_url` thumbnail (when the instance provides one) so a feed listing doesn't download full-res files. The same files are reused by `ls --mobile`, whose rows carry the media/avatar URLs from the SQLite cache.

## Exit codes
- `0` success
- `1` usage error
- `2` not logged in
- `3` network / HTTP error

## Project layout
```
sloptoot/
├── meson.build
├── subprojects/cjson.wrap
├── src/
│   ├── main.c
│   ├── version.h.in        (template; Meson generates version.h)
│   ├── config.h / config.c
│   ├── http.h / http.c
│   ├── json_helpers.h / json_helpers.c
│   ├── oauth.h / oauth.c
│   ├── loopback.h / loopback.c
│   ├── browser.h / browser.c
│   ├── sha256.h
│   ├── sha256_apple.c
│   ├── sha256_posix.c
│   ├── base64.h / base64.c
│   ├── toot.h / toot.c       (post + reply, delete, normalize id, chain anchor)
│   ├── timeline.h / timeline.c (ls: profile/home/#tag)
│   ├── cache.h / cache.c     (SQLite status cache)
│   ├── media_cache.h / media_cache.c (on-disk image cache keyed by URL hash; feed + view images)
│   ├── edit.h / edit.c       ($EDITOR compose flow)
│   ├── view.h / view.c       (view: detailed single-status view; inline image attachments)
│   ├── image.h / image.c     (decode JPEG/PNG/GIF/WebP -> RGBA frames; encode RGBA -> PNG)
│   ├── kitty.h / kitty.c     (kitty graphics protocol: probe, transmit, rectangle placement, animation)
├── tests/
│   ├── crypto_test.c
│   ├── config_test.c
│   ├── loopback_test.c
│   ├── cache_test.c
│   ├── image_test.c          (decode/round-trip checks against real fixtures)
│   ├── image_fixtures.h      (generated base64 of the sample PNG/JPEG/GIF/WebP images)
│   └── fixtures/             (sample source images used to build image_fixtures.h)
├── Spec.md
├── AGENTS.md
├── README.md
└── LICENSE
```

## Build dependencies
- macOS: `brew install meson pkg-config curl sqlite libpng jpeg-turbo libnsgif webp` (cJSON auto-fetched via wrap, or use `brew install cjson` to use the system one).
- Linux: install `meson`, `pkg-config`, `libcurl4-openssl-dev` (or distro equivalent), `libssl-dev` (provides `libcrypto`), `libsqlite3-dev`, `libpng-dev`, `libjpeg-turbo8-dev`, `libnsgif-dev`, `libwebp-dev` (or the distro equivalents).

## Meson build essentials
- `project('sloptoot','c',version:'<version>',default_options:['c_std=c23','warning_level=2','werror=true'])`.
- `dependency('libcurl')` via pkg-config.
- `dependency('libcjson', fallback : ['cjson', 'libcjson_dep'])` — uses system cjson if available, else builds the subproject.
- `dependency('sqlite3')` via pkg-config (Homebrew keg `sqlite` provides a `.pc`).
- Image deps via pkg-config: `dependency('libpng')`, `dependency('libjpeg')`, `dependency('libnsgif')`, `dependency('libwebp')`, `dependency('libwebpdemux')`.
- `configure_file(input : 'src/version.h.in', output : 'version.h', configuration : ...)` injects the project version.
- `_GNU_SOURCE` added to `c_args` on non-darwin for POSIX function visibility.
- Linux only: `dependency('libcrypto')`.
- Per-platform source selection: `sha256_apple.c` on `'darwin'`, else `sha256_posix.c`.
- `executable('sloptoot', ..., install : true)` — `meson install` places the binary in the prefix `bin/`.
- Tests: `crypto_test`, `config_test`, `loopback_test`, `cache_test`, `image_test` (run via `meson test`).

## Verification
- `meson setup build && meson compile -C build` clean under `-Werror c_std=c23`.
- `meson test -C build` — all tests pass (crypto NIST vector + verifier length, config round-trip + 0600 mode, loopback end-to-end, cache store/list + chain anchor, image decode fixtures for PNG/JPEG/GIF/WebP + PNG round trip).
- `./build/sloptoot version` prints `sloptoot <version>`.
- `./build/sloptoot login <instance>` → browser opens, token stored, `whoami` works. Tested with Mastodon and GoToSocial.
- `./build/sloptoot toot "hello"` → post visible.
- `./build/sloptoot toot --reply` chains to the last post (after posting one); `--reply <id|url>` replies to a specific post.
- `./build/sloptoot toot` with `EDITOR` set → editor opens, text posts; untouched buffer cancels with exit 0.
- `./build/sloptoot ls`, `ls timeline`, `ls #tag` fetch and print (`-l`/`--long` wraps instead of truncating to the terminal width); `ls --mobile`, `ls --mobile timeline`, `ls --mobile '#tag'` serve the cached rows without network. In a kitty-capable terminal, `ls` renders the poster/reblogger avatar beside each row and image attachments below (aspect-preserved, capped to ~40% of the screen); `--mobile` renders the same layout from the local media cache (fetching any media not yet cached); in a non-supporting terminal or piped output it prints the plain text rows.
- `./build/sloptoot delete <id-or-url>` removes a post from the server and cache.
- `./build/sloptoot view <id-with-image>` in a kitty/ghostty/wezterm terminal renders PNG/JPEG/GIF/WebP attachments with the hybrid layout: side-by-side (text left / image right) in a ≥92-col terminal with a single image, otherwise stacked. In a non-supporting terminal or piped output it prints the plain `<type>`/alt-text listing instead. Animated GIFs/WebPs animate. `SLOPTOOT_FORCE_GFX=1 script -q log sh -c "./build/sloptoot view <id>"` captures the emitted APC sequences (`a=t`, `a=f`, `a=a`, `a=p`) for inspection.
- Force OOB fallback (loopback bind failure); confirm paste flow works.
- Smoke test on Linux: `xdg-open`, `/dev/urandom`, libcrypto link, `_GNU_SOURCE` visibility.
- Homebrew formula: `brew install jiqiren/tap/sloptoot` builds and installs; `brew test` and `brew audit --new --formula` pass.
