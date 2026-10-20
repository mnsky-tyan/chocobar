// linux/src/menu.c - the right-click menu, matching the Windows tray menu.
//
// On Windows the menu is a tray icon plus a popup; on Linux the bar itself is the
// handle, so a right-click on the strip opens this: a small override-redirect
// window painted with cairo, keyboard-navigable, closing on Escape or an outside
// click. The items mirror native/src/p_ui.c's tray menu (Token dashboard,
// Subscription dashboard, Edit config, Open config folder, Reload, Quit) minus the
// two that have no Linux meaning (Start with Windows is systemd here; Check for
// updates needs the release feed).
//
// It is a separate top-level window on purpose: the bar is shaped to a rounded
// strip, and drawing a menu inside it would either be clipped by that shape or
// force the bar to grow. A second small window keeps the bar's geometry honest.

#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>

#include "linux_shape.h"
#include "menu.h"

#define MENU_W 236.0        // CSS px
#define MENU_ROW 30.0
#define MENU_PAD 6.0
#define MENU_SEP 9.0
#define MENU_RADIUS 8.0     // corner radius, CSS px (one name, one place)

typedef struct {
    const char *label;
    int id;
    int separator;          // 1 = draw a divider instead of a row
} MenuItem;

// ids the bar acts on (menu.h)
static const MenuItem g_items[] = {
    { "Token dashboard",        MENU_TOKEN_DASH, 0 },
    { "Subscription dashboard", MENU_SUBS_DASH,  0 },
    { "", 0, 1 },
    { "Edit config",            MENU_EDIT,       0 },
    { "Open config folder",     MENU_OPEN_DIR,   0 },
    { "", 0, 1 },
    { "Reload chocobar",        MENU_RELOAD,     0 },
    { "Quit chocobar",          MENU_QUIT,       0 },
};
#define NITEMS ((int)(sizeof(g_items) / sizeof(g_items[0])))

typedef struct {
    Display *dpy;
    Window win;
    Visual *visual;
    Colormap cmap;
    int w, h;
    int hover;
    int result;
    double scale;
    int fontPx;
    char fontFamily[128];
    /* palette, resolved by the caller from the config */
    double bg[3], fg[3], divider[3], accent[3];
    double bgAlpha;
    /* persistent paint state: the bar keeps one too, and building a surface
       plus a toy font face on every hover move was an allocation per mouse
       pixel. The menu is static, so there is nothing to resize. */
    cairo_surface_t *surf;
    cairo_t *cr;
    cairo_font_face_t *ff;
} Menu;

static void menuPaint(Menu *m) {
    cairo_t *cr = m->cr;
    double r = MENU_RADIUS * m->scale;

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    roundedPath(cr, 0, 0, m->w, m->h, r);
    // the menu is more opaque than the bar: a menu must be readable over anything
    cairo_set_source_rgba(cr, m->bg[0], m->bg[1], m->bg[2], m->bgAlpha);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, m->divider[0], m->divider[1], m->divider[2], 0.75);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_set_font_face(cr, m->ff);
    cairo_set_font_size(cr, m->fontPx);

    double y = MENU_PAD * m->scale;
    for (int i = 0; i < NITEMS; i++) {
        if (g_items[i].separator) {
            double sy = y + MENU_SEP * m->scale / 2;
            cairo_set_source_rgba(cr, m->divider[0], m->divider[1], m->divider[2], 0.55);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, MENU_PAD * m->scale + 8 * m->scale, sy);
            cairo_line_to(cr, m->w - (MENU_PAD + 8) * m->scale, sy);
            cairo_stroke(cr);
            y += MENU_SEP * m->scale;
            continue;
        }
        double rh = MENU_ROW * m->scale;
        if (i == m->hover) {
            // hover pill: the accent at 18%, rounded, inset like the Windows menu
            double inset = 3 * m->scale, rr = 5 * m->scale;
            double x0 = inset, y0 = y + 1, w0 = m->w - 2 * inset, h0 = rh - 2;
            roundedPath(cr, x0, y0, w0, h0, rr);
            cairo_set_source_rgba(cr, m->accent[0], m->accent[1], m->accent[2], 0.18);
            cairo_fill(cr);
        }
        cairo_text_extents_t ext;
        cairo_text_extents(cr, g_items[i].label, &ext);
        cairo_set_source_rgba(cr, m->fg[0], m->fg[1], m->fg[2], 1.0);
        cairo_move_to(cr, 14 * m->scale, y + (rh - ext.height) / 2 - ext.y_bearing);
        cairo_show_text(cr, g_items[i].label);
        cairo_new_path(cr);
        y += rh;
    }
    cairo_surface_flush(m->surf);
    XFlush(m->dpy);
}

