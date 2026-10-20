// linux/src/dash.c - the token dashboard, the Linux twin of the Windows board.
//
// The Windows dashboard is ~2000 lines of GDI painting inside p_ui.c. This is the
// same BOARD - stat cards for today/week/month/all-time, a daily heatmap, a
// per-app table and a per-model table - drawn with cairo, reading the SAME
// aggregates through the cbTok* accessors in p_agg.c. The numbers therefore cannot
// disagree with the Windows board about the same store; only the painting differs.
//
// It is a normal managed window (not override-redirect) on purpose: a dashboard is
// something you open, move and close, so it should behave like a window, unlike the
// bar and the menu.

#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xcursor/Xcursor.h>
#include "linux_svg.h"

#include "dash.h"
#include "linux_shape.h"

// ---- the shared aggregation (p_agg.c's portable accessors) ---------------------
extern long long cbTokAll(void);
extern long long cbTokToday(void);
extern long long cbTokWeek(void);
extern long long cbTokMonth(void);
extern int cbTokAppCount(void);
extern const char *cbTokAppName(int i);
extern long long cbTokAppSum(int i);
extern int cbTokAppReqs(int i);
extern long long cbTokAppIn(int i);
extern long long cbTokAppOut(int i);
extern long long cbTokAppCr(int i);
extern long long cbTokAppCw(int i);
extern int cbTokModelCount(void);
extern const char *cbTokModelLabel(int i);
extern long long cbTokModelSum(int i);
extern int cbTokModelReqs(int i);
extern int cbTokDayCount(void);
extern long long cbTokDayTotal(int i);
extern int cbCfgTokensEnabled(void);

typedef struct { double r, g, b; } Rgb3;

typedef struct {
    Display *dpy;
    Window win;
    Visual *visual;
    int w, h;
    double scale;
    char fontFamily[128];
    Rgb3 fg, dim, card, divider;
    double page[3];
    Rgb3 ramp[5];
    Atom wmDelete;             // WM_DELETE_WINDOW, registered on the window
} Dash;

// token counts abbreviate exactly as the bar and the Windows board do
static void fmtTok(long long n, char *out, size_t cap) {
    if (n < 0) { snprintf(out, cap, "—"); return; }
    if (n >= 1000000000LL) snprintf(out, cap, "%.2fB", n / 1e9);
    else if (n >= 1000000) snprintf(out, cap, "%.1fM", n / 1e6);
    else if (n >= 1000) snprintf(out, cap, "%.1fk", n / 1e3);
    else snprintf(out, cap, "%lld", n);
}

static void setFont(Dash *d, cairo_t *cr, double px) {
    cairo_font_face_t *ff = cairo_toy_font_face_create(d->fontFamily,
                                                      CAIRO_FONT_SLANT_NORMAL,
                                                      CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_face(cr, ff);
    cairo_font_face_destroy(ff);
    cairo_set_font_size(cr, px);
}

static void text(cairo_t *cr, double x, double y, const char *s, Rgb3 c, double a) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, s);
    cairo_new_path(cr);
}

static double textW(cairo_t *cr, const char *s) {
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

// a rounded card, the board's unit of layout. The path itself is the shared
// roundedPath() - the board's four-arc copy was the third one and the reason a
// geometry fix had to be made three times.
static void card(cairo_t *cr, double x, double y, double w, double h, double r,
                 Rgb3 fill, double fillA, Rgb3 edge) {
    roundedPath(cr, x, y, w, h, r);
    cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fillA);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, edge.r, edge.g, edge.b, 0.55);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
}

