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
#   - the bar must NOT ask the compositor for a blur (it flattens the strip)
#   - the bar must ride above the terminal, and hide when there is no room
#   - every map of the bar - the startup one and the tick's after a born-hidden
#     start - must make the EWMH all-desktops request, or that bar lives on one
#     desktop with DEMANDS_ATTENTION set
#   - the pet chip starts and stops the configured process, by the process's
#     EXECUTED file - never a process name, and never a process that merely
#     carries the path as an argument - including an env-shebang script the
#     user starts from its own directory with a relative path, a version-pinned
#     interpreter, and a shebang that carries an argument (the script sits at
#     argv[2] there); and the children it leaves behind are reaped, not
#     zombies; a start is logged only once the kernel took the exec, and a
#     refused one logs "cannot start" and claims no start at all
#   - the click that starts the pet never waits on the child's exec: a slow
#     exec leaves the bar ticking, answering a right-click and following a
#     fresh terminal report while the handshake pends, and the outcome still
#     lands as the one true line
#   - the parked handshake is BOUNDED: a click refused while it pends is logged
#     rather than swallowed, and a start that never reports is abandoned and
#     logged, leaving the bar free to try again
#   - the bar rides above the terminal at the height it is actually DRAWN with,
#     so a tiny configured height cannot overlap the terminal, and hides when
#     there is no room
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
command -v xdotool   >/dev/null || skip "xdotool is not installed"
command -v gdbus     >/dev/null || skip "gdbus (libglib2.0-bin) is not installed"
command -v dbus-send >/dev/null || skip "dbus-send is not installed"
command -v gcc      >/dev/null || skip "gcc is not installed"

# The bar itself has to be BUILT to be driven, and building needs the X11/cairo
# DEVELOPMENT headers, not just the runtime tools above. A developer machine
# without them used to fail `npm test` outright, which contradicts the promise
# that a missing prerequisite skips cleanly here and fails in CI. A machine that
# HAS the headers and still fails to build keeps the hard FAIL below.
buildable() {
    command -v pkg-config >/dev/null || return 1
    for p in x11 xrender xfixes xcursor cairo dbus-1 fontconfig libcurl; do
        pkg-config --exists "$p" || return 1
    done
    return 0
}

BIN="$ROOT/linux/build/chocobar-linux"
if [ ! -x "$BIN" ]; then
    buildable || skip "the X11/cairo development headers (pkg-config) are not installed"
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
    [ -n "${MON_PID:-}" ] && kill "$MON_PID" 2>/dev/null
    if [ -s "$WORK/pet.pid" ]; then kill "$(cat "$WORK/pet.pid")" 2>/dev/null; fi
    [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null
    [ -n "${DPID:-}" ] && kill "$DPID" 2>/dev/null
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
calls_n() { if [ -f "$CALLS" ]; then wc -l < "$CALLS"; else echo 0; fi; }

# ---- the private display and bus -----------------------------------------
DPYNUM=$(( 90 + RANDOM % 8 ))
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

# the ClientMessage watcher: the all-desktops request is an X message to the
# window manager, so this is the only trace of it on a bare Xvfb
: > "$CLIENTMSG"
"$XMON" "$CLIENTMSG" &
MON_PID=$!
sleep 0.3
kill -0 "$MON_PID" 2>/dev/null || { echo "FAIL: the ClientMessage monitor did not start"; exit 1; }
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
# the report and the config are in LOGICAL pixels and the bar scales them. The
# scale is read off the bar's own height (height = bar.height logical px *
# scale), so every expected pixel value below is derived, not assumed.
SCALE=$(awk -v h="$HEIGHT" 'BEGIN{printf "%.4f", h/30.0}')

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
# the menu's row geometry, in logical px (linux/src/menu.c: MENU_PAD, MENU_ROW,
# MENU_SEP). The second row is "Subscription dashboard", whose choice is
# observable in the bar's own log without changing anything on disk; its centre
# is MENU_PAD + MENU_ROW + MENU_ROW/2 from the menu's top edge.
ROW2_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (6+30+15)*s}')
# the "Start on login" row centre, in logical px from the menu's top edge
# (menu.c: MENU_PAD, two rows, MENU_SEP, then half of this row)
ROW_AUTO_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (6+2*30+9+15)*s}')

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
#     separators (p_ui.c kMenuItems) inside MENU_PAD, i.e. 279 logical px tall
#     for a 236 logical px wide menu. The ratio is scale-free, so it needs no
#     DPI assumption, and a missing or extra item changes the height. (No click
#     can observe this parity: the last row quits the bar.)
read -r MENU_WIN MENU_W_DEV MENU_H_DEV <<EOF
$(menu_wh)
EOF
MENU_WIN=${MENU_WIN:-0}; MENU_W_DEV=${MENU_W_DEV:-0}; MENU_H_DEV=${MENU_H_DEV:-0}
WANT_MENU_H=$(awk -v w="$MENU_W_DEV" 'BEGIN{printf "%.0f", w*279/236}')
[ "$MENU_W_DEV" -gt 0 ] && [ "$MENU_H_DEV" -ge $((WANT_MENU_H-2)) ] && [ "$MENU_H_DEV" -le $((WANT_MENU_H+2)) ]
check "the menu renders the Windows item set (${MENU_W_DEV}x${MENU_H_DEV}, want h~$WANT_MENU_H)" $?

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
! menu_open && ! grep -q "subscription board is not ported" "$WORK/bar.log" &&
    xwininfo -id "$BARWIN" | grep -q "Map State: IsViewable"
check "a click OUTSIDE the menu picks no row and leaves the bar running" $?

# 5e. a click INSIDE a row picks that row: the bar acts on the choice it logs
"$XCLICK" $((X+40)) $((Y+10)) 3 full
sleep 0.6
"$XCLICK" $((X+40+40)) $((Y+10+ROW2_Y)) 1 full
sleep 0.8
! menu_open && grep -q "subscription board is not ported" "$WORK/bar.log"
check "a click INSIDE a menu row picks that row" $?

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
ROOT_BEFORE=$(root_children)
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
    [ "$(root_children)" -eq "$ROOT_BEFORE" ]
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

# 7e. the autostart row acts through the stateful fake: is-enabled answers
#     disabled, enable records its call and succeeds. The fake is swapped in
#     by renaming so the bar's own PATH resolves it; the failing fake stays
#     for the unknown-state checks on either side of this pair.
mv "$WORK/fakebin/systemctl" "$WORK/fakebin/systemctl-fail"
mv "$WORK/fakebin/systemctl-good" "$WORK/fakebin/systemctl"
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
ROW_UPDATES_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (6+5*30+3*9+15)*s}')
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

echo
echo "--- behaviour: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
