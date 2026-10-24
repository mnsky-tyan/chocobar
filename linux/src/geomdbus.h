// geomdbus.h - the bar's side of the KWin geometry REPORT.
//
// The KWin helper (linux/src/geomwatch.js) cannot write files inside its sandbox,
// so it calls the bar over D-Bus. This is the listener. It uses libdbus-1: the
// shared library is already loaded by kwin_wayland on any KDE desktop, and a
// hand-rolled wire decoder was tried first and abandoned (three alignment bugs,
// each of which the daemon punished with a silent disconnect).
//
// A METHOD CALL, not a signal: KWin's callDBus has no way to emit a signal at an
// arbitrary service, so the helper calls org.chocobar.Geom.Report and the bar
// answers. That is why this is a listener with a match rule, not a subscriber.
#ifndef GEOMDBUS_H
#define GEOMDBUS_H

typedef struct {
    int valid;         // 1 = a usable report
    char cls[64];      // the terminal's resource class
    int width;         // logical px
    int x, y;          // logical px
    int screenW;       // logical px
} GeomReport;

// The bus state. fd is not a file descriptor: libdbus's connection is opaque and
// main.c only needs a boolean, so it is -1 (down) or >= 0 (up). The error reason
// lives in the implementation's own static, read back through geomBusLastError.
typedef struct {
    int fd;
} GeomBus;

// Connect to the session bus, own org.chocobar.Geom and add the match rule that
// delivers Report calls. Returns 1 on success. On failure the reason is in
// geomBusLastError() (never fatal: the bar simply centres itself).
int geomBusConnect(GeomBus *bus);

// TRUE when this process owns org.chocobar.Geom, i.e. it is the ONE bar the KWin
// helper talks to. A second bar must then never install, reload or unload that
// helper: two bars managing one compositor-side script is how a stacked poller
// froze the whole desktop (2026-10-24).
int geomBusOwnsName(void);

// Read pending messages. Returns the number of reports decoded into *out (the
// last one wins), or -1 if the connection is gone - in which case it has already
// been released and the caller must call geomBusConnect again.
int geomBusPoll(GeomBus *bus, GeomReport *out);

void geomBusClose(GeomBus *bus);

// The last failure reason, for the log line when geometry is unavailable.
const char *geomBusLastError(void);

// The KWin script's load state, read over the bus: 1 loaded, 0 NOT loaded,
// -1 the question could not be asked. A 0 is the cue for geomBusReloadScript.
int geomBusScriptLoaded(const char *pluginName);

// Re-load the KWin helper over the bus - what --install-geometry does, but
// callable from inside the bar when the script has been unloaded out from
// under it (a KWin restart, or anything else calling unloadScript).
//
// It UNLOADS first and then loads, so exactly one instance exists afterwards.
// That is not tidiness: KWin stacks a second live copy of a script when
// loadScript is called on a name that is already loaded, and this script polls
// the compositor's main thread every 250 ms - two copies froze the whole
// desktop for a minute on 2026-10-24. Returns 1 on success, 0 on failure.
int geomBusReloadScript(const char *scriptPath, const char *pluginName);

// Unload one instance of the helper by plugin name (--remove-geometry).
// Returns 1 when KWin answered, 0 otherwise.
int geomBusUnloadScript(const char *pluginName);

#endif
