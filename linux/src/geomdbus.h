// geomdbus.h - the bar's side of the KWin geometry signal.
//
// The KWin helper (linux/src/geomwatch.js) cannot write files inside its sandbox,
// so it calls the bar over D-Bus. This is the listener. It uses libdbus-1: the
// shared library is already loaded by kwin_wayland on any KDE desktop, and a
// hand-rolled wire decoder was tried first and abandoned (three alignment bugs,
// each of which the daemon punished with a silent disconnect).
#ifndef GEOMDBUS_H
#define GEOMDBUS_H

typedef struct {
    int valid;         // 1 = a usable report
    char cls[64];      // the terminal's resource class
    int width;         // logical px
    int x, y;          // logical px
    int screenW;       // logical px
} GeomReport;

typedef struct {
    int fd;            // unused; kept so callers can test "is it up" uniformly
    char err[160];
} GeomBus;

// Connect to the session bus, send Hello and subscribe to the geometry signal.
// Returns 1 on success. On failure bus->err says why (never fatal: the bar simply
// centres itself).
int geomBusConnect(GeomBus *bus);

// Read pending messages. Returns the number of reports decoded into *out (the
// last one wins). A broken connection closes itself; call geomBusConnect again.
int geomBusPoll(GeomBus *bus, GeomReport *out);

void geomBusClose(GeomBus *bus);

// The last failure reason, for the log line when geometry is unavailable.
const char *geomBusLastError(void);

#endif
