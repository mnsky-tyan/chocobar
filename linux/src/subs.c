// subs.c - the subscription board window, the Linux twin of p_ui.c's
// g_dashType == 1 branch of paintDash.
//
// WHY IT EXISTS: the Linux bar shipped the token dashboard but left the
// subscription board behind (the menu printed "not ported to Linux yet"), so a
// Linux user could see the gauge chip's number but never the board that names
// each pool, its reset time and its credit totals. This closes that gap: same
// layout constants, same per-window donut + meta text, same status pill and
// footer stamp, reading the SAME records through p_subs.c's cbSubs* seam.
//
// SHARED-CORE RULE: the numbers come from the shared core
// (p_subs.c), which fetches them on its own thread exactly as on Windows. This
// file only paints; it never re-fetches or re-derives anything. Where the
// Windows branch formats something (the uppercased window label, the relative
// reset countdown), the helper is ported here from p_ui.c and kept identical;
// the credit tiers are shared instead of copied - fmtNum calls cbSubsFmtNum,
// the seam to p_utils.c's fmtNum, so the two boards cannot disagree.
//
// The window is driven by the BAR's loop like the token board: one connection,
// two windows, one pump.
#include "subs.h"

#include <X11/Xatom.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo-xlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "linux_shape.h"
#include "linux_text.h"
#include "linux_svg.h"
#include "svg_geometry.h"
#include "chocobar_core.h"

// general.debug through the shared core's config seam (p_agg.c), the same
// key the Windows board gates its panels-fit diagnostic on
extern int cbCfgDebug(void);

// The board's window corner radius is BOARD_RADIUS_CSS from linux_shape.h:
// Windows 11's DWMWCP_ROUND_NATIVE for a normal window, one definition for both
// boards.

#define SUBS_HM_MAX 8 // MAX_SUBS in the shared core; a board never shows more

// The board's minimum, CSS px: the PMinSize hint and fitToContent's floor are
// the same number, so no fit ever requests a height the WM would constrain
// back up against the hint the window itself declares.
#define SUBS_MIN_CSS 280

typedef struct { double r, g, b; } Rgb3;

typedef struct {
    Display *dpy;
    Window win;
    Visual *visual;
    int w, h;
    double scale;
    char fontFamily[128];
    Rgb3 bg, card, head, fg, dim, divider, pinkDeep, pink, warn, good, yellow;
    Rgb3 iconCol;
    Rgb3 ramp[5];
    cairo_font_face_t *ffSymbol;   // the close glyph's face (U+2715, see below)
    int contentH;              // painted content bottom (device px) -> content fit
    int x, y;                  // window position (content fit keeps it on screen)
    int cfgH;                  // the CONFIG height in CSS px
    /* persistent paint state: the board repaints on every hover move */
    cairo_surface_t *surf;
    cairo_t *cr;
    cairo_font_face_t *ffReg, *ffBold;
    XserverRegion shape;       // the window's rounded shape (DWM round-native twin)
    int shapeW, shapeH, shapeR;   // the geometry that region was built for
    /* titlebar buttons (device px), hover state: the Windows board draws
     * [refresh] [close] on BOTH boards (p_ui.c), so this one does too */
    int btnRefreshL, btnRefreshR, btnRefreshT, btnRefreshB;
    int btnCloseL, btnCloseR, btnCloseT, btnCloseB;
    int btnHover;              // 0 none, 1 refresh, 2 close
    int busy;                  // refresh in flight: the button reads "..."
    long long busyUntilMs;     // the 12 s click debounce floor (p_ui.c)
    long long busyEpoch;       // the epoch the refresh started from
    int tbBottom;              // titlebar bottom (device px)
    int dragging;              // 1 = a title-band Button1 drag is in progress
    int dragDx, dragDy;        // pointer offset from the window's top-left
} Subs;

static Subs g_subs;
static int g_open = 0;
// the window that held the X keyboard focus when the board took it, restored
// on close (same pattern as the token board's g_focusPrev in dash.c)
static Window g_focusPrev = None;

// ---- small helpers ---------------------------------------------------------------

static void paint(Subs *d);

// p_utils.c fmtNum, through the shared core's seam: M and k tiers only (no B),
// values rounded. The Windows board prints the credits with these tiers, so the
// port must too - and one formatter means the two cannot disagree.
static void fmtNum(long long n, char *out, size_t cap) {
    wchar_t w[32];
    cbSubsFmtNum(n, w, 32);
    wideToUtf8(w, out, cap);
}

// p_ui.c subsWinUpper: the panel head shows the window label uppercased.
// Uppercase the label and convert it to UTF-8. The label is user text from the
// shared core, not a byte string, so it goes through the shared converter every
// other wide string the bar paints uses.
static void winUpper(const wchar_t *label, char *out, size_t cap) {
    wchar_t tmp[64];
    size_t i = 0;
    for (; label[i] && i + 1 < 64; i++) {
        wchar_t ch = label[i];
        tmp[i] = (ch >= L'a' && ch <= L'z') ? ch - 32 : ch;
    }
    tmp[i] = 0;
    wideToUtf8(tmp, out, cap);
}

// p_ui.c fmtReset: relative countdown to the window reset. resetAt is a true
// UTC epoch in ms (as Date.now() is), 0 = unknown.
static void fmtReset(long long ms, char *out, size_t cap) {
    if (ms <= 0) { snprintf(out, cap, "reset \u2014"); return; }
    long long now = (long long)time(NULL) * 1000LL;
    long long delta = ms - now;
    if (delta <= 0) { snprintf(out, cap, "reset now"); return; }
    long long h = delta / 3600000LL, m = (delta % 3600000LL) / 60000LL, d = h / 24;
    if (d >= 1) snprintf(out, cap, "reset in %lldd %lldh", d, h % 24);
    else if (h >= 1) snprintf(out, cap, "reset in %lldh %lldm", h, m);
    else snprintf(out, cap, "reset in %lldm", m);
}

