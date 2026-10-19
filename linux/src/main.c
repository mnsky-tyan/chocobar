// linux/src/main.c - the Linux Chocobar bar.
//
// WHAT THIS IS
// A small always-on-top, frameless, translucent strip that floats over the
// desktop and shows the same chips as the Windows bar: cpu, ram, cpu temp,
// volume, battery, clock, plus the token count the shared scanner produces.
// It does NOT follow a terminal (that was a Wayland-shaped problem worth
// skipping: see the README's Linux section); it sits where it is put and
// remembers that position.
//
// HOW IT PAINTS
// The Windows bar draws into a premultiplied 32bpp DIB and hands it to
// UpdateLayeredWindow. The Linux equivalent is an override-redirect X11 window
// whose background is an XRender picture built from a cairo ARGB32 surface:
//  - override-redirect: the window manager never manages it, so it has no
//    title bar, no taskbar entry, never takes focus, and sits exactly where we
//    put it. That is the same role WS_EX_LAYERED + WS_EX_NOACTIVATE plays on
//    Windows, and it is what makes a "floating bar" trivial instead of a
//    protocol negotiation.
//  - cairo draws with real anti-aliasing, so the rounded corners and the icons
//    are smooth; the window is shaped to the same rounded rect so the pixels
//    outside it are genuinely not part of the window (no black corner squares).
//  - XWayland presents it on KWin here; on an X11 session it is a plain window.
//
// The bar is one process, ~1 timer, no worker threads: the shared scan is called
// from the same loop that repaints, exactly like the macOS bar's timer.

#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <stdarg.h>
#include <cairo/cairo-xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>

#include "chocobar_core.h"
#include "svg_geometry.h"   // the shared icon ids (SVG_CPU, ...)
#include "linux_metrics.h"
#include "geomdbus.h"
#include "menu.h"
#include "dash.h"
#include "linux_svg.h"

// ---- core seam (the CHOCOBAR_PORTABLE accessors in p_agg.c) --------------------
extern void cbCoreInit(void);
extern void cbTokensInit(void);
extern int cbLoadConfig(const wchar_t *path);
extern long long cbScanTokens(void);
extern long long cbTokAll(void);
extern int cbTokAppCount(void);
extern const char *cbTokAppName(int i);
extern long long cbTokAppSum(int i);
extern int cbCfgTokensEnabled(void);
extern int cbCfgRescanSec(void);
extern int cbCfgHeight(void);
extern int cbCfgGap(void);
extern int cbCfgFontSize(void);
extern int cbCfgAlign(void);
extern long long cbTokToday(void);
extern long long cbTokWeek(void);
extern long long cbTokMonth(void);
extern int cbCfgDashW(void);
extern int cbCfgDashH(void);
extern const wchar_t *cbCfgPinkBg(void);
extern const wchar_t *cbCfgHeat(int i);
extern int cbCfgBarRadius(void);
extern int cbCfgBgAlpha(void);
extern int cbCfgIconOpacity(void);
extern const wchar_t *cbCfgTint(void);
extern const wchar_t *cbCfgFontFamily(void);
extern const wchar_t *cbCfgClockFormat(void);
extern const wchar_t *cbCfgFg(void);
extern const wchar_t *cbCfgFgDim(void);
extern const wchar_t *cbCfgPink(void);
extern const wchar_t *cbCfgPinkDeep(void);
extern const wchar_t *cbCfgWarn(void);
extern const wchar_t *cbCfgGood(void);
extern const wchar_t *cbCfgYellow(void);
extern const wchar_t *cbCfgDivider(void);
extern const wchar_t *cbCfgIconColor(void);
extern int cbCfgModCpu(void);
extern int cbCfgModRam(void);
extern int cbCfgModTemp(void);
extern int cbCfgModVol(void);
extern int cbCfgModBatt(void);
extern int cbCfgModClock(void);
extern int cbCfgModGpu(void);
extern int cbCfgCpuWarnAt(void);
extern int cbCfgRamWarnAt(void);
extern int cbCfgTempWarnAt(void);

// ---- colours ------------------------------------------------------------------
typedef struct { double r, g, b; } Rgb;

// #RRGGBB (the config's format) -> linear-ish sRGB components cairo wants (0..1)
static Rgb hexRgb(const wchar_t *hex, Rgb fallback) {
    if (!hex) return fallback;
    unsigned v = 0;
    int n = 0;
    for (const wchar_t *p = hex; *p && n < 6; p++) {
        int d = -1;
        if (*p >= L'0' && *p <= L'9') d = *p - L'0';
        else if (*p >= L'a' && *p <= L'f') d = *p - L'a' + 10;
        else if (*p >= L'A' && *p <= L'F') d = *p - L'A' + 10;
        else if (n == 0 && *p == L'#') continue;
        else break;
        v = (v << 4) | (unsigned)d;
        n++;
    }
    if (n != 6) return fallback;
    Rgb c;
    c.r = ((v >> 16) & 0xFF) / 255.0;
    c.g = ((v >> 8) & 0xFF) / 255.0;
    c.b = (v & 0xFF) / 255.0;
    return c;
}

static void setSrc(cairo_t *cr, Rgb c, double a) { cairo_set_source_rgba(cr, c.r, c.g, c.b, a); }

