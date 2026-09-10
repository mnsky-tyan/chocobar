# WizBar

A slim acrylic system-status bar that floats **above your Windows Terminal window**, plus a
cross-CLI **token usage tracker** (zcode · zai · opencode) with a GitHub-style heatmap.

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
- today's tokens (click it → dashboard) · GPU % · CPU % · RAM % · volume % · battery %
- Bluetooth device battery (earbuds — shows `87·85` for L/R when the device reports it)
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

**Token tracker** — click the `◇` chip, double-launch WizBar, or tray → Token dashboard:
- GitHub-style daily heatmap (26 weeks, shades spread by quantiles so heavy usage days
  still differentiate)
- Today / 7 days / 30 days / all-time totals
- Per-app and per-model breakdown (input / output / cache read / cache write / calls)
- **Totals = input + output.** zcode's `input_tokens` already includes cached tokens, so
  adding cache reads back in would double-count (~2x input). Cache columns are shown
  separately in the tables for anyone who wants the raw picture.
- Sources, all read-only from local files:
  - **zcode**: `~/.zcode/cli/db/db.sqlite` → `turn_usage` table (durable, every model call)
  - **zai**: zai runs the same engine and writes to the same DB; sessions whose id also
    appears in `~/.zai/agent/sessions/ZCODE_sess_*` are attributed to `zai`, the rest to `zcode`
  - **opencode**: `~/.local/share/opencode/storage/message/**` (assistant messages with `tokens`)
- Rescans every 5 minutes; needs `python` on PATH for the sqlite read.

## Run

```powershell
cd C:\Users\tyanw\work\general\wizbar
npm start
```

Silent launchers: `scripts\start-wizbar.vbs` (no console) and `scripts\start-wizbar.cmd`.
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

Tray icon: dashboard · edit config · reload config · quit. Right-click the bar for the same
menu. Single-instance locked.

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
- Follow loop runs at ~60fps; metrics: CPU/RAM 0.8s, GPU ~1.2s, battery 1.5s, volume 0.5s.
- Your terminal's `initialPosition` was nudged from `140,40` to `140,60` so new terminal
  windows open with room for the floating bar.
- The bar's bottom edge has a subtle 1px pink divider (hardcoded in `renderer/bar.css`).

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
