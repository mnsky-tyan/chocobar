// subs.h - the subscription board window.
//
// The Linux twin of p_ui.c's g_dashType == 1 branch of paintDash: one full-width
// panel per enabled provider, each of that provider's windows as a donut with
// its label, credit totals and reset countdown beside it. The numbers come from
// the shared core through the cbSubs* seam (p_subs.c), so the board shows the
// same records the gauge chip rotates through.
//
// Driven by the BAR's loop, like the token board: one connection, two windows,
// one pump.
#ifndef CHOCOBAR_SUBS_H
#define CHOCOBAR_SUBS_H

#include <X11/Xlib.h>

// Creates the board window and paints it once. Returns the window id, or None.
// Events for that window are routed by the bar's loop into subsEvent(), and
// subsClose() destroys it.
Window subsOpen(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
                int cssW, int cssH, double scale, const char *fontFamily,
                const double page[3], const double card[3], const double head[3],
                const double fg[3], const double dim[3], const double divider[3],
                const double ramp[5][3], const double pink[3], const double pinkDeep[3],
                const double warn[3], const double good[3], const double yellow[3],
                const double iconCol[3]);

// Repaint after a fetch lands (a new subs epoch on the shared core).
void subsRepaint(void);

// Handles one event for the board's window. Returns 1 when the board wants to
// close (Esc, q, middle/right click, the WM's close button).
int subsEvent(XEvent *ev);

// Destroys the board window. Safe when no board is open.
void subsClose(void);

// The board window's id, None when no board is open.
Window subsWin(void);

#endif
