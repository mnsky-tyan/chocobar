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
- A copy started from a different path in an isolated profile does not fight the
  live mutex in any way that matters, but if a test *must* have the mutex,
  test the component or a scratch build - do not evict the user's bar.

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
