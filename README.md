# WizBar

A slim acrylic system-status bar that floats **above your Windows Terminal window**, plus a
cross-CLI **token usage tracker** (zcode · zai · opencode · Xiaomi MiMo AI) with a GitHub-style
heatmap.

Built to match your terminal theme: pale-yellow acrylic (`#F5F0D8` / `#FDEFF2`), pink accents
(`#E8C7D0`), MesloLGLDZ Nerd Font.

```
        ┌────────────────────────────────────────────────────────────────────┐
        │                       ◇ 235.7M  ⬡23% ⌗18% ▤60% 🔇0% 🔋65% Sep 09 20:04 │   <- WizBar (24px, follows width)
        └────────────────────────────────────────────────────────────────────┘
        ┌────────────────────────────────────────────────────────────────────┐
        │  − □ ✕   PowerShell                                                │   <- Windows Terminal
        │  PS C:\Users\tyanw>                                                │
        └────────────────────────────────────────────────────────────────────┘
```

## What it does

**Bar** (thin strip above the terminal, right-aligned modules):
- today's tokens (click it → dashboard) · 🎀 Little Remielle pet toggle (click = start/stop)
- GPU % · CPU % · CPU temp (HWiNFO) · RAM % · volume % · battery %
- Bluetooth device battery (earbuds — shows `87·85` for L/R when the device reports it)
- agent fleet chip (`6 live`) — real-time count of coding agents running in herdr,
  with `1/6 working` detail on hover
- clock

**Follow semantics** (exactly as specified):
- The bar attaches to the **first** terminal window it sees (frontmost when WizBar starts).
- It follows only that window: move, resize (width), minimize (bar hides), restore (bar returns).
- Opening more terminal windows does **not** retrigger it.
- When the followed window closes, the bar hides and waits for the **next newly opened**
  terminal window (existing other windows are ignored). Set `terminal.reattachToExisting:
  true` to grab any existing window instead.
- If there's no room above the terminal (maximized / snapped to top), the bar flips to the
  bottom inside edge (`bar.maximizedBehavior`: `bottom` | `overlay` | `hide`).
- **Z-order matches the terminal's band**: the bar is not topmost — it is re-inserted
  directly above the followed terminal in the window stack, so any app that covers the
  terminal covers the bar too, and focusing the terminal brings the bar back with it.
- Width tracks the terminal's *visible* frame (DWM extended frame bounds, excluding the
  invisible resize borders), so the edges line up exactly.

**Token tracker** — click the `◇` chip, double-launch WizBar, or Ctrl+Alt+D → Token dashboard:
- GitHub-style daily heatmap (26 weeks, shades spread by quantiles so heavy usage days
  still differentiate)
- Today / 7 days / 30 days / all-time totals
- Per-app and per-model breakdown (input / output / cache read / cache write / calls)
- **Totals = input + output.** zcode's `input_tokens` already includes cached tokens, so
  adding cache reads back in would double-count (~2x input). Cache columns are shown
  separately in the tables for anyone who wants the raw picture.
- Sources, all read-only from local files:
  - **zcode**: `~/.zcode/cli/db/db.sqlite` → `turn_usage` table (durable, every model call)
  - **zai**: two stores. Sessions launched before the 2026-09-10 engine rebuild are in the
    zcode DB, attributed to `zai` when the session id appears as `~/.zai/agent/sessions/ZCODE_sess_*`.
    The rebuilt pi-based engine keeps transcripts as `<utc-ts>_<uuid>.jsonl` in that same
    folder whose ids never reach the DB; wizbar scans those files directly and reads the
    per-message `usage` on assistant messages
  - **opencode**: `~/.local/share/opencode/storage/message/**` (assistant messages with `tokens`)
  - **Xiaomi MiMo AI**: while the desktop app is running it publishes a localhost HTTP API
    (`%APPDATA%\Xiaomi MiMo AI\desktop-api.json` holds the port + token); wizbar reads
    `/v1/sessions` + `/v1/sessions/<id>/messages` and counts the per-message `tokens`
    (input excludes cache there, same fold as pi/opencode). History persists in wizbar's own
    token cache; when the app is closed there is simply nothing new to scan.
- Rescans every minute (`tokens.rescanMinutes`); needs `python` on PATH for the sqlite read.
  Scans are incremental — file mtimes / per-session stamps skip everything already scanned,
  and the record map + `~/.wizbar/token-cache.json` dedupe make rescans idempotent.

## Run

Recommended — the silent launcher (double-click, or autostart uses it too):

```powershell
wscript C:\Users\tyanw\work\general\wizbar\scripts\start-wizbar.vbs
```

The vbs launcher detaches the bar from your terminal: it survives the terminal
closing, and never prints console noise. `scripts\start-wizbar.cmd` is the same
launch with a console, for debugging.

```powershell
cd C:\Users\tyanw\work\general\wizbar
npm start
```

`npm start` also works, but it runs Electron **as a child of your terminal** —
close the terminal and the bar dies with it. Use it for development; use the
vbs launcher for daily driving.
Autostart at login: set `"general": { "autostart": true }` in the config (writes an
`HKCU\...\Run` entry pointing at the vbs launcher).

