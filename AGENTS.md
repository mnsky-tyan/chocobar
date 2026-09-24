# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.

## Sharp edges

- A dead stdout sink (start-wizbar.vbs redirect) makes every `console.*`
  throw EPIPE, and Electron pops an "A JavaScript error occurred" dialog
  PER LINE - the app logs every scan, so the dialogs never stop until the
  pipe reader comes back. main.js swallows stream EPIPE; keep it.
- The native icons are a 1:1 port of `ICONS` in renderer/bar.js (viewBox 24,
  stroke-width 2.2, bow 2). Never hand-redraw them again: port the exact path
  data (rect/circle -> path syntax), and render through the 2x supersample
  pass in iconRenderGdip - 1:1 GDI+ AA reads blocky next to Chromium.
  Per-chip icon colors live in bar.css (#seg-tokens .ico = yellow, all
  others pinkDeep).
- pw_native.ps1-style PrintWindow captures of the LAYERED bar return the raw
  premultiplied DIB at LOGICAL size if the bitmap is not rect x2; analyze at
  x2 physical or every position reads wrong.

## Tests & checks

- `npm test` = `scripts/portable_regression.js` (portability layer, perf-critical pure logic,
  public-release default guarantees, and the native first-run template `g_template`
  decoded + parsed as JSONC; headless, any platform) + `scripts/pi_source_regression.js`
  (pi session-log source; synthetic fixture + raw-sum cross-check when a real
  `~/.pi/agent/sessions` exists) + `scripts/model_case_regression.js` (case-variant
  model grouping in aggregate(); synthetic, self-skips its optional live-store half).
  The real-store cross-check takes a stable
  snapshot (two agreeing raw walks around the scan) because a live pi session
  appends usage records while the test runs; it SKIPs if the store never quiets.
  pet_test.js (old tasklist-path E2E) was stripped in the v1.0.0 pass.
- Windows-side: `scripts/token_regression.js` (zcode+zai attribution; needs those stores),
  `scripts/cputemp_regression.js` (HWiNFO shm reader, Windows only).

## Cross-platform architecture (since the portability pass, retired Electron app)

Everything below describes the RETIRED Electron app. The shipped bar is one
mingw cross-compiled Windows exe (`native/`; build/run/verify in
`native/README.md`) and has no non-Windows path to degrade, so read these as
history, not as the product's current behavior.

Windows is primary; non-Windows must degrade gracefully, never fake data:

- `src/native.js` is the platform gate: koffi loads/bindings are guarded (`IS_WIN`,
  `loadLib`/`bind`/`kstruct`/`ksize` wrappers); with koffi absent or broken every binding
  is a stub returning 0 and all helpers degrade (null/false/[]/'no-section'). Portable
  readers live there too: battery + CPU temp via sysfs (`getBatteryLinux`, `getCpuTempLinux`).
- `src/tracker.js`: static mode off-Windows (STATIC_MODE) — no Win32 classes to follow;
  bar is a pill/strip (`bar.staticWidth`: 'content' default | 'workarea'); the
  `bar-content-size` IPC handshake lets the window shrink to the pill.
- Platform gates elsewhere: PowerShell GPU/Bluetooth workers + Core Audio volume +
  desktop pet + registry autostart are Windows-only (guarded in `src/metrics.js` /
  `main.js`); `src/tokens.js` probes `python3` when `python` is missing.
- Non-Windows acrylic does not exist: `themePayload` sends the tint SOLID off-Windows.

## Chocobar naming split (since the menu/dashboard pass)

User-visible strings (tray/context menu entries, window titles, error dialogs,
config template comments, README product name) say **Chocobar**; internal
identifiers stay `wizbar` on purpose (npm/package name, `~/.wizbar` APP_DIR,
`start-wizbar.vbs/.cmd` file names, the `[wizbar]` console log prefix, the
registry Run value, `window.wizbar` bridge). A portable test guards the
depersonalized defaults; don't "fix" the remaining wizbar strings.

## Terminal follow + subscription source

- Native bar chip semantics mirror the Electron renderer exactly: the token
  value = today's `input + output + cacheRead + cacheWrite` (Electron
  rowTotal, NOT cache-exclusive - do not "fix"), read from the Electron
  app's `~/.wizbar/token-cache.json` (v4) with a needle scan; keys may
  arrive mid-rewrite (retry full-size reads). Local midnight must go
  through LocalFileTimeToFileTime (SystemTimeToFileTime treats fields as
  UTC; HKT showed an exact 8h shift). GDI colors: hexToColorref returns a
  real COLORREF (raw 0xRRGGBB byte-swapped), and hand-written DIB pixels
  are DWORD = A<<24 | R<<16 | G<<8 | B. GDI text runs ~15% wider than
  browser metrics at the same nominal px - the bar scales the font by
  0.864 to match Electron's measured layout.
- Terminal targeting is terminal-agnostic: with `terminal.className: ""` the
  NATIVE bar probes four Win32 classes and nothing else - Windows Terminal
  (`CASCADIA_HOSTING_WINDOW_CLASS`), conhost (`ConsoleWindowClass`), ConEmu
  (`VirtualConsoleClass`), mintty - in that order; the authoritative list is
  `findTerminalByProbe` / `isTerminalHwnd` in `native/src/p_ui.c`, and a
  configured `className` is authoritative (no fallback to the list). The
  retired Electron tracker ALSO matched WezTerm / Alacritty / Hyper by owning
  process because their class is the generic winit/Electron one
  (`resolveProbe` + the AUTO_PROBE_* constants in `src/tracker.js`); the
  native build has no process probe, so such a terminal must be named in
  `terminal.className` by hand.
- The subscription plan-usage source is a file snapshot, not a session store:
  `tokens.sources.subscription.usagePath` points at user JSON
  (`{plans:[{name,total,used,resetsAt}]}`); re-read per rescan, invalid entries
  skipped, and the payload rides `tokens.aggregate().subscription` (null = hide
  the dashboard card). Gated by the tokens master switch like every source.
  **Retired Electron app only** - the native scan reads JSONL session stores, so
  a legacy `subscription` entry (no `sessionsDir`) is skipped with the rest of
  the legacy object conversion; the shipped plan-usage surface is
  `subs.providers[]` and the subscription board.

## Running on Linux (WSLg) — recipe

- Repo electron's binary needs system libs WSL lacks; use the nix-wrapped one:
  `nix run nixpkgs#electron -- . --no-sandbox --disable-gpu` (add
  `--extra-experimental-features 'nix-command flakes'`), with
  `LD_LIBRARY_PATH=<nix gcc.cc.lib>/lib64` so koffi's libstdc++ dep resolves.
  koffi is optional though — the app boots with the stub path too.
- Isolate the user home for test runs (`HOME=/tmp/...`) so `~/.wizbar` stays untouched.
- WSLg blocks screen capture (grim unsupported, xwd BadMatch). Verify windows instead via
  `xwininfo -root -tree` (window size/position proves static-mode placement).
