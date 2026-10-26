// linux_shape.c - the two shape helpers, defined once.
//
// See linux_shape.h for why this exists: the rounded path and the scanline
// staircase were three copies with divergent caps, and one of them had a
// truncating float that left a 1 px hole the shape clipped away.

#include <math.h>
#include <string.h>

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
