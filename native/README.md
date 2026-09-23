# Chocobar native (phase 1)

Native Win32 rewrite of the Electron bar. One C translation unit, no runtime
dependencies beyond Windows itself. Reads the SAME config file as the Electron
build (`--config <path>`, `WIZBAR_CONFIG`, or `%USERPROFILE%\.wizbar\config.json`);
keys it does not implement are ignored. Launch with the same `--config` the
Electron bar uses (`runbar.ps1` passes the personal config).

## Status

Phase 1 scope works end to end: acrylic bar, terminal follow with z-glue,
chips (cpu/cputemp/ram/volume/battery/clock + pet/shortcut/custom), tray,
config hot-reload, template materialization, single instance, no-activate
click behavior. Measured on the deployment machine: ~47 MB working set /
34 MB private / one process (Electron baseline was 408 MB / 245 MB / four
processes).

Phase 2 progress: the token analytics chips + dashboards and the
subscription board are DONE (`paintDash` / `dashToggle`, ported 1:1 from
`renderer/dash.css` + `subs.css`). Still open: bluetooth battery, autostart
writing.

## Build (from WSL, cross-compiled)

```sh
native/build.sh        # -> native/chocobar.exe (~245 KB)
```

Requires Nix (`pkgsCross.mingwW64` gcc/binutils + mcfgthreads). `build.sh`
assembles `src/chocobar_full.c` from the seven source parts, then compiles.
Edit the PARTS (`src/chocobar.c`, `src/p_utils.c`, `src/p_metrics.c`,
`src/p_subs.c`, `src/p_icons.c`, `src/p_tokens.c`, `src/p_ui.c`), never the
assembled file.

## Run

```
chocobar-native.exe --config C:\Users\<you>\.wizbar\personal.json
```

Single-instance; a second launch exits silently. The window starts
off-screen at (-2000,-2000) and follows the foreground terminal once found.

## Sharp edges (learned the hard way - do not "simplify" these)

- **Render path is GDI + UpdateLayeredWindow** (deliberate). The original
  D2D-over-DComp-swapchain pipeline is dead on this machine: with
  `D2D1_BITMAP_OPTIONS_TARGET` alone `CreateBitmapFromDxgiSurface` fails
  E_INVALIDARG on EVERY frame; adding `CANNOT_DRAW` makes it "succeed" but
  every EndDraw returns D2DERR_WRONG_STATE. Do not resurrect that design.
- The window is `WS_EX_LAYERED`; content = a 32bpp top-down premultiplied
  DIB selected into a memory DC (tint at `backgroundAlpha`, chips drawn
  right-aligned with DrawTextW), handed to DWM via `UpdateLayeredWindow`
  (`AC_SRC_ALPHA`). DWM backdrop attributes are ignored on layered
  windows - the tint alpha itself provides the translucency.
- **Layered windows never receive WM_PAINT.** The first frame must be
  drawn explicitly (`paint(g_bar)` in wWinMain after `initRender`);
  otherwise the bar stays invisible forever.
- The bar FOLLOWS the terminal, so screen captures at probed coordinates
  race the follow loop (stale rect = black/empty screenshots).
  `PrintWindow` on the `ChocobarBar` hwnd is the reliable verification.
- Icons are flattened SVG path data (viewBox 24, stroke-width 2.2), rendered
  with GDI+ (`SmoothingModeAntiAlias8x8`, round caps/joins) into per-icon
  premultiplied DIB caches and `AlphaBlend`ed - never hand-redraw them, port
  the exact path data from `renderer/bar.js`'s `ICONS`. One stroke color each
  (`theme.iconColor`, default pinkDeep); `theme.iconOpacity` rides `AlphaBlend`'s
  `SourceConstantAlpha` (default 90 = Electron's `.seg svg { opacity: .9 }`).
  Battery is a dynamic fill drawn by `svgDrawBatt`. Icon + space is drawn dim
  (`theme.fgDim`), the value in `theme.fg` / warn / override color - the
  two-tone look.
- `theme.fgDim` is a real config key (default `#5a5245`); the config
  jsmn walker counts key+value PAIRS (2N tokens) - never "fix" that.
- All wide strings go through `wideDup`/HeapFree; mixing `_wcsdup` with
  HeapFree caused a 0xC0000374 heap corruption once.
- A crash handler (`SetUnhandledExceptionFilter` -> `writeLogA`) appends
  the exception code to `native.log` next to the config; paint failures
  log there too. No other logging in steady state.
- `CreateWindowExW` starts the bar at (-2000,-2000); the follow tick moves
  it. Never paint assumptions before `followTick` has run.

## Verifying on the machine

`native_probe2.js` (koffi through electron-as-node, DPI-aware) checks the
bar exists, is visible, uncloaked, and sits directly above the terminal.
Screenshot verification only works while the session is UNLOCKED - when
locked, captures show the lock screen for every window, which once sent a
debugging session chasing phantom "invisible window" bugs. Kill the bar
only via `taskkill /IM chocobar-native.exe /F` (targeted; never blanket
taskkill powershell).
