#!/bin/bash
# Behaviour test for the Linux bar.
#
# The source guards in scripts/portable_regression.js assert TEXT; this script
# asserts BEHAVIOUR: it starts the real binary on a private Xvfb display, drives
# the real pointer with XTEST and checks what the user would see.
#
# It is the regression net for the traps that kept biting:
#   - the menu must open on the button RELEASE (Windows parity: WM_RBUTTONUP)
#   - a release that ends off the bar opens nothing
#   - the dismissing right-click must close the menu and NOT bounce it open
#   - only a click INSIDE the menu may pick a row
#   - only the LEFT button may pick a row: a wheel notch or a middle click
#     over a row must pick nothing and close the menu
#   - the bar must NOT ask the compositor for a blur (it flattens the strip)
#   - the bar must ride above the terminal, and hide when there is no room
#   - every map of the bar - the startup one and the tick's after a born-hidden
#     start - must make the EWMH all-desktops request, or that bar lives on one
#     desktop with DEMANDS_ATTENTION set
#   - the pet chip starts and stops the configured process, by the process's
#     EXECUTED file - never a process name, and never a process that merely
#     carries the path as an argument - including an env-shebang script the
#     user starts from its own directory with a relative path, a version-pinned
#     interpreter, a shebang that carries an argument (the script sits at
#     argv[2] there), a SYMLINK to the configured file and a redundant absolute
#     spelling of it, and never a process that carries the path as an ARGUMENT
#     to an interpreter - a later token, a "-c" or bundled "-ec" command, a
#     `node -e` / `perl -e` command, an `sh -n` syntax check, or a relative
#     name - while a value-taking option before the script
#     (`#!/usr/bin/env -S python3 -X utf8`) is still seen;
#     and the children it leaves behind are reaped, not
#     zombies; a start is logged only once the kernel took the exec, and a
#     refused one logs "cannot start" and claims no start at all
#   - the click that starts the pet never waits on the child's exec: a slow
#     exec leaves the bar ticking, answering a right-click and following a
#     fresh terminal report while the handshake pends, and the outcome still
#     lands as the one true line
#   - the parked handshake is BOUNDED: a click refused while it pends is logged
#     rather than swallowed, and a start that never reports is abandoned and
#     logged, leaving the bar free to try again - but the budget counts only
#     time the loop was free to drain, so a menu held open past the deadline
#     does not abandon a pending start
#   - the bar rides above the terminal at the height it is actually DRAWN with,
#     so a tiny configured height cannot overlap the terminal, and hides when
#     there is no room
#   - the scale every expected pixel value is derived from is the one the bar
#     itself detects off the server - the root's Xft.dpi resource, else the
#     screen's physical DPI - never a ratio assumed off the bar's rounded
#     height, so the pixel bands keep their margin under any DPI the server
#     reports
#   - the autostart mark is read once per menu open, the child's stderr pipe
#     cannot block the bar's loop, and a width change moves the window and its
#     rounded shape together
#
# Nothing here touches the user's session: the display is a private Xvfb, the
# HOME is a throwaway directory, the D-Bus is a private daemon and systemctl is
# a fake that fails on purpose. A missing tool skips the run on a developer
# machine, but FAILS it when CI is set: GitHub Actions exports CI=true, and the
# gate exists to behaviour-test the bar, so it must not become a silent no-op.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

skip() {
    if [ "${CI:-}" = "true" ]; then
        echo "FAIL: $1 (CI must not skip the Linux behaviour gate)"
        exit 1
    fi
    echo "SKIP: $1"
    exit 0
}
command -v Xvfb      >/dev/null || skip "Xvfb is not installed"
command -v dbus-daemon >/dev/null || skip "dbus-daemon is not installed"
command -v xwininfo  >/dev/null || skip "xwininfo (x11-utils) is not installed"
command -v xprop     >/dev/null || skip "xprop (x11-utils) is not installed"
command -v xdpyinfo  >/dev/null || skip "xdpyinfo (x11-utils) is not installed"
command -v xdotool   >/dev/null || skip "xdotool is not installed"
command -v gdbus     >/dev/null || skip "gdbus (libglib2.0-bin) is not installed"
command -v dbus-send >/dev/null || skip "dbus-send is not installed"
command -v gcc      >/dev/null || skip "gcc is not installed"
# the KWin Scripting stub is Python: the helper-stack check drives it instead of
# reading the reload path's source, because the unload-before-load contract is
# only observable against a service that answers isScriptLoaded
command -v python3  >/dev/null || skip "python3 is not installed"
python3 -c 'import dbus, gi' >/dev/null 2>&1 \
    || skip "python3-dbus and python3-gi are not installed"

# The suite BUILDS things: its own XTEST driver and shims on every run, and the
# bar itself when it is not built yet. Both need DEVELOPMENT headers, not just
# the runtime tools above. A developer machine without them used to fail
# `npm test` outright, which contradicts the promise that a missing prerequisite
# skips cleanly here and fails in CI. A machine that HAS the headers and still
# fails to build keeps the hard FAIL below.
buildable() { # buildable <pkg-config module>...
    command -v pkg-config >/dev/null || return 1
    for p in "$@"; do
        pkg-config --exists "$p" || return 1
    done
    return 0
}

# the driver is compiled on EVERY run, so its headers are probed on every run too:
# a bar built by an earlier run must not turn a missing libxtst-dev into a hard
# FAIL on a developer machine
buildable x11 xtst || skip "the X11/XTest development headers (libx11-dev, libxtst-dev) are not installed"

BIN="$ROOT/linux/build/chocobar-linux"
if [ ! -x "$BIN" ]; then
    buildable x11 xrender xfixes xcursor cairo dbus-1 fontconfig libcurl \
        || skip "the X11/cairo development headers (pkg-config) are not installed"
    echo "--- building the bar"
    bash "$ROOT/linux/build.sh" >/dev/null || { echo "FAIL: the bar does not build"; exit 1; }
fi
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN"; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/chocobar-behave.XXXXXX") || exit 1
export DBUS_SESSION_BUS_ADDRESS="unix:path=$WORK/bus"
XCLICK="$WORK/xclick"
XMON="$WORK/xmonitor"
CLIENTMSG="$WORK/clientmsg.log"   # one line per root-window ClientMessage
CALLS="$WORK/systemctl-calls"   # the fake systemctl appends one line per call

cleanup() {
    [ -n "${BAR_PID:-}" ] && kill "$BAR_PID" 2>/dev/null
    [ -n "${DECOY_PID:-}" ] && kill "$DECOY_PID" 2>/dev/null
    [ -n "${ARGDECOY_PID:-}" ] && kill "$ARGDECOY_PID" 2>/dev/null
    for p in ${SHAPED_PIDS:-}; do kill "$p" 2>/dev/null; done
    [ -n "${MON_PID:-}" ] && kill "$MON_PID" 2>/dev/null
    if [ -s "$WORK/pet.pid" ]; then kill "$(cat "$WORK/pet.pid")" 2>/dev/null; fi
    [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null
    [ -n "${DPID:-}" ] && kill "$DPID" 2>/dev/null
    for p in ${KWBAR:-} ${KWSTUB_PID:-} ${KWDPID:-} ${SECOND_BAR:-} ${HEATXPID:-}; do
        [ -n "$p" ] && kill "$p" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0; FAIL=0
check() { # check <name> <condition-result>
    if [ "$2" -eq 0 ]; then echo "PASS: $1"; PASS=$((PASS+1))
    else echo "FAIL: $1"; FAIL=$((FAIL+1)); fi
}
# Wait for a line to appear in the bar's log, up to <timeout> seconds. The bar
# picks a config change up on its OWN 2 s config check and only then logs
# what it decided, so a fixed sleep after writing the config races that
# cadence: a check that sleeps a constant and then greps is a check that
# sometimes fails for no reason. Waiting for the OBSERVABLE is what the check
# actually means.
wait_log() { # wait_log <pattern> <timeout-seconds> [log]
    local pat="$1" limit="$2" log="${3:-$WORK/bar.log}" waited=0
    while [ "$waited" -lt "$((limit * 10))" ]; do
        grep -q "$pat" "$log" && return 0
        sleep 0.1
        waited=$((waited+1))
    done
    return 1
}

# ---- the throwaway environment -------------------------------------------
mkdir -p "$WORK/home/.wizbar" "$WORK/fakebin" "$WORK/decoy"
# the pet module is FIRST (the leftmost left-group chip) and the tokens chip is
# disabled, so every left click on the bar either hits the pet chip or is inert
# - the pet chip can be FOUND by clicking x offsets and watching for the bar's
# own "pet: starting" log line
cat > "$WORK/home/.wizbar/config.json" <<JSON
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 1, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": false },
  "modules": { "pet": { "enabled": true, "label": "pet",
                          "exePath": "$WORK/home/pet" } }
}
JSON
# the fake pet: an ENV-shebang script (the most common shebang form), so the
# matcher must see through `env` to the script the kernel finally execs. It
# records its pid, traps SIGTERM and records that SIGTERM was what stopped it
# (a SIGKILL would leave the pid file in place and the state at "running"),
# then sleeps
cat > "$WORK/home/pet" <<PET
#!/usr/bin/env bash
echo \$\$ > "$WORK/pet.pid"
echo running > "$WORK/pet.state"
trap 'echo SIGTERM > "$WORK/pet.state"; rm -f "$WORK/pet.pid"; exit 0' TERM
while :; do sleep 0.2; done
PET
chmod +x "$WORK/home/pet"
# the decoy: a sleeper whose executable is NAMED like the pet's basename
# ("pet", so its comm is the pet's basename) but whose cmdline carries a
# DIFFERENT path - exactly the process a name-based match would kill and an
# exact-executed-file match must leave alone
printf '#include <unistd.h>\nint main(void){for(;;)pause();}\n' > "$WORK/decoy/sleeper.c"
# the argument decoy: a sleeper that carries the CONFIGURED PET PATH as argv[1]
# and nothing else - the shape `tail -f <path>` has, and the one a matcher that
# accepts any cmdline token would mistake for the pet
printf '#include <unistd.h>\nint main(void){for(;;)pause();}\n' > "$WORK/decoy/argdecoy.c"
# the shaped decoy: a sleeper that presents a CHOSEN cmdline. The kernel fixes
# a process's cmdline at exec time, so a shape can only be presented by
# exec'ing with it - this program re-execs ITSELF with the tokens it was
# given, and the re-executed copy sleeps for ever. /proc/<pid>/cmdline then
# reads exactly those tokens: the shape the matcher has to judge, carried by a
# real process and not by a mock.
cat > "$WORK/decoy/shaped.c" <<'CSH'
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (!getenv("SHAPED_REEXEC")) {
        char *args[8];
        int n = 0;
        if (argc < 2) _exit(2);
        setenv("SHAPED_REEXEC", "1", 1);
        for (int i = 1; i < argc && n < 7; i++) args[n++] = argv[i];
        args[n] = 0;
        execv("/proc/self/exe", args);
        _exit(127);
    }
    for (;;) pause();
}
CSH
# the script the shaped decoys name as "the script the interpreter runs": it is
# never executed (the decoy only presents a cmdline), but the shape is only
# believable when the file it names exists
printf 'import time\nwhile True: time.sleep(0.2)\n' > "$WORK/decoy/other.py"
# a systemctl that always fails the way the real one does for a unit that is not
# installed: the state WORD "not-found" on stdout and exit 4, plus the failure
# text on stderr. The word is what the bar must read (the exit code cannot tell
# "disabled" from a failure), and it logs one line per call so the test can
# count reads. The stateful twin (systemctl-good) answers is-enabled from a state
# file, printing the same words the real tool prints, and records enable/disable;
# the checks swap them by renaming so the bar's own PATH resolves whichever mode
# a check needs.
cat > "$WORK/fakebin/systemctl" <<SH
#!/bin/sh
echo "call: \$*" >> "$CALLS"
echo "not-found"
echo "Failed to get unit file state for chocobar.service: No such file or directory" >&2
exit 4
SH
chmod +x "$WORK/fakebin/systemctl"
cat > "$WORK/fakebin/systemctl-good" <<SH
#!/bin/sh
echo "call: \$*" >> "$CALLS"
case "\$2" in
  is-enabled) if [ -f "$WORK/autostart.enabled" ]; then echo enabled; exit 0; else echo disabled; exit 1; fi ;;
  enable) : > "$WORK/autostart.enabled"; exit 0 ;;
  disable) rm -f "$WORK/autostart.enabled"; exit 0 ;;
  *) exit 4 ;;
esac
SH
chmod +x "$WORK/fakebin/systemctl-good"
# a systemctl that answers "masked" and exits 1 - exactly what a real masked
# unit does. The exit code alone cannot tell this from "disabled", so a bar
# that reads the code reports a masked unit as disabled and flips it blind.
cat > "$WORK/fakebin/systemctl-masked" <<SH
#!/bin/sh
echo "call: \$*" >> "$CALLS"
case "\$2" in
  is-enabled) echo masked; exit 1 ;;
  *) exit 4 ;;
esac
SH
chmod +x "$WORK/fakebin/systemctl-masked"
# an xdg-open that records what it was asked to open. The bar resolves the
# program to an absolute path BEFORE it forks (a child of a fork in this
# threaded bar may not allocate, and the PATH search allocates), so this fake
# in the bar's own PATH is the one that runs - and its record proves the whole
# path: resolve, fork, exec.
cat > "$WORK/fakebin/xdg-open" <<SH
#!/bin/sh
echo "\$*" >> "$WORK/xdg-open-calls"
SH
chmod +x "$WORK/fakebin/xdg-open"

gcc -O1 -o "$XCLICK" "$HERE/xclick.c" -lX11 -lXtst 2>"$WORK/cc.log" \
    || { echo "FAIL: cannot build the XTEST driver"; cat "$WORK/cc.log"; exit 1; }
gcc -O1 -o "$XMON" "$HERE/xmonitor.c" -lX11 2>>"$WORK/cc.log" \
    || { echo "FAIL: cannot build the ClientMessage monitor"; cat "$WORK/cc.log"; exit 1; }
gcc -O1 -o "$WORK/decoy/pet" "$WORK/decoy/sleeper.c" 2>>"$WORK/cc.log" \
    || { echo "FAIL: cannot build the decoy sleeper"; cat "$WORK/cc.log"; exit 1; }
gcc -O1 -o "$WORK/decoy/argdecoy" "$WORK/decoy/argdecoy.c" 2>>"$WORK/cc.log" \
    || { echo "FAIL: cannot build the argument decoy"; cat "$WORK/cc.log"; exit 1; }
gcc -O1 -o "$WORK/decoy/shaped" "$WORK/decoy/shaped.c" 2>>"$WORK/cc.log" \
    || { echo "FAIL: cannot build the shaped decoy"; cat "$WORK/cc.log"; exit 1; }
calls_n() { if [ -f "$CALLS" ]; then wc -l < "$CALLS"; else echo 0; fi; }
# how many times the bar opened something with a browser (the fake xdg-open
# records one line per call)
xdg_opens_n() { if [ -f "$WORK/xdg-open-calls" ]; then wc -l < "$WORK/xdg-open-calls"; else echo 0; fi; }

# ---- the private display and bus -----------------------------------------
DPYNUM=$(( 90 + RANDOM % 8 ))
while [ -e "/tmp/.X11-unix/X$DPYNUM" ]; do DPYNUM=$((DPYNUM+1)); done
# the bar scales by whatever THIS server reports, and the arguments are
# overridable so the gate can be run under another DPI configuration
# (CHOCOBAR_TEST_XVFB_ARGS='-screen 0 1600x400x24 -dpi 120') to prove the
# expectations follow the server instead of assuming one scale
Xvfb ":$DPYNUM" ${CHOCOBAR_TEST_XVFB_ARGS:--screen 0 1600x400x24} >"$WORK/xvfb.log" 2>&1 &
XPID=$!
export DISPLAY=":$DPYNUM"
for _ in $(seq 1 50); do xwininfo -root >/dev/null 2>&1 && break; sleep 0.2; done
xwininfo -root >/dev/null 2>&1 || { echo "FAIL: Xvfb did not come up"; exit 1; }

dbus-daemon --session --address="unix:path=$WORK/bus" --print-address --nofork \
    >"$WORK/dbus.log" 2>&1 &
DPID=$!
for _ in $(seq 1 50); do [ -S "$WORK/bus" ] && break; sleep 0.2; done
[ -S "$WORK/bus" ] || { echo "FAIL: the private bus did not come up"; exit 1; }