static void paint(Dash *d) {
    cairo_surface_t *surf = cairo_xlib_surface_create(d->dpy, d->win, d->visual, d->w, d->h);
    cairo_t *cr = cairo_create(surf);
    double S = d->scale;
    double pad = 18 * S;

    // page: an OPAQUE fill first. CAIRO_OPERATOR_SOURCE with an rgb colour writes
    // alpha = 1, but only if the surface has an alpha channel to write; on a 24-bit
    // visual the surface is opaque anyway, so both cases end up solid here.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, d->page[0], d->page[1], d->page[2], 1.0);
    cairo_paint(cr);
    cairo_restore(cr);

    // ---- title
    setFont(d, cr, 15 * S);
    text(cr, pad, pad + 14 * S, "Token usage", d->fg, 1.0);
    char all[32];
    fmtTok(cbTokAll(), all, sizeof(all));
    setFont(d, cr, 11 * S);
    double aw = textW(cr, all);
    text(cr, d->w - pad - aw, pad + 14 * S, all, d->dim, 1.0);

    double y = pad + 28 * S;

    // ---- stat cards: Today / Last 7 / Last 30 / All time
    struct { const char *label; long long v; } stats[4] = {
        { "Today",     cbTokToday() },
        { "Last 7",    cbTokWeek() },
        { "Last 30",   cbTokMonth() },
        { "All time",  cbTokAll() },
    };
    double cardH = 54 * S, gap = 10 * S;
    double cardW = (d->w - 2 * pad - 3 * gap) / 4;
    for (int i = 0; i < 4; i++) {
        double cx = pad + i * (cardW + gap);
        card(cr, cx, y, cardW, cardH, 8 * S, d->card, 0.85, d->divider);
        setFont(d, cr, 9.5 * S);
        text(cr, cx + 10 * S, y + 16 * S, stats[i].label, d->dim, 1.0);
        char v[32];
        fmtTok(stats[i].v, v, sizeof(v));
        setFont(d, cr, 17 * S);
        text(cr, cx + 10 * S, y + 40 * S, v, d->fg, 1.0);
    }
    y += cardH + 16 * S;

    if (!cbCfgTokensEnabled()) {
        setFont(d, cr, 11 * S);
        text(cr, pad, y + 16 * S, "Token usage is off. Set tokens.enabled to true in the config.", d->dim, 1.0);
        cairo_surface_flush(surf);
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        XFlush(d->dpy);
        return;
    }

    // ---- heatmap: the last `heatWeeks` weeks, one column per week, one row per
    // weekday, colour from the theme ramp. This is the board's signature element.
    setFont(d, cr, 10 * S);
    text(cr, pad, y + 10 * S, "DAILY", d->dim, 1.0);
    y += 18 * S;

    int days = cbTokDayCount();
    // 26 weeks, matching the Windows board. This was a `heatWeeks` parameter the
    // caller passed as a literal 26 and nothing else read, so it is now the
    // number it always was.
    int weeks = 26;
    if (weeks * 7 > days) weeks = days / 7;
    int first = days - weeks * 7;
    double cell = 11 * S, cgap = 2 * S;
    // max over the visible window, so the ramp uses the full colour range
    long long maxDay = 0;
    for (int i = first; i < days; i++) {
        long long v = cbTokDayTotal(i);
        if (v > maxDay) maxDay = v;
    }
    // weekday alignment: index `days-1` is TODAY, so a day `back` days ago has
    // weekday (todayDow - back) mod 7. Each cell is placed at its own weekday row
    // and its own week column, which is what makes the grid read as weeks.
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int todayDow = (tm.tm_wday + 6) % 7;             // Mon = 0
    for (int idx = first; idx < days; idx++) {
        int back = days - 1 - idx;
        int wd = ((todayDow - back) % 7 + 7) % 7;
        // which column: the Monday-based week that contains this day, counted
        // back from today's column. `(back + todayDow) / 7` only lands on
        // Monday boundaries when today IS a Thursday; the correct offset puts
        // the day's weekday at the START of its week.
        int weeksBack = (back + 6 - todayDow) / 7;
        int col = weeks - 1 - weeksBack;
        if (col < 0 || col >= weeks) continue;
        long long v = cbTokDayTotal(idx);
        double cx = pad + col * (cell + cgap);
        double cy = y + wd * (cell + cgap);
        int level = 0;
        if (v > 0 && maxDay > 0) {
            double frac = (double)v / (double)maxDay;
            level = 1 + (int)(frac * 3.999);          // 1..4
            if (level > 4) level = 4;
        }
        Rgb3 c = d->ramp[level];
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
        cairo_rectangle(cr, cx, cy, cell, cell);
        cairo_fill(cr);
    }
    y += 7 * (cell + cgap) + 12 * S;

    // ---- the two tables, side by side: BY APP and BY MODEL
    double colGap = 14 * S;
    double colW = (d->w - 2 * pad - colGap) / 2;
    double tableY = y;
    double rowH = 20 * S;

    setFont(d, cr, 10 * S);
    text(cr, pad, tableY + 10 * S, "BY APP", d->dim, 1.0);
    text(cr, pad + colW + colGap, tableY + 10 * S, "BY MODEL", d->dim, 1.0);
    double ty = tableY + 20 * S;

    int napp = cbTokAppCount();
    if (napp > 10) napp = 10;
    for (int i = 0; i < napp && ty + rowH < d->h - pad; i++) {
        char v[32];
        fmtTok(cbTokAppSum(i), v, sizeof(v));
        setFont(d, cr, 11 * S);
        text(cr, pad, ty + 13 * S, cbTokAppName(i), d->fg, 1.0);
        setFont(d, cr, 11 * S);
        double vw = textW(cr, v);
        text(cr, pad + colW - vw, ty + 13 * S, v, d->fg, 1.0);
        // a hairline under each row, like the Windows table
        cairo_set_source_rgba(cr, d->divider.r, d->divider.g, d->divider.b, 0.35);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, pad, ty + rowH - 1);
        cairo_line_to(cr, pad + colW, ty + rowH - 1);
        cairo_stroke(cr);
        ty += rowH;
    }

    int nmodel = cbTokModelCount();
    if (nmodel > 10) nmodel = 10;
    double my = tableY + 20 * S;
    for (int i = 0; i < nmodel && my + rowH < d->h - pad; i++) {
        char v[32];
        fmtTok(cbTokModelSum(i), v, sizeof(v));
        setFont(d, cr, 11 * S);
        text(cr, pad + colW + colGap, my + 13 * S, cbTokModelLabel(i), d->fg, 1.0);
        double vw = textW(cr, v);
        text(cr, pad + colW + colGap + colW - vw, my + 13 * S, v, d->fg, 1.0);
        cairo_set_source_rgba(cr, d->divider.r, d->divider.g, d->divider.b, 0.35);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, pad + colW + colGap, my + rowH - 1);
        cairo_line_to(cr, pad + colW + colGap + colW, my + rowH - 1);
        cairo_stroke(cr);
        my += rowH;
    }

    // a footer with the last refresh, so a stale board is recognisable
    char foot[96];
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(foot, sizeof(foot), "updated %02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    setFont(d, cr, 9 * S);
    double fw = textW(cr, foot);
    text(cr, d->w - pad - fw, d->h - pad + 4 * S, foot, d->dim, 0.9);

    cairo_surface_flush(surf);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    XFlush(d->dpy);
}

