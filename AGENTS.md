# Chocobar - project notes

## The bar the user is actually running

`C:\Users\tyanw\review\chocobar\native\chocobar.exe`. It is a single portable
exe, registered in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` under the
name `Chocobar`, and it is the user's live status bar - it holds the
single-instance mutex, so a second copy cannot coexist with it.

**Never kill, stop, or restart that process as part of a test or validation.**
It is not a test fixture; it is the thing the user looks at all day, and it does
not come back on its own.

## Where a test run of the bar must happen

Any validation that needs to *run* the bar (the no-mistakes Test step, a manual
probe, a smoke run) must launch its own copy in isolation:

- Launch it on the **`second` Windows virtual desktop**, in its own terminal
  there, so it never appears over the user's work. Use `win-hidden-launch`
  (`~/.local/bin`) - see the global `mn-windows-desktops` skill.
- Point it at an **isolated config and scratch dir** (`--config <scratch>`,
  a throwaway `USERPROFILE`-shaped dir), never the live `~/.wizbar/config.json`,
  so it cannot touch the real cursor file or log.
- **Pin the scratch copy's terminal.** `evictRivalBars()` (p_ui.c) enforces
  one bar per terminal by ordered eviction: the **lowest pid on a terminal keeps
  it and every higher-pid rival is closed with WM_CLOSE**. A scratch bar with no
  `terminal.className`/`terminal.title` will attach to the same terminal as the
  live bar and, if its pid happens to be lower, will close the live bar. Give
  the scratch config its own `terminal.className` **plus a unique
  `terminal.title`** so it can never attach to the user's terminal, or ensure
  the scratch pid is higher than the live bar's so it yields instead of evicting.
  Measured 2026-10-07: a scratch run evicted the live bar this way once.
- Also rename the single-instance mutex and the `HKCU` Run value in a scratch
  build if the copy must coexist with the live one.

After any test round on this repo, **confirm the live bar is still running**
(`Get-CimInstance Win32_Process -Filter "Name='chocobar.exe'"`); relaunch from
`C:\Users\tyanw\review\chocobar\native\chocobar.exe` if it is not.

If a test cannot run in isolation, do not run it against the live bar. Report
that instead of taking the user's screen.

## Release and deployment boundary

The user runs published releases; agents run isolated second-desktop builds.
Never build into, copy over or relaunch the user's Run path to test something.
Before any authorized release/deployment, read
[the single release/install procedure](docs/agent/RELEASE.md). It includes the
explicit local-build exception, release version checks and immediate relaunch
requirement. Do not independently maintain a second deployment recipe here.

The hazard this boundary exists for is a single script, measured 2026-10-08:
`C:\Users\tyanw\.wizbar\deploy_bar.ps1` does `Stop-Process` + `Copy-Item` over
the Run path + `Start-Process` onto the **active** desktop. An agent ran it and
put an unreviewed local build in front of the user mid-session. It installs a
LOCAL BUILD, never a release; do not use it to ship anything. Installs for the
user come only from a downloaded release asset, on the user's explicit request.

## Subscription credentials must never point at Pi's auth store

The legacy `pi-agy` extension (`~/.pi/agent/npm/node_modules/pi-agy`) stored its
OAuth credential under the `antigravity` key of `~/.pi/agent/auth.json`. It is
retired/disabled; do not re-enable it to supply the bar's credentials. **Do not point
`subs.providers[].authPath` at that file.** Logging out of Antigravity in pi
(which the user does deliberately, to keep Google models out of `/model`)
deletes that key, and the bar's chip immediately goes stale with
`subs agy: no login (open Antigravity once)` in `native.log`.

Measured 2026-10-08: that is exactly what happened. The bar's `authPath` was
`\\wsl.localhost\Ubuntu\home\tyan\.pi\agent\auth.json`.

**The bar keeps its own credential copy** at
`C:\Users\tyanw\.wizbar\antigravity.json`, in the shape the reader expects:
`{"antigravity": {access, refresh, expires, projectId, email, type}}`, with
`expires` in **epoch milliseconds** (the pi-agy store uses the same field; the
Antigravity *CLI* file `~/.gemini/antigravity-cli/antigravity-oauth-token` uses
an ISO string under `token.expiry`, which this reader does NOT accept).
`subsAgyReadAuth` reads it, `subsAgyRefresh` refreshes near-expiry tokens with
the provider's `clientId`/`clientSecret`, and `subsAgySaveAuth` writes the
rotated token back - so the copy self-maintains and never needs pi to log in.

To restore it after a pi logout, take the last good credential from a pi backup
(`~/.pi/agent/auth.json.bak-before-antigravity-logout-*`) and rewrite the bar's
file. Note the WSL path `~/.wizbar` is a **stale Sep-2026 leftover**, NOT the
live config directory; the live one is `C:\Users\tyanw\.wizbar`
(`/mnt/c/Users/tyanw/.wizbar`).

## Scratch-build state isolation

**The single-instance mutex is a fixed name** (`APP_MUTEX` =
`ChocobarSingleInstanceMutex`, `chocobar.c`), so a test copy CANNOT run at the
same time as the user's bar: it exits immediately with `ERROR_ALREADY_EXISTS`.
That exit is **not** a failure of the build, and stopping the user's bar to
"make room" is forbidden. A scratch run also has to neuter two more real
effects or it will damage user state from a throwaway build:
- `autoStartHeal()` rewrites the user's real `HKCU\...\Run\Chocobar` value on
  every non-fresh load when it points at a different exe, so a scratch exe path
  would repoint the user's autostart at the scratch build.
- the bar writes to `%USERPROFILE%\.wizbar` (log, cursors), so a scratch run
  needs a redirected `USERPROFILE` or it mixes with the real files.

The `second`-desktop scratch recipe that does all three is `~/.local/bin/cbdev`.

## Build and the compile gate

Build authority: [native/build.sh](native/build.sh). Release authority:
[release workflow](.github/workflows/release.yml) and
[version header](native/src/version.h). Reconcile these notes when they change.

```sh
bash native/build.sh            # -> native/chocobar.exe (system mingw if present, else nix pkgsCross.mingwW64)
bash native/build.sh --check    # compile-only: -fsyntax-only -Wall -Wextra, no link
bash native/build.sh --no-nix   # force the system mingw recipe (what CI/release.yml use)
```

`--check` is the C compiler gate pinned in `.no-mistakes.yaml` (`commands.lint`)
and in `.github/workflows/ci.yml`. It runs the same amalgamation step as a full
build, so it rewrites `native/src/chocobar_full.c`; that file is gitignored in
`native/.gitignore`, so the gate leaves no git-status noise. Keep it that way -
if a new generated artifact appears in the source tree, add it to that
`.gitignore` in the same change.

**Nothing else in this repo compiles the C.** The portable suite is pure
source/config text inspection and cannot see an out-of-bounds read, a type
error, or a missing declaration. Do not weaken or drop `--check`: it is the
only gate that catches compile-time C errors (types, declarations, signatures)
before anything runs. It cannot see runtime bugs - the `jobjGet` OOB read
fixed on 2026-10-07 was a runtime negative index that a `-fsyntax-only`
compile accepts cleanly; that bug class needs runtime validation, not this
gate.

## Assembling a run to review

The pipeline owns the branch and commits in its own worktree, so the checkout
here lags. Before reviewing "what shipped", fetch and read the pipeline head
(`git log FETCH_HEAD`) instead of local `git log` - the two can differ by
several commits until the run finishes. If local review-report edits land after
the pipeline already based a fix commit on the branch, push them as a
fast-forward on top of the pipeline head so no pipeline fix commit is dropped.
