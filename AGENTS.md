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
Authoritative reader: `src/tokens.js`; contract check: `node scripts/token_regression.js`.

