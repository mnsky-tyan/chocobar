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

cleanup() {
    [ -n "${BAR_PID:-}" ] && kill "$BAR_PID" 2>/dev/null
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

# ---- the throwaway environment -------------------------------------------
mkdir -p "$WORK/home/.wizbar" "$WORK/fakebin"
cat > "$WORK/home/.wizbar/config.json" <<'JSON'
{
  "bar": { "height": 30, "gap": 12, "fontSize": 10, "fontFamily": "sans",
           "align": 1, "backgroundTint": "#E8D8C3", "backgroundAlpha": 120,
           "backdrop": "acrylic", "radius": 8 },
  "theme": { "foreground": "#080808", "foregroundDim": "#5a5245" }
}
JSON
# a systemctl that always fails, so the autostart path is exercised honestly
cat > "$WORK/fakebin/systemctl" <<'SH'
#!/bin/sh
echo "Failed to get unit file state for chocobar.service: No such file or directory" >&2
exit 4
SH
chmod +x "$WORK/fakebin/systemctl"

gcc -O1 -o "$XCLICK" "$HERE/xclick.c" -lX11 -lXtst 2>"$WORK/cc.log" \
    || { echo "FAIL: cannot build the XTEST driver"; cat "$WORK/cc.log"; exit 1; }

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

HOME="$WORK/home" PATH="$WORK/fakebin:$PATH" CHOCOBAR_DEBUG=1 \
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
# the menu's row geometry, in logical px (linux/src/menu.c: MENU_PAD, MENU_ROW,
# MENU_SEP). The second row is "Subscription dashboard", whose choice is
# observable in the bar's own log without changing anything on disk; its centre
# is MENU_PAD + MENU_ROW + MENU_ROW/2 from the menu's top edge.
ROW2_Y=$(awk -v s="$SCALE" 'BEGIN{printf "%.0f", (6+30+15)*s}')

# 1. the bar must not ask for a compositor blur
xprop -id "$BARWIN" _KDE_NET_WM_BLUR_BEHIND_REGION >"$WORK/prop.txt" 2>&1
! grep -qE '[0-9]+, *[0-9]+, *[0-9]+, *[0-9]+' "$WORK/prop.txt"
check "the bar asks the compositor for NO blur" $?

# 2. a press alone opens nothing (Windows opens on WM_RBUTTONUP)
"$XCLICK" $((X+40)) $((Y+10)) 3 press
sleep 0.6
! menu_open
check "a right-PRESS opens nothing" $?

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
menu_wh() {
    for w in $(xwininfo -root -children | grep -o '^     0x[0-9a-f]*'); do
        [ "$((w))" -eq "$BARWIN" ] && continue
        xwininfo -id "$w" | awk '/Width:/{W=$2} /Height:/{H=$2} END{if (W>1 && H>1) printf "%d %d", W, H}'
    done
}
read -r MENU_W_DEV MENU_H_DEV <<EOF
$(menu_wh)
EOF
MENU_W_DEV=${MENU_W_DEV:-0}; MENU_H_DEV=${MENU_H_DEV:-0}
WANT_MENU_H=$(awk -v w="$MENU_W_DEV" 'BEGIN{printf "%.0f", w*279/236}')
[ "$MENU_W_DEV" -gt 0 ] && [ "$MENU_H_DEV" -ge $((WANT_MENU_H-2)) ] && [ "$MENU_H_DEV" -le $((WANT_MENU_H+2)) ]
check "the menu renders the Windows item set (${MENU_W_DEV}x${MENU_H_DEV}, want h~$WANT_MENU_H)" $?

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

# 5c. a right-press whose release lands OFF the bar opens nothing: the press
#     grabs the pointer, so the release arrives at the bar anyway, and Windows
#     sends WM_RBUTTONUP only when the release is over the window.
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

echo
echo "--- behaviour: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
