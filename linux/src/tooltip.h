// linux/src/tooltip.h - the one tooltip surface on the Linux bar.
//
// The Windows bar has exactly this component (p_ui.c tipShow/tipHide): a
// browser-style white tooltip naming a chip under the pointer ("CPU usage",
// "Memory usage", ...) and a dark rounded card for a heatmap cell
// ("Thursday, Sep 12, 2025 - 1.2M tokens"). Both platforms must show the same
// thing on hover, so both front-ends share one implementation instead of each
// growing its own popup.
//
// It is ONE window reused for every tip: created on first use, shown/hidden,
// resized to the text. Override-redirect and never focused, so it behaves like
// a tooltip - no window manager decoration, no taskbar entry, no activation.
#ifndef CHOCOBAR_TOOLTIP_H
#define CHOCOBAR_TOOLTIP_H

#include <X11/Xlib.h>

// Remember the display/visual the tips are drawn on. Safe to call repeatedly
// (a config reload may change the font, not the display).
void tipInit(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
             double scale);

// Show a tip for `text` with the pointer at window-relative (cx, cy).
//   family - the font family for the text ("" = the system sans, which is the
//            Linux stand-in for the Segoe UI a Windows tooltip uses)
//   cssPx  - font size in CSS pixels
//   dark   - 0 = white browser tooltip + hairline border, 1 = the board's dark
//            rounded card with a bold first line
// A NULL or empty text hides the tip (so a caller can pass a chip title
// straight through). Returns nothing: a tip that cannot be created simply does
// not show.
void tipShow(const char *text, int cx, int cy, const char *family, double cssPx, int dark);

void tipHide(void);

// The tip's window, so an event loop can recognise (and ignore) its events.
Window tipWin(void);

// Destroy the window (bar shutdown). Idempotent.
void tipFree(void);

#endif
