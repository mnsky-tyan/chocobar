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
  public-release default guarantees; headless, any platform) + `scripts/pi_source_regression.js`
  (pi session-log source; synthetic fixture + raw-sum cross-check when a real
  `~/.pi/agent/sessions` exists) + `scripts/model_case_regression.js` (case-variant
  model grouping in aggregate(); synthetic, self-skips its optional live-store half).
  The real-store cross-check takes a stable
  snapshot (two agreeing raw walks around the scan) because a live pi session
  appends usage records while the test runs; it SKIPs if the store never quiets.
  pet_test.js (old tasklist-path E2E) was stripped in the v1.0.0 pass.
- Windows-side: `scripts/token_regression.js` (zcode+zai attribution; needs those stores),
  `scripts/cputemp_regression.js` (HWiNFO shm reader, Windows only).

## Cross-platform architecture (since the portability pass)

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
- Non-Windows acryl­ic does not exist: `themePayload` sends the tint SOLID off-Windows.

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
- Terminal targeting is terminal-agnostic: `terminal.className: ""` (default)
  probes Windows Terminal / conhost / ConEmu / mintty by Win32 class, then
  WezTerm / Alacritty / Hyper by owning process (their class is the generic
  winit/Electron one). Authoritative list + order: `resolveProbe` and the
  AUTO_PROBE_* constants in `src/tracker.js`.
- The subscription plan-usage source is a file snapshot, not a session store:
  `tokens.sources.subscription.usagePath` points at user JSON
  (`{plans:[{name,total,used,resetsAt}]}`); re-read per rescan, invalid entries
  skipped, and the payload rides `tokens.aggregate().subscription` (null = hide
  the dashboard card). Gated by the tokens master switch like every source.

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
- Native bar is FEATURE-PARITY phase 2 (dashboards, subs, icons). The bar
  window MUST stay WS_EX_TOPMOST (Electron uses alwaysOnTop 'floating') AND
  the follow tick must SetWindowPos with HWND_TOPMOST: inserting the bar
  after a normal window (g_term) silently CLEARS the topmost bit and the
  raised terminal then swallows every click/hover meant for the bar
  (WindowFromPoint proves it in one call). Hidden bar (terminal minimized)
  also explains "dead" hover - check IsWindowVisible first.
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
  (harness allowlist for the chip/dash), subs providers/interval/timeout
  (which plans to check), modules toggles + warnAt, custom chips (label/
  icon/color/title/command/toggle), terminal.className/title. Template
  (writeTemplate) documents all of it.
- Screenshot verification of Windows windows only works while the session is
  UNLOCKED; when locked, captures show the lock screen for every window.
  PW captures of the LAYERED bar return the raw premultiplied DIB (tint
  alpha 70% reads dark; alpha 0 reads black) - threshold accordingly.

## Native config parser (sharp edges, all bit us once)

- jsmn object children are key+value PAIRS: any subtree walk must advance
  `1 + jtokSpan(value)`; `jtokSpan` (chocobar.c) counts object children as
  pairs. The old ad-hoc walk silently skipped top-level keys that follow a
  deeply nested sibling (general/terminal/tokens/subs were never parsed!).
- JSON booleans MUST go through `jboolDefault` - `jintTok` uses atoi and
  `atoi("true") == 0` (disabled every subs provider silently).
- MinGW `swprintf` follows C99: `%s` = char*, NOT wchar_t*. Every wide
  format needs `%ls` or the value truncates to its first byte
  ("Authorization: b" -> 401 token expired). Bit pet-kill, custom chips,
  the default config path and both subs auth headers.

## Native subs chip (p_subs.c)

- Mirrors src/subs.js chip semantics: ChatGPT wham/usage (Bearer token from
  `~/.codex/auth.json`) + Z.ai quota/limit (apiKey from the ZCode config);
  lowest remaining window across PROVIDERS wins (per-provider rem/stale
  arrays - one provider failing must never stale the others, and the old
  global state did exactly that via a never-set `any` flag); chip states:
  em dash (no data), "stale", `N%` colored good/dim/warn at 70/30. The subs
  board reads the same per-provider window arrays (label/pct/used/total).
- The Z.ai gateway 200s with body `{code:401,msg:"token expired or
  incorrect"}` for a bad key and 200+`{code:500}` for missing identity
  headers - check the body `code`, not just HTTP status.