- Sustained RAM/CPU method: sample `/proc/<pid>/stat` utime+stime + `status` VmRSS per
  electron process (main/renderer/gpu/utility) every 5s over minutes; compare like-for-like.

## Native rewrite (native/, phase 1)

- Native Win32 port of the bar lives in `native/` (PR #49). Build = `native/build.sh`
  (assembles the source parts into `src/chocobar_full.c`, then nix mingw
  cross-compiles). Edit the parts, never the assembled file. The render path
  is GDI + UpdateLayeredWindow on a WS_EX_LAYERED window (the earlier
  D2D-over-DComp pipeline is dead on this machine: TARGET-only bitmap options
  fail E_INVALIDARG per frame, TARGET|CANNOT_DRAW hits D2DERR_WRONG_STATE at
  EndDraw - do not resurrect). Layered windows never get WM_PAINT (first
  paint is explicit), DWM backdrops are ignored on them, and screen captures
  must use PrintWindow (CopyFromScreen races the follow loop). Sharp edges:
  `native/README.md` - read it before touching the render path.
- The phase-2 surface (token dashboards, subscription board, theme.icons)
  is DONE - the phase status is owned by `native/README.md` ("## Status");
  do not re-derive it here.
- The bar window MUST stay an OWNED window of the followed terminal
  (`CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED, ...)`, NO
  WS_EX_TOPMOST, plus `SetWindowLongPtrW(GWLP_HWNDPARENT, g_term)`) AND the
  follow tick must keep inserting it right after g_term
  (`SetWindowPos(g_bar, g_term, ...)`): an owned window rides the terminal's
  own band, so it stays out of the taskbar/Alt-Tab, hides with the terminal,
  and can never float over an unrelated window the way a topmost bar does -
  re-adding WS_EX_TOPMOST (or HWND_TOPMOST in the tick) re-breaks both
  halves. The pair must also stay ADJACENT in z: raising the terminal walks
  it over the bar, which then swallows every click/hover meant for it
  (WindowFromPoint proves it in one call) - the tick re-inserts on that
  drift. A hidden bar (terminal minimized) also explains "dead" hover -
  check IsWindowVisible first.
- Icons (p_icons.c) = ONE stroke color each (theme.iconColor, default
  pinkDeep), 24-unit paths flattened once, rendered with GDI+
  (SmoothingModeAntiAlias8x8, round caps/joins) into per-icon premultiplied
  DIB caches and AlphaBlended - GDI Polyline stroking has no AA and read
  wiggly. theme.iconOpacity rides AlphaBlend's SourceConstantAlpha (default
  90 = Electron's `.seg svg { opacity: .9 }`); the old per-box pixel fade
  is GONE (it double-faded). The gdip* facade + flattener serve the
  dashboards too (donut pies, AA cards). Flattener sharp edges: the M point
  must be stored as subpath vertex 0, and svgArc must sample from the START
  angle. Battery = dynamic fill (charge amount, warn <10%, full + zigzag on
  AC) drawn by svgDrawBatt into its own cache keyed by charge bucket.
- GDI+ flat-API binding: GetProcAddress EVERY function and bail to the GDI
  fallback if any is missing - `GdipCloseFigure` does not exist (the export
  is `GdipClosePathFigure`); a NULL binding crashes the first paint
  (C0000005 addr 0).
- Bar font = FW_NORMAL: the Meslo Nerd Font family ships only Regular+Bold,
  Chromium maps bar.css weight 600 to Regular, GDI rounds FW_SEMIBOLD to
  Bold (read too heavy). 700 -> FW_BOLD.
- Bar chrome parity: rounded corners are CSS (bar.css border-radius), drawn
  by fading the premultiplied tint per-pixel in the corner boxes (pxScale);
  DWM rounding does not apply to ULW surfaces. Hover pill = CLICKABLE chips
  only (align 2: shortcut/pet/tokens/subs/custom), pink at 50% blended over
  the tint, rounded 5px, inflated 5x2 CSS px; RHS metric segs get title
  tooltips instead (hand-rolled layered ChocobarTip window; TIP has
  WS_EX_TRANSPARENT so it never eats the click, and MUST be destroyed with
  the bar - a lingering topmost tip reads as a second bar ghost). Clicking
  empty bar raises the followed terminal. Token dashboard + subs board =
  WS_POPUP WS_EX_APPWINDOW "ChocobarDash" windows (DWM-rounded via
  DwmSetWindowAttribute 33), a 1:1 port of renderer/dash.css + subs.css:
  stat cards white25 over pinkBg, plan-usage rows, 26-week heatmap (cell
  radius 2.5 NOT the card 10, month labels, Sun/Fri rows, quantile
  thresholds, click = day detail), by-app + by-model tables (sorted by
  total desc, models capped 7, share bar pre-blended behind the first
  cell, cache columns only when data exists), subs = donut pies per window
  + status pills. Titlebar drag = HTCAPTION; refresh/✕ buttons drawn.
- Posted mouse coordinates to the PMv2 bar from a DPI-UNAWARE process get
  DOUBLED by Windows message-DPI translation: SetProcessDpiAwarenessContext(-4)
  in the poster, or halve the coords. (Cost a debug round twice.)
- Native config surface (all hot-reload): theme colors incl iconColor +
  iconOpacity + heatmap[5], bar.radius/backgroundAlpha/tint/font, dashboard
  + subs popup sizes, tokens.cachePath override + tokens.appFilter
  (harness allowlist for the chip/dash), terminal.className/title.
  The rest of the surface is the agnostic part and the README's Configuration
  section owns it: `tokens.sources[]` (any harness = one config entry),
  `subs.providers[].type: "generic"`, and the command-output custom chips
  (`intervalMs`/`format`/`warnAbove`/`warnBelow`). The first-run template
  (writeTemplate) ships every one of those keys with an inline annotation.
- Screenshot verification of Windows windows only works while the session is
  UNLOCKED; when locked, captures show the lock screen for every window.
  PW captures of the LAYERED bar return the raw premultiplied DIB (tint
  alpha 70% reads dark; alpha 0 reads black) - threshold accordingly.
- **The captain's live config (`~/.wizbar/config.json`, default path; also the
  `Edit config` menu target, resolved as `--config` > `WIZBAR_CONFIG` >
  `%USERPROFILE%\.wizbar\config.json`) is not a sandbox.** A no-mistakes test
  round once overwrote it with a 3-key stub and the captain lost the pet chip,
  the token scan and 2 of 4 subs panels until it was restored. The bar never
  WRITES this file (only an absent file gets the annotated template), so a
  read-only attribute is safe for it - but do NOT leave it read-only: the
  captain edits the config in Notepad with Ctrl+S, and a read-only file turns
  that into a Save-As dialog. Guard strategy that worked: keep
  `~/.wizbar-config-backup-good.json` outside the live home plus a 15s
  stub-signature watchdog (restore only when the file has no `tokens` AND no
  `modules` keys - the stub signature), so the captain's own edits are never
  reverted.

