# Todo — sloptoot

Current version: 1.4.5 (release; media cache + offline `ls --mobile` graphics).

## Completed
- [x] Project scaffolding (meson.build, cjson.wrap, libcurl, cJSON, platform sha256, stub main.c)
- [x] Crypto + base64 helpers (sha256 shim, base64url, random verifier, NIST test vector)
- [x] HTTP layer (libcurl: post_form, get, post_json, urlencode)
- [x] JSON helpers (cJSON wrappers: parse, get_string, get_int)
- [x] Config storage (XDG path, load/save key=value, 0600 mode)
- [x] Browser launch (open / xdg-open)
- [x] Loopback HTTP server (127.0.0.1:0, state validation, callback capture)
- [x] OAuth flow (register_app, PKCE, authorize URL, token exchange, verify_credentials, login orchestrator)
- [x] CLI dispatch (login / toot / whoami / version / help, exit codes)
- [x] Toot posting (POST /api/v1/statuses, print URL)
- [x] Build & verification (clean -Werror c23 build, tests pass, smoke tests)
- [x] Polish (README, .gitignore, Spec/AGENTS review)
- [x] GoToSocial compatibility (newline-joined redirect_uris)
- [x] Version flag (--version / -V / version)
- [x] Linux build fix (_GNU_SOURCE for POSIX functions)
- [x] Meson install support (install : true)
- [x] Homebrew tap (jiqiren/homebrew-tap, formula, bottles)
- [x] Thread chaining: `toot --reply/-r` (bare = last post, `--reply <id|url>` = specific) with SQLite chain anchor
- [x] `ls`/`--ls`/`-l` subcommand — profile / timeline / #hashtag listings
- [x] `--mobile`/`-m` — serve `ls` from the SQLite cache, no network; with no type, uses the last loaded timeline (falls back to `profile`)
- [x] Editor compose: bare `toot` opens `$EDITOR`; empty temp file cancels (exit 0)
- [x] SQLite status cache (dedup statuses per timeline type, last_post_id meta)
- [x] `ls` output shows `id  relative-time  body` so listed posts can be replied to (`-r <id>`) or deleted (`delete <id>`)
- [x] Boost display in `ls`: boost id + rel time + 🚀 + booster handle, then indented `⎣` line with the original post's id/rel time/text
- [x] UTF-8-aware `ls` truncation: codepoint decoding, wide (CJK/emoji) display width, combining marks, no mid-sequence split, `…` ellipsis
- [x] `ls` pages (max_id/limit=40) until it fills the terminal height − 2 rows, then prints only that screenful
- [x] `ls` screen budget counted in display lines, so boosts (2 lines each) are trimmed correctly and never overflow
- [x] Fix use-after-free in `ls` pagination (`max_id` copied before the page is freed)
- [x] Removed `--ls`/`-l` aliases for the `ls` command (only `ls`)
- [x] `delete <id-or-url>` — remove a post from the server and cache (clears chain anchor)
- [x] `view <id-or-url>` — detailed single-status view (author, text, date, boost/like/reply counts)
- [x] `boost`/`like`/`bookmark` status actions (POST /api/v1/statuses/{id}/{reblog,favourite,bookmark})
- [x] `view` of a boost shows the boost line (booster, when) then the underlying post's detail
- [x] Status actions and replies on a boost id unwrap to the underlying `reblog` post (`resolve_status_id`)
- [x] Status refs accept Mastodon numeric snowflakes and GoToSocial ULIDs (and URLs)
- [x] Inline image viewing in `view` via the kitty graphics protocol (PNG/JPEG/GIF/WebP, animated GIFs/WebPs, terminal probe + graceful text fallback, `SLOPTOOT_FORCE_GFX=1`) — deterministic check via `SLOPTOOT_FORCE_GFX=1 script q log · …` captures the `a=t`/`a=f`/`a=a`/`a=p` sequences.
- [x] Hybrid `view` layout to maximize the terminal: side-by-side (text left / image pinned top-right, adaptive split) in ≥92-col terminals with a single image, otherwise images stacked under the text; text pane is display-width word-wrapped, stacked images share the leftover rows by aspect ratio, and every placement preserves the image's aspect ratio (cell-size-aware). After the side placement the cursor is parked below the taller column so the prompt doesn't overwrite the text.
- [x] `ls` timeline/hashtag inline images: poster/reblogger avatar rendered beside each row (aspect-preserving, two rows tall in a reserved gutter, `C=1` so the cursor never moves) plus image attachments stacked below the post, capped at ~40% of the terminal height; the listing stops when the screen is full, and non-kitty terminals / `--mobile` fall back to the plain text rows.

## Backlog (out of scope for v1)
- [ ] Media uploads / attachments (`-i/--include`) — deferred (ALT-text concerns)
- [ ] Streaming / notifications
- [ ] Multiple accounts