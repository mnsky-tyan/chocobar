// linux_shape.h - the two shape helpers every Linux front-end draws with.
//
// The rounded rectangle and the XFixes scanline staircase were written out in
// THREE places (bar, menu, dashboard) with divergent magic caps, and the copies
// had already drifted when a truncating float in one of them left a 1 px hole
// across the bar - the same bug then needed fixing twice. One definition is what
// keeps the next geometry change from needing three edits and missing one.
//
// Cost: two plain functions, no state; the scanline builder allocates nothing
// beyond the caller's array.
#ifndef CHOCOBAR_LINUX_SHAPE_H
#define CHOCOBAR_LINUX_SHAPE_H

#include <cairo/cairo.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

// the boards' corner radius, CSS px. p_ui.c rounds BOTH Windows boards with
// DWMWCP_ROUND_NATIVE, whose default is 8 px - one definition so the two Linux
// boards cannot drift apart (they are drawn side by side in screenshots).
#define BOARD_RADIUS_CSS 8.0

// Build the path for a rounded rectangle. The path is left open for
// fill_preserve(); the caller closes or fills as it needs. r is clamped to the
// rectangle, and a 0 radius degenerates to a plain rectangle.
void roundedPath(cairo_t *cr, double x, double y, double w, double h, double r);

// Fill rects with the scanline staircase for a rounded rect of the given size
// and radius, and return how many were written. maxRects caps the array; on a
// cap too small the corners are the bands that lose resolution, never the
// interior. Rows tile exactly - this is where the 1 px hole used to come from.
int roundedRectScanlines(XRectangle *rects, int maxRects, int w, int h, double radius);

// ---- the boards' rounded window shape, defined once ----------------------------
//
// Both boards (the token dashboard and the subscription board) round their
// window corners, and BOTH resize their window to fit their content. The shape
// therefore has to be REBUILT whenever the built geometry changes: a guard on
// the radius alone (which both copies had) keeps the region created at open, so
// a board that grows to more providers clips its last panel and a board that
// shrinks keeps a too-tall region with square bottom corners - and the
// ConfigureNotify path could never fix it either, because it hit the same guard.

// Rebuild the caller's region (an XserverRegion it owns, with the (w, h, r)
// geometry it was last built for in *heldW / *heldH / *heldR) to the rounded
// rect of (w, h, radius*scale) and set it on `win`. The region is rebuilt only
// when that key differs, so calling this on every resize costs one region per
// actual geometry change. Any of heldW / heldH / heldR may be NULL (that part
// of the key is then always treated as changed). Returns 1 when a region was
// (re)built, 0 when the cached one still matched.
int boardShapeApply(Display *dpy, Window win, int w, int h, double radius,
                    double scale, XserverRegion *held,
                    int *heldW, int *heldH, int *heldR);

// Free the region and forget the cache. Safe to call on a zeroed struct.
void boardShapeFree(Display *dpy, XserverRegion *held,
                    int *heldW, int *heldH, int *heldR);

// Top-left for a w x h board centred on the head the pointer is on (p_ui.c's
// dashToggle placement: centred on the screen, both axes). The boards use this
// at open and after every content-fit, so a fit that changed the height keeps
// the board centred instead of drifting down.
void boardCenterOnHead(Display *dpy, int screen, int w, int h, int *ox, int *oy);

// Turn a board window into a managed popup that behaves like p_ui.c's owned
// WS_POPUP: no titlebar, no taskbar entry, an exact rectangle - but NORMAL
// stacking, so another window can cover it, and movable by the app's own drag
// (an override-redirect window is neither coverable nor movable: KWin composites
// OR windows in the top layer and nothing ever translates them, which is how
// v1.6.3 shipped both board defects at once). The properties are written BEFORE
// the map: KWin reads window type, state and hints when it first manages the
// window, so a property written after the map is a visible relayout.
//
// NOT in the state: _NET_WM_STATE_BELOW. The bar carries it (the bar must sit
// under normal windows); a board is the opposite - HWND_TOP at open, coverable
// but above the bar.
//
// The window keeps its input handling: unlike the bar, a board takes the
// keyboard (Esc/q close it), so this sets no InputHint and no _NET_WM_USER_TIME
// - the caller activates the window itself after the map, the same way it did
// when the board was override-redirect. It DOES advertise WM_DELETE_WINDOW:
// a managed window that takes focus but answers no close protocol makes a WM
// fall back to XKillClient, which would sever the bar's shared connection.
void boardMakeManaged(Display *dpy, Window win, const char *name,
                      const char *resName, int x, int y, int w, int h);

// Activate a just-mapped board the way Windows' SetWindowPos(HWND_TOP) does for
// p_ui.c's board: raise it above the window the user was in, and give it the
// keyboard (the Linux boards close on Esc/q, so the keyboard is the point).
//
// The EWMH request is what a window manager can honour: KWin's focus-stealing
// prevention refuses an activation it cannot attribute to user activity, and a
// board that maps with no request sits BEHIND the terminal - the exact "it
// sinks behind my windows" bug the Windows board fixed with HWND_TOP. `when`
// must be the timestamp of the click that opened the board (KWin compares it
// with the last user input and lets a matching one through); 0 means no click
// is known and only the direct focus is used.
//
// The direct XSetInputFocus stays as the fallback a WM-less display needs (the
// behaviour suite's Xvfb): it is the same call the boards always made.
// The post-map all-desktops request (EWMH _NET_WM_DESKTOP ClientMessage): a
// pre-map statement of the desktop does not survive the map on KWin - it
// resolves to the current desktop and writes the concrete number back (the bar
// learned the same lesson; main.c afterMap() carries the bar's request). Call
// right after XMapWindow.
void boardRequestAllDesktops(Display *dpy, Window win);

void boardActivate(Display *dpy, Window win, Time when);

#endif