# the ClientMessage watcher: the all-desktops request is an X message to the
# window manager, so this is the only trace of it on a bare Xvfb
: > "$CLIENTMSG"
"$XMON" "$CLIENTMSG" &
MON_PID=$!
sleep 0.3
kill -0 "$MON_PID" 2>/dev/null || { echo "FAIL: the ClientMessage monitor did not start"; exit 1; }
# a real session carries an Xft.dpi resource and the bar lets it decide the
# scale before any physical size does, so the gate can set one - xprop writes
# the root property exactly as xrdb would - and run under a scale the
# RESOURCE chose rather than the screen size (CHOCOBAR_TEST_XFT_DPI=120).
# The monitor above must already be running: it is the client that keeps the
# server open, and a server left with no client at all RESETS itself -
# wiping the root property - so this is the earliest spot the value survives
# until the bar connects and reads it
if [ -n "${CHOCOBAR_TEST_XFT_DPI:-}" ]; then
    xprop -root -f RESOURCE_MANAGER 8s -set RESOURCE_MANAGER "Xft.dpi: ${CHOCOBAR_TEST_XFT_DPI}"
fi
# how many EWMH all-desktops requests did the bar send for that window? The
# window id alone is not enough: Xvfb recycles a client's id range, so a bar
# restarted in the same suite can be handed a previous bar's window id.
all_desktops_n() { # all_desktops_n <window id>
    awk -v w="$1" '$1 == w && $2 == "_NET_WM_DESKTOP" && $3 == "4294967295" { n++ } END { print n+0 }' "$CLIENTMSG"
}
want_all_desktops() { # want_all_desktops <window id>
    [ "$(all_desktops_n "$1")" -ge 1 ]
}

HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$WORK/bar.log" 2>&1 &
BAR_PID=$!
for _ in $(seq 1 60); do
    BARWIN=$(xdotool search --class chocobar 2>/dev/null | head -1)
    [ -n "${BARWIN:-}" ] && break
    sleep 0.2
done
[ -n "${BARWIN:-}" ] || { echo "FAIL: the bar never opened a window"; cat "$WORK/bar.log"; exit 1; }
sleep 0.5
# the bar's own window geometry
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
echo "--- bar window $BARWIN at ${X},${Y} ${WIDTH}x${HEIGHT}"
# the startup map is where the all-desktops request must have been made: the
# bar lives on every desktop, not just the one it happened to map on
want_all_desktops "$BARWIN"
check "the bar requests all desktops when it maps" $?
# the report and the config are in LOGICAL pixels and the bar scales them, so
# every expected pixel value below is derived, not assumed. The scale must be
# the bar's own, read off the same server the bar reads it (main.c:
# detectScale()): the root's Xft.dpi resource decides, and only when that
# leaves the scale at or below 1.01 does the screen's physical size - pixels
# over millimetres, via xdpyinfo - take over, clamped to [1, 4]. A scale
# derived from the bar's own window height instead only tells the scale to
# the half pixel that height rounds to: half a pixel in 30 is up to 15 px on a
# 900-logical-px width, more than the 8 px the bands below allow - on the
# ~1.04 scale a plain Xvfb reports (height 31: 31/30 against the real 1.0427)
# the width band's edge sat ON the real value and one pixel of drift turned
# the gate red. This derivation matches the bar's own to the four decimals it
# prints - under 0.05 px at 900 px - so the whole 8 px of every band below
# is margin.
bar_scale() {
    awk -v dpi_str="$(
        xprop -root RESOURCE_MANAGER 2>/dev/null | awk '
            { i = index($0, "Xft.dpi:")
              if (i) { rest = substr($0, i + 8)
                       sub(/^[^0-9.+-]*/, "", rest)   # xprop renders a tab as \t
                       print rest + 0; exit } }'
    )" -v dims="$(
        xdpyinfo | sed -n 's/^ *dimensions: *\([0-9]*\)x[0-9]* pixels (\([0-9]*\)x[0-9]* millimeters.*/\1 \2/p' | head -1
    )" 'BEGIN {
        s = 1.0
        if (dpi_str != "" && dpi_str > 40 && dpi_str < 1000) s = dpi_str / 96.0
        if (s <= 1.01) {
            split(dims, a, " ")
            if (a[2] > 0) {
                d = a[1] * 25.4 / a[2]
                if (d > 40 && d < 1000) s = d / 96.0
            }
        }
        if (s < 1.0) s = 1.0
        if (s > 4.0) s = 4.0
        printf "%.4f", s
    }'
}
SCALE=$(bar_scale)
# and the derivation is proved against the bar itself on every run: the bar's
# own height is 30 logical px times ITS scale, rounded to whole device
# pixels, so a measured height more than a pixel away means the derivation
# above and the bar's detectScale() have parted ways and every expectation
# below is scaled by the wrong number
H_EXPECT=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", 30*s}')
[ "$HEIGHT" -ge $((H_EXPECT-1)) ] && [ "$HEIGHT" -le $((H_EXPECT+1)) ]
check "the derived scale is the bar's own (h=$HEIGHT, 30*${SCALE}=$H_EXPECT)" $?

# how many mapped windows does the root have? the menu is an extra one. The
# baseline is MEASURED, never assumed: the bar itself is not one root child but
# several (its own window plus a 1x1 helper X creates), so a hard-coded count
# breaks the day that baseline changes or another client appears.
root_children() { xwininfo -root -tree | grep -c '^     0x'; }
BAR_BASE=$(root_children)
menu_open() { [ "$(root_children)" -gt "$BAR_BASE" ]; }
# the menu's window id and geometry: the mapped root child that is not the bar
# (the bar's extra 1x1 helper window is filtered by the w/h > 1 test)
menu_wh() {
    for w in $(xwininfo -root -children | grep -o '^     0x[0-9a-f]*'); do
        [ "$((w))" -eq "$BARWIN" ] && continue
        out=$(xwininfo -id "$w" | awk '/Width:/{W=$2} /Height:/{H=$2} END{if (W>1 && H>1) printf "%d %d", W, H}')
        if [ -n "$out" ]; then echo "$w $out"; return; fi
    done
}
# same count, minus the tooltip window: the tip is created once on the first
# hover and then mapped/unmapped, so it must not read as "a click opened a window"
root_children_excluding_tip() {
    for w in $(xwininfo -root -children | grep -o '^     0x[0-9a-f]*'); do
        case "$(xprop -id "$w" WM_CLASS 2>/dev/null)" in
            *ChocobarTip*) continue ;;
        esac
        echo "$w"
    done | wc -l
}
# the menu's row geometry, in logical px (linux/src/menu.c: MENU_PAD, MENU_ROW,
# MENU_SEP). The second row is "Subscription dashboard", whose choice is
# observable in the bar's own log without changing anything on disk; its centre
# is MENU_PAD + MENU_ROW + MENU_ROW/2 from the menu's top edge.
ROW2_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (4+21+10)*s}')
# the "Start on login" row centre, in logical px from the menu's top edge
# (menu.c: MENU_PAD, two rows, MENU_SEP, then half of this row)
ROW_AUTO_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (4+2*21+5+10)*s}')
# the "Check for updates" row centre: five rows and three separators down
ROW_UPDATES_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (4+5*21+3*5+10)*s}')

# the geometry report can go out two ways, and the difference decides WHEN it
# lands: gdbus introspects its destination before it calls, and the bar never
# answers Introspect (its bus loop replies to Report alone), so a gdbus call
# stalls for seconds and its report arrives long after the bar's 700 ms
# startup geometry wait. dbus-send delivers the method call directly, and the
# bar's 10 ms startup pump answers it inside that wait - so a check that needs
# the report to arrive BEFORE the first map, or DURING a window the bar's own
# tick must live through, has to use the fast one.
report() { gdbus call --timeout 5 --session --dest org.chocobar.Geom --object-path /ChocobarGeom \
              --method org.chocobar.Geom.Report "$@" >/dev/null 2>&1; }
report_fast() { dbus-send --session --dest=org.chocobar.Geom --type=method_call \
    --print-reply /ChocobarGeom org.chocobar.Geom.Report \
    "string:$1" "int32:$2" "int32:$3" "int32:$4" "int32:$5" >/dev/null 2>&1; }

# 1. the bar must not ask for a compositor blur
xprop -id "$BARWIN" _KDE_NET_WM_BLUR_BEHIND_REGION >"$WORK/prop.txt" 2>&1
! grep -qE '[0-9]+, *[0-9]+, *[0-9]+, *[0-9]+' "$WORK/prop.txt"
check "the bar asks the compositor for NO blur" $?

# 2. a press alone opens nothing (Windows opens on WM_RBUTTONUP)
"$XCLICK" $((X+40)) $((Y+10)) 3 press
sleep 0.6
! menu_open
check "a right-PRESS opens nothing" $?

# 2b. with the menu never opened and the bar ticking on its 1 s loop, the
#     autostart mark is not read at all: the fake systemctl logs one line per
#     call and its log must be empty
sleep 2
[ "$(calls_n)" -eq 0 ]
check "with the menu closed the autostart mark is not re-read per tick" $?

# 3. the release opens the menu
"$XCLICK" $((X+40)) $((Y+10)) 3 release
sleep 0.6
menu_open
check "the right-RELEASE opens the menu" $?

# 3b. the menu the user sees is the Windows item table rendered: 8 rows and 3
#     separators (p_ui.c kMenuItems) inside MENU_PAD, i.e. 2*4 + 8*21 + 3*5 =
#     191 logical px tall (menu.c: MENU_PAD 4, MENU_ROW 21, MENU_SEP 5 - the
#     Windows WM_MEASUREITEM values). The row count is what this proves: a
#     missing or extra item changes the height. ALL conditions must land in ONE
#     status, or a trailing bare test overwrites $? and the check silently passes
#     on a window it never measured.
read -r MENU_WIN MENU_W_DEV MENU_H_DEV <<EOF
$(menu_wh)
EOF
MENU_WIN=${MENU_WIN:-0}; MENU_W_DEV=${MENU_W_DEV:-0}; MENU_H_DEV=${MENU_H_DEV:-0}
WANT_MENU_H=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (2*4+8*21+3*5)*s}')
rc=0
[ "$MENU_W_DEV" -gt 0 ] || rc=1
[ "$MENU_H_DEV" -ge $((WANT_MENU_H-2)) ] && [ "$MENU_H_DEV" -le $((WANT_MENU_H+2)) ] || rc=1
check "the menu renders the Windows item set (${MENU_W_DEV}x${MENU_H_DEV}, want h~$WANT_MENU_H)" "$rc"

# 3c. opening the menu read the autostart mark exactly ONCE (one line in the
#     fake systemctl's own call log), and the menu may then repaint - pointer
#     moves across its rows repaint the menu on every hover change (menu.c) -
#     without reading it again: the once-per-open, not-per-paint invariant
[ "$(calls_n)" -eq 1 ]
check "opening the menu read the autostart mark exactly once" $?
if [ "$MENU_WIN" != "0" ]; then
    MGEO=$(xdotool getwindowgeometry --shell "$MENU_WIN")
    MENU_X=$(echo "$MGEO" | awk -F= '/^X=/{print $2}')
    MENU_Y=$(echo "$MGEO" | awk -F= '/^Y=/{print $2}')
    for i in 1 3 5 7 9 11; do
        xdotool mousemove $((MENU_X + MENU_W_DEV / 2)) $((MENU_Y + i * MENU_H_DEV / 12))
        sleep 0.1
    done
fi
sleep 0.3
[ "$(calls_n)" -eq 1 ]
check "menu repaints do not re-read the autostart mark" $?

# 4. right-clicking again dismisses it, and it STAYS dismissed
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
! menu_open
check "the dismissing right-click closes the menu and it stays closed" $?

# 5. and the next right-click opens it again (the swallow is one-shot)
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
menu_open
check "the next right-click opens the menu again" $?

# 5b. close the menu again: the bar sits in the menu's own loop while it is
#     open, so a test that leaves it open cannot talk to the bar at all.
"$XCLICK" 100 300 1 full
sleep 0.8
! menu_open
check "the menu closes again when the bar is clicked outside it" $?

# 5c. a right-press whose release lands OFF the bar opens nothing. The X
#     implicit grab delivers the off-bar release to the BAR at out-of-bounds
#     coordinates (the press window gets every pointer event until the last
#     release), so the bar's bounds check is what enforces the rule, exactly
#     like Windows sending WM_RBUTTONUP only over the window. This check IS
#     the deletion detector for that bounds check: without it the release far
#     below the bar would open the menu.
"$XCLICK" $((X+40)) $((Y+10)) 3 drag $((X+40)) $((Y+HEIGHT+60))
sleep 0.8
! menu_open
check "a right-press released OFF the bar opens nothing" $?

# 5d. a click OUTSIDE the menu picks nothing, even at a row's own height, and
#     the bar survives it. The pointer grab delivers the click to the menu with
#     coordinates outside the window; the row pick used the y coordinate alone,
#     so this click could choose whatever row that height happened to hit.
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.6
"$XCLICK" $((X-100)) $((Y+10+ROW2_Y)) 1 full
sleep 0.8
! menu_open && ! xwininfo -root -children 2>/dev/null | grep -q "Subscriptions" &&
    xwininfo -id "$BARWIN" | grep -q "Map State: IsViewable"
check "a click OUTSIDE the menu picks no row and leaves the bar running" $?

# 5e. a click INSIDE a row picks that row: the Subscription row opens the
#     real board window now (named "Chocobar - Subscriptions"), replacing the
#     old "not ported" log line. menu_open() cannot be reused here: it counts
#     root children, and the board IS one.
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.6
"$XCLICK" $((X+40+40)) $((Y+10+ROW2_Y)) 1 full
sleep 1.2
SUBSWIN=$(xwininfo -root -children 2>/dev/null | awk '/Subscriptions/ { print $1; exit }')
[ -n "${SUBSWIN:-}" ]
check "a click INSIDE the Subscription row opens the board" $?

# 5e-bis. the board the user sees is a POPUP, not a desktop window: xwininfo on
#     a real mapped override-redirect window prints 'Override Redirect State:
#     yes'. This is the observable property (p_ui.c's owned WS_POPUP parity),
#     not a source-text match - a decorated board or a dock entry fails here.
if [ -n "${SUBSWIN:-}" ]; then
    xwininfo -id "$SUBSWIN" | grep -q "Override Redirect State: yes"
    check "the subscription board is an override-redirect popup (no titlebar, no dock entry)" $?
else
    check "the subscription board is an override-redirect popup (no titlebar, no dock entry)" 1
fi

# 5f. the close button ends the board: the titlebar's ✕ (subs.c btnClose, the
#     same frame geometry dash.c uses: right edge - padL, 20*S tall from ty-3*S).
#     The board's geometry is read into its OWN variables: X/Y stay the BAR's,
#     which 5g still needs.
if [ -n "${SUBSWIN:-}" ]; then
    read -r BX BY BW BH <<EOF
$(xwininfo -id "$SUBSWIN" | awk '/Absolute upper-left X:/{x=$4} /Absolute upper-left Y:/{y=$4} /Width:/{w=$2} /Height:/{h=$2} END{print x, y, w, h}')
EOF
    CLOSE_X=$(awk -v s="$SCALE" -v bw="$BW" 'BEGIN{printf "%.0f", bw - 27*s}')
    CLOSE_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", 21*s}')
    "$XCLICK" $((BX + CLOSE_X)) $((BY + CLOSE_Y)) 1 full
    sleep 1.0
    ! xwininfo -root -children 2>/dev/null | grep -q "Subscriptions"
    check "the board's close button closes it" $?
else
    check "the board's close button closes it" 1
fi

# 5g. activating the row again while the board is open closes it (a toggle,
#     never a leak): open once more, then pick the same row again
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.6
"$XCLICK" $((X+40+40)) $((Y+10+ROW2_Y)) 1 full
sleep 1.2
SUBSWIN=$(xwininfo -root -children 2>/dev/null | awk '/Subscriptions/ { print $1; exit }')
if [ -n "${SUBSWIN:-}" ]; then
    "$XCLICK" $((X+40)) $((Y+10)) 3 full
    sleep 0.6
    "$XCLICK" $((X+40+40)) $((Y+10+ROW2_Y)) 1 full
    sleep 1.2
    N=$(xwininfo -root -children 2>/dev/null | grep -c "Subscriptions")
    [ "$N" -eq 0 ]
    check "a second activation closes the board, not a second window" $?
else
    check "a second activation closes the board, not a second window" 1
fi

# 6. the autostart state is honest: the failure text reaches the log
grep -q "autostart state unknown" "$WORK/bar.log"
check "an unreadable autostart state says so, with the real reason" $?

# 6b. with bar.align 1 but no geometry report yet, the bar sits at the
#     screen-top fallback (bar.gap below the top edge) instead of pinning itself
#     to a terminal it cannot see; x and the width stay content-sized.
WANT_TOP=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", 12*s}')
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
[ "$Y" -ge $((WANT_TOP-4)) ] && [ "$Y" -le $((WANT_TOP+4)) ]
check "with no geometry report the bar falls back to the screen top (y=$Y want~$WANT_TOP)" $?

