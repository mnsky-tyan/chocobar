# Chocobar

Chocobar is a lightweight desktop status bar for Windows, Linux, and macOS. It shows live system state in a slim floating bar, follows terminal windows on Windows, and provides an optional local usage dashboard.

## Features

- System chips for CPU, GPU, memory, CPU temperature, volume, battery, Bluetooth device battery, and local time.
- Terminal-aware placement on Windows. Auto-detection covers Windows Terminal, classic conhost, ConEmu, mintty, WezTerm, Alacritty, and Hyper. Set `terminal.className` to pin one window class when needed.
- Static placement elsewhere: a corner pill by default, or a full-width top strip with `bar.staticWidth: "workarea"`.
- Dashboard with daily heatmap, summary cards, app/model breakdowns, cache details, and request counts.
- Subscription plan card for quota snapshots. It shows used credits, total credits, a progress bar, and the next reset date.
- Soft pastel themes, configurable spacing, fonts, alignment, position, opacity, and corner radius.
- Tray and bar context actions for the dashboard, `Reload chocobar`, config editing, and quit. Reloading keeps the single app instance and optional companion process intact.

All usage adapters are opt-in and read local data only. Supported input shapes include provider or CLI SQLite usage databases (zcode is one optional example), JSONL session logs, JSON message stores, local desktop APIs, and subscription plan snapshots. No usage data is read until the `tokens.enabled` master switch and a source are enabled.

## Platform support

| Feature | Windows | Linux / macOS |
|---|---|---|
| Bar, themes, clock, dashboard | Yes | Yes |
| CPU and memory | Yes | Yes |
| Battery | Win32 power status | Linux sysfs; macOS shows `-` when unavailable |
| CPU temperature | Shared-memory sensor provider | Linux hwmon / thermal zones; macOS shows `-` when unavailable |
| Follows terminal window | Win32 tracking, placement, visibility, and z-order | Static pill or top strip |
| GPU, volume, Bluetooth battery | Native Windows providers | Chip shows `-` when unavailable |
| Acrylic backdrop | Translucent tint | Solid tint |
| Optional desktop companion | Windows executable | Not available |

Windows-only settings are ignored on other platforms. Non-Windows mode degrades to honest empty values; it does not fabricate metrics.

## Install and run

```powershell
npm install
npm start
```

On Windows, the silent launcher keeps Chocobar independent from the terminal that started it:

```powershell
wscript C:\path\to\project\scripts\start-wizbar.vbs
```

Use `scripts\start-wizbar.cmd` when console output is useful for debugging. On Linux or macOS, use the Electron executable supplied by your environment; add `--no-sandbox` only when your environment requires it.

Autostart is controlled by `general.autostart` in the config. On Windows it writes an HKCU Run entry for the silent launcher.

## Configuration

On first run, Chocobar creates an annotated config at:

- Linux/macOS: `~/.wizbar/config.json`
- Windows: `%USERPROFILE%\\.wizbar\\config.json`

The file is hot-reloaded after saving. The generated comments document every available key. Main groups:

| Group | Controls |
|---|---|
| `bar` | Height, gap, position, alignment, font, spacing, tint, opacity, backdrop, and static width |
| `theme` | Text, pastel accents, warning, success, and divider colors; dashboard surfaces; the five daily-heatmap shades |
| `modules` | System chip switches, polling intervals, thresholds, clock format, and optional companion |
| `terminal` | Auto-detection, class pinning, and whether existing windows may be selected after a close |
| `tokens` | Master switch, dashboard chip, rescan interval, heatmap range, harness display names, dashboard section toggles, and local usage adapters |
| `subs` | Live subscription board: provider adapters, poll interval, request deadline, and window size |
| `general` | Tray, autostart, and debug logging |

### Subscription usage

Enable the master switch and the subscription source, then point `usagePath` at a local JSON file of plan snapshots. The generated config documents the snapshot shape, and Chocobar re-reads it on each usage rescan.

