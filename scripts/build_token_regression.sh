#!/usr/bin/env bash
# Build the console token-regression harness. This is the ONE executable copy
# of that compile command: CI and the README both invoke this script, so a
# library added to the product's link line reaches the harness in the same
# change instead of silently under-linking one of the copies.
#
# usage: scripts/build_token_regression.sh [output-path]
#   default output: native/token-regression.exe
set -euo pipefail
cd "$(dirname "$0")/.."

out="${1:-native/token-regression.exe}"
libs="-ldwmapi -lpdh -lole32 -lgdi32 -lwinhttp -lmsimg32 -luser32 -lshell32 -ladvapi32"
src="native/tests/token_regression.c"
inc="-Inative/vendor -Inative/src"

# Two toolchain recipes, matching native/build.sh's split:
#  - a plain mingw gcc on PATH (CI: Debian gcc-mingw-w64, winpthreads thread model)
#  - otherwise nix's pkgsCross.mingwW64, whose wrapper forces -lmcfgthread, so
#    the static lib must be on the link line (the exe imports no thread DLL).
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  exec x86_64-w64-mingw32-gcc -O2 -municode -mconsole "$src" $inc -o "$out" $libs
fi
MCFG="$(nix eval nixpkgs#pkgsCross.mingwW64.windows.mcfgthreads --raw)/lib"
exec nix shell nixpkgs#pkgsCross.mingwW64.stdenv.cc -c \
  x86_64-w64-mingw32-gcc -O2 -municode -mconsole "$src" $inc -o "$out" $libs \
  -L"$MCFG" -Wl,-Bstatic -lmcfgthread -Wl,-Bdynamic
