#!/bin/bash
# Product smoke for the Linux bar: the built binary starts on a private Xvfb
# display and a private D-Bus, with a throwaway HOME, opens its window, scans
# a seeded token store, and exits cleanly when stopped.
#
# This is NOT the behaviour suite (behaviour.sh, about 5 minutes). It is the
# short "the product runs end to end" check: the one to run after a build
# before anything slower, and the separate smoke a verification plan pairs
# with the full suite. It touches nothing of the user's - private display,
# private bus, throwaway HOME - and the bar is stopped by the PID captured
# here, never by name.
#
# Exit 0 = every check passed; 1 = a check failed or a tool is missing.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BIN="${1:-$ROOT/linux/build/chocobar-linux}"

[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (build it: bash linux/build.sh)"; exit 1; }
for t in Xvfb dbus-daemon xdotool xwininfo; do
    command -v "$t" >/dev/null || { echo "FAIL: $t is not installed"; exit 1; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/chocobar-smoke.XXXXXX") || exit 1
BAR_PID=""; DPID=""; XPID=""; DPYNUM=""
cleanup() {
    [ -n "$BAR_PID" ] && kill "$BAR_PID" 2>/dev/null
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    # reap our Xvfb before touching its socket: a server killed in its first
    # moments never reaches the code that unlinks it, and the next run would
    # skip the number as taken
    if [ -n "$XPID" ]; then
        kill "$XPID" 2>/dev/null
        wait "$XPID" 2>/dev/null
        [ -n "$DPYNUM" ] && rm -f "/tmp/.X11-unix/X$DPYNUM"
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0; FAIL=0
check() { # check <name> <condition-result>
    if [ "$2" -eq 0 ]; then echo "PASS: $1"; PASS=$((PASS+1))
    else echo "FAIL: $1"; FAIL=$((FAIL+1)); fi
}
# the bar's window, or nothing. Liveness is read off the window rather than
# kill -0: a crashed child of this script stays a zombie until reaped, and
# kill -0 still succeeds on a zombie.
bar_window() { xdotool search --class chocobar 2>/dev/null | head -1; }

# a seeded store: one pi record, 10000 tokens in total
mkdir -p "$WORK/home/.wizbar" "$WORK/store"
NOW=$(date +%s%3N)
printf '{"type":"assistant","usage":{"input":1000,"output":2000,"cacheRead":3000,"cacheWrite":4000},"model":"m","timestamp":%s}\n' \
    "$((NOW - 1000))" > "$WORK/store/usage.jsonl"
cat > "$WORK/home/.wizbar/config.json" <<JSON
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 0, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": true,
              "sources": [ { "app": "pi", "path": "$WORK/store", "recursive": false } ] },
  "modules": { "pet": { "enabled": false } }
}
JSON

# the private display and bus. The display number avoids the low numbers a
# real session uses, and an existing socket means the number is taken.
DPYNUM=$(( 150 + RANDOM % 20 ))
while [ -e "/tmp/.X11-unix/X$DPYNUM" ]; do DPYNUM=$((DPYNUM+1)); done
Xvfb ":$DPYNUM" -screen 0 1600x400x24 >"$WORK/xvfb.log" 2>&1 &
XPID=$!
export DISPLAY=":$DPYNUM"
for _ in $(seq 1 50); do xwininfo -root >/dev/null 2>&1 && break; sleep 0.2; done
xwininfo -root >/dev/null 2>&1 || { echo "FAIL: Xvfb did not come up"; exit 1; }
dbus-daemon --session --address="unix:path=$WORK/bus" --print-address --nofork \
    >"$WORK/dbus.log" 2>&1 &
DPID=$!
for _ in $(seq 1 50); do [ -S "$WORK/bus" ] && break; sleep 0.2; done
[ -S "$WORK/bus" ] || { echo "FAIL: the private bus did not come up"; exit 1; }

LOG="$WORK/home/.wizbar/native.log"
HOME="$WORK/home" DBUS_SESSION_BUS_ADDRESS="unix:path=$WORK/bus" \
    "$BIN" >"$WORK/bar.log" 2>&1 &
BAR_PID=$!

WIN=""
for _ in $(seq 1 100); do
    WIN=$(bar_window)
    [ -n "$WIN" ] && break
    sleep 0.1
done
[ -n "$WIN" ]; RC=$?
check "the bar opens its window on the private display" "$RC"

# the scan runs off-thread; its own log line is the observable
for _ in $(seq 1 150); do
    grep -q "token live scan: +10000 tokens" "$LOG" 2>/dev/null && break
    sleep 0.1
done
grep -q "token live scan: +10000 tokens" "$LOG" 2>/dev/null
check "the token scan reads the seeded store and logs its 10000 tokens" $?

[ -n "$(bar_window)" ]
check "the bar is still running after the scan" $?

kill "$BAR_PID" 2>/dev/null
for _ in $(seq 1 30); do [ -z "$(bar_window)" ] && break; sleep 0.1; done
[ -z "$(bar_window)" ]; RC=$?
check "the bar's window is gone once it is stopped" "$RC"
BAR_PID=""

echo
echo "--- smoke: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