// The board's state, created by dashOpen and driven by the bar's loop. One
// board at a time: it is opened from the menu and closed by Esc, a click or
// the WM's close button.
static Dash g_dashBoard;
static int g_boardOpen = 0;

Window dashOpen(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
                int cssW, int cssH, double scale, const char *fontFamily,
                const double page[3], const double card[3], const double fg[3],
                const double dim[3], const double divider[3], const double ramp[5][3]) {
    Dash *d = &g_dashBoard;
    memset(d, 0, sizeof(*d));
    d->dpy = dpy; d->visual = visual;
    d->scale = scale;
    snprintf(d->fontFamily, sizeof(d->fontFamily), "%s", fontFamily ? fontFamily : "sans");
    memcpy(d->page, page, sizeof(d->page));
    d->card = (Rgb3){ card[0], card[1], card[2] };
    d->fg = (Rgb3){ fg[0], fg[1], fg[2] };
    d->dim = (Rgb3){ dim[0], dim[1], dim[2] };
    d->divider = (Rgb3){ divider[0], divider[1], divider[2] };
    for (int i = 0; i < 5; i++) d->ramp[i] = (Rgb3){ ramp[i][0], ramp[i][1], ramp[i][2] };
    d->w = (int)(cssW * scale + 0.5);
    d->h = (int)(cssH * scale + 0.5);

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                     PointerMotionMask | StructureNotifyMask;
    d->win = XCreateWindow(dpy, RootWindow(dpy, screen), 60, 60,
                          (unsigned)d->w, (unsigned)d->h, 0, depth, InputOutput, visual,
                          CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &swa);
    if (!d->win) return None;
    XStoreName(dpy, d->win, "Chocobar - Token usage");
    // WM_DELETE_WINDOW: without it a client message is never sent and KWin falls
    // back to XKillClient, which severs the SHARED connection - the dashboard
    // using the bar's Display means its close button would take the whole bar
    // process down with it.
    {
        Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy, d->win, &wmDelete, 1);
        d->wmDelete = wmDelete;
    }
    // The dashboard's own cursor, at the desktop's scaled size: without it the
    // pointer over the board shows Xwayland's root cursor at the raw
    // XCURSOR_SIZE and shrinks against the desktop's scaled one (the bar fixed
    // the same thing; one shared definition of the size, not two).
    {
        XcursorImage *img = XcursorLibraryLoadImage("left_ptr", NULL,
                                                    (int)(cbCursorBaseSize() * cbCursorScale() + 0.5));
        if (img) {
            Cursor c = XcursorImageLoadCursor(dpy, img);
            XDefineCursor(dpy, d->win, c);
            XFreeCursor(dpy, c);
            XcursorImageDestroy(img);
        }
    }
    XSizeHints sh;
    memset(&sh, 0, sizeof(sh));
    sh.flags = PMinSize;
    sh.min_width = (int)(520 * scale);
    sh.min_height = (int)(380 * scale);
    XSetWMNormalHints(dpy, d->win, &sh);
    XMapWindow(dpy, d->win);
    paint(d);

    g_boardOpen = 1;
    return d->win;
}

