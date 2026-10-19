// linux/src/geomwatch.js - the KWin side of "as long as the terminal".
//
// WHY THIS EXISTS
// A Wayland compositor does not let an application position itself relative to
// another window, and an X11 client cannot see Wayland-native windows at all, so
// the bar cannot discover its terminal's geometry on its own. KWin CAN: its
// scripting API exposes every window's frameGeometry. This script is the bridge -
// it watches the active window and writes that geometry to a small file the bar
// reads on its own tick. Nothing is drawn here and nothing is sent back.
//
// It is installed and started by the bar itself (chocobar-linux --install-geometry)
// and unloaded when that is asked for; KWin loads scripts from
// ~/.local/share/kwin/scripts/<name>/contents/code/main.js, and the bar writes
// this file there.
//
// HOW IT REPORTS
// A KWin script is sandboxed: QFile and QIODevice are undefined inside it
// (measured), so it cannot write a file. callDBus IS available, so the geometry
// leaves as a D-Bus signal on the session bus:
//     interface org.chocobar.Geom, signal Report
//     (s class, i width, i x, i y, i screenWidth)
// in LOGICAL pixels (KWin's coordinate space; the bar scales to device pixels).
// The bar subscribes with its own sd-bus-free listener (linux/src/geomdbus.c).
// No such name needs to be owned by anyone: a signal to a well-known name with
// no owner is simply dropped, so the helper is harmless when the bar is not
// running.
//
// A terminal that is not the active window still counts if it was the last one:
// switching to a browser and back would otherwise make the bar jump to centre
// and back, which reads as a flicker. The last terminal geometry is therefore
// kept and only replaced by a newer terminal's.

// what counts as "the terminal": the classes the project documents for Windows
// (Windows Terminal, conhost, mintty) plus the Linux ones actually in use here.
var TERMINAL = /^(kitty|konsole|alacritty|foot|wezterm|org\.wezfurlong\.wezterm|ghostty|com\.mitchellh\.ghostty|xterm|urxvt|rxvt|st|terminator|tilix|gnome-terminal-server|org\.gnome\.Terminal|xfce4-terminal|lxterminal|qterminal|cool-retro-term|contour|blackbox|warp-terminal)$/i;

var lastLine = "";
var lastPublish = 0;

function describe(w) {
    if (!w) return null;
    // skip our own bar and any other override-redirect/desktop window
    if (w.dock || w.desktopWindow || w.notification || w.onScreenDisplay) return null;
    var g = w.frameGeometry;
    if (!g || !g.width) return null;
    return {
        cls: w.resourceClass || w.resourceName || "",
        w: Math.round(g.width),
        x: Math.round(g.x),
        y: Math.round(g.y)
    };
}

function screenWidth() {
    try {
        var s = workspace.activeScreen;
        if (s && s.geometry) return Math.round(s.geometry.width);
    } catch (e) {}
    return 0;
}

function publish(info) {
    if (!info) return;
    var line = info.cls + "|" + info.w + "|" + info.x + "|" + info.y + "|" + screenWidth();
    if (line === lastLine) return;   // only report changes
    lastLine = line;
    lastPublish = Date.now();
    try {
        callDBus("org.chocobar.Geom", "/ChocobarGeom", "org.chocobar.Geom", "Report",
                 info.cls, info.w, info.x, info.y, screenWidth());
    } catch (e) {}
}

function consider(w) {
    var info = describe(w);
    if (!info) return false;
    if (!TERMINAL.test(info.cls)) return false;
    publish(info);
    return true;
}

// Poll rather than hook: KWin's frameGeometryChanged fires for every drag frame of
// every window, and this only needs to be current within a tick. 250 ms is well
// under the bar's own 1 s repaint and costs nothing measurable in KWin.
var timer = new QTimer();
timer.interval = 250;
timer.timeout.connect(function () {
    var active = workspace.activeWindow;
    if (active && consider(active)) { /* the terminal is focused: use it */ }
    // otherwise keep the last terminal we saw, so alt-tabbing does not move the bar
    // Re-announce every few seconds even when nothing changed: the bar may have
    // restarted and missed the last signal.
    var now = Date.now();
    if (now - lastPublish > 3000 && lastLine) {
        lastPublish = now;
        var parts = lastLine.split("|");
        try {
            callDBus("org.chocobar.Geom", "/ChocobarGeom", "org.chocobar.Geom", "Report",
                     parts[0], parseInt(parts[1]), parseInt(parts[2]), parseInt(parts[3]), parseInt(parts[4]));
        } catch (e) {}
    }
});

// Publish once immediately so the bar has geometry before its first tick.
var a = workspace.activeWindow;
if (!consider(a)) {
    var all = workspace.windowList();
    for (var i = 0; i < all.length && i < 64; i++) {
        if (consider(all[i])) break;
    }
}
timer.start();
