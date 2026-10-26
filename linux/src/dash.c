// linux/src/dash.c - the token dashboard, the Linux twin of the Windows board.
//
// This is a PIXEL PORT, not an approximation. The Windows board is ~2000 lines of
// GDI painting inside p_ui.c (paintDash); this file reproduces its layout,
// type scale, colours and every element, drawn with cairo, reading the SAME
// aggregates through the cbTok* accessors in p_agg.c. The numbers therefore
// cannot disagree with the Windows board about the same store, and the layout
// constants below are transcribed from p_ui.c (CSS px, scaled by g_scale) so the
// two boards cannot drift apart by accident either.
//
// What the Windows board has and this reproduces, in its order:
//   1. titlebar: the diamond/gauge icon, "Token Usage" at 15px bold, then the
//      "refresh" and close buttons at the right, then a hairline divider
//   2. four stat cards (Today / Last 7 days / Last 30 days / All time) at 10px
//      label + 17px bold value
//   3. a DAILY USAGE card: 26 weeks of heatmap cells with month labels along the
//      top, Sun/Fri row labels, a "less [5 swatches] more" legend, and a day
//      detail block that opens when a cell is clicked
//   4. two tables side by side, BY APP and BY MODEL, each row a dot, the name,
//      a share bar behind it, and right-aligned INPUT / OUTPUT / CACHE R /
//      CACHE W / CALLS columns whose presence depends on the data
//   5. the "+N more models" overflow line and the "last scan HH:MM:SS" footer
//
// It is a normal managed window (not override-redirect) on purpose: a dashboard
// is something you open, move and close, so it should behave like a window, unlike
// the bar and the menu.

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
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <fontconfig/fontconfig.h>
#include "linux_svg.h"

#include "dash.h"
#include "linux_shape.h"
#include "tooltip.h"
#include "svg_geometry.h"  // native/src: the shared icon ids (SVG_DIAMOND, ...)

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
extern long long cbTokModelIn(int i);
extern long long cbTokModelOut(int i);
extern long long cbTokModelCr(int i);
extern long long cbTokModelCw(int i);
extern int cbTokModelRowCap(void);
extern long long cbTokDayAppIn(int daysBack, int i);
extern long long cbTokDayAppOut(int daysBack, int i);
extern long long cbTokDayAppCr(int daysBack, int i);
extern long long cbTokDayAppCw(int daysBack, int i);
extern int cbTokDayAppReqs(int daysBack, int i);
extern int cbCfgTokensEnabled(void);
extern int cbCfgSubsRotateSec(void);
extern const wchar_t *cbCfgIconColor(void);
extern const wchar_t *cbCfgPinkDeep(void);
extern const wchar_t *cbCfgYellow(void);

// The board's own pixel constants, transcribed from p_ui.c paintDash. CSS px;
// every use multiplies by the scale, exactly as DX() does on Windows.
#define DASH_PAD_L 18
#define DASH_PAD_T 14
#define DASH_GAP 12
#define DASH_HM_WEEKS 26          // p_ui.c HM_WEEKS: fixed heatmap width in weeks
#define DASH_MAX_DAYS 190         // p_agg.c's per-day window (about 27 weeks)
#define DASH_DAY_DETAIL_ROWS 6    // p_ui.c ddRows cap
// the MODEL table's row cap is NOT defined here: it is p_agg's own
// DASH_MODEL_ROWS, reached through cbTokModelRowCap() so the two boards cannot
// disagree about how many rows the same store shows

typedef struct { double r, g, b; } Rgb3;

typedef struct {
    Display *dpy;
    Window win;
    Visual *visual;
    int w, h;
    double scale;
    char fontFamily[128];
    Rgb3 bg, card, zebra, fg, dim, divider, pinkDeep, pink, yellow;
    Rgb3 ramp[5];
    Atom wmDelete;             // WM_DELETE_WINDOW, registered on the window
    int contentH;              // painted content bottom (device px) -> content fit
    int tbBottom;              // titlebar bottom (device px)
    /* titlebar button rects (device px), hover state and the click areas */
    int btnRefreshL, btnRefreshR, btnRefreshT, btnRefreshB;
    int btnCloseL, btnCloseR, btnCloseT, btnCloseB;
    int btnHover;              // 0 none, 1 refresh, 2 close
    int busy;                  // refresh in flight: the button reads "..."
    /* heatmap hit rects (device px) and the days-back each holds */
    int hmL[DASH_HM_WEEKS][7], hmT[DASH_HM_WEEKS][7];
    int hmR[DASH_HM_WEEKS][7], hmB[DASH_HM_WEEKS][7];
    int hmBack[DASH_HM_WEEKS][7];
    int hmValid;               // 1 = the rects above describe the current paint
    int daySel;                // selected heatmap cell (days back), -1 = none
    /* persistent paint state: the board repaints on hover, so building a
       surface and toy font faces per frame would be an allocation per pixel */
    cairo_surface_t *surf;
    cairo_t *cr;
    XserverRegion shape;        // the window's rounded shape (DWM round-native twin)
    int shapeR;                 // the radius the shape was built with (device px)
    cairo_font_face_t *ffReg, *ffBold;
    // Glyphs the display family does not carry (the titlebar's U+2715 close
    // cross). Windows falls back through GDI's font linking to Segoe UI Symbol;
    // cairo's toy API does NOT fall back - a missing glyph renders as a tofu
    // box, which is exactly what the close button showed before this face.
    // Resolved through fontconfig once at open, like the bar's own family.
    cairo_font_face_t *ffSymbol;
    int cfgH;                  // the CONFIG height in CSS px (content-fit measures against it)
    int x, y;                  // window position: content fit keeps the board on screen
    long long lastScanAt;      // the stamp the footer shows (local epoch seconds)
} Dash;

static Dash g_dash;
static int g_boardOpen = 0;

// The board's own window corner radius, in CSS px: Windows 11's DWMWCP_ROUND_NATIVE
// for a normal (non-maximized) window. p_ui.c dashRoundCorners asks for exactly
// that, so the Linux board's shape uses the same number.
#define DASH_WIN_RADIUS 8.0

// ---- small helpers ---------------------------------------------------------------

// p_agg.c hands out UTF-8 names; cairo wants UTF-8, so nothing to convert. The
// bar's own wideToUtf8 lives in main.c (a front-end file); the board only needs
// the ASCII-ish label helpers below.

// p_ui.c fmtTokens: the stat-card abbreviation
static void fmtTok(long long n, char *out, size_t cap) {
    if (n < 0) { snprintf(out, cap, "\u2014"); return; }
    if (n >= 1000000000LL) snprintf(out, cap, "%.2fB", n / 1e9);
    else if (n >= 1000000) snprintf(out, cap, "%.1fM", n / 1e6);
    else if (n >= 1000) snprintf(out, cap, "%.1fk", n / 1e3);
    else snprintf(out, cap, "%lld", n);
}

// p_ui.c fmtCalls: the CALLS column. Different tiers from fmtTokens on purpose
// (100000+ abbreviates, so the column stays narrow).
static void fmtCalls(long long n, char *out, size_t cap) {
    if (n >= 999950000LL) snprintf(out, cap, "%.2fB", n / 1e9);
    else if (n >= 999950) snprintf(out, cap, "%.1fM", n / 1e6);
    else if (n >= 100000) snprintf(out, cap, "%.1fk", n / 1e3);
    else snprintf(out, cap, "%lld", n);
}

