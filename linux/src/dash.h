// dash.h - the token dashboard window.
//
// A normal managed window (unlike the bar and the menu): a dashboard is opened,
// moved and closed like any window. It reads the shared aggregates through
// p_agg.c's cbTok* accessors, so its numbers are the Windows board's numbers.
//
// The window is driven by the BAR's event loop, not its own: one connection,
// two windows, one pump. On Windows the board is a second window dispatched by
// the same GetMessage loop while the bar's WM_TIMER keeps ticking; a nested
// loop here would freeze the bar's clock for as long as the board stays open.
#ifndef CHOCOBAR_DASH_H
#define CHOCOBAR_DASH_H

#include <X11/Xlib.h>

// Creates the board window and paints it once. Returns the window id, or None
// if the window could not be created. Events for that window are then routed
// by the bar's loop into dashEvent(), and dashClose() destroys it.
// lastScanAt: the local epoch the token scan last finished, shown in the
// footer as "last scan HH:MM:SS" (0 = never scanned, drawn as an em dash).
Window dashOpen(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
                int cssW, int cssH, double scale, const char *fontFamily,
                const double page[3], const double card[3], const double fg[3],
                const double dim[3], const double divider[3], const double ramp[5][3],
                const double pink[3], const double pinkDeep[3],
                const double yellow[3], long long lastScanAt);

// Handles one event that belongs to the board's window (the caller routes by
// xany.window). Returns 1 when the board wants to close (Esc, q, middle/right
// click, the WM's close button) - the caller then calls dashClose().
int dashEvent(XEvent *ev);

// Destroys the board window. Safe to call when no board is open.
void dashClose(void);

// The board window's id, None when no board is open.
Window dashWin(void);

#endif
