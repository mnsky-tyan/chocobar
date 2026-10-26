#!/usr/bin/env bash
# Build the Linux Chocobar bar.
#
# WHAT IS SHARED AND WHAT IS NOT
# The core translation unit is assembled the same way native/build.sh (Windows)
# and macos/build.sh (macOS) assemble theirs - the parts concatenated in order, so
# the core's cross-part `static` forward declarations resolve identically. That
# assembly is the whole portability mechanism: chocobar_core.h supplies the Win32
# spellings and a POSIX adapter implements them.
#
#   native/src/chocobar.c   config struct, JSONC parsing, jsmn helpers
#   native/src/p_utils.c    file slurp, encoding, colour helpers
#   native/src/p_svg.c      chip icon path data + the flattener (shared geometry)
#   native/src/p_tokens.c   the live JSONL session-store scanner + cursors
#   native/src/p_agg.c      the per-app/model/day aggregation + the accessors
#
# The POSIX adapter is macos/src/adapter.m - the ONE implementation of the Win32
# file/dir/string/time seam on POSIX. It contains no Objective-C (verified: it
# compiles as C here, which is what this script does), so the Linux build reuses
# it rather than forking a copy. Two adapters would be two chances for the
# scanner to see a different file than the other platform.
#
# p_subs.c IS compiled here: the subscription gauge and board were Windows/mac
# only while their HTTP went through winhttp.h. That one function
# (subsHttpRequest) now carries a libcurl body under CHOCOBAR_PORTABLE and the
# rest of the part is threads + file reads + jsmn, all of which the adapter
# above provides. No forks, no copies: the same provider fetchers run on all
# three platforms.
#
# NOT compiled here, deliberately:
#   p_metrics.c  wholly Win32 (PDH_*, Core Audio, Win32 battery). linux/src/
#                p_metrics_linux.c is the real per-OS implementation.
#   p_icons.c    its RENDERER is GDI/GDI+. The geometry it used to own is now
#                p_svg.c (shared); linux/src/icons_cairo.c strokes that geometry
#                with cairo.
#   p_ui.c       the Windows window manager and renderer. Replaced by main.c.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="linux/build"
mkdir -p "$OUT"

CORE_PARTS=(
  native/src/chocobar.c
  native/src/p_utils.c
  native/src/p_svg.c
  native/src/p_subs.c
  native/src/p_tokens.c
  native/src/p_agg.c
)

# --- part-list drift guard -------------------------------------------------------
# native/build.sh calls its PARTS array "authoritative". This core list is a
# SUBSEQUENCE of it (p_metrics.c, p_subs.c, p_icons.c and p_ui.c are deliberately
# skipped) but the relative order must match, because the amalgamated translation
# unit resolves the core's cross-part `static` forward declarations by
# concatenation order. A part renamed or reordered on the Windows side would
# otherwise silently drop or misorder here; this makes it a hard error.
native_parts=$(sed -n '/^PARTS=(/,/^)/p' native/build.sh | grep -o 'src/[a-z_]*\.c' | sed 's|^|native/|')
prev=-1
for p in "${CORE_PARTS[@]}"; do
  idx=$(grep -nFx "$p" <<<"$native_parts" | cut -d: -f1 | head -n1)
  if [ -z "$idx" ]; then
    echo "FATAL: $p is not in native/build.sh's authoritative PARTS list" >&2
    exit 1
  fi
  if [ "$idx" -le "$prev" ]; then
    echo "FATAL: $p is out of order relative to native/build.sh's PARTS" >&2
    exit 1
  fi
  prev=$idx
done
echo "part list: subsequence of native/build.sh PARTS, order verified"

# --- assemble the core translation unit -------------------------------------------
# Same concatenation the Windows/macOS builds do, plus the two substitutions the
# portable builds need:
#   1. the Windows SDK includes become chocobar_core.h
#   2. CHOCOBAR_PORTABLE guards the `static void writeLogA` forward declarations
#      that are correct in the Windows single-TU build but would collide with the
#      adapter's real definition here.
CORE_TU="$OUT/core_tu.c"
python3 - "$CORE_TU" "${CORE_PARTS[@]}" <<'PY'
import sys
out = sys.argv[1]
paths = sys.argv[2:]
texts = {p: open(p).read() for p in paths}
base = texts.pop('native/src/chocobar.c')
utils = texts.pop('native/src/p_utils.c')
marker = '// --------------------------------------------------------------- config ----'
full = base.replace(marker, utils + '\n' + marker, 1)
if full == base:
    raise SystemExit('config marker not found: the amalgamation would omit p_utils.c')
