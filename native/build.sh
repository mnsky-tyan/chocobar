#!/usr/bin/env bash
# Build the native Chocobar bar (x64 Windows exe) from WSL via nix mingw.
set -e
cd "$(dirname "$0")"

# Authoritative source-part list, in assembly order. The first entry is the base
# translation unit that carries the config marker; p_utils.c is spliced in at
# that marker and every remaining part is appended after it. Edit this list, not
# the assembled output.
PARTS=(
  src/chocobar.c
  src/p_utils.c
  src/p_metrics.c
  src/p_subs.c
  src/p_icons.c
  src/p_tokens.c
  src/p_ui.c
)

# assemble the single translation unit from the source parts (same order every time)
PARTS_JOINED="$(printf '%s\n' "${PARTS[@]}")" python3 - << 'PY'
import os
paths = os.environ['PARTS_JOINED'].splitlines()
base_path, rest = paths[0], paths[1:]
sources = {p: open(p).read() for p in rest}
base = open(base_path).read()
utils = sources.pop('src/p_utils.c')     # logging, string/config helpers
marker = '// --------------------------------------------------------------- config ----'
full = base.replace(marker, utils + '\n' + marker, 1)
# fail loudly: a marker typo would silently drop p_utils.c from the build
if full == base:
    raise SystemExit('config marker not found in %s: the amalgamation would omit p_utils.c' % base_path)
full += '\n' + '\n'.join(sources[p] for p in rest if p in sources)
open('src/chocobar_full.c', 'w').write(full)
PY
. ~/.nix-profile/etc/profile.d/nix.sh 2>/dev/null || true
export NIX_CONFIG="extra-experimental-features = nix-command flakes"
MCFGTHREAD_LIB=$(nix eval nixpkgs#pkgsCross.mingwW64.windows.mcfgthreads --raw)/lib
nix shell \
  nixpkgs#pkgsCross.mingwW64.buildPackages.gcc \
  nixpkgs#pkgsCross.mingwW64.buildPackages.binutils \
  nixpkgs#pkgsCross.mingwW64.windows.mcfgthreads \
  -c sh -c "x86_64-w64-mingw32-windres src/version.rc -o version.o && x86_64-w64-mingw32-gcc -O2 -municode -mwindows src/chocobar_full.c version.o -o chocobar.exe -Ivendor -I src -L$MCFGTHREAD_LIB -ldwmapi -lpdh -lole32 -lgdi32 -lwinhttp -lmsimg32 -luser32 -lshell32 -ladvapi32 -Wl,-Bstatic -lmcfgthread -Wl,-Bdynamic"
echo "built: $(ls -la chocobar.exe | awk '{print $5}') bytes"
