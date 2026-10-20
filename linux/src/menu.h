// menu.h - the right-click menu's item ids and entry point.
//
// The menu is its own override-redirect window (see menu.c): the bar is shaped to
// a rounded strip, so a menu drawn inside it would be clipped by that shape or
// force the bar to grow. Ids are what the bar acts on after menuRun returns.
#ifndef CHOCOBAR_MENU_H
#define CHOCOBAR_MENU_H

#include <X11/Xlib.h>

enum {
    MENU_NONE = 0,
    MENU_TOKEN_DASH = 10,
    MENU_SUBS_DASH = 11,
    MENU_EDIT = 1,
    MENU_OPEN_DIR = 2,
    MENU_RELOAD = 3,
    MENU_QUIT = 4,
};

// Run the menu modally at screen position (atX, atY) and return the chosen id
// (MENU_NONE when dismissed). scale/fontPx/fontFamily come from the config so the
// menu matches the bar; the colours are already resolved 0..1 triples. There is
// no dim colour: the menu has no dim text, and passing one invited a parameter
// the painter never read.
int menuRun(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
            int atX, int atY, double scale, int fontPx, const char *fontFamily,
            const double bg[3], double bgAlpha, const double fg[3],
            const double divider[3], const double accent[3]);

#endif