# 6c. THE PET CHIP. The config wires the fake pet as the FIRST (and only
#     left-group) chip; the other clickable chip (tokens, which opens the
#     dashboard) is disabled, so the pet chip can be found safely by clicking
#     x offsets and watching for the bar's own "pet: starting" log line.
#     The DECOY runs first: its executable is named like the pet's basename,
#     so its comm IS the pet's basename while its cmdline carries a different
#     path - the process a name-based match would stop.
"$WORK/decoy/pet" >/dev/null 2>&1 &
DECOY_PID=$!
sleep 0.3
kill -0 "$DECOY_PID" 2>/dev/null && [ "$(cat "/proc/$DECOY_PID/comm" 2>/dev/null)" = "pet" ]
check "the decoy is a name twin of the pet (comm equals the pet's basename)" $?
PET_X=0
for xoff in 20 30 40 50 60 70 80; do
    "$XCLICK" $((X+xoff)) $((Y+10)) 1 full
    sleep 0.4
    if grep -q "pet: starting" "$WORK/bar.log"; then PET_X=$xoff; break; fi
done
[ "$PET_X" -gt 0 ]
check "a click on the pet chip starts the pet (chip found at x offset $PET_X)" $?
# one start, one line, and it is the truth: the exec's outcome comes back over
# the CLOEXEC pipe, so a successful start has "starting" and no "cannot start"
# - the bar never claims a start the kernel refused
grep -q "pet: starting: $WORK/home/pet" "$WORK/bar.log" &&
    ! grep -q "pet: cannot start $WORK/home/pet" "$WORK/bar.log"
check "a successful start logs 'starting' and no 'cannot start'" $?
# the started pet must be the configured script and be alive: it recorded its
# pid (the file exists) and its state is "running"
[ -s "$WORK/pet.pid" ] && kill -0 "$(cat "$WORK/pet.pid")" 2>/dev/null &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "running" ]
check "the started pet is alive and recorded itself" $?

# 6d. click the pet chip again: the chip's state source (the matcher over
#     /proc/<pid>/cmdline) saw the started pet, so the toggle must STOP it -
#     one start, one stop, and the pet's own record shows a SIGTERM (a SIGKILL
#     would leave the pid file in place with the state "running"; a blind
#     duplicate start would log a second "pet: starting" line)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
grep -q "pet: stopped" "$WORK/bar.log" && [ "$(grep -c "pet: starting" "$WORK/bar.log")" -eq 1 ] &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
check "the second click stops the pet with a SIGTERM, not a duplicate, not a SIGKILL" $?

# 6d-2. THE RIGHT GROUP IS NOT A BUTTON, proven at runtime. A metric chip is a
#       label: clicking one must do NOTHING - no pet start or stop, no menu, no
#       dashboard - and the bar must still answer the next click. Every offset
#       here is inside the right group (the right group is laid out from the
#       right edge leftward, and the clock/battery/volume/ram/temp/cpu chips
#       fill it), and strictly right of the pet chip, which is the ONLY
#       clickable chip in this config (tokens are disabled). The counts, not a
#       grep, are the oracle: the bar's log already carries pet lines from the
#       checks above.
PET_STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
PET_STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
# the chip's state comes from the bar's 1 Hz tick and 6d just stopped the pet:
# let a tick pass so the chip reads "off" before anything is clicked
sleep 2
# the chip tooltip is a hover effect, not a click action: the sweep passes over
# every chip, so the tip window legitimately appears and the count must ignore it
ROOT_BEFORE=$(root_children_excluding_tip)
RIGHT_HITS=0
# a dense sweep of the right group's own extent: the right group is laid out
# from the right edge leftward, so every offset between the right padding and
# WIDTH-200 is a real metric chip (clock, battery, volume, ram, temp, cpu) or
# the padding beside it - and never the pet chip, which is the only clickable
# chip in this config (tokens are disabled) and sits at the far left.
off=$((WIDTH-15))
while [ "$off" -gt 90 ] && [ "$off" -lt "$WIDTH" ]; do
    "$XCLICK" $((X+off)) $((Y+10)) 1 full
    RIGHT_HITS=$((RIGHT_HITS+1))
    off=$((off-8))
    sleep 0.15
done
sleep 0.6
[ "$RIGHT_HITS" -ge 8 ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq "$PET_STARTED_BEFORE" ] &&
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq "$PET_STOPPED_BEFORE" ] &&
    [ "$(root_children_excluding_tip)" -eq "$ROOT_BEFORE" ]
check "a left click on a right-group metric chip does nothing ($RIGHT_HITS clicks, no action)" $?
# and the bar still answers the next click, so the inert chip did not wedge it
sleep 2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 2
[ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq $((PET_STARTED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq "$PET_STOPPED_BEFORE" ]
check "the bar still answers a click after the right-group clicks" $?
# stop it again, so the checks below start from a stopped pet
sleep 2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 2
[ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((PET_STOPPED_BEFORE+1)) ]
check "the pet chip still stops the pet after the right-group clicks" $?

# 6e. the decoy must survive the pet's stop: the match is the executed file,
#     never a process name, and the decoy's comm IS the pet's basename
kill -0 "$DECOY_PID" 2>/dev/null
check "the name-twin decoy survives the pet's stop (never a process name)" $?

# 6f. a process that merely CARRIES the configured path as an argument is not
#     the pet: with the pet stopped and an argdecoy whose argv[1] is exactly
#     the configured path alive (the shape `tail -f <path>` has), a click must
#     START the pet - not stop the decoy - and the decoy must survive. A
#     matcher that accepts any cmdline token fails all three: it reads the
#     chip as "on", takes the stop path, SIGTERMs the decoy and logs a stop.
"$WORK/decoy/argdecoy" "$WORK/home/pet" >/dev/null 2>&1 &
ARGDECOY_PID=$!
sleep 0.3
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
[ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq "$STOPPED_BEFORE" ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq $((STARTED_BEFORE+1)) ] &&
    kill -0 "$ARGDECOY_PID" 2>/dev/null &&
    [ -s "$WORK/pet.pid" ] && kill -0 "$(cat "$WORK/pet.pid")" 2>/dev/null
check "an argument decoy is not the pet: the click starts the pet and spares the decoy" $?
# stop the real pet again so the checks below start from a stopped one
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
kill "$ARGDECOY_PID" 2>/dev/null
ARGDECOY_PID=""

# 6f-2. a script the USER starts the way a user starts one - from its own
#      directory with a RELATIVE path (./pet) - is still the pet. Its cmdline
#      is `<interpreter>|./pet`, so the matcher must resolve argv[1] against
#      that process's own cwd to see it is the configured file. A matcher that
#      only compares strings reads the chip "off", so the click STARTS a second
#      copy instead of stopping the one that is already running - and the stop
#      action can then never stop what it started. The argdecoy is gone by
#      here, so this click must take the STOP path against exactly this pid.
( cd "$WORK/home" && ./pet >/dev/null 2>&1 & )
USER_PID=""
for _ in $(seq 1 20); do
    [ -s "$WORK/pet.pid" ] && USER_PID=$(cat "$WORK/pet.pid" 2>/dev/null) && break
    sleep 0.1
done
STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
[ -n "$USER_PID" ] && kill -0 "$USER_PID" 2>/dev/null &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "running" ]
check "a user-started env-shebang pet (relative path) is alive and recording" $?
# the chip's state source is the 1 Hz tick, so let one poll see the pet before
# clicking: the click reads g_m.petRunning, not /proc
sleep 2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
[ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq "$STARTED_BEFORE" ] &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
check "the chip sees a relative-path pet: the click stops it, no duplicate start" $?
[ -n "$USER_PID" ] && kill -0 "$USER_PID" 2>/dev/null && kill "$USER_PID" 2>/dev/null
# the click above already stopped it, so the zombie check below starts stopped

# 6f-2b. the same file under another ABSOLUTE spelling: a SYMLINK to the
#       configured file. The kernel hands the interpreter the script path
#       exactly as the user typed it, so starting the pet through a symlink
#       runs it as `<interpreter>|<symlink path>` - a token that is neither
#       the configured path nor its realpath, though the file is the same one.
#       A matcher that decides an absolute token by string comparison alone
#       reads the chip "off" while the pet runs, so the click starts a
#       duplicate it can then never stop.
ln -s "$WORK/home/pet" "$WORK/home/petlink"
"$WORK/home/petlink" >/dev/null 2>&1 &
LINK_PID=""
for _ in $(seq 1 20); do
    [ -s "$WORK/pet.pid" ] && LINK_PID=$(cat "$WORK/pet.pid" 2>/dev/null) && break
    sleep 0.1
done
STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
[ -n "$LINK_PID" ] && kill -0 "$LINK_PID" 2>/dev/null &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "running" ]
check "a pet started through a symlink to it is alive and recording" $?
# the chip's state source is the 1 Hz tick, so let one poll see the pet
# before clicking: the click reads g_m.petRunning, not /proc
sleep 2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
[ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq "$STARTED_BEFORE" ] &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
check "the chip sees a symlink-started pet: the click stops it, no duplicate start" $?
[ -n "$LINK_PID" ] && kill -0 "$LINK_PID" 2>/dev/null && kill "$LINK_PID" 2>/dev/null
rm -f "$WORK/home/petlink"

# 6f-2c. the same file under a REDUNDANT absolute spelling: `$WORK/home/./pet`
#       runs as `<interpreter>|$WORK/home/./pet`, which is not the configured
#       path by bytes either - the same trap as the symlink above, spelled
#       without any symlink
"$WORK/home/./pet" >/dev/null 2>&1 &
DOT_PID=""
for _ in $(seq 1 20); do
    [ -s "$WORK/pet.pid" ] && DOT_PID=$(cat "$WORK/pet.pid" 2>/dev/null) && break
    sleep 0.1
done
STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
[ -n "$DOT_PID" ] && kill -0 "$DOT_PID" 2>/dev/null &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "running" ]
check "a pet started through a redundant path spelling is alive and recording" $?
# the chip's state source is the 1 Hz tick, so let one poll see the pet
# before clicking: the click reads g_m.petRunning, not /proc
sleep 2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
[ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq "$STARTED_BEFORE" ] &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
check "the chip sees a redundantly-spelled pet: the click stops it, no duplicate start" $?
[ -n "$DOT_PID" ] && kill -0 "$DOT_PID" 2>/dev/null && kill "$DOT_PID" 2>/dev/null

# 6f-3. a VERSION-PINNED interpreter. The kernel names the live process after the
#      interpreter the shebang finally selected, version included: an
#      `#!/usr/bin/env python3.14` script runs with argv[0] = "python3.14", so a
#      matcher with an exact-name allowlist ("python", "python3") never examines
#      argv[1] and reads the chip "off" while the pet is running - a click then
#      starts a SECOND copy and the stop action can never match it. The
#      interpreter is whatever THIS runner has (never hardcoded), and the case
#      is skipped when the runner has no versioned interpreter at all.
VINTERP=""
for c in /usr/bin/python3.* /usr/local/bin/python3.*; do
    [ -x "$c" ] || continue
    b=${c##*/}
    # the interpreter itself, not a sibling like python3.14-config
    case "$b" in *-*) continue ;; esac
    case "$b" in python3.[0-9]*) VINTERP="$c"; break ;; esac
done
if [ -n "$VINTERP" ]; then
    VBASE=${VINTERP##*/}
    cat > "$WORK/home/pet" <<PET
#!$VINTERP
import os, signal, sys, time
with open("$WORK/pet.pid", "w") as f: f.write(str(os.getpid()))
with open("$WORK/pet.state", "w") as f: f.write("running")
def bye(*a):
    with open("$WORK/pet.state", "w") as f: f.write("SIGTERM")
    try: os.unlink("$WORK/pet.pid")
    except OSError: pass
    sys.exit(0)
signal.signal(signal.SIGTERM, bye)
while True: time.sleep(0.2)
PET
    chmod +x "$WORK/home/pet"
    rm -f "$WORK/pet.pid" "$WORK/pet.state"
    # the chip's state is the bar's 1 Hz tick, and the check above left the pet
    # stopped: let a tick pass so the click below takes the START path
    sleep 2
    STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
    "$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
    sleep 1.5
    VPID=$(cat "$WORK/pet.pid" 2>/dev/null)
    # the matcher reads /proc/<pid>/cmdline, and for a shebang script the kernel
    # replaces argv[0] with the INTERPRETER (argv[1] is the script), so the
    # first token's basename is the versioned interpreter - that is the token
    # the allowlist has to accept. (comm is the SCRIPT's basename here, not
    # the interpreter's, so it is not the oracle.)
    VPID_ARGV0=$(tr '\0' '\n' < "/proc/$VPID/cmdline" 2>/dev/null | head -1)
    [ -n "$VPID" ] && kill -0 "$VPID" 2>/dev/null &&
        [ "${VPID_ARGV0##*/}" = "$VBASE" ]
    check "the pet runs under the versioned interpreter $VBASE (its argv[0])" $?
    # the chip's state source is the 1 Hz tick, so let one poll see it
    sleep 2
    STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
    "$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
    sleep 1.5
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
        [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq $((STARTED_BEFORE+1)) ] &&
        [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
    check "a $VBASE-shebang pet is seen: the click stops it, no duplicate start" $?
    [ -n "$VPID" ] && kill -0 "$VPID" 2>/dev/null && kill "$VPID" 2>/dev/null
    # restore the shell-shebang pet the later checks were written against
    cat > "$WORK/home/pet" <<PET
#!/usr/bin/env bash
echo \$\$ > "$WORK/pet.pid"
echo running > "$WORK/pet.state"
trap 'echo SIGTERM > "$WORK/pet.state"; rm -f "$WORK/pet.pid"; exit 0' TERM
while :; do sleep 0.2; done
PET
    chmod +x "$WORK/home/pet"
    rm -f "$WORK/pet.pid" "$WORK/pet.state"
fi

# 6f-4. a SHEBANG THAT CARRIES AN ARGUMENT. The kernel shebang rule is
#      `<interpreter> [optional-arg] <script>`, so `#!/bin/bash -e` runs as
#      `/bin/bash|-e|./pet` - the script is at argv[2], not argv[1], and a
#      matcher that reads only argv[1] sees the interpreter's own option there
#      and reads the chip "off" while the pet runs. A click then STARTS a second
#      copy and the stop action can never match either. The pet is started by
#      the USER the way a user starts one, from its own directory, so the token
#      is the relative `./pet` the matcher must resolve against that process's
#      own cwd. /proc/<pid>/cmdline is the oracle, not an assumption: the check
#      below proves the shape before asserting the chip saw it.
cat > "$WORK/home/pet" <<PET
#!/bin/bash -e
echo \$\$ > "$WORK/pet.pid"
echo running > "$WORK/pet.state"
trap 'echo SIGTERM > "$WORK/pet.state"; rm -f "$WORK/pet.pid"; exit 0' TERM
while :; do sleep 0.2; done
PET
chmod +x "$WORK/home/pet"
rm -f "$WORK/pet.pid" "$WORK/pet.state"
( cd "$WORK/home" && ./pet >/dev/null 2>&1 & )
ARG_PID=""
for _ in $(seq 1 20); do
    [ -s "$WORK/pet.pid" ] && ARG_PID=$(cat "$WORK/pet.pid" 2>/dev/null) && break
    sleep 0.1
done
# the shape this check exists for: the interpreter, then its option, then the
# script - three tokens, so argv[1] is an option and only a full scan finds the
# file
ARG_ARGV=$(tr '\0' '|' < "/proc/$ARG_PID/cmdline" 2>/dev/null)
[ -n "$ARG_PID" ] && kill -0 "$ARG_PID" 2>/dev/null &&
    [ "$(echo "$ARG_ARGV" | cut -d'|' -f1)" = "/bin/bash" ] &&
    [ "$(echo "$ARG_ARGV" | cut -d'|' -f2)" = "-e" ] &&
    [ "$(echo "$ARG_ARGV" | cut -d'|' -f3)" = "./pet" ] &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "running" ]
check "the pet runs as /bin/bash|-e|./pet (the shebang argument is argv[1], the script argv[2])" $?
# the chip's state source is the bar's 1 Hz tick, so let one poll see the pet
# before clicking: the click reads g_m.petRunning, not /proc
sleep 2
STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
[ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq "$STARTED_BEFORE" ] &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ] &&
    ! kill -0 "$ARG_PID" 2>/dev/null
check "a shebang-with-argument pet is seen: the click stops it, no duplicate start" $?
[ -n "$ARG_PID" ] && kill -0 "$ARG_PID" 2>/dev/null && kill "$ARG_PID" 2>/dev/null
# restore the shell-shebang pet the later checks were written against
cat > "$WORK/home/pet" <<PET
#!/usr/bin/env bash
echo \$\$ > "$WORK/pet.pid"
echo running > "$WORK/pet.state"
trap 'echo SIGTERM > "$WORK/pet.state"; rm -f "$WORK/pet.pid"; exit 0' TERM
while :; do sleep 0.2; done
PET
chmod +x "$WORK/home/pet"
rm -f "$WORK/pet.pid" "$WORK/pet.state"

