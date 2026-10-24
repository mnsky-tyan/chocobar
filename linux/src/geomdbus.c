// linux/src/geomdbus.c - the bar's listener for the KWin terminal geometry.
//
// WHY libdbus AND NOT A HAND-ROLLED DECODER
// This file first spoke the D-Bus wire format directly, to avoid a build
// dependency. That was the wrong call: the format has several alignment rules
// whose violations make the daemon close the connection with NO error text, and
// three separate byte-level bugs (flags, field-array length, string padding) each
// presented as "connection reset by peer". A bar whose terminal-width feature
// silently stops working is worse than one that links a library every KDE desktop
// already loads at runtime (libdbus-1.so.3 is present on any system running
// KWin, and kwin_wayland itself links it). The dependency is therefore accepted
// deliberately: -ldbus-1 at build time, the shared library at run time.
//
// WHAT IT LISTENS FOR
// The KWin helper (linux/src/geomwatch.js) cannot write files - QFile is undefined
// inside a KWin script's sandbox (measured) - but callDBus works. callDBus emits a
// METHOD CALL, not a signal, so the bar OWNS the name org.chocobar.Geom and
// answers Report(s class, i width, i x, i y, i screenWidth) in LOGICAL pixels.
// Owning the name is also what lets the script's call return instead of blocking.

#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "geomdbus.h"

static DBusConnection *g_conn = NULL;
static char g_err[200];
// 1 while this process owns org.chocobar.Geom. Only that process may touch the
// compositor-side helper (see geomBusOwnsName in the header).
static int g_ownsName = 0;

static void recordError(const char *what) {
    snprintf(g_err, sizeof(g_err), "%s", what ? what : "unknown");
}

// close + unref the private connection, if any. libdbus requires close before
// unref on a private connection, and forgetting it in one path is how a broken
// connection used to be handed back to the next caller.
static void closeConn(void) {
    g_ownsName = 0;
    if (!g_conn) return;
    dbus_connection_close(g_conn);
    dbus_connection_unref(g_conn);
    g_conn = NULL;
}

// TRUE when this process owns org.chocobar.Geom, i.e. it is the ONE bar the KWin
// helper talks to. A second bar (a dev build next to the released one, or a
// service restart overlapping the old process) gets FALSE, and must then never
// install, reload or unload the helper: managing a compositor-side script that
// belongs to another bar's session is how a stacked poller freezes the desktop.
int geomBusOwnsName(void) { return g_ownsName; }

int geomBusConnect(GeomBus *bus) {
    (void)bus;
    DBusError err;
    dbus_error_init(&err);
    closeConn();

    g_conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!g_conn) {
        recordError(err.message ? err.message : "cannot connect to the session bus");
        dbus_error_free(&err);
        return 0;
    }
    // the bar must not keep the bus alive after it exits, and must not be killed
    // by a bus disconnect it can recover from
    dbus_connection_set_exit_on_disconnect(g_conn, FALSE);

    // own the name the KWin script calls
    int rc = dbus_bus_request_name(g_conn, "org.chocobar.Geom",
                                   DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        recordError(err.message);
        dbus_error_free(&err);
        dbus_connection_close(g_conn);
        dbus_connection_unref(g_conn);
        g_conn = NULL;
        return 0;
    }
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
        rc != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        recordError("another process already owns org.chocobar.Geom");
        closeConn();
        g_ownsName = 0;
        return 0;
    }
    g_ownsName = 1;

    // accept the call from any sender: the KWin script runs inside kwin_wayland
    // and its unique name changes with every KWin restart
    const char *rule = "type='method_call',interface='org.chocobar.Geom',member='Report'";
    dbus_bus_add_match(g_conn, rule, &err);
    if (dbus_error_is_set(&err)) {
        // without the match rule NO Report call is ever delivered, so reporting
        // success here would leave the feature silently dead - which is the one
        // outcome this file exists to prevent.
        recordError(err.message);
        dbus_error_free(&err);
        closeConn();
        return 0;
    }
    return 1;
}

// Read and decode Report calls. Returns how many were decoded (the last wins),
// or -1 if the connection died and the caller must call geomBusConnect again.
int geomBusPoll(GeomBus *bus, GeomReport *out) {
    (void)bus;
    if (!g_conn) return 0;
    if (!dbus_connection_get_is_connected(g_conn)) {
        recordError("the session bus disconnected");
        // a dead connection must be RELEASED, not polled forever: geomdbus.h
        // promises this, and without it the bar loses bar.align 1 permanently
        // after any KWin or session-bus restart.
        closeConn();
        return -1;
    }
    // do not block: the bar has its own tick
    dbus_connection_read_write(g_conn, 0);
    int got = 0;
    for (;;) {
        DBusMessage *msg = dbus_connection_pop_message(g_conn);
        if (!msg) break;

        if (dbus_message_is_method_call(msg, "org.chocobar.Geom", "Report")) {
            const char *cls = NULL;
            dbus_int32_t width = 0, x = 0, y = 0, screenW = 0;
            if (dbus_message_get_args(msg, NULL,
                                      DBUS_TYPE_STRING, &cls,
                                      DBUS_TYPE_INT32, &width,
                                      DBUS_TYPE_INT32, &x,
                                      DBUS_TYPE_INT32, &y,
                                      DBUS_TYPE_INT32, &screenW,
                                      DBUS_TYPE_INVALID)) {
                memset(out, 0, sizeof(*out));
                snprintf(out->cls, sizeof(out->cls), "%s", cls ? cls : "");
                out->width = width;
                out->x = x;
                out->y = y;
                out->screenW = screenW;
                out->valid = width > 0;
                if (out->valid) got++;
            }
            // always answer, even a malformed call: the KWin script's callDBus
            // waits for a reply, and a blocked call there would stall its timer
            DBusMessage *reply = dbus_message_new_method_return(msg);
            if (reply) {
                dbus_connection_send(g_conn, reply, NULL);
                dbus_message_unref(reply);
            }
        }
        dbus_message_unref(msg);
    }
    dbus_connection_flush(g_conn);
    return got;
}

