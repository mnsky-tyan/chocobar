// dash.h - the token dashboard window.
//
// A normal managed window (unlike the bar and the menu): a dashboard is opened,
// moved and closed like any window. It reads the shared aggregates through
// p_agg.c's cbTok* accessors, so its numbers are the Windows board's numbers.
#ifndef CHOCOBAR_DASH_H
#define CHOCOBAR_DASH_H

#include <X11/Xlib.h>

void dashRun(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
             int cssW, int cssH, double scale, int fontPx, const char *fontFamily,
             const double page[3], const double card[3], const double fg[3],
             const double dim[3], const double accent[3], const double divider[3],
             const double warn[3], const double ramp[5][3], int heatWeeks);

#endif