Window dashWin(void) {
    return g_boardOpen ? g_dashBoard.win : None;
}

// The board wants to close: Esc, q, a non-left click, the WM's close button, or
// the window already being gone. Returns 1 and lets the caller call dashClose().
static int boardClose(XEvent *ev) {
    Dash *d = &g_dashBoard;
    switch (ev->type) {
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev->xkey, 0);
            return ks == XK_Escape || ks == XK_q;
        }
        case ButtonPress:
            // a middle/right click also closes the window
            return ev->xbutton.button != 1;
        case ClientMessage:
            // the window manager's close button (see WM_DELETE_WINDOW above)
            return (Atom)ev->xclient.data.l[0] == d->wmDelete;
        case DestroyNotify:
            g_boardOpen = 0;   // the window is already gone; do not touch it
            return 1;
        default: return 0;
    }
}

int dashEvent(XEvent *ev) {
    if (!g_boardOpen) return 0;
    Dash *d = &g_dashBoard;
    if (boardClose(ev)) return 1;
    switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) paint(d);
            break;
        case ConfigureNotify:
            if (ev->xconfigure.width != d->w || ev->xconfigure.height != d->h) {
                d->w = ev->xconfigure.width;
                d->h = ev->xconfigure.height;
                paint(d);
            }
            break;
        default: break;
    }
    return 0;
}

void dashClose(void) {
    if (!g_boardOpen) return;
    Dash *d = &g_dashBoard;
    g_boardOpen = 0;
    // DestroyNotify may have already taken the window; XDestroyWindow on a dead
    // window only raises BadWindow, which the error handler ignores.
    XDestroyWindow(d->dpy, d->win);
    XFlush(d->dpy);
    // the bar's own surface is still valid; the caller repaints it
}