full += '\n' + '\n'.join(texts[p] for p in paths[1:] if p in texts)
for h in ('<initguid.h>', '<windows.h>', '<windowsx.h>', '<dwmapi.h>', '<pdh.h>',
          '<shellapi.h>', '<mmdeviceapi.h>', '<endpointvolume.h>',
          '<tlhelp32.h>', '<winhttp.h>', '<winsock2.h>',
          '<iphlpapi.h>', '<ws2tcpip.h>'):
    full = full.replace('#include ' + h, '#include "chocobar_core.h"')
full = full.replace('#include <stdio.h>',
                    '#include "chocobar_core.h"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>', 1)
open(out, 'w').write(full)
print('core TU: %d lines' % len(full.splitlines()))
PY

# --- compile -----------------------------------------------------------------------
# The unused-symbol suppressions live ONLY on the core TU: it compiles a SUBSET of
# the Windows TU, so helpers the Windows renderer/providers use are genuinely
# unreferenced here. They stay in the shared source because the Windows build needs
# them; deleting them to quiet a warning would stop the platforms sharing code.
# The Linux-native files compile at full strictness - a dead symbol there is dead
# code that should be removed.
CORE_CFLAGS=(
  -O2 -g -Wall -Wextra
  -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
  -DCHOCOBAR_PORTABLE=1
  -Inative/src -Inative/vendor -Ilinux/src
)
APP_CFLAGS=(
  -O2 -g -Wall -Wextra
  -Wno-unused-parameter
  -DCHOCOBAR_PORTABLE=1
  -Inative/src -Inative/vendor -Ilinux/src
)
PKGS=(x11 xrender xfixes xcursor cairo dbus-1 fontconfig libcurl xinerama)
# xinerama: the dashboard's content fit asks which head the board is on so it
# never grows past a screen's bottom edge (dash.c dashFitToContent)

echo "compiling the core + POSIX adapter..."
gcc "${CORE_CFLAGS[@]}" -c "$CORE_TU" -o "$OUT/core.o"
# the adapter is C, not ObjC: -x c proves it on every build rather than in a comment
gcc "${APP_CFLAGS[@]}" -x c -c macos/src/adapter.m -o "$OUT/adapter.o"

echo "compiling the bar..."
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags "${PKGS[@]}") -c linux/src/main.c -o "$OUT/main.o"
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags "${PKGS[@]}") -c linux/src/linux_shape.c -o "$OUT/linux_shape.o"
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags "${PKGS[@]}") -c linux/src/icons_cairo.c -o "$OUT/icons_cairo.o"
gcc "${APP_CFLAGS[@]}" -c linux/src/p_metrics_linux.c -o "$OUT/p_metrics_linux.o"
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags dbus-1) -c linux/src/geomdbus.c -o "$OUT/geomdbus.o"
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags dbus-1) -c linux/src/selfheal.c -o "$OUT/selfheal.o"
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags "${PKGS[@]}") -c linux/src/menu.c -o "$OUT/menu.o"
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags "${PKGS[@]}") -c linux/src/dash.c -o "$OUT/dash.o"
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags "${PKGS[@]}") -c linux/src/tooltip.c -o "$OUT/tooltip.o"
gcc "${APP_CFLAGS[@]}" $(pkg-config --cflags "${PKGS[@]}") -c linux/src/subs.c -o "$OUT/subs.o"

echo "linking..."
gcc -o "$OUT/chocobar-linux" \
  "$OUT/main.o" "$OUT/linux_shape.o" "$OUT/icons_cairo.o" "$OUT/p_metrics_linux.o" "$OUT/geomdbus.o" "$OUT/selfheal.o" "$OUT/menu.o" "$OUT/dash.o" "$OUT/tooltip.o" "$OUT/subs.o" \
  "$OUT/core.o" "$OUT/adapter.o" \
  $(pkg-config --libs "${PKGS[@]}") -lm -lpthread

echo "built: $OUT/chocobar-linux ($(stat -c%s "$OUT/chocobar-linux") bytes)"