## Config

`%USERPROFILE%\.wizbar\config.json` — written as an **annotated template on first run**
(every key explained inline with `//` comments), and **hot-reloaded the moment you save it** —
no restart needed. The main dials, all under `"bar"`:

| Key | What it does |
|---|---|
| `backdrop` | `"acrylic"` (blurred see-through) · `"solid"` (opaque) · `"none"` (fully clear) |
| `backgroundTint` | the box color, e.g. `"#FBF2E2"` |
| `backgroundAlpha` | **fill opacity over the blur, 0–255** — 0 = fully clear, 110 = airy, 180 = creamy, 255 = solid |
| `gap` | floating air between the bar and the terminal's top edge |
| `height` | bar thickness — **don't go below 36**: Windows silently clamps frameless acrylic windows to a ~35.5 DIP minimum, and the clipped remainder renders as a grey band |
| `fontSize` / `fontFamily` / `segmentSpacing` | typography |
| `insetX` | per-side trim so the bar doesn't overhang the terminal frame |
| `roundCorners` | rounded corners (+ the subtle floating shadow) |
| `align` | `right` · `left` · `center` |
| `position` | `above` · `below` |

Theme colors (`"theme"`), module toggles/intervals (`"modules"`), window-follow behavior
(`"terminal"`), and token sources (`"tokens"`) are documented in the same file.

No tray icon by default (`general.showTray: true` brings one back — it follows config live).
Summon the dashboard via the `◇` chip, Ctrl+Alt+D, or double-launching WizBar. Right-click the
bar for edit config · reload config · quit. Single-instance locked. The dashboard is a normal
window on purpose: when the windows above it close or minimize, it is activated like any
window and can never be demoted below the terminal (its taskbar/Alt-Tab entry shows the
WizBar icon while it is open; the tray stays empty).

## Notes & limits

- The bar's acrylic is the **smooth system backdrop forced to light mode** (`backgroundMaterial`
  + `DWMWA_USE_IMMERSIVE_DARK_MODE=false`), tinted with your color by the CSS overlay
  (`backgroundAlpha`, default 180). The legacy `SetWindowCompositionAttribute` accent API was
  tried and rejected — it renders mosaic/blocky on Win11. Rounding was also rejected — it
  renders blocky AND dark on some builds.
- `roundCorners: true` (default) rounds all four corners; on Windows this re-enables a subtle
  DWM shadow (Electron's `hasShadow` is ignored there) which reads as part of the floating look.
- The dark edge you may see in the gap is the **terminal's own drop shadow** — part of Windows
  Terminal, not the bar.
- `bar.insetX` (default 2 DIP per side) trims the bar so it doesn't overhang the terminal frame.
- If there isn't room above the terminal (maximized / opened at the very top), the bar **hides**
  until there's room again — it never relocates.
- Bluetooth battery chip **auto-hides** until a device reports a level; hover it for names.
- Follow loop runs at ~60fps; metrics: CPU/RAM 0.8s, battery 1.5s, volume 0.5s, CPU temp 2s.
  Expensive counters have hard floors: the GPU Engine counter polls no faster than every 5s
  (its own query dominates the cost), Bluetooth every 30s. Initial polls are staggered so
  the cadences never align.
- Your terminal's `initialPosition` was nudged from `140,40` to `140,60` so new terminal
  windows open with room for the floating bar.
- The bar's bottom edge has a subtle 1px pink divider (hardcoded in `renderer/bar.css`).

- The agent fleet chip talks to the **herdr server socket** directly (on Windows an AF_UNIX
  socket is reachable as `\\.\pipe\<path>`): one persistent connection receives push
  events for agent status changes, plus a slow 60s refresh. No PowerShell worker involved.
  When herdr is not running the chip falls back to the orchestrator's `fleet-status.json`
  and shows `—` when neither source exists.

- GPU % comes from the Windows `GPU Engine` performance counters (same as Task Manager,
  summed across engines, capped at 100). English counter names — on a non-English Windows
  install the GPU segment shows `—`.
- Bluetooth battery uses the PnP property Windows exposes for BT devices
  (`{83DA6326-…},2`). Devices that don't report battery over the standard GATT service
  (some earbuds only report to their vendor app) show `—`. Use `modules.bluetooth.filter`
  to match your earbuds by name.
- Volume is the default render device master level (Core Audio, read-only).
- The follow loop is 25 fps; the bar trails a few px behind while dragging, then snaps.

## Layout

```
main.js            app entry: tray, IPC, wiring, lifecycle
src/config.js      defaults + ~/.wizbar/config.json hot reload
src/tracker.js     first-window attach + follow state machine (koffi/Win32)
src/native.js      koffi bindings: EnumWindows/GetWindowRect, battery, IAudioEndpointVolume
src/metrics.js     cpu/ram (node), gpu + bluetooth battery (persistent PowerShell workers)
src/tokens.js      token scanners + aggregation, cache at ~/.wizbar/token-cache.json
src/bar.js         the bar BrowserWindow
renderer/          bar.html/css/js, dash.html/css/js (+ preloads)
scripts/           launchers, zcode_query.py, smoke.js
```
