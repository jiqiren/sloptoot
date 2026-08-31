# sloptoot

A minimal command-line Mastodon client for sending quick toots, written in pure C23. Targets macOS and Linux.

## Install

```sh
brew install jiqiren/tap/sloptoot
```

Then run `sloptoot login <instance>` to get started.

## What it does

- `sloptoot login <instance>` — run the Mastodon OAuth login dance and store a Bearer token.
- `sloptoot toot "<text>"` — post a new public status.
- `sloptoot toot` — compose in `$EDITOR` (leaving the buffer empty cancels the post).
- `sloptoot toot --reply` — reply to your most recent post (thread chaining).
- `sloptoot toot --reply <id-or-url>` — reply to a specific status.
- `sloptoot ls [profile|timeline|#tag]` — list your posts, your home timeline, or hashtag posts. Default output truncates to your terminal width; `-l`/`--long` prints full (wrapped) text.
- `sloptoot ls --mobile` — serve the last listing from the local cache instead of hitting the network.
- `sloptoot delete <id-or-url>` — remove one of your posts (server and local cache).
- `sloptoot view <id-or-url>` — show a detailed view of a post (author, text, date, boost/like/reply counts). Image attachments (PNG/JPEG/GIF/WebP) are rendered inline when your terminal supports the [kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol.html); elsewhere they fall back to a text-only listing. Animated GIFs/WebPs animate in place.
- `sloptoot boost <id-or-url>` — boost (reblog) a post.
- `sloptoot like <id-or-url>` — favourite a post.
- `sloptoot bookmark <id-or-url>` — bookmark a post.
- `sloptoot whoami` — show the logged-in account handle.
- `sloptoot version` — show version (`--version` / `-V` also work).
- `sloptoot help` — print usage.

The app registers itself with Mastodon as `slop TooT` (exact spelling intentional).

## Quick start

```sh
brew install jiqiren/tap/sloptoot
sloptoot login fosstodon.org
# browser opens → authorize → "Logged in as @you@fosstodon.org"

sloptoot toot "hello from sloptoot"
# https://fosstodon.org/@you/...

sloptoot ls
# 20268888408374051  2h  hello from sloptoot
# 50  3m  🚀 @bob
#   ⎣  [10] 2h  original art post that got boosted
# (id, relative time, then body; HTML stripped; boosts shown specially)

sloptoot toot "a follow-up reply" --reply 20268888408374051
# posts a reply to that specific post
```

Works with both Mastodon and GoToSocial instances.

## Login flow

`login` uses PKCE (`S256`) with a loopback HTTP server by default:

1. Binds a local server on `127.0.0.1:<ephemeral port>/callback`.
2. Registers an app with the instance, listing the exact bound callback URI (plus `urn:ietf:wg:oauth:2.0:oob` as a fallback).
3. Generates a PKCE `code_verifier` (32 bytes from `/dev/urandom`, base64url-encoded) and `code_challenge = base64url(SHA-256(verifier))`.
4. Opens your browser to the instance's `/oauth/authorize` endpoint with the challenge and a random `state`.
5. Waits for the instance to redirect back with `?code=...&state=...`, validates `state`, exchanges the code for an access token, and verifies via `/api/v1/accounts/verify_credentials`.
6. Saves credentials to `~/.config/sloptoot/config` (mode 0600).

If the loopback server can't bind, it falls back to the OOB flow: the instance displays a code on a page and you paste it into the CLI.

Set `SLOPTOOT_DEBUG=1` to have the loopback server print the raw callback request to stderr — useful for diagnosing OAuth errors.

## Build from source

Requires Meson, pkg-config, libcurl, SQLite (`sqlite3`), and the image libraries `libpng`, `jpeg-turbo`, `libnsgif`, and `webp` (+ `webpdemux`), plus (on Linux) libcrypto/openssl dev headers. cJSON is fetched automatically via a Meson subproject wrap (or use a system-installed `cjson`).

### macOS (Homebrew)

```sh
brew install meson pkg-config curl sqlite libpng jpeg-turbo libnsgif webp
meson setup build && meson compile -C build
```

### Linux (Debian/Ubuntu)

```sh
sudo apt install meson pkg-config libcurl4-openssl-dev libssl-dev libsqlite3-dev \
  libpng-dev libjpeg-turbo8-dev libnsgif-dev libwebp-dev
meson setup build && meson compile -C build
```

