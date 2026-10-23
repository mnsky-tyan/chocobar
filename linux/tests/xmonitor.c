/* xmonitor - a root-window ClientMessage watcher for the Linux bar's behaviour test.
 *
 * Some of the bar's contracts are X requests TO THE WINDOW MANAGER, not window
 * properties, so nothing about the window can be read back afterwards: the EWMH
 * all-desktops request is a ClientMessage sent to the root window, and the
 * private Xvfb this suite runs on has no window manager to act on it. The only
 * observable trace is the message itself, so this helper selects
 * SubstructureNotifyMask on the root window - one of the two masks the message
 * is sent with, and the one that does NOT make the client responsible for
 * mapping top-level windows (SubstructureRedirect would swallow every
 * XMapWindow) - and appends one line per ClientMessage it sees:
 *
 *   <window id, decimal> <message type atom name> <data.l[0], unsigned 32-bit>
 *
 * usage: xmonitor <logfile>     (runs until killed; lines are flushed per event)
 *
 * Only ever run this on a private Xvfb display, never on the user's session.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: xmonitor <logfile>\n");
        return 2;
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xmonitor: cannot open display\n"); return 1; }
    FILE *out = fopen(argv[1], "a");
    if (!out) { fprintf(stderr, "xmonitor: cannot open %s\n", argv[1]); return 1; }
    Window root = RootWindow(d, DefaultScreen(d));
    XSelectInput(d, root, SubstructureNotifyMask);
    XSync(d, False);
    for (;;) {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type != ClientMessage) continue;
        char *name = XGetAtomName(d, ev.xclient.message_type);
        fprintf(out, "%lu %s %lu\n", (unsigned long)ev.xclient.window,
                name ? name : "?", (unsigned long)(unsigned int)ev.xclient.data.l[0]);
        fflush(out);
        if (name) XFree(name);
    }
}