## Native config parser (sharp edges, all bit us once)

- jsmn object children are key+value PAIRS: any subtree walk must advance
  `1 + jtokSpan(value)`; `jtokSpan` (chocobar.c) counts object children as
  pairs. The old ad-hoc walk silently skipped top-level keys that follow a
  deeply nested sibling (general/terminal/tokens/subs were never parsed!).
- The first-run template (`g_template`) is a JSONC string whose braces must
  match or a fresh install silently comes up with a truncated root object -
  jsmn reports a token COUNT for the unbalanced tail, so loadConfig proceeds.
  One object per root key (a duplicate key is dead: `jobjGet` takes the first)
  and the template guard in `scripts/portable_regression.js` (section 4b) is
  the only check that catches it: it decodes the C string, strips the `//`
  comments in JS and calls `JSON.parse`, so it proves the template is valid
  JSONC and ships neutrally - it does NOT run jsmn or `parseConfigInto`, so
  the parser half of this note is verified only by a real bar run.
- The live config is a GENERATION behind `g_cfgCur` (`#define g_cfg (*g_cfgCur)`,
  chocobar.c): `loadConfig` installs a fresh generation and retires the old one
  instead of freeing it, because the provider fetch threads (a `SubsProvider*`
  held across a multi-second WinHTTP call) and the command-chip poll read it.
  A worker thread that walks the config must `cfgPin()` / `cfgUnpin()`; a reader
  with no pin is confined to the UI thread. `g_cfgCustomLock` (p_ui.c) covers
  only the published chip text, never the config.
- JSON booleans MUST go through `jboolDefault` - `jintTok` uses atoi and
  `atoi("true") == 0` (disabled every subs provider silently).
- **`tokens.sources` reads BOTH shapes.** The array form is one object per
  harness; the legacy OBJECT form (`{ "pi": { "sessionsDir": ... } }`) from a
  pre-array config is CONVERTED, not dropped (key -> `app`, `sessionsDir` ->
  `path`, `enabled` carries over) with one `native.log` line naming the
  conversion. Without that branch such a config yields zero sources with no
  word and the chip/dashboard silently keep serving the cache seed. A converted
  entry with no `sessionsDir` (the old zcode/opencode sqlite stores) is still
  skipped - as it always was - so those stores never scan. `enabled` defaults
  ON in both forms: adding an entry is the act of turning it on.
- MinGW `swprintf` follows C99: `%s` = char*, NOT wchar_t*. Every wide
  format needs `%ls` or the value truncates to its first byte
  ("Authorization: b" -> 401 token expired). Bit pet-kill, custom chips,
  the default config path and both subs auth headers.

## Release + install (since v1.1.0)

- Release = one exe, no installer: `git tag -a vX.Y.Z`, `bash native/build.sh`,
  `gh release create vX.Y.Z native/chocobar.exe`. The version lives ONLY in
  `native/src/version.h` - it feeds the startup log line (`writeLogA("chocobar "
  CB_VER_STR " start")`), the windres version resource, and the tag. Bump it
  there or the three disagree.
- `v1.0.0` is ALREADY TAKEN by the PR #11 integration point and was never
  released, so the first release is v1.1.0. Do not move a published tag.
- Installed location is `%USERPROFILE%\Chocobar\chocobar.exe`, autostart is the
  HKCU Run value `Chocobar` (update it when the exe moves - `general.autoStart`
  only writes it on a fresh install), and `~/.wizbar/runbar.ps1` launches the
  native bar. The two `personal.json` files are stale Electron-era display
  configs; the live config is `~/.wizbar/config.json`, which the bar reads by
  default with no `--config`.
- Config is never written next to the exe, so updating is a file swap and
  personal settings survive it. Say so in the release notes.