static void setFont(Subs *d, cairo_t *cr, double cssPx, int bold) {
    cairo_set_font_face(cr, bold ? d->ffBold : d->ffReg);
    cairo_set_font_size(cr, cssPx * d->scale);
}

static void text(cairo_t *cr, double x, double y, const char *s, Rgb3 c, double a) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, s);
    cairo_new_path(cr);
}

static void textR(cairo_t *cr, double x, double y, const char *s, Rgb3 c, double a) {
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    text(cr, x - e.x_advance, y, s, c, a);
}

static double textW(cairo_t *cr, const char *s) {
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

static void hairline(cairo_t *cr, double x0, double x1, double y, Rgb3 c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x0, y);
    cairo_line_to(cr, x1, y);
    cairo_stroke(cr);
}

// The card: a rounded rect with a translucent fill and a hairline edge. The
// path itself is the shared roundedPath() - the board's four-arc copy was the
// third copy of the same geometry, so it is shared like the other two.
static void card(Subs *d, cairo_t *cr, double x, double y, double w, double h, double r,
                 Rgb3 fill, double alpha, Rgb3 line) {
    (void)d;
    roundedPath(cr, x, y, w, h, r);
    if (alpha > 0) {
        cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, alpha);
        cairo_fill_preserve(cr);
    }
    if (line.r >= 0) {
        cairo_set_source_rgba(cr, line.r, line.g, line.b, 1.0);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    } else {
        cairo_new_path(cr);
    }
}

// A small filled dot, the provider's identity colour (the heatmap ramp entry).
static void dot(cairo_t *cr, double cx, double cy, double r, Rgb3 c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);
}

// A dashed line, the Windows PS_DOT pen: the panel head/foot underlines.
static void dotted(cairo_t *cr, double x0, double x1, double y, Rgb3 c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
    cairo_set_line_width(cr, 1.0);
    double dashes[] = { 2.0, 3.0 };
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_move_to(cr, x0, y);
    cairo_line_to(cr, x1, y);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
}

// blend: two opaque colours, num/256 of the way toward the second - num is
// p_ui.c blendCr's 0-255 scale (31 is a 12% tint), not a percentage. The subs
// board's pill background and donut track carry p_ui.c's 31/20 constants.
static Rgb3 blend(Rgb3 a, Rgb3 b, int num) {
    double t = (double)num / 256.0;
    Rgb3 o = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
    return o;
}

// A donut: the track is a full circle, the arc sweeps clockwise from 12 o'clock
// (rotate -90, rem * 3.6 degrees). GDI+ stroked an ellipse; cairo's arc is the
// same geometry.
static void donut(cairo_t *cr, double cx, double cy, double r, double penW,
                  Rgb3 track, Rgb3 arc, int remPct) {
    cairo_set_line_width(cr, penW);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_source_rgba(cr, track.r, track.g, track.b, 1.0);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);
    if (remPct > 0) {
        double a0 = -M_PI / 2.0;
        double a1 = a0 + (double)remPct * 2.0 * M_PI / 100.0;
        cairo_set_source_rgba(cr, arc.r, arc.g, arc.b, 1.0);
        cairo_arc(cr, cx, cy, r, a0, a1);
        cairo_stroke(cr);
    }
}

// ---- window shape ----------------------------------------------------------------

// Rebuild the window shape from the current size (dashRoundCorners twin).
static void reshape(Subs *d) {
    // the shared helper rebuilds the region whenever the built geometry (w, h,
    // r) changes - this board fits its content on open AND on every repaint
    // that changes the content height, so a radius-only guard kept the
    // open-time region and clipped the panels that grew past it
    boardShapeApply(d->dpy, d->win, d->w, d->h, BOARD_RADIUS_CSS, d->scale,
                    &d->shape, &d->shapeW, &d->shapeH, &d->shapeR);
}

