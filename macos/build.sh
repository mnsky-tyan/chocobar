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

# --- part-list drift guard -------------------------------------------------------
# native/build.sh calls its PARTS array "authoritative". The macOS core list is
# a SUBSEQUENCE of it (this build deliberately skips p_metrics.c and p_subs.c,
# which are wholly Win32-coupled) but the relative order must match, because the
# amalgamated translation unit resolves the core's cross-part `static` forward
# declarations by concatenation order. A part renamed or reordered on the
# Windows side would otherwise silently drop or misorder here; this makes it a
# hard error. (Windows entries are "src/x.c" relative to native/.)
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
# The unused-symbol suppressions live ONLY on the core translation unit: it
# compiles a SUBSET of the Windows TU, so helpers the renderer and provider
# readers use are genuinely unreferenced here (wallNowLocalMs, cfgPin on the
# Windows side, appLabelW until macOS draws a dashboard). They stay in the
# shared source because the Windows build needs them; deleting them to quiet a
# warning would stop the platforms sharing code. adapter.m and main.m are
# macOS-native files where EVERY symbol should earn its place, so they compile
# at full strictness - that is what surfaced the dead code the review found.
CORE_CFLAGS=(
  -O2
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-unused-function
  -Wno-unused-variable
  -DCHOCOBAR_PORTABLE=1
  -Inative/src
  -Inative/vendor
  -mmacosx-version-min=13.0
)
APP_CFLAGS=(
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
clang "${CORE_CFLAGS[@]}" -c "$CORE_TU" -o "$OUT/core.o"
clang "${APP_CFLAGS[@]}" -c macos/src/adapter.m -o "$OUT/adapter.o"

echo "compiling the bar..."
clang "${APP_CFLAGS[@]}" -c macos/src/main.m -o "$OUT/main.o"

echo "linking..."
clang -o "$OUT/chocobar-mac" \
  "$OUT/main.o" "$OUT/core.o" "$OUT/adapter.o" \
  -framework Cocoa -framework CoreGraphics

echo "built: $OUT/chocobar-mac ($(stat -f%z "$OUT/chocobar-mac" 2>/dev/null || stat -c%s "$OUT/chocobar-mac") bytes)"

# --- bundle ----------------------------------------------------------------------
# A bare binary works from a terminal, but a .app is what a person can actually
# run: double-click, and the accessory activation policy keeps it out of the
# Dock. LSUIElement in the Info.plist is the bundle-level version of that policy,
# so even a plain `open` shows no Dock icon.
APP="$OUT/Chocobar.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$OUT/chocobar-mac" "$APP/Contents/MacOS/Chocobar"
# The bundle's version comes from the same single source of truth the Windows
# asset and every release tag use (release.yml refuses a tag that disagrees
# with it), so a downloaded mac artifact can never report a version frozen at
# 0.1.0.
CB_VER=$(sed -n 's/^#define CB_VER_STR *"\(.*\)"/\1/p' native/src/version.h)
# sed exits 0 with EMPTY output when the pattern misses (a tab separator, a
# trailing comment, the define moved off column 0), and an empty value would be
# written verbatim into both plist version keys - the wrong-version artifact
# this extraction exists to prevent. Fail loudly instead. Validate the SHAPE
# too: a whitespace-only or otherwise non-version value would sail past a bare
# -n test and still ship a broken CFBundleShortVersionString.
[ -n "$CB_VER" ] || { echo "FATAL: cannot read CB_VER_STR from native/src/version.h" >&2; exit 1; }
# every dot-separated component must be one or more digits, with at least one
# dot (so '1..4', '1.', '.1', '' and '1' are all rejected, and nothing with a
# shell or XML metacharacter can reach the plist)
case "$CB_VER" in
    *[!0-9.]*) echo "FATAL: CB_VER_STR '$CB_VER' is not a dotted version" >&2; exit 1 ;;
esac
OLDIFS=$IFS; IFS=.; set -- $CB_VER; IFS=$OLDIFS
[ $# -ge 2 ] || { echo "FATAL: CB_VER_STR '$CB_VER' is not a dotted version" >&2; exit 1; }
for part in "$@"; do
    case "$part" in
        ''|*[!0-9]*) echo "FATAL: CB_VER_STR '$CB_VER' is not a dotted version" >&2; exit 1 ;;
    esac
done
# quoted heredoc: the version is injected by one explicit substitution below,
# so a CB_VER containing $(...), backticks, & or < cannot be executed by the
# shell or emit an unparseable plist.
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>                <string>Chocobar</string>
    <key>CFBundleDisplayName</key>         <string>Chocobar</string>
    <key>CFBundleIdentifier</key>          <string>com.mnsky.chocobar</string>
    <key>CFBundleExecutable</key>          <string>Chocobar</string>
    <key>CFBundlePackageType</key>         <string>APPL</string>
    <key>CFBundleShortVersionString</key>  <string>@CB_VER@</string>
    <key>CFBundleVersion</key>             <string>@CB_VER@</string>
    <key>LSMinimumSystemVersion</key>      <string>13.0</string>
    <!-- Accessory app: no Dock icon, no menu bar takeover. The bar is a
         floating panel, not an application the user manages. -->
    <key>LSUIElement</key>                 <true/>
    <key>NSHighResolutionCapable</key>     <true/>
</dict>
</plist>
PLIST
# single explicit substitution, quoted so the value is never word-split or globbed.
# python3 is used only for a literal string replace; a missing interpreter must
# fail loudly, not leave @CB_VER@ in the shipped plist.
command -v python3 >/dev/null 2>&1 || { echo "FATAL: python3 not found (needed to stamp the bundle version)" >&2; exit 1; }
python3 - "$APP/Contents/Info.plist" "$CB_VER" <<'PY'
import sys
path, ver = sys.argv[1], sys.argv[2]
s = open(path).read()
if '@CB_VER@' not in s:
    sys.exit('FATAL: @CB_VER@ placeholder missing from Info.plist')
open(path, 'w').write(s.replace('@CB_VER@', ver))
PY
codesign --force --sign - "$APP" 2>/dev/null || true   # ad-hoc: no identity needed
ditto -c -k --keepParent "$APP" "$OUT/Chocobar-mac.zip"
echo "bundled: $OUT/Chocobar.app -> $OUT/Chocobar-mac.zip ($(stat -f%z "$OUT/Chocobar-mac.zip" 2>/dev/null || stat -c%s "$OUT/Chocobar-mac.zip") bytes)"