- `writeLogA` follows `%USERPROFILE%\.wizbar` (it used to hardcode the
  captain's home, which made a release build log nowhere on any other machine).
- **The parser's key list is the authority on what a config means** - see
  `native/src/chocobar.c`. `jintTok` means `bar.align` is 1=right / 2=left, so an
  Electron-era `"align": "right"` string is coerced by `atoi` to 0 and lands on
  1 (right) by luck. Keys the native build ignores, and that should be pruned
  from a live config rather than left to imply they work: `bar.position`,
  `bar.insetX`, `bar.segmentSpacing`, `bar.roundCorners`, `theme.yellowBg`,
  every `modules.*.intervalMs` plus `gpu.mode` / `volume.role`, all of
  `modules.bluetooth` and `modules.agents`, `terminal.reattachToExisting`,
  `tokens.showOnBar`, `tokens.heatmapWeeks`, and `tokens.sources.zcode` /
  `opencode` / `mimo` (only `zai` and `pi` are scanned).


## Verifying the dashboards' z-order on the live box

- `WindowFromPoint` and the `GW_HWNDPREV` walk both LIE when an unrelated window
  covers the probe point (an installer dialog once read as "the dash is behind
  the desktop", and the z-walk skipped the covering window entirely). The only
  trustworthy check is PIXELS: BitBlt the screen over a probe strip inside the
  dash and compare before/after. Pick the probe ADAPTIVELY - scan a grid inside
  the dash rect for a strip that already shows the board's own pinkBg
  (254,247,249) - so whatever else covers his screen cannot pollute the result.
  Then place the covering window over THAT strip, confirm the strip changed,
  and only then minimize/close it.
- The bar's chip rects MOVE between runs: the captain edits his live config, and
  a pet chip appearing or disappearing shifts every chip to its right by ~100px
  (pet 32..90, tokens 118..236, subs 264..343 with the pet on; the tokens chip
  starts at ~0 without it). Never hard-code click coordinates across runs - scan
  for the chip in the same script run, or drop a temporary `writeLogA` in
  WM_LBUTTONDOWN that prints every chip's `[type left..right]`; that is also the
  fastest way to prove a chip's hit area matches its ink (it does - a "pet chip
  opens the token board" scare was just the layout shifting under a stale
  coordinate).


## Config reference + ignored keys

- The complete user-facing config reference lives in README.md ("## Configuration"):
  every key the native parser reads, its default, and what editing it achieves,
  plus the full starting-point JSON. The in-app template (writeTemplate) stays
  the annotated first-run file; the README is the reference.
- Electron-era keys the native parser does NOT read (safe to delete, no effect
  when present): `bar.position`, `bar.insetX`, `bar.segmentSpacing`,
  `bar.roundCorners`, `modules.bluetooth`, `tokens.showOnBar`, `tokens.dashboard`,
  `tokens.heatmapDays`, `theme.surfaces`, `terminal.reattachToExisting`, and
  `tokens.sources.zcode` / `tokens.sources.opencode` (those two stores are
  SQLite and reach the board only through the `tokens.cachePath` seed). What the
  live scan covers is NOT a fixed list: it walks every entry of the
  `tokens.sources[]` array, so any harness the user declares is scanned.
  `bar.align` is 1=right, 2=left (there is no "center").
- `subs` supports 0-5 providers per the captain's ask (MAX_SUBS stays 6); one
  antigravity entry = one panel with two rows.

## Native subs chip (p_subs.c)

- Mirrors src/subs.js chip semantics: ChatGPT wham/usage (Bearer token from
  `~/.codex/auth.json`) + Z.ai quota/limit (apiKey from the ZCode config);
  lowest remaining window across PROVIDERS wins (per-provider rem/stale
  arrays - one provider failing must never stale the others, and the old
  global state did exactly that via a never-set `any` flag); chip states:
  em dash (no data), "stale", `N%` colored good/dim/warn at 70/30. The subs
  board reads the same per-provider window arrays (label/pct/used/total)
  - it asks PER PROVIDER, the chip asks the whole stack.
- **The chip readers (`subsChipRem` / `subsChipStale`) are scoped to declared
  AND enabled providers, and "stale" is a verdict on the CHIP, not per
  provider.** `g_subsProvRem` / `g_subsProvStale` are zero-initialised globals
  covering all MAX_SUBS slots, and a slot that is unused or declared-but-disabled
  is never written (subsThreadProc skips it), so it reads as a live provider
  sitting at 0% and drowns every real provider - hence the `subsProvEnabled`
  filter in both loops. Stale is set only when EVERY provider that has a value
  failed its last fetch (or nothing ever succeeded): a provider marked stale
  still has its last good windows on the board and still worth reporting, and a
  single timeout used to blank the chip to "stale" while the board showed fresh
  numbers. One provider failing must never stale the others.
- Generic (type 3) provider URLs go through `subsCrackUrl`, which is the only
  url decomposition in the file: the SCHEME alone decides TLS, a config
  `insecure` only AUTHORIZES the cleartext scheme (`http://` with `insecure`
  unset is refused, never sent with the token in the clear; `https://` with
  `insecure` set is still TLS - a downgraded https request would post the
  token to port 80), the host stops at the first `/` OR `:`, and an explicit
  `:port` is split out because `WinHttpConnect` wants a bare server name beside
  the port (leaving `:8443` inside the host breaks name resolution). The scheme
  match is case-insensitive, and scheme-less or `https:host` (no `//`) urls
  are rejected with a log line rather than silently assumed to be https.
  `subsHttpGet`/`subsHttpPost` take the port (0 = the scheme default); the two
  debug log lines format it with `snprintf` into `sizeof`-bounded buffers
  because `host` and the provider label are config-sized (the old `sprintf`
  overflowed a 160-byte stack buffer).
- `expectStatus` is GONE from the generic provider: `require` (a path that must
  exist) plus the default 2xx acceptance - with 401/403 called out first - cover
  every case a status list could express, and a per-provider status whitelist is
  one more key to forget. Do not re-add it.
- Generic capacities are ONE constant each and the README documents them:
  `windows[]` is `MAX_GEN_WIN` = **6** per provider and `auth` is
  `MAX_GEN_AUTH` = 3. The parser field, `g_subsWin[MAX_SUBS][MAX_GEN_WIN]`,
  `subsSetWins`' clamp and every reader's local array all size off the same
  constant - a literal 4 anywhere is a silent drop of a declared window. A
  generic auth `key` starting with `$` is a JSON path (a secret nested in the
  auth file is reachable as `$.auth.token`); any other key is one flat
  top-level key. An unrecognized `type` string logs a line naming it, because a
  typo otherwise silently becomes a chatgpt quota fetch.
- **The Antigravity source is `fetchAvailableModels` on BOTH Google endpoints
  merged with daily/sandbox OVERWRITING production, per family key priority -
  byte-for-byte the same source the harness's /quota uses** (pi-quota ->
  quota-axi -> pi-quota-inject.mjs). Production's `retrieveUserQuotaSummary`
  reports gemini as a constant rf=1 untracked pool and production/sandbox
  carry DIFFERENT quota figures - reading the summary endpoint was the
  "board 100% while /quota correct" bug. A quotaInfo may carry only a
  resetTime and NO remainingFraction (Claude/GPT between resets): keep the
  row, rem=-1 renders as an em dash ("reset-only pool"), and unknown rows are
  excluded from the CAPPED/lowest math. Never reintroduce a persistence layer
  or "local wins" policy without evidence - a fabricated cache file once fed
  the captain stale numbers for hours.
- **`agyFetchModels` launches one thread PER HOST and tracks that as a per-host
  mask, never a count.** A count (`mthN`) collapses "host 1's thread could not
  start" into "only one thread was started", so the sequential fallback then
  fetches host 0 a SECOND time inline and drops the started thread's `mj[0].resp`
  on the floor (a pure leak, and a doubled endpoint). The mask keeps each host's
  result - a host whose thread failed is exactly and only that host.
- The Z.ai gateway 200s with body `{code:401,msg:"token expired or
  incorrect"}` for a bad key and 200+`{code:500}` for missing identity
  headers - check the body `code`, not just HTTP status.
- Providers fetch CONCURRENTLY: `subsThreadProc` starts one thread per enabled
  provider (WinHTTP, AUTOMATIC_PROXY) and waits for them, because every fetch
  is independent and each writes only its own slot through the locked setters -
  a cycle costs the slowest provider, not the sum (5.9s -> 1.9s END TO END on
  the captain's box with three providers; the fanout step alone measured 2.6s,
  as the comment in `subsThreadProc` records). The UI timer only reads the
  latest state; HTTP failures log one line to native.log.
- Antigravity (type 2) reads the pi auth store `~/.pi/agent/auth.json` key
  `antigravity` ({access, refresh, expires(epoch MS), projectId}) and
  refreshes with Google's public desktop-client pair; grouped quota summary
  first, per-model `fetchAvailableModels` on 403 SUBSCRIPTION_REQUIRED
  (free tier - NOT an auth failure). ISO-8601 reset times go through
  `subsIsoToMs` (civil-days, no libc date code). The models body is ~150KB,
  so it parses through a grown token array (`subsParseBig`), never a fixed
  one. The refresh-token write-back is targeted text surgery inside the
  `"antigravity"` object and ABORTS on any doubt - a corrupted auth.json
  breaks the captain's whole toolchain, not just this bar.

## Antigravity local quota source (DEAD CODE since 2026-09-23)

- The local language-server path (subsFetchAgyLocal/subsAgyLocalApply +
  PEB/TCP-table discovery) is NO LONGER CALLED: the captain's ground truth is
  his /quota command, which never used the language server - it reads
  fetchAvailableModels over HTTPS with the pi auth token (works with the IDE
  closed). The local numbers (tiered 5h pools) DIVERGE from /quota, so letting
  them feed the board reintroduces the mismatch. The dead helpers are still
  compiled (excise in a dedicated pass); do not wire them back into the fetch
  chain. Historical notes below still hold for that machinery:
