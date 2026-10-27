# Chocobar on Linux

A floating translucent bar in the LOWER layer (under normal windows, the same
behaviour as the Windows bar) showing the same chips: pet, the AI token count
the shared scanner produces, the subscription gauge, cpu, ram, cpu temperature,
volume, battery, gpu and clock. With `bar.align: 1` it rides the **active
terminal**: it takes the terminal's width and horizontal position and sits just
above its top edge, so it reads as a strip along the top of that terminal.

![the bar](../docs/img/linux-bar.png)

## Build and run

```sh
sudo apt install libx11-dev libxrender-dev libxfixes-dev libxcursor-dev \
                 libcairo2-dev libdbus-1-dev libfontconfig1-dev \
                 libcurl4-openssl-dev libxinerama-dev
bash linux/build.sh                 # -> linux/build/chocobar-linux
./linux/build/chocobar-linux        # reads ~/.wizbar/config.json
./linux/build/chocobar-linux --config /path/to/config.json
```

The release asset (`chocobar-linux`) is built on the
`ubuntu-latest` runner and linked dynamically, so it needs that image's glibc
(2.39): Ubuntu 24.04, Debian 13 and up. An older distro loads nothing - the
loader stops on `GLIBC_2.39 not found` - and building it here, against the local
glibc, is the way to run it there. The README carries the same note next to the
release download.

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

### The gpu chip needs one capability

The gpu chip on Intel hardware reads the i915 PMU's engine-busy counters
(`rcs0-busy` and friends), which the kernel gates behind
`kernel.perf_event_paranoid` (4 on this machine: unprivileged opens are denied).
Rather than weaken that global setting, grant the permission to exactly this
binary with a file capability:

```sh
sudo setcap cap_perfmon+ep ~/.local/bin/chocobar-linux
```

Re-run it after every `cp` install - a fresh copy carries no capabilities. If
the capability is missing the gpu chip stays hidden and the bar logs the exact
command to run (in `~/.wizbar/native.log`). Nothing else in the bar needs any
privilege, and the capability cannot be used to read other processes' memory: it
only allows opening PMU counters. AMD GPUs need none of this - they publish
`gpu_busy_percent` as a plain sysfs file the bar reads unprivileged.

### "As long as the terminal" (`bar.align`)

| `bar.align` | meaning |
|---|---|
| `0` (default) | centre of the screen, content-sized |
| `1` | match the **active terminal's** width and x, and ride just above its top edge |
| `2` | span the whole screen width |

`align: 1` needs a helper, because a Wayland compositor does not let a client
read another window's geometry. Install it once:

```sh
chocobar-linux --install-geometry     # writes a KWin script and loads it
chocobar-linux --remove-geometry      # unload and delete it
```

Both refuse while the bar itself is running, because the running bar owns
`org.chocobar.Geom` and owns the helper: a second process loading a copy is what
stacks a second poller in KWin, and a second process cannot unload at all, so
`--remove-geometry` leaves the script file in place rather than deleting it and
reporting a removal that did not happen. Stop the service
(`systemctl --user stop chocobar`) first.

The helper (`linux/src/geomwatch.js`) is a KWin script that watches the active
window and calls the bar over D-Bus with its `frameGeometry` in logical pixels.
It recognises the terminal classes listed at the top of the script. Switching to
a non-terminal keeps the last terminal's geometry, so alt-tabbing does not move
the bar. A stale report (the script stopped, KWin restarted) is ignored after
30 s and the bar falls back to the screen-top position (`bar.gap` below the top
edge) rather than pinning itself to a window that no longer exists. The bar
never becomes narrower than its chips.

Left-click the tokens chip for the dashboard (a second click closes it), the pet
chip to start or stop the pet. Hovering a chip shows its title - except the pet
chip, whose own on/off text is the message - and hovering a heatmap cell on the
dashboard shows that day's totals and apps. Right-click opens the menu (the two
dashboards, the autostart toggle, edit the config, open its folder, the releases
page, reload, quit - the Windows tray menu's items in the same order).
The menu opens on the button RELEASE, which is what Windows does
(`WM_RBUTTONUP`): opening it on the press handed the menu its own opening
release, which landed in its padding corner and dismissed it a few milliseconds
after it appeared. `Esc`/`q` also quits when
the bar has focus. The config is the SAME `~/.wizbar/config.json` the Windows bar
reads, and it hot-reloads on save. The change is detected by the file's full
stat identity (seconds, nanoseconds and size), not by the mtime alone: a save
that is still in progress leaves the file truncated, and a whole-second mtime
cannot tell that empty file from the finished one, so the bar would read the
half-written config, keep the old one and never see the new one. With the full
identity the finished file still reads as a change, and the bar re-reads it.