# 6f-5. THE PET PATH AS AN ARGUMENT IS NOT THE PET, in every shape an
#       interpreter can carry it in. The kernel executes the FIRST script an
#       interpreter is given; a token after that one is the script's argument,
#       so `python3 other.py <petpath>` runs other.py, and `sh -c <petpath>`
#       runs a command rather than a script. All of them carry the configured
#       path in their cmdline, so a matcher that scans every token after an
#       interpreter reads the chip "on" while the pet is stopped: the click
#       then takes the STOP path and SIGTERMs an unrelated process instead of
#       starting the pet. The decoys below present those shapes as real
#       processes and must all survive - the click has to START the pet, and
#       the stop that follows has to spare them.
#
#       The command forms are spelled every way getopt allows, because a guard
#       that recognises only the literal `-c` misses the BUNDLED `-ec` (there
#       the command flag is the second letter) and the `node -e` / `perl -e`
#       spellings entirely; `sh -n` is the no-execute twin - it syntax-checks
#       the file rather than running it, so the process that carries it is
#       never the pet.
SHAPED_PIDS=""
"$WORK/decoy/shaped" python3 "$WORK/decoy/other.py" "$WORK/home/pet" >/dev/null 2>&1 &
SHAPED1_PID=$!
"$WORK/decoy/shaped" sh -c "$WORK/home/pet" >/dev/null 2>&1 &
SHAPED2_PID=$!
# the third decoy carries the path RELATIVE, and runs in the pet's own
# directory, so the name resolves to the configured file there
( cd "$WORK/home" && exec "$WORK/decoy/shaped" python3 "$WORK/decoy/other.py" pet ) >/dev/null 2>&1 &
SHAPED3_PID=$!
# the bundled command flag: `-ec` is -e and -c, so the next token is a command
"$WORK/decoy/shaped" sh -ec "$WORK/home/pet" >/dev/null 2>&1 &
SHAPED4_PID=$!
# the same flag split across two option tokens
"$WORK/decoy/shaped" sh -e -c "$WORK/home/pet" >/dev/null 2>&1 &
SHAPED5_PID=$!
# the interpreters whose command flag is not `c` at all
"$WORK/decoy/shaped" node -e "require('$WORK/home/pet')" >/dev/null 2>&1 &
SHAPED6_PID=$!
"$WORK/decoy/shaped" perl -e "1 while 1" "$WORK/home/pet" >/dev/null 2>&1 &
SHAPED7_PID=$!
# the no-execute switch: `sh -n <petpath>` syntax-checks the file, so the
# process that carries it never runs the pet
"$WORK/decoy/shaped" sh -n "$WORK/home/pet" >/dev/null 2>&1 &
SHAPED8_PID=$!
SHAPED_PIDS="$SHAPED1_PID $SHAPED2_PID $SHAPED3_PID $SHAPED4_PID $SHAPED5_PID $SHAPED6_PID $SHAPED7_PID $SHAPED8_PID"
sleep 0.3
# the shape is what the matcher has to judge, so it is asserted, not assumed:
# /proc/<pid>/cmdline is the oracle
shaped_cmdline() { tr '\0' '|' < "/proc/$1/cmdline" 2>/dev/null; }
SHAPED_OK=1
for p in $SHAPED_PIDS; do
    kill -0 "$p" 2>/dev/null || SHAPED_OK=0
done
[ "$SHAPED_OK" -eq 1 ] &&
    [ "$(shaped_cmdline "$SHAPED1_PID")" = "python3|$WORK/decoy/other.py|$WORK/home/pet|" ] &&
    [ "$(shaped_cmdline "$SHAPED2_PID")" = "sh|-c|$WORK/home/pet|" ] &&
    [ "$(shaped_cmdline "$SHAPED3_PID")" = "python3|$WORK/decoy/other.py|pet|" ] &&
    [ "$(shaped_cmdline "$SHAPED4_PID")" = "sh|-ec|$WORK/home/pet|" ] &&
    [ "$(shaped_cmdline "$SHAPED5_PID")" = "sh|-e|-c|$WORK/home/pet|" ] &&
    [ "$(shaped_cmdline "$SHAPED6_PID")" = "node|-e|require('$WORK/home/pet')|" ] &&
    [ "$(shaped_cmdline "$SHAPED7_PID")" = "perl|-e|1 while 1|$WORK/home/pet|" ] &&
    [ "$(shaped_cmdline "$SHAPED8_PID")" = "sh|-n|$WORK/home/pet|" ]
check "the argument-shaped decoys are alive and carry the shapes" $?
# the chip's state comes from the bar's 1 Hz tick, so let one poll see the
# decoys: the click reads g_m.petRunning, not /proc
sleep 2
STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.5
[ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq $((STARTED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq "$STOPPED_BEFORE" ] &&
    [ -s "$WORK/pet.pid" ] && kill -0 "$(cat "$WORK/pet.pid")" 2>/dev/null
check "a pet path carried as an argument is not the pet: the click starts the pet" $?
# and the stop that follows must spare every decoy
sleep 2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.5
SHAPED_OK=1
for p in $SHAPED_PIDS; do
    kill -0 "$p" 2>/dev/null || SHAPED_OK=0
done
[ "$SHAPED_OK" -eq 1 ] &&
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq $((STARTED_BEFORE+1)) ]
check "the argument-shaped decoys survive the pet's stop" $?
for p in $SHAPED_PIDS; do kill "$p" 2>/dev/null; done
SHAPED_PIDS=""

# 6f-6. A VALUE-TAKING OPTION BEFORE THE SCRIPT. `#!/usr/bin/env -S python3 -X
#       utf8` is a real shebang shape, and the kernel runs it as
#       `python3|-X|utf8|<script>` - a matcher that reads the first non-option
#       token as the script reads `utf8` instead and never reaches the file, so
#       the chip shows "off" for a pet the bar itself just started: the next
#       click forks a SECOND copy and the stop action can never match either.
#       The pet is started through the bar's own start path (the click), and
#       /proc is the oracle for the shape before the chip's answer is asserted.
#       A runner without python3 or without an `env` that understands `-S`
#       cannot present the shape; that is a skip on a developer machine and a
#       FAIL in CI, where the gate must not become a silent no-op.
if ! command -v python3 >/dev/null 2>&1 || ! env -S /bin/true >/dev/null 2>&1; then
    if [ "${CI:-}" = "true" ]; then
        echo "FAIL: python3 and 'env -S' are needed to present a value-taking-option shebang"
        FAIL=$((FAIL+1))
    else
        echo "SKIP: no python3 or no 'env -S', so the value-taking-option shebang is not presented"
    fi
else
cat > "$WORK/home/pet" <<PET
#!/usr/bin/env -S python3 -X utf8
import os, signal, sys, time
with open("$WORK/pet.pid", "w") as f: f.write(str(os.getpid()))
with open("$WORK/pet.state", "w") as f: f.write("running")
def bye(*a):
    with open("$WORK/pet.state", "w") as f: f.write("SIGTERM")
    try: os.unlink("$WORK/pet.pid")
    except OSError: pass
    sys.exit(0)
signal.signal(signal.SIGTERM, bye)
while True: time.sleep(0.2)
PET
chmod +x "$WORK/home/pet"
rm -f "$WORK/pet.pid" "$WORK/pet.state"
# the check above left the pet stopped, and the chip's state comes from the 1 Hz
# tick, so let a tick pass before the click that has to take the START path
sleep 2
STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.5
VALUE_PID=$(cat "$WORK/pet.pid" 2>/dev/null)
VALUE_ARGV=$(tr '\0' '|' < "/proc/$VALUE_PID/cmdline" 2>/dev/null)
[ -n "$VALUE_PID" ] && kill -0 "$VALUE_PID" 2>/dev/null &&
    [ "$(echo "$VALUE_ARGV" | cut -d'|' -f1)" = "python3" ] &&
    [ "$(echo "$VALUE_ARGV" | cut -d'|' -f2)" = "-X" ] &&
    [ "$(echo "$VALUE_ARGV" | cut -d'|' -f3)" = "utf8" ] &&
    [ "$(echo "$VALUE_ARGV" | cut -d'|' -f4)" = "$WORK/home/pet" ]
check "the pet runs as python3|-X|utf8|<script> (a value-taking option before the script)" $?
# the chip's state source is the 1 Hz tick, so let one poll see it
sleep 2
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.5
[ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq $((STARTED_BEFORE+1)) ] &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
check "a value-taking-option pet is seen: the click stops it, no duplicate start" $?
[ -n "$VALUE_PID" ] && kill -0 "$VALUE_PID" 2>/dev/null && kill "$VALUE_PID" 2>/dev/null
# restore the shell-shebang pet the later checks were written against
cat > "$WORK/home/pet" <<PET
#!/usr/bin/env bash
echo \$\$ > "$WORK/pet.pid"
echo running > "$WORK/pet.state"
trap 'echo SIGTERM > "$WORK/pet.state"; rm -f "$WORK/pet.pid"; exit 0' TERM
while :; do sleep 0.2; done
PET
chmod +x "$WORK/home/pet"
rm -f "$WORK/pet.pid" "$WORK/pet.state"
fi

# 6g. no zombie children. The pet and the detached spawns are fire-and-forget,
#     so a bar that never waits keeps one defunct entry per invocation for as
#     long as the service runs. One start/stop cycle above and a second one here
#     must leave the bar's child table with no Z state at all.
zombies() {
    local n=0 pid st
    for pid in $(cat "/proc/$BAR_PID/task/$BAR_PID/children" 2>/dev/null); do
        st=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)
        [ "$st" = "Z" ] && n=$((n+1))
    done
    echo "$n"
}
STARTED_BEFORE=$(grep -c "pet: starting" "$WORK/bar.log" || true)
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 2
[ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq $((STARTED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
    [ "$(zombies)" -eq 0 ]
check "a pet start/stop cycle leaves no zombie child of the bar" $?

# 6h. a DIRECTORY as the pet path is refused: access(X_OK) on a directory
#     tests search permission, so a directory used to pass as "the pet" and
#     every process carrying it as an argument became stopable. The bar
#     hot-reloads its config (the 2 s config check), so swap in the throwaway
#     HOME (a 0755 directory) and click the chip: nothing may start, nothing
#     may stop, and the decoy must survive. The refusal is logged once the bar
#     has re-read the config, so the check WAITS for that line instead of
#     sleeping a constant and racing the reload cadence.
#
#     The swap is written the way a writer IN PROGRESS leaves the file, on
#     purpose: the file is truncated, held empty across the bar's 2 s check,
#     and only then filled - and the finished file keeps the truncated file's
#     whole-second mtime, because a truncate+write inside one second produces
#     exactly that (the mtime has one-second resolution). A whole-second
#     mtime therefore cannot tell the empty file from the finished one, and a
#     bar that reads the empty file and moves its change token onto it loses
#     the change for good. The bar's token carries the nanoseconds and the
#     size as well, so the finished file still reads as a change and gets
#     re-read - which is what the refusal line below proves.
CFG="$WORK/home/.wizbar/config.json"
: > "$CFG"                              # the writer truncates the file
cp -p "$CFG" "$WORK/cfg.mtime"          # the timestamp the writer's write carries
sleep 5                                 # the bar's 2 s check reads the EMPTY file
cat > "$CFG" <<JSON
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 1, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": false },
  "modules": { "pet": { "enabled": true, "label": "pet",
                          "exePath": "$WORK/home" } }
}
JSON
touch -r "$WORK/cfg.mtime" "$CFG"       # same whole-second mtime as the empty file
wait_log "pet path is not an absolute path to a regular executable file" 10
REFUSED=$?
STARTED_N=$(grep -c "pet: starting" "$WORK/bar.log" || true)
STOPPED_N=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
[ "$REFUSED" -eq 0 ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar.log" || true)" -eq "$STARTED_N" ] &&
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq "$STOPPED_N" ] &&
    kill -0 "$DECOY_PID" 2>/dev/null
check "a directory pet path is refused and the chip is inert (decoy survives)" $?
kill "$DECOY_PID" 2>/dev/null
DECOY_PID=""

# 6i. a start the kernel refuses reports THAT, and claims nothing. The path
#     passes the bar's own checks - absolute, a regular file, executable - and
#     the exec still fails: a 0755 text file with no shebang returns ENOEXEC.
#     The exec's outcome travels back over the CLOEXEC pipe, so a refused exec
#     leaves the failure line and NO "starting" line at all, and nothing is left
#     running. The config is re-read on the bar's own tick, which only runs
#     while the event loop is idle, so the click is repeated SLOWLY (each gap
#     lets one tick pass; until the reload the chip still answers for the
#     directory path above, which is refused silently) until the new path is in
#     force.
printf 'not a program\n' > "$WORK/home/notpet"
chmod +x "$WORK/home/notpet"
cat > "$CFG" <<JSON
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 1, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": false },
  "modules": { "pet": { "enabled": true, "label": "pet",
                          "exePath": "$WORK/home/notpet" } }
}
JSON
for _ in $(seq 1 20); do
    "$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
    sleep 1.2
    grep -q "pet: cannot start $WORK/home/notpet" "$WORK/bar.log" && break
done
grep -q "pet: cannot start $WORK/home/notpet" "$WORK/bar.log" &&
    ! grep -q "pet: started" "$WORK/bar.log" &&
    [ "$(grep -c "pet: starting: $WORK/home/notpet" "$WORK/bar.log" || true)" -eq 0 ] &&
    [ ! -e "$WORK/pet.pid" ]
check "a pet that cannot exec logs 'cannot start' and claims no start" $?

# 6i-2. the refused start leaves the bar alive and tidy: the failed exec's
#       child is gone, so the bar must have reaped it (no zombie) and its own
#       loop must still answer a click (the menu opens).
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 1.5
menu_open && [ "$(zombies)" -eq 0 ]
check "the bar still opens the menu after a refused pet start, and reaps its child" $?
"$XCLICK" 100 300 1 full
sleep 0.8

