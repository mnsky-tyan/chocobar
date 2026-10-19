// linux/src/linux_svg.h - the seam to the shared icon geometry (native/src/p_svg.c).
//
// p_svg.c is compiled into the same translation unit as this front-end (the
// amalgamation the build does), so the SvgPath tables it flattens are visible
// here. This header only declares the two entry points the bar calls, so main.c
// does not depend on p_svg.c's internal types.
#ifndef LINUX_SVG_H
#define LINUX_SVG_H

#include <cairo/cairo.h>

// Flatten the built-in icon table once (p_svg.c's svgFlattenAll).
void svgInitPaths(void);

// Stroke icon `id` (SVG_CPU, SVG_RAM, ... from p_svg.c's enum) into `cr` inside a
// `box`-pixel square at (x, y), in the given colour. This is the Linux renderer:
// the Windows one is GDI+ in p_icons.c, the geometry they stroke is the same.
void svgDrawCairo(cairo_t *cr, int id, double x, double y, double box,
                  double r, double g, double b, double alpha);

#endif
