// linux/src/tooltip.c - the tooltip surface, the Linux twin of p_ui.c's tipShow.
//
// Two looks, both from the Windows bar:
//   - the CHIP tooltip (dark = 0): a browser-style white box with a hairline
//     gray border and near-black text at 12 CSS px. p_ui.c draws exactly that
//     (tipShow's !dark branch: white fill, RGB(200,200,200) 1px border,
//     RGB(31,31,31) text, Segoe UI 12) and every chip in the Windows bar gets
//     one: "CPU usage", "GPU usage", "Memory usage", "Volume", "Battery",
//     "Local time", "CPU temperature", "Token usage today", the subscription
//     plan's "<plan> <window> N% left", and the shortcut's "Run: <command>".
//     The font family is the bar's configured family on Windows only for the
//     DARK card; the light tooltip is deliberately Segoe UI, the system UI font,
//     so the Linux bar asks for "" (the system sans) here for the same reason.
//   - the HEATMAP card (dark = 1): the board's #heat-tip - a dark rounded card
//     (RGB 36,31,22), first line bold in pink (246,216,224), following lines in
//     a pale cream (245,240,216), 10 CSS px, 15 px line height, 9x6 padding.
//
// Placement matches p_ui.c: 6 CSS px right of and 16 CSS px below the pointer,
// flipped to the left/up when it would leave the screen, then clamped. It is
// override-redirect so it never takes focus, and it is mapped with
// XMapRaised + XRaiseWindow so it draws above the bar.

#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

#include "tooltip.h"

// the chip tooltip's own look (p_ui.c tipShow, !dark): a system-UI box, NOT the
// bar's configured display family - the Windows bar hardcodes Segoe UI here.
#define TIP_UI_FAMILY ""
#define TIP_UI_PX 12.0
#define TIP_PAD_X 7.0     // p_ui.c padX for the light tip
#define TIP_PAD_Y 4.0     // p_ui.c padY for the light tip
#define TIP_BORDER 1      // the 1px gray hairline

// the heat card's look (p_ui.c tipShow, dark; dash.css #heat-tip)
#define TIP_HEAT_PX 10.0
#define TIP_HEAT_LINE 15.0
#define TIP_HEAT_PAD_X 9.0
#define TIP_HEAT_PAD_Y 6.0
#define TIP_HEAT_RADIUS 6.0
#define TIP_HEAT_BG_R 0x24 / 255.0, 0x1F / 255.0, 0x16 / 255.0
#define TIP_HEAT_FG1 0xF6 / 255.0, 0xD8 / 255.0, 0xE0 / 255.0
#define TIP_HEAT_FG2 0xF5 / 255.0, 0xF0 / 255.0, 0xD8 / 255.0

#define TIP_MAX_LINES 8
#define TIP_MAX_LINE 200

typedef struct {
    Display *dpy;
    int screen;
    Visual *visual;
    Colormap cmap;
    int depth;
    double scale;
    Window win;
    int w, h;                 // current device size
    int shown;
} Tip;

static Tip g_tip;

void tipInit(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
             double scale) {
    g_tip.dpy = dpy;
    g_tip.screen = screen;
    g_tip.visual = visual;
    g_tip.cmap = cmap;
    g_tip.depth = depth;
    g_tip.scale = scale > 0 ? scale : 1.0;
}

Window tipWin(void) {
    return g_tip.win;
}

// Split `text` into at most TIP_MAX_LINES lines, in place on a copy.
// A line longer than TIP_MAX_LINE characters is truncated (the Windows tip
// would simply be wide; a 200-char line already exceeds any sane screen).
static int splitLines(const char *text, char lines[TIP_MAX_LINES][TIP_MAX_LINE]) {
    int n = 0;
    const char *p = text;
    while (p && *p && n < TIP_MAX_LINES) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > TIP_MAX_LINE - 1) len = TIP_MAX_LINE - 1;
        memcpy(lines[n], p, len);
        lines[n][len] = 0;
        n++;
        p = nl ? nl + 1 : NULL;
    }
    return n;
}