```json
{
  "tokens": {
    "enabled": true,
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

Separately from the plan-usage file above, the subscription board shows live
rate-limit / quota windows for plans you wire up under `subs.providers`. Each
entry picks an adapter `type` (`chatgpt` reads a Codex CLI login, `zai` reads
a Z.ai coding-plan credential), a display `label`, and where the credential
lives. All entries ship disabled; nothing is pre-wired to any vendor. The
poll interval (`intervalMinutes`), per-provider request deadline
(`fetchTimeoutMs`), and board window size are configurable. A provider whose
request fails or times out keeps its last good windows on the board, marked
stale, until the next successful poll.

### Harness names and dashboard sections

The dashboard is harness-agnostic: source ids (`zcode`, `zai`, `pi`,
`opencode`, `mimo`) are display names by default, and `tokens.labels` maps
any of them to your own name (for example `{ "opencode": "opencode(wsl)" }`).
`tokens.dashboard` switches individual dashboard sections (stat cards,
heatmap, day detail, by-app, by-model, plan usage) on or off.

## Token accounting

Every total in the bar and dashboard is:

```
total = input + output
```

The `input` column is cache-INCLUSIVE: each scanner folds its provider's
prompt-cache reads and writes into the stored input, so `input + output`
equals the total the provider reported for that request. The cache columns
on the dashboard are informational breakdowns and are never added on top
(adding them would double-count). Reasoning tokens, where a store reports
them separately, are stored as a breakdown only and never added.

Per source, the numbers come from the provider's own usage records:

| Source | Record read | Provider-reported total | Stored input |
|---|---|---|---|
| `zcode` (+ legacy `zai` DB turns) | `turn_usage` row in the zcode SQLite DB | `input_tokens + output_tokens` (the DB's `input_tokens` already includes cached tokens; `cache_*_input_tokens` columns are the breakdown) | `input_tokens` as stored |
| `zai` / `pi` sessions | assistant `message.usage` in the session JSONL | `input + output + cacheRead + cacheWrite` (input excludes cache) | `input + cacheRead + cacheWrite` |
| `opencode` | assistant message `tokens` in `msg_*.json` | `input + output + cache.read + cache.write` (input excludes cache) | `input + cache.read + cache.write` |
| `mimo` | assistant message `tokens` from the local desktop API | `input + output + cache.read + cache.write` (input excludes cache) | same fold as opencode |
| `subscription` | your plan-usage JSON (`used` / `total` per plan) | n/a (credits, not tokens) | never mixed into token totals |

Exact read sites, for reference:

- zcode: `scripts/zcode_query.py` (the SQL) and `_scanZcode` in `src/tokens.js`
- zai/pi sessions: `_readSessionTail` in `src/tokens.js`
- opencode: `_scanOpencode` in `src/tokens.js`
- mimo: `_scanMimo` in `src/tokens.js`
- aggregation: `aggregate()` in `src/tokens.js` (`rowTotal = input + output`)

`npm test` cross-checks the scanner against a raw walk of a real session
store: record counts and per-column sums must match exactly, and the
portable suite pins the aggregation contract (totals = input + output, cache
as breakdown).

## Use the bar and dashboard

- Click the usage chip or use `Ctrl+Alt+D` to open the dashboard.
- Double-launch Chocobar to open the dashboard when it is already running.
- Right-click the bar or tray icon for dashboard, reload, config, and quit actions.
- Optional leftmost shortcut chip: set `modules.shortcut` (`enabled`, `label`,
  `command`) in the config and the bar shows a bolt button that runs any
  command you put there.
- The clock format supports `{Wkk}` (weekday, `Mon`..`Sun`), for example
  `{MMM} {dd} ({Wkk}) {HH}:{mm}` renders `Sep 17 (Thu) 23:33` in local time.
- On Windows, the bar follows the terminal you are in: the foreground window wins when it is a supported terminal, otherwise the first match in probe order. With the default empty `terminal.className`, it probes common terminals in documented order. Set `terminal.reattachToExisting: true` to use an existing terminal after the followed window closes.
- If there is no room above a terminal, the bar hides until room returns instead of relocating unexpectedly.

## Defaults and privacy

The shipped defaults are neutral: usage sources, the usage master switch, and the optional companion are off; paths are empty; no personal identifiers are required. Chocobar has no telemetry. Enabled sources are read-only and local, except for an explicitly enabled local desktop API adapter.

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
