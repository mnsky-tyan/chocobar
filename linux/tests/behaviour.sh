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
#     user starts from its own directory with a relative path, and the children
#     it leaves behind are reaped, not zombies
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

BIN="$ROOT/linux/build/chocobar-linux"
if [ ! -x "$BIN" ]; then
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
    rm -f /tmp/chocobar-events.log
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0; FAIL=0
check() { # check <name> <condition-result>
    if [ "$2" -eq 0 ]; then echo "PASS: $1"; PASS=$((PASS+1))
    else echo "FAIL: $1"; FAIL=$((FAIL+1)); fi
}

# ---- the throwaway environment -------------------------------------------
mkdir -p "$WORK/home/.wizbar" "$WORK/fakebin" "$WORK/decoy"
# the pet module is FIRST (the leftmost left-group chip) and the tokens chip is
# disabled, so every left click on the bar either hits the pet chip or is inert
# - the pet chip can be FOUND by clicking x offsets and watching for the bar's
# own "pet: started" log line
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
# a systemctl that always fails, so the autostart path is exercised honestly,
# and logs one line per call so the test can count reads. The stateful twin
# (systemctl-good) answers is-enabled from a state file and records
# enable/disable; the checks swap the two by renaming so the bar's own PATH
# resolves whichever mode a check needs.
cat > "$WORK/fakebin/systemctl" <<SH
#!/bin/sh
echo "call: \$*" >> "$CALLS"
echo "Failed to get unit file state for chocobar.service: No such file or directory" >&2
exit 4
SH
chmod +x "$WORK/fakebin/systemctl"
cat > "$WORK/fakebin/systemctl-good" <<SH
#!/bin/sh
echo "call: \$*" >> "$CALLS"
case "\$2" in
  is-enabled) [ -f "$WORK/autostart.enabled" ] && exit 0 || exit 1 ;;
  enable) : > "$WORK/autostart.enabled"; exit 0 ;;
  disable) rm -f "$WORK/autostart.enabled"; exit 0 ;;
  *) exit 4 ;;
esac
SH
chmod +x "$WORK/fakebin/systemctl-good"

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
#     x offsets and watching for the bar's own "pet: started" log line.
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
    if grep -q "pet: started" "$WORK/bar.log"; then PET_X=$xoff; break; fi
done
[ "$PET_X" -gt 0 ]
check "a click on the pet chip starts the pet (chip found at x offset $PET_X)" $?
# the started pet must be the configured script and be alive: it recorded its
# pid (the file exists) and its state is "running"
[ -s "$WORK/pet.pid" ] && kill -0 "$(cat "$WORK/pet.pid")" 2>/dev/null &&
    [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "running" ]
check "the started pet is alive and recorded itself" $?

# 6d. click the pet chip again: the chip's state source (the matcher over
#     /proc/<pid>/cmdline) saw the started pet, so the toggle must STOP it -
#     one start, one stop, and the pet's own record shows a SIGTERM (a SIGKILL
#     would leave the pid file in place with the state "running"; a blind
#     duplicate start would log a second "pet: started" line)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
grep -q "pet: stopped" "$WORK/bar.log" && [ "$(grep -c "pet: started" "$WORK/bar.log")" -eq 1 ] &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
check "the second click stops the pet with a SIGTERM, not a duplicate, not a SIGKILL" $?

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
STARTED_BEFORE=$(grep -c "pet: started" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
[ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq "$STOPPED_BEFORE" ] &&
    [ "$(grep -c "pet: started" "$WORK/bar.log" || true)" -eq $((STARTED_BEFORE+1)) ] &&
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
STARTED_BEFORE=$(grep -c "pet: started" "$WORK/bar.log" || true)
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
    [ "$(grep -c "pet: started" "$WORK/bar.log" || true)" -eq "$STARTED_BEFORE" ] &&
    [ ! -e "$WORK/pet.pid" ] && [ "$(cat "$WORK/pet.state" 2>/dev/null)" = "SIGTERM" ]
check "the chip sees a relative-path pet: the click stops it, no duplicate start" $?
[ -n "$USER_PID" ] && kill -0 "$USER_PID" 2>/dev/null && kill "$USER_PID" 2>/dev/null
# the click above already stopped it, so the zombie check below starts stopped

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
STARTED_BEFORE=$(grep -c "pet: started" "$WORK/bar.log" || true)
STOPPED_BEFORE=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 2
[ "$(grep -c "pet: started" "$WORK/bar.log" || true)" -eq $((STARTED_BEFORE+1)) ] &&
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq $((STOPPED_BEFORE+1)) ] &&
    [ "$(zombies)" -eq 0 ]
