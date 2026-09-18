# Chocobar

Chocobar is a lightweight desktop status bar for Windows. It shows live system state in a slim floating bar that follows your terminal window, and provides an optional local usage dashboard and subscription plan board.

## Features

- System chips for CPU, GPU, memory, CPU temperature, volume, battery, Bluetooth device battery, and local time.
- Terminal-aware placement: the bar sits above the followed terminal and moves with it. Auto-detection covers Windows Terminal, classic conhost, ConEmu, mintty, WezTerm, Alacritty, and Hyper. Set `terminal.className` to pin one window class when needed.
- Dashboard with daily heatmap, summary cards, app/model breakdowns, cache details, and request counts.
- Subscription plan board with live quota windows, plus a plan-usage card fed by a local JSON file.
- Soft pastel themes, configurable spacing, fonts, alignment, position, opacity, and corner radius.
- Tray and bar context actions for the dashboards, `Reload chocobar`, config editing, and quit. Reloading keeps the single app instance and optional companion process intact.

All usage adapters are opt-in and read local data only. Supported input shapes include provider or CLI SQLite usage databases (zcode is one optional example), JSONL session logs, JSON message stores, local desktop APIs, and subscription plan snapshots. No store is read until you enable its source, even though the toggles ship visible.

## Install and run

```powershell
npm install
npm start
```

The silent launcher keeps Chocobar independent from the terminal that started it:

```powershell
wscript C:\path\to\project\scripts\start-wizbar.vbs
```

Use `scripts\start-wizbar.cmd` when console output is useful for debugging.

Autostart is controlled by `general.autostart` in the config; it writes an HKCU Run entry for the silent launcher.

## Configuration

On first use of a config path, Chocobar writes an annotated config template there - for a normal install that is:

```
%USERPROFILE%\.wizbar\config.json
```

This includes an explicit `--config <path>` launch: the file is created with the template on first use, so `Edit config` (bar or tray right-click) always has a file to open. The file is hot-reloaded after saving; the generated comments document every available key. Main groups:

| Group | Controls |
|---|---|
| `bar` | Height, gap, position, alignment, font, spacing, tint, opacity, and backdrop |
| `theme` | Text, pastel accents, warning, success, and divider colors; the five daily-heatmap shades; per-surface backgrounds (`surfaces`) for the dashboards and the context menu |
| `modules` | System chip switches, polling intervals, thresholds, clock format, shortcut chip, user-defined chips, and optional companion |
| `terminal` | Auto-detection, class pinning, and whether existing windows may be selected after a close |
| `tokens` | Master switch, dashboard chip, rescan interval, heatmap range, harness display names, dashboard section toggles, and local usage adapters |
| `subs` | Live subscription board: provider adapters, poll interval, request deadline, and window size |
| `general` | Tray, autostart, and debug logging |

### The three toggles on the left of the bar

The bar ships with three toggle chips visible, leftmost. Each is a button; hide any of them by flipping its flag in the config if you do not want it.

| Chip | Opens / does | Intended use | Hide with |
|---|---|---|---|
| Bolt (shortcut) | Runs `modules.shortcut.command` | One-click launch of anything: an app, a script, a URL. With no command set it does nothing - set `modules.shortcut.command` (and optionally `label`) to wire it up | `modules.shortcut.enabled: false` |
| Diamond (tokens) | Token usage dashboard | Local LLM/harness usage at a glance: today / 7 days / 30 days / all time, a daily heatmap, per-app and per-model breakdowns. Shows `–` until you enable a usage source under `tokens.sources` | `tokens.showOnBar: false` (or master `tokens.enabled: false` to also stop all scans) |
| Gauge (subs) | Subscription plan board | Live rate-limit / quota windows for plans you wire under `subs.providers` (ChatGPT via a Codex CLI login, Z.ai coding plan via the zcode credential - both ship disabled). The chip reads `—` until one is enabled | `subs.enabled: false` |

A fourth chip, the bow (pet), is opt-in: set `modules.pet` (`enabled`, `exePath`, optional `label`) and it launches/stops a Windows companion exe, reading on/off from the live process.