// ---- content fit (dashFitToContent) ----------------------------------------------
// Same contract as the token board: the painted content's height, clamped to
// 45% of the configured height and to the screen minus a margin, with the board
// moved up so it never runs past the bottom edge. Returns 1 when the window was
// resized (the surface is new, so the caller repaints), 0 when it already fit.
static int fitToContent(void) {
    Subs *d = &g_subs;
    if (!d->win || d->contentH <= 0) return 0;
    int want = d->contentH;
    int minH = (int)(SUBS_MIN_CSS * d->scale);
    int lo = (int)(d->cfgH * d->scale * 45 / 100);
    if (lo < minH) lo = minH;
    if (want < lo) want = lo;
    // The fit moves the window only when the height actually changes (p_ui.c's
    // dashFitToContent contract, and dash.c's fit now runs the same shape): a
    // repaint whose content height is unchanged must not touch the window at
    // all, so a board the user dragged partly off the work area STAYS there
    // across every hover and fetch repaint. The height below is limited to the
    // room at the kept position, so a resize can never hang past the head's
    // bottom edge either.
    int hi = 0;
    if (d->dpy) {
        int scr = DefaultScreen(d->dpy);
        // the head's ABSOLUTE work-area bottom (the board's top edge is
        // y-aware, so the clamp has to be too)
        hi = DisplayHeight(d->dpy, scr) - 40;
        int n = 0;
        XineramaScreenInfo *xi = XineramaQueryScreens(d->dpy, &n);
        if (xi && n > 0) {
            for (int i = 0; i < n; i++) {
                if (xi[i].x_org <= d->x && d->x < xi[i].x_org + xi[i].width &&
                    xi[i].y_org <= d->y && d->y < xi[i].y_org + xi[i].height) {
                    hi = xi[i].y_org + xi[i].height - 40;
                    break;
                }
            }
            XFree(xi);
        }
    }
    if (hi <= 0) return 0;
    // p_ui.c moves the window UP to the head's work-area bottom before sizing
    // (ny = rcWork.bottom - want, floored at the top), so the FULL content
    // height stays visible even when the user dragged the board's top edge
    // down to the floor: the old room-at-the-kept-position clamp instead
    // shrank a dragged-down board to its minimum-height sliver on the next
    // fetch and left the lower panels off-screen. Same order as dash.c: the
    // move-up MUST precede the height clamp.
    if (d->y + want > hi) {
        d->y = hi - want;
        if (d->y < 0) d->y = 0;
    }
    int room = hi - d->y;
    if (room <= 0) return 0;
    if (want > room) want = room;
    if (d->h == want) return 0;
    d->h = want;
    if (d->surf) {
        cairo_destroy(d->cr);
        cairo_surface_destroy(d->surf);
        d->surf = cairo_xlib_surface_create(d->dpy, d->win, d->visual, d->w, d->h);
        d->cr = cairo_create(d->surf);
    }
    XMoveResizeWindow(d->dpy, d->win, d->x, d->y, (unsigned)d->w, (unsigned)d->h);
    reshape(d);
    return 1;
}

// Paint the live board and then latch it to the height it just painted. The
// panels' stack IS the content, and it changes while the board is up - a fetch
// landing, a config reload enabling another provider - so every live paint goes
// through here; fitting only on open left the extra panels clipped in a window
// that could never grow. The fit must stay OUTSIDE paint(), which would destroy
// the context mid-draw.
static void paintAndFit(Subs *d) {
    paint(d);
    cairo_surface_flush(d->surf);
    XFlush(d->dpy);
    if (fitToContent()) {
        paint(d);
        cairo_surface_flush(d->surf);
        XFlush(d->dpy);
    }
}

// the titlebar button under (x, y): 1 refresh, 2 close, 0 none
static int buttonAt(Subs *d, int x, int y) {
    if (x >= d->btnRefreshL && x < d->btnRefreshR &&
        y >= d->btnRefreshT && y < d->btnRefreshB) return 1;
    if (x >= d->btnCloseL && x < d->btnCloseR &&
        y >= d->btnCloseT && y < d->btnCloseB) return 2;
    return 0;
}

// GDI draws text from the TOP of the line box; cairo from the BASELINE. The
// Windows coordinates this file ports are GDI tops, so every text() call here
// goes through this: baseline = gdiTop + the current font's ascent (the same
// conversion dash.c's centring code makes with cairo_font_extents).
static double bl(cairo_t *cr, double gdiTop) {
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    return gdiTop + fe.ascent;
}

static long long nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- paint ------------------------------------------------------------------------