static void setFont(Dash *d, cairo_t *cr, double cssPx, int bold) {
    cairo_set_font_face(cr, bold ? d->ffBold : d->ffReg);
    cairo_set_font_size(cr, cssPx * d->scale);
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

// p_ui.c dashStrR: right-aligned text at xRight
static void textR(cairo_t *cr, double xRight, double y, const char *s, Rgb3 c, double a) {
    text(cr, xRight - textW(cr, s), y, s, c, a);
}

// The board's card: a rounded rect with a translucent fill and a hairline edge.
// The path itself is the shared roundedPath() - the board's four-arc copy was the
// third one and the reason a geometry fix had to be made three times.
static void card(Dash *d, cairo_t *cr, double x, double y, double w, double h,
                 double r, Rgb3 fill, double fillA, Rgb3 edge) {
    roundedPath(cr, x, y, w, h, r);
    cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fillA);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, edge.r, edge.g, edge.b, 0.55);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_new_path(cr);
}

// the app dot: p_ui.c appDotColor's per-app palette, falling back to a dim card
// blend for an app it does not name
static Rgb3 appDotColor(const char *app, Dash *d) {
    if (!strcmp(app, "zcode")) return d->pinkDeep;
    if (!strcmp(app, "zai")) return d->yellow;
    if (!strcmp(app, "opencode")) return (Rgb3){ 0x7F / 255.0, 0xA8 / 255.0, 0xA0 / 255.0 };
    if (!strcmp(app, "mimo")) return (Rgb3){ 0xB7 / 255.0, 0x9C / 255.0, 0xE0 / 255.0 };
    if (!strcmp(app, "pi")) return (Rgb3){ 0x8F / 255.0, 0xA8 / 255.0, 0xD4 / 255.0 };
    return (Rgb3){ (d->card.r + d->dim.r) / 2.0, (d->card.g + d->dim.g) / 2.0,
                   (d->card.b + d->dim.b) / 2.0 };
}

static void dot(cairo_t *cr, double cx, double cy, double d, Rgb3 c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
    cairo_arc(cr, cx + d / 2.0, cy + d / 2.0, d / 2.0, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_new_path(cr);
}

// a horizontal gradient bar (p_ui.c dashTableRow's share bar): pink -> pinkDeep
// at 35% over the card
static void shareBar(cairo_t *cr, Dash *d, double x, double y, double w, double h) {
    Rgb3 c1 = { (d->card.r + d->pink.r) / 2.0, (d->card.g + d->pink.g) / 2.0,
                (d->card.b + d->pink.b) / 2.0 };
    Rgb3 c2 = { (d->card.r + d->pinkDeep.r) / 2.0, (d->card.g + d->pinkDeep.g) / 2.0,
                (d->card.b + d->pinkDeep.b) / 2.0 };
    cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x + w, y);
    cairo_pattern_add_color_stop_rgba(p, 0, c1.r, c1.g, c1.b, 1.0);
    cairo_pattern_add_color_stop_rgba(p, 1, c2.r, c2.g, c2.b, 1.0);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_new_path(cr);
}

// a hairline (p_ui.c's divider strokes: solid or dotted)
static void hairline(cairo_t *cr, double x0, double x1, double y, Rgb3 c, double a,
                     int dotted) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
    cairo_set_line_width(cr, 1.0);
    if (dotted) {
        double dashes[1] = { 2.0 };
        cairo_set_dash(cr, dashes, 1, 0);
    }
    cairo_move_to(cr, x0, y);
    cairo_line_to(cr, x1, y);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
    cairo_new_path(cr);
}

// ---- the tables ------------------------------------------------------------------
// Column geometry is measured from the widest VALUE per column, not fixed widths
// (p_ui.c dashTableCols): the fixed widths let a 5-digit call count run into the
// cache column. need[] is indexed [0]=calls [1]=cacheW [2]=cacheR [3]=output
// [4]=input, in device px.
static void tableCols(int innerRight, int cacheR, int cacheW, const int *need, int *xs) {
    double s = g_dash.scale;
    int x = innerRight;
    xs[0] = x; x -= need[0] + (int)(12 * s);
    if (cacheW && need[1] > 0) { xs[1] = x; x -= need[1] + (int)(12 * s); } else xs[1] = 0;
    if (cacheR && need[2] > 0) { xs[2] = x; x -= need[2] + (int)(12 * s); } else xs[2] = 0;
    xs[3] = x; x -= need[3] + (int)(12 * s);
    xs[4] = x;
}

typedef struct {
    long long in, out, cr, cw;
    int req;
} RowAgg;

// fold one row's formatted values into need[] (max width per column)
static void tableRowNeeds(Dash *d, cairo_t *cr, const RowAgg *a, int *need) {
    char vs[32];
    int w;
    setFont(d, cr, 11, 0);
    fmtTok(a->in, vs, 32); w = (int)textW(cr, vs); if (w > need[4]) need[4] = w;
    fmtTok(a->out, vs, 32); w = (int)textW(cr, vs); if (w > need[3]) need[3] = w;
    if (a->cr > 0) { fmtTok(a->cr, vs, 32); w = (int)textW(cr, vs); if (w > need[2]) need[2] = w; }
    if (a->cw > 0) { fmtTok(a->cw, vs, 32); w = (int)textW(cr, vs); if (w > need[1]) need[1] = w; }
    fmtCalls(a->req, vs, 32); w = (int)textW(cr, vs); if (w > need[0]) need[0] = w;
}

static void tableHeadNeeds(Dash *d, cairo_t *cr, int *need) {
    static const char *hd[5] = { "CALLS", "CACHE W", "CACHE R", "OUTPUT", "INPUT" };
    setFont(d, cr, 9, 0);
    for (int i = 0; i < 5; i++) {
        int w = (int)textW(cr, hd[i]);
        if (w > need[i]) need[i] = w;
    }
}

// one table row: the share bar behind the first cell, the dot, the name, then
// the numbers right-aligned in their columns
static void tableRow(Dash *d, cairo_t *cr, double x0, double innerW, double y,
                     double rowH, const int *xs, const char *label, Rgb3 dotc,
                     double share, const RowAgg *a) {
    double s = d->scale;
    if (share > 0.003) {
        double bw = share * innerW;
        if (bw < 3 * s) bw = 3 * s;
        double bh = rowH * 0.7;
        shareBar(cr, d, x0 + 2, y + (rowH - bh) / 2.0, bw, bh);
    }
    dot(cr, x0 + 2, y + (rowH - 8 * s) / 2.0, 8 * s, dotc);
    setFont(d, cr, 11, 0);
    // the name takes every pixel left of the INPUT column (a fixed width
    // ellipsized real model ids even in a narrow card)
    cairo_save(cr);
    cairo_rectangle(cr, x0 + 2 + 14 * s, y, xs[4] - (x0 + 2 + 14 * s) - 6 * s, rowH);
    cairo_clip(cr);
    text(cr, x0 + 2 + 14 * s, y + rowH * 0.5 + 4 * s, label, d->fg, 1.0);
    cairo_restore(cr);
    char vs[32];
    fmtTok(a->in, vs, 32); textR(cr, xs[4], y + rowH * 0.5 + 4 * s, vs, d->fg, 1.0);
    fmtTok(a->out, vs, 32); textR(cr, xs[3], y + rowH * 0.5 + 4 * s, vs, d->fg, 1.0);
    if (xs[2]) { fmtTok(a->cr, vs, 32); textR(cr, xs[2], y + rowH * 0.5 + 4 * s, vs, d->fg, 1.0); }
    if (xs[1]) { fmtTok(a->cw, vs, 32); textR(cr, xs[1], y + rowH * 0.5 + 4 * s, vs, d->fg, 1.0); }
    fmtCalls(a->req, vs, 32); textR(cr, xs[0], y + rowH * 0.5 + 4 * s, vs, d->fg, 1.0);
}