// Measure one line in the given face/size, in device px.
static double lineWidth(cairo_t *cr, cairo_font_face_t *ff, double px, const char *s) {
    cairo_set_font_face(cr, ff);
    cairo_set_font_size(cr, px);
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

static void roundedPath(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void tipShow(const char *text, int cx, int cy, const char *family, double cssPx,
             int dark) {
    if (!g_tip.dpy) return;
    if (!text || !*text) { tipHide(); return; }

    char lines[TIP_MAX_LINES][TIP_MAX_LINE];
    int n = splitLines(text, lines);
    if (n <= 0) { tipHide(); return; }

    double S = g_tip.scale;
    double px = (dark ? TIP_HEAT_PX : (cssPx > 0 ? cssPx : TIP_UI_PX)) * S;
    double padX = (dark ? TIP_HEAT_PAD_X : TIP_PAD_X) * S;
    double padY = (dark ? TIP_HEAT_PAD_Y : TIP_PAD_Y) * S;
    double lineH = (dark ? TIP_HEAT_LINE : 15.0) * S;

    // fonts: the light tip is the system UI face, the dark card the bar's family
    const char *fam = dark ? (family && *family ? family : "sans") : TIP_UI_FAMILY;
    cairo_font_face_t *fReg = cairo_toy_font_face_create(fam, CAIRO_FONT_SLANT_NORMAL,
                                                         CAIRO_FONT_WEIGHT_NORMAL);
    cairo_font_face_t *fBold = cairo_toy_font_face_create(fam, CAIRO_FONT_SLANT_NORMAL,
                                                          CAIRO_FONT_WEIGHT_BOLD);

    // measure on a throwaway image surface: no window needed to size the box
    cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *mc = cairo_create(ms);
    double maxW = 0;
    for (int i = 0; i < n; i++) {
        double w = lineWidth(mc, (dark && i == 0) ? fBold : fReg, px, lines[i]);
        if (w > maxW) maxW = w;
    }
    cairo_destroy(mc);
    cairo_surface_destroy(ms);

    int w = (int)(maxW + 2 * padX + (dark ? 0 : 2 * TIP_BORDER) + 0.5);
    int h = (int)(n * lineH + 2 * padY + (dark ? 0 : 2 * TIP_BORDER) + 0.5);
    if (w < 8) w = 8;
    if (h < 8) h = 8;

    if (!g_tip.win) {
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.override_redirect = True;
        swa.background_pixel = 0;
        swa.border_pixel = 0;
        swa.colormap = g_tip.cmap;
        swa.event_mask = ExposureMask;
        g_tip.win = XCreateWindow(g_tip.dpy, RootWindow(g_tip.dpy, g_tip.screen),
                                  0, 0, (unsigned)w, (unsigned)h, 0, g_tip.depth,
                                  InputOutput, g_tip.visual,
                                  CWOverrideRedirect | CWBackPixel | CWBorderPixel |
                                  CWColormap | CWEventMask, &swa);
        if (!g_tip.win) {
            cairo_font_face_destroy(fReg);
            cairo_font_face_destroy(fBold);
            return;
        }
        // never accept input: a tooltip under the pointer must not eat a click.
        // An empty event mask is NOT enough - the tip is placed 6 px right and
        // 16 px BELOW the pointer, so the pointer's next move lands on the tip
        // itself; with no input shape the X server delivers that motion to the
        // tip (which ignores it) and the window UNDERNEATH never sees it, which
        // froze the heatmap card the moment the pointer entered the card. An
        // EMPTY INPUT SHAPE is the X11 click-through: events pass through to
        // the board below, exactly as the WS_EX_TRANSPARENT Windows tooltip
        // does. The shape is window-relative and empty, so it survives every
        // resize without re-setting.
        XSetWindowAttributes ia;
        memset(&ia, 0, sizeof(ia));
        ia.event_mask = 0;
        XChangeWindowAttributes(g_tip.dpy, g_tip.win, CWEventMask, &ia);
        {
            XserverRegion empty = XFixesCreateRegion(g_tip.dpy, NULL, 0);
            if (empty) {
                XFixesSetWindowShapeRegion(g_tip.dpy, g_tip.win, ShapeInput, 0, 0, empty);
                XFixesDestroyRegion(g_tip.dpy, empty);
            }
        }
        // the tip must not be a taskbar/Alt-Tab citizen
        {
            XClassHint *ch = XAllocClassHint();
            if (ch) {
                ch->res_name = (char *)"chocobar-tip";
                ch->res_class = (char *)"ChocobarTip";
                XSetClassHint(g_tip.dpy, g_tip.win, ch);
                XFree(ch);
            }
        }
    } else {
        XResizeWindow(g_tip.dpy, g_tip.win, (unsigned)w, (unsigned)h);
    }
    g_tip.w = w;
    g_tip.h = h;

    // placement: 6 px right / 16 px below the pointer, flipped and clamped so
    // the box always lands on the screen (p_ui.c tipShow's tail)
    int sw = DisplayWidth(g_tip.dpy, g_tip.screen);
    int sh = DisplayHeight(g_tip.dpy, g_tip.screen);
    int x = cx + (int)(6 * S);
    int y = cy + (int)(16 * S);
    if (x + w > sw) x = cx - w - (int)(6 * S);
    if (y + h > sh) y = cy - h - (int)(14 * S);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    // place and MAP first, then paint: a pre-map RENDER draw on a 32-bit ARGB
    // window never lands (measured on Xvfb - the painted-then-mapped tip came
    // up black, the mapped-then-painted one keeps every pixel; the bar only
    // works because its Expose-driven repaints happen after the map). The tip
    // paints once per show, so its paint must follow the map.
    XMoveResizeWindow(g_tip.dpy, g_tip.win, x, y, (unsigned)w, (unsigned)h);
    XMapRaised(g_tip.dpy, g_tip.win);
    // paint
    cairo_surface_t *surf = cairo_xlib_surface_create(g_tip.dpy, g_tip.win,
                                                      g_tip.visual, w, h);
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (dark) {
        // the board's dark card: rounded, no border
        roundedPath(cr, 0, 0, w, h, TIP_HEAT_RADIUS * S);
        cairo_set_source_rgba(cr, TIP_HEAT_BG_R, 1.0);
        cairo_fill(cr);
        for (int i = 0; i < n; i++) {
            cairo_set_font_face(cr, (i == 0) ? fBold : fReg);
            cairo_set_font_size(cr, px);
            if (i == 0) cairo_set_source_rgba(cr, TIP_HEAT_FG1, 1.0);
            else cairo_set_source_rgba(cr, TIP_HEAT_FG2, 1.0);
            // baseline inside the line box, like DrawText's single line
            cairo_move_to(cr, padX, padY + i * lineH + px * 0.80);
            cairo_show_text(cr, lines[i]);
        }
    } else {
        // opaque white box with a gray hairline (p_ui.c's light tooltip)
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0xC8 / 255.0, 0xC8 / 255.0, 0xC8 / 255.0);
        cairo_set_line_width(cr, TIP_BORDER);
        cairo_rectangle(cr, TIP_BORDER / 2.0, TIP_BORDER / 2.0,
                        w - TIP_BORDER, h - TIP_BORDER);
        cairo_stroke(cr);
        cairo_set_font_face(cr, fReg);
        cairo_set_font_size(cr, px);
        cairo_set_source_rgb(cr, 31 / 255.0, 31 / 255.0, 31 / 255.0);
        for (int i = 0; i < n; i++) {
            cairo_move_to(cr, TIP_BORDER + padX, TIP_BORDER + padY + i * lineH + px * 0.80);
            cairo_show_text(cr, lines[i]);
        }
    }
    cairo_surface_flush(surf);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    cairo_font_face_destroy(fReg);
    cairo_font_face_destroy(fBold);

    XRaiseWindow(g_tip.dpy, g_tip.win);
    XFlush(g_tip.dpy);
    g_tip.shown = 1;
}

void tipHide(void) {
    if (!g_tip.dpy || !g_tip.win || !g_tip.shown) return;
    XUnmapWindow(g_tip.dpy, g_tip.win);
    XFlush(g_tip.dpy);
    g_tip.shown = 0;
}

void tipFree(void) {
    if (!g_tip.dpy) return;
    if (g_tip.win) {
        XDestroyWindow(g_tip.dpy, g_tip.win);
        XFlush(g_tip.dpy);
    }
    memset(&g_tip, 0, sizeof(g_tip));
}