- The IDE's own /quota numbers come from its LOCAL language server, not the
  cloud: find `language_server*.exe`, read `--csrf_token` from its
  command line, POST `{}` to `http://127.0.0.1:<port>/exa.language_server_pb.
  LanguageServerService/GetUserStatus` with header `X-Codeium-Csrf-Token`.
  Process match is a case-insensitive PREFIX (`language_server`): the shipped IDE
  runs `language_server.exe` (from resources\bin), older builds
  `language_server_windows_x64.exe` - an exact-name match silently found 0
  candidates and fell back to cloud (caught by the pipeline's live test twice).
  Port/token are NOT persisted: the command line is read via
  NtQueryInformationProcess -> PEB -> ProcessParameters. PebBaseAddress and the
  CommandLine UNICODE_STRING offsets VARY per boot/build - every candidate is
  validated (page-aligned PEB, path-like decoded string); never hard-code one.
  Listening ports come from GetExtendedTcpTable(TCP_TABLE_OWNER_PID_LISTENER)
  (build.sh links iphlpapi). Prefer the non-daily endpoint instance.
- One panel per config entry shows TWO 5h rows - GEMINI 5H and CLAUDE/GPT 5H -
  the same two rows the IDE's /quota panel shows. No weekly row: Antigravity
  exposes no weekly quota (the cloud summary returns one; it never resets and
  reads stale - dropped). NOT one combined
  pie (reversed after the captain compared against /quota 2026-09-22): each
  family owns its own rolling 5h window with its own reset, so the binding
  constraint (chip) is the MIN remainingFraction across both families while the
  board shows both. subsAgyQuotaKey keeps the family split.
- The JSON endpoint port must be TRIED, not assumed: the language server owns
  several listeners (LSP/gRPC + the JSON one + the extension server) and which
  one serves GetUserStatus varies per boot (a 2026-09-22 boot answered 400 on
  the first listener, 200 with the real payload on the second). subsFetchAgyLocal
  now tries every listener of the chosen pid until a 200 with a parseable body.
- The Google desktop OAuth pair is NOT in the repo (removed 2026-09-22 after the
  captain refused to allowlist a public secret): subs.providers[].clientId /
  clientSecret carry it in the USER config, and only the cloud fallback needs
  it (the local language server needs none). History was scrubbed of the pair,
  so a fresh clone never trips push protection.
- tokens.labels ({ "pi": "pi-wsl" }) maps raw source keys to dashboard display
  names (parsed in chocobar.c, applied in p_ui.c's appLabelW). Aggregation keys,
  byte cursors and the appFilter keep the RAW key; only rendered row text swaps.

## Dev bar etiquette

- The native dev bar runs on its OWN shell: spawn a `wt` window titled
  `chocobar-dev` away from the captain's workspace, then launch the bar so
  it attaches to that window (sticky follow keeps it there). NEVER launch
  it on the captain's terminal: his Electron bar lives there too, and two
  always-on-top bars fight (his disappears under the dev bar). A closed or
  zombie dev shell leaves the bar hidden at -1000,-1000 400x26; recreate
  with `wt -w new nt --title chocobar-dev` (a dead shell can also linger as
  an offscreen 157x25 rect - EnumWindows finds it, GetWindowRect fails).
- `terminal.className` in config is AUTHORITATIVE: when set, the probe tries
  only that class and never falls back to the generic terminal class list.

## Never leave an Electron debug port open (cost the captain his bar once)

- 2026-09-21: an agent relaunched the app with `--remote-debugging-port=9222`
  to dump the renderer DOM and left it open. Another agent's browser
  automation (playwright) found the open port, attached to the FIRST page
  target - the bar - and `Page.navigate`d it to a GitHub issue. The bar then
  rendered a thin web page in its exact frame, which reads as "the bar
  became a website". CDP is browser tooling and will happily drive any port
  it finds; a status bar is the most exposed window on screen.
- Defense in depth now, THREE layers (one is not enough - verified the hard
  way): (1) `will-navigate`/`will-redirect` stop RENDERER-initiated navigations
  only; (2) a session `webRequest` handler refuses any non-file request from a
  renderer, which is the only thing that covers a browser-side CDP
  `Page.navigate`; (3) because a network-blocked request leaves the renderer
  on an error page WITHOUT firing any navigation event, a 1s URL poll reloads
  the window's own document if it is ever not its file (`_urlHealTimer` in
  src/bar.js, dies with the window). Verified live: a CDP navigation now
  fails ERR_BLOCKED_BY_CLIENT and the bar is back with all chips live within
  two seconds.
- `session.defaultSession` throws "Session can only be received when app is
  ready" at module load - register the handler via `app.isReady()` /
  `app.once('ready')`, never at require time.
- The REAL fix is operational: close the port when done. His canonical
  launch is `~/.wizbar/runbar.ps1` (personal.json, no debug port) - always
  end a debugging session by relaunching through it, and never leave
  `--remote-debugging-port` in a long-running instance. A stale instance also
  holds the single-instance lock, so a "relaunch" that silently quits is
  usually an old process still alive - kill the whole tree first.
- Restore recipe if it ever happens again: with the port up,
  `curl -s 127.0.0.1:9222/json/list`, find the target whose url is NOT
  `file:///.../bar.html`, and `Page.navigate` it back to
  `file:///C:/Users/tyanw/review/chocobar/renderer/bar.html`. Dependency-
  free CDP client recipe: raw `net` + `crypto` websocket handshake
  (Runtime.enable / Runtime.evaluate / Page.navigate). ALWAYS read the
  webSocketDebuggerUrl from the LIVE /json/list - a hardcoded/stale page id
  makes Page.navigate time out and reads as "the attack failed" when nothing
  was ever tested.

## Perf invariants (do not reintroduce)

- Stats push is ON-CHANGE **in the retired Electron app** (MetricsEngine `_dirty`
  + 250ms trailing loop in main.js): no fixed heartbeat, and `snapshot()` has no
  always-different `now` field. The bar/visibility gates run BEFORE
  `consumeDirty()` so changes observed while the bar is hidden stay pending and
  flush on restore. The shipped native bar has no MetricsEngine and no
  on-change push at all: it repaints from fixed timers (1s metrics, 100ms
  follow, plus the token and subs cycles), so this discipline is history - the
  native cost controls are those periods, not a dirty flag.
- Follow loop is ADAPTIVE **in the retired Electron app** (src/tracker.js
  `_scheduleFollow`): 8ms while the terminal is in a modal move/size (120Hz -
  drag latency is the only place it shows), 16ms for 500ms after a move, 100ms
  idle. It FOLLOWS LIVE through a drag (the old hands-off freeze made drags
  read as broken) and the zGluedTo sweep no longer skips mid-drag, so the bar
  keeps the pane's layer while it moves. Measured 2026-09-21: CPU 10.3% -> ~5%
  of one core vs the fixed 60Hz loop, RSS ~414 MB (Chromium-baseline dominated).
  The SHIPPED native bar does NOT use that tiered profile: a flat 100 ms
  `TIMER_FOLLOW` plus a foreground win-event hook, throttled to one sync per
  120 ms inside the terminal's modal move/size loop (`followTick`,
  native/src/p_ui.c).
