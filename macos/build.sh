#!/usr/bin/env bash
# Build the macOS Chocobar bar.
#
# WHAT IS SHARED AND WHAT IS NOT
# The core translation unit is assembled the same way native/build.sh assembles
# the Windows one - the parts concatenated in order, so the core's cross-part
# `static` forward declarations resolve exactly as they do on Windows. That
# assembly is the whole portability mechanism: chocobar_core.h supplies the
# Win32 spellings and macos/src/adapter.m implements them on POSIX.
#
# Compiled here:
#   native/src/chocobar.c   config struct, JSONC parsing, jsmn helpers
#   native/src/p_utils.c    file slurp, encoding, colour helpers
#   native/src/p_tokens.c   the live JSONL session-store scanner + cursors
#   native/src/p_agg.c      the per-app/model/day aggregation (shared with Windows)
#   macos/src/adapter.m     the POSIX implementations of chocobar_core.h
#   macos/src/main.m        the bar itself
#
# The macOS-specific accessors into the core (cbTokAll, cbScanTokens, ...) live
# at the bottom of p_agg.c behind CHOCOBAR_PORTABLE, so the Windows translation
# unit gains no symbols and cannot collide with anything.
#
# NOT compiled here, deliberately:
#   p_metrics.c  wholly Win32-coupled (PDH_*, IMMDeviceEnumerator, Win32 battery
#                and RAM). A macOS build grows its own; pretending otherwise
#                would mean shipping zeros that look like real readings.
#   p_subs.c     WinHTTP + WinINet credential reads. Same story.
#   p_icons.c    its SVG PATH PARSER is portable, but the renderer is GDI/GDI+.
#                The parser is not needed until icons are drawn; when it is
#                brought across, only the renderer is new work.
#   p_ui.c       the Windows renderer and window manager. Replaced by main.m.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="macos/build"
mkdir -p "$OUT"

CORE_PARTS=(
  native/src/chocobar.c
  native/src/p_utils.c
  native/src/p_tokens.c
  native/src/p_agg.c
)

# --- assemble the core translation unit -----------------------------------------
# Same concatenation the Windows build does, plus two substitutions the Windows
# build does not need:
#   1. the Windows SDK includes become chocobar_core.h
#   2. CHOCOBAR_PORTABLE guards two `static void writeLogA` forward declarations
#      that are correct in the Windows single-TU build but would collide with
#      the adapter's real definition here.
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
          '<tlhelp32.h>', '<winhttp.h>'):
    full = full.replace('#include ' + h, '#include "chocobar_core.h"')
full = full.replace('#include <stdio.h>',
                    '#include "chocobar_core.h"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>', 1)
open(out, 'w').write(full)
print('core TU: %d lines' % len(full.splitlines()))
PY

# --- compile ---------------------------------------------------------------------
CFLAGS=(
  -O2
  -Wall
  -Wextra
  -Wno-unused-parameter
  -DCHOCOBAR_PORTABLE=1
  -Inative/src
  -Inative/vendor
  -mmacosx-version-min=13.0
)

echo "compiling the core + adapter..."
clang "${CFLAGS[@]}" -c "$CORE_TU" -o "$OUT/core.o"
clang "${CFLAGS[@]}" -c macos/src/adapter.m -o "$OUT/adapter.o"

echo "compiling the bar..."
clang "${CFLAGS[@]}" -c macos/src/main.m -o "$OUT/main.o"

echo "linking..."
clang -o "$OUT/chocobar-mac" \
  "$OUT/main.o" "$OUT/core.o" "$OUT/adapter.o" \
  -framework Cocoa -framework CoreGraphics

echo "built: $OUT/chocobar-mac ($(stat -f%z "$OUT/chocobar-mac" 2>/dev/null || stat -c%s "$OUT/chocobar-mac") bytes)"
