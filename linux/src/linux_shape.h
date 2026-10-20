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

// Build the path for a rounded rectangle. The path is left open for
// fill_preserve(); the caller closes or fills as it needs. r is clamped to the
// rectangle, and a 0 radius degenerates to a plain rectangle.
void roundedPath(cairo_t *cr, double x, double y, double w, double h, double r);

// Fill rects with the scanline staircase for a rounded rect of the given size
// and radius, and return how many were written. maxRects caps the array; on a
// cap too small the corners are the bands that lose resolution, never the
// interior. Rows tile exactly - this is where the 1 px hole used to come from.
int roundedRectScanlines(XRectangle *rects, int maxRects, int w, int h, double radius);

#endif
