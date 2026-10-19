# Chocobar on Linux

A floating, always-on-top, translucent bar showing the same chips as the Windows
bar: cpu, ram, cpu temperature, volume, battery, clock, and the AI token count
the shared scanner produces. It does **not** follow a terminal window around; it
sits where you put it vertically, and with `bar.align: 1` it takes the **active
terminal's width and horizontal position**, so it reads as a strip along the top
of that terminal.

![the bar](../docs/img/linux-bar.png)

## Build and run

```sh
sudo apt install libx11-dev libxrender-dev libxfixes-dev libcairo2-dev \
                 libdbus-1-dev
bash linux/build.sh                 # -> linux/build/chocobar-linux
./linux/build/chocobar-linux        # reads ~/.wizbar/config.json
./linux/build/chocobar-linux --config /path/to/config.json
```

### Run it supervised (recommended)

The bar is a small X11 client and a compositor restart can take it down. Install
it as a user service and it cannot stay gone:

```sh
cp linux/build/chocobar-linux ~/.local/bin/
cp linux/chocobar.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now chocobar.service
```

`Restart=always` with `StartLimitIntervalSec=0` means a kill, a crash, or an
Xwayland hiccup is followed by a restart within ~2 s, and a restart loop is never
given up on. Verified: `kill -9` on the running bar produced a new PID and an
active service 4 s later.

### "As long as the terminal" (`bar.align`)

| `bar.align` | meaning |
|---|---|
| `0` (default) | centre of the screen, content-sized |
| `1` | match the **active terminal's** width and x (the terminal look) |
| `2` | span the whole screen width |

`align: 1` needs a helper, because a Wayland compositor does not let a client
read another window's geometry. Install it once:

```sh
chocobar-linux --install-geometry     # writes a KWin script and loads it
chocobar-linux --remove-geometry      # unload and delete it
```

The helper (`linux/src/geomwatch.js`) is a KWin script that watches the active
window and calls the bar over D-Bus with its `frameGeometry` in logical pixels.
It recognises the terminal classes listed at the top of the script. Switching to
a non-terminal keeps the last terminal's geometry, so alt-tabbing does not move
the bar. A stale report (the script stopped, KWin restarted) is ignored after
30 s and the bar falls back to centring rather than pinning itself to a window
that no longer exists. The bar never becomes narrower than its chips.

Drag it with the left mouse button. Right-click quits. `Esc`/`q` also quits when
the bar has focus. The config is the SAME `~/.wizbar/config.json` the Windows bar
reads, and it hot-reloads on save.

## Measured on this machine (Ubuntu 25.10, KDE Plasma 6.6 on Wayland, 2880x1800)

| what | value |
|---|---|
| memory (RSS) | 15.4 MB |
| CPU, steady state | 0.17% of one core (5 ticks in 30 s), 1 Hz repaint |
| threads / fds | 1 / 4 |
| binary | 448 KB, no runtime |
| token scan, cold | ~1.5 s for 2,100 session records (297 M tokens), then incremental |
| token scan, warm | a few ms (per-file byte cursors) |

For comparison the Windows bar quotes ~30 MB and under 1% of total CPU.

## What is shared, what is not

The port is not a rewrite. `linux/build.sh` assembles the same core translation
unit the Windows and macOS builds do - `chocobar.c`, `p_utils.c`, `p_svg.c`,
`p_tokens.c`, `p_agg.c` - and links it against `macos/src/adapter.m`, which is the
POSIX implementation of the Win32 file/dir/string/time seam and contains no
Objective-C (the build compiles it as C, with `-x c`, on every run). The scanner,
the config parser and the aggregation are therefore literally the same code, and
the token totals match the Windows bar's because they are computed by the same
function.

Per-OS parts, written fresh for Linux:

| part | Windows | Linux |
|---|---|---|
| metrics | `p_metrics.c` (PDH, Core Audio, HWiNFO) | `linux/src/p_metrics_linux.c` (`/proc`, `/sys`, `pactl`) |
| icons | `p_icons.c` (GDI+) | `linux/src/icons_cairo.c` (cairo) - same geometry from `p_svg.c` |
| window/paint | `p_ui.c` (layered GDI window) | `linux/src/main.c` (X11 override-redirect + cairo) |
| subscriptions | `p_subs.c` (WinHTTP) | not ported: the config parses, nothing fetches |