// Which row is under (px, py) in window pixels, or -1.
static int menuHit(Menu *m, int px, int py) {
    double y = MENU_PAD * m->scale;
    for (int i = 0; i < NITEMS; i++) {
        if (g_items[i].separator) { y += MENU_SEP * m->scale; continue; }
        double rh = MENU_ROW * m->scale;
        if (py >= y && py < y + rh) return i;
        y += rh;
    }
    (void)px;
    return -1;
}

int menuRun(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
            int atX, int atY, double scale, int fontPx, const char *fontFamily,
            const double bg[3], double bgAlpha, const double fg[3],
            const double divider[3], const double accent[3]) {
    Menu m;
    memset(&m, 0, sizeof(m));
    m.dpy = dpy; m.visual = visual; m.cmap = cmap;
    m.scale = scale; m.fontPx = fontPx;
    snprintf(m.fontFamily, sizeof(m.fontFamily), "%s", fontFamily ? fontFamily : "sans");
    memcpy(m.bg, bg, sizeof(m.bg)); m.bgAlpha = bgAlpha;
    memcpy(m.fg, fg, sizeof(m.fg));
    memcpy(m.divider, divider, sizeof(m.divider));
    memcpy(m.accent, accent, sizeof(m.accent));
    m.hover = -1;
    m.result = 0;

    // height: every row plus the separators plus the padding
    double hh = MENU_PAD * 2;
    for (int i = 0; i < NITEMS; i++)
        hh += g_items[i].separator ? MENU_SEP : MENU_ROW;
    m.w = (int)(MENU_W * scale + 0.5);
    m.h = (int)(hh * scale + 0.5);

    // keep it on screen: flip up/left when it would fall off an edge
    int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);
    int x = atX, y = atY;
    if (x + m.w > sw) x = sw - m.w - 4;
    if (y + m.h > sh) y = atY - m.h;   // open upward when there is no room below
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.override_redirect = True;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     ButtonMotionMask | PointerMotionMask | LeaveWindowMask |
                     KeyPressMask | FocusChangeMask;
    m.win = XCreateWindow(dpy, RootWindow(dpy, screen), x, y, (unsigned)m.w, (unsigned)m.h,
                          0, depth, InputOutput, visual,
                          CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap |
                          CWEventMask, &swa);
    // shape it to the rounded rect: ONE scanline builder shared with the bar,
    // so a geometry fix lands on both (the 1 px hole needed fixing twice).
    XRectangle rects[40];
    int nr = roundedRectScanlines(rects, 40, m.w, m.h, MENU_RADIUS * scale);
    XserverRegion reg = XFixesCreateRegion(dpy, rects, nr);
    XFixesSetWindowShapeRegion(dpy, m.win, ShapeBounding, 0, 0, reg);
    XFixesDestroyRegion(dpy, reg);

    // a menu takes input so it can be clicked and Escaped; it is mapped raised
    XWMHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = InputHint;
    hints.input = True;
    XSetWMHints(dpy, m.win, &hints);
    XMapRaised(dpy, m.win);
    XSetInputFocus(dpy, m.win, RevertToParent, CurrentTime);
    // persistent paint state, created once: the menu is static, so there is
    // nothing to resize and nothing to rebuild. Building a surface plus a toy
    // font face on every hover move was an allocation per mouse pixel.
    m.surf = cairo_xlib_surface_create(dpy, m.win, visual, m.w, m.h);
    m.cr = cairo_create(m.surf);
    m.ff = cairo_toy_font_face_create(m.fontFamily, CAIRO_FONT_SLANT_NORMAL,
                                      CAIRO_FONT_WEIGHT_NORMAL);
    menuPaint(&m);

    // grab the pointer so a click outside closes the menu (the Windows behaviour)
    // A failed grab is not fatal - the menu still closes on Escape or an item
    // click - but it must not be silent, because the whole dismissal model rests
    // on these two calls.
    int pg = XGrabPointer(dpy, m.win, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                          GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    int kg = XGrabKeyboard(dpy, m.win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    if (pg != GrabSuccess || kg != GrabSuccess)
        fprintf(stderr, "chocobar: menu grab partial (pointer=%d keyboard=%d); "
                        "Escape and a click on an item still close it\n", pg, kg);

    int done = 0;
    while (!done) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
            case Expose:
                // coalesce: the content is static, so one repaint per burst is
                // enough, and painting from inside an Expose handler that itself
                // provokes exposes (via the shape) is what spun this loop before
                while (XPending(dpy)) {
                    XEvent nx;
                    XPeekEvent(dpy, &nx);
                    if (nx.type != Expose) break;
                    XNextEvent(dpy, &nx);
                }
                menuPaint(&m);
                break;
            case MotionNotify: {
                int hit = menuHit(&m, ev.xmotion.x, ev.xmotion.y);
                if (hit != m.hover) { m.hover = hit; menuPaint(&m); }
                break;
            }
            case LeaveNotify:
                if (m.hover != -1) { m.hover = -1; menuPaint(&m); }
                break;
            case ButtonPress:
                // a press outside the window (the grab delivers it) closes
                if (ev.xbutton.window != m.win) { done = 1; break; }
                if (ev.xbutton.button == 3) { done = 1; break; }
                break;
            case ButtonRelease: {
                if (ev.xbutton.window != m.win) { done = 1; break; }
                int hit = menuHit(&m, ev.xbutton.x, ev.xbutton.y);
                if (hit >= 0 && !g_items[hit].separator) { m.result = g_items[hit].id; done = 1; }
                else if (hit < 0) done = 1;   // released outside a row
                break;
            }
            case KeyPress: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_Escape) { done = 1; break; }
                if (ks == XK_Down || ks == XK_Up) {
                    int dir = (ks == XK_Down) ? 1 : -1;
                    int i = m.hover;
                    do { i += dir; if (i < 0) i = NITEMS - 1; if (i >= NITEMS) i = 0; }
                    while (g_items[i].separator && i != m.hover);
                    m.hover = i;
                    menuPaint(&m);
                } else if (ks == XK_Return || ks == XK_KP_Enter) {
                    if (m.hover >= 0 && !g_items[m.hover].separator) {
                        m.result = g_items[m.hover].id;
                        done = 1;
                    }
                }
                break;
            }
            case FocusOut:
                // Do NOT close on FocusOut. The menu is an override-redirect
                // window that XSetInputFocus just took focus for, and on this
                // session an immediate FocusOut arrives before the user has even
                // seen the menu - it closed itself one millisecond after opening.
                // The pointer grab already handles "clicked elsewhere" (that press
                // is delivered to us with a different window), so focus loss is
                // not a close condition.
                break;
            default: break;
        }
    }
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    cairo_destroy(m.cr);
    cairo_font_face_destroy(m.ff);
    cairo_surface_destroy(m.surf);
    XDestroyWindow(dpy, m.win);
    XFlush(dpy);
    return m.result;
}