"Start on login" toggles the systemd user unit (`systemctl --user
enable/disable chocobar`), the Linux counterpart of the Windows "Start with
Windows" Run value: the mark at the row's right edge is read at open time, and
the config file is never rewritten. "Check for updates" opens the GitHub
releases page, exactly as the Windows item does - it never downloads or swaps a
binary.

### The bar heals its own KWin outline exclusion

The bar paints its own hairline border in the theme divider colour, but one KDE
effect (`kwin4_effect_shapecorners`, the package `kwin-effect-roundcorners`)
strokes a 1px black outline over every managed window's edges - including
inactive ones, painted OVER the client's pixels. On the bar that outline rides
its border and shows as a continuous black line (the "broken border" defect).
Measured 2026-10-20: the bar's first row composited as (0,0,0) while the
window's own pixel there was a light alpha-154 border; unloading the effect
restored (239,229,216).

The fix is the effect's OWN exclusion list: `[Round-Corners] Exclusions` in
`~/.config/kwinrc`, keyed on window class (case-insensitive substring; the
bar's class is `chocobar Chocobar`). The bar appends `chocobar` to that key at
every startup (`linux/src/selfheal.c`) and asks KWin to reconfigure, so a fresh
install needs no manual step. It edits exactly that one key - appending to the
value when other exclusions exist, leaving the rest of the file byte-for-byte -
and does nothing when the exclusion is already in place.

## Measured on this machine (Ubuntu 25.10, KDE Plasma 6.6 on Wayland, 2880x1800)

| what | value |
|---|---|
| memory (RSS) | 50 MB with the subs gauge and libcurl (15 MB was without) |
| CPU, steady state | 0.2% of one core, 1 Hz repaint |
| threads | 2 (main + the subs fetch cycle); one more per extra enabled provider |
| binary | 1080 KB, no runtime |
| token scan, cold | ~1.5 s for 2,100 session records, then incremental |
| token scan, warm | a few ms (per-file byte cursors) |
| subs fetch | 147-184 ms per cycle for one provider (api.z.ai), every 2 min |

For comparison the Windows bar quotes ~30 MB and under 1% of total CPU.

## What is shared, what is not

The port is not a rewrite. `linux/build.sh` assembles the same core translation
unit the Windows and macOS builds do - `chocobar.c`, `p_utils.c`, `p_svg.c`,
`p_subs.c`, `p_tokens.c`, `p_agg.c` - and links it against `macos/src/adapter.m`,
which is the POSIX implementation of the Win32 file/dir/string/time/thread seam
and contains no Objective-C (the build compiles it as C, with `-x c`, on every
run). The scanner, the config parser, the subscription fetchers and the chip
rotation are therefore literally the same code, and the totals match the Windows
bar's because they are computed by the same function.

Per-OS parts, written fresh for Linux:

| part | Windows | Linux |
|---|---|---|
| metrics | `p_metrics.c` (PDH, Core Audio, HWiNFO) | `linux/src/p_metrics_linux.c` (`/proc`, `/sys`, `pactl`) |
| icons | `p_icons.c` (GDI+) | `linux/src/icons_cairo.c` (cairo) - same geometry from `p_svg.c` |
| window/paint | `p_ui.c` (layered GDI window) | `linux/src/main.c` (managed X11 window + cairo) |
| shared X helpers | - | `linux/src/linux_shape.c` (the rounded window region), `linux/src/linux_text.c` (wide-to-UTF-8, the symbol face) |
| subscriptions | `p_subs.c` (WinHTTP) | the SAME `p_subs.c`: its HTTP is a libcurl body under `CHOCOBAR_PORTABLE` |

## Why a managed X11 window in the lower layer

The bar is a MANAGED window carrying `_NET_WM_STATE_BELOW`, `_MOTIF_WM_HINTS`
(no decorations), `InputHint = False` (it can never take the keyboard) and
`USPosition` geometry (KWin honours our exact rectangle instead of placing the
window itself). That combination is the direct equivalent of the Windows bar:
on Windows the bar is pinned in z immediately above the terminal, so raising
another window covers the bar too - it is a lower-layer resident, not an
always-on-top overlay.

It is NOT override-redirect, and that is a measured fact, not a style choice:
KWin composites every override-redirect window in its top layer no matter which
state atoms it carries (probed 2026-10-19 on Plasma 6.6: BELOW, DESKTOP and
DOCK hint combinations all still painted over a raised terminal). The only X11
shape that lands under normal windows is a managed BELOW window. The price of
being managed - decorations, placement, focus - is exactly what those four
properties neutralise.

The bar is not draggable: its position is derived (x and width from the
terminal via `bar.align`, y from the terminal's top edge and `bar.gap`), and a
dragged position would break that contract.

On a Wayland session the window is presented through XWayland, which is how most
X11 applications still run on KDE. It is translucent for real: the window is on a
32-bit ARGB visual and its pixels carry an alpha channel, so the compositor
blends it.

**The bar asks the compositor for NO blur.** The config's `backdrop: "acrylic"`
is honoured as the translucent tint itself - the warm glass of the reference
screenshots IS the wallpaper showing through 47% of cream - and the backdrop
behind it is left raw. A blur-behind hint (`_KDE_NET_WM_BLUR_BEHIND_REGION`)
was tried and measured: on this desktop the compositor's blur effect flattened
the strip into a grey wash (the bar's empty middle measured std 8.4 with the
hint and 25.0 without it, while the reference screenshot measures 34 - the raw
backdrop). Readability never needed it: the strip sits above the terminal, not
over its text.

Right-click opens the menu on the button **release** (the Windows parity:
`WM_RBUTTONUP`), and the button that dismisses the menu is consumed by the
menu, not by the bar - Win32's `TrackPopupMenu` eats the dismissing click the
same way, so the bar never sees a release for it. Without that, the dismissing
right-click bounced the menu straight back open. The menu is drawn like the
Windows tray menu too (p_ui.c's owner-draw): the bar family at 9 CSS px, 21 px
rows, the widest label measuring the width, and the hovered row filled with
the background blended toward pink with a pinkDeep edge marker and pinkDeep
label - not a generic dark system popup.

## What each chip reads

| chip | source | note |
|---|---|---|
| pet | `/proc/<pid>/cmdline` | the process whose EXECUTED file is `modules.pet.exePath`: `argv[0]` equal to the path (or its `realpath`), or an interpreter `argv[0]` with the path at the FIRST token after it that is not one of the interpreter's own options. The kernel shebang rule is `<interpreter> [optional-arg] <script>`, so `#!/bin/bash -e` runs as `/bin/bash|-e|./pet` and the script is at argv[2], not argv[1]; the scan stops at that first non-option token, because every token after it is an ARGUMENT to the script and never the file the kernel executed - so `python3 report.py <path>` runs report.py and is not the pet. Options are parsed as getopt bundles against the interpreter's own grammar: a value-taking option consumes the next token (`#!/usr/bin/env -S python3 -X utf8` runs as `python3|-X|utf8|<script>`, and the script is still found), an option that takes a COMMAND rather than a script ends the scan in every spelling and bundle (`-c`, `-ec`, `-e`, `-r`, `-m`), so `sh -c <path>` and `sh -ec <path>` are not the pet either, and an option whose value behaviour is unknown also ends the scan without matching, so no unrelated process is credited. A relative token (the user started the pet as `./pet`) is resolved against that process's own cwd, so it still matches, and any other absolute spelling - a SYMLINK to the file, a redundant path such as `/x/./pet` - is compared by `realpath`, so the same file under its symlink is the pet too. The interpreter is matched by basename plus an optional version suffix, because that is the name the kernel gives the live process: `#!/usr/bin/env python3.14` runs as `python3.14`, and an exact-name list would read that pet as `off` while it runs. `env` is not in that list: env replaces ITSELF with the interpreter it resolved, so the live process is named after that interpreter, never after env. A process that merely carries the path as an argument - `tail -f <path>`, an editor with the file open - is NOT the pet, so the chip cannot read `on` for it and a click cannot stop it. The chip shows `on`/`off` and a click starts or stops the pet. The configured value must be one ABSOLUTE path to a regular executable file - a bare name, a relative path, or a directory cannot identify one process and is refused, for starting and for matching alike |
| cpu | `/proc/stat` delta | same arithmetic as `top` |
| ram | `/proc/meminfo` | used = 100 - MemAvailable/MemTotal |
| temp | hwmon `coretemp`/`k10temp`, then `x86_pkg_temp`, then thermal zones, then `acpitz` | prefers the CPU's own sensor over a lagging ACPI zone |
| volume | `pactl get-sink-volume` | the only source that runs a process; spawned once per second, whether or not the chip is enabled |
| battery | `/sys/class/power_supply/*` | green while charging, red only when low **and** on battery |
| gpu | `amdgpu`'s `gpu_busy_percent`, then the i915 PMU | Intel needs the PMU (see the capability note above); without the capability the chip hides |
| clock | local time, the config's `modules.clock.format` | same `{MMM} {dd} {Wkk} {HH}:{mm}` tokens |
| tokens | the shared JSONL session scan | the chip shows the all-time total; sources may be directories OR single ledger files |
| subs gauge | the shared `p_subs.c` providers | one plan's weekly remaining %, rotating per `subs.rotateSec`; same fetchers as Windows |

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

### The subscription gauge

The gauge chip fetches through `native/src/p_subs.c` - the SAME provider fetchers
the Windows bar runs (ChatGPT, Z.ai, antigravity, and config-only generic REST
endpoints). The Z.ai provider reads the API key out of the ZCode config the
desktop app already maintains, so no credentials live in this config:

```json
"subs": {
  "enabled": true,
  "intervalMinutes": 2,
  "rotateSec": 60,
  "providers": [
    { "type": "zai", "enabled": true, "label": "zai" }
  ]
}
```

The chip shows the selected plan's WEEKLY remaining percentage and advances to
the next enabled provider every `rotateSec` seconds. `>70%` is green, `>30%`
dim, below that red - the Windows thresholds. `stale` means every provider
failed its last fetch; an em dash means nothing has ever succeeded. The board
(Subscription dashboard in the right-click menu, or a click on the gauge chip)
is the Windows board ported: one full-width panel per enabled provider, one
donut per plan window with the credits left and the reset countdown, and the
cycle's fetched time in the foot. Its `refresh` re-fetches the providers and
its close button ends it, exactly as on Windows. Both boards are
override-redirect popups, which is what the Windows boards are too (owned
`WS_POPUP` windows): no titlebar, no taskbar/dock entry, no Alt-Tab entry, and
no focus steal on open - the keyboard stays with the terminal. They open
centred on the screen, both axes, and re-centre as their content-fit changes
the height.

## Tests

`npm test` runs two suites. `scripts/portable_regression.js` covers the shared
core and the Linux bar's structural invariants (the ARGB visual, `CWEventMask`,
the per-frame clear, the shape region, and that the icon geometry is shared
rather than copied). `linux/tests/behaviour.sh` is the suite that carries the
real behaviour guarantees: it starts the built binary on a private Xvfb display
with a throwaway `HOME`, a private D-Bus and a fake `systemctl`, drives the real
pointer with XTEST, and asserts what the user would see - the menu opens on the
button release and does not bounce, a press alone opens nothing, a click outside
the menu picks no row, the bar asks for no blur hint, it rides above the reported
terminal and hides/returns as that terminal moves, the pet chip starts and stops
the configured process (never a process that merely names it), the autostart row
reads its state once per open and refuses to flip an unreadable unit, the
Subscription row opens the real board (its close button ends it, a second
activation toggles it shut), and a left click on a right-group metric chip does
nothing (hovering one still shows its title). The helper-stack gate runs against
a stub `org.kde.KWin` (`linux/tests/kwstub.py`) that answers `isScriptLoaded`
and records every `unloadScript`/`loadScript` call, so the unload-before-load
contract is observed on the bus rather than read off the source; the lock that
keeps a second bar process out is exercised through the binary's own acquire
(`--self-test-lock`), never a shell re-implementation of it.

A missing tool (or the development headers the build needs - the bar's own
X11/cairo ones, and `libxtst-dev` for the XTEST driver the suite compiles) skips
the behaviour suite cleanly on a developer machine and fails it when `CI` is set,
so the gate cannot become a silent no-op. The KWin stub additionally needs
`python3-dbus` and `python3-gi`.

The executable token regressions (`scripts/token_regression.py`) are Windows-only;
on Linux the scan is exercised by running the bar against a real store, which is
how the totals quoted above were checked.
