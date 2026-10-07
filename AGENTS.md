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

## Build

```sh
bash native/build.sh   # -> native/chocobar.exe (needs nix pkgsCross.mingwW64)
```