### Your own chips (`modules.custom`)

Anything the built-in toggles don't cover, you add yourself - each entry in `modules.custom` renders one more chip on the bar, with your design and your function:

```json
{ "enabled": true, "icon": "\uf011", "label": "Focus", "color": "",
  "title": "Toggle the focus script", "toggle": true,
  "command": "C:\\tools\\focus.bat" }
```

- **Design**: `icon` (a nerd-font glyph or emoji), `label` (chip text; icon-only if empty), `color` (chip color; empty = theme default), `title` (hover tooltip).
- **Function**: a click runs `command` - any program, script, or URL your shell can launch. With `toggle: true` the chip keeps an on/off state and runs `command on` / `command off` so your script can react; the state is per-run, like the pet's.
- Chips appear right of the built-in toggles; remove an entry (or set `enabled: false`) and the chip is gone.

### Surface colors (`theme.surfaces`)

The three popup windows take their own background, independent of the shared palette: `theme.surfaces.dashboard`, `theme.surfaces.subs`, and `theme.surfaces.menu` each accept `followBar` (reuse the bar's tint + alpha + backdrop exactly - the context menu's default, so it looks like the bar it came from) or an explicit `backgroundTint` (empty = each surface's built-in pink). The dashboards are opaque windows by design, so there the tint sets the color only; the menu window is transparent and also honors `backgroundAlpha` (0 = clear, 255 = solid).

### Subscription usage file

Enable the master switch and the subscription source, then point `usagePath` at a local JSON file of plan snapshots. The generated config documents the snapshot shape, and Chocobar re-reads it on each usage rescan.

```json
{
  "tokens": {
    "sources": {
      "subscription": {
        "enabled": true,
        "usagePath": "~/path/to/plan-usage.json"
      }
    }
  }
}
```

Keep the file local and do not place secrets in the repository.

### Subscription plans board (live quotas)

Separately from the plan-usage file above, the subscription board shows live rate-limit / quota windows for plans you wire up under `subs.providers`. Each entry picks an adapter `type` (`chatgpt` reads a Codex CLI login, `zai` reads a Z.ai coding-plan credential), a display `label`, and where the credential lives. The two examples ship disabled; nothing is pre-wired to any vendor. The poll interval (`intervalMinutes`), per-provider request deadline (`fetchTimeoutMs`), and board window size are configurable. A provider whose request fails or times out keeps its last good windows on the board, marked stale, until the next successful poll.

### Harness names and dashboard sections

The dashboard is harness-agnostic: source ids (`zcode`, `zai`, `pi`, `opencode`, `mimo`) are display names by default, and `tokens.labels` maps any of them to your own name (for example `{ "opencode": "opencode(wsl)" }`). `tokens.dashboard` switches individual dashboard sections (stat cards, heatmap, day detail, by-app, by-model, plan usage) on or off.

## Token accounting

Every total in the bar and dashboard follows the common usage-dashboard convention (ccusage & co):

```
total = input + output + cache read + cache write
```

The total counts every token the provider processed. The `input` and `output` columns are the raw, cache-EXCLUSIVE counts, and the `cache R` / `cache W` columns break the rest of the total down - together they sum to the total, with nothing hidden and nothing doubled. Reasoning tokens, where a store reports them separately, are stored as a breakdown only and never added (providers include them in their own totals).

This is the general convention - the providers themselves total it this way, verified on real stores:

- pi/zai transcripts: the provider's own `usage.totalTokens` equals `input + output + cacheRead + cacheWrite` exactly on every record checked.
- opencode: the store's own `tokens.total` equals `input + output + cache.read + cache.write` on sampled rows (outliers differ by exactly their `reasoning` - opencode adds reasoning to its total, we keep it as a breakdown).
- zcode DB: `computed_total_tokens` equals `input_tokens + output_tokens + reasoning_tokens`, and `input_tokens` already contains the cache columns - i.e. the DB's own total includes cache too.
- ccusage, the reference usage dashboard for Claude-shaped APIs, defines its Total column the same way (input + output + cache creation + cache read).

Per source, the raw numbers come from the provider's own usage records:

| Source | Record read | Provider-reported semantics | Stored input (raw) |
|---|---|---|---|
| `zcode` (+ legacy `zai` DB turns) | `turn_usage` row in the zcode SQLite DB | `computed_total_tokens == input_tokens + output_tokens + reasoning_tokens`, so `input_tokens` already INCLUDES the cache columns | `input_tokens - cacheWrite - cacheRead` |
| `zai` / `pi` sessions | assistant `message.usage` in the session JSONL | `usage.totalTokens == input + output + cacheRead + cacheWrite`, so `input` EXCLUDES cache | `usage.input` as stored |
| `opencode` | assistant message `tokens` in the SQLite `message.data` JSON (or the legacy `msg_*.json` tree) | `input + output + cache.read + cache.write` (input excludes cache) | `tokens.input` as stored |
| `mimo` | assistant message `tokens` from the local desktop API | `input + output + cache.read + cache.write` (input excludes cache) | `tokens.input` as stored |
| `subscription` | your plan-usage JSON (`used` / `total` per plan) | n/a (credits, not tokens) | never mixed into token totals |

Exact read sites, for reference:

- zcode: `scripts/zcode_query.py` (the SQL) and `_scanZcode` in `src/tokens.js`
- zai/pi sessions: `_readSessionTail` in `src/tokens.js`
- opencode: `_scanOpencodeDb` / `_scanOpencodeFiles` in `src/tokens.js`
- mimo: `_scanMimo` in `src/tokens.js`
- aggregation: `aggregate()` in `src/tokens.js` (`rowTotal = input + output + cacheRead + cacheWrite`)

`npm test` cross-checks the scanner against a raw walk of a real session store: record counts and per-column sums must match exactly, and the portable suite pins the aggregation contract (totals include cache; input/output columns stay raw).

## Use the bar and dashboard

- Click the diamond (tokens) chip or use `Ctrl+Alt+D` to open the dashboard.
- Double-launch Chocobar to open the dashboard when it is already running.
- Right-click the bar for the themed context menu (it wears the bar's color and opacity) and the tray icon for the same actions natively; both close on Esc or an outside click. Saving the config hot-reloads, so there is no reload-config entry.
- The clock format supports `{Wkk}` (weekday, `Mon`..`Sun`), for example `{MMM} {dd} ({Wkk}) {HH}:{mm}` renders `Sep 17 (Thu) 23:33` in local time.
- The bar follows the terminal you are in: the foreground window wins when it is a supported terminal, otherwise the first match in probe order. With the default empty `terminal.className`, it probes common terminals in documented order. Set `terminal.reattachToExisting: true` to use an existing terminal after the followed window closes.
- If there is no room above a terminal, the bar hides until room returns instead of relocating unexpectedly.
- Launch with a specific config file: `chocobar --config <path>` (or the `WIZBAR_CONFIG` environment variable). The file gets the annotated template on first use, so personal wiring stays in personal files while a fresh install just works.

## Defaults and privacy

The shipped defaults read nothing: every usage source and board provider is off and every path is empty. The three toggle chips are visible so you can find them; with nothing wired they show `–` / `—` and scan nothing. Chocobar has no telemetry. Enabled sources are read-only and local, except for an explicitly enabled local desktop API adapter.

Internal compatibility paths and filenames still use `wizbar`, including `~/.wizbar` and `start-wizbar.vbs`. The application and all user-visible strings use Chocobar.

## Tests

Tests are headless, use a temporary HOME, and require no GUI:

```bash
npm test
```

The suite covers portable readers, neutral defaults, token aggregation, subscription snapshots, terminal probe resolution, and session-log scanning. Windows-only checks cover native sensor providers.

## Layout

```text
main.js            app entry, tray, IPC, lifecycle, and reload action
src/config.js      defaults and hot-reloaded user config
src/tracker.js     terminal detection and follow state machine
src/native.js      native bindings and portable readers
src/metrics.js     system metric polling
src/tokens.js      local usage adapters and aggregation
src/bar.js         bar BrowserWindow
renderer/          bar and dashboard HTML, CSS, JavaScript, and preloads
scripts/           launchers and regression suites
```