# 6j. a SLOW exec must not park the bar's loop. The click that starts the pet
#     parks the CLOEXEC handshake and returns; the tick settles it once the
#     child's exec lands. The pre-fix bar sat in a blocking read() on that pipe
#     until the exec completed, so a slow exec - a big binary on cold storage -
#     froze the clock, the menu and every click for as long as the storage
#     took. The slowness is injected where it really happens: a preload shim
#     sleeps inside the child's execv before handing it to the real one, so the
#     handshake is guaranteed pending for the whole check. While it pends the
#     bar must still open the menu on a right-click and follow a fresh terminal
#     report on its own tick, and once the exec lands the one true pair must
#     hold: "starting", and no "cannot start". The pet config is restored
#     first because the restart reads it at startup, so the swap needs no
#     reload wait, and the shim bar gets its own log so the greps below see
#     only its own lines.
cat > "$CFG" <<JSON
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 1, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": false },
  "modules": { "pet": { "enabled": true, "label": "pet",
                          "exePath": "$WORK/home/pet" } }
}
JSON
cat > "$WORK/execslow.c" <<'SHIM'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>
static int (*real_execv)(const char *, char *const[]);
static long slow_ms;
__attribute__((constructor)) static void slowExecInit(void) {
    real_execv = dlsym(RTLD_NEXT, "execv");
    const char *ms = getenv("CHOCOBAR_TEST_SLOW_EXEC_MS");
    if (ms && *ms) slow_ms = strtol(ms, NULL, 10);
}
int execv(const char *path, char *const argv[]) {
    if (slow_ms > 0 && real_execv) {
        struct timespec ts = { slow_ms / 1000, (slow_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return real_execv ? real_execv(path, argv) : -1;
}
SHIM
gcc -shared -fPIC -O1 -o "$WORK/execslow.so" "$WORK/execslow.c" -ldl 2>>"$WORK/cc.log" ||
    { echo "FAIL: cannot build the slow-exec shim"; cat "$WORK/cc.log"; exit 1; }
kill "$BAR_PID" 2>/dev/null
wait "$BAR_PID" 2>/dev/null
BAR_PID=""
for _ in $(seq 1 50); do
    report_fast kitty 900 100 300 1600 && sleep 0.05 || break
done
for _ in $(seq 1 50); do
    [ -z "$(xdotool search --class chocobar 2>/dev/null)" ] && break
    sleep 0.05
done
CHOCOBAR_TEST_SLOW_EXEC_MS=4000 LD_PRELOAD="$WORK/execslow.so" \
    HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$WORK/bar4.log" 2>&1 &
BAR_PID=$!
BARWIN=""
for _ in $(seq 1 60); do
    BARWIN=$(xdotool search --class chocobar 2>/dev/null | head -1)
    [ -n "${BARWIN:-}" ] && break
    sleep 0.2
done
[ -n "${BARWIN:-}" ] || { echo "FAIL: the slow-exec bar never opened a window"; cat "$WORK/bar4.log"; exit 1; }
sleep 0.5
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
# the click that starts the pet: the handshake is pending from here for 4 s
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 0.4
# still inside the pending window: the menu must open NOW, not 4 s from now -
# a bar parked in a blocking read only reaches this click after the exec
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.6
menu_open
check "the bar opens the menu while the pet's exec handshake is pending" $?
"$XCLICK" 100 300 1 full
sleep 0.5
# and the tick kept running: a fresh report must move the bar inside the same
# pending window (a blocked loop has no tick, so the bar would stay put)
report_fast kitty 900 100 320 1600
sleep 1.4
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
WANT_Y_SLOW=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (320-30-12)*s}')
[ "$Y" -ge $((WANT_Y_SLOW-8)) ] && [ "$Y" -le $((WANT_Y_SLOW+8)) ]
check "the clock kept ticking while the handshake pended (y=$Y want~$WANT_Y_SLOW)" $?
# once the exec lands, the one true line - and the pet it started is alive
wait_log "pet: starting: $WORK/home/pet" 10 "$WORK/bar4.log"
SLOW_SETTLED=$?
sleep 0.5
[ "$SLOW_SETTLED" -eq 0 ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar4.log" || true)" -eq 1 ] &&
    ! grep -q "pet: cannot start" "$WORK/bar4.log" &&
    [ -s "$WORK/pet.pid" ] && kill -0 "$(cat "$WORK/pet.pid")" 2>/dev/null &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "running" ]
check "the slow exec settles as the one true line (starting, no cannot start, pet alive)" $?

# 6j-2. the refused half of the pair under the same slowness, and the
#       one-handshake guard: the pet is stopped first, the config moves to the
#       file the kernel refuses (ENOEXEC), and the start click forks a child
#       whose exec fails four seconds later. A second click inside that window
#       must fork nothing - the matcher cannot see a fork whose exec has not
#       landed, so the guard is what keeps one click from starting a duplicate
#       - and the refusal must land as exactly one "cannot start" with no
#       "starting" at all.
sleep 2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.5
grep -q "pet: stopped" "$WORK/bar4.log" &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
check "the pet the slow exec started stops on a click" $?
cat > "$CFG" <<JSON
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 1, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": false },
  "modules": { "pet": { "enabled": true, "label": "pet",
                          "exePath": "$WORK/home/notpet" } }
}
JSON
sleep 2.5
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
# the handshake is pending: this second click must be a no-op, not a second fork
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 0.3
wait_log "pet: cannot start $WORK/home/notpet" 10 "$WORK/bar4.log"
REFUSED_SLOW=$?
sleep 5.5
[ "$REFUSED_SLOW" -eq 0 ] &&
    [ "$(grep -c "pet: cannot start $WORK/home/notpet" "$WORK/bar4.log" || true)" -eq 1 ] &&
    [ "$(grep -c "pet: starting: $WORK/home/notpet" "$WORK/bar4.log" || true)" -eq 0 ] &&
    [ ! -e "$WORK/pet.pid" ]
check "a second click while the handshake pends forks nothing (one cannot start, no starting)" $?

# 6j-3. the parked handshake has a DEADLINE, and a refused click is never
#       silent. The pre-deadline bar parked the pipe for ever: a child stuck
#       inside execv (an unresponsive mount) held the write end open, so every
#       later click returned at the guard with NO log line at all and the stuck
#       child was never reaped. Two halves, both observable in the bar's own
#       log:
#       (a) while the handshake pends, a click is LOGGED as ignored - the user
#           made a click and the bar says why nothing happened;
#       (b) when the budget passes, the tick closes the pipe, logs that the
#           start never reported, and CLEARS the state - so the bar is not
#           wedged for ever and the next click can try again.
#       This needs its OWN bar: the shim's delay is fixed when the bar starts,
#       and it must outlast the bar's own deadline for the deadline to be what
#       settles the handshake (the 4 s bar above settles on its exec first,
#       which is the point of that check). 12 s against a 5 s budget. The child
#       itself is not killed: its exec still lands when the storage answers, so
#       the pet runs and the bar reaps it - the deadline stopped the WAIT, not
#       the child, which is what makes it an honest report rather than a kill.
cat > "$CFG" <<JSON
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 1, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": false },
  "modules": { "pet": { "enabled": true, "label": "pet",
                          "exePath": "$WORK/home/pet" } }
}
JSON
kill "$BAR_PID" 2>/dev/null
wait "$BAR_PID" 2>/dev/null
BAR_PID=""
for _ in $(seq 1 50); do
    [ -z "$(xdotool search --class chocobar 2>/dev/null)" ] && break
    sleep 0.05
done
CHOCOBAR_TEST_SLOW_EXEC_MS=12000 LD_PRELOAD="$WORK/execslow.so" \
    HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$WORK/bar6.log" 2>&1 &
BAR_PID=$!
BARWIN6=""
for _ in $(seq 1 60); do
    BARWIN6=$(xdotool search --class chocobar 2>/dev/null | head -1)
    [ -n "${BARWIN6:-}" ] && break
    sleep 0.2
done
[ -n "${BARWIN6:-}" ] || { echo "FAIL: the deadline bar never opened a window"; cat "$WORK/bar6.log"; exit 1; }
sleep 0.5
# the report goes to the bar over its own bus name, so it can only be sent
# once the bar owns it - then the bar sits where the earlier ones did, and the
# pet chip is at the same offset in the window
report_fast kitty 900 100 300 1600
sleep 2
eval "$(xdotool getwindowgeometry --shell "$BARWIN6")"
rm -f "$WORK/pet.pid" "$WORK/pet.state"
DEADLINE_BEFORE=$(grep -c "did not report" "$WORK/bar6.log" || true)
IGNORED_BEFORE=$(grep -c "this click was ignored" "$WORK/bar6.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
# (a) the refused click: the handshake is still parked, so this click cannot
#     start anything - and it must say so
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 0.5
[ "$(grep -c "this click was ignored" "$WORK/bar6.log" || true)" -eq $((IGNORED_BEFORE+1)) ]
check "a click refused while the handshake pends is logged, not silent" $?
# and the bar is still live: the loop never parked on the child's exec
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
menu_open
check "the bar still opens the menu while the handshake pends" $?
"$XCLICK" 100 300 1 full
sleep 0.5
# (b) the deadline: the tick gives up waiting, logs it, and clears the state
wait_log "did not report within" 12 "$WORK/bar6.log"
DEADLINE_HIT=$?
sleep 0.5
[ "$DEADLINE_HIT" -eq 0 ] &&
    [ "$(grep -c "did not report" "$WORK/bar6.log" || true)" -eq $((DEADLINE_BEFORE+1)) ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar6.log" || true)" -eq 0 ] &&
    [ "$(grep -c "pet: cannot start" "$WORK/bar6.log" || true)" -eq 0 ]
check "the parked handshake reports that its start never reported, and claims no start" $?
# the abandoned child still lands its exec: the deadline stopped the WAIT, not
# the child, so the pet runs - the pet's own pid file is the oracle, because
# the bar had already given the handshake up and never heard the exec
for _ in $(seq 1 200); do
    [ -s "$WORK/pet.pid" ] && break
    sleep 0.1
done
sleep 2
[ -s "$WORK/pet.pid" ] && kill -0 "$(cat "$WORK/pet.pid")" 2>/dev/null &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "running" ]
check "the abandoned child still lands its exec, and the pet runs" $?
# and the chip can stop it: the chip's state source is the 1 Hz matcher, which
# sees the pet the abandoned child started, so the click takes the STOP path
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.5
grep -q "pet: stopped" "$WORK/bar6.log" && [ ! -e "$WORK/pet.pid" ] &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ] && [ "$(zombies)" -eq 0 ]
check "the pet the deadline did not kill is stopped by a click and leaves no zombie" $?

# 6j-4. a menu held open past the deadline must not count against the park's
#       budget. The budget measures time the loop was free to drain, and the
#       menu's nested event loop owns the clock while it is open, so a start
#       that outlived its budget only because the user held a menu open must
#       NOT be abandoned ("not confirmed"), and the next click must be refused
#       by the still-pending handshake - never forked into a second pet. The
#       12 s shim bar above is still running: click the pet, hold the menu open
#       past the 5 s budget, close it, and click the pet once more - one
#       start, one refused click, no deadline line.
rm -f "$WORK/pet.pid" "$WORK/pet.state"
MENU_SLOW_STARTED=$(grep -c "pet: starting" "$WORK/bar6.log" || true)
MENU_SLOW_DEADLINE=$(grep -c "did not report" "$WORK/bar6.log" || true)
MENU_SLOW_IGNORED=$(grep -c "this click was ignored" "$WORK/bar6.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 0.6
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
menu_open
check "the menu is open while the pet start pends (the deadline budget check)" $?
# hold the menu open past the 5 s budget, then close it and click the chip once
sleep 7
"$XCLICK" 100 300 1 full
sleep 1.2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 0.5
[ "$(grep -c "did not report" "$WORK/bar6.log" || true)" -eq "$MENU_SLOW_DEADLINE" ] &&
    [ "$(grep -c "this click was ignored" "$WORK/bar6.log" || true)" -eq $((MENU_SLOW_IGNORED+1)) ]
check "a menu held open past the deadline does not abandon the pending start (click refused, no not-confirmed)" $?
# the exec still lands and settles as the one true line, once
wait_log "pet: starting: $WORK/home/pet" 10 "$WORK/bar6.log"
MENU_SLOW_SETTLED=$?
sleep 0.5
[ "$MENU_SLOW_SETTLED" -eq 0 ] &&
    [ "$(grep -c "pet: starting" "$WORK/bar6.log" || true)" -eq $((MENU_SLOW_STARTED+1)) ] &&
    [ -s "$WORK/pet.pid" ] && kill -0 "$(cat "$WORK/pet.pid")" 2>/dev/null
check "the start the menu protected settles as one starting line and one pet" $?
# the chip's state source is the 1 Hz tick, so let one poll see the pet before
# the click below: the click reads g_m.petRunning, not /proc. The bar's own
# geometry is re-read here too: the menu hold plus the waits above can outlive
# the 30 s geometry-report freshness, and the bar then falls back to the screen
# top at its own content width, so a coordinate from earlier is off the window
sleep 2
eval "$(xdotool getwindowgeometry --shell "$BARWIN6")"
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.5
grep -q "pet: stopped" "$WORK/bar6.log" && [ ! -e "$WORK/pet.pid" ] &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ] && [ "$(zombies)" -eq 0 ]
check "the pet the protected start ran stops on a click and leaves no zombie" $?
grep -q "pet: stopped" "$WORK/bar6.log" && [ ! -e "$WORK/pet.pid" ] &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ] && [ "$(zombies)" -eq 0 ]
check "the pet the protected start ran stops on a click and leaves no zombie" $?

# back to the ordinary bar: the shim was this check's tool, not a property of
# the bar, and the sections below were written against an unshimmed one
kill "$BAR_PID" 2>/dev/null
wait "$BAR_PID" 2>/dev/null
BAR_PID=""
for _ in $(seq 1 50); do
    report_fast kitty 900 100 300 1600 && sleep 0.05 || break
done
for _ in $(seq 1 50); do
    [ -z "$(xdotool search --class chocobar 2>/dev/null)" ] && break
    sleep 0.05
done
HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >>"$WORK/bar.log" 2>&1 &
BAR_PID=$!
BARWIN=""
for _ in $(seq 1 60); do
    BARWIN=$(xdotool search --class chocobar 2>/dev/null | head -1)
    [ -n "${BARWIN:-}" ] && break
    sleep 0.2
done
[ -n "${BARWIN:-}" ] || { echo "FAIL: the restored bar never opened a window"; cat "$WORK/bar.log"; exit 1; }
sleep 0.5
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"

# 7. the bar rides above the terminal: a geometry report moves it.
#    The move happens on the bar's own tick (at most once a second), so the
#    waits below are generous on purpose - a flaky sleep must not look like a
#    broken bar.
report kitty 900 100 300 1600
sleep 2
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
# the strip's bottom must sit 12 logical px (the gap) above the terminal's top
# edge
WANT_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (300-30-12)*s}')
WANT_W=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", 900*s}')
[ "$Y" -ge $((WANT_Y-8)) ] && [ "$Y" -le $((WANT_Y+8)) ] && [ "$WIDTH" -ge $((WANT_W-8)) ] && [ "$WIDTH" -le $((WANT_W+8)) ]
check "the bar rides just above the reported terminal (y=$Y want~$WANT_Y, w=$WIDTH want~$WANT_W)" $?

# 7a. a terminal NARROWER than the bar's own content must not shrink it: the
#     width is resizeToContent()'s decision alone - the chips win over a narrow
#     report and the bar sticks out over the terminal - so a 60-logical report
#     leaves the bar wider than the report and above the 80-px floor, instead
#     of clipping the chips down to the terminal.
WANT_NARROW=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", 60*s+8}')
report kitty 60 100 300 1600
sleep 2
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
[ "$WIDTH" -ge 80 ] && [ "$WIDTH" -gt "$WANT_NARROW" ]
check "a terminal narrower than the bar does not shrink it (w=$WIDTH, the report said 60 logical)" $?

# The autostart child's own budget path is not covered by these checks: the
# fakes answer immediately, and the case that would tell a bounded reap from a
# blocking wait - a child that survives SIGKILL in uninterruptible sleep -
# cannot be made deterministically in this harness (it needs a hung kernel
# resource such as a wedged mount), so runTimed's kill window is trusted from
# reading it, not from a click.
#
# 7b. a menu open spawns the autostart child whose stderr pipe is the one that
#     used to hang the bar (the fake systemctl writes to stderr and exits): the
#     menu must open anyway - the spawn, the stderr drain and the wait all
#     completed inside the open - and the child must actually have run (its
#     call landed in the log)
CALLS_BEFORE=$(calls_n)
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
menu_open && [ "$(calls_n)" -eq $((CALLS_BEFORE+1)) ]
check "a menu open spawns the stderr-writing child and still opens (the pipe cannot block the loop)" $?

# 7c. the bar is still responsive to clicks after the child: the menu opens again
"$XCLICK" 100 300 1 full
sleep 0.8
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
menu_open
check "after the child the bar still opens the menu on a click" $?
"$XCLICK" 100 300 1 full
sleep 0.8

# 7d. and the clock still ticks: the move below only happens in the bar's own
#     1 s tick branch, which a blocked loop would never reach - the report moves
#     the terminal and the bar must follow it
report kitty 900 100 350 1600
sleep 2
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
WANT_Y2=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (350-30-12)*s}')
[ "$Y" -ge $((WANT_Y2-8)) ] && [ "$Y" -le $((WANT_Y2+8)) ]
check "the clock still ticks after the child (the bar followed a fresh report to y=$Y want~$WANT_Y2)" $?

# press a button on a menu row: open the menu, put the pointer on the row and
# press+release the button there. The row offset is in logical px from the
# menu's top edge (menu.c: MENU_PAD, MENU_ROW, MENU_SEP). Echoes the menu's
# window id, or nothing at all when the menu never opened.
row_press() { # row_press <button> <row offset>
    "$XCLICK" $((X+40)) $((Y+10)) 3 full
    sleep 0.8
    local mw="" mw_dev="" mh_dev="" mg="" mx="" my=""
    read -r mw mw_dev mh_dev <<EOF
$(menu_wh)
EOF
    [ -n "$mw" ] || return 1
    mg=$(xdotool getwindowgeometry --shell "$mw")
    mx=$(echo "$mg" | awk -F= '/^X=/{print $2}')
    my=$(echo "$mg" | awk -F= '/^Y=/{print $2}')
    "$XCLICK" $((mx + mw_dev / 2)) $((my + $2)) "$1" full
    sleep 0.8
    echo "$mw"
}

# 7e-a. a WHEEL over a menu row must choose nothing. Core X11 has no scroll
#     events: a wheel notch is a ButtonPress(4|5) followed by a
#     ButtonRelease(4|5), and the menu's pointer grab delivers both of them to
#     the menu. The release used to pick a row without asking which button it
#     was, so one scroll over "Start on login" toggled the systemd unit. The
#     oracle is the fake systemctl's own call log: a pick would add an
#     enable/disable call, and the stateful fake in place WOULD record it (the
#     failing fake would swallow the toggle, so the count would not move even
#     for a pick - the fake has to be the stateful one for this to mean
#     anything). The left click in 7e is the counterpart: the row still works.
mv "$WORK/fakebin/systemctl" "$WORK/fakebin/systemctl-fail"
mv "$WORK/fakebin/systemctl-good" "$WORK/fakebin/systemctl"
ED_BEFORE=$(grep -cE "call: --user (enable|disable) chocobar" "$CALLS" || true)
XDG_BEFORE=$(xdg_opens_n)
MW=$(row_press 4 "$ROW_AUTO_Y")
[ -n "$MW" ] && ! menu_open &&
    [ "$(grep -cE "call: --user (enable|disable) chocobar" "$CALLS" || true)" -eq "$ED_BEFORE" ] &&
    [ "$(xdg_opens_n)" -eq "$XDG_BEFORE" ] &&
    xwininfo -id "$BARWIN" | grep -q "Map State: IsViewable"
