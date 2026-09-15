# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.

## Token usage stores (sharp edge)

zcode CLI and zai persist usage in DIFFERENT stores, and zai's store can change across engine rebuilds:

- zcode CLI: `~/.zcode/cli/db/db.sqlite` `turn_usage` (read via `scripts/zcode_query.py`).
- zai (pi-based engine): per-message `usage` inside `~/.zai/agent/sessions/*.jsonl`.
  Since the 2026-09-10 rebuild these are named `<utc-ts>_<uuid>.jsonl` and never touch
  the zcode DB; the legacy `ZCODE_sess_<uuid>_*.jsonl` files are DB-backed (imports /
  pre-rebuild sessions) and must NOT be counted from disk too, or they double-count.
- Raw usage semantics differ by store: zcode DB input_tokens already INCLUDES cached
  tokens; pi and opencode report cache BESIDE input (and opencode reasoning is a
  breakdown of output, never additive). _scanZaiSessions/_scanOpencode therefore fold
  cache into the stored input, so every stored record is cache-inclusive and
  aggregate() totals stay input+output. pi per-message totalTokens
  (= input+output+cacheRead+cacheWrite) is the raw ground truth to check against.
- Xiaomi MiMo AI desktop: no local transcript store — the app serves a localhost HTTP API
  while running (port + bearer token in `%APPDATA%\Xiaomi MiMo AI\desktop-api.json`;
  routes `GET /v1/sessions`, `GET /v1/sessions/<id>/messages`). Assistant messages carry
  `tokens {input, output, reasoning, cache:{read,write}}` with input EXCLUDING cache
  (total = input+output+cacheRead+cacheWrite). _scanMimo folds cache like pi/opencode,
  dedups on `m:<message id>`, and skips unchanged sessions via a `mimoSigs` cursor
  (session `time.updated` → stamped into token-cache.json).
Authoritative reader: `src/tokens.js`; contract check: `node scripts/token_regression.js`.

## CPU hot-loop baselines (measured 2026-09-13, this machine)

- GPU widget: the PowerShell Get-Counter loop dominates; the sleep floor is the lever
  (0.8s floor ≈ 41.7% of one core, 5s ≈ 9.5%). Floor lives in _startGpuWorker.
- pollRemielle: a tasklist.exe spawn cost ~290ms CPU per 3s poll; the in-process
  Toolhelp32 snapshot (native.findProcessIdByName) costs ~5ms. Keep it in-process.
- zcode token scan: python scripts/zcode_query.py ≈ 650ms per run; _scanZcode must
  stay async or the bar main process freezes for that long every rescan.
- Negligible despite tight cadence (do not churn without re-measuring): tracker
  follow-tick natives ≈ 0.01ms per 8ms tick, HWiNFO temp parse ≈ 1.4ms per 2s,
  z-sync EnumWindows ≈ 0.6ms per 400ms.
## Running and restarting (sharp edges)

- WizBar is not packaged: the app IS a git worktree (`electron .`), autostarted at login
  by `HKCU\...\Run` -> that worktree's `scripts/start-wizbar.vbs`. A commit is therefore
  NOT live until the app is restarted - check the running electron's start time against
  the commit date before concluding that a fix did not work.
- The pet 小蕾米 is spawned by the app, so `taskkill /PID <app> /T` kills her too; `/F` on
  the app pid alone leaves her running. To bring her back, spawn her detached with her own
  cwd (what the bar's toggle does) and save the spot in `~/.wizbar/remielle-position.json`
  plus her own `设置.json`; the app adopts the new process through its name poll.