check "a pet start/stop cycle leaves no zombie child of the bar" $?

# 6h. a DIRECTORY as the pet path is refused: access(X_OK) on a directory
#     tests search permission, so a directory used to pass as "the pet" and
#     every process carrying it as an argument became stopable. The bar
#     hot-reloads its config (the 2 s mtime check), so swap in the throwaway
#     HOME (a 0755 directory) and click the chip: nothing may start, nothing
#     may stop, and the decoy must survive.
cat > "$WORK/home/.wizbar/config.json" <<JSON
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
sleep 3
grep -q "pet path is not an absolute path to a regular executable file" "$WORK/bar.log"
REFUSED=$?
STARTED_N=$(grep -c "pet: started" "$WORK/bar.log" || true)
STOPPED_N=$(grep -c "pet: stopped" "$WORK/bar.log" || true)
"$XCLICK" $((X+PET_X)) $((Y+10)) 1 full
sleep 1.2
[ "$REFUSED" -eq 0 ] &&
    [ "$(grep -c "pet: started" "$WORK/bar.log" || true)" -eq "$STARTED_N" ] &&
    [ "$(grep -c "pet: stopped" "$WORK/bar.log" || true)" -eq "$STOPPED_N" ] &&
    kill -0 "$DECOY_PID" 2>/dev/null
check "a directory pet path is refused and the chip is inert (decoy survives)" $?
kill "$DECOY_PID" 2>/dev/null
DECOY_PID=""

# 7. the bar rides above the terminal: a geometry report moves it.
#    The move happens on the bar's own tick (at most once a second), so the
#    waits below are generous on purpose - a flaky sleep must not look like a
#    broken bar.
report() { gdbus call --timeout 5 --session --dest org.chocobar.Geom --object-path /ChocobarGeom \
              --method org.chocobar.Geom.Report "$@" >/dev/null 2>&1; }
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
kill "$BAR_PID" 2>/dev/null
wait "$BAR_PID" 2>/dev/null
BAR_PID=""
for _ in $(seq 1 50); do
    report kitty 900 100 300 1600 && sleep 0.05 || break
done
for _ in $(seq 1 50); do
    [ -z "$(xdotool search --class chocobar 2>/dev/null)" ] && break
    sleep 0.05
done
HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" \
    "$BIN" >"$WORK/bar2.log" 2>&1 &
BAR_PID=$!
for _ in $(seq 1 30); do
    report kitty 900 100 300 1600 && break
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
#     The reports here go through dbus-send, NOT gdbus: gdbus introspects its
#     destination before calling and the bar never answers Introspect (its bus
#     loop replies to Report alone), so a gdbus call stalls for seconds and
#     lands only after the bar's 700 ms startup geometry wait - too late to be
#     the report the bar is BORN with. dbus-send delivers the method call
#     directly and the bar's 10 ms startup pump answers it inside the wait.
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
report_fast() { dbus-send --session --dest=org.chocobar.Geom --type=method_call \
    --print-reply /ChocobarGeom org.chocobar.Geom.Report \
    "string:$1" "int32:$2" "int32:$3" "int32:$4" "int32:$5" >/dev/null 2>&1; }
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

echo
echo "--- behaviour: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