- Baseline -> after (4-min Linux samples, 2026-09) - **the retired Electron
  app's four processes**: CPU 7.12% -> 1.93% of a core; RSS ~429 -> ~426 MB
  (Chromium-baseline dominated, flat by design). The shipped bar's figures are
  the ~30 MB / 4-5% of one core the top-level README quotes.
- Pet presence = in-process Toolhelp32 snapshot (`native.findProcessIdByName`
  in the retired app, `petRunning` in `native/src/p_metrics.c`), ~5ms/3s,
  never a tasklist.exe spawn (~164ms/spawn measured; ~290ms in older notes).
- The bar's heal interval must die with its window (see `BarWindow` closed/destroy) —
  it used to leak one 400ms timer per rebuild.

## Config surfaces (since the dashboard-config pass)

- `--config <path>` / `--config=<path>` / `WIZBAR_CONFIG` launch flag points the
  whole app at any config file (never auto-created; the default `~/.wizbar/config.json`
  still gets the annotated template on first run). Personal wiring = personal file;
  shipped defaults stay neutral.
- Personal display files live outside the repo (e.g. `~/.wizbar/personal.json`:
  pet name, token sources on, subs providers) — launched with
  `electron . --config ~/.wizbar/personal.json`. They carry the machine's real
  names and paths so the repo never has to. **Retired Electron app only**: the
  native bar takes the same `--config` / `WIZBAR_CONFIG` flag against
  `~/.wizbar/config.json`, and the two old `personal.json` files are stale
  display configs (see Release + install above).

## Public release (de-personalized)

Shipped defaults are neutral - nothing is read until the user turns a
source on. The native first-run template ships `tokens.enabled=false` with every
source off (example store paths, each disabled) and the pet chip and subs board
off; the retired app's JS defaults ship the master ON with every source off and
every path empty. No personal identifiers in repo code/config/docs. Personal
stores/pet wiring belongs only in the user-level `~/.wizbar/config.json`
(outside the repo). Dashboard shows an explanatory empty state (`sourcesEnabled`)
when nothing is configured. `tokens.enabled` is a true master switch:
off = zero scans, zero dashboard data, no chip (the dashboard says so via
`masterEnabled:false`); per-source flags decide which stores are read only when it is on
(regression: `scripts/portable_regression.js` section 4 for the JS defaults and
section 4b for the native template).

## Token usage stores (sharp edge)

Usage semantics differ by store. `src/tokens.js` is the retired Electron
app's authoritative reader; the SHIPPED native bar reads the same shapes in
`native/src/p_tokens.c` (its own needle scan + per-file byte cursor, fed by
whatever `tokens.sources[]` declares - not a hardcoded store pair):

- zcode CLI: `~/.zcode/cli/db/db.sqlite` `turn_usage` (via `scripts/zcode_query.py`).
  On some WSL filesystems a live `-wal` store rejects `mode=ro` mid-query
  ('disk I/O error'); the script falls back to an `immutable=1` snapshot
  (may miss uncheckpointed rows) instead of failing the whole scan.
- zai: per-message `usage` in `~/.zai/agent/sessions/*.jsonl` (flat; `ZCODE_sess_*` files
  are DB-backed legacy — never count them from disk too, they double-count).
- pi (new source): `~/.pi/agent/sessions/<project-slug>/*.jsonl` — same per-message
  `usage` records, nested per project; scanned by the shared `_scanPiSessions`
  (zai = 'zf:' keys/flat, pi = 'pf:' keys/nested, per-source mtime cursors).
  Timestamp shape: `message.timestamp` is epoch-ms int (verified 1402 real records);
  the ISO string lives on the line-level top-level `timestamp` the scanner never reads —
  don't 'fix' the `Number()` parse, it is correct (a review round was burned on this).
- opencode: assistant message `tokens` in the SQLite `message.data` JSON
  (dbPath preferred) or the legacy `msg_*.json` tree; a second WSL-distro
  store can be copied in read-only and counts as its own app
  (`opencode-wsl`, relabelable via `tokens.labels`).
- TOKEN CONVENTION: records are cache-EXCLUSIVE raw input/output; cache
  R/W are breakdown columns never added to totals. zcode DB input INCLUDES
  cache (proven by `computed_total_tokens`), so its scan subtracts; pi/zai/
  opencode/mimo input excludes cache and is stored raw. See the README
  'Token accounting' section. Cache version pinning: the stored record
  shape is `token-cache.json v4` — bump the version whenever record
  semantics change or stale values are silently kept.
- Session JSONL scans use per-file BYTE cursors (`_readSessionTail`, cache
  `v:4` with `progress`): warm scans read only appends (ms, not the old
  multi-second whole-file re-read that froze the bar every rescan; event-loop
  lag across rescans measured 0ms). Cursor shape, key stability (message id or
  absolute byte offset), and the cache version are one contract - change them
  together or a cold start re-reads everything. Cache writes are async+coalesced;
  `flushCacheSync` at quit lands the final cursors.
- Subscription board (`src/subs.js`, `subs.providers` in config): every provider fetch is
  bounded by `subs.fetchTimeoutMs` (clamped 3-60s; per-provider `timeoutMs` overrides),
  and a failed cycle keeps the provider's last good windows marked stale (status
  'stale' pill) instead of wiping the board. Disabled providers report status 'disabled'
  and the board renders no panel and no notes box for them. Tests inject a fake fetch
  (portable_regression section 10). Both shipped adapters verified live 2026-09-18:
  chatgpt wham/usage ~0.4-0.7s, zai quota/limit ~0.2-0.5s from this network.
- Bar chips: pinned order is shortcut (bolt, leftmost) - pet (bow, on/off toggle,
  optional `pet.label` prefix) - tokens (diamond) - subs (gauge, exactly right of
  tokens, rotates per enabled provider every minute, shows the week window
  remaining %). The subs chip renders only when `subs.enabled`.
- Dashboard surface: `tokens.labels` (harness display names), `tokens.dashboard`
  (section visibility toggles), `theme.heatmap` (5-shade ramp) are user config; the
  renderers treat absent keys as defaults-on. Nothing may pin a vendor name in UI.
- The experimental herdr agents-chip wiring was removed (no renderer ever drew it);
  `modules.agents` no longer exists in the config. History: commit b9c9f8d removed the
  chip, the 2026-09 public-release pass removed the polling/socket wiring.

## Tray icon (drawn, not stock)

- `makeBarIcon(px)` in p_ui.c builds a 32bpp alpha DIB (BITMAPV5HEADER +
  BI_BITFIELDS, top-down rows) wrapped with CreateIconIndirect: a pinkDeep
  rounded tile with the bar itself (a cream pill) centered, 1px feathered so
  it survives 16px on any taskbar theme. `LoadIconW(IDI_APPLICATION)` (the
  blank white window rectangle) reads as a broken app - never go back.
