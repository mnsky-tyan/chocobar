# Codebase Review - 2026-10-08
Scope: /home/tyan/mnsky/chocobar | Review units: native/src (C sources), non-C files, cross-scope
Base: 1bf3ae0 (PR #72 merged) + the release-workflow work (3d98ec6, f9941b1)
Summary: 11 findings (0 Critical, 0 High, 4 Medium, 7 Low).

## Findings by Severity
| # | Category | Severity | File(s) | Description | Fix Complexity |
|---|---|---|---|---|---|
| 1 | Correctness - uninitialized read | Medium | native/src/p_ui.c:676-685, 765-768 | `BY_HANDLE_FILE_INFORMATION fi` is only filled when `GetFileInformationByHandle` succeeds, but `g_cacheStamp[]`/`g_cacheMtimeScan` are stamped unconditionally. A failed stat publishes garbage as the cache identity, so the next scan either re-reads the token cache needlessly or falsely matches and skips a changed cache - leaving the board on stale numbers. Fix: only stamp under a captured success flag. | Inline |
| 2 | Correctness - uninitialized read | Medium | native/src/p_metrics.c:171-178, 202 | `wchar_t sensors[256][64]` is uninitialized; the fill loop breaks at the first empty name, but the reading loop copies for any `id < 256`. An id past the filled count copies stack garbage into `recs[n].sensor`, and `pickCpuTemp` then substring-matches "cpu" against it, so the CPU temperature can silently select a non-CPU sensor. Fix: track the filled count and gate on it. | Inline |
| 3 | Correctness - OOB / int overflow | Medium | native/src/p_metrics.c:223-243 | The SM2 reader validates `readingsOff + readingCount * readingSize > total` in 32-bit DWORD arithmetic, which wraps; `sensorCount` is never validated against `total` at all, and `48 + id * sensorSize` also overflows. A malformed writer of HWiNFO's shared section can push reads outside the mapped region. Fix: do both bounds in 64-bit and clamp the sensor table. | Inline |
| 4 | Correctness - uninitialized read / race | Medium | native/src/p_ui.c:3386-3389 | `subsChipFormat` declares `SubsWin w[MAX_GEN_WIN]` uninitialized and reads `w[k]` after a second `subsProvWins` call. A provider with zero windows (a legit state) plus a fetch thread landing `g_subsWinN=0` between the two lock acquisitions leaves `wn == 0`, `k = 0`, and the gauge prints garbage. Fix: bail with an em dash when `wn <= 0`, as `subsChipRotated` already does. | Inline |
| 5 | Cross-Directory Pattern Divergence | Medium | native/src/p_ui.c:135-152, 1738-1747, 1878-1891 | "take the first font family from the config string" exists three times: `initRender` re-implements `uiFontFamily` character-for-character (because `initRender` precedes its forward declaration), and the `CreateFontW` construction is duplicated in `configCheckTick`. A fix to the family rules must be made twice. Fix: move `uiFontFamily` above `initRender` and share a `barFont()` helper. | Inline |
| 6 | Consistency | Low | native/src/p_subs.c:1306 vs 1407 | Two sibling `swprintf` appends into a growing wide buffer differ: `subsGenAuthLine` guards the return (`if (n > 0) *used += n`), the static-header append right next to it does not. The latter is safe only incidentally. | Inline |
| 7 | YAGNI | Low | native/src/p_subs.c:1643, 1683 | `UpdateVerdict.newer` is written by `updThread` and never read anywhere; the tray note and log derive text from `msg`. Dead state that invites trust. | Inline |
| 8 | Correctness - workflow trigger | Low | .github/workflows/release.yml:10-15 | **FIXED in f9941b1.** The tag glob `v[0-9]*.[0-9]*.[0-9]*` matched pre-release and 4-component tags (`v1.3.0-rc1`, `v1.3.0.1`) because GitHub's `*` is zero-or-more, contradicting its own comment. Now `v[0-9]+.[0-9]+.[0-9]+`, matching GitHub's documented semver example. | Fixed |
| 9 | Documentation contradiction | Low | AGENTS.md:44-52 vs 96-113 | **FIXED in f9941b1.** The old Deploy section still told agents to run `deploy_bar.ps1`, which copies a locally built binary over the user's Run path - exactly what the new development-split section forbids. Reconciled; `install_release.ps1` added for asset installs. | Fixed |
| 10 | Documentation staleness | Low | AGENTS.md:127, native/README.md:34 | **FIXED in f9941b1.** Both claimed the build requires nix; `build.sh` prefers a system mingw and falls back to nix. `--no-nix` (used by release.yml) was undocumented. | Fixed |
| 11 | Simplicity & Efficiency | Low | native/build.sh --no-nix path | On a machine with both a system mingw and nix, the normal path always takes the system recipe (the `||` short-circuits); that is intended for CI but the docs now state the rule. `--no-nix` with no system toolchain fails with a bare "command not found" from `set -e`. | Inline |

## Clean Areas
- jobjGet `obj < 0` guard, the 4 MB subscription-body cap, `g_tokDbgStart` removal, `iconUserAdd` cleanup, `chipClickable` command check, the Z.ai header sizing, the `clockFmtReload` single call site, and the svgNum/svgWalk loop fix from PR #72 - all verified present and correct.
- The three-way version agreement guard (version.h / package.json / package-lock.json) holds; all three read 1.3.0.
- The compile gate (`build.sh --check`) passes clean on the merged tree; the release workflow's tag/version assertions were re-run locally and pass.
- `permissions: contents: write` is sufficient for `gh release create/upload`; the `version.h` sed parse and the `strings -el | grep -qx` version assertion were both verified against a real built exe.

## Notes for the fix round
- Findings 1-4 are the ones that matter: all four are uninitialized/out-of-bounds reads reachable in normal operation (1 and 4 especially), and none is flagged by `-Wall -Wextra`.
- Findings 8, 9 and 10 were found and fixed during this review round before the report was written; they are recorded for the audit trail.
