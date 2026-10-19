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
