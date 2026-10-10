# Chocobar release, installation and deployment

Read before releasing, installing or restarting the live bar. Publishing or
deployment requires the user's requested scope; tests never stop the live bar.
The live Run path is `C:\Users\tyanw\review\chocobar\native\chocobar.exe`,
registered as `Chocobar` in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
This is not a build directory.

## Release artifact is the source of truth

The user's bar runs the newest published RELEASE asset, never an agent's local
work-in-progress build. Local builds run only in isolated second-desktop state
using the `cbdev` scratch recipe described in AGENTS.md.

Tag `v<major>.<minor>.<patch>` must equal `CB_VER_STR` in
`native/src/version.h`; the release workflow fails otherwise. Pushing a matching
version tag makes `.github/workflows/release.yml` build the Windows exe on the
runner and attach it to the release. The asset is reproducible from that tag
tree; it is the single source of truth for installation.

## Install or restart only when the user requests it

1. Find the newest published release with `gh release list` and download its
   asset, not a local build:
   `gh release download <tag> --pattern chocobar.exe --dir /tmp/reldl`.
2. Stop only the verified live bar instance for the authorized deployment, copy
   the downloaded asset over the Run path, then RELAUNCH IMMEDIATELY. Do not
   leave the user without the bar. Follow the global Windows launch/cleanup
   rules; never use name-wide process killing.
   `C:\Users\tyanw\.wizbar\install_release.ps1` is the existing downloaded-asset
   installer, not a build command. Inspect its process/launch behavior before
   using it under the current global Windows rules.
3. Confirm the bar process is running, the installed hash equals the release
   asset's hash, and `native.log` reports the expected version. Tell the user
   when it is back.
4. Do NOT use `C:\Users\tyanw\.wizbar\deploy_bar.ps1` to ship a build. That old
   script copies a local WSL build over the Run path and launches onto the active
   desktop. The only exception is an explicit user request to run that exact
   local build; even then, inspect/adapt its launch to the current desktop rules
   and verify the version in `native.log` afterwards.
5. Clearing `token-cursors.json` forces a full cold re-scan (recorded ~45 seconds
   over ~990 MB); the dashboard's temporary "No token data" is expected during
   that scan. Do not clear user state just to make a test pass.

## Previous failure, not a current deployment method

An agent once ran the old `deploy_bar.ps1`, installing an unreviewed local build
on the user's active desktop. This is why the release/local-build split and
explicit-deploy boundary exist. Do not copy that incident's command sequence as
a valid recipe.

Command examples are relative to the project root unless a path is absolute.
Bound every actual command/network operation and validate CI locally before any
authorized push, as required by the global instructions.