static void paint(Subs *d) {
    double S = d->scale;
    cairo_t *cr = d->cr;

    // the busy "..." clears when the fetch lands (the epoch moved) or the 12 s
    // click-debounce floor expires - the same rule p_ui.c's WM_TIMER applies
    // (g_btnBusyUntil / g_subsBusyEpoch). Without this the button read "..."
    // forever after one refresh click.
    if (d->busy) {
        long long ep = cbSubsFetchedEpochMs();
        if (ep != d->busyEpoch || nowMs() >= d->busyUntilMs) d->busy = 0;
    }

    // page: opaque inside the rounded shape, transparent corners
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, d->bg.r, d->bg.g, d->bg.b, 1.0);
    roundedPath(cr, 0, 0, d->w, d->h, BOARD_RADIUS_CSS * d->scale);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_new_path(cr);

    double padL = 18 * S, padT = 14 * S, gap = 12 * S;
    double y = padT;

    // ---- titlebar: gauge logo, "Subscriptions", close button, divider
    {
        double logoBox = 14 * S;
        // the icon colour, the same key the bar's icons use (p_ui.c icol)
        Rgb3 acc = d->iconCol;
        // p_ui.c draws SVG_GAUGE for the subscription board (SVG_DIAMOND is the
        // token board's logo); the accent is the icon colour, not always pink
        svgDrawCairo(cr, SVG_GAUGE, padL, y + (20 * S - logoBox) / 2.0, logoBox,
                     acc.r, acc.g, acc.b, 1.0);
        setFont(d, cr, 15, 1);
        text(cr, padL + logoBox + 10 * S, y + 12 * S, "Subscriptions", d->fg, 1.0);
        // buttons right: [refresh] [close], the same pair the token board's
        // titlebar draws (p_ui.c puts them on both boards) - refresh re-fetches
        // the providers, close closes the board
        double bx = d->w - padL;
        setFont(d, cr, 11, 0);
        const char *cl = "\u2715";
        double cw = textW(cr, cl) + 18 * S;
        d->btnCloseR = (int)bx;
        d->btnCloseL = (int)(bx - cw);
        d->btnCloseT = (int)(y - 3 * S);
        d->btnCloseB = (int)(y + 11 * S + 6 * S);
        bx -= cw + 6 * S;
        const char *rf = d->busy ? "..." : "refresh";
        double rw = textW(cr, "refresh") + 18 * S;
        d->btnRefreshR = (int)bx;
        d->btnRefreshL = (int)(bx - rw);
        d->btnRefreshT = (int)(y - 3 * S);
        d->btnRefreshB = (int)(y + 11 * S + 6 * S);

        // centre each label in its frame on both axes (the Windows comment: the
        // ink block, not the glyph box, is what the eye centres)
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        for (int bi = 1; bi <= 2; bi++) {
            int l = bi == 1 ? d->btnRefreshL : d->btnCloseL;
            int r = bi == 1 ? d->btnRefreshR : d->btnCloseR;
            int t = bi == 1 ? d->btnRefreshT : d->btnCloseT;
            int b = bi == 1 ? d->btnRefreshB : d->btnCloseB;
            const char *tx = bi == 1 ? rf : cl;
            // the cross comes from the SYMBOL face: the display family has no
            // U+2715 and cairo does not fall back
            if (bi == 2) cairo_set_font_face(cr, d->ffSymbol);
            double txOff = ((r - l) - textW(cr, tx)) / 2.0;
            double tyOff = (b - t) / 2.0 + (fe.ascent - fe.descent) / 2.0;
            if (d->btnHover == bi) {
                card(d, cr, l, t, r - l, b - t, 10 * S, d->bg, 1.0, d->pink);
                text(cr, l + txOff, t + tyOff, tx, d->pinkDeep, 1.0);
            } else {
                text(cr, l + txOff, t + tyOff, tx, d->dim, 1.0);
            }
        }
        d->tbBottom = (int)(d->btnRefreshB + 10 * S + 1);
        hairline(cr, padL, d->w - padL, d->tbBottom, d->divider);
        y = d->tbBottom + gap;
    }

    double innerW = d->w - 2 * padL;

    // ---- one full-width panel per enabled provider
    int pn = cbSubsProviderCount();
    if (pn > SUBS_HM_MAX) pn = SUBS_HM_MAX;
    int en = 0;
    if (cbSubsEnabled())
        for (int i = 0; i < pn; i++)
            if (cbSubsProvEnabled(i)) en++;

    int headH = (int)(40 * S), footH = (int)(26 * S), bodyH = (int)(112 * S);
    // compress (body first, then foot, then head) until every panel fits the
    // screen; the floors keep a 5-panel board legible (p_ui.c)
    if (en > 0) {
        int avail = DisplayHeight(d->dpy, DefaultScreen(d->dpy)) - 40 - (int)y - (int)(24 * S);
        for (;;) {
            if (en * (headH + bodyH + footH) + (en - 1) * (int)gap <= avail) break;
            if (bodyH > (int)(72 * S)) { bodyH -= (int)(4 * S); continue; }
            if (footH > (int)(16 * S)) { footH -= (int)(2 * S); continue; }
            if (headH > (int)(28 * S)) { headH -= (int)(2 * S); continue; }
            break;
        }
    }
    int panelH = headH + bodyH + footH;
    int maxPanels = (DisplayHeight(d->dpy, DefaultScreen(d->dpy)) - 40 - (int)y - (int)(24 * S) +
                     (int)gap) / (panelH + (int)gap);
    if (maxPanels < 1) maxPanels = 1;

    int shown = 0;
    for (int i = 0; i < pn; i++) {
        if (!cbSubsEnabled() || !cbSubsProvEnabled(i)) continue;
        if (shown >= maxPanels) {
            // the same diagnostic p_ui.c logs under general.debug when
            // panels are skipped (the README documents the line); silent when
            // debug is off
            if (cbCfgDebug())
                fprintf(stderr,
                        "[wizbar] subs board: %d of %d panels fit the screen (%d px body)\n",
                        maxPanels, pn,
                        DisplayHeight(d->dpy, DefaultScreen(d->dpy)) - 40 - (int)y - (int)(24 * S));
            break;
        }

        char label[64];
        wchar_t wl[64];
        cbSubsProvLabel(i, wl, 64);
        wideToUtf8(wl, label, sizeof(label));

        SubsWin wins[SUBS_HM_MAX * 4];
        int wn = cbSubsProvWins(i, wins, (int)(sizeof(wins) / sizeof(wins[0])));
        int stale = wn < 0;
        if (wn < 0) wn = -wn;

        double px2 = padL, py2 = y + shown * (panelH + gap);
        // panel card, then the head band (opaque fill, dashed underline)
        card(d, cr, px2, py2, innerW, panelH, 8 * S, d->card, 0.85, d->divider);
        card(d, cr, px2, py2, innerW, headH, 8 * S, d->head, 1.0, (Rgb3){ -1, 0, 0 });
        dotted(cr, px2, px2 + innerW, py2 + headH, d->divider);

        // provider dot + label, plan name beside it (p_ui.c)
        dot(cr, px2 + 14 * S + 4.5 * S, py2 + (headH - 9 * S) / 2 + 4.5 * S, 4.5 * S,
            d->ramp[i % 5]);
        setFont(d, cr, 13, 1);
        double lx = px2 + 14 * S + 9 * S + 8 * S;
        text(cr, lx, bl(cr, py2 + 11 * S), label, d->fg, 1.0);
        char plan[64];
        wchar_t wplan[64];
        cbSubsProvPlan(i, wplan, 64);
        wideToUtf8(wplan, plan, sizeof(plan));
        if (!plan[0]) snprintf(plan, sizeof(plan), "\u2014");
        double lw = textW(cr, label);
        setFont(d, cr, 10, 0);
        text(cr, lx + lw + 8 * S, bl(cr, py2 + 14 * S), plan, d->dim, 1.0);

        // status pill: stale / capped / near cap / ok (p_ui.c)
        {
            int lowest = 1000, anyw = 0;
            for (int k = 0; k < wn; k++) {
                if (wins[k].rem < 0) continue;   // no fraction reported: not a cap
                anyw = 1;
                if (wins[k].rem < lowest) lowest = wins[k].rem;
            }
            const char *ps = NULL;
            Rgb3 pc = d->dim, pbg = blend(d->head, d->dim, 31);
            if (stale) { ps = "STALE"; pc = d->yellow; pbg = blend(d->head, d->yellow, 31); }
            else if (!anyw) ps = NULL;
            else if (lowest <= 0) { ps = "CAPPED"; pc = d->warn; pbg = blend(d->head, d->warn, 20); }
            else if (lowest <= 10) { ps = "NEAR CAP"; pc = d->warn; pbg = blend(d->head, d->warn, 20); }
            else { ps = "OK"; pc = d->good; pbg = blend(d->head, d->good, 20); }
            if (ps) {
                setFont(d, cr, 10, 1);
                double pw = textW(cr, ps) + 16 * S;
                double ph = 18 * S;
                double plx = px2 + innerW - 14 * S - pw;
                card(d, cr, plx, py2 + (headH - ph) / 2, pw, ph, 10 * S, pbg, 1.0, pc);
                // centre the ink block in the frame on both axes (p_ui.c uses
                // DT_VCENTER; the equivalent is the ascent-descent midpoint)
                cairo_font_extents_t pfe;
                cairo_font_extents(cr, &pfe);
                text(cr, plx + (pw - textW(cr, ps)) / 2,
                     py2 + (headH - ph) / 2 + (ph + pfe.ascent - pfe.descent) / 2,
                     ps, pc, 1.0);
            }
        }

        // body: one cell per window, left to right
        double bodyY = py2 + headH + 4 * S;
        if (wn == 0) {
            setFont(d, cr, 11, 0);
            text(cr, px2 + 14 * S, bl(cr, bodyY + 14 * S),
                 stale ? "stale \u2014 no data yet" : "no windows reported", d->dim, 1.0);
        }
        int cols = wn > 0 ? wn : 1;
        double cellW = (innerW - 28 * S) / cols;
        // the pie takes only what is left of the label (p_ui.c)
        double labNeed = 0;
        setFont(d, cr, 13, 1);          // the font the label is painted with
        for (int k = 0; k < wn; k++) {
            char up[24];
            winUpper(wins[k].label, up, sizeof(up));
            double w2 = textW(cr, up) + 0.7 * S * strlen(up);
            if (w2 > labNeed) labNeed = w2;
        }
        double pieD = cellW - labNeed - 20 * S;
        if (pieD > 96 * S) pieD = 96 * S;
        if (pieD > bodyH - 8 * S) pieD = bodyH - 8 * S;
        if (pieD < 24 * S) pieD = 24 * S;
        double pieScale = pieD / (96 * S);
        double penW = 12.57 * S * pieScale;
        double rr = 36.4 * S * pieScale;
        for (int k = 0; k < wn; k++) {
            double kx = px2 + 14 * S + k * cellW;
            double ky = bodyY + (bodyH - pieD) / 2;
            int rem = wins[k].rem;
            int unk = rem < 0;
            if (rem < 0) rem = 0;
            if (rem > 100) rem = 100;
            Rgb3 track = blend(d->card, d->dim, 31);
            donut(cr, kx + pieD / 2, ky + pieD / 2, rr, penW, track, d->pinkDeep, rem);
            // centre: "N%" then "left"
            char pct[8];
            if (unk) snprintf(pct, sizeof(pct), "\u2014");
            else snprintf(pct, sizeof(pct), "%d", rem);
            // measure with the face each string is PAINTED with (p_ui.c's
            // dashStrW(dc, pctS, fPct) / dashStrW(dc, L"%", fS10)): measuring
            // with a stale font here misplaces the percent sign entirely
            setFont(d, cr, pieD >= 56 * S ? 18 : (pieD >= 38 * S ? 13 : 10), 1);
            double nw = textW(cr, pct);
            setFont(d, cr, 10, 0);
            double inkW = unk ? nw : nw + 1 * S + textW(cr, "%");
            double cx0 = kx + (pieD - inkW) / 2;
            double cyc = ky + pieD / 2;
            setFont(d, cr, pieD >= 56 * S ? 18 : (pieD >= 38 * S ? 13 : 10), 1);
            text(cr, cx0, bl(cr, cyc - 18.5 * S), pct, d->pinkDeep, 1.0);
            if (!unk) {
                setFont(d, cr, 10, 0);
                text(cr, cx0 + nw + 1 * S, bl(cr, cyc - 14.5 * S), "%", d->pinkDeep, 1.0);
            }
            setFont(d, cr, 9, 0);
            text(cr, kx + (pieD - textW(cr, "left")) / 2, bl(cr, cyc + 2.5 * S), "left", d->dim, 1.0);
            // meta right of the pie
            double mx = kx + pieD + 12 * S;
            char up[24];
            winUpper(wins[k].label, up, sizeof(up));
            setFont(d, cr, 13, 1);
            double metaY = ky + (int)(pieD * 0.25f);
            text(cr, mx, bl(cr, metaY), up, d->fg, 1.0);
            char usedLine[96];
            if (wins[k].used >= 0 && wins[k].total > 0) {
                char lf[24], ts[24];
                long long left = (long long)wins[k].total - wins[k].used;
                if (left < 0) left = 0;
                fmtNum(left, lf, sizeof(lf));
                fmtNum(wins[k].total, ts, sizeof(ts));
                snprintf(usedLine, sizeof(usedLine), "%s left of %s", lf, ts);
            } else if (wins[k].pct < 0) {
                snprintf(usedLine, sizeof(usedLine), "reset-only pool");
            } else {
                snprintf(usedLine, sizeof(usedLine), "%d%% used", wins[k].pct);
            }
            setFont(d, cr, 10, 0);
            text(cr, mx, bl(cr, ky + (int)(pieD * 0.52f)), usedLine, d->dim, 1.0);
            char rst[64];
            fmtReset(wins[k].resetAt, rst, sizeof(rst));
            text(cr, mx, bl(cr, ky + (int)(pieD * 0.69f)), rst, d->dim, 1.0);
        }

        double fy = py2 + panelH - footH;
        dotted(cr, px2 + 14 * S, px2 + innerW - 14 * S, fy, d->divider);
        // panel foot: dashed top + the cycle's fetched time, right-aligned.
        // The stamp is the wall clock in p_utils.c's UTC-labelled frame (the
        // dashFmtTime contract), so it reads back through gmtime_r.
        char ftim[16] = "\u2014";
        long long fepoch = cbSubsFetchedEpochMs();
        if (fepoch) {
            time_t t = (time_t)(fepoch / 1000);
            struct tm utc;
            gmtime_r(&t, &utc);
            snprintf(ftim, sizeof(ftim), "%02d:%02d:%02d", utc.tm_hour, utc.tm_min, utc.tm_sec);
        }
        setFont(d, cr, 10, 0);
        textR(cr, px2 + innerW - 14 * S, bl(cr, fy + 6 * S), ftim, d->dim, 1.0);
        shown++;
    }

    if (!shown) {
        setFont(d, cr, 11, 0);
        text(cr, padL, bl(cr, y),
             cbSubsEnabled() ? "No providers enabled." : "Subscriptions are off (subs.enabled).",
             d->dim, 1.0);
    }

    // panels are fixed-height rows: the stack's bottom is the content edge
    if (shown) d->contentH = (int)(y + shown * panelH + (shown - 1) * gap + 14 * S);
    else d->contentH = (int)(y + 38 * S + 14 * S);
}

