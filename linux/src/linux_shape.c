// linux_shape.c - the two shape helpers, defined once.
//
// See linux_shape.h for why this exists: the rounded path and the scanline
// staircase were three copies with divergent caps, and one of them had a
// truncating float that left a 1 px hole the shape clipped away.

#include <math.h>

#include "linux_shape.h"

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
