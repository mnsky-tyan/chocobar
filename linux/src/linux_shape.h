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

#endif