- SDF gotcha: clamp each half-distance at zero BEFORE the hypot, or a negative
  axis inflates the length and the side edges pinch inward (petal silhouette
  instead of a rounded square).

## Native bar cosmetics (captain pass, 2026-09-21)

- The bar window needs its own `WM_RBUTTONUP` -> `showTrayMenu`. The tray icon
  path (WM_TRAY) is the only one that existed, so right-clicking the BAR itself
  did nothing. The menu mirrors main.js `buildChocobarMenu` (token dashboard,
  subscription dashboard, edit config, open config folder, reload, quit).
- Bar edge padding is 16/18 CSS px (`padL`/`padR` in repaintBar), matching
  `#bar { padding: 0 18px 0 16px }`. The rounded corners need it too.
- Battery value color: `battAc ? good : (low ? warn : NULL)` - charging green
  OUTRANKS the low warning red (captain's call 2026-09-23: plugged in means
  the charge is rising, so red would be a lie). Red only when low AND on
  battery.
- Dashboard button labels (refresh/close) are CENTRED in their frames, both
  axes, measured with the same fBtn they are drawn with. The refresh frame
  stays sized for the word "refresh" (no reflow mid-click), so the busy
  "..." must be centred by advance width or it huddles at the left edge of
  the wide pill. Vertical: GDI's y is the line-box top, so centre the ink
  block (ascent+descent); the ellipsis is the exception - its ink is only
  the dots ON the baseline (~tmDescent/3 tall), so it centres the baseline
  plus half a dot. Verified live: both labels within 1px of frame centre.
- **Verifying the native bar without a screen**: CopyFromScreen dies the moment
  the session is locked (`OpenInputDesktop` returns 0 and you capture the lock
  screen - it reads as "the bar went black"), and PrintWindow on the LAYERED
  bar returns an all-black bitmap (a layered window renders through
  UpdateLayeredWindow, so it has nothing to paint into a DC). What DOES work:
  (a) `Get-Process LogonUI` tells you whether you are looking at a lock screen
  at all - check it BEFORE trusting any capture; (b) PrintWindow works on the
  NON-layered dash/subs popups (ChocobarDash), so dashboard changes are still
  visually verifiable; (c) the layered bar is verified by GEOMETRY
  (`GetWindowRect`, and `WindowFromPoint` for hit areas) or a `general.debug`
  log line, never by pixels - the full recipe is in native/README.md
  "Verifying on the machine". There is no chip-dump build flag: for the chip
  table, drop a temporary `writeLogA` in `chipClick` (p_ui.c) printing
  `g_chips[idx].r` and the chip type, then strip it again.
- Config gotchas found in his `~/.wizbar/config.json`: the shortcut module key
  was missing entirely (bolt chip silently absent) and the pet lived under
  `modules.remielle`, a name the parser no longer reads - it must be
  `modules.pet`. A missing/renamed module key disables that chip with no error,
  so when a chip is "gone", diff the config keys against the parser first.

## Native token live scan (p_tokens.c) - sharp edges

- **`tokens.sources[]` guard and storage must be the same number.** The loop
  admits `MAX_TOK_SRC` entries (8) and stores into `TokSource
  tokSrc[MAX_TOK_SRC]`- a literal smaller than the guard makes the 5th source
  overwrite `theme.icons[]`, which sits right behind it in `Config`, and the
  8th write runs past the whole heap generation (`loadConfig` installs a
  heap-allocated `Config`). When a config array grows a guard, grow the field
  with it in the same commit.
- **`tokKeysBuild` markers stop at the COLON** (`"model":`, 8 bytes), not at the
  value's opening quote as the Electron reader's 9-byte `"model":"` did. The
  extractor in `tokParseLine` must therefore step over any spaces/TABs and the
  opening `"` before scanning for the closing one - a marker-ending-at-colon
  left as-is stops on the very first byte and yields modelLen=0, which silently
  empties the dashboard's BY MODEL table (every record then fails `aggRecord`'s
  `model && mlen > 0` gate) while every other section keeps counting. The
  timestamp extractor right above has always skipped whitespace + quoted
  strings; the model extractor must do the same.
- **One bounded `~` expansion: `subsPathExpand`** (p_subs.c).
  Both `~`-relative config paths in the token scan call it (the source store and
  the cursor file). It refuses a `%USERPROFILE%` that does not fit
  (`!n || n >= MAX_PATH`) and truncates the remainder into the room that is
  left - inlining `GetEnvironmentVariableW` again would reintroduce the
  negative `lstrcpynW` count (GetEnvironmentVariableW returns the REQUIRED
  length and writes nothing when the buffer is too small, so `dir + n` is
  already past the array).
- **The app name is 19 chars, full stop.** `TokSource.app` is `char[20]` and
  the parse loop copies by `sizeof(ts->app) - 1` so it cannot drift again;
  `aggRecord` clamps `alen` to 19, and `g_appName[][]`,
  `tokensApps[][]` and `tokLabelKeys[][]` all hold 20 bytes. Every writer and
  every sink cap at the same place, so one key reaches the filter, the row and
  the `tokens.labels` lookup. A field sized differently (the old `char[24]`
  with `i2 < 23`) let a long config name be silently truncated into a different
  key on the way in.
- The omitted-`app` key is derived by `tokAppFromDir` (chocobar.c) from the
  EXPANDED path (subsPathExpand first, so a `~` config string is resolved),
  after trailing separators are stripped: the walk from the store end stops at
  the NEAREST dot-directory and strips its dot (`~/.claude/projects` ->
  `claude`, not the old fixed two-levels-up walk that yielded `~`). No
  dot-directory anywhere -> the store folder's own name. Both separators are
  accepted and doubled separators yield no component.
- The bar reads the Electron app's `~/.wizbar/token-cache.json` as the HISTORY
  SEED, then folds in everything newer from the live JSONL session stores -
  whichever ones `tokens.sources[]` declares (not a hardcoded pi/zai pair: a new
  harness is a config entry, and `recursive` decides whether that store is
  walked flat or per project) - via a per-file BYTE cursor in
  `~/.wizbar/token-cursors.json`. Only records with `ts > cacheMaxTs` are
  counted, so nothing is double counted. The Electron app is retired, so this
  is now the only thing keeping "Today" non-zero.
- **`tokLiveInit()` MUST run before `loadConfig()`** in wWinMain. loadConfig
  rebuilds the chips, which runs the FIRST token scan, and that scan is what
  populates the cursors; loading them afterwards wiped the in-memory set, so the
  very next rescan re-read every active file from byte 0 and DOUBLED every live
  record (today jumped 2x within a minute - "the token usage fluctuates").
  `tokLiveScan` also self-heals (`if (!g_tokCursorN) tokCursorLoad();`).
- There is ONE day-indexed histogram (`g_dayTot`/`g_dayApp`, p_ui.c,
  oldest-first, `[DASH_MAX_DAYS-1]` = today) and every displayed number is
  DERIVED from it per rescan (`tokDeriveWindows`): today = today's bucket,
  Last 7/30 = the in-window day sums, All time = every bucket. Never keep a
  second, cache-side base (`base + live`) or a mirrored live histogram: a base
  captured once is never re-derived as the window slides, so the cards
  permanently over-count the heatmap they sit above.
