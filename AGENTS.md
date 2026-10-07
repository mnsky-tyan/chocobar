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

## Deploy / restart, if the live bar genuinely must be restarted

Only when the user asks for a deploy:

1. `powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\Users\tyanw\.wizbar\deploy_bar.ps1`
   (stops the old copy, copies the fresh build, clears the cursor file, relaunches).
2. **Relaunch immediately and verify** - the script stops the bar, and the user
   is left with nothing until a process is running again. Confirm with
   `Get-CimInstance Win32_Process -Filter "Name='chocobar.exe'"` and tell the
   user it is back.
3. Clearing `token-cursors.json` forces a full cold re-scan (~45 s over ~990 MB),
   during which the dashboard shows "No token data" - that is expected, not a
   failure.

## Build and the compile gate

```sh
bash native/build.sh            # -> native/chocobar.exe (needs nix pkgsCross.mingwW64)
bash native/build.sh --check    # compile-only: -fsyntax-only -Wall -Wextra, no link
```

`--check` is the C compiler gate pinned in `.no-mistakes.yaml` (`commands.lint`)
and in `.github/workflows/ci.yml`. It runs the same amalgamation step as a full
build, so it rewrites `native/src/chocobar_full.c`; that file is gitignored in
`native/.gitignore`, so the gate leaves no git-status noise. Keep it that way -
if a new generated artifact appears in the source tree, add it to that
`.gitignore` in the same change.

**Nothing else in this repo compiles the C.** The portable suite is pure
source/config text inspection and cannot see an out-of-bounds read, a type
error, or a missing declaration. Do not weaken or drop `--check`: it is the only
gate that would have caught the `jobjGet` OOB read fixed on 2026-10-07.

## Assembling a run to review

The pipeline owns the branch and commits in its own worktree, so the checkout
here lags. Before reviewing "what shipped", fetch and read the pipeline head
(`git log FETCH_HEAD`) instead of local `git log` - the two can differ by
several commits until the run finishes. If local review-report edits land after
the pipeline already based a fix commit on the branch, push them as a
fast-forward on top of the pipeline head so no pipeline fix commit is dropped.