// ---- public API -------------------------------------------------------------------

Window subsOpen(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
                int cssW, int cssH, double scale, const char *fontFamily,
                const double page[3],
                const double fg[3], const double dim[3], const double divider[3],
                const double ramp[5][3], const double pink[3],
                const double pinkDeep[3], const double warn[3],
                const double good[3], const double yellow[3],
                const double iconCol[3]) {
    if (g_open) subsClose();
    Subs *d = &g_subs;
    memset(d, 0, sizeof(*d));
    d->dpy = dpy; d->visual = visual;
    d->scale = scale > 0 ? scale : 1.0;
    snprintf(d->fontFamily, sizeof(d->fontFamily), "%s", fontFamily ? fontFamily : "sans");
    d->bg = (Rgb3){ page[0], page[1], page[2] };
    // card and head are DERIVED from the page colour, not the theme's pink:
    // p_ui.c's dashResolveTheme computes card = blendCr(bg, white, 64) and
    // head = blendCr(card, white, 64), so the panels stay near-white with a
    // lighter stacked head band.
    Rgb3 white = { 1.0, 1.0, 1.0 };
    d->card = blend(d->bg, white, 64);
    d->head = blend(d->card, white, 64);
    d->fg = (Rgb3){ fg[0], fg[1], fg[2] };
    d->dim = (Rgb3){ dim[0], dim[1], dim[2] };
    d->divider = (Rgb3){ divider[0], divider[1], divider[2] };
    d->pink = (Rgb3){ pink[0], pink[1], pink[2] };
    d->pinkDeep = (Rgb3){ pinkDeep[0], pinkDeep[1], pinkDeep[2] };
    d->warn = (Rgb3){ warn[0], warn[1], warn[2] };
    d->good = (Rgb3){ good[0], good[1], good[2] };
    d->yellow = (Rgb3){ yellow[0], yellow[1], yellow[2] };
    for (int i = 0; i < 5; i++) d->ramp[i] = (Rgb3){ ramp[i][0], ramp[i][1], ramp[i][2] };
    d->iconCol = (Rgb3){ iconCol[0], iconCol[1], iconCol[2] };
    d->cfgH = cssH;

    // the board's two faces, UPRIGHT like the Windows board
    d->ffReg = cairo_toy_font_face_create(d->fontFamily, CAIRO_FONT_SLANT_NORMAL,
                                          CAIRO_FONT_WEIGHT_NORMAL);
    d->ffBold = cairo_toy_font_face_create(d->fontFamily, CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_BOLD);
    // the close glyph's face comes from the shared lookup (the display family
    // has no U+2715 and cairo does not fall back)
    d->ffSymbol = symbolFontFace();

    d->w = (int)(cssW * d->scale + 0.5);
    d->h = (int)(cssH * d->scale + 0.5);
    // p_ui.c dashToggle clamps the size to dashMaxH() (CYSCREEN-40) and
    // sw-40 BEFORE it opens and centres: the content fit later shrinks the
    // height to the content, but the OPEN position must already fit the
    // screen. Without the clamp a board taller than the screen centres to a
    // NEGATIVE y, and the fit's keep-position rule then pins it to the top
    // edge - over the bar (measured on the 400 px test screen: the 580 css
    // subs board opened at y=0 and covered the bar, eating the next clicks).
    {
        int sh = DisplayHeight(dpy, screen);
        int sw = DisplayWidth(dpy, screen);
        if (d->w > sw - 40) d->w = sw - 40;
        int maxH = sh - 40;
        if (maxH > 0 && d->h > maxH) d->h = maxH;
    }
    // p_ui.c dashToggle placement: the board opens centred, both axes -
    // boardCenterOnHead centres it on the head the pointer is on - before the
    // content fit adjusts the height.
    boardCenterOnHead(dpy, screen, d->w, d->h, &d->x, &d->y);

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.colormap = cmap;
    // MANAGED, not override-redirect (linux_shape.c boardMakeManaged for the
    // full why): an OR window is composited in KWin's top layer and cannot be
    // moved or covered - v1.6.3 shipped both defects. The managed popup keeps
    // the popup properties (no titlebar, no taskbar entry, exact rectangle)
    // and regains the Windows board's behaviour: HWND_TOP at open, coverable,
    // draggable by its title band.
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
                     LeaveWindowMask;
    d->win = XCreateWindow(dpy, RootWindow(dpy, screen), d->x, d->y,
                           (unsigned)d->w, (unsigned)d->h, 0, depth, InputOutput, visual,
                           CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
                           &swa);
    if (!d->win) return None;
    boardMakeManaged(dpy, d->win, "Chocobar - Subscriptions", "chocobar-subs",
                     d->x, d->y, d->w, d->h);
    // Managed popup: the properties written by boardMakeManaged keep this a
    // popup as p_ui.c registers ChocobarDash - an owned WS_POPUP: no taskbar
    // button, no Alt-Tab entry, no titlebar. The stacking (HWND_TOP at open,
    // coverable afterwards) is the window manager's normal-layer behaviour
    // instead of a hardcoded topmost bit. The keyboard diverges from Windows
    // (which never activates the board): on X the board takes the focus while
    // it is up so Esc/q close it, and hands it back on close. Close is the
    // board's own X button or the menu toggle.
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
    // Size hints are set by boardMakeManaged (USPosition); the content fit is
    // the only sizer.

    d->surf = cairo_xlib_surface_create(dpy, d->win, visual, d->w, d->h);
    d->cr = cairo_create(d->surf);
    reshape(d);

    // paint, fit, then map (p_ui.c paints while hidden so the board appears once)
    paintAndFit(d);
    XMoveResizeWindow(dpy, d->win, d->x, d->y, (unsigned)d->w, (unsigned)d->h);
    // Plain map: the stacking is the window manager's now. A newly mapped
    // window lands above the current stack (KWin), and the focus take below
    // activates the board, which lifts it above the terminal - the Windows
    // board's SetWindowPos(HWND_TOP) without a hardcoded topmost bit.
    XMapWindow(dpy, d->win);
    boardRequestAllDesktops(dpy, d->win);
    // Take the keyboard while the board is up. The managed board CAN be
    // activated by KWin, but only on a click; opening it from the bar's click
    // would leave the keyboard with the terminal, so the board takes the focus
    // itself (p_ui.c parity: clicking the Windows board activates it, and Esc
    // closes it). The menu does the same for the same reason. AFTER the map:
    // SetInputFocus on an unmapped window is a BadMatch (not viewable).
    // reason. The window that held the focus is remembered first and restored
    // by subsClose, so the terminal the user was typing in keeps its keys.
    {
        Window pf = None;
        int pr = 0;
        XGetInputFocus(dpy, &pf, &pr);
        g_focusPrev = pf;
    }
    // Ask the window manager to activate (raise above the window the user was
    // in) with the timestamp of the click that opened the board, then focus
    // directly as the WM-less fallback. Both steps live in boardActivate.
    extern Time g_lastInputTime;
    boardActivate(dpy, d->win, g_lastInputTime);

    g_open = 1;
    return d->win;
}