void geomBusClose(GeomBus *bus) {
    (void)bus;
    if (!g_conn) return;
    dbus_connection_flush(g_conn);
    closeConn();
}

const char *geomBusLastError(void) { return g_err; }

// Re-load the KWin script, for when it has been unloaded out from under the bar.
//
// Load the KWin helper back after it has been unloaded out from under the bar.
//
// THE UNLOAD FIRST IS LOAD-BEARING, not belt-and-braces. KWin keeps script
// INSTANCES: loadScript on a name that is already loaded adds a SECOND live
// copy of the same file. The helper polls every 250 ms and calls back over
// D-Bus each time it sees a change, so N stacked copies mean N polls and N
// calls per 250 ms inside the COMPOSITOR'S MAIN THREAD. Measured on this
// machine 2026-10-24: a bar restart that raced the self-heal loaded the script
// twice, and KWin logged "The main thread was hanging temporarily!" for over a
// minute while the desktop froze. The bar must therefore be able to promise
// "exactly one instance", which means unloading before loading - and it must
// never call this while an instance it knows about is still loaded.
//
// Returns 1 when the script was loaded (or was already loaded), 0 on failure.
int geomBusReloadScript(const char *scriptPath, const char *pluginName) {
    if (!g_conn || !dbus_connection_get_is_connected(g_conn)) return 0;
    if (!scriptPath || !*scriptPath || !pluginName || !*pluginName) return 0;

    // 1. unload any existing instance. A false reply means "was not loaded",
    //    which is the normal path for the first load of a fresh KWin.
    {
        DBusMessage *u = dbus_message_new_method_call("org.kde.KWin", "/Scripting",
                                                      "org.kde.kwin.Scripting",
                                                      "unloadScript");
        if (u) {
            const char *n = pluginName;
            if (dbus_message_append_args(u, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID)) {
                dbus_message_set_auto_start(u, FALSE);
                DBusError ue;
                dbus_error_init(&ue);
                DBusMessage *ur = dbus_connection_send_with_reply_and_block(g_conn, u, 2000, &ue);
                if (ur) dbus_message_unref(ur);
                dbus_error_free(&ue);
            }
            dbus_message_unref(u);
        }
    }

    // 2. load exactly one instance.
    DBusMessage *msg = dbus_message_new_method_call("org.kde.KWin", "/Scripting",
                                                    "org.kde.kwin.Scripting",
                                                    "loadScript");
    if (!msg) return 0;
    if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &scriptPath,
                                     DBUS_TYPE_STRING, &pluginName, DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return 0;
    }
    dbus_message_set_auto_start(msg, FALSE);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(g_conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (dbus_error_is_set(&err)) recordError(err.message);
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(reply);

    // 3. start it (KWin loads a script paused; start() is what runs its timers).
    {
        DBusMessage *st = dbus_message_new_method_call("org.kde.KWin", "/Scripting",
                                                       "org.kde.kwin.Scripting", "start");
        if (st) {
            dbus_message_set_auto_start(st, FALSE);
            DBusError se;
            dbus_error_init(&se);
            DBusMessage *sr = dbus_connection_send_with_reply_and_block(g_conn, st, 2000, &se);
            if (sr) dbus_message_unref(sr);
            dbus_error_free(&se);
            dbus_message_unref(st);
        }
    }
    return 1;
}

// Unload one instance of the KWin helper by plugin name (--remove-geometry).
// Returns 1 when KWin answered, 0 otherwise.
int geomBusUnloadScript(const char *pluginName) {
    if (!g_conn || !dbus_connection_get_is_connected(g_conn)) return 0;
    if (!pluginName || !*pluginName) return 0;
    DBusMessage *msg = dbus_message_new_method_call("org.kde.KWin", "/Scripting",
                                                    "org.kde.kwin.Scripting",
                                                    "unloadScript");
    if (!msg) return 0;
    const char *n = pluginName;
    if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return 0;
    }
    dbus_message_set_auto_start(msg, FALSE);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(g_conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (dbus_error_is_set(&err)) recordError(err.message);
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(reply);
    return 1;
}

// TRUE when the KWin script of that name is loaded. Read through the same
// connection; a false here is the bar's cue to reload it.
int geomBusScriptLoaded(const char *pluginName) {
    if (!g_conn || !dbus_connection_get_is_connected(g_conn)) return -1;
    DBusMessage *msg = dbus_message_new_method_call("org.kde.KWin", "/Scripting",
                                                    "org.kde.kwin.Scripting",
                                                    "isScriptLoaded");
    if (!msg) return -1;
    const char *n = pluginName ? pluginName : "";
    if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return -1;
    }
    dbus_message_set_auto_start(msg, FALSE);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(g_conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (dbus_error_is_set(&err)) recordError(err.message);
        dbus_error_free(&err);
        return -1;
    }
    int loaded = 0;
    if (!dbus_message_get_args(reply, &err, DBUS_TYPE_BOOLEAN, &loaded, DBUS_TYPE_INVALID))
        loaded = -1;
    dbus_message_unref(reply);
    dbus_error_free(&err);
    return loaded;
}