static void tableHead(Dash *d, cairo_t *cr, double x0, double y, const int *xs,
                      const char *firstCol) {
    setFont(d, cr, 9, 0);
    text(cr, x0 + 2, y, firstCol, d->dim, 1.0);
    textR(cr, xs[4], y, "INPUT", d->dim, 1.0);
    textR(cr, xs[3], y, "OUTPUT", d->dim, 1.0);
    if (xs[2]) textR(cr, xs[2], y, "CACHE R", d->dim, 1.0);
    if (xs[1]) textR(cr, xs[1], y, "CACHE W", d->dim, 1.0);
    textR(cr, xs[0], y, "CALLS", d->dim, 1.0);
}

// ---- the paint -------------------------------------------------------------------

// The theme, resolved from the config exactly as p_ui.c dashResolveTheme does.
// card = pinkBg blended 25% toward white; zebra = card blended 50% back to the
// page.
static void resolveTheme(Dash *d) {
    Rgb3 bg = d->bg;
    d->card = (Rgb3){ bg.r + (1 - bg.r) * 0.25, bg.g + (1 - bg.g) * 0.25,
                      bg.b + (1 - bg.b) * 0.25 };
    d->zebra = (Rgb3){ (d->card.r + bg.r) / 2.0, (d->card.g + bg.g) / 2.0,
                       (d->card.b + bg.b) / 2.0 };
}

