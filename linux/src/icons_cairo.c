// linux/src/icons_cairo.c - the cairo renderer for the shared icon geometry.
//
// The template is identical to the Windows renderer (p_icons.c's GDI+ path
// stroke): take the flattened polylines for an icon id and stroke them with
// round caps and joins, scaled from the 24-unit SVG viewBox into the box the bar
// asks for. Only the drawing call differs - cairo_stroke instead of
// GdipDrawPath - so an icon cannot look different on Linux by accident.
//
// The bezier and arc SAMPLING (which decides how round a circle is) lives in
// p_svg.c and is therefore literally the same code on both platforms; this file
// adds no geometry of its own.

#include <math.h>

#include "linux_svg.h"
#include "svg_geometry.h"  // native/src: the shared shape + ids

void svgInitPaths(void) { cbSvgFlattenAll(); }

// The battery is DYNAMIC, exactly as on Windows (p_icons.c's svgDrawBatt): the
// static path draws only the shell and the nub, and the inner fill tracks the
// charge - growing from empty to full, red under 10% while on battery, and a
// zigzag bolt on top while charging. Electron's batBody geometry, so the two
// platforms show the same thing: innerX 4.3, innerW 12.4, y 9.7, h 5.1, rx 0.8,
// fill opacity 0.75 (0.9 when low), a 4% floor so it never collapses to nothing.
void svgDrawBattery(cairo_t *cr, int pct, int ac, double x, double y, double box,
                    double r, double g, double b, double alpha,
                    double wr, double wg, double wb) {
    svgDrawCairo(cr, SVG_BAT, x, y, box, r, g, b, alpha);
    if (pct < 0) return;
    if (pct > 100) pct = 100;
    int bucket = pct / 2;                       // 2% steps, like the Windows key
    double s = box / 24.0;
    int low = (pct < 10) && !ac;                // charging outranks the red fill
    double fr = low ? wr : r, fg2 = low ? wg : g, fb = low ? wb : b;
    double falpha = (low ? 0.9 : 0.75) * alpha;

    double fw = 12.4 * (double)(bucket * 2) / 100.0;
    if (fw < 0.9) fw = 0.9;                     // never a zero-width sliver
    double fx = 4.3, fy = 9.7, fh = 5.1, frad = 0.8;
    if (frad * 2 > fw) frad = fw / 2;
    if (frad * 2 > fh) frad = fh / 2;

    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    cairo_new_sub_path(cr);
    cairo_arc(cr, fx + frad, fy + frad, frad, M_PI, 1.5 * M_PI);
    cairo_arc(cr, fx + fw - frad, fy + frad, frad, 1.5 * M_PI, 2 * M_PI);
    cairo_arc(cr, fx + fw - frad, fy + fh - frad, frad, 0, 0.5 * M_PI);
    cairo_arc(cr, fx + frad, fy + fh - frad, frad, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, fr, fg2, fb, falpha);
    cairo_fill(cr);

    if (ac) {
        // the charge bolt, over the fill (Electron batCharge, stroke 1.8)
        // Same 4-point open polyline the Windows renderer draws (p_icons.c:509-512)
        cairo_new_path(cr);
        cairo_move_to(cr, 12.2, 8.9);
        cairo_line_to(cr, 9.6, 12.4);
        cairo_line_to(cr, 12.0, 12.4);
        cairo_line_to(cr, 9.9, 15.7);
        cairo_set_source_rgba(cr, r, g, b, alpha);
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void svgDrawCairo(cairo_t *cr, int id, double x, double y, double box,
                  double r, double g, double b, double alpha) {
    int nparts = 0;
    const SvgFlat *fl = cbSvgFlatById(id, &nparts);
    if (!fl || nparts <= 0) return;

    // the paths are authored in a 24-unit viewBox; the bar's icon box is 12 CSS
    // px, so the scale is box/24 (the Windows renderer's `s = g_scale / 2` is the
    // same ratio once box = 12 * scale)
    double s = box / 24.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int j = 0; j < nparts; j++) {
        const SvgPath *p = &fl[j].path;
        // stroke width, in the 24-unit space: the Windows table carries a
        // per-part multiplier (SVG_P[][].w) that p_icons.c applies to its pen;
        // 2.2 units is the authored base every built-in path uses
        double w = 2.2 * (fl[j].w > 0 ? fl[j].w : 1.0);
        cairo_set_line_width(cr, w);
        for (int si = 0; si < p->nsub; si++) {
            const SvgSub *ss = &p->s[si];
            if (ss->n < 2) continue;
            cairo_move_to(cr, ss->p[0].x, ss->p[0].y);
            for (int i = 1; i < ss->n; i++) cairo_line_to(cr, ss->p[i].x, ss->p[i].y);
            if (ss->closed) cairo_close_path(cr);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
}