// The rounded rectangle, built once. It was written out as the same four
// cairo_arc quarter-calls in five places across the front-end (the bar strip,
// the hover pill, the menu background and pill, the battery fill and the
// dashboard cards); one helper keeps them from drifting apart.
void roundedPath(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    if (r <= 0) { cairo_rectangle(cr, x, y, w, h); return; }
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

// ---- global state ---------------------------------------------------------------
static Display *g_dpy;
static Window g_win;
static int g_screen;
static Visual *g_visual;
static Colormap g_cmap;
static int g_depth;
static cairo_surface_t *g_surf;
static cairo_t *g_cr;
static XRenderPictFormat *g_pictFmt;
static Picture g_pic;
static XserverRegion g_region;

static int g_x, g_y;               // window position (pixels, derived: never dragged)
static int g_w, g_h;               // window size (pixels)
static double g_scale = 1.0;       // DPI scale: config values are CSS px
static int g_hover = -1;            // chip under the pointer, -1 = none
static volatile sig_atomic_t g_quit = 0;
// ---- right-click menu and dashboard state -----------------------------------------
static int g_menuPending = 0;
static int g_menuX = 0, g_menuY = 0;
static int g_dashPending = 0;   // 1 = token dashboard, 2 = subscriptions


static LinuxMetrics g_m;
static time_t g_lastScan = 0;
static long long g_tokToday = -1;
static int g_tokReady = 0;

#define MAX_CHIPS 16
// hover pill geometry for a clickable left-group chip (CSS px, like the rest)
#define HOVER_PAD_X 6.0
#define HOVER_PAD_Y 3.0
#define HOVER_RADIUS 7.0
// align values are the WINDOWS spelling (p_ui.c:110): 1 = right group (metrics,
// growing leftward from the right edge), 2 = left pinned group (tokens, subs,
// custom buttons, growing rightward from the left edge). Same names, same
// numbers, so the two renderers read the same way.
#define CHIP_RIGHT 1
#define CHIP_LEFT 2

typedef struct {
    int icon;                 // SVG id, -1 = text only
    wchar_t text[64];
    Rgb color;                // value colour
    int align;                // CHIP_LEFT or CHIP_RIGHT
    Rgb iconColor;            // icon colour, used only when hasIconColor
    int hasIconColor;         // 0 = inherit the shared icon colour
} Chip;
static Chip g_chips[MAX_CHIPS];
static int g_chipCount = 0;

// defined with the event handlers below; used by the painter
static int chipClickable(const Chip *c);
static void paint(void);

// geometry, in CSS px, matching the Windows bar's own numbers (p_ui.c paintBar)
#define PAD_L 16.0
#define PAD_R 18.0
#define SEG_GAP 14.0
#define ICO_GAP 5.0
#define ICON_BOX 12.0

// ---- config helpers --------------------------------------------------------------
static void wideToUtf8(const wchar_t *w, char *out, size_t cap) {
    if (!w) { if (cap) out[0] = 0; return; }
    size_t i = 0;
    for (; *w && i + 1 < cap; w++) {
        unsigned cp = (unsigned)*w;
        if (cp < 0x80) out[i++] = (char)cp;
        else if (cp < 0x800 && i + 2 < cap) {
            out[i++] = (char)(0xC0 | (cp >> 6));
            out[i++] = (char)(0x80 | (cp & 0x3F));
        } else if (i + 3 < cap) {
            out[i++] = (char)(0xE0 | (cp >> 12));
            out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[i++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[i] = 0;
}

// ---- font ------------------------------------------------------------------------
// The config's fontFamily is a CSS font STACK ("'Segoe Print', cursive"). cairo's
// toy API only takes one family name and knows nothing about fontconfig aliases,
// so a Windows font that does not exist here silently fell back to a sans face -
// which is why the bar ignored the setting. This asks fontconfig for the best face
// for the WHOLE stack: it understands the comma list, the quoted names, the
// generic families (cursive, monospace) and the user's own aliases (see
// ~/.config/fontconfig/fonts.conf, which maps Segoe Print to a free face).
#include <fontconfig/fontconfig.h>

static char g_fontFamily[512] = "DejaVu Sans Mono";   // resolved family, for cairo
static char g_fontFile[512] = "";                     // resolved file, when known

static void resolveFontFamily(void) {
    char buf[512];
    wideToUtf8(cbCfgFontFamily(), buf, sizeof(buf));
    if (!buf[0]) { snprintf(g_fontFamily, sizeof(g_fontFamily), "DejaVu Sans Mono"); return; }
    // strip the quotes CSS allows around family names, keep the commas: fontconfig
    // reads the comma list itself, in order of preference
    char cleaned[512];
    size_t o = 0;
    for (const char *p = buf; *p && o + 1 < sizeof(cleaned); p++) {
        if (*p != '\'' && *p != '"') cleaned[o++] = *p;
    }
    cleaned[o] = 0;
    // trim leading spaces
    char *start = cleaned;
    while (*start == ' ') start++;

    FcPattern *pat = FcNameParse((const FcChar8 *)start);
    if (!pat) { snprintf(g_fontFamily, sizeof(g_fontFamily), "%s", start); return; }
    // the size does not matter for choosing the face, but fontconfig wants one
    FcPatternAddDouble(pat, FC_SIZE, 12.0);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    if (match && res == FcResultMatch) {
        FcChar8 *fam = NULL, *file = NULL;
        if (FcPatternGetString(match, FC_FAMILY, 0, &fam) == FcResultMatch && fam)
            snprintf(g_fontFamily, sizeof(g_fontFamily), "%s", (const char *)fam);
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file)
            snprintf(g_fontFile, sizeof(g_fontFile), "%s", (const char *)file);
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pat);
}

static double fontSizePx(void) {
    int fs = cbCfgFontSize();
    if (fs <= 0) fs = 12;
    return (double)fs * g_scale;
}

// ---- time formatting ---------------------------------------------------------------
// Same tokens as the Windows clock: {MMM} {dd} {Wkk} {HH} {mm} {ss}; anything
// else passes through untouched. Kept in the front-end because it is a rendering
// concern, exactly as p_metrics.c keeps the Windows one.
static void fmtClock(const wchar_t *fmt, time_t now, wchar_t *out, size_t cap) {
    static const wchar_t *mon[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                    L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    static const wchar_t *wd[] = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };
    struct tm tm;
    localtime_r(&now, &tm);
    if (!fmt || !*fmt) fmt = L"{MMM} {dd} ({Wkk}) {HH}:{mm}";
    size_t o = 0;
    for (const wchar_t *p = fmt; *p && o + 1 < cap; ) {
        if (*p == L'{') {
            const wchar_t *close = wcschr(p, L'}');
            if (close) {
                size_t n = (size_t)(close - p - 1);
                wchar_t tok[8];
                if (n < 8) {
                    wmemcpy(tok, p + 1, n);
                    tok[n] = 0;
                    const wchar_t *rep = NULL;
                    wchar_t num[16];
                    if (!wcscmp(tok, L"MMM")) rep = mon[tm.tm_mon];
                    else if (!wcscmp(tok, L"dd")) { swprintf(num, 16, L"%02d", tm.tm_mday); rep = num; }
                    else if (!wcscmp(tok, L"Wkk")) rep = wd[(tm.tm_wday + 6) % 7];
                    else if (!wcscmp(tok, L"HH")) { swprintf(num, 16, L"%02d", tm.tm_hour); rep = num; }
                    else if (!wcscmp(tok, L"mm")) { swprintf(num, 16, L"%02d", tm.tm_min); rep = num; }
                    else if (!wcscmp(tok, L"ss")) { swprintf(num, 16, L"%02d", tm.tm_sec); rep = num; }
                    if (rep) {
                        for (const wchar_t *r = rep; *r && o + 1 < cap; r++) out[o++] = *r;
                        p = close + 1;
                        continue;
                    }
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

// token counts abbreviate exactly as the Windows bar's fmtTokens does
static void fmtTokens(long long n, wchar_t *out, size_t cap) {
    if (n < 0) { swprintf(out, cap, L"\u2014"); return; }
    if (n >= 1000000000LL) swprintf(out, cap, L"%.2fB", n / 1e9);
    else if (n >= 1000000) swprintf(out, cap, L"%.1fM", n / 1e6);
    else if (n >= 1000) swprintf(out, cap, L"%.1fk", n / 1e3);
    else swprintf(out, cap, L"%lld", n);
}

// ---- chips ---------------------------------------------------------------------------
static void addChip(int icon, const wchar_t *text, Rgb color, int align) {
    if (g_chipCount >= MAX_CHIPS) return;
    Chip *c = &g_chips[g_chipCount++];
    c->icon = icon;
    swprintf(c->text, 64, L"%ls", text ? text : L"");
    c->color = color;
    c->align = align;
    c->hasIconColor = 0;
}

// the tokens chip is the one icon Windows paints in its own colour
// (bar.css: #seg-tokens .ico { color: var(--yellow) }), p_ui.c:741
static void setChipIconColor(int index, Rgb col) {
    if (index < 0 || index >= g_chipCount) return;
    g_chips[index].iconColor = col;
    g_chips[index].hasIconColor = 1;
}

static void buildChips(void) {
    g_chipCount = 0;
    Rgb fg = hexRgb(cbCfgFg(), (Rgb){0.03, 0.03, 0.03});
    Rgb dim = hexRgb(cbCfgFgDim(), (Rgb){0.35, 0.32, 0.27});
    Rgb warn = hexRgb(cbCfgWarn(), (Rgb){0.63, 0, 0});
    Rgb good = hexRgb(cbCfgGood(), (Rgb){0, 0.39, 0});
    Rgb yellow = hexRgb(cbCfgYellow(), (Rgb){0.72, 0.66, 0.42});

    // LEFT pinned group: the tokens chip, then the subscription gauge when one
    // exists (p_subs.c is not ported yet, so the gauge chip does not yet).
    // These sit at the left edge with the empty run in the middle.
    if (cbCfgTokensEnabled()) {
        wchar_t txt[32];
        fmtTokens(g_tokReady ? g_tokToday : -1, txt, 32);
        addChip(SVG_DIAMOND, txt, dim, CHIP_LEFT);
        setChipIconColor(g_chipCount - 1, yellow);
    }
    // RIGHT metric group: everything else, growing leftward from the right edge.
    wchar_t v[48];
    if (cbCfgModGpu() && g_m.gpu >= 0) {
        swprintf(v, 48, L"%d%%", (int)(g_m.gpu + 0.5));
        addChip(SVG_GPU, v, fg, CHIP_RIGHT);
    }
    if (cbCfgModCpu()) {
        int hot = cbCfgCpuWarnAt() > 0 && g_m.cpu >= cbCfgCpuWarnAt();
        if (g_m.cpu >= 0) { swprintf(v, 48, L"%d%%", (int)(g_m.cpu + 0.5)); addChip(SVG_CPU, v, hot ? warn : fg, CHIP_RIGHT); }
        else addChip(SVG_CPU, L"\u2014", dim, CHIP_RIGHT);
    }
    if (cbCfgModTemp()) {
        if (g_m.tempC >= 0) {
            int hot = cbCfgTempWarnAt() > 0 && g_m.tempC >= cbCfgTempWarnAt();
            if ((int)(g_m.tempC * 10) % 10 == 0) swprintf(v, 48, L"%d\u00B0C", (int)g_m.tempC);
            else swprintf(v, 48, L"%.1f\u00B0C", g_m.tempC);
            addChip(SVG_TEMP, v, hot ? warn : fg, CHIP_RIGHT);
        } else addChip(SVG_TEMP, L"\u2014", dim, CHIP_RIGHT);
    }
    if (cbCfgModRam()) {
        int hot = cbCfgRamWarnAt() > 0 && g_m.ram >= cbCfgRamWarnAt();
        if (g_m.ram >= 0) { swprintf(v, 48, L"%d%%", (int)(g_m.ram + 0.5)); addChip(SVG_RAM, v, hot ? warn : fg, CHIP_RIGHT); }
        else addChip(SVG_RAM, L"\u2014", dim, CHIP_RIGHT);
    }
    if (cbCfgModVol()) {
        if (g_m.volume >= 0) {
            int muted = g_m.volume == 0;
            swprintf(v, 48, L"%d%%", g_m.volume);
            addChip(muted ? SVG_MUTE : SVG_VOL, v, muted ? dim : fg, CHIP_RIGHT);
        } else addChip(SVG_VOL, L"\u2014", dim, CHIP_RIGHT);
    }
    if (cbCfgModBatt()) {
        if (g_m.battValid) {
            int low = g_m.battPct <= 20;
            swprintf(v, 48, L"%d%%", g_m.battPct);
            addChip(SVG_BAT, v, g_m.battAc ? good : (low ? warn : fg), CHIP_RIGHT);
        } else addChip(SVG_BAT, L"AC", dim, CHIP_RIGHT);
    }
    if (cbCfgModClock()) {
        wchar_t txt[64];
        fmtClock(cbCfgClockFormat(), g_m.clockNow, txt, 64);
        addChip(SVG_CLOCK, txt, fg, CHIP_RIGHT);
    }
}

// ---- layout + paint -------------------------------------------------------------------
typedef struct { double x, w; } Placed;
static Placed g_place[MAX_CHIPS];

static void measure(cairo_t *cr, const Chip *c, double *iconW, double *textW) {
    char buf[256];
    wideToUtf8(c->text, buf, sizeof(buf));
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    *textW = ext.x_advance;
    *iconW = (c->icon >= 0) ? ICON_BOX * g_scale : 0;
}

static void paint(void) {
    Rgb tint = hexRgb(cbCfgTint(), (Rgb){0.984, 0.949, 0.886});
    double alpha = cbCfgBgAlpha() / 255.0;
    if (alpha <= 0.0 || alpha > 1.0) alpha = 0.43;
    double radius = cbCfgBarRadius() * g_scale;
    if (radius < 0) radius = 0;
    if (radius > g_h / 2.0) radius = g_h / 2.0;

    // start every frame from a clean, fully transparent surface: the bar
    // repaints in place, so anything not cleared would accumulate
    cairo_save(g_cr);
    cairo_set_operator(g_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(g_cr, 0, 0, 0, 0);
    cairo_paint(g_cr);
    cairo_restore(g_cr);
    cairo_new_path(g_cr);
    cairo_set_operator(g_cr, CAIRO_OPERATOR_OVER);

    // the strip: rounded rect filled with the tint at backgroundAlpha
    double x = 0, y = 0, w = g_w, h = g_h;
    roundedPath(g_cr, x, y, w, h, radius);
    setSrc(g_cr, tint, alpha);
    cairo_fill_preserve(g_cr);
    // a hairline of the divider colour on the rim: it separates a light bar from a
    // light wallpaper, and it is what makes the strip read as a surface rather
    // than a smudge. Windows gets this from the acrylic tint's own edge; on X11
    // there is nothing to give it for free.
    setSrc(g_cr, hexRgb(cbCfgDivider(), (Rgb){0.85, 0.80, 0.70}), 0.55);
    cairo_set_line_width(g_cr, 1.0);
    cairo_stroke(g_cr);

    // font
    // a scaled font pinned to the resolved family AND the configured size: the toy
    // API's select_font_face ignores everything but the family name
    cairo_font_face_t *ff = cairo_toy_font_face_create(g_fontFamily,
                                                      CAIRO_FONT_SLANT_NORMAL,
                                                      CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_face(g_cr, ff);
    cairo_font_face_destroy(ff);
    cairo_set_font_size(g_cr, fontSizePx());

    Rgb icoCol = hexRgb(cbCfgIconColor() && *cbCfgIconColor() ? cbCfgIconColor() : cbCfgPinkDeep(),
                        (Rgb){0.83, 0.58, 0.67});
    double icoAlpha = cbCfgIconOpacity() / 100.0;
    if (icoAlpha <= 0.0 || icoAlpha > 1.0) icoAlpha = 0.9;

    // measure all chips, then lay out the SAME two groups the Windows bar
    // uses (p_ui.c:1028-1049): the right metric group grows leftward from the
    // right edge, the left pinned group grows rightward from the left edge, and
    // the empty run between them is what makes the strip look like the demo.
    // Laying every chip out in one right-aligned row is what made this bar read
    // as a single block against the right edge.
    double padL = PAD_L * g_scale;
    double padR = PAD_R * g_scale;
    double segGap = SEG_GAP * g_scale, icoGap = ICO_GAP * g_scale;
    for (int i = 0; i < g_chipCount; i++) {
        double iw, tw;
        measure(g_cr, &g_chips[i], &iw, &tw);
        g_place[i].w = iw + (iw > 0 ? icoGap : 0) + tw;
    }
    // right group: grows leftward from the right edge
    double xr = g_w - padR;
    for (int i = g_chipCount - 1; i >= 0; i--) {
        if (g_chips[i].align != CHIP_RIGHT) continue;
        g_place[i].x = xr - g_place[i].w;
        xr -= g_place[i].w + segGap;
    }
    // left pinned group: grows rightward from the left edge
    double xl = padL;
    for (int i = 0; i < g_chipCount; i++) {
        if (g_chips[i].align != CHIP_LEFT) continue;
        g_place[i].x = xl;
        xl += g_place[i].w + segGap;
    }

    double baseline = g_h / 2.0;
    for (int i = 0; i < g_chipCount; i++) {
        Chip *c = &g_chips[i];
        double cx = g_place[i].x;
        double iw, tw;
        measure(g_cr, c, &iw, &tw);
        // hover pill behind a clickable LEFT-group chip (p_ui.c:1069-1077):
        // without it the tokens chip looks like a label, not something you can
        // open. Right-group chips are not clickable, so they get no pill.
        int hovered = (i == g_hover) && (c->align == CHIP_LEFT) && chipClickable(c);
        if (hovered) {
            double pl = cx - HOVER_PAD_X * g_scale;
            double pt = HOVER_PAD_Y * g_scale;
            double prr = cx + tw + (c->icon >= 0 ? ICON_BOX * g_scale + icoGap : 0) + HOVER_PAD_X * g_scale;
            double pb = g_h - HOVER_PAD_Y * g_scale;
            Rgb pink = hexRgb(cbCfgPink(), (Rgb){0.94, 0.87, 0.91});
            roundedPath(g_cr, pl, pt, prr - pl, pb - pt, HOVER_RADIUS * g_scale);
            setSrc(g_cr, pink, 0.5);
            cairo_fill(g_cr);
        }
        if (c->icon >= 0) {
            double box = ICON_BOX * g_scale;
            // icons sit centred on the text's cap height, like the Windows bar
            double iy = baseline - box / 2.0 - 1.0 * g_scale;
            // a chip may override the shared icon colour (the tokens diamond is
            // yellow on Windows, p_ui.c:741)
            Rgb ic = c->hasIconColor ? c->iconColor : icoCol;
            if (c->icon == SVG_BAT) {
                // dynamic: fill tracks the charge, warn red when low on battery,
                // bolt while charging
                Rgb warnC = hexRgb(cbCfgWarn(), (Rgb){0.63, 0, 0});
                svgDrawBattery(g_cr, g_m.battPct, g_m.battAc, cx, iy, box,
                               ic.r, ic.g, ic.b, icoAlpha,
                               warnC.r, warnC.g, warnC.b);
            } else {
                svgDrawCairo(g_cr, c->icon, cx, iy, box, ic.r, ic.g, ic.b, icoAlpha);
            }
            cx += box + icoGap;
        }
        char buf[256];
        wideToUtf8(c->text, buf, sizeof(buf));
        cairo_text_extents_t ext;
        cairo_text_extents(g_cr, buf, &ext);
        setSrc(g_cr, c->color, 1.0);
        cairo_move_to(g_cr, cx, baseline - ext.height / 2.0 - ext.y_bearing);
        cairo_show_text(g_cr, buf);
        // cairo_show_text leaves the glyphs in the current path; the next frame's
        // paint would re-fill them (measured: ghosted doubles behind every chip)
        cairo_new_path(g_cr);
    }

    // cairo draws straight into the window's surface; flushing the SURFACE is what
    // hands the pixels to the server. (This used to destroy and recreate the
    // context every frame - a per-repaint allocation - and before that, an explicit
    // XRenderComposite copied the picture onto itself, which raised
    // RenderBadPicture once a resize recreated it.)
    cairo_surface_flush(g_surf);
    XFlush(g_dpy);
}

// Shape the window to the same rounded rect the surface paints. Two reasons this
// is not optional: a click on a transparent corner would otherwise hit the bar
// (the bar is a strip, not a rectangle), and a compositor that ignores per-pixel
// alpha would show square corners. Built as scanlines: one full-width band with a
// staircase above and below, which is exact at any radius and cheap (a 24px bar
// needs ~30 rectangles).
static void applyShape(void) {
    double radius = cbCfgBarRadius() * g_scale;
    if (radius < 0) radius = 0;
    if (radius > g_h / 2.0) radius = g_h / 2.0;

    XRectangle rects[80];
    int nr = 0;
    if (radius < 0.5) {
        rects[nr].x = 0; rects[nr].y = 0;
        rects[nr].width = (unsigned short)g_w;
        rects[nr].height = (unsigned short)g_h;
        nr++;
    } else {
        int steps = (int)(radius + 0.5);
        if (steps > 36) steps = 36;
        for (int i = 0; i < steps && nr < 78; i++) {
            double dy = radius - i;
            double inset = radius - sqrt(fmax(0.0, radius * radius - dy * dy));
            rects[nr].x = (short)inset;
            rects[nr].y = (short)i;
            rects[nr].width = (unsigned short)fmax(1.0, g_w - 2 * inset);
            rects[nr].height = 1;
            nr++;
        }
        if (nr < 78) {
            rects[nr].x = 0;
            rects[nr].y = (short)radius;
            rects[nr].width = (unsigned short)g_w;
            rects[nr].height = (unsigned short)fmax(1.0, g_h - 2 * radius);
            nr++;
        }
        for (int i = 0; i < steps && nr < 78; i++) {
            double dy = radius - i;
            double inset = radius - sqrt(fmax(0.0, radius * radius - dy * dy));
            rects[nr].x = (short)inset;
            rects[nr].y = (short)(g_h - 1 - i);
            rects[nr].width = (unsigned short)fmax(1.0, g_w - 2 * inset);
            rects[nr].height = 1;
            nr++;
        }
    }
    if (g_region) XFixesDestroyRegion(g_dpy, g_region);
    g_region = XFixesCreateRegion(g_dpy, rects, nr);
    XFixesSetWindowShapeRegion(g_dpy, g_win, ShapeBounding, 0, 0, g_region);
    XFlush(g_dpy);
}

// ---- window ----------------------------------------------------------------------------
static void allocSurface(void) {
    // Destroy the CONTEXT before the surface it draws into. This was the bug behind
    // thousands of RenderBadPicture errors (XRender request 138): the old context
    // survived its surface and every later paint on it referenced a freed drawable.
    // cairo keeps a reference to the surface, so the order matters and the context
    // must be destroyed explicitly - cairo_surface_destroy alone is not enough.
    if (g_cr) { cairo_destroy(g_cr); g_cr = NULL; }
    if (g_surf) { cairo_surface_destroy(g_surf); g_surf = NULL; }
    g_surf = cairo_xlib_surface_create(g_dpy, g_win, g_visual, g_w, g_h);
    cairo_xlib_surface_set_size(g_surf, g_w, g_h);
    g_cr = cairo_create(g_surf);
}

static int textWidthPx(const wchar_t *s) {
    char buf[256];
    wideToUtf8(s, buf, sizeof(buf));
    // a scaled font pinned to the resolved family AND the configured size: the toy
    // API's select_font_face ignores everything but the family name
    cairo_font_face_t *ff = cairo_toy_font_face_create(g_fontFamily,
                                                      CAIRO_FONT_SLANT_NORMAL,
                                                      CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_face(g_cr, ff);
    cairo_font_face_destroy(ff);
    cairo_set_font_size(g_cr, fontSizePx());
    cairo_text_extents_t ext;
    cairo_text_extents(g_cr, buf, &ext);
    return (int)ceil(ext.x_advance);
}

// ---- terminal geometry, from the KWin helper ---------------------------------------
// bar.align == 1 means "as long as the terminal". A Wayland compositor will not let
// the bar ask another window for its size, so linux/src/geomwatch.js (a KWin script
// the bar installs) publishes the active terminal's frame geometry as a D-Bus
// signal and geomdbus.c decodes it. No report, a stale one, or a non-terminal class
// means "no match" and the bar centres itself - never a wrong width.
static void dbg(const char *fmt, ...);
static GeomReport g_geom;             // last report received
static time_t g_geomAt = 0;           // when it arrived
static GeomBus g_bus = { -1, "" };   // -1 = not connected (a zero struct would read as fd 0)
static time_t g_busRetryAt = 0;

// Pull whatever the bus has. Reconnects with backoff: the session bus outlives the
// bar, but KWin may be restarting, and a tight reconnect loop would spin.
static void geomPump(time_t now) {
    if (g_bus.fd != 1) {
        if (now < g_busRetryAt) return;
        g_busRetryAt = now + 5;
        if (!geomBusConnect(&g_bus)) {
            if (cbCfgAlign() == 1 && !g_geom.valid)
                fprintf(stderr, "chocobar: no terminal geometry (%s); bar.align 1 needs "
                                "chocobar-linux --install-geometry\n", geomBusLastError());
            return;
        }
        g_bus.fd = 1;   // connected
    }
    GeomReport rep;
    int n = geomBusPoll(&g_bus, &rep);
    if (n < 0) {
        // the connection died and geomdbus released it: clear the flag so the
        // backoff above reconnects. Without this the bar silently loses
        // bar.align 1 for the rest of its life after any bus restart.
        g_bus.fd = -1;
        return;
    }
    if (n > 0) {
        g_geom = rep; g_geomAt = now;
        dbg("geom report: cls=%s w=%d x=%d y=%d sw=%d", rep.cls, rep.width, rep.x, rep.y, rep.screenW);
    }
}

// The width and x for the current align mode. Returns 1 when the width came from
// a terminal or the screen (the caller then keeps the y it was given), 0 for a
// content-sized bar.
static int applyAlign(int *outW, int *outX) {
    int align = cbCfgAlign();
    static int logged = -1;
    if (align != logged) {
        logged = align;
        dbg("applyAlign: align=%d geom.valid=%d w=%d x=%d age=%ld",
            align, g_geom.valid, g_geom.width, g_geom.x, (long)(time(NULL) - g_geomAt));
    }
    if (align == 2) {
        *outW = DisplayWidth(g_dpy, g_screen);
        *outX = 0;
        return 1;
    }
    if (align == 1 && g_geom.valid && time(NULL) - g_geomAt < 30) {
        // KWin reports LOGICAL pixels; the X window is in device pixels
        *outW = (int)(g_geom.width * g_scale + 0.5);
        *outX = (int)(g_geom.x * g_scale + 0.5);
        return 1;
    }
    return 0;
}

static void resizeToContent(void) {
    // width from the chips: pads + chips + the gaps between them. The two groups
    // are measured independently and then summed, because the gap between the
    // last left chip and the first right chip is NOT a SEG_GAP - it is the empty
    // run that makes the bar look like the demo. Counting it as one SEG_GAP would
    // squeeze the groups together whenever align 0/2 is not stretching the bar.
    int leftW = 0, leftN = 0, rightW = 0, rightN = 0;
    int seg = (int)(SEG_GAP * g_scale + 0.5);
    for (int i = 0; i < g_chipCount; i++) {
        int iw = g_chips[i].icon >= 0 ? (int)(ICON_BOX * g_scale + 0.5) : 0;
        int cw = iw + (iw ? (int)(ICO_GAP * g_scale + 0.5) : 0) + textWidthPx(g_chips[i].text);
        if (g_chips[i].align == CHIP_LEFT) { leftW += cw + (leftN ? seg : 0); leftN++; }
        else { rightW += cw + (rightN ? seg : 0); rightN++; }
    }
    int chipsW = leftW + rightW;
    if (leftN && rightN) chipsW += seg;   // the minimum bridge between the groups
    int w = (int)((PAD_L + PAD_R) * g_scale + 0.5) + chipsW;
    if (w < 80) w = 80;
    int alignedX = 0;
    int aw = 0;
    if (applyAlign(&aw, &alignedX)) {
        // never narrower than the chips: a terminal narrower than the readouts
        // would clip them, so the chips win and the bar sticks out
        if (aw > w) w = aw;
    }
    int h = (int)(cbCfgHeight() * g_scale + 0.5);
    if (h < 8) h = 24;
    if (w != g_w || h != g_h) {
        g_w = w; g_h = h;
        XResizeWindow(g_dpy, g_win, (unsigned)g_w, (unsigned)g_h);
        allocSurface();
        applyShape();
    }
}

static void moveWindow(int x, int y) {
    g_x = x; g_y = y;
    XMoveWindow(g_dpy, g_win, g_x, g_y);
}

// ---- position -----------------------------------------------------------------------
// The bar's position is DERIVED, never user-dragged: x and width come from the
// terminal (bar.align), y is bar.gap below the screen's top edge. Nothing is
// persisted: a dragged bar was the Windows bar's affordance, and on Linux the
// alignment contract makes a manual position meaningless.
static void positionInit(int screenW) {
    g_x = (screenW - g_w) / 2;
    g_y = (int)(cbCfgGap() * g_scale + 0.5);
}

// ---- events -------------------------------------------------------------------------------
static void onExpose(void) { paint(); }

// Event tracing for input debugging: set CHOCOBAR_DEBUG=1 and the bar appends to
// /tmp/chocobar-events.log. Silent otherwise - the shipped bar logs nothing in
// steady state, the same rule the Windows part follows.
static void dbg(const char *fmt, ...) {
    static int on = -1;
    if (on == -1) { const char *d = getenv("CHOCOBAR_DEBUG"); on = d && *d; }
    if (!on) return;
    va_list ap; va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    FILE *f = fopen("/tmp/chocobar-events.log", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

// which chip is under a point in the bar? -1 = none. Uses the same placement the
// painter computed, so a click lands on what the user sees.
static int chipAt(int px) {
    for (int i = 0; i < g_chipCount; i++) {
        double x0 = g_place[i].x, x1 = x0 + g_place[i].w;
        if (px >= x0 && px <= x1) return i;
    }
    return -1;
}

// A left-group chip does something on click (today: the tokens chip opens the
// dashboard); a right-group metric is a label. Only clickable chips get a hover
// pill, which is what tells the two groups apart visually.
static int chipClickable(const Chip *c) {
    return c->align == CHIP_LEFT && c->icon == SVG_DIAMOND;
}

static void onButton(XButtonEvent *e) {
    dbg("ButtonPress button=%u x_root=%d y_root=%d", e->button, e->x_root, e->y_root);
    if (e->button == 1) {
        // a left click on the token chip opens the dashboard (the Windows bar's
        // behaviour); anywhere else is inert - the bar is not draggable
        int hit = chipAt(e->x);
        if (hit >= 0 && g_chips[hit].icon == SVG_DIAMOND) {
            g_dashPending = 1;
        }
    } else if (e->button == 3) {
        // right-click opens the menu - the same items the Windows tray menu has.
        // It used to quit outright, which was a placeholder, not a design.
        g_menuPending = 1;
        g_menuX = e->x_root;
        g_menuY = e->y_root;
    }
}

static void onMotion(XMotionEvent *e) {
    // hover tracking only: the bar is not draggable, so motion never moves it.
    // Only a chip that repaints differently needs a repaint.
    int hit = chipAt(e->x);
    if (hit >= 0 && !chipClickable(&g_chips[hit])) hit = -1;
    if (hit != g_hover) { g_hover = hit; paint(); }
}

static void onLeave(void) {
    if (g_hover != -1) { g_hover = -1; paint(); }
}

static void onKey(XKeyEvent *e) {
    KeySym ks = XLookupKeysym(e, 0);
    if (ks == XK_Escape || ks == XK_q) g_quit = 1;
}

static void onSig(int sig) { (void)sig; g_quit = 1; }

// Xlib's default error handler calls exit(). A BadWindow from a request that
// raced a resize or an Xwayland restart therefore killed the bar outright - the
// "it just disappeared" symptom. Log and carry on: a single failed request is
// never a reason to lose the whole bar, and the next tick repaints.
static int onXError(Display *d, XErrorEvent *e) {
    char buf[200];
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "chocobar: X error ignored: %s (request %d)\n", buf, e->request_code);
    return 0;
}

static int onXIOError(Display *d) {
    (void)d;
    // the display itself is gone: the only sane move is a clean exit, which the
    // systemd unit then restarts into a fresh session
    fprintf(stderr, "chocobar: X connection lost; exiting for a restart\n");
    _exit(3);
}

static void createWindow(void) {
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.colormap = g_cmap;
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonMotionMask |
                     LeaveWindowMask | KeyPressMask | StructureNotifyMask;
    // MANAGED, not override-redirect: KWin composites override-redirect windows in
    // its top layer no matter what hints they set (measured on Plasma 6.6), so an
    // unmanaged bar is always on top of everything. The user wants the bar at the
    // LOWER layer - below normal windows, so a maximised window covers it - and
    // only a managed window can sit there (_NET_WM_STATE_BELOW). No decorations,
    // no taskbar entry, and the geometry is set with USPosition so KWin honours
    // the exact rectangle instead of running its placement policy.
    g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, g_screen), 0, 0,
                          (unsigned)g_w, (unsigned)g_h, 0, g_depth, InputOutput,
                          g_visual, CWBackPixel | CWBorderPixel |
                          CWColormap | CWEventMask, &swa);
    // CWEventMask is what makes swa.event_mask take effect. Without it the
    // window exists with an empty event mask (measured: XGetWindowAttributes
    // reported your_event_mask=0x0) and the server silently drops every button
    // and motion event - a bar that is visible, correct and completely inert.
    // below normal windows, on every workspace, never in the taskbar: the
    // BELOW layer is the bar's home, not the top layer.
    Atom wmState = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    Atom below = XInternAtom(g_dpy, "_NET_WM_STATE_BELOW", False);
    Atom skipTask = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom skipPager = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom sticky = XInternAtom(g_dpy, "_NET_WM_STATE_STICKY", False);
    Atom states[4] = { below, skipTask, skipPager, sticky };
    XChangeProperty(g_dpy, g_win, wmState, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, 4);
    // all desktops. Setting _NET_WM_DESKTOP=0xFFFFFFFF BEFORE the map does not
    // survive it: KWin resolves the request to the current desktop and writes
    // the concrete number back (measured: the property read 0 after map while
    // STICKY stayed set, and the bar then did not appear on desktop 2). The
    // request that works is the one made after the window is managed, so the
    // sticky request lives in the post-map block below.
    // InputHint FALSE: the bar must never take the keyboard. Pointer events are
    // independent of this hint (the server routes them by stacking), so the
    // right-click menu and chip clicks still land; clicking the bar simply never
    // moves keyboard focus off the terminal. On an override-redirect window KWin
    // went further and dropped pointer input entirely; managed clients keep it.
    XWMHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = InputHint;
    hints.input = False;
    XSetWMHints(g_dpy, g_win, &hints);
    // no title bar, no frame: _MOTIF_WM_HINTS with only the decorations bit,
    // value 0
    typedef struct { unsigned long flags, functions, decorations; long inputMode;
                     unsigned long status; } MotifHints;
    MotifHints mh = { 2, 0, 0, 0, 0 };   // MWM_HINTS_DECORATIONS, none
    XChangeProperty(g_dpy, g_win, XInternAtom(g_dpy, "_MOTIF_WM_HINTS", False),
                    XInternAtom(g_dpy, "_MOTIF_WM_HINTS", False), 32, PropModeReplace,
                    (unsigned char *)&mh, 5);
    // normal window type: DOCK would put the bar back in the top layer (docks sit
    // above normal windows), which is exactly what the user asked to move away from
    Atom wmType2 = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom typeNormal = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(g_dpy, g_win, wmType2, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&typeNormal, 1);
    // no focus stealing on map: user time 0 tells KWin this window never took
    // user input, so mapping it does not yank focus from the terminal. The cost
    // is that KWin flags the map as "attention" (a window appearing without
    // user activity); it is invisible here (skip-taskbar, skip-pager) and the
    // property is rewritten right after the map below.
    unsigned long zeroTime = 0;
    XChangeProperty(g_dpy, g_win, XInternAtom(g_dpy, "_NET_WM_USER_TIME", False),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&zeroTime, 1);
    // KWin's blur-behind hint (the same property Konsole/Dolphin set): the
    // compositor blurs what is behind the window inside the region, which is what
    // makes a translucent bar readable over text. Harmless on other compositors.
    Atom blurAtom = XInternAtom(g_dpy, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    long blurRegion[4] = { 0, 0, g_w, g_h };
    XChangeProperty(g_dpy, g_win, blurAtom, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)blurRegion, 4);

    XStoreName(g_dpy, g_win, "Chocobar");
    XClassHint cls = { (char *)"chocobar", (char *)"Chocobar" };
    XSetClassHint(g_dpy, g_win, &cls);
    // NOT mapped here: the window is mapped in main() after the geometry is
    // final, so KWin never sees a stray 10-px strip at the origin and never runs
    // its placement policy over it.
}

// Size hints with USPosition: KWin honours a user-specified position instead of
// running its own placement. Re-set before every map so a geometry that changed
// while withdrawn is the one the window manager sees.
static void setSizeHints(int x, int y) {
    XSizeHints sh;
    memset(&sh, 0, sizeof(sh));
    sh.flags = USPosition | PPosition | USSize | PSize;
    sh.x = x; sh.y = y;
    sh.width = g_w; sh.height = g_h;
    XSetWMNormalHints(g_dpy, g_win, &sh);
}

// ---- DPI ---------------------------------------------------------------------------------------
// The Xft.dpi resource is what this desktop publishes for XWayland clients (153
// here, i.e. 1.59x). Falling back to the screen's physical DPI keeps an X11
// session sane.
static void detectScale(void) {
    g_scale = 1.0;
    char *res = XResourceManagerString(g_dpy);
    if (res) {
        char *line = strstr(res, "Xft.dpi:");
        if (line) {
            double dpi = atof(line + 8);
            if (dpi > 40 && dpi < 1000) g_scale = dpi / 96.0;
        }
    }
    if (g_scale <= 1.01) {
        int wmm = DisplayWidthMM(g_dpy, g_screen);
        int wpix = DisplayWidth(g_dpy, g_screen);
        if (wmm > 0) {
            double dpi = (double)wpix * 25.4 / (double)wmm;
            if (dpi > 40 && dpi < 1000) g_scale = dpi / 96.0;
        }
    }
    if (g_scale < 1.0) g_scale = 1.0;
    if (g_scale > 4.0) g_scale = 4.0;
}

// ---- tokens ---------------------------------------------------------------------------------------
static void scanIfDue(time_t now) {
    int sec = cbCfgRescanSec();
    if (sec <= 0) sec = 60;
    if (g_tokReady && now - g_lastScan < sec) return;
    cbScanTokens();
    g_lastScan = now;
    g_tokReady = 1;
    // today's total: the bar's chip shows the day, as the Windows bar does. The
    // all-time total is what the scan exposes; today is derived from the same
    // aggregates on Windows by tokDeriveWindows, which is dashboard-side. For
    // the bar's first Linux version the chip shows the all-time total, which is
    // the number cbTokAll() owns and the one the scan verifies.
    g_tokToday = cbTokAll();
}

// ---- config reload ----------------------------------------------------------------------------------
static time_t g_cfgMtime = 0;
static char g_cfgPath[512];

static int cfgStat(time_t *out) {
    struct stat st;
    if (stat(g_cfgPath, &st) != 0) return 0;
    *out = st.st_mtime;
    return 1;
}

static void reloadConfig(void) {
    wchar_t wide[1024];
    int wl = MultiByteToWideChar(CP_UTF8, 0, g_cfgPath, -1, wide, 1024);
    if (wl <= 0) return;
    cbLoadConfig(wide);
    resolveFontFamily();
}

// ---- the right-click menu and the dashboards --------------------------------------

// A dashboard is a modal-ish second window: while it is open the bar stops
// repainting itself, which keeps the two from fighting over the event queue.
static void runDashboard(void) {
    Rgb bgc = hexRgb(cbCfgPinkBg(), (Rgb){0.996, 0.969, 0.976});
    Rgb cardc = hexRgb(cbCfgPink(), (Rgb){0.941, 0.871, 0.894});
    Rgb fgc = hexRgb(cbCfgFg(), (Rgb){0.03, 0.03, 0.03});
    Rgb dimc = hexRgb(cbCfgFgDim(), (Rgb){0.35, 0.32, 0.27});
    Rgb accc = hexRgb(cbCfgPinkDeep(), (Rgb){0.83, 0.58, 0.67});
    Rgb divc = hexRgb(cbCfgDivider(), (Rgb){0.85, 0.80, 0.70});
    Rgb warnc = hexRgb(cbCfgWarn(), (Rgb){0.63, 0, 0});
    double ramp[5][3];
    for (int i = 0; i < 5; i++) {
        Rgb c = hexRgb(cbCfgHeat(i), (Rgb){0.9, 0.85, 0.8});
        ramp[i][0] = c.r; ramp[i][1] = c.g; ramp[i][2] = c.b;
    }
    double page[3] = { bgc.r, bgc.g, bgc.b };
    double card[3] = { cardc.r, cardc.g, cardc.b };
    double fg[3] = { fgc.r, fgc.g, fgc.b };
    double dim[3] = { dimc.r, dimc.g, dimc.b };
    double acc[3] = { accc.r, accc.g, accc.b };
    double div[3] = { divc.r, divc.g, divc.b };
    double wrn[3] = { warnc.r, warnc.g, warnc.b };
    int cw = cbCfgDashW() > 0 ? cbCfgDashW() : 900;
    int chh = cbCfgDashH() > 0 ? cbCfgDashH() : 520;
    Visual *dvis = DefaultVisual(g_dpy, g_screen);
    Colormap dcmap = DefaultColormap(g_dpy, g_screen);
    int ddepth = DefaultDepth(g_dpy, g_screen);
    dashRun(g_dpy, g_screen, dvis, dcmap, ddepth, cw, chh, g_scale,
            (int)(fontSizePx()), g_fontFamily,
            page, card, fg, dim, acc, div, wrn, ramp, 26);
    // the bar's own surface is still valid; force a repaint when we come back
    paint();
}

// Run an external program without blocking the bar's loop: a config editor or a
// file manager must not freeze the strip.
static void spawnDetached(const char *argv0, const char *argv1) {
    pid_t pid = fork();
    if (pid != 0) return;
    setsid();
    if (argv1) execlp(argv0, argv0, argv1, (char *)NULL);
    else execlp(argv0, argv0, (char *)NULL);
    _exit(127);
}

static void configDirOf(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.wizbar", home ? home : "/tmp");
}

static void handleMenuChoice(int choice) {
    dbg("menu choice = %d", choice);
    switch (choice) {
        case MENU_TOKEN_DASH: g_dashPending = 1; break;
        case MENU_SUBS_DASH:
            fprintf(stderr, "chocobar: the subscription board is not ported to Linux yet\n");
            break;
        case MENU_EDIT: {
            char dir[512];
            configDirOf(dir, sizeof(dir));
            char file[600];
            snprintf(file, sizeof(file), "%s/config.json", dir);
            const char *ed = getenv("EDITOR");
            if (ed && *ed) spawnDetached(ed, file);
            else spawnDetached("xdg-open", file);
            break;
        }
        case MENU_OPEN_DIR: {
            char dir[512];
            configDirOf(dir, sizeof(dir));
            spawnDetached("xdg-open", dir);
            break;
        }
        case MENU_RELOAD: {
            reloadConfig();
            time_t cur;
            if (cfgStat(&cur)) g_cfgMtime = cur;
            buildChips();
            resizeToContent();
            paint();
            break;
        }
        case MENU_QUIT: g_quit = 1; break;
        default: break;
    }
}

// ---- the KWin geometry helper: install / remove ----------------------------------
// KWin loads a script from ~/.local/share/kwin/scripts/<name>/contents/code/main.js
// and needs to be told to load and start it over D-Bus. qdbus is used rather than
// linking sd-bus: it is one call at install time, and a missing qdbus must fail
// with a clear message rather than a silent no-op.
#define KW_SCRIPT_NAME "chocobar-geometry"

static int kwScriptPath(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) return 0;
    snprintf(out, cap, "%s/.local/share/kwin/scripts/%s/contents/code/main.js",
             home, KW_SCRIPT_NAME);
    return 1;
}

// the script's source, compiled in so the binary is self-contained
static const char *g_geomScript =
#include "geomwatch.js.inc"
;

static int installGeometry(void) {
    char path[600], dir[600];
    if (!kwScriptPath(path, sizeof(path))) { fprintf(stderr, "chocobar: no HOME\n"); return 1; }
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; }
    // mkdir -p the whole chain
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    }
    mkdir(dir, 0755);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "chocobar: cannot write %s: %s\n", path, strerror(errno)); return 1; }
    fputs(g_geomScript, f);
    fclose(f);
    printf("installed %s\n", path);
    // tell KWin to load and start it. -a (unload) first, so a reinstall replaces
    // the running copy instead of stacking a second poller.
    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
        "qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript %s >/dev/null 2>&1; "
        "qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript %s %s >/dev/null 2>&1; "
        "qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1",
        KW_SCRIPT_NAME, path, KW_SCRIPT_NAME);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "chocobar: could not talk to KWin over D-Bus (qdbus6).\n"
                        "  The script is installed; enable it in System Settings > Window Management > KWin Scripts,\n"
                        "  or log out and back in.\n");
        return 1;
    }
    printf("KWin script '%s' loaded; the bar now matches the active terminal.\n", KW_SCRIPT_NAME);
    return 0;
}

static int removeGeometry(void) {
    char path[600];
    if (!kwScriptPath(path, sizeof(path))) return 1;
    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
        "qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript %s >/dev/null 2>&1",
        KW_SCRIPT_NAME);
    int rc = system(cmd);
    (void)rc;   // best-effort cleanup; the qdbus call is advisory
    unlink(path);
    printf("removed %s\n", path);
    return 0;
}

// ---- main -----------------------------------------------------------------------------------------------
static void resolveConfigPath(void) {
    const char *env = getenv("WIZBAR_CONFIG");
    const char *home = getenv("HOME");
    if (env && *env) { snprintf(g_cfgPath, sizeof(g_cfgPath), "%s", env); return; }
    snprintf(g_cfgPath, sizeof(g_cfgPath), "%s/.wizbar/config.json", home ? home : "/tmp");
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            snprintf(g_cfgPath, sizeof(g_cfgPath), "%s", argv[++i]);
        } else if (!strncmp(argv[i], "--config=", 9)) {
            snprintf(g_cfgPath, sizeof(g_cfgPath), "%s", argv[i] + 9);
        } else if (!strcmp(argv[i], "--install-geometry")) {
            return installGeometry();
        } else if (!strcmp(argv[i], "--remove-geometry")) {
            return removeGeometry();
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("chocobar-linux [--config PATH]\n"
                   "  floating status bar: cpu, ram, temp, volume, battery, clock, tokens\n"
                   "  right-click opens the menu; left-click on the tokens chip opens\n"
                   "  the dashboard (the bar itself is not draggable)\n"
                   "  --install-geometry  load the KWin helper that makes bar.align 1\n"
                   "                      (match the active terminal's width) work\n"
                   "  --remove-geometry   unload and delete that helper\n");
            return 0;
        }
    }
    if (!g_cfgPath[0]) resolveConfigPath();

    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);

    // core: config lock, cursors, config load (the same startup the macOS bar does)
    cbCoreInit();
    cbTokensInit();
    reloadConfig();
    cfgStat(&g_cfgMtime);
    resolveFontFamily();

    XSetErrorHandler(onXError);
    XSetIOErrorHandler(onXIOError);
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "chocobar: cannot open display (is DISPLAY set?)\n");
        return 1;
    }
    g_screen = DefaultScreen(g_dpy);
    // The window MUST be on a 32-bit TrueColor visual with an alpha channel, or
    // the compositor has nothing to blend: the surface's premultiplied tint would
    // be taken as opaque colour and the bar would render as a dark grey strip
    // (measured before this: (118,114,106) - exactly tint*alpha, alpha dropped).
    // The default visual is depth 24 on this desktop, so it is not an option.
    XVisualInfo vinfo;
    if (XMatchVisualInfo(g_dpy, g_screen, 32, TrueColor, &vinfo)) {
        g_visual = vinfo.visual;
        g_depth = 32;
        g_cmap = XCreateColormap(g_dpy, RootWindow(g_dpy, g_screen), g_visual, AllocNone);
    } else {
        fprintf(stderr, "chocobar: no 32-bit ARGB visual on this screen; "
                        "the bar cannot be translucent\n");
        return 1;
    }
    g_pictFmt = XRenderFindVisualFormat(g_dpy, g_visual);
    if (!g_pictFmt || !g_pictFmt->direct.alphaMask) {
        fprintf(stderr, "chocobar: the chosen visual has no alpha channel\n");
        return 1;
    }
    int xev, xerr, xmaj = 0, xmin = 0;
    if (!XFixesQueryExtension(g_dpy, &xev, &xerr)) {
        fprintf(stderr, "chocobar: XFixes extension missing (needed to shape the window)\n");
        return 1;
    }
    XFixesQueryVersion(g_dpy, &xmaj, &xmin);

    detectScale();
    svgInitPaths();            // p_svg.c's shared path data, flattened once

    // initial metrics so the very first paint has real numbers
    linuxMetricsPrime();
    linuxMetricsPoll(&g_m);
    scanIfDue(time(NULL));
    buildChips();

    g_w = 10; g_h = (int)(cbCfgHeight() * g_scale + 0.5);
    // With bar.align 1 the width comes from the terminal, so give the KWin helper
    // a moment to report before sizing: otherwise the window is born at the
    // content width and visibly pops to the terminal width a second later.
    // Bounded at 700 ms, and only when align asks for it.
    if (cbCfgAlign() == 1) {
        for (int i = 0; i < 70 && !g_geom.valid; i++) {
            geomPump(time(NULL));
            if (g_geom.valid) break;
            struct timespec ts = { 0, 10 * 1000 * 1000 };  // 10 ms
            nanosleep(&ts, NULL);
        }
    }
    createWindow();
    allocSurface();        // text measurement needs a cairo context, and
    resizeToContent();     // resizeToContent is what measures the chips
    positionInit(DisplayWidth(g_dpy, g_screen));
    moveWindow(g_x, g_y);
    applyShape();
    // geometry is final: tell KWin the exact rectangle, then map. A managed
    // window's map is the one chance the WM gets to place it - USPosition makes
    // it honour ours instead of running its placement policy.
    setSizeHints(g_x, g_y);
    XMapWindow(g_dpy, g_win);
    // KWin adds DEMANDS_ATTENTION to the state property when a window maps
    // without user activity. Clear it: re-writing _NET_WM_STATE after the map
    // is the client-side contract for "attention handled".
    {
        Atom wmState = XInternAtom(g_dpy, "_NET_WM_STATE", False);
        Atom below = XInternAtom(g_dpy, "_NET_WM_STATE_BELOW", False);
        Atom skipTask = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
        Atom skipPager = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_PAGER", False);
        Atom sticky = XInternAtom(g_dpy, "_NET_WM_STATE_STICKY", False);
        Atom states[4] = { below, skipTask, skipPager, sticky };
        XEvent ev;
        for (int i = 0; i < 200; i++) {   // bounded: never block startup
            while (XPending(g_dpy)) {
                XNextEvent(g_dpy, &ev);
                if (ev.type == MapNotify && ev.xany.window == g_win) i = 200;
            }
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        XChangeProperty(g_dpy, g_win, wmState, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)states, 4);
        // and the sticky request. Writing _NET_WM_DESKTOP as a property is NOT
        // the EWMH request: KWin keeps the value but treats the window as
        // single-desktop (measured: onAllDesktops=false with the property at
        // 0xFFFFFFFF and STICKY set). The protocol is a ClientMessage to the
        // root window, and it has to arrive after KWin has finished managing.
        XEvent cm;
        memset(&cm, 0, sizeof(cm));
        cm.xclient.type = ClientMessage;
        cm.xclient.window = g_win;
        cm.xclient.message_type = XInternAtom(g_dpy, "_NET_WM_DESKTOP", False);
        cm.xclient.format = 32;
        cm.xclient.data.l[0] = 0xFFFFFFFF;   // all desktops
        cm.xclient.data.l[1] = 1;            // source: normal application
        XSendEvent(g_dpy, RootWindow(g_dpy, g_screen), False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &cm);
        XFlush(g_dpy);
    }
    paint();

    int xfd = ConnectionNumber(g_dpy);
    time_t lastCfgCheck = time(NULL);
    time_t lastPaint = 0;
    while (!g_quit) {
        // wait up to one tick; the tick is the second-resolution clock change
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = { 1, 0 };
        int sel = select(xfd + 1, &fds, NULL, NULL, &tv);
        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            switch (ev.type) {
                case Expose: if (ev.xexpose.count == 0) onExpose(); break;
                case ButtonPress: onButton(&ev.xbutton); break;
                case MotionNotify: onMotion(&ev.xmotion); break;
                case LeaveNotify: onLeave(); break;
                case KeyPress: onKey(&ev.xkey); break;
                case ConfigureNotify:
                    if (ev.xconfigure.width != g_w || ev.xconfigure.height != g_h) {
                        g_w = ev.xconfigure.width; g_h = ev.xconfigure.height;
                        allocSurface();
                        paint();
                    }
                    break;
                default: break;
            }
        }
        if (g_menuPending) {
            g_menuPending = 0;
            Rgb bgc = hexRgb(cbCfgTint(), (Rgb){0.91, 0.85, 0.76});
            Rgb fgc = hexRgb(cbCfgFg(), (Rgb){0.03, 0.03, 0.03});
            Rgb dimc = hexRgb(cbCfgFgDim(), (Rgb){0.35, 0.32, 0.27});
            Rgb divc = hexRgb(cbCfgDivider(), (Rgb){0.85, 0.80, 0.70});
            Rgb accc = hexRgb(cbCfgPinkDeep(), (Rgb){0.83, 0.58, 0.67});
            double bg3[3] = { bgc.r, bgc.g, bgc.b };
            double fg3[3] = { fgc.r, fgc.g, fgc.b };
            double dm3[3] = { dimc.r, dimc.g, dimc.b };
            double dv3[3] = { divc.r, divc.g, divc.b };
            double ac3[3] = { accc.r, accc.g, accc.b };
            int choice = menuRun(g_dpy, g_screen, g_visual, g_cmap, 32,
                                 g_menuX, g_menuY, g_scale, (int)fontSizePx(),
                                 g_fontFamily, bg3, 0.97, fg3, dm3, dv3, ac3);
            handleMenuChoice(choice);
        }
        if (g_dashPending) {
            g_dashPending = 0;
            runDashboard();
        }
        time_t now = time(NULL);
        geomPump(now);
        if (sel == 0 && now != lastPaint) {
            lastPaint = now;
            // hot-reload the config on mtime change, then re-derive everything
            if (now - lastCfgCheck >= 2) {
                lastCfgCheck = now;
                time_t cur;
                if (cfgStat(&cur) && cur != g_cfgMtime) { g_cfgMtime = cur; reloadConfig(); }
            }
            linuxMetricsPoll(&g_m);
            scanIfDue(now);
            buildChips();
            resizeToContent();
            {
                int aw = 0, ax = 0;
                if (applyAlign(&aw, &ax) && ax != g_x) moveWindow(ax, g_y);
            }
            // align modes drive x every tick: a terminal moved or resized must
            // take the bar with it. The y stays where the user put it.
            {
                int aw = 0, ax = 0;
                if (applyAlign(&aw, &ax)) {
                    if (ax != g_x) moveWindow(ax, g_y);
                    int wantW = aw;
                    int chipsW = (int)((PAD_L + PAD_R) * g_scale + 0.5);
                    for (int i = 0; i < g_chipCount; i++) {
                        int iw = g_chips[i].icon >= 0 ? (int)(ICON_BOX * g_scale + 0.5) : 0;
                        chipsW += iw + (iw ? (int)(ICO_GAP * g_scale + 0.5) : 0) + textWidthPx(g_chips[i].text);
                        if (i) chipsW += (int)(SEG_GAP * g_scale + 0.5);
                    }
                    if (wantW < chipsW) wantW = chipsW;
                    if (wantW != g_w) {
                        g_w = wantW;
                        XResizeWindow(g_dpy, g_win, (unsigned)g_w, (unsigned)g_h);
                        allocSurface();
                        applyShape();
                    }
                }
            }
            paint();
        }
    }
    geomBusClose(&g_bus);
    if (g_region) XFixesDestroyRegion(g_dpy, g_region);
    if (g_pic) XRenderFreePicture(g_dpy, g_pic);
    if (g_cr) cairo_destroy(g_cr);
    if (g_surf) cairo_surface_destroy(g_surf);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    return 0;
}