## Why an X11 window, and how it behaves on Wayland

The bar is an override-redirect X11 window, which means the window manager does
not manage it: no title bar, no taskbar entry, no focus, and it sits exactly
where it is put. That is the direct equivalent of the Windows bar's
`WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST` combination, and it is what
makes "floating" a non-problem rather than a protocol negotiation.

On a Wayland session the window is presented through XWayland, which is how most
X11 applications still run on KDE. It is translucent for real: the window is on a
32-bit ARGB visual and its pixels carry an alpha channel, so the compositor
blends it. `_KDE_NET_WM_BLUR_BEHIND_REGION` is set so KWin blurs what is behind
the strip, which is what keeps the text readable over a busy terminal.

The one thing this design cannot do is follow a terminal window: a Wayland
compositor does not let a client position itself relative to another window, and
an X11 client cannot see Wayland-native windows at all. Following was left out on
purpose - see the project README's feasibility notes.

## What each chip reads

| chip | source | note |
|---|---|---|
| cpu | `/proc/stat` delta | same arithmetic as `top` |
| ram | `/proc/meminfo` | used = 100 - MemAvailable/MemTotal |
| temp | hwmon `coretemp`/`k10temp`, then `x86_pkg_temp`, then thermal zones, then `acpitz` | prefers the CPU's own sensor over a lagging ACPI zone |
| volume | `pactl get-sink-volume` | the only source that runs a process; once per second, only while enabled |
| battery | `/sys/class/power_supply/*` | green while charging, red only when low **and** on battery |
| gpu | `amdgpu`'s `gpu_busy_percent` | Intel iGPU exposes no utilisation file, so this machine shows no gpu chip |
| clock | local time, the config's `modules.clock.format` | same `{MMM} {dd} {Wkk} {HH}:{mm}` tokens |
| tokens | the shared JSONL session scan | the chip shows the all-time total; sources may be directories OR single ledger files |

### Token sources, including magpie and API-key traffic

`sources[].path` accepts a **directory** (a tree of session `.jsonl` files) or a
**single `.jsonl` file** (a tool that keeps one ledger). A file path was silently
ignored before - the walk only enumerated directories - so a configured ledger
source contributed nothing.

To count magpie's traffic and API-key usage under one name:

```json
"tokens": {
  "enabled": true,
  "labels": { "pi": "pi" },
  "sources": [
    { "app": "pi", "path": "~/.config/magpie/usage.jsonl", "enabled": true,
      "recursive": false, "timestampFormat": "iso",
      "fields": { "input": "in", "output": "out", "cacheRead": "cache_read",
                  "cacheWrite": "cache_write", "timestamp": "t" },
      "providers": ["workbuddy-ai", "workbuddy", "opencode-zen-free", "cline",
                    "qoder", "qoder-cn", "antigravity", "kilo", "zcode"] },
    { "app": "pi", "path": "~/.pi/agent/sessions", "enabled": true,
      "recursive": true, "exclude": ["magpie"] },
    { "app": "pi", "path": "~/.zai/agent/sessions", "enabled": true }
  ]
}
```

The `exclude: ["magpie"]` is what makes the total right, not larger: pi's own
transcripts already record every magpie-routed request, so counting them in both
places would double them. Measured on this machine: magpie's ledger plus pi's
non-magpie routes equalled the bar's total EXACTLY (382,751,319 = 382,751,319).

A sensor that does not exist reads as an em dash, never as a fabricated 0.

## Tests

`npm test` covers the shared core and the Linux bar's structural invariants (the
ARGB visual, `CWEventMask`, the per-frame clear, the shape region, and that the
icon geometry is shared rather than copied). Each of those guards exists because
its absence produced a bar that looked right and did not work.

The executable token regressions (`scripts/token_regression.py`) are Windows-only;
on Linux the scan is exercised by running the bar against a real store, which is
how the totals quoted above were checked.