- Fetches run on a worker thread (WinHTTP, AUTOMATIC_PROXY); the UI timer
  only reads the latest state. HTTP failures log one line to native.log.
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
- Defense in depth now: every app window refuses `will-navigate` /
  `will-redirect` and denies `window.open` (`lockWindowNavigation` in
  main.js, `lockNav` in src/bar.js) - verified live, both a CDP
  `Page.navigate` and a renderer `window.location=` are refused.
- The REAL fix is operational: close the port when done. His canonical
  launch is `~/.wizbar/runbar.ps1` (personal.json, no debug port) - always
  end a debugging session by relaunching through it, and never leave
  `--remote-debugging-port` in a long-running instance.
- Restore recipe if it ever happens again: with the port up,
  `curl -s 127.0.0.1:9222/json/list`, find the target whose url is NOT
  `file:///.../bar.html`, and `Page.navigate` it back to
  `file:///C:/Users/tyanw/review/chocobar/renderer/bar.html`. Dependency-
  free CDP client recipe: raw `net` + `crypto` websocket handshake
  (Runtime.enable / Runtime.evaluate / Page.navigate).

## Perf invariants (do not reintroduce)

- Stats push is ON-CHANGE (MetricsEngine `_dirty` + 250ms trailing loop in main.js);
  no fixed heartbeat, and `snapshot()` has no always-different `now` field. The bar/
  visibility gates run BEFORE `consumeDirty()` so changes observed while the bar is
  hidden stay pending and flush on restore.
- Follow loop is ADAPTIVE (src/tracker.js `_scheduleFollow`): 8ms while the
  terminal is in a modal move/size (120Hz - drag latency is the only place it
  shows), 16ms for 500ms after a move, 100ms idle. It FOLLOWS LIVE through a
  drag (the old hands-off freeze made drags read as broken) and the zGluedTo
  sweep no longer skips mid-drag, so the bar keeps the pane's layer while it
  moves. Measured 2026-09-21: CPU 10.3% -> ~5% of one core vs the fixed 60Hz
  loop, RSS ~414 MB (Chromium-baseline dominated).
- Baseline -> after (4-min Linux samples, 2026-09): CPU 7.12% -> 1.93% of a core;
  RSS ~429 -> ~426 MB (Chromium-baseline dominated, flat by design).
- Pet presence = in-process Toolhelp32 snapshot (`native.findProcessIdByName`, ~5ms/3s),
  never a tasklist.exe spawn (~164ms/spawn measured; ~290ms in older notes).
- The bar's heal interval must die with its window (see `BarWindow` closed/destroy) —
  it used to leak one 400ms timer per rebuild.
- Baseline -> after (4-min Linux samples, 2026-09): CPU 7.12% -> 1.93% of a core;
  RSS ~429 -> ~426 MB (Chromium-baseline dominated, flat by design).

## Config surfaces (since the dashboard-config pass)

- `--config <path>` / `--config=<path>` / `WIZBAR_CONFIG` launch flag points the
  whole app at any config file (never auto-created; the default `~/.wizbar/config.json`
  still gets the annotated template on first run). Personal wiring = personal file;
  shipped defaults stay neutral.
- Personal display files live outside the repo (e.g. `~/.wizbar/personal.json`:
  pet name, token sources on, subs providers) — launched with
  `electron . --config ~/.wizbar/personal.json`. They carry the machine's real
  names and paths so the repo never has to.

## Public release (de-personalized)

Shipped defaults are neutral: `tokens.enabled=false` with all sources off and empty paths,
pet chip off, no personal identifiers in repo code/config/docs (a portable test
guards this). Personal stores/pet wiring belongs only in the user-level
`~/.wizbar/config.json` (outside the repo). Dashboard shows an explanatory empty state
(`sourcesEnabled`) when nothing is configured. `tokens.enabled` is a true master switch:
off = zero scans, zero dashboard data, no chip (the dashboard says so via
`masterEnabled:false`); per-source flags decide which stores are read only when it is on
(regression: the master-switch block in `scripts/portable_regression.js`).

## Token usage stores (sharp edge)

Usage semantics differ by store; `src/tokens.js` is the authoritative reader:

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
