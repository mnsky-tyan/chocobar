// linux_shape.c - the shape and board-placement helpers, defined once.
//
// See linux_shape.h for why this exists: the rounded path and the scanline
// staircase were three copies with divergent caps, and one of them had a
// truncating float that left a 1 px hole the shape clipped away.

#include <math.h>
#include <string.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#include "linux_shape.h"

// Top-left for a w x h board centred on the head the POINTER is on. p_ui.c
// centres a board on the primary screen (x = (SM_CXSCREEN - w)/2,
// y = (CYSCREEN - h)/2 - dashMaxH() is CYSCREEN-40, and
// (dashMaxH() + 40 - h)/2 folds back to plain vertical centring). On X the
// pointer's head is the one the user is looking at, which centres on the same
// screen on a single-head desktop and on the right one on a multi-head one.
void boardCenterOnHead(Display *dpy, int screen, int w, int h, int *ox, int *oy) {
    Window root, child;
    int rx = 0, ry = 0, wx, wy;
    unsigned int mask = 0;
    int px = -1, py = -1;
    if (XQueryPointer(dpy, RootWindow(dpy, screen), &root, &child,
                      &rx, &ry, &wx, &wy, &mask)) {
        px = rx; py = ry;
    }
    int hx = 0, hy = 0;
    int hw = DisplayWidth(dpy, screen), hh = DisplayHeight(dpy, screen);
    int n = 0;
    XineramaScreenInfo *xi = XineramaQueryScreens(dpy, &n);
    if (xi && n > 0) {
        // fall back to the first head when the pointer answers nothing or sits
        // off every head (a detached head in the layout)
        hx = xi[0].x_org; hy = xi[0].y_org; hw = xi[0].width; hh = xi[0].height;
        for (int i = 0; i < n; i++) {
            if (px >= xi[i].x_org && px < xi[i].x_org + xi[i].width &&
                py >= xi[i].y_org && py < xi[i].y_org + xi[i].height) {
                hx = xi[i].x_org; hy = xi[i].y_org;
                hw = xi[i].width; hh = xi[i].height;
                break;
            }
        }
    }
    if (xi) XFree(xi);
    int x = hx + (hw - w) / 2, y = hy + (hh - h) / 2;
    if (x < hx) x = hx;
    if (y < hy) y = hy;
    *ox = x; *oy = y;
}

