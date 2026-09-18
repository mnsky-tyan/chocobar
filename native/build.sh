#!/usr/bin/env bash
# Build the native Chocobar bar (x64 Windows exe) from WSL via nix mingw.
set -e
cd "$(dirname "$0")"

# assemble the single translation unit from the source parts (same order every time)
python3 - << 'PY'
base = open('src/chocobar.c').read()      # entry, config parser, wWinMain
utils = open('src/p_utils.c').read()      # logging, string/config helpers
metrics = open('src/p_metrics.c').read()  # cpu/ram/temp/volume/battery/gpu/pet polls
ui = open('src/p_ui.c').read()            # D2D/DComp render, window, follow, tray
marker = '// --------------------------------------------------------------- config ----'
full = base.replace(marker, utils + '\n' + marker, 1)
full += '\n' + metrics + '\n' + ui
open('src/chocobar_full.c', 'w').write(full)
PY
. ~/.nix-profile/etc/profile.d/nix.sh 2>/dev/null || true
export NIX_CONFIG="extra-experimental-features = nix-command flakes"
MCFGTHREAD_LIB=$(nix eval nixpkgs#pkgsCross.mingwW64.windows.mcfgthreads --raw)/lib
nix shell \
  nixpkgs#pkgsCross.mingwW64.buildPackages.gcc \
  nixpkgs#pkgsCross.mingwW64.buildPackages.binutils \
  nixpkgs#pkgsCross.mingwW64.windows.mcfgthreads \
  -c sh -c "x86_64-w64-mingw32-gcc -O2 -municode src/chocobar_full.c -o chocobar.exe -Ivendor -L$MCFGTHREAD_LIB -ldwmapi -ld2d1 -ldwrite -lpdh -lcomctl32 -lole32 -luuid -lgdi32 -ld3d11 -ldxgi -ldcomp -Wl,-Bstatic -lmcfgthread -Wl,-Bdynamic"
echo "built: $(ls -la chocobar.exe | awk '{print $5}') bytes"
