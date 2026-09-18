#!/bin/bash
# Copy the WSL opencode.db to a Windows-accessible temp path for Chocobar's
# read-only scan (a live WAL store cannot be opened across the 9p boundary).
# Called from Windows via: wsl.exe -d <distro> --exec bash <this file>
# WIN_TEMP (Windows path of %TEMP%) is passed through the environment.
set -e
SRC="${OPENCODE_DB:-$HOME/.local/share/opencode/opencode.db}"
DEST_WSL=/tmp/wizbar-oc-copy.db
# WIN_TEMP (Windows path of %TEMP%) is passed through the environment by
# the Node side; there is no usable default on a stranger's machine.
WIN_TEMP="${WIN_TEMP:?WIN_TEMP env var not set (pass %TEMP% as a Windows path)}"
# Accept a native Windows path from the Node side: translate the drive
# letter and normalize separators ("C:\Users\x" -> "/mnt/c/Users/x").
case "$WIN_TEMP" in
  [A-Za-z]:*)
    drive=$(printf '%s' "${WIN_TEMP:0:1}" | tr '[:upper:]' '[:lower:]')
    WIN_TEMP="/mnt/$drive${WIN_TEMP:2}" ;;
esac
WIN_TEMP="${WIN_TEMP//\\//}"
DEST_WIN="$WIN_TEMP/wizbar-oc-copy.db"
python3 - "$SRC" "$DEST_WSL" <<'PY'
import sqlite3, sys
src = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
dst = sqlite3.connect(sys.argv[2])
src.backup(dst)
dst.close(); src.close()
PY
mkdir -p "$WIN_TEMP"
cp -f "$DEST_WSL" "$DEST_WIN"
echo "$DEST_WIN"