static void paint(Dash *d) {
    double S = d->scale;
    cairo_t *cr = d->cr;

    // page: an OPAQUE fill inside the window's rounded shape (the board is a
    // solid panel, not a translucent bar; CAIRO_OPERATOR_SOURCE with an opaque
    // colour writes alpha = 1). The corners stay transparent so the shape's
    // staircase anti-aliases instead of clipping hard edges - the X11 twin of
    // Windows' DWMWCP_ROUND_NATIVE.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, d->bg.r, d->bg.g, d->bg.b, 1.0);
    roundedPath(cr, 0, 0, d->w, d->h, DASH_WIN_RADIUS * d->scale);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_new_path(cr);

    resolveTheme(d);

    double padL = DASH_PAD_L * S, padT = DASH_PAD_T * S, gap = DASH_GAP * S;

    // ---- titlebar: logo + title, refresh/close buttons, hairline divider
    double ty = padT;
    {
        double logoBox = 14 * S;
        Rgb3 acc = d->pinkDeep;
        svgDrawCairo(cr, SVG_DIAMOND, padL, ty + (20 * S - logoBox) / 2.0, logoBox,
                     acc.r, acc.g, acc.b, 1.0);
        setFont(d, cr, 15, 1);
        text(cr, padL + logoBox + 10 * S, ty + 12 * S, "Token Usage", d->fg, 1.0);

        // buttons right: [refresh] [close]
        double bx = d->w - padL;
        setFont(d, cr, 11, 0);
        const char *cl = "\u2715";
        double cw = textW(cr, cl) + 18 * S;
        d->btnCloseR = (int)bx;
        d->btnCloseL = (int)(bx - cw);
        d->btnCloseT = (int)(ty - 3 * S);
        d->btnCloseB = (int)(ty + 11 * S + 6 * S);
        bx -= cw + 6 * S;
        const char *rf = d->busy ? "..." : "refresh";
        double rw = textW(cr, "refresh") + 18 * S;
        d->btnRefreshR = (int)bx;
        d->btnRefreshL = (int)(bx - rw);
        d->btnRefreshT = (int)(ty - 3 * S);
        d->btnRefreshB = (int)(ty + 11 * S + 6 * S);
        bx -= rw + 6 * S;

        // centre each label in its frame on both axes (the Windows comment: GDI's
        // y is the top of the line box, so centre the ink block instead)
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        for (int bi = 1; bi <= 2; bi++) {
            int l = bi == 1 ? d->btnRefreshL : d->btnCloseL;
            int r = bi == 1 ? d->btnRefreshR : d->btnCloseR;
            int t = bi == 1 ? d->btnRefreshT : d->btnCloseT;
            int b = bi == 1 ? d->btnRefreshB : d->btnCloseB;
            const char *tx = bi == 1 ? rf : cl;
            // the cross comes from the SYMBOL face: the display family has no
            // U+2715 and cairo does not fall back (see ffSymbol)
            if (bi == 2) cairo_set_font_face(cr, d->ffSymbol);
            double txOff = ((r - l) - textW(cr, tx)) / 2.0;
            // baseline: centre the ink block (ascent + descent) in the frame
            double tyOff = (b - t) / 2.0 + (fe.ascent - fe.descent) / 2.0;
            if (d->btnHover == bi) {
                card(d, cr, l, t, r - l, b - t, 4 * S, d->bg, 1.0, d->pink);
                text(cr, l + txOff, t + tyOff, tx, d->pinkDeep, 1.0);
            } else {
                text(cr, l + txOff, t + tyOff, tx, d->dim, 1.0);
            }
        }
        d->tbBottom = (int)(d->btnRefreshB + 10 * S + 1);
        hairline(cr, padL, d->w - padL, d->tbBottom, d->divider, 1.0, 0);
    }

    double y = d->tbBottom + gap;
    double innerW = d->w - 2 * padL;
    // content-fit: every fit decision measures against the CONFIG height, not the
    // window's actual height, so the window can later shrink to its painted
    // content without the shrink dropping a row and oscillating (p_ui.c vh)
    double vh = d->cfgH * S;

    d->hmValid = 0;
    d->contentH = (int)(y + 38 * S + 14 * S);

    if (!cbCfgTokensEnabled() || (cbTokToday() < 0 && cbTokAll() == 0)) {
        // The two very different reasons for an empty board, in the Windows
        // wording: the master switch is off vs the cache file is unreadable.
        card(d, cr, padL, y, innerW, 38 * S, 8 * S, d->card, 0.85, d->divider);
        setFont(d, cr, 11, 0);
        const char *why = cbCfgTokensEnabled()
            ? "No token data. The token cache file could not be read \u2014 start Chocobar, or check tokens.cachePath."
            : "Token usage is off. Set tokens.enabled to true in the config to scan again.";
        text(cr, padL + 14 * S, y + 23 * S, why, d->dim, 1.0);
        d->contentH = (int)(y + 38 * S + 14 * S);
        return;
    }

    // ---- stat cards: Today / Last 7 days / Last 30 days / All time
    {
        double cw2 = (innerW - 3 * 10 * S) / 4.0;
        double chh = 52 * S;
        struct { const char *label; long long v; } cards[4] = {
            { "Today",         cbTokToday() < 0 ? 0 : cbTokToday() },
            { "Last 7 days",   cbTokWeek() },
            { "Last 30 days",  cbTokMonth() },
            { "All time",      cbTokAll() },
        };
        for (int i = 0; i < 4; i++) {
            double cx = padL + i * (cw2 + 10 * S);
            card(d, cr, cx, y, cw2, chh, 8 * S, d->card, 0.85, d->divider);
            setFont(d, cr, 10, 0);
            text(cr, cx + 12 * S, y + 20 * S, cards[i].label, d->dim, 1.0);
            char vs[32];
            fmtTok(cards[i].v, vs, sizeof(vs));
            setFont(d, cr, 17, 1);
            text(cr, cx + 12 * S, y + 44 * S, vs, d->fg, 1.0);
        }
        y += chh + gap;
    }

    // ---- DAILY USAGE: heatmap + legend (+ the day detail when a cell is picked)
    {
        double rowLabW = (16 + 5) * S;
        int weeks = DASH_HM_WEEKS;
        double secPadX = 12 * S;
        double cgap = 3 * S;
        double cell = (innerW - 2 * secPadX - rowLabW) / weeks - cgap;
        if (cell > 16 * S) cell = 16 * S;
        if (cell < 9 * S) cell = 9 * S;
        double pitch = cell + cgap;

        time_t now = time(NULL);
        struct tm nowSt;
        localtime_r(&now, &nowSt);
        int endDow = nowSt.tm_wday;              // 0 = Sun, as SYSTEMTIME.wDayOfWeek
        int days = cbTokDayCount();

        // quantile thresholds over the NONZERO days (dash.js renderHeatmap): the
        // ramp uses the full colour range whatever the absolute numbers are
        long long th[3] = { 1, 1, 1 };
        {
            long long nz[DASH_MAX_DAYS];
            int nnz = 0;
            for (int i = 0; i < days && nnz < DASH_MAX_DAYS; i++) {
                long long v = cbTokDayTotal(i);
                if (v > 0) nz[nnz++] = v;
            }
            for (int i = 1; i < nnz; i++) {
                long long v = nz[i];
                int j = i - 1;
                while (j >= 0 && nz[j] > v) { nz[j + 1] = nz[j]; j--; }
                nz[j + 1] = v;
            }
            if (nnz) { th[0] = nz[nnz / 4]; th[1] = nz[nnz / 2]; th[2] = nz[nnz * 3 / 4]; }
        }

        double hmH = 7 * pitch - cgap;
        int hasSel = d->daySel >= 0 && d->daySel < days;
        int ddAll = 0, ddRows = 0;
        if (hasSel) {
            // the selected day's apps: every row the detail could draw, then
            // how many of them fit (p_ui.c ddRows/ddAll)
            int napp = cbTokAppCount();
            for (int i = 0; i < napp; i++) if (cbTokDayAppReqs(d->daySel, i) > 0) ddAll++;
            ddRows = ddAll > DASH_DAY_DETAIL_ROWS ? DASH_DAY_DETAIL_ROWS : ddAll;
        }
        double ddFixed = (10 + 8 + 15 + 13) * S;
        double secH = (10 + 11 + 8 + 13) * S + hmH + 2 * S + 10 * S;
        if (hasSel) {
            // the detail's height is fitted to the room the tables still leave,
            // and the FIT GATE stays on the no-selection height: the detail
            // expands the card only while the expansion fits, else it is
            // clipped - a selection must never collapse the section (the hit
            // rects would go stale and keep firing)
            int tblReserve = (10 + 13 + 2 * 17 + 10) * S;
            double room = vh - 24 * S - gap - tblReserve - y - secH;
            int fitRows = (int)((room - ddFixed) / (17 * S));
            if (fitRows < 0) fitRows = 0;
            if (ddRows > fitRows) ddRows = fitRows;
            if (ddRows <= 0) hasSel = 0;
        }
        double ddH = hasSel ? ddFixed + ddRows * 17 * S : 0;

        if (y + secH < vh - 26 * S) {
            if (hasSel && y + secH + ddH < vh - 26 * S) secH += ddH;
            else hasSel = 0;
            card(d, cr, padL, y, innerW, secH, 8 * S, d->card, 0.85, d->divider);
            double hx = padL + secPadX;
            double hy = y + (10 + 11 + 8 + 13) * S;
            setFont(d, cr, 11, 1);
            text(cr, hx, y + 21 * S, "DAILY USAGE", d->dim, 1.0);

            // legend right: less [5 swatches] more
            {
                double sw2 = 10 * S, sgap = 3 * S;
                setFont(d, cr, 9, 0);
                double lw = textW(cr, "less") + 4 * S + 5 * sw2 + 4 * sgap + 4 * S +
                            textW(cr, "more");
                double lx = padL + innerW - secPadX - lw;
                double ly2 = y + 10 * S + (11 * S - sw2) / 2.0;
                text(cr, lx, ly2 + 8 * S, "less", d->dim, 1.0);
                double sx2 = lx + textW(cr, "less") + 4 * S;
                for (int i = 0; i < 5; i++) {
                    Rgb3 rc = d->ramp[i];
                    card(d, cr, sx2, ly2, sw2, sw2, 2.5 * S, rc, 1.0,
                         (Rgb3){ rc.r * 0.85, rc.g * 0.85, rc.b * 0.85 });
                    sx2 += sw2 + sgap;
                }
                text(cr, sx2, ly2 + 8 * S, "more", d->dim, 1.0);
            }

            // month labels along the top, one per month change
            static const char *MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
            setFont(d, cr, 9, 0);
            int lastMonth = -1;
            for (int wk = 0; wk < weeks; wk++) {
                int daysBack0 = (weeks - 1 - wk) * 7 + endDow;   // the dow=0 cell
                if (daysBack0 < 0 || daysBack0 >= days) continue;
                struct tm cd;
                time_t ct = now - (time_t)daysBack0 * 86400;
                localtime_r(&ct, &cd);
                if (cd.tm_mon != lastMonth && cd.tm_mday <= 21) {
                    lastMonth = cd.tm_mon;
                    text(cr, hx + rowLabW + wk * pitch, hy - 4 * S, MONTHS[cd.tm_mon],
                         d->dim, 1.0);
                }
            }
            // the cells: column per week, row per weekday, colour from the ramp
            for (int wk = 0; wk < weeks; wk++) {
                int daysBack0 = (weeks - 1 - wk) * 7 + endDow;
                for (int dow = 0; dow < 7; dow++) {
                    int daysBack = daysBack0 - dow;
                    double cx = hx + rowLabW + wk * pitch;
                    double cy = hy + dow * pitch;
                    d->hmL[wk][dow] = (int)cx;
                    d->hmT[wk][dow] = (int)cy;
                    d->hmR[wk][dow] = (int)(cx + cell);
                    d->hmB[wk][dow] = (int)(cy + cell);
                    d->hmBack[wk][dow] = daysBack;
                    if (daysBack < 0 || daysBack >= days) continue;   // future
                    long long tot = cbTokDayTotal(days - 1 - daysBack);
                    int lv = 0;
                    if (tot > 0) lv = (tot <= th[0]) ? 1 : (tot <= th[1]) ? 2 : (tot <= th[2]) ? 3 : 4;
                    Rgb3 cc = d->ramp[lv];
                    roundedPath(cr, cx, cy, cell, cell, 2.5 * S);
                    cairo_set_source_rgba(cr, cc.r, cc.g, cc.b, 1.0);
                    cairo_fill_preserve(cr);
                    cairo_set_source_rgba(cr, cc.r * 0.9, cc.g * 0.9, cc.b * 0.9, 1.0);
                    cairo_set_line_width(cr, 1.0);
                    cairo_stroke(cr);
                    cairo_new_path(cr);
                    if (daysBack == d->daySel) {
                        roundedPath(cr, cx - 1, cy - 1, cell + 2, cell + 2, 3 * S);
                        cairo_set_source_rgba(cr, d->pinkDeep.r, d->pinkDeep.g,
                                              d->pinkDeep.b, 1.0);
                        cairo_set_line_width(cr, 1.5 * S);
                        cairo_stroke(cr);
                        cairo_new_path(cr);
                    }
                }
            }
            d->hmValid = 1;
            // sparse row labels: Sun + Fri (p_ui.c draws exactly those two)
            setFont(d, cr, 9, 0);
            text(cr, hx, hy + 8 * S, "Sun", d->dim, 1.0);
            text(cr, hx, hy + 5 * pitch + 8 * S, "Fri", d->dim, 1.0);

            // day detail: the picked day's per-app rows
            if (hasSel) {
                double ddy = y + secH - 10 * S - ddH;
                hairline(cr, padL + secPadX, padL + innerW - secPadX, ddy, d->divider, 0.55, 1);
                ddy += 8 * S;
                int selIdx = days - 1 - d->daySel;
                long long tot = cbTokDayTotal(selIdx);
                struct tm d2;
                time_t dt = now - (time_t)d->daySel * 86400;
                localtime_r(&dt, &d2);
                static const char *DAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
                char dtitle[160], dnum[24];
                fmtTok(tot, dnum, sizeof(dnum));
                if (tot > 0)
                    snprintf(dtitle, sizeof(dtitle), "%s, %s %d, %d \u2014 %s tokens",
                             DAYS[d2.tm_wday % 7], MONTHS[d2.tm_mon], d2.tm_mday,
                             d2.tm_year + 1900, dnum);
                else
                    snprintf(dtitle, sizeof(dtitle), "%s, %s %d, %d \u2014 no usage",
                             DAYS[d2.tm_wday % 7], MONTHS[d2.tm_mon], d2.tm_mday,
                             d2.tm_year + 1900);
                setFont(d, cr, 11, 1);
                text(cr, padL + secPadX, ddy + 10 * S, dtitle, d->fg, 1.0);
                ddy += 15 * S;
                // the selected day's per-app rows (g_dayApp on Windows), capped at
                // the reserved count; each row is the same table row the app table
                // draws, so the detail reads as part of the same board
                RowAgg dayRows[128];
                int dayAppN = 0;
                int napp = cbTokAppCount();
                for (int i = 0; i < napp && dayAppN < 128; i++) {
                    int req = cbTokDayAppReqs(d->daySel, i);
                    if (req <= 0) continue;         // nothing that day: no row
                    dayRows[dayAppN].in = cbTokDayAppIn(d->daySel, i);
                    dayRows[dayAppN].out = cbTokDayAppOut(d->daySel, i);
                    dayRows[dayAppN].cr = cbTokDayAppCr(d->daySel, i);
                    dayRows[dayAppN].cw = cbTokDayAppCw(d->daySel, i);
                    dayRows[dayAppN].req = req;
                    dayAppN++;
                }
                int shownRows = 0;
                if (dayAppN > 0) {
                    // the day's own columns: cache columns only while that day has
                    // data in them (p_ui.c's per-day cacheR/cacheW)
                    int dCacheR = 0, dCacheW = 0;
                    for (int i = 0; i < dayAppN; i++) {
                        if (dayRows[i].cr > 0) dCacheR = 1;
                        if (dayRows[i].cw > 0) dCacheW = 1;
                    }
                    int xs[5] = { 0, 0, 0, 0, 0 };
                    int need[5] = { 0, 0, 0, 0, 0 };
                    for (int i = 0; i < dayAppN; i++) tableRowNeeds(d, cr, &dayRows[i], need);
                    tableHeadNeeds(d, cr, need);
                    tableCols((int)(padL + innerW - secPadX), dCacheR, dCacheW, need, xs);
                    setFont(d, cr, 9, 0);
                    tableHead(d, cr, padL + secPadX, ddy + 9 * S, xs, "APP");
                    ddy += 13 * S;
                    // the reserve holds the fitted rows: when the day has more
                    // apps, the last slot carries an overflow line instead of a
                    // silently dropped row (p_ui.c ddLimit)
                    int ddLimit = dayAppN > ddRows ? ddRows - 1 : ddRows;
                    int k = 0;
                    for (int i = 0; i < napp && shownRows < ddLimit; i++) {
                        int req = cbTokDayAppReqs(d->daySel, i);
                        if (req <= 0) continue;
                        tableRow(d, cr, padL + secPadX, innerW - 2 * secPadX, ddy,
                                 17 * S, xs, cbTokAppName(i),
                                 appDotColor(cbTokAppName(i), d), 0.0, &dayRows[k]);
                        shownRows++;
                        k++;
                        ddy += 17 * S;
                    }
                    if (shownRows < dayAppN) {
                        char more[48];
                        snprintf(more, sizeof(more), "+%d more", dayAppN - shownRows);
                        setFont(d, cr, 10, 0);
                        text(cr, padL + secPadX + 2 + 14 * S, ddy + 10 * S, more,
                             d->dim, 1.0);
                    }
                } else {
                    setFont(d, cr, 10, 0);
                    text(cr, padL + secPadX, ddy + 10 * S, "no usage recorded on this day",
                         d->dim, 1.0);
                }
            }
            y += secH + gap;
        }
    }

    // ---- the two tables, side by side: BY APP and BY MODEL
    {
        double colW2 = (innerW - 10 * S) / 2.0;
        double secPad = 10 * S;
        double th2 = (9 + 4) * S;
        double rowH = 19 * S;

        int appN = cbTokAppCount();
        RowAgg apps[128];
        if (appN > 128) appN = 128;
        for (int i = 0; i < appN; i++) {
            apps[i].in = cbTokAppIn(i);
            apps[i].out = cbTokAppOut(i);
            apps[i].cr = cbTokAppCr(i);
            apps[i].cw = cbTokAppCw(i);
            apps[i].req = cbTokAppReqs(i);
        }
        int appOrder[128];
        for (int i = 0; i < appN; i++) appOrder[i] = i;
        for (int i = 1; i < appN; i++) {
            int v = appOrder[i], j = i - 1;
            long long vt = apps[v].in + apps[v].out + apps[v].cr + apps[v].cw;
            while (j >= 0) {
                int u = appOrder[j];
                long long ut = apps[u].in + apps[u].out + apps[u].cr + apps[u].cw;
                if (ut >= vt) break;
                appOrder[j + 1] = appOrder[j];
                j--;
            }
            appOrder[j + 1] = v;
        }

        int mdlAll = cbTokModelCount();
        RowAgg models[128];
        char modelLabels[128][64];
        if (mdlAll > 128) mdlAll = 128;
        // the cap the WINDOWS board applies: p_agg's own DASH_MODEL_ROWS through
        // the accessor it publishes for this, so both boards show the same rows
        // for the same store. The remainder is the "+N more models" line.
        int mdlN = cbTokModelRowCap();
        if (mdlN > mdlAll) mdlN = mdlAll;
        // the same INPUT / OUTPUT / CACHE R / CACHE W / CALLS columns the app
        // table shows: both tables read the same record shape, so the board
        // cannot tell two different stories about one token record
        for (int i = 0; i < mdlAll; i++) {
            models[i].in = cbTokModelIn(i);
            models[i].out = cbTokModelOut(i);
            models[i].cr = cbTokModelCr(i);
            models[i].cw = cbTokModelCw(i);
            models[i].req = cbTokModelReqs(i);
            snprintf(modelLabels[i], 64, "%s", cbTokModelLabel(i));
        }
        int mdlOrder[128];
        // sort the full set, cap only the displayed rows: every slot the sort
        // touches must be defined, exactly as p_ui.c does
        for (int i = 0; i < mdlAll; i++) mdlOrder[i] = i;
        for (int i = 1; i < mdlAll; i++) {
            int v = mdlOrder[i], j = i - 1;
            long long vt = cbTokModelSum(v);
            while (j >= 0) {
                int u = mdlOrder[j];
                if (cbTokModelSum(u) >= vt) break;
                mdlOrder[j + 1] = mdlOrder[j];
                j--;
            }
            mdlOrder[j + 1] = v;
        }

        // cache columns are per TABLE and per COLUMN (p_ui.c appCacheR/W): an
        // all-zero column is dropped, never drawn empty
        int appCacheR = 0, appCacheW = 0;
        for (int i = 0; i < appN; i++) {
            if (apps[i].cr > 0) appCacheR = 1;
            if (apps[i].cw > 0) appCacheW = 1;
        }
        int mdlCacheR = 0, mdlCacheW = 0;
        // the cap is applied to the SORTED order, so every decision about the
        // model table's rows must read mdlOrder too: models[i] is the first N by
        // index, which is not the set the table draws
        for (int i = 0; i < mdlN; i++) {
            if (models[mdlOrder[i]].cr > 0) mdlCacheR = 1;
            if (models[mdlOrder[i]].cw > 0) mdlCacheW = 1;
        }

        // rows that fit: reserve the "+N more models" line, then settle the two
        // together (the Windows comment: reserving the line can cost a row, and
        // losing a row can create the overflow, so it converges in 2 passes)
        int rows = appN > mdlN ? appN : mdlN;
        double moreH = 0;
        double budget = 0;
        for (int pass = 0; pass < 2; pass++) {
            budget = vh - 24 * S - y - secPad - th2 - moreH - secPad;
            int fitMax = (int)(budget / (17 * S));
            if (rows > fitMax) rows = fitMax;
            if (rows < 0) rows = 0;
            int mdlRows = mdlN < rows ? mdlN : rows;
            moreH = mdlAll > mdlRows ? 14 * S : 0;
        }
        if (rows > 0) {
            rowH = budget / rows;
            if (rowH > 19 * S) rowH = 19 * S;
            if (rowH < 17 * S) rowH = 17 * S;
        }
        double tblH = secPad + th2 + rows * rowH + moreH + secPad;

        if (y + tblH <= vh - 24 * S) {
            for (int side = 0; side < 2; side++) {
                double sx = padL + side * (colW2 + 10 * S);
                card(d, cr, sx, y, colW2, tblH, 8 * S, d->card, 0.85, d->divider);
                double inner = colW2 - 24 * S;
                int xs[5] = { 0, 0, 0, 0, 0 };
                int need[5] = { 0, 0, 0, 0, 0 };
                if (side == 0) {
                    for (int i = 0; i < appN; i++) tableRowNeeds(d, cr, &apps[i], need);
                } else {
                    for (int i = 0; i < mdlN; i++)
                        tableRowNeeds(d, cr, &models[mdlOrder[i]], need);
                }
                tableHeadNeeds(d, cr, need);
                tableCols((int)(sx + 12 * S + inner), side == 0 ? appCacheR : mdlCacheR,
                          side == 0 ? appCacheW : mdlCacheW, need, xs);
                double ry = y + secPad;
                setFont(d, cr, 9, 0);
                tableHead(d, cr, sx + 12 * S, ry + 9 * S, xs, side == 0 ? "APP" : "MODEL");
                ry += th2;
                if (side == 0) {
                    long long maxAll = 1;
                    for (int i = 0; i < appN; i++) {
                        long long s2 = apps[i].in + apps[i].out + apps[i].cr + apps[i].cw;
                        if (s2 > maxAll) maxAll = s2;
                    }
                    int appRows = appN < rows ? appN : rows;
                    for (int i = 0; i < appRows; i++) {
                        int ai = appOrder[i];
                        if (i & 1) {
                            cairo_set_source_rgba(cr, d->zebra.r, d->zebra.g, d->zebra.b, 1.0);
                            cairo_rectangle(cr, sx + 1, ry, colW2 - 2, rowH);
                            cairo_fill(cr);
                            cairo_new_path(cr);
                        }
                        long long s2 = apps[ai].in + apps[ai].out + apps[ai].cr + apps[ai].cw;
                        tableRow(d, cr, sx + 12 * S, inner, ry, rowH, xs,
                                 cbTokAppName(ai), appDotColor(cbTokAppName(ai), d),
                                 (double)s2 / (double)maxAll, &apps[ai]);
                        ry += rowH;
                    }
                    if (appN == 0 && rows > 0) {
                        setFont(d, cr, 11, 0);
                        text(cr, sx + 12 * S, ry + 13 * S, "No usage recorded yet.",
                             d->dim, 1.0);
                    }
                } else {
                    long long maxAll = 1;
                    for (int i = 0; i < mdlN; i++) {
                        long long s2 = cbTokModelSum(mdlOrder[i]);
                        if (s2 > maxAll) maxAll = s2;
                    }
                    int mdlRows = mdlN < rows ? mdlN : rows;
                    for (int i = 0; i < mdlRows; i++) {
                        int mi = mdlOrder[i];
                        if (i & 1) {
                            cairo_set_source_rgba(cr, d->zebra.r, d->zebra.g, d->zebra.b, 1.0);
                            cairo_rectangle(cr, sx + 1, ry, colW2 - 2, rowH);
                            cairo_fill(cr);
                            cairo_new_path(cr);
                        }
                        long long s2 = cbTokModelSum(mi);
                        tableRow(d, cr, sx + 12 * S, inner, ry, rowH, xs,
                                 modelLabels[mi], d->dim, (double)s2 / (double)maxAll,
                                 &models[mi]);
                        ry += rowH;
                    }
                    if (mdlAll > mdlRows) {
                        char more[48];
                        snprintf(more, sizeof(more), "+%d more models", mdlAll - mdlRows);
                        setFont(d, cr, 10, 0);
                        text(cr, sx + 12 * S + 2 + 14 * S, ry + 10 * S, more, d->dim, 1.0);
                    }
                }
            }
            y += tblH + gap;
        }
    }

    // footer: the last scan time, right-aligned under the content (not glued to
    // the window's bottom edge, which the content-fit resize moves)
    {
        char fl[64];
        struct tm lt;
        time_t last = (time_t)d->lastScanAt;
        localtime_r(&last, &lt);
        if (d->lastScanAt)
            snprintf(fl, sizeof(fl), "last scan %02d:%02d:%02d", lt.tm_hour, lt.tm_min,
                     lt.tm_sec);
        else
            snprintf(fl, sizeof(fl), "last scan \u2014");
        setFont(d, cr, 9, 0);
        double fy = y;
        if (fy > d->h - 16 * S) fy = d->h - 16 * S;
        textR(cr, d->w - padL, fy + 9 * S, fl, d->dim, 0.9);
        d->contentH = (int)(fy + 10 * S + 12 * S);
    }
}