- The day buckets are aged by `dashDayRollover` alone (toward LOWER indices);
  the live scan indexes each record with a boundary array built fresh from the
  current local time, so it needs no shift of its own.
- **Opening one file over the WSL redirector costs ~25ms, and a
  `FindFirstFileW` per project dir ~25ms too.** A warm rescan that re-opens all
  21 active files costs ~550ms and blocks the bar. Skip a file when the cursor
  already covers it (`c->size == fsz && c->mtimeMs == mt`, both taken from the
  directory enumeration - never a separate `GetFileAttributesExW`), and read
  mtime+size from `WIN32_FIND_DATAW`, not a second stat.
- `tokens.rescanMinutes` was silently ignored (the tick was hard-coded to 30 =
  30s, twice what his config asks). It is now `tokensRescanSec`, clamped 1..60
  minutes. If a native config key seems to do nothing, grep the parser.
- Measured (2026-09-22, captain's box): CPU ~4.5% of one core, RSS ~27.6 MB,
  commit ~15.9 MB, 348 handles, 11 threads. The cold scan (cursor file absent)
  reads ~148MB over UNC and takes ~3.3s once.

## Native dashboards - layout sharp edges

- **Both dashboards are OWNED popups (owner = the bar, no WS_EX_TOOLWINDOW) and
  open with SetWindowPos(HWND_TOP | SWP_NOACTIVATE)**. Three requirements had to
  hold at once, and each one-line fix broke the next until the ownership was
  right:
    * `SetForegroundWindow` (original) raised the board but STOLE the keyboard
      from the followed terminal - he typed " like" and every keystroke landed
      on the dashboard (caught by a debug WM_KEYDOWN log).
    * `SW_SHOWNA` kept the focus but left the board SUNK behind his windows.
    * `HWND_TOP | SWP_NOACTIVATE` gives both halves, but ONLY while the board is
      a normal window: WS_EX_TOOLWINDOW (the "hide the taskbar icon" fix) makes
      Windows SKIP the dash when choosing the next window to activate, so the
      moment the window above it was minimized or closed, activation fell
      through to the terminal and Windows raised the TERMINAL over the board -
      the "dashboard sinks to the bottom layer" bug, documented in this repo's
      own history (main.js: the Electron dash is "A NORMAL window,
      deliberately", for exactly this reason).
  The fix is an OWNED popup. Owned windows get no taskbar button and no Alt-Tab
  entry (the captain's ask, now free of charge), always sit above their owner
  (bar above terminal, so the board cannot sink behind what it follows), and
  remain activation candidates, so Windows raises the board WITH the terminal
  instead of demoting it below it. Verified live: with a window placed over the
  board and then MINIMIZED, and again with one placed over it and then CLOSED,
  both boards are still painted at the probe point (the same probe showed the
  board gone - 6174 of 6240 pixels changed - before the change); the foreground
  window is unchanged across the open. A click on the board still activates it;
  Esc closes it.
- Tray menu metrics (captain's "-25%" pass): rows DX(21), separators DX(5), 9px
  labels, width from `menuWidthPx()` (widest label + DX(34), min DX(96)) - a
  fixed DX(210) was wider than its content. The autostart check draws at the
  RIGHT edge (`r.right - DX(18)..DX(6)`, DT_RIGHT); the label's right bound
  shrinks by DX(24) on that row only.
- Subs board must show EVERY enabled provider, 0 through 5: `en` is counted
  first, then the row (body -> footer -> head, in that order, with floors) is
  compressed until the whole stack fits the screen. **The stack is
  n*(panelH+gap) - gap**, so `maxPanels` must be
  `(avail + gap) / (panelH + gap)` - without the gap the 5th panel is lost to
  integer truncation right after the compression loop made it fit (cost a full
  debug round). With 0 providers the board still refits to its one empty-state
  line (it used to keep the previous board's height).
- Pie text is centred on the INK, not the glyph box: GDI puts the ink ~11 CSS px
  below the draw origin, so the number/caption offsets are 18.5 / 14.5 / 2.5
  (was 15/11/6, which left the number 3.5 CSS px low - measured off the
  captain's screenshot, +6.5 CSS for the number+caption pair).
- Probe gotchas: `FindWindowW("ChocobarDash", $null)` returns 0 - the window
  needs its EXACT title ("Chocobar dashboard" / "Chocobar subscriptions").
  A DPI-UNAWARE caller reads GetWindowRect HALVED on a 200% display (its screen
  is 1440x900), so a screen-capture script must either call
  SetProcessDpiAwarenessContext(-4) first or double the rect; PrintWindow on
  `#32768` menus intermittently returns an all-black bitmap (BitBlt the screen
  DC instead). The captain works with the board open, so a "toggle" click may
  CLOSE his board - poll-and-shoot instead of click-then-sleep.

## Native dashboards - layout sharp edges

- Both dash windows are created HIDDEN, painted once (`UpdateWindow`), content-
  fitted, and only then shown. Resizing a visible board reads as "bolted
  together" (the top appears, the lower part lags in). `dashFitToContent()`
  latches the window to `g_dashContentH` with a 45%-of-config-height floor.
- **`GetTextExtentPoint32W` ignores `SetTextCharacterExtra`.** Any label drawn
  with character extra must be measured with the same extra (`dashStrWEx`) or it
  loses its last character. This clipped "CLAUDE/GPT WEEK" to "CLAUDE/GPT WEE".
- The subs panel pie is sized from the WIDEST window label (`cellW - labNeed -
  DX(20)`), never by fixed tiers: a pie that takes the whole cell clips the
  label. Providers with many windows get a smaller pie, never a dropped window.
- Table name columns must end at the first numeric column (`xs[4] - DX(6)`),
  not a hard-coded width - a fixed `DX(150)` ellipsized real model ids
  ("xiaomi/mimo-x-flash-preview") even in a 500px card.
- The board footers must render a STORED wall-clock stamp, never
  `wallNow - truncatedTickAge`: mixing `GetSystemTimeAsFileTime` with a
  truncated `GetTickCount64` age made the seconds field oscillate (41 -> 42 ->
  41) on every repaint. The stamp is in dashFmtTime's frame (local fields
  reinterpreted as UTC), which is NOT a true UTC epoch - `subsNowMs()` would
  print 8 hours off in HKT.
- The heatmap cell is now adaptive (`(innerW - rowLabW)/weeks - gap`, capped
  DX(16)); 26 fixed DX(11) cells left the right half of the card blank.
- `tokens.enabled` off is a full reset, not a pause: the aggregates are
  cleared AND the on-disk byte cursors are deleted (`tokLiveReset`), so a
  re-enable does one clean full re-read (a cold scan of a few seconds) instead
  of resuming from cursors that silently skipped everything written while off.