// Repaints the board after a fetch lands (the bar calls this on the subs epoch).
void subsRepaint(void) {
    if (!g_open || !g_subs.cr) return;
    paintAndFit(&g_subs);
}

// Returns 1 when the board wants to close (Esc, q, middle/right click, WM close).
int subsEvent(XEvent *ev) {
    Subs *d = &g_subs;
    if (!g_open || !d->win) return 0;
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count != 0) break;
        paintAndFit(d);
        break;
    case ConfigureNotify:
        d->x = ev->xconfigure.x;
        d->y = ev->xconfigure.y;
        if (ev->xconfigure.width != d->w || ev->xconfigure.height != d->h) {
            d->w = ev->xconfigure.width;
            d->h = ev->xconfigure.height;
            cairo_destroy(d->cr);
            cairo_surface_destroy(d->surf);
            d->surf = cairo_xlib_surface_create(d->dpy, d->win, d->visual, d->w, d->h);
            d->cr = cairo_create(d->surf);
            reshape(d);
            // the board is content-sized (p_ui.c re-fits on every paint), so a
            // foreign resize is answered by the fit, not kept
            paintAndFit(d);
        }
        break;
        case ClientMessage:
            // WM_DELETE_WINDOW: the WM's close request (Alt+F4, the window
            // menu) closes the board exactly like the X button - the protocol
            // boardMakeManaged advertises, because a managed window that
            // answers no close protocol drives a WM to XKillClient on the
            // bar's shared connection.
            if (ev->xclient.message_type == XInternAtom(d->dpy, "WM_PROTOCOLS", False) &&
                (Atom)ev->xclient.data.l[0] == XInternAtom(d->dpy, "WM_DELETE_WINDOW", False))
                return 1;
            break;
    case KeyPress: {
        char buf[8];
        KeySym ks;
        if (XLookupString(&ev->xkey, buf, sizeof(buf), &ks, NULL) == 1) {
            if (buf[0] == 27 || buf[0] == 'q') return 1;
        }
        if (ks == XK_Escape) return 1;
        break;
    }
    case MotionNotify: {
        // Title-band drag (p_ui.c WM_NCHITTEST returns HTCAPTION there, so
        // Windows moves the window under a held left button with no extra
        // code): on X the app does the move. The press recorded the pointer
        // offset from the window's top-left; every motion re-applies it.
        // Nothing else runs while dragging - the hover repaint would follow a
        // pointer that is really just moving the window.
        if (d->dragging) {
            // d->x/d->y must track the drag: the content fit and its head
            // clamp read them, so a stale position would move the board
            // BACK to where the drag started on the next resize.
            d->x = ev->xmotion.x_root - d->dragDx;
            d->y = ev->xmotion.y_root - d->dragDy;
            XMoveWindow(d->dpy, d->win, d->x, d->y);
            break;
        }
        int b = buttonAt(d, ev->xmotion.x, ev->xmotion.y);
        if (b != d->btnHover) {
            d->btnHover = b;
            paintAndFit(d);
        }
        break;
    }
    case LeaveNotify:
        if (d->btnHover) {
            d->btnHover = 0;
            paintAndFit(d);
        }
        break;
    case ButtonRelease:
        // the buttons act on the RELEASE inside their own rect (the bar's own
        // rule: a press that wandered off must not fire)
        if (ev->xbutton.button == 1) {
            // a title-band drag ends silently: the release that completes a
            // move must not also act as a click on whatever button the
            // pointer ended over (Windows: HTCAPTION swallows the release)
            if (d->dragging) {
                d->dragging = 0;
                XUngrabPointer(d->dpy, CurrentTime);
                break;
            }
            int b = buttonAt(d, ev->xbutton.x, ev->xbutton.y);
            if (b == 2) return 1;                    // close
            if (b == 1) {
                // refresh: re-fetch the providers (p_ui.c's subsRefetchNow),
                // with the same 12 s click debounce so a second click cannot
                // re-scan while the first fetch is in flight
                long long now = nowMs();
                if (now < d->busyUntilMs) break;
                d->busyEpoch = cbSubsFetchedEpochMs();
                d->busyUntilMs = now + 12000;
                d->busy = 1;
                paintAndFit(d);
                cbSubsRefetchNow();
                paintAndFit(d);
            }
        }
        break;
    case ButtonPress:
        // A left press in the title band starts the drag the Windows board
        // gets from WM_NCHITTEST's HTCAPTION (p_ui.c 2739: any point above
        // the titlebar bottom that is not a button).
        if (ev->xbutton.button == 1 && ev->xbutton.y < d->tbBottom &&
            !buttonAt(d, ev->xbutton.x, ev->xbutton.y)) {
            d->dragging = 1;
            d->dragDx = ev->xbutton.x_root - d->x;
            d->dragDy = ev->xbutton.y_root - d->y;
            // grab the pointer for the drag (menu.c's menuRun is the in-repo
            // precedent): a fast drag outruns the async XMoveWindow, the
            // pointer crosses out of the board, and motion/release then go to
            // whatever window is under it - dragging would stay 1 and the next
            // hover would teleport the window. The grab keeps every event on
            // the board until the release; owner_events=True keeps the board's
            // own hover machinery live.
            XGrabPointer(d->dpy, d->win, True,
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            break;
        }
        // middle/right click closes the board (the Windows dashProc does)
        if (ev->xbutton.button != 1) return 1;
        break;
    case DestroyNotify:
        // the window is gone from outside (xkill, a compositor restart): the
        // bar's epoch poll would keep painting into a dead drawable and the
        // next activation would only call subsClose() on a ghost
        subsClose();
        return 1;
    }
    return 0;
}