// ---- window lifetime ---------------------------------------------------------------

// Rebuild the window shape from the current size. p_ui.c dashRoundCorners asks
// DWM for DWMWCP_ROUND_NATIVE; this is the X11 twin, using the same shared
// staircase the bar and the menu use. Called after any resize, because a stale
// shape clips the new size.
static void dashReshape(Dash *d) {
    if (!d->dpy || !d->win) return;
    double r = DASH_WIN_RADIUS * d->scale;
    if (r < 0) r = 0;
    if (r > d->w / 2.0) r = d->w / 2.0;
    if (r > d->h / 2.0) r = d->h / 2.0;
    if (d->shape && d->shapeR == (int)r) return;
    XRectangle rects[80];
    int nr = roundedRectScanlines(rects, 80, d->w, d->h, r);
    if (nr <= 0) return;
    if (d->shape) XFixesDestroyRegion(d->dpy, d->shape);
    d->shape = XFixesCreateRegion(d->dpy, rects, nr);
    XFixesSetWindowShapeRegion(d->dpy, d->win, ShapeBounding, 0, 0, d->shape);
    d->shapeR = (int)r;
}

// ---- content fit ---------------------------------------------------------------
// p_ui.c dashFitToContent: after a paint, the window takes the painted content's
// height, clamped to 45% of the configured height (a board that is shorter than
// that reads as a stub) and to the screen minus a margin (no scrollbar, no size
// grip: a panel pushed past the bottom edge is unreachable). The height only
// ever shrinks here, so the shrink cannot drop a row and oscillate - which is
// why every fit decision in the paint measures against d->cfgH, not d->h.
//
// Windows does it before ShowWindow so the board never appears at the wrong
// height; on X the equivalent is to paint before the map, which is what the
// open path does below.
static void dashFitToContent(void) {
    Dash *d = &g_dash;
    if (!d->win || d->contentH <= 0) return;
    int want = d->contentH;
    int lo = (int)(d->cfgH * d->scale * 45 / 100);
    if (want < lo) want = lo;
    // the containing head's work-area bottom, in device px: Xinerama-aware,
    // because the board can live on any head. Both the height clamp and the
    // move-up below read this one value.
    int headBottom = 0;
    if (d->dpy) {
        headBottom = DisplayHeight(d->dpy, DefaultScreen(d->dpy)) - 40;
        int n = 0;
        XineramaScreenInfo *xi = XineramaQueryScreens(d->dpy, &n);
        if (xi && n > 0) {
            for (int i = 0; i < n; i++) {
                if (xi[i].x_org <= d->x && d->x < xi[i].x_org + xi[i].width &&
                    xi[i].y_org <= d->y && d->y < xi[i].y_org + xi[i].height) {
                    headBottom = xi[i].y_org + xi[i].height - 40;
                    break;
                }
            }
            XFree(xi);
        }
    }
    if (headBottom <= 0) return;
    // keep the whole board on screen: p_ui.c moves the window up to the head's
    // work-area bottom before sizing it, so a board whose content reaches the
    // clamp still shows its last row (and the "last scan" footer) instead of
    // having it clipped off the bottom edge. The move MUST come before the
    // clamp: clamping to the room at the OLD position would make the move below
    // unreachable, which is the bug this order fixes.
    if (d->y + want > headBottom) {
        d->y = headBottom - want;
        if (d->y < 0) d->y = 0;
    }
    int hi = headBottom - d->y;
    if (hi <= 0) return;
    if (want > hi) want = hi;
    if (d->dpy) {
        int scr = DefaultScreen(d->dpy);
        int sw = DisplayWidth(d->dpy, scr);
        if (d->x + d->w > sw) d->x = sw - d->w > 0 ? sw - d->w : 0;
    }
    if (d->h != want) {
        d->h = want;
        if (d->surf) {
            cairo_destroy(d->cr);
            cairo_surface_destroy(d->surf);
            d->surf = cairo_xlib_surface_create(d->dpy, d->win, d->visual, d->w, d->h);
            d->cr = cairo_create(d->surf);
        }
        XResizeWindow(d->dpy, d->win, (unsigned)d->w, (unsigned)d->h);
        dashReshape(d);
    }
}