check "a wheel notch over a menu row picks nothing and closes the menu" $?

# 7e-b. a MIDDLE click is a click like any other, and it picks nothing either:
#     only the left button may choose a row. The oracle is the fake xdg-open's
#     own record - a pick on "Check for updates" would run the program that
#     opens the releases page.
XDG_BEFORE=$(xdg_opens_n)
MW=$(row_press 2 "$ROW_UPDATES_Y")
[ -n "$MW" ] && ! menu_open &&
    [ "$(xdg_opens_n)" -eq "$XDG_BEFORE" ] &&
    [ "$(grep -cE "call: --user (enable|disable) chocobar" "$CALLS" || true)" -eq "$ED_BEFORE" ] &&
    xwininfo -id "$BARWIN" | grep -q "Map State: IsViewable"
check "a middle click over a menu row picks nothing and closes the menu" $?

# 7e. the autostart row acts through the stateful fake swapped in above:
#     is-enabled answers disabled, enable records its call and succeeds. The
#     fake is swapped in by renaming so the bar's own PATH resolves it; the
#     failing fake stays for the unknown-state checks on either side of this
#     pair.
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
MENU_WIN=""
read -r MENU_WIN MENU_W_DEV MENU_H_DEV <<EOF
$(menu_wh)
EOF
if [ -n "$MENU_WIN" ]; then
    MGEO=$(xdotool getwindowgeometry --shell "$MENU_WIN")
    MENU_X=$(echo "$MGEO" | awk -F= '/^X=/{print $2}')
    MENU_Y=$(echo "$MGEO" | awk -F= '/^Y=/{print $2}')
    "$XCLICK" $((MENU_X + MENU_W_DEV / 2)) $((MENU_Y + ROW_AUTO_Y)) 1 full
fi
sleep 0.8
[ -n "$MENU_WIN" ] && ! menu_open && grep -q "call: --user enable chocobar" "$CALLS"
check "clicking Start on login enables the unit" $?

# 7f. the enabled state round-trips: the next menu open reads enabled (the
#     stateful fake exits 0 - the same value that draws the tick) and the next
#     click on the row disables again
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
MENU_WIN=""
read -r MENU_WIN MENU_W_DEV MENU_H_DEV <<EOF
$(menu_wh)
EOF
if [ -n "$MENU_WIN" ]; then
    MGEO=$(xdotool getwindowgeometry --shell "$MENU_WIN")
    MENU_X=$(echo "$MGEO" | awk -F= '/^X=/{print $2}')
    MENU_Y=$(echo "$MGEO" | awk -F= '/^Y=/{print $2}')
    "$XCLICK" $((MENU_X + MENU_W_DEV / 2)) $((MENU_Y + ROW_AUTO_Y)) 1 full
fi
sleep 0.8
[ -n "$MENU_WIN" ] && ! menu_open && grep -q "call: --user disable chocobar" "$CALLS"
check "the enabled state round-trips (the next click disables)" $?

# 7g. with the always-failing fake back, clicking the row must add NO
#     enable/disable call: the tri-state guard suppresses the toggle when the
#     read failed
mv "$WORK/fakebin/systemctl" "$WORK/fakebin/systemctl-good"
mv "$WORK/fakebin/systemctl-fail" "$WORK/fakebin/systemctl"
ED_BEFORE=$(grep -cE "call: --user (enable|disable) chocobar" "$CALLS" || true)
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
MENU_WIN=""
read -r MENU_WIN MENU_W_DEV MENU_H_DEV <<EOF
$(menu_wh)
EOF
if [ -n "$MENU_WIN" ]; then
    MGEO=$(xdotool getwindowgeometry --shell "$MENU_WIN")
    MENU_X=$(echo "$MGEO" | awk -F= '/^X=/{print $2}')
    MENU_Y=$(echo "$MGEO" | awk -F= '/^Y=/{print $2}')
    "$XCLICK" $((MENU_X + MENU_W_DEV / 2)) $((MENU_Y + ROW_AUTO_Y)) 1 full
fi
sleep 0.8
[ -n "$MENU_WIN" ] && ! menu_open &&
    [ "$(grep -cE "call: --user (enable|disable) chocobar" "$CALLS" || true)" -eq "$ED_BEFORE" ]
check "an unreadable state suppresses the enable/disable call" $?

# 7h. a MASKED unit is unknown, not disabled. systemctl exits 1 for a masked unit
#     exactly as it does for a disabled one, so a bar that reads the exit code
#     shows "Start on login" unticked and flips the unit blind. The deciding
#     input must be the state WORD on stdout: "masked" is neither enabled nor
#     disabled, so the bar logs unknown and the toggle click adds no
#     enable/disable call. The call log is the oracle - a bar that read the exit
#     code would have issued "enable" here.
mv "$WORK/fakebin/systemctl" "$WORK/fakebin/systemctl-fail"
mv "$WORK/fakebin/systemctl-masked" "$WORK/fakebin/systemctl"
ED_BEFORE=$(grep -cE "call: --user (enable|disable) chocobar" "$CALLS" || true)
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
MENU_WIN=""
read -r MENU_WIN MENU_W_DEV MENU_H_DEV <<EOF
$(menu_wh)
EOF
if [ -n "$MENU_WIN" ]; then
    MGEO=$(xdotool getwindowgeometry --shell "$MENU_WIN")
    MENU_X=$(echo "$MGEO" | awk -F= '/^X=/{print $2}')
    MENU_Y=$(echo "$MGEO" | awk -F= '/^Y=/{print $2}')
    "$XCLICK" $((MENU_X + MENU_W_DEV / 2)) $((MENU_Y + ROW_AUTO_Y)) 1 full
fi
sleep 0.8
[ -n "$MENU_WIN" ] && ! menu_open &&
    [ "$(grep -cE "call: --user (enable|disable) chocobar" "$CALLS" || true)" -eq "$ED_BEFORE" ] &&
    grep -q 'answer "masked"' "$WORK/bar.log"
check "a masked unit reads as unknown: logged, and the toggle flips nothing" $?
mv "$WORK/fakebin/systemctl" "$WORK/fakebin/systemctl-masked"
mv "$WORK/fakebin/systemctl-fail" "$WORK/fakebin/systemctl"

# 7i. the "Check for updates" row runs the program it names. The bar resolves
#     the program to an absolute path BEFORE the fork - a child of a fork in
#     this threaded bar may not allocate, and the PATH search allocates - so
#     the fake xdg-open in the bar's own PATH is the one that runs, and its
#     record proves the whole path: resolve, fork, exec.
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.8
MENU_WIN=""
read -r MENU_WIN MENU_W_DEV MENU_H_DEV <<EOF
$(menu_wh)
EOF
if [ -n "$MENU_WIN" ]; then
    MGEO=$(xdotool getwindowgeometry --shell "$MENU_WIN")
    MENU_X=$(echo "$MGEO" | awk -F= '/^X=/{print $2}')
    MENU_Y=$(echo "$MGEO" | awk -F= '/^Y=/{print $2}')
    "$XCLICK" $((MENU_X + MENU_W_DEV / 2)) $((MENU_Y + ROW_UPDATES_Y)) 1 full
fi
sleep 1.5
[ -n "$MENU_WIN" ] && ! menu_open &&
    grep -q "chocobar/releases/latest" "$WORK/xdg-open-calls"
check "the Check for updates row runs the program it names (the PATH is resolved before the fork)" $?

# 8. a terminal too high to fit the strip above it hides the bar
report kitty 900 100 5 1600
sleep 2
xwininfo -id "$BARWIN" | grep -q "Map State: IsUnMapped"
check "a terminal too high HIDES the bar (Windows hideBar parity)" $?

# 9. and it comes back when the terminal moves down again
report kitty 900 100 300 1600
sleep 2
xwininfo -id "$BARWIN" | grep -q "Map State: IsViewable"
check "the bar comes back when the terminal moves down" $?

# 9b. a report that CHANGES the terminal's width must move BOTH the bar's
#     window and its rounded SHAPE: the shape region is in window coordinates,
#     so a resize that skipped re-applying it would leave the window clipped
#     to the old rounded rect. The expected width is derived from the width the
#     900-wide report produced (a ratio, so no DPI guess), and the shape's own
#     bounding rectangle (xwininfo -shape) is the observable that catches a
#     stale shape still reading the old width.
WANT_W2=$(awk -v w="$WIDTH" 'BEGIN{printf "%.0f", w*1100/900}')
report kitty 1100 100 300 1600
sleep 2
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
SHAPE_W=$(xwininfo -id "$BARWIN" -shape | awk '/Window shape extents:/ {split($4, a, "x"); print a[1]}')
[ "$WIDTH" -ge $((WANT_W2-8)) ] && [ "$WIDTH" -le $((WANT_W2+8)) ] &&
    [ -n "$SHAPE_W" ] && [ "$SHAPE_W" -ge $((WANT_W2-8)) ] && [ "$SHAPE_W" -le $((WANT_W2+8)) ]
check "a width change moves the window AND its shape (w=$WIDTH shape=$SHAPE_W want~$WANT_W2)" $?

# 10. a STALE report (the helper stopped, KWin restarted) is dropped after 30 s
#     and the bar falls back to the screen top instead of pinning itself to a
#     terminal that no longer exists. This is the one check that has to wait out
#     the bar's own 30 s staleness window; there is no way to age a cached
#     report from outside the process.
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
STALE_FROM=$Y
sleep 31
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
[ "$STALE_FROM" -ge $((WANT_Y-8)) ] && [ "$Y" -ge $((WANT_TOP-4)) ] && [ "$Y" -le $((WANT_TOP+4)) ]
check "a stale geometry report falls back to the screen top (y=$Y want~$WANT_TOP)" $?

# 11. a terminal reported BEFORE the first map makes the bar BORN at its final
#     position: start a fresh bar, report the terminal while it is still inside
#     its startup geometry wait, and read the window geometry at the first
#     moment it exists. The bar must already sit at the reported terminal - not
#     first at the screen-top centre and then jump one tick later.
#     The report therefore goes out over dbus-send: over gdbus it lands
#     seconds late, long after the first map, and the check would then measure
#     a later tick instead of the position the bar was BORN at.
kill "$BAR_PID" 2>/dev/null
wait "$BAR_PID" 2>/dev/null
BAR_PID=""
# wait for the dead bar's bus name to be released (the report keeps working
# while the old bar still owns it)
for _ in $(seq 1 50); do
    report_fast kitty 900 100 300 1600 && sleep 0.05 || break
done
for _ in $(seq 1 50); do
    [ -z "$(xdotool search --class chocobar 2>/dev/null)" ] && break
    sleep 0.05
done
HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$WORK/bar2.log" 2>&1 &
BAR_PID=$!
for _ in $(seq 1 30); do
    report_fast kitty 900 100 300 1600 && break
    sleep 0.05
done
BARWIN2=""
for _ in $(seq 1 60); do
    BARWIN2=$(xdotool search --class chocobar 2>/dev/null | head -1)
    if [ -n "$BARWIN2" ]; then
        eval "$(xdotool getwindowgeometry --shell "$BARWIN2")"
        [ "${WIDTH:-0}" -gt 100 ] && [ "${X:-0}" -gt 0 ] && break
    fi
    sleep 0.05
done
[ -n "$BARWIN2" ] && [ "${WIDTH:-0}" -gt 100 ] ||
    { echo "FAIL: the fresh bar never opened a window"; cat "$WORK/bar2.log"; exit 1; }
WANT_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (300-30-12)*s}')
WANT_X=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", 100*s}')
WANT_W=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", 900*s}')
[ "$Y" -ge $((WANT_Y-8)) ] && [ "$Y" -le $((WANT_Y+8)) ] &&
    [ "$X" -ge $((WANT_X-8)) ] && [ "$X" -le $((WANT_X+8)) ] &&
    [ "$WIDTH" -ge $((WANT_W-8)) ] && [ "$WIDTH" -le $((WANT_W+8)) ]
check "a report before the first map makes the bar BORN at the terminal (x=$X y=$Y w=$WIDTH, want x~$WANT_X y~$WANT_Y w~$WANT_W)" $?

# 12. a terminal reported TOO HIGH before the bar starts must leave the bar
#     BORN hidden: positionInit() hides it before any map, so the window exists
#     (findable by class) but must never become viewable while the too-high
#     report is the fresh one - not even for the one or two frames a
#     map-then-unmap startup would have composited at the top. The visibility
#     watch below starts before the window exists and runs continuously, so ANY
#     map is caught; a roomy terminal must then bring the bar back through the
#     ordinary tick map, which for this window is the FIRST map of its life
#     (position and size hints set before it, the same way the startup map
#     would have set them).
#     The reports here go through report_fast (dbus-send), for the reason
#     report_fast spells out: a gdbus call stalls for seconds and lands after
#     the bar's 700 ms startup geometry wait - too late to be the report the
#     bar is BORN with.
kill "$BAR_PID" 2>/dev/null
wait "$BAR_PID" 2>/dev/null
BAR_PID=""
for _ in $(seq 1 50); do
    [ -z "$(xdotool search --class chocobar 2>/dev/null)" ] && break
    sleep 0.05
done
# the too-high report has to land INSIDE the bar's startup geometry wait, so
# the sender is retried in the background while the visibility watch runs in
# the foreground without a gap
# every all-desktops request so far belongs to a bar started earlier: this bar
# must add one of its own when the tick maps it
ALLDESK_BEFORE=$(grep -c '_NET_WM_DESKTOP 4294967295' "$CLIENTMSG" || true)
HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$WORK/bar3.log" 2>&1 &
BAR_PID=$!
( for _ in $(seq 1 60); do report_fast kitty 900 100 5 1600 && break; sleep 0.05; done ) &
SEED_PID=$!
FLASHED=0
for _ in $(seq 1 250); do
    if [ -n "$(xdotool search --onlyvisible --class chocobar 2>/dev/null)" ]; then
        FLASHED=1
        break
    fi
    sleep 0.01
done
kill "$SEED_PID" 2>/dev/null
wait "$SEED_PID" 2>/dev/null
[ "$FLASHED" -eq 0 ]
check "a too-high terminal at startup leaves the bar NEVER viewable" $?
BARWIN3=""
for _ in $(seq 1 60); do
    BARWIN3=$(xdotool search --class chocobar 2>/dev/null | head -1)
    [ -n "$BARWIN3" ] && break
    sleep 0.05
done
[ -n "$BARWIN3" ] || { echo "FAIL: the hidden bar never opened a window"; cat "$WORK/bar3.log"; exit 1; }
xwininfo -id "$BARWIN3" | grep -q "Map State: IsUnMapped"
check "the hidden startup left the window unmapped (it exists, it is just not mapped)" $?
report_fast kitty 900 100 300 1600
sleep 2
xwininfo -id "$BARWIN3" | grep -q "Map State: IsViewable"
check "the bar appears once a roomy terminal is reported" $?
# the tick's map must run the SAME post-map work the startup map does: the
# all-desktops request is what puts the bar on every workspace, and a bar born
# hidden is mapped only here, so without it that bar lives on one desktop
want_all_desktops "$BARWIN3" &&
    [ "$(grep -c '_NET_WM_DESKTOP 4294967295' "$CLIENTMSG" || true)" -gt "$ALLDESK_BEFORE" ]
check "the bar mapped by the tick asks for all desktops like a startup map" $?

