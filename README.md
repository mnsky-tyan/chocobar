# Chocobar

A slim acrylic system-status bar that floats **above your terminal window**, plus a
cross-CLI **token usage tracker** (zcode · zai · pi · opencode · Xiaomi MiMo AI · subscription plans) with a GitHub-style
heatmap.

Built to match your terminal theme: pale-yellow acrylic (`#F5F0D8` / `#FDEFF2`), pink accents
(`#E8C7D0`), MesloLGLDZ Nerd Font.

```
        ┌────────────────────────────────────────────────────────────────────┐
        │                       ◇ 235.7M  ⬡23% ⌗18% ▤60% 🔇0% 🔋65% Sep 09 20:04 │   <- Chocobar (24px, follows width)
        └────────────────────────────────────────────────────────────────────┘
        ┌────────────────────────────────────────────────────────────────────┐
        │  − □ ✕   PowerShell                                                │   <- Windows Terminal
        │  PS C:\Users\you>                                                │
        └────────────────────────────────────────────────────────────────────┘
```

## What it does

**Bar** (thin strip above the terminal, right-aligned modules):
- today's tokens (click it → dashboard) · desktop-pet toggle (private module, off by default — see Config)
- GPU % · CPU % · CPU temp · RAM % · volume % · battery %
- Bluetooth device battery (earbuds — shows `87·85` for L/R when the device reports it)
- clock

## Platform support

Windows is the primary platform and gets the full feature set. The app also **runs on
Linux/macOS with graceful degradation**: every Windows-only capability is detected at
runtime and its chip simply shows `—` instead of crashing or faking data.

| Feature | Windows | Linux / macOS |
|---|---|---|
| Bar window, themes, clock, token usage dashboard | ✅ | ✅ |
| CPU % / RAM % | ✅ | ✅ |
| Battery | Win32 `GetSystemPowerStatus` | ✅ Linux sysfs (`/sys/class/power_supply`); macOS shows `—` |
| CPU temperature | HWiNFO shared memory | ✅ sysfs hwmon / thermal zones (if sensors exist) |
| Follows the terminal window (attach/move/minimize/z-order) | ✅ Win32 window tracking | — (no equivalent API): Linux can't measure the terminal, so the bar becomes a **corner pill** by default; `bar.staticWidth: "workarea"` gives a full-width top strip instead |
| GPU % (GPU Engine counters) | ✅ | — (chip shows `—`) |
| Volume (Core Audio master level) | ✅ | — (chip shows `—`) |
| Bluetooth device battery (PnP property) | ✅ | — (chip shows `—`) |
| Acrylic backdrop | ✅ translucent tint, alpha 0–255 over a transparent window | tint renders solid (alpha over arbitrary wallpapers reads murky); same colors |
| Desktop-pet toggle | private module, opt-in | Windows executables only — stays off |
| Autostart at login | ✅ HKCU Run entry | — (no-op; use your desktop environment's autostart) |
| Token sources (zcode / zai / pi / opencode / mimo / subscription) | ✅ | ✅ (paths are user config; mimo auto-discovers only on Windows) |

Use `terminal.reattachToExisting` / `terminal.className` only on Windows; they are ignored
elsewhere.

**Follow semantics** (exactly as specified):
- The bar attaches to the **first** terminal window it sees (frontmost when Chocobar starts).
- **Terminal-agnostic by default**: with `terminal.className` left empty it auto-detects
  what you actually run — Windows Terminal, classic conhost (cmd/PowerShell console),
  ConEmu and mintty by window class, then WezTerm, Alacritty and Hyper by owning process
  (their window class is the generic winit/Electron one). The first candidate with a real
  window wins; set `terminal.className` to a Win32 class to pin one specific terminal.
- It follows only that window: move, resize (width), minimize (bar hides), restore (bar returns).
- Opening more terminal windows does **not** retrigger it.
- When the followed window closes, the bar hides and waits for the **next newly opened**
  terminal window (existing other windows are ignored). Set `terminal.reattachToExisting:
  true` to grab any existing window instead.
- If there's no room above the terminal (maximized / snapped to top), the bar **hides**
  until there's room again — it never relocates.
- **Z-order matches the terminal's band**: the bar is not topmost — it is re-inserted
  directly above the followed terminal in the window stack, so any app that covers the
  terminal covers the bar too, and focusing the terminal brings the bar back with it.
- Width tracks the terminal's *visible* frame (DWM extended frame bounds, excluding the
  invisible resize borders), so the edges line up exactly.

**Token tracker** — click the `◇` chip, double-launch Chocobar, or Ctrl+Alt+D → Token dashboard:
- GitHub-style daily heatmap (26 weeks, shades spread by quantiles so heavy usage days
  still differentiate)
