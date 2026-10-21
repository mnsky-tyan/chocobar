/* xclick - a three-line XTEST pointer driver for the Linux bar's behaviour test.
 *
 * The test needs REAL pointer input: the bar's menu opens on the button
 * RELEASE, and the dismissing click is consumed by the menu's own pointer
 * grab. A synthetic event sent with XSendEvent would not reproduce either,
 * because a fake event carries no grab and no pointer state.
 *
 * XTEST drives the virtual pointer of whatever display it is pointed at, so
 * this MUST only ever be used on a private Xvfb display - never on the user's
 * session (the global rule: no synthetic input on the real desktop).
 *
 * usage: xclick <x> <y> <button> [full|press|release]
 *   full    press then release (default)
 *   press   press only  (the bar must open NOTHING on this)
 *   release release only
 */
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: xclick <x> <y> <button> [full|press|release]\n");
        return 2;
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xclick: cannot open display\n"); return 1; }
    int x = atoi(argv[1]);
    int y = atoi(argv[2]);
    int b = atoi(argv[3]);
    const char *mode = (argc > 4) ? argv[4] : "full";

    XTestFakeMotionEvent(d, -1, x, y, CurrentTime);
    XFlush(d);
    usleep(60000);
    if (!strcmp(mode, "press")) {
        XTestFakeButtonEvent(d, b, True, CurrentTime);
    } else if (!strcmp(mode, "release")) {
        XTestFakeButtonEvent(d, b, False, CurrentTime);
    } else {
        XTestFakeButtonEvent(d, b, True, CurrentTime);
        XFlush(d);
        usleep(60000);
        XTestFakeButtonEvent(d, b, False, CurrentTime);
    }
    XFlush(d);
    XSync(d, False);
    XCloseDisplay(d);
    return 0;
}