# 12. the y is derived from the height the bar is DRAWN with, never from the raw
#     configured height. A configured height below 8 device px is unreadable, so
#     the bar is drawn 24 device px tall instead - and with the raw height in the
#     y arithmetic the strip's bottom edge sat about 16 logical px BELOW the
#     terminal's top edge, i.e. the bar overlapped the very terminal it is meant
#     to ride above. The observable is the window's own geometry on a bar
#     started with the tiny height (the config is read at startup). This runs
#     last because it is the only check that needs its own bar height: the
#     sections above measure the 30 px bar.
#
#     The scale is read off this bar's own WIDTH against the reported terminal
#     width, not off its height: the clamp makes the drawn height exactly 24
#     DEVICE px for every scale, so the height carries no scale information at
#     all (reading it back would always yield 1.0). The terminal is reported
#     WIDE enough that the width cannot be the chips' own, and the assertion
#     below proves it: a content-sized bar would be narrower than the report.
cat > "$CFG" <<JSON
{
  "bar": { "height": 4, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 1, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": false },
  "modules": { "pet": { "enabled": false, "label": "pet",
                          "exePath": "$WORK/home/pet" } }
}
JSON
kill "$BAR_PID" 2>/dev/null
wait "$BAR_PID" 2>/dev/null
BAR_PID=""
for _ in $(seq 1 50); do
    [ -z "$(xdotool search --class chocobar 2>/dev/null)" ] && break
    sleep 0.05
done
HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$WORK/bar5.log" 2>&1 &
BAR_PID=$!
BARWIN5=""
for _ in $(seq 1 60); do
    BARWIN5=$(xdotool search --class chocobar 2>/dev/null | head -1)
    [ -n "${BARWIN5:-}" ] && break
    sleep 0.2
done
[ -n "${BARWIN5:-}" ] || { echo "FAIL: the tiny-height bar never opened a window"; cat "$WORK/bar5.log"; exit 1; }
sleep 0.5
# the report goes to the bar over its own bus name, so it can only be sent
# once the bar owns it
report_fast kitty 1200 0 300 1600
sleep 2
eval "$(xdotool getwindowgeometry --shell "$BARWIN5")"
# the width must be the REPORT's width in device px (the terminal is far wider
# than the bar's own chips), which is what makes the scale readable
[ "$WIDTH" -ge 1200 ]
check "the tiny bar took the reported terminal's width (w=$WIDTH, report 1200 logical)" $?
TINY_SCALE=$(awk -v w="$WIDTH" 'BEGIN{printf "%.4f", w/1200.0}')
# the drawn height is the clamp's 24 DEVICE px whatever the scale is
[ "$HEIGHT" -eq 24 ]
check "the tiny configured height is drawn as the 24 device px floor (h=$HEIGHT)" $?
# the strip's bottom edge must sit the gap above the terminal's top edge - both
# in device px: the terminal's top is 300 logical px, the gap 12 logical px
BOTTOM=$((Y+HEIGHT))
WANT_BOTTOM=$(awk -v s="$TINY_SCALE" 'BEGIN{printf "%.0f", (300-12)*s}')
[ "$BOTTOM" -ge $((WANT_BOTTOM-3)) ] && [ "$BOTTOM" -le $((WANT_BOTTOM+3)) ]
check "the tiny bar's bottom edge rides the gap above the terminal (bottom=$BOTTOM want~$WANT_BOTTOM, y=$Y h=$HEIGHT scale=$TINY_SCALE)" $?
# and it must not overlap the terminal at all
OVERLAP=$(awk -v b="$BOTTOM" -v s="$TINY_SCALE" 'BEGIN{printf "%d", (b > 300*s) ? 1 : 0}')
[ "$OVERLAP" -eq 0 ]
check "the tiny bar's bottom edge stays above the terminal's top edge (no overlap)" $?

# ---------------------------------------------------------------------------
# 12b. HOVER: the pill and the title tooltip (the user's own request, 2026-10-23:
#      "hovering LHS buttons must highlight them; hovering RHS buttons must show
#      a label line saying what it is").
#
#      Two defects made that impossible and neither is visible to a click test:
#        * the bar selected ButtonMotionMask but NOT PointerMotionMask, so a
#          plain hover never produced a MotionNotify and onMotion never ran;
#        * onMotion handed tipShow the WINDOW-relative pointer position while
#          tipShow places the box at screen coordinates, so the tip landed in
#          the screen's top-left corner instead of under the pointer.
#      The tooltip is an override-redirect window of its own, so this reads the
#      root's child list rather than the bar's pixels. Both defects are only
#      visible as behaviour - a grep for the event mask or for the coordinate
#      field would pass on a branch that never runs - so the check below drives
#      the pointer and reads where the tip actually landed.
tip_win() { xwininfo -root -children 2>/dev/null | awk '/chocobar-tip/ { print $1; exit }'; }
eval "$(xdotool getwindowgeometry --shell "$BARWIN")"
Y_BAR=$Y
TIP_SEEN=0
TIP_NEAR=0
for frac in 3 12 22 32 42 52 62 72 82 92; do
    hx=$((WIDTH * frac / 100))
    HOVER_X=$((X + hx))
    xdotool mousemove "$HOVER_X" $((Y + HEIGHT / 2))
    sleep 0.35
    TW=$(tip_win)
    if [ -n "${TW:-}" ]; then
        eval "$(xdotool getwindowgeometry --shell "$TW")"
        TIP_SEEN=$((TIP_SEEN+1))
        # the tip must sit just under the pointer and roughly above/right of it.
        # This is the assertion that separates the two bugs: window-relative
        # coordinates land the box at a FIXED place (tipX = pointerX - barX), so
        # it would be ~0 on a centred bar and never track the pointer across the
        # strip. Allowing a band around the hovered x is what makes a pass mean
        # "the box followed the pointer" rather than "a box appeared".
        if [ "$Y" -ge "$Y_BAR" ] && [ "$X" -ge $((HOVER_X - 40)) ] \
           && [ "$X" -le $((HOVER_X + 40)) ]; then
            TIP_NEAR=$((TIP_NEAR+1))
        fi
    fi
done
[ "$TIP_SEEN" -ge 1 ] && [ "$TIP_NEAR" -ge 1 ]
check "hovering a chip shows its tooltip under the pointer ($TIP_SEEN tips, $TIP_NEAR tracked the pointer)" $?
# leave the bar the way a pointer really does: motion OUT of the window fires
# LeaveNotify, which is what tipHide hangs off. A single jump to the screen
# corner can be delivered as one synthetic motion with no leave event at all,
# so the move is made in two steps, the second landing outside the bar.
xdotool mousemove $((X + WIDTH / 2)) $((Y + HEIGHT + 30))
sleep 0.25
xdotool mousemove 5 5
sleep 0.45
TW=$(tip_win)
# tipHide UNMAPS the window rather than destroying it (one shared tip window,
# so a hover storm never allocates), so the check reads its map state
[ -z "${TW:-}" ] || xwininfo -id "$TW" 2>/dev/null | grep -qi "Map State: IsUnMapped"
check "moving off the bar hides the tooltip" $?

# ---------------------------------------------------------------------------
# 13. THE KWIN HELPER CAN NEVER BE STACKED.
#
#     geomwatch.js polls the COMPOSITOR'S MAIN THREAD every 250 ms. KWin runs
#     every loadScript as a NEW instance of the same file, so a second copy
#     doubles that work, a third triples it. Measured on this machine
#     2026-10-24: a bar restart raced the self-heal, two instances ended up
#     loaded, and KWin logged "The main thread was hanging temporarily!" for
#     over a minute while the whole desktop froze.
#
#     The suite's private session bus means the bar's KWin calls go nowhere, so
#     this cannot test KWin itself. What it CAN prove - and what actually
#     prevents the stack - is the bar's own contract:
#       a. the reload path unloads before it loads (one instance after any
#          reload, even a racing one);
#       b. the cross-process lock makes a SECOND bar process refuse to load
#          while the first one holds it, so two bars cannot both decide;
#       c. a stale lock (its owner is gone) is taken over instead of wedging
#          the bar out of ever loading the helper again.
#     (a), (b) and (c) are all exercised for real: (a) against a stub
#     org.kde.KWin on a second private bus, (b) and (c) through the bar's own
#     acquire via --self-test-lock.
#
# (a) is observable after all: a stub org.kde.KWin on the suite's own bus
#     answers isScriptLoaded and records every call. The bar is started against
#     it with the helper's file MISSING, so KWin reports "not loaded", the bar
#     reloads, and the recorded call sequence is the contract: an unload must
#     precede every load, and exactly one load must follow it. A stacked reload
#     shows up as two loads with no unload between them, or a load with no
#     unload before it at all.
if command -v python3 >/dev/null && python3 -c 'import dbus, gi' 2>/dev/null; then
    # a second bus: the one above carries the bar's own org.chocobar.Geom, and a
    # stub KWin must not disturb it. The address is BUILT from the socket path,
    # exactly as the suite's own bus address is, and never read back from the
    # daemon's log: the socket appears before the daemon prints its address, so
    # a log read can come back empty - and an empty DBUS_SESSION_BUS_ADDRESS
    # makes the bar fall back to the DEFAULT session bus, which is the user's.
    dbus-daemon --session --address="unix:path=$WORK/bus2" --print-address \
        --nofork >"$WORK/dbus2.log" 2>&1 &
    KWDPID=$!
    for _ in $(seq 1 50); do [ -S "$WORK/bus2" ] && break; sleep 0.2; done
    [ -S "$WORK/bus2" ] || { echo "FAIL: the KWin stub bus did not come up"; exit 1; }
    KWADDR="unix:path=$WORK/bus2"
    [ -n "$KWADDR" ] || { echo "FAIL: the KWin stub bus has no address"; exit 1; }
    KWLOG="$WORK/kwin-calls.log"
    : > "$KWLOG"
    # A private XDG_RUNTIME_DIR: the geometry lock lives there when it is set,
    # and the LIVE user bar holds that lock for its whole lifetime after its own
    # startup reload. Inheriting the real one makes the reload path refuse (the
    # lock is taken) and the check below sees zero loads - an environment leak,
    # not a product finding. HOME and DISPLAY were already private; this is the
    # same rule for the third global.
    STUBRUN="$WORK/run"
    mkdir -p "$STUBRUN"
    # loaded=0: KWin says the helper is NOT loaded, which is the only situation
    # in which the bar is allowed to reload it
    python3 "$HERE/kwstub.py" "$KWADDR" "$KWLOG" 0 >"$WORK/kwstub.log" 2>&1 &
    KWSTUB_PID=$!
    # Wait until the stub actually OWNS org.kde.KWin before the bar starts. The
    # bar probes once on its fresh connection and then not again for 30 s, so a
    # bar that starts while the stub is still importing loses the whole window
    # and the check below sees zero loads - a race that has already fired once.
    for _ in $(seq 1 100); do
        dbus-send --bus="$KWADDR" --dest=org.freedesktop.DBus --type=method_call \
            --print-reply /org/freedesktop/DBus \
            org.freedesktop.DBus.NameHasOwner string:org.kde.KWin 2>/dev/null |
            grep -q "boolean true" && break
        sleep 0.1
    done
    HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
        XDG_RUNTIME_DIR="$STUBRUN" \
        DBUS_SESSION_BUS_ADDRESS="$KWADDR" "$BIN" >"$WORK/bar-kwin.log" 2>&1 &
    KWBAR=$!
    for _ in $(seq 1 60); do
        grep -q '^loadScript ' "$KWLOG" && break
        sleep 0.2
    done
    sleep 1
    kill "$KWBAR" 2>/dev/null
    wait "$KWBAR" 2>/dev/null
    kill "$KWSTUB_PID" 2>/dev/null
    wait "$KWSTUB_PID" 2>/dev/null
    kill "$KWDPID" 2>/dev/null
    wait "$KWDPID" 2>/dev/null
    # every loadScript must be preceded, in the same log, by an unloadScript:
    # unload-then-load is the whole shape, and a load without its unload is how
    # a second live copy gets stacked in KWin
    read -r RELOAD_LOADS RELOAD_BAD <<<"$(awk '
        /^unloadScript / { seen_unload = 1; next }
        /^loadScript / {
            loads++
            if (!seen_unload) bad++
            seen_unload = 0
            next
        }
        END { printf "%d %d", loads + 0, bad + 0 }' "$KWLOG")"
    [ "${RELOAD_LOADS:-0}" -ge 1 ] && [ "${RELOAD_BAD:-1}" -eq 0 ]
    check "the helper reload unloads first and loads exactly once (${RELOAD_LOADS} loads, ${RELOAD_BAD} without a preceding unload)" $?
    # a zero-load result is a harness race, not a product finding: say so with
    # the evidence rather than leaving a bare number to interpret
    if [ "${RELOAD_LOADS:-0}" -lt 1 ]; then
        echo "  (KWin stub bus: $KWADDR)"
        echo "  (stub log:) $(cat "$WORK/kwstub.log" 2>/dev/null)"
        echo "  (bar log:) $(cat "$WORK/bar-kwin.log" 2>/dev/null)"
    fi
else
    # python3 + dbus are on every runner this suite targets; skipping would let
    # the gate pass on a machine that cannot prove the contract at all
    check "the helper reload unloads first and loads exactly once" 1
fi

# only the bar that OWNS org.chocobar.Geom may manage the compositor-side
# helper. Two bars both reloading it is what stacked a second 250 ms poller in
# KWin and froze the desktop on 2026-10-24, so a SECOND bar must stay out of the
# way - and that is observable in its own words: it cannot take the name, so it
# logs the ownership reason instead of managing anything. The bar above is still
# running, so this second process is exactly the dev-build-beside-the-service
# case the gate exists for.
SECOND_BIN_OUT="$WORK/bar2.log"
HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$SECOND_BIN_OUT" 2>&1 &
SECOND_BAR=$!
SECOND_SEEN=0
for _ in $(seq 1 40); do
    grep -q "already owns org.chocobar.Geom" "$SECOND_BIN_OUT" && { SECOND_SEEN=1; break; }
    kill -0 "$SECOND_BAR" 2>/dev/null || break
    sleep 0.2
done
kill "$SECOND_BAR" 2>/dev/null
wait "$SECOND_BAR" 2>/dev/null
[ "$SECOND_SEEN" -eq 1 ]
check "a second bar does not take the geometry name and says so ($SECOND_SEEN of 1)" $?
# --install-geometry must refuse while another bar owns the name, and say why.
# This is the CLI path of the same gate, run against the bar that is still
# running: it cannot own the name, so it must print the ownership advice (stop
# the service first), NOT the D-Bus/KWin diagnosis that would name a problem
# that does not exist.
INST_OUT=$(HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" "$BIN" --install-geometry 2>&1); INST_RC=$?
[ "$INST_RC" -ne 0 ] &&
    echo "$INST_OUT" | grep -q "another Chocobar instance is running and owns the" &&
    ! echo "$INST_OUT" | grep -q "could not talk to KWin"
check "--install-geometry refuses while another bar owns the name, and names the real reason" $?
# --remove-geometry has the same gate and must not delete the helper's file when
# it cannot unload it: the compositor would keep running the loaded script with
# its file gone, and "removed" would be a lie. Observable: the file survives and
# the exit status is non-zero.
SCRIPT_FILE="$WORK/home/.local/share/kwin/scripts/chocobar-geometry/contents/code/main.js"
mkdir -p "$(dirname "$SCRIPT_FILE")"
printf '// placeholder\n' > "$SCRIPT_FILE"
REM_OUT=$(HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" "$BIN" --remove-geometry 2>&1); REM_RC=$?
[ "$REM_RC" -ne 0 ] && [ -f "$SCRIPT_FILE" ] &&
    echo "$REM_OUT" | grep -q "another Chocobar instance is running and owns the"
check "--remove-geometry refuses while another bar owns the name, and keeps the file" $?
rm -f "$SCRIPT_FILE"

# the lock: a SECOND process must be refused while a live bar holds it, and a
# lock whose holder DIED (SIGKILL: systemd timeout, OOM, a killed dev build)
# must be taken over - that last one is the wedge: the bar silently gives up
# on bar.align 1 for the rest of its life. Both run through the bar's OWN
# acquire (main.c scriptLockAcquire, reached by --self-test-lock), and the
# refusal is observed against a REAL holder: --self-test-lock-hold takes the
# lock in a live process and marks it, so what is refused is an actual kernel
# lock rather than a file the suite seeded with a pid. There is no stale-pid
# case left to stage: the lock is an flock the kernel releases when the holder
# dies, and the bar never reads the lock file's pid line.
LOCK="$WORK/geom.lock"
mkdir -p "$(dirname "$LOCK")"
rm -f "$LOCK"
HELD="$WORK/geom.lock.holder"
rm -f "$HELD"
"$BIN" --self-test-lock-hold "$LOCK" "$HELD" 30 >/dev/null 2>&1 &
HOLDER=$!
for _ in $(seq 1 50); do [ -s "$HELD" ] && break; sleep 0.1; done
# (a) a lock held by a live process: refused, and the holder's own record in
#     the file is left standing
if [ -s "$HELD" ]; then
    HELD_PID=$(cat "$HELD")
    OUT=$("$BIN" --self-test-lock "$LOCK"); RC=$?
    [ "$RC" -ne 0 ] && echo "$OUT" | grep -q LOCK-REFUSED &&
        [ "$(cat "$LOCK")" = "$HELD_PID" ]
    check "a live holder's lock is refused and left untouched ($OUT)" $?
else
    check "a live holder's lock is refused and left untouched" 1
fi
# (b) the holder is SIGKILLed mid-hold - exactly how a bar dies under a
#     systemd timeout or OOM - and the next acquirer takes over with nothing
#     to judge: the kernel dropped the flock the moment the holder died
kill -9 "$HOLDER" 2>/dev/null
wait "$HOLDER" 2>/dev/null
OUT=$("$BIN" --self-test-lock "$LOCK"); RC=$?
[ "$RC" -eq 0 ] && echo "$OUT" | grep -q LOCK-OK
check "a killed holder's lock is taken over ($OUT)" $?
rm -f "$LOCK" "$HELD"


# ---------------------------------------------------------------------------
# 14. THE HEATMAP'S COLUMNS ARE DAY-ALIGNED.
#
#     The grid is one column per week and one row per weekday, Sun-first, and
#     the dow=0 cell of column wk is (weeks-1-wk)*7 + endDow days back. Every
#     other offset - a Monday-based (back + todayDow)/7 included - shifts whole
#     columns against each other and the grid reads as diagonal streaks. A
#     source grep cannot see that, so this drives the real bar: a store is
#     seeded with usage on ONE day, the board is opened, and the pointer is
#     swept across it. The heat card for a day WITH usage carries one more line
#     than the "no usage" card, so exactly one cell stands out, and the column
#     and row it lands in must be the pair the Sun-first grid predicts.
#
#     The day is 8 back rather than 0: today's cell sits in the LAST column and
#     TODAY's row whatever the offset is, so it cannot tell the two apart.
HEAT_BACK=8
HEAT_STORE="$WORK/heatstore"
mkdir -p "$HEAT_STORE"
HEAT_TS=$(( $(date +%s%3N) - HEAT_BACK * 86400000 ))
printf '{"type":"assistant","usage":{"input":1000,"output":2000,"cacheRead":3000,"cacheWrite":4000},"model":"m","timestamp":%s}\n' \
    "$HEAT_TS" > "$HEAT_STORE/seed.jsonl"
cat > "$WORK/home/.wizbar/config.json" <<JSON
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 0, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" },
  "tokens": { "enabled": true,
              "sources": [ { "app": "pi", "path": "$HEAT_STORE", "recursive": false } ] },
  "modules": { "pet": { "enabled": false } }
}
JSON
kill "$BAR_PID" 2>/dev/null
wait "$BAR_PID" 2>/dev/null
BAR_PID=""
sleep 0.5
# A taller display for this section only: the suite's own is 400 px, which
# clamps the board to the full head height and makes both the content fit and
# the selection's effect on it unobservable. The bus is display-independent, so
# the bar below reuses the suite's.
HEATDPYNUM=$((DPYNUM + 40))
while [ -e "/tmp/.X11-unix/X$HEATDPYNUM" ]; do HEATDPYNUM=$((HEATDPYNUM+1)); done
Xvfb ":$HEATDPYNUM" -screen 0 1600x1000x24 >"$WORK/heatxvfb.log" 2>&1 &
HEATXPID=$!
export DISPLAY=":$HEATDPYNUM"
for _ in $(seq 1 50); do xwininfo -root >/dev/null 2>&1 && break; sleep 0.2; done
xwininfo -root >/dev/null 2>&1 ||
    { echo "FAIL: the heatmap display did not come up"; exit 1; }
HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$WORK/bar-heat.log" 2>&1 &
BAR_PID=$!
sleep 3
HEATWIN=""
for _ in $(seq 1 60); do
    HEATWIN=$(xdotool search --class chocobar 2>/dev/null | head -1)
    [ -n "${HEATWIN:-}" ] && break
    sleep 0.2
done
[ -n "${HEATWIN:-}" ] || { echo "FAIL: the heatmap bar never opened a window"; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$HEATWIN")"
# left-click along the strip until the dashboard opens: the tokens chip is the
# leftmost left-group chip, and a bar that has not scanned yet shows nothing
for xo in 10 20 30 40 50 60 70 80 90 100 110 120; do
    xdotool mousemove $((X + xo)) $((Y + HEIGHT / 2))
    sleep 0.1
    xdotool click 1
    sleep 0.7
    HEATDASH=$(xwininfo -root -children 2>/dev/null |
               awk '/Token usage/ { print $1; exit }')
    [ -n "${HEATDASH:-}" ] && break
done
if [ -z "${HEATDASH:-}" ]; then
    check "the heatmap's columns are day-aligned (the dashboard never opened)" 1
else
    # The token board is a popup too: no titlebar, no dock entry (p_ui.c's owned
    # WS_POPUP parity). xwininfo states it on a real mapped window, so this is
    # the observable, not a source match.
    xwininfo -id "$HEATDASH" | grep -q "Override Redirect State: yes"
    check "the token dashboard is an override-redirect popup (no titlebar, no dock entry)" $?
    eval "$(xdotool getwindowgeometry --shell "$HEATDASH")"
    # The bar's event loop is select-driven: a motion is handled some time
    # AFTER xdotool's warp lands, and no fixed sleep bounds that, so every
    # observation below polls the tip window until its state reflects the
    # position being sampled. The pointer always enters a sample from a KNOWN
    # mapped state (the anchor cell, or the previous sample's own card), which
    # makes both outcomes decisive: a geometry other than the pre-move one can
    # only be the sample's own card, and the tip disappearing can only be the
    # sample's own motion hiding it. A stale card from an earlier position can
    # therefore never be recorded, at any pointer latency.
    TIP_TW=""; TIP_ST=0; TIP_GX=0; TIP_GY=0; TIP_W=0; TIP_H=0
    HOPX=0; HOPY=0; ANCHOR_G="none"; LASTX=0; LASTY=0
    tip_read() { # one X round trip -> TIP_ST plus the tip's placement and size
        local out line
        if [ -z "$TIP_TW" ]; then
            TIP_TW=$(xwininfo -root -tree 2>/dev/null |
                     awk '/chocobar-tip/ { print $1; exit }')
            [ -z "$TIP_TW" ] && { TIP_ST=0; return 0; }
        fi
        out=$(xwininfo -id "$TIP_TW" 2>/dev/null) ||
            { TIP_ST=0; TIP_TW=""; return 0; }
        TIP_ST=0; TIP_GX=0; TIP_GY=0; TIP_W=0; TIP_H=0
        while IFS= read -r line; do
            case "$line" in
                *"Map State: IsViewable"*) TIP_ST=1 ;;
                *"Absolute upper-left X:"*) TIP_GX=$(( ${line##*: } )) ;;
                *"Absolute upper-left Y:"*) TIP_GY=$(( ${line##*: } )) ;;
                *"Width:"*) TIP_W=$(( ${line##*: } )) ;;
                *"Height:"*) TIP_H=$(( ${line##*: } )) ;;
            esac
        done <<<"$out"
        return 0
    }
    # Put the tip back on the anchor cell - the one cell the search found - so
    # the next sample always starts from a known mapped state. Already showing
    # the anchor's geometry is the common case and costs nothing.
    tip_anchor() { # 0 anchored, 1 the bar stopped answering
        local pre="$TIP_GX $TIP_GY $TIP_W $TIP_H" preSt=$TIP_ST
        local dl g
        if [ "$preSt" = 1 ] && [ "$pre" = "$ANCHOR_G" ]; then return 0; fi
        xdotool mousemove "$HOPX" "$HOPY" 2>/dev/null
        dl=$(( $(date +%s%N) + 3000000000 ))
        while :; do
            tip_read
            if [ "$TIP_ST" = 1 ]; then
                g="$TIP_GX $TIP_GY $TIP_W $TIP_H"
                if [ "$preSt" = 0 ] || [ "$g" != "$pre" ]; then
                    ANCHOR_G="$g"
                    LASTX=$HOPX; LASTY=$HOPY
                    return 0
                fi
            fi
            [ "$(date +%s%N)" -ge "$dl" ] && return 1
        done
    }
    # 0 = the heat card for THIS screen position is showing, 1 = this
    # position's own motion proved there is none, 2 = the bar stopped
    # answering. Only the caller records anything, and it records the SAMPLE's
    # position with the card's size, so a stale card can never be misfiled.
    tip_at() { # tip_at <x> <y>
        local x=$1 y=$2 pre dl
        if [ "$TIP_ST" = 0 ] || { [ "$x" = "$LASTX" ] && [ "$y" = "$LASTY" ]; }; then
            tip_anchor || return 2
        fi
        if [ "$x" = "$HOPX" ] && [ "$y" = "$HOPY" ]; then return 0; fi
        pre="$TIP_GX $TIP_GY $TIP_W $TIP_H"
        xdotool mousemove "$x" "$y" 2>/dev/null
        dl=$(( $(date +%s%N) + 3000000000 ))
        while :; do
            tip_read
            if [ "$TIP_ST" = 1 ]; then
                if [ "$TIP_GX $TIP_GY $TIP_W $TIP_H" != "$pre" ]; then
                    LASTX=$x; LASTY=$y
                    return 0
                fi
            else
                return 1
            fi
            [ "$(date +%s%N)" -ge "$dl" ] && return 2
        done
    }
    # Settle on the board's empty top-left corner: the click loop ended on the
    # bar, where the chip tooltip may still be mapped, and the corner is
    # padding - it can never show a card - so once the pointer rests there the
    # tip is hidden and stays hidden.
    SETTLED=0
    xdotool mousemove $((X + 2)) $((Y + 2)) 2>/dev/null
    for _ in $(seq 1 60); do
        tip_read
        [ "$TIP_ST" = 0 ] && { SETTLED=1; break; }
        sleep 0.05
    done
    HITX=0; HITY=0
    if [ "$SETTLED" = 1 ]; then
        # Search for the grid: walk DOWN three parallel columns until a card
        # appears. A gap between cells is 3 px wide and the pitch is at least
        # 12 px (the 9 px cell floor plus the gap), so two probes 8 px apart
        # can never both land in a gap. From the settled hidden state any map
        # that appears belongs to the position just sampled - absent positions
        # never map - so this hit is the anchor cell. A probe that finds
        # nothing within 100 ms parks the pointer on the corner until the tip
        # has been hidden for three straight polls and tries the SAME position
        # once more: a card that arrives late is that position's own card,
        # never the next probe's.
        DASHMX=$((X + WIDTH / 2))
        for gy in $(seq "$Y" 8 $((Y + HEIGHT))); do
            for dx in 0 8 -8; do
                px=$((DASHMX + dx))
                for try in 1 2; do
                    xdotool mousemove "$px" "$gy" 2>/dev/null
                    dl=$(( $(date +%s%N) + 100000000 ))
                    while :; do
                        tip_read
                        [ "$TIP_ST" = 1 ] && break
                        [ "$(date +%s%N)" -ge "$dl" ] && break
                    done
                    [ "$TIP_ST" = 1 ] && { HITX=$px; HITY=$gy; break; }
                    xdotool mousemove $((X + 2)) $((Y + 2)) 2>/dev/null
                    dl=$(( $(date +%s%N) + 2000000000 ))
                    calm=0
                    while :; do
                        tip_read
                        if [ "$TIP_ST" = 0 ]; then
                            calm=$((calm + 1))
                            [ "$calm" -ge 3 ] && break
                        else
                            calm=0
                        fi
                        [ "$(date +%s%N)" -ge "$dl" ] && { SETTLED=0; break; }
                    done
                    [ "$SETTLED" = 0 ] && break
                done
                [ "$HITY" != 0 ] && break
                [ "$SETTLED" = 0 ] && break
            done
            [ "$HITY" != 0 ] && break
            [ "$SETTLED" = 0 ] && break
        done
    fi
    if [ "$SETTLED" = 0 ]; then
        check "the heatmap's columns are day-aligned (the bar stopped answering the pointer)" 1
    elif [ "$HITY" = 0 ]; then
        check "the heatmap's columns are day-aligned (no card ever appeared)" 1
    else
        HOPX=$HITX; HOPY=$HITY
        ANCHOR_G="$TIP_GX $TIP_GY $TIP_W $TIP_H"
        # The grid's own geometry, observed along the two lines that cross it:
        # the row through the anchor cell finds the columns, the column
        # through it finds the rows. A 1 px step crosses every 3 px gap, so
        # each cell shows up as one unbroken run of samples.
        : > "$WORK/heatrow.txt"
        for gx in $(seq "$X" 1 $((X + WIDTH))); do
            tip_at "$gx" "$HOPY"; RC=$?
            [ "$RC" -eq 2 ] && { echo "FAIL: the bar stopped answering the pointer at $gx,$HOPY"; exit 1; }
            [ "$RC" -eq 0 ] && echo "$gx" >> "$WORK/heatrow.txt"
        done
        : > "$WORK/heatcol.txt"
        for gy in $(seq "$Y" 1 $((Y + HEIGHT))); do
            tip_at "$HOPX" "$gy"; RC=$?
            [ "$RC" -eq 2 ] && { echo "FAIL: the bar stopped answering the pointer at $HOPX,$gy"; exit 1; }
            [ "$RC" -eq 0 ] && echo "$gy" >> "$WORK/heatcol.txt"
        done
        # Runs of recorded samples, split where the jump between them exceeds
        # the 3 px gap between cells; each run is one cell's extent.
        bands() {
            awk '{ x[NR] = $1 + 0 }
                 END {
                     if (NR == 0) exit
                     for (a = 1; a <= NR; a++) for (b = a + 1; b <= NR; b++)
                         if (x[b] < x[a]) { t = x[a]; x[a] = x[b]; x[b] = t }
                     s = x[1]; last = x[1]
                     for (a = 2; a <= NR; a++) {
                         if (x[a] - last > 2) { print s, last; s = x[a] }
                         last = x[a]
                     }
                     print s, last
                 }' "$1"
        }
        # 0-based index of the run holding $1 on stdin.
        band_index() {
            awk -v v="$1" '
                { s[NR] = $1; e[NR] = $2 }
                END {
                    for (a = 1; a <= NR; a++)
                        if (v >= s[a] && v <= e[a]) { print a - 1; exit }
                    print -1
                }'
        }
        # The tall card, found by probing the centre of every observed cell.
        # A run's centre always lies inside its cell (the runs come from
        # samples inside the cell, and the layout's 9 px floor keeps a cell
        # wider than the 8 px search step is long), and only the last column
        # can hold a future (unhittable) cell - it sits to the RIGHT of every
        # seeded day, so the indices still match.
        COLS=$(bands "$WORK/heatrow.txt" | awk '{ print int(($1 + $2) / 2) }')
        ROWS=$(bands "$WORK/heatcol.txt" | awk '{ print int(($1 + $2) / 2) }')
        : > "$WORK/heattips.txt"
        for cx in $COLS; do
            for cy in $ROWS; do
                tip_at "$cx" "$cy"; RC=$?
                [ "$RC" -eq 2 ] && { echo "FAIL: the bar stopped answering the pointer at $cx,$cy"; exit 1; }
                [ "$RC" -eq 0 ] && echo "$cx $cy ${TIP_W}x${TIP_H}" >> "$WORK/heattips.txt"
            done
        done
        [ -s "$WORK/heattips.txt" ] || { echo "FAIL: no cell showed a card"; exit 1; }
        read -r TX TY TSH <<<"$(awk '
            { split($3, p, "x"); if (p[2] + 0 > best) { best = p[2] + 0; bx = $1; by = $2; bs = $3 } }
            END { printf "%d %d %s", bx, by, bs }' "$WORK/heattips.txt")"
        HEAT_COL=$(bands "$WORK/heatrow.txt" | band_index "$TX")
        HEAT_ROW=$(bands "$WORK/heatcol.txt" | band_index "$TY")
        # the expected pair, from today's weekday and the Sun-first formula
        read -r WANT_COL WANT_ROW <<<"$(awk -v back="$HEAT_BACK" -v weeks=26 '
            BEGIN {
                "date +%u" | getline u
                endDow = u % 7          # 0 = Sun, as SYSTEMTIME.wDayOfWeek
                for (wk = 0; wk < weeks; wk++)
                    for (dow = 0; dow < 7; dow++)
                        if ((weeks - 1 - wk) * 7 + endDow - dow == back)
                            printf "%d %d\n", wk, dow
            }')"
        [ "${HEAT_COL:-x}" = "$WANT_COL" ] && [ "${HEAT_ROW:-y}" = "$WANT_ROW" ]
        check "the seeded day's cell is the Sun-first grid's column $WANT_COL row $WANT_ROW (got column ${HEAT_COL:-?} row ${HEAT_ROW:-?}, card ${TSH:-?})" $?
        # and picking that day must not move the grid. The bug let the
        # section's height run past the viewport, which dropped the heatmap
        # out of it entirely: the cells that were under the pointer a moment
        # ago were gone, while their hit rects kept firing from where they
        # used to be. Same position, same card, same day.
        tip_at "$TX" "$TY"; RC=$?
        [ "$RC" -eq 2 ] && { echo "FAIL: the bar stopped answering the pointer at $TX,$TY"; exit 1; }
        xdotool click 1
        sleep 0.8
        SEL_CARD="none"
        tip_at "$TX" "$TY"; RC=$?
        [ "$RC" -eq 2 ] && { echo "FAIL: the bar stopped answering the pointer at $TX,$TY"; exit 1; }
        [ "$RC" -eq 0 ] && SEL_CARD="${TIP_W}x${TIP_H}"
        [ "$SEL_CARD" = "$TSH" ]
        check "picking a day leaves the heatmap's cells where they were (card $SEL_CARD, was $TSH)" $?
    fi
fi

echo
echo "--- behaviour: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