void roundedPath(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    if (r <= 0) { cairo_rectangle(cr, x, y, w, h); return; }
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

int roundedRectScanlines(XRectangle *rects, int maxRects, int w, int h, double radius) {
    int nr = 0;
    if (radius < 0.5) {
        if (nr >= maxRects) return nr;
        rects[nr].x = 0; rects[nr].y = 0;
        rects[nr].width = (unsigned short)w;
        rects[nr].height = (unsigned short)h;
        return nr + 1;
    }
    int steps = (int)(radius + 0.5);              // rows the corner arcs take
    // leave two rows of headroom for the middle band and the square fallback
    if (steps > maxRects / 2 - 2) steps = maxRects / 2 - 2;
    if (steps < 0) steps = 0;
    for (int i = 0; i < steps && nr < maxRects; i++) {
        double dy = radius - i;
        double inset = radius - sqrt(fmax(0.0, radius * radius - dy * dy));
        rects[nr].x = (short)inset;
        rects[nr].y = (short)i;
        rects[nr].width = (unsigned short)fmax(1.0, w - 2 * inset);
        rects[nr].height = 1;
        nr++;
    }
    // the middle band is derived from steps, NOT from truncating the float
    // radius: (unsigned short)(h - 2*radius) floors down (22.56 -> 22), which
    // once left row 34 uncovered between this band and the bottom loop - a 1 px
    // horizontal hole the shape clipped away, so raw wallpaper showed through
    // the bar as a bright line.
    if (nr < maxRects) {
        rects[nr].x = 0;
        rects[nr].y = (short)steps;
        rects[nr].width = (unsigned short)w;
        rects[nr].height = (unsigned short)fmax(1, h - 2 * steps);
        nr++;
    }
    for (int i = 0; i < steps && nr < maxRects; i++) {
        double dy = radius - i;
        double inset = radius - sqrt(fmax(0.0, radius * radius - dy * dy));
        rects[nr].x = (short)inset;
        rects[nr].y = (short)(h - 1 - i);
        rects[nr].width = (unsigned short)fmax(1.0, w - 2 * inset);
        rects[nr].height = 1;
        nr++;
    }
    return nr;
}

// ---- the boards' rounded window shape ------------------------------------------
//
// One definition for both boards. The rebuild key is the geometry the region was
// built for (w, h, r): a board that fits its content resizes on open and again
// on every repaint that changes the content height, and the old radius-only
// guard kept the open-time region forever, clipping whatever grew past it.
int boardShapeApply(Display *dpy, Window win, int w, int h, double radius,
                    double scale, XserverRegion *held,
                    int *heldW, int *heldH, int *heldR) {
    if (!dpy || !win || w <= 0 || h <= 0) return 0;
    double r = radius * scale;
    if (r < 0) r = 0;
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    int ri = (int)r;
    if (held && *held &&
        (!heldW || *heldW == w) && (!heldH || *heldH == h) &&
        (!heldR || *heldR == ri)) return 0;   // geometry unchanged: keep it
    XRectangle rects[80];
    int nr = roundedRectScanlines(rects, 80, w, h, r);
    if (nr <= 0) return 0;
    if (held && *held) XFixesDestroyRegion(dpy, *held);
    XserverRegion reg = XFixesCreateRegion(dpy, rects, nr);
    if (!reg) return 0;
    XFixesSetWindowShapeRegion(dpy, win, ShapeBounding, 0, 0, reg);
    if (held) *held = reg; else XFixesDestroyRegion(dpy, reg);
    if (heldW) *heldW = w;
    if (heldH) *heldH = h;
    if (heldR) *heldR = ri;
    return 1;
}

void boardShapeFree(Display *dpy, XserverRegion *held,
                    int *heldW, int *heldH, int *heldR) {
    if (dpy && held && *held) XFixesDestroyRegion(dpy, *held);
    if (held) *held = 0;
    if (heldW) *heldW = 0;
    if (heldH) *heldH = 0;
    if (heldR) *heldR = 0;
}

// The managed-popup recipe, shared by both boards (main.c carries the bar's own
// variant, whose differences are deliberate: the bar wants BELOW + no input;
// a board wants the normal layer and takes the keyboard). The 32-bit long is
// the format-32 property width, not a platform choice: on 64-bit Xlib the
// CARDINAL/ATOM arrays must be `long`.
typedef struct { unsigned long flags, functions, decorations; long inputMode;
                 unsigned long status; } BoardMotifHints;
#define BOARD_MWM_HINTS_DECORATIONS 2UL

void boardMakeManaged(Display *dpy, Window win, const char *name,
                      const char *resName, int x, int y, int w, int h) {
    // USPosition: KWin honours a user-specified position instead of running its
    // placement policy (which would centre the board and land it somewhere the
    // pointer's head never asked for). The bar does the same before every map.
    XSizeHints sh;
    memset(&sh, 0, sizeof(sh));
    sh.flags = USPosition | PPosition | USSize | PSize;
    sh.x = x; sh.y = y; sh.width = w; sh.height = h;
    XSetWMNormalHints(dpy, win, &sh);
    // no titlebar, no frame: _MOTIF_WM_HINTS with only the decorations bit, 0
    BoardMotifHints mh = { BOARD_MWM_HINTS_DECORATIONS, 0, 0, 0, 0 };
    XChangeProperty(dpy, win, XInternAtom(dpy, "_MOTIF_WM_HINTS", False),
                    XInternAtom(dpy, "_MOTIF_WM_HINTS", False), 32, PropModeReplace,
                    (unsigned char *)&mh, 5);
    // normal window type: DOCK/UTILITY would pin the board to a special layer
    // or tie it to another window; NORMAL is the layer Windows' HWND_TOP sits in
    Atom typeNormal = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False),
                    XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&typeNormal, 1);
    // Out of the taskbar and the pager, like the Windows owned popup (no
    // taskbar button, no Alt-Tab entry), and STICKY - on all desktops, so the
    // board stays over the bar when the user switches desktops while it is up
    // (the bar is on every desktop; KWin's all-desktops state is STICKY - it
    // normalises _NET_WM_DESKTOP to the concrete number and keeps the STICKY
    // state, which is exactly how the bar reads back). Pre-map properties, not
    // client messages: nothing is managing the window yet.
    Atom states[3] = { XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                       XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False),
                       XInternAtom(dpy, "_NET_WM_STATE_STICKY", False) };
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False),
                    XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, 3);
    // NOT set here: _NET_WM_DESKTOP. The board should sit on all desktops like
    // the bar it floats over, but a pre-map statement of that desktop does not
    // survive the map - KWin resolves it to the current desktop and writes the
    // concrete number back (measured, and main.c's afterMap() documents the
    // same fact for the bar). The working request is the post-map ClientMessage,
    // which the open paths send through boardRequestAllDesktops().
    XStoreName(dpy, win, name);
    XClassHint cls = { (char *)resName, (char *)"Chocobar" };
    XSetClassHint(dpy, win, &cls);
}

// The post-map all-desktops request (EWMH _NET_WM_DESKTOP ClientMessage). The
// pre-map STICKY state above is what KWin keys on; this message is the EWMH-correct
// request on top of it (a pre-map _NET_WM_DESKTOP statement itself is dropped -
// KWin resolves it to the current desktop and writes the number back, which
// main.c's afterMap() documents for the bar). Sent right after XMapWindow.
void boardRequestAllDesktops(Display *dpy, Window win) {
    XEvent cm;
    memset(&cm, 0, sizeof(cm));
    cm.xclient.type = ClientMessage;
    cm.xclient.window = win;
    cm.xclient.message_type = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    cm.xclient.format = 32;
    cm.xclient.data.l[0] = 0xFFFFFFFF;   // all desktops
    cm.xclient.data.l[1] = 1;            // source: normal application
    XSendEvent(dpy, RootWindow(dpy, DefaultScreen(dpy)), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &cm);
    XFlush(dpy);
}

void boardActivate(Display *dpy, Window win, Time when) {
    if (when != 0) {
        // source indication 1 = application (EWMH 1.4, _NET_ACTIVE_WINDOW
        // data.l[0]): the board is the app's own window, and KWin's
        // focus-stealing prevention allows an application-sourced activation
        // whose timestamp matches the last user input - which the click that
        // opened the board provides. Claiming "pager" would be a lie about who
        // asked.
        XEvent cm;
        memset(&cm, 0, sizeof(cm));
        cm.xclient.type = ClientMessage;
        cm.xclient.window = win;
        cm.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
        cm.xclient.format = 32;
        cm.xclient.data.l[0] = 1;                 // source: application
        cm.xclient.data.l[1] = (long)when;        // the click's timestamp
        cm.xclient.data.l[2] = 0;                 // currently active window
        XSendEvent(dpy, RootWindow(dpy, DefaultScreen(dpy)), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &cm);
    }
    // fallback (and confirmation): the direct focus a WM-less display needs
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
    XFlush(dpy);
}