Window dashOpen(Display *dpy, int screen, Visual *visual, Colormap cmap, int depth,
                int cssW, int cssH, double scale, const char *fontFamily,
                const double page[3], const double card[3], const double fg[3],
                const double dim[3], const double divider[3], const double ramp[5][3],
                const double pink[3], const double pinkDeep[3],
                const double yellow[3], long long lastScanAt) {
    // A SECOND open must never leak the first board. The struct below is
    // memset, so opening over a live board used to forget the old window id and
    // leave a mapped, unclosable window behind - every extra click on the tokens
    // chip added one, which is the "I can open so many dashboards and they crash"
    // report. Close first, always.
    if (g_boardOpen) dashClose();
    Dash *d = &g_dash;
    memset(d, 0, sizeof(*d));
    d->dpy = dpy; d->visual = visual;
    d->scale = scale > 0 ? scale : 1.0;
    snprintf(d->fontFamily, sizeof(d->fontFamily), "%s", fontFamily ? fontFamily : "sans");
    d->bg = (Rgb3){ page[0], page[1], page[2] };
    d->card = (Rgb3){ card[0], card[1], card[2] };
    d->fg = (Rgb3){ fg[0], fg[1], fg[2] };
    d->dim = (Rgb3){ dim[0], dim[1], dim[2] };
    d->divider = (Rgb3){ divider[0], divider[1], divider[2] };
    d->pink = (Rgb3){ pink[0], pink[1], pink[2] };
    d->pinkDeep = (Rgb3){ pinkDeep[0], pinkDeep[1], pinkDeep[2] };
    d->yellow = (Rgb3){ yellow[0], yellow[1], yellow[2] };
    for (int i = 0; i < 5; i++) d->ramp[i] = (Rgb3){ ramp[i][0], ramp[i][1], ramp[i][2] };
    d->daySel = -1;
    d->cfgH = cssH;
    d->lastScanAt = lastScanAt;

    // the board's two faces: the configured display family, UPRIGHT (p_ui.c's
    // dashFont passes lfItalic = FALSE - Segoe Print's lean is in the glyphs),
    // and its bold. The CSS-600 elements are the Regular face because the
    // family has no true semibold - so the bold face here is the real bold,
    // used only where the Windows board asks for FW_BOLD. The port used to ask
    // for Italic, which cairo synthesizes into an oblique for an upright face.
    d->ffReg = cairo_toy_font_face_create(d->fontFamily, CAIRO_FONT_SLANT_NORMAL,
                                          CAIRO_FONT_WEIGHT_NORMAL);
    d->ffBold = cairo_toy_font_face_create(d->fontFamily, CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_BOLD);
    // The symbol face: whatever fontconfig resolves for a family that carries
    // U+2715. DejaVu Sans is on every desktop this bar runs on and has it; the
    // lookup is the same FcNameParse path resolveFontFamily uses in main.c, so
    // a machine without DejaVu still gets a real match instead of tofu.
    {
        char symFam[128] = "DejaVu Sans";
        FcPattern *pat = FcNameParse((const FcChar8 *)"DejaVu Sans");
        if (pat) {
            FcPatternAddDouble(pat, FC_SIZE, 12.0);
            FcConfigSubstitute(NULL, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);
            FcResult res = FcResultNoMatch;
            FcPattern *m = FcFontMatch(NULL, pat, &res);
            if (m && res == FcResultMatch) {
                FcChar8 *fam = NULL;
                if (FcPatternGetString(m, FC_FAMILY, 0, &fam) == FcResultMatch && fam)
                    snprintf(symFam, sizeof(symFam), "%s", (const char *)fam);
            }
            if (m) FcPatternDestroy(m);
            FcPatternDestroy(pat);
        }
        d->ffSymbol = cairo_toy_font_face_create(symFam, CAIRO_FONT_SLANT_NORMAL,
                                                 CAIRO_FONT_WEIGHT_NORMAL);
    }

    d->w = (int)(cssW * d->scale + 0.5);
    d->h = (int)(cssH * d->scale + 0.5);

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
                     LeaveWindowMask;
    d->x = 60; d->y = 60;
    d->dpy = dpy;
    d->win = XCreateWindow(dpy, RootWindow(dpy, screen), d->x, d->y,
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
    sh.min_width = (int)(520 * d->scale);
    sh.min_height = (int)(380 * d->scale);
    XSetWMNormalHints(dpy, d->win, &sh);

    // persistent paint state: the board repaints on every hover move
    d->surf = cairo_xlib_surface_create(dpy, d->win, visual, d->w, d->h);
    d->cr = cairo_create(d->surf);
    dashReshape(d);

    // p_ui.c paints while the window is still hidden and fits BEFORE ShowWindow,
    // because resizing after the board is on screen reads as "bolted together"
    // (the top appears, then the lower part lags in). X keeps the same ordering.
    paint(d);
    cairo_surface_flush(d->surf);
    dashFitToContent();
    XMoveResizeWindow(dpy, d->win, d->x, d->y, (unsigned)d->w, (unsigned)d->h);
    XMapWindow(dpy, d->win);
    XFlush(dpy);

    g_boardOpen = 1;
    return d->win;
}

Window dashWin(void) {
    return g_boardOpen ? g_dash.win : None;
}

// The board wants to close: Esc, q, a non-left click, the WM's close button, or
// the window already being gone. Returns 1 and lets the caller call dashClose().
static int boardClose(XEvent *ev) {
    Dash *d = &g_dash;
    switch (ev->type) {
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev->xkey, 0);
            return ks == XK_Escape || ks == XK_q;
        }
        case ButtonPress:
            // a middle/right click also closes the window (p_ui.c dashProc
            // WM_RBUTTONUP destroys the board)
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

// which titlebar button is under the pointer, 0 = none (p_ui.c's g_btnHover)
static int buttonAt(Dash *d, int x, int y) {
    if (x >= d->btnRefreshL && x < d->btnRefreshR &&
        y >= d->btnRefreshT && y < d->btnRefreshB) return 1;
    if (x >= d->btnCloseL && x < d->btnCloseR &&
        y >= d->btnCloseT && y < d->btnCloseB) return 2;
    return 0;
}

// the heatmap cell under the pointer, as days-back; -1 = none
static int heatCellAt(Dash *d, int x, int y) {
    if (!d->hmValid) return -1;
    for (int wk = 0; wk < DASH_HM_WEEKS; wk++)
        for (int dow = 0; dow < 7; dow++)
            if (x >= d->hmL[wk][dow] && x < d->hmR[wk][dow] &&
                y >= d->hmT[wk][dow] && y < d->hmB[wk][dow])
                return d->hmBack[wk][dow];
    return -1;
}

// p_ui.c dashTipCell: the heat card for the cell under the pointer - the
// weekday/date, that day's token total and its per-app lines - shown on every
// motion, hidden the moment the pointer is not over a real cell. Root
// coordinates, not window ones: tipShow places the box on the screen.
static void dashTipCell(Dash *d, int x, int y, int xRoot, int yRoot) {
    int days = cbTokDayCount();
    int back = heatCellAt(d, x, y);
    if (back < 0 || back >= days) { tipHide(); return; }
    int di = days - 1 - back;
    long long tot = cbTokDayTotal(di);
    struct tm dt;
    time_t ts = time(NULL) - (time_t)back * 86400;
    localtime_r(&ts, &dt);
    static const char *DAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char tip[704], num[24];
    fmtTok(tot, num, sizeof(num));
    if (tot > 0) {
        int n = snprintf(tip, sizeof(tip), "%s, %s %d, %d\n%s tokens",
                         DAYS[dt.tm_wday % 7], MONTHS[dt.tm_mon], dt.tm_mday,
                         dt.tm_year + 1900, num);
        // per-app lines, capped so the card stays a card and the last slot
        // carries the remainder instead of silently dropping apps
        int napp = cbTokAppCount(), skipped = 0;
        for (int i = 0; i < napp; i++) {
            int req = cbTokDayAppReqs(back, i);
            if (req <= 0) continue;
            long long s = cbTokDayAppIn(back, i) + cbTokDayAppOut(back, i) +
                          cbTokDayAppCr(back, i) + cbTokDayAppCw(back, i);
            char sn[24], line[80];
            fmtTok(s, sn, sizeof(sn));
            int ln = snprintf(line, sizeof(line), "\n%s: %s (%d)",
                              cbTokAppName(i), sn, req);
            if (n < 0 || ln < 0 || (size_t)(n + ln) >= sizeof(tip) - 32) { skipped++; continue; }
            memcpy(tip + n, line, (size_t)ln);
            n += ln;
            tip[n] = 0;
        }
        if (skipped) {
            char more[32];
            int mn = snprintf(more, sizeof(more), "\n+%d more", skipped);
            if (n >= 0 && mn > 0 && (size_t)(n + mn) < sizeof(tip)) {
                memcpy(tip + n, more, (size_t)mn);
                n += mn;
                tip[n] = 0;
            }
        }
    } else {
        snprintf(tip, sizeof(tip), "%s, %s %d, %d\nno usage",
                  DAYS[dt.tm_wday % 7], MONTHS[dt.tm_mon], dt.tm_mday,
                  dt.tm_year + 1900);
    }
    tipShow(tip, xRoot, yRoot, d->fontFamily, 10.0, 1);
}

int dashEvent(XEvent *ev) {
    if (!g_boardOpen) return 0;
    Dash *d = &g_dash;
    if (boardClose(ev)) return 1;
    switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) {
                paint(d);
                cairo_surface_flush(d->surf);
                XFlush(d->dpy);
            }
            break;
        case ConfigureNotify:
            if (ev->xconfigure.width != d->w || ev->xconfigure.height != d->h) {
                d->w = ev->xconfigure.width;
                d->h = ev->xconfigure.height;
                cairo_xlib_surface_set_size(d->surf, d->w, d->h);
                paint(d);
                cairo_surface_flush(d->surf);
                XFlush(d->dpy);
            }
            break;
        case MotionNotify: {
            int b = buttonAt(d, ev->xmotion.x, ev->xmotion.y);
            if (b != d->btnHover) {
                d->btnHover = b;
                paint(d);
                cairo_surface_flush(d->surf);
                XFlush(d->dpy);
            }
            // the heat card, the board's own tooltip (p_ui.c dashTipCell on every
            // WM_MOUSEMOVE): the weekday/date, that day's token total and its
            // per-app lines, in the dark rounded card the Windows board shows
            dashTipCell(d, ev->xmotion.x, ev->xmotion.y,
                        ev->xmotion.x_root, ev->xmotion.y_root);
            break;
        }
        case LeaveNotify:
            if (d->btnHover) {
                d->btnHover = 0;
                paint(d);
                cairo_surface_flush(d->surf);
                XFlush(d->dpy);
            }
            tipHide();
            break;
        case ButtonRelease:
            // the board's titlebar buttons act on the RELEASE inside their own
            // rect, the bar's own rule (a press that wandered off must not fire)
            if (ev->xbutton.button == 1) {
                int b = buttonAt(d, ev->xbutton.x, ev->xbutton.y);
                if (b == 2) return 1;                    // close
                if (b == 1) {
                    // refresh: the Windows button re-scans the token cache
                    extern long long cbScanTokens(void);
                    d->busy = 1;
                    paint(d);
                    cairo_surface_flush(d->surf);
                    XFlush(d->dpy);
                    cbScanTokens();
                    d->busy = 0;
                    d->lastScanAt = time(NULL);
                    paint(d);
                    cairo_surface_flush(d->surf);
                    XFlush(d->dpy);
                }
            }
            break;
        case ButtonPress:
            // a left click on a heatmap cell selects that day (p_ui.c's
            // g_daySel); a click anywhere else deselects it
            if (ev->xbutton.button == 1) {
                int back = heatCellAt(d, ev->xbutton.x, ev->xbutton.y);
                if (back != d->daySel) {
                    d->daySel = back;
                    paint(d);
                    cairo_surface_flush(d->surf);
                    XFlush(d->dpy);
                }
            }
            break;
        default: break;
    }
    return 0;
}

void dashClose(void) {
    if (!g_boardOpen) return;
    Dash *d = &g_dash;
    g_boardOpen = 0;
    tipHide();   // the board's own heat card must not outlive the board
    if (d->cr) { cairo_destroy(d->cr); d->cr = NULL; }
    if (d->surf) { cairo_surface_destroy(d->surf); d->surf = NULL; }
    if (d->shape) { XFixesDestroyRegion(d->dpy, d->shape); d->shape = 0; }
    if (d->ffReg) { cairo_font_face_destroy(d->ffReg); d->ffReg = NULL; }
    if (d->ffBold) { cairo_font_face_destroy(d->ffBold); d->ffBold = NULL; }
    if (d->ffSymbol) { cairo_font_face_destroy(d->ffSymbol); d->ffSymbol = NULL; }
    // DestroyNotify may have already taken the window; XDestroyWindow on a dead
    // window only raises BadWindow, which the error handler ignores.
    XDestroyWindow(d->dpy, d->win);
    XFlush(d->dpy);
    // the bar's own surface is still valid; the caller repaints it
}
