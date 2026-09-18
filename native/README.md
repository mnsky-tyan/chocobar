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

Phase 2 (not here yet): token analytics chips + dashboards, subscription
board, bluetooth battery, autostart writing. Until phase 2 lands, the
Electron bar stays the daily driver.

## Build (from WSL, cross-compiled)

```sh
native/build.sh        # -> native/chocobar.exe (~245 KB)
```

Requires Nix (`pkgsCross.mingwW64` gcc/binutils + mcfgthreads). `build.sh`
assembles `src/chocobar_full.c` from the four source parts, then compiles.
Edit the PARTS (`src/chocobar.c`, `src/p_utils.c`, `src/p_metrics.c`,
`src/p_ui.c`), never the assembled file.

## Run

```
chocobar-native.exe --config C:\Users\<you>\.wizbar\personal.json
```

Single-instance; a second launch exits silently. The window starts
off-screen at (-2000,-2000) and follows the foreground terminal once found.

## Sharp edges (learned the hard way - do not "simplify" these)

- **mingw headers**: `d2d1_1.h` and `dxgi1_2.h` provide working C bindings
  and macros (use `ID2D1Factory1_CreateDevice`,
  `ID2D1Device_CreateDeviceContext`,
  `ID2D1DeviceContext_CreateBitmapFromDxgiSurface`, `..._SetTarget`,
  `IDXGIFactory2_CreateSwapChainForComposition`). `dcomp.h` is BROKEN in C
  mode (duplicate overload members) - `src/p_ui.c` hand-declares those
  vtbls with slots verified against the header's interface blocks:
  Commit=3, CreateTargetForHwnd=6, CreateVisual=7, SetRoot=3,
  SetContent=15 (the offset/transform setter pairs occupy 3-14; the
  MSVC-C++ header orders each pair opposite to the C/IDL order - both
  place SetContent at 15).
- `ID2D1Resource::GetFactory` owns slot 3 on every D2D object; interface
  methods start at slot 4. RT vtbl = 4 base + 48 own = slots 0..51, so
  DeviceContext additions start at 52 (CreateBitmapFromDxgiSurface=57,
  SetTarget=69). ID2D1Factory1::CreateDevice = slot 17 (never 16).
- **Swapchain usage must be `DXGI_USAGE_RENDER_TARGET_OUTPUT` = 0x20**
  (0x40 is BACKBUFFER and makes the back buffer unusable as a D2D target:
  CreateBitmapFromDxgiSurface gives E_INVALIDARG for any explicit props).
- Swapchain: `CreateSwapChainForComposition`, FLIP_DISCARD(4),
  ALPHA_PREMULTIPLIED, 2 buffers, B8G8R8A8_UNORM.
- `GetBuffer` with `IID_IDXGISurface` returns E_NOINTERFACE on this path;
  fetch with `IID_IUnknown` (or IDXGIResource) and QueryInterface to
  `IDXGISurface` (`cafcb56c-b2ca-47bb-a495-08e6a41ace2b` - get GUIDs from
  the headers' `__CRT_UUID_DECL`, not from memory).
- Back-buffer bitmap options: `D2D1_BITMAP_OPTIONS_TARGET` alone works;
  adding `CANNOT_DRAW` compiles but makes BeginDraw fail with
  D2DERR_WRONG_STATE on every EndDraw.
- After any swapchain rebuild (WM_SIZE path), rebind the composition tree:
  `rebindVisual()` (SetContent + SetRoot + Commit) or the visual keeps
  pointing at the released swapchain and the bar goes silent.
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