- Today / 7 days / 30 days / all-time totals
- Per-app and per-model breakdown (input / output / cache read / cache write / calls)
- **Subscription plans**: quota-based plan credits don't come from session logs — point
  `tokens.sources.subscription.usagePath` at a small JSON file you keep anywhere
  (`{ "plans": [{ "name": "Pro Plan", "total": 1500, "used": 430, "resetsAt": "2026-10-14" }] }`)
  and the dashboard renders each plan as a soft pastel usage card (bar, percent, reset date)
- **Totals = input + output.** zcode's `input_tokens` already includes cached tokens, so
  adding cache reads back in would double-count (~2x input). Cache columns are shown
  separately in the tables for anyone who wants the raw picture.
- Sources — **all opt-in, all read-only from local files**; enable the ones you use in
  `tokens.sources` and point them at your own stores (typical paths are documented in
  the config template). Nothing is read unless the **tokens.enabled** master switch is
  on: with it off the bar chip is hidden and the dashboard explains how to turn usage
  on, with it on but no source enabled the dashboard shows an explanatory empty state.
  Chocobar ships with zero usage data.
  - **zcode**: `~/.zcode/cli/db/db.sqlite` → `turn_usage` table (durable, every model call; needs `python`/`python3` on PATH)
  - **zai**: two stores. Sessions launched before the 2026-09-10 engine rebuild are in the
    zcode DB, attributed to `zai` when the session id appears as `~/.zai/agent/sessions/ZCODE_sess_*`.
    The rebuilt pi-based engine keeps transcripts as `<utc-ts>_<uuid>.jsonl` in that same
    folder whose ids never reach the DB; chocobar scans those files directly and reads the
    per-message `usage` on assistant messages
  - **pi**: [pi coding-agent](https://github.com/badlogic/pi-mono) session logs under
    `~/.pi/agent/sessions` (scans the per-project subfolders recursively, same
    per-message `usage` records as zai's post-rebuild store)
  - **opencode**: `~/.local/share/opencode/storage/message/**` (assistant messages with `tokens`)
  - **Xiaomi MiMo AI**: while the desktop app is running it publishes a localhost HTTP API
    (`%APPDATA%\Xiaomi MiMo AI\desktop-api.json` holds the port + token); chocobar reads
    `/v1/sessions` + `/v1/sessions/<id>/messages` and counts the per-message `tokens`
    (input excludes cache there, same fold as pi/opencode). History persists in chocobar's own
    token cache; when the app is closed there is simply nothing new to scan.
  - **subscription**: plan credit usage read from your own JSON file (see above)
- Rescans every minute (`tokens.rescanMinutes`). Scans are incremental — file mtimes /
  per-session stamps skip everything already scanned, and the record map +
  `~/.wizbar/token-cache.json` dedupe make rescans idempotent.

## Run

Recommended — the silent launcher (double-click, or autostart uses it too):

```powershell
wscript C:\path\to\wizbar\scripts\start-wizbar.vbs
```

The vbs launcher detaches the bar from your terminal: it survives the terminal
closing, and never prints console noise. `scripts\start-wizbar.cmd` is the same
launch with a console, for debugging.

On Linux/macOS: `npx electron .` (or your distribution's electron) with the usual
`--no-sandbox` flag if your setup requires it. See **Platform support** above for
what to expect off Windows.

```powershell
cd C:\path\to\wizbar
npm install
npm start
```

`npm start` also works, but it runs Electron **as a child of your terminal** —
close the terminal and the bar dies with it. Use it for development; use the
vbs launcher for daily driving.
Autostart at login: set `"general": { "autostart": true }` in the config (writes an
`HKCU\...\Run` entry pointing at the vbs launcher).

## Config

`~/.wizbar/config.json` (`%USERPROFILE%\.wizbar\config.json` on Windows) — written as an
**annotated template on first run**
(every key explained inline with `//` comments), and **hot-reloaded the moment you save it** —
no restart needed. The main dials, all under `"bar"`:

| Key | What it does |
|---|---|
| `backdrop` | `"acrylic"` (translucent tint over the desktop) · `"solid"` (opaque) |
| `backgroundTint` | the box color, e.g. `"#FBF2E2"` |
| `backgroundAlpha` | **fill opacity, 0–255** — 0 = fully clear, 110 = airy (default), 180 = creamy, 255 = solid |
| `gap` | floating air between the bar and the terminal's top edge |
| `height` | bar thickness in DIP (default 24 — applied as-is on current Electron; only very old Electron builds clamped frameless windows toward ~36) |
| `fontSize` / `fontFamily` / `segmentSpacing` | typography |
| `insetX` | per-side trim so the bar doesn't overhang the terminal frame |
| `radius` | corner rounding of the bar box (painted by the page, px) |
| `align` | `right` · `left` · `center` |
| `position` | `above` · `below` |

Theme colors (`"theme"`), module toggles/intervals (`"modules"`), window-follow behavior
(`"terminal"`), and token sources (`"tokens"`) are documented in the same file.

No tray icon by default (`general.showTray: true` brings one back — it follows config live).
Summon the dashboard via the `◇` chip, Ctrl+Alt+D, or double-launching Chocobar. Right-click the
bar for token dashboard · reload chocobar · edit config · reload config · quit. Single-instance locked. The dashboard is a normal
window on purpose: when the windows above it close or minimize, it is activated like any
window and can never be demoted below the terminal (its taskbar/Alt-Tab entry shows the
Chocobar icon while it's open; the tray stays empty).

## De-personalized by default

The shipped defaults are neutral: no usage sources enabled, no pet, no
personal paths anywhere in the repo. Everything machine- or person-specific lives in the
**user-level config** (`~/.wizbar/config.json`, written as an annotated template on first
run and hot-reloaded on save). Point `tokens.sources` at your own stores and enable the
pet module (Windows) with your own exe path if you want it.

## Notes & limits

- The bar's backdrop is **per-pixel translucency, not blur**: the window is created with
  `transparent: true` and the page paints your tint as `rgba(..., backgroundAlpha/255)`
  (default 110) — 0 is fully clear, 255 is solid. DWM `backgroundMaterial` composites its
  window opaquely (the alpha byte is ignored), which is why it was rejected: the bar never
  went see-through under it. On Linux/macOS the tint renders solid — compositing alpha over
  arbitrary wallpapers reads dark/murky there.
- Corners are the page's CSS `border-radius` (`bar.radius`, default 8). The window itself
  carries no DWM rounding, border, or shadow (`hasShadow: false`).
- The dark edge you may see in the gap is the **terminal's own drop shadow** — part of Windows
  Terminal, not the bar.
- `bar.insetX` (default 2 DIP per side) trims the bar so it doesn't overhang the terminal frame.
- The Bluetooth battery chip shows `—` until a device reports a level; hover it for names.
- Follow loop runs at 60Hz (16ms, display refresh) and only touches the window when the
  terminal's geometry actually changes; stats reach the renderer only when a value
  changed (≤4 pushes/s instead of a 10Hz heartbeat). Metrics: CPU/RAM 0.8s, battery 1.5s,
  volume 0.5s, CPU temp 2s.
  Expensive counters have hard floors: the GPU Engine counter polls no faster than every 5s
  (its own query dominates the cost), Bluetooth every 30s. Initial polls are staggered so
  the cadences never align.
- If you open new terminal windows near the very top of the screen, leave ~40px of
  headroom so the floating bar has room to appear above them.

- GPU % comes from the Windows `GPU Engine` performance counters (same as Task Manager,
  summed across engines, capped at 100). English counter names — on a non-English Windows
  install the GPU segment shows `—`.
- Bluetooth battery uses the PnP property Windows exposes for BT devices
  (`{83DA6326-…},2`). Devices that don't report battery over the standard GATT service
  (some earbuds only report to their vendor app) show `—`. Use `modules.bluetooth.filter`
  to match your earbuds by name.
- Volume is the default render device master volume (Core Audio, read-only).
- The desktop-pet presence check is an in-process Toolhelp32 snapshot (~5ms per 3s poll),
  not a tasklist.exe spawn (~164ms measured per spawn — it dominated the CPU budget).

## Tests

Headless, no GUI required, safe on every platform: tests run against a temp HOME (your real
`~/.wizbar` cache is never read or written) and only ever read the real stores:

```
npm test
```

- `scripts/portable_regression.js` — portability layer (synthetic sysfs battery/CPU-temp trees,
  metrics on-change dirty tracking, neutral-defaults guarantees, module load with koffi
  completely broken),
- `scripts/pi_source_regression.js` — pi session-log source: synthetic fixture (nested projects,
  legacy-file skip, corrupt tail line, dedup, mtime cursors) + exact raw-sum cross-check when a
  real `~/.pi/agent/sessions` exists.

Windows-side extras: `scripts/token_regression.js` (zcode+zai attribution, needs those stores),
`scripts/cputemp_regression.js` (HWiNFO shared-memory reader, Windows only).

## Layout

```
main.js            app entry: tray, IPC, wiring, lifecycle
src/config.js      defaults + ~/.wizbar/config.json hot reload
src/tracker.js     first-window attach + follow state machine (Win32); static pill/strip placement off-Windows
src/native.js      platform gate: koffi Win32 bindings (windows, battery, volume, HWiNFO temp)
                   + portable readers (sysfs battery/CPU temp, Toolhelp32 process snapshot)
src/metrics.js     cpu/ram (node); gpu + bluetooth battery via persistent PowerShell workers (Windows only)
src/tokens.js      token scanners + aggregation, cache at ~/.wizbar/token-cache.json
src/bar.js         the bar BrowserWindow
renderer/          bar.html/css/js, dash.html/css/js (+ preloads)
scripts/           launchers, regression suites (npm test), zcode_query.py, smoke.js
```
