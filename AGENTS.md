# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.

## Tests & checks

- `npm test` = `scripts/portable_regression.js` (portability layer, perf-critical pure logic,
  public-release default guarantees; headless, any platform) + `scripts/pi_source_regression.js`
  (pi session-log source; synthetic fixture + raw-sum cross-check when a real
  `~/.pi/agent/sessions` exists). The real-store cross-check takes a stable
  snapshot (two agreeing raw walks around the scan) because a live pi session
  appends usage records while the test runs; it SKIPs if the store never quiets.
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

## Perf invariants (do not reintroduce)

- Stats push is ON-CHANGE (MetricsEngine `_dirty` + 250ms trailing loop in main.js);
  no fixed heartbeat, and `snapshot()` has no always-different `now` field. The bar/
  visibility gates run BEFORE `consumeDirty()` so changes observed while the bar is
  hidden stay pending and flush on restore.
- Follow loop is 16ms (60Hz) and the unchanged-bounds fast path touches nothing
  (no isVisible()/assertNoTaskbar per tick); `assertNoTaskbar` has a 2s re-assert floor.
- Pet presence = in-process Toolhelp32 snapshot (`native.findProcessIdByName`, ~5ms/3s),
  never a tasklist.exe spawn (~164ms/spawn measured; ~290ms in older notes).
- The bar's heal interval must die with its window (see `BarWindow` closed/destroy) —
  it used to leak one 400ms timer per rebuild.
- Baseline -> after (4-min Linux samples, 2026-09): CPU 7.12% -> 1.93% of a core;
  RSS ~429 -> ~426 MB (Chromium-baseline dominated, flat by design).

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
- zai: per-message `usage` in `~/.zai/agent/sessions/*.jsonl` (flat; `ZCODE_sess_*` files
  are DB-backed legacy — never count them from disk too, they double-count).
- pi (new source): `~/.pi/agent/sessions/<project-slug>/*.jsonl` — same per-message
  `usage` records, nested per project; scanned by the shared `_scanPiSessions`
  (zai = 'zf:' keys/flat, pi = 'pf:' keys/nested, per-source mtime cursors).
  Timestamp shape: `message.timestamp` is epoch-ms int (verified 1402 real records);
  the ISO string lives on the line-level top-level `timestamp` the scanner never reads —
  don't 'fix' the `Number()` parse, it is correct (a review round was burned on this).
- opencode: cache BESIDE input; mimo: input EXCLUDES cache. All scans fold cache into
  stored input so aggregate() totals stay input+output (zcode DB input already includes
  cache). Contract checks: `npm test` + `scripts/token_regression.js`.
- The experimental herdr agents-chip wiring was removed (no renderer ever drew it);
  `modules.agents` no longer exists in the config. History: commit b9c9f8d removed the
  chip, the 2026-09 public-release pass removed the polling/socket wiring.