The build uses `c_std=c23`, `warning_level=2`, and `werror=true` — it must compile cleanly under `-Wall -Wextra -Werror`.

### Config

Credentials live at `$XDG_CONFIG_HOME/sloptoot/config` (or `~/.config/sloptoot/config` if `XDG_CONFIG_HOME` is unset), written with mode `0600`. The file is a flat `key=value` list of `instance`, `client_id`, `client_secret`, `access_token`, `account_id`, `username`.

### Cache

Fetched listings live in a SQLite database at `$XDG_CONFIG_HOME/sloptoot/cache.db` (or `~/.config/sloptoot/cache.db`). It holds post metadata only (never credentials) and remembers your last post so `toot --reply` can chain threads.

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
│   ├── main.c            CLI dispatch
│   ├── version.h.in      version template (Meson generates version.h)
│   ├── oauth.{c,h}       app registration, PKCE, token exchange, login
│   ├── toot.{c,h}        post a status, replies, delete, chain anchor
│   ├── timeline.{c,h}    ls command (profile / home / hashtag)
│   ├── cache.{c,h}       SQLite status cache
│   ├── edit.{c,h}        $EDITOR compose flow
│   ├── view.{c,h}        view command (detailed single-status view)
│   ├── image.{c,h}       image decode (JPEG/PNG/GIF/WebP -> RGBA) + PNG encode
│   ├── kitty.{c,h}       kitty graphics protocol (transmit/place/animate)
│   ├── http.{c,h}        libcurl helpers (form/json/get, urlencode)
│   ├── loopback.{c,h}    127.0.0.1 callback server
│   ├── config.{c,h}      XDG config load/save (0600)
│   ├── browser.{c,h}    open / xdg-open
│   ├── base64.{c,h}     base64url + PKCE verifier
│   ├── json_helpers.{c,h}  cJSON wrappers
│   └── sha256.{h,apple.c,posix.c}  per-platform SHA-256 shim
├── tests/
│   ├── crypto_test.c
│   ├── config_test.c
│   ├── loopback_test.c
│   ├── cache_test.c
│   └── image_test.c     decodes real PNG/JPEG/GIF/WebP fixtures
├── Spec.md
├── AGENTS.md
├── README.md
└── LICENSE
```

## Running tests

```sh
meson test -C build
```

## Dependencies

- [libcurl](https://curl.se/) — HTTP/TLS, via pkg-config.
- [cJSON](https://github.com/DaveGamble/cJSON) — JSON, via Meson wrap (auto-fetched) or system `cjson` formula.
- [SQLite](https://www.sqlite.org/) — status cache, via `dependency('sqlite3')` (Homebrew `sqlite` keg on macOS).
- [libpng](http://www.libpng.org/) — PNG decode + re-encode (RGBA), via `dependency('libpng')`.
- [libjpeg-turbo](https://libjpeg-turbo.org/) — JPEG decode, via `dependency('libjpeg')`.
- [libnsgif](https://www.netsurf-browser.org/projects/libnsgif/) — animated GIF decode, via `dependency('libnsgif')`.
- [libwebp](https://developers.google.com/speed/webp/) — WebP decode + WebPDemux animation, via `dependency('libwebp')`/`dependency('libwebpdemux')`.
- SHA-256: CommonCrypto on macOS, libcrypto (`SHA256()`) on Linux. No vendored crypto.
- Random bytes: `/dev/urandom`.

## Viewing images

`sloptoot view` renders image attachments inline using the kitty graphics protocol (works in kitty, Ghostty, WezTerm, Konsole, iTerm2, and other terminals that implement it). It probes the terminal once per run; if the terminal doesn't answer (Apple Terminal, tmux without passthrough, piped output, `TERM=dumb`), it automatically falls back to the plain text attachment listing. `SLOPTOOT_FORCE_GFX=1` skips the probe so the control sequences can be captured for debugging (e.g. under `script`).

The view uses a hybrid layout so the whole post fits one screen: in a wide (≥92 col) terminal with a single image, the text sits on the left and the image is pinned to the top-right (the split adapts to the image's aspect ratio so it fills the terminal height); otherwise the image is stacked under the text. Images always keep their aspect ratio, and the shell prompt lands below the whole view.