void subsClose(void) {
    Subs *d = &g_subs;
    if (!g_open) return;
    // a drag that never saw its release (the board closed under the pointer)
    // must not leave the pointer grabbed: every event would stay routed here
    XUngrabPointer(d->dpy, CurrentTime);
    // hand the keyboard back before the focused window dies (same as dashClose;
    // only while the saved window is still mapped - SetInputFocus on an
    // unmapped window is a BadMatch)
    if (g_focusPrev != None && d->dpy) {
        XWindowAttributes at;
        if (XGetWindowAttributes(d->dpy, g_focusPrev, &at) && at.map_state == IsViewable)
            XSetInputFocus(d->dpy, g_focusPrev, RevertToParent, CurrentTime);
        g_focusPrev = None;
    }
    if (d->cr) { cairo_destroy(d->cr); d->cr = NULL; }
    if (d->surf) { cairo_surface_destroy(d->surf); d->surf = NULL; }
    if (d->win) { XDestroyWindow(d->dpy, d->win); d->win = None; }
    boardShapeFree(d->dpy, &d->shape, &d->shapeW, &d->shapeH, &d->shapeR);
    if (d->ffReg) { cairo_font_face_destroy(d->ffReg); d->ffReg = NULL; }
    if (d->ffBold) { cairo_font_face_destroy(d->ffBold); d->ffBold = NULL; }
    if (d->ffSymbol) { cairo_font_face_destroy(d->ffSymbol); d->ffSymbol = NULL; }
    g_open = 0;
    XFlush(d->dpy);
}

Window subsWin(void) { return g_open ? g_subs.win : None; }
