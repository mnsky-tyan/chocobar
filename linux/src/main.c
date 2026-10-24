// linux/src/main.c - the Linux Chocobar bar.
//
// WHAT THIS IS
// A small lower-layer, frameless, translucent strip that floats over the
// desktop and shows the same chips as the Windows bar: pet, the token count
// the shared scanner produces, the subscription gauge, cpu, gpu, cpu temp,
// ram, volume, battery and clock.
// With bar.align 1 it rides the terminal: x and width come from the active
// terminal (the KWin helper's report) and y sits just above its top edge - the
// position is derived every tick and never persisted.
//
// HOW IT PAINTS
// The Windows bar draws into a premultiplied 32bpp DIB and hands it to
// UpdateLayeredWindow. The Linux equivalent is a MANAGED X11 window on a
// 32-bit ARGB visual, painted through a cairo surface bound to the window:
//  - managed, NOT override-redirect (a measured fact: KWin composites every
//    override-redirect window in its top layer no matter which state atoms it
//    carries, so the lower layer is reachable only by a managed BELOW window).
//    _NET_WM_STATE_BELOW/SKIP_TASKBAR/SKIP_PAGER/STICKY, undecorated
//    _MOTIF_WM_HINTS, InputHint False and a USPosition geometry give it no
//    title bar, no taskbar entry, no focus, and the exact derived rectangle -
//    the same role WS_EX_LAYERED + WS_EX_NOACTIVATE plays on Windows.
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
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/X.h>
#include <X11/Xlib.h>

#include "linux_shape.h"
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>

#include "chocobar_core.h"
#include "version.h"      // CB_VER_STR: the one version the release tags
#include "svg_geometry.h"   // the shared icon ids (SVG_CPU, ...)
#include "linux_metrics.h"
#include "geomdbus.h"
#include "menu.h"
#include "dash.h"
#include "tooltip.h"
#include "linux_svg.h"
#include "selfheal.h"

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
// the pet chip: parsed in the core's config, read here
extern int cbCfgPetEnabled(void);
extern const wchar_t *cbCfgPetLabel(void);
extern const wchar_t *cbCfgPetPath(void);
extern int cbCfgCpuWarnAt(void);
extern int cbCfgRamWarnAt(void);
extern int cbCfgTempWarnAt(void);
// the subscription gauge: fetched by the shared p_subs.c providers
// (see chocobar_core.h's cbSubs* seam)
extern int cbCfgSubsRotateSec(void);
extern int cbSubsEnabled(void);
extern void cbSubsStart(void);
extern void cbSubsSetRotateSec(int sec);
extern int cbSubsChipRem(void);
extern int cbSubsChipStale(void);
extern int cbSubsChipRotated(wchar_t *txt, int cb, wchar_t *tip, int tipCb);

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

// the rounded rectangle and the scanline staircase are defined ONCE in
// linux_shape.c: three copies with divergent caps is how a truncating float in
// one of them left a 1 px hole across the bar.

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
static int g_swallowRelease = 0;   // the menu's dismissing gesture: eat its release
static int g_dashPending = 0;   // 1 = token dashboard, 2 = subscriptions


static LinuxMetrics g_m;
static time_t g_lastScan = 0;
// the subscription chip's rotating tooltip ("<plan> <window> N% left"), produced
// by the shared formatter on every rebuild and read by chipTitle() on hover -
// the Linux twin of p_ui.c's g_subsRotTip
static char g_subsTip[96];
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
static void cursorUpdate(int clickable);
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
    g_subsTip[0] = 0;
    Rgb fg = hexRgb(cbCfgFg(), (Rgb){0.03, 0.03, 0.03});
    Rgb dim = hexRgb(cbCfgFgDim(), (Rgb){0.35, 0.32, 0.27});
    Rgb warn = hexRgb(cbCfgWarn(), (Rgb){0.63, 0, 0});
    Rgb good = hexRgb(cbCfgGood(), (Rgb){0, 0.39, 0});
    Rgb yellow = hexRgb(cbCfgYellow(), (Rgb){0.72, 0.66, 0.42});

    // LEFT pinned group, in the Windows order (p_ui.c:715-770): the shortcut
    // chip (SVG_BOLT, disabled on Linux - there is no global hotkey), then the
    // pet ("is this process alive"), then the tokens chip, then the
    // subscription gauge when one exists.
    if (cbCfgPetEnabled()) {
        wchar_t txt[80];
        int running = g_m.petRunning;
        if (cbCfgPetLabel() && *cbCfgPetLabel())
            swprintf(txt, 80, L"%ls %ls", cbCfgPetLabel(), running ? L"on" : L"off");
        else swprintf(txt, 80, L"%ls", running ? L"on" : L"off");
        addChip(SVG_BOW, txt, running ? good : dim, CHIP_LEFT);
    }
    if (cbCfgTokensEnabled()) {
        wchar_t txt[32];
        fmtTokens(g_tokReady ? g_tokToday : -1, txt, 32);
        addChip(SVG_DIAMOND, txt, dim, CHIP_LEFT);
        setChipIconColor(g_chipCount - 1, yellow);
    }
    // subscription gauge: the PLAN WEEK LIMITS meter, in the Windows order
    // (p_ui.c: the tokens chip, then the gauge, then user-defined buttons).
    // It rotates one provider's remaining % every subs.rotateSec. The colour
    // thresholds and the "stale"/em-dash fallbacks are the Windows ones, so a
    // failed fetch reads the same on both bars.
    if (cbSubsEnabled()) {
        wchar_t txt[16];
        Rgb col;
        int stale = cbSubsChipStale();
        int rem = cbSubsChipRem();
        if (rem == -1) {
            lstrcpynW(txt, L"\u2014", 16);
            col = dim;
        } else if (stale) {
            lstrcpynW(txt, L"stale", 16);
            col = dim;
        } else {
            // the tooltip ("<plan> <window> N% left") is produced by the shared
            // formatter and shown on hover, exactly as on Windows (p_ui.c
            // stores it in the chip's tipOverride and chipTitle returns it)
            wchar_t tip[96];
            rem = cbSubsChipRotated(&txt[0], 16, tip, 96);
            wideToUtf8(tip, g_subsTip, sizeof(g_subsTip));
            // the Windows thresholds: >70 good, >30 dim, else warn
            col = rem > 70 ? good : rem > 30 ? dim : warn;
        }
        addChip(SVG_GAUGE, txt, col, CHIP_LEFT);
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
    // API's select_font_face ignores everything but the family name.
    //
    // The slant is ITALIC, not NORMAL. Segoe Print on Windows is a hand-
    // written face whose letters already lean; the closest installed family
    // (Comic Neue) offers the same lean as its Italic face, and the config's
    // "fontFamily" names that face by intent. Rendered upright the bar reads
    // as a different font than the reference screenshot, even though every
    // metric matches.
    cairo_font_face_t *ff = cairo_toy_font_face_create(g_fontFamily,
                                                      CAIRO_FONT_SLANT_ITALIC,
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
        int hovered = (i == g_hover) && chipClickable(c);
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
    int nr = roundedRectScanlines(rects, 80, g_w, g_h, radius);
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
    // the SAME face paint() uses, slant included: measuring with a different
    // slant gives a different advance width, and the chips then overlap
    cairo_font_face_t *ff = cairo_toy_font_face_create(g_fontFamily,
                                                      CAIRO_FONT_SLANT_ITALIC,
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
#define KW_SCRIPT_NAME "chocobar-geometry"   // defined here: geomPump needs it
static int kwScriptPath(char *out, size_t cap);   // defined below, with the installer
static GeomReport g_geom;             // last report received
static time_t g_geomAt = 0;           // when it arrived
static GeomBus g_bus = { -1 };       // -1 = not connected (a zero struct would read as fd 0)
static time_t g_busRetryAt = 0;
static time_t g_scriptCheckAt = 0;    // last isScriptLoaded probe

// ---- one KWin helper, ever ---------------------------------------------------------
// KWin runs every loadScript as a NEW instance of the same file, and geomwatch.js
// polls the compositor's main thread every 250 ms. Two instances therefore double
// the compositor's per-tick work, three triple it - measured on 2026-10-24, when a
// bar restart raced the self-heal and the desktop froze for over a minute with
// KWin logging "The main thread was hanging temporarily!".
//
// The reload path already unloads before loading, which makes the common case
// safe. This lock closes the remaining window: two bar PROCESSES (the service
// restarting while an old one is still alive, or a dev build next to the released
// one) must not both decide to load it. The lock is a file under the runtime dir,
// taken with O_EXCL and released on exit; a stale lock from a killed process is
// detected by checking whether the pid it names is still alive.
static char g_scriptLockPath[600];
static int g_scriptLockHeld = 0;

// TRUE when the lock's named owner is still a live chocobar process. A bare
// kill(pid, 0) is not enough: pids are recycled, so a lock left behind by a
// killed bar can name an unrelated long-running process for years, and that
// would wedge the bar out of ever taking the helper back (the freeze this lock
// exists to prevent, paid for as a permanently dead bar.align 1).
static int lockOwnerAlive(long owner) {
    if (owner <= 1) return 0;   // 1 is init, and the file may be empty or junk
    if (kill((pid_t)owner, 0) != 0) return 0;
    char path[64], comm[32];
    snprintf(path, sizeof(path), "/proc/%ld/comm", owner);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;      // a live process we cannot inspect: leave it alone
    ssize_t r = read(fd, comm, sizeof(comm) - 1);
    close(fd);
    if (r <= 0) return 1;
    comm[r] = 0;
    char *nl = strchr(comm, '\n');
    if (nl) *nl = 0;
    // the install name varies (chocobar-linux from the build, whatever the
    // user renamed it to when installing by hand), so match the prefix
    return strncmp(comm, "chocobar", 8) == 0;
}

// The lock file's path: XDG_RUNTIME_DIR when set, else $HOME/.cache. One
// definition, so the self-test and the bar agree on which file is the lock.
static int scriptLockPath(char *out, size_t cap) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) { snprintf(out, cap, "%s/chocobar-geom.lock", rt); return 1; }
    const char *home = getenv("HOME");
    if (!home || !*home) return 0;
    snprintf(out, cap, "%s/.cache/chocobar-geom.lock", home);
    return 1;
}

// Take the lock on `path`: O_EXCL first, and when a lock already exists, take
// it over only if its owner is not a live bar. The held flag is this process's
// own record, so a second call while we hold it succeeds without touching the
// file. Returns 1 when this process holds the lock afterwards.
static int scriptLockAcquireAt(const char *path) {
    if (g_scriptLockHeld) return 1;
    snprintf(g_scriptLockPath, sizeof(g_scriptLockPath), "%s", path);
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = open(g_scriptLockPath, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) {
            char b[32];
            int n = snprintf(b, sizeof(b), "%ld\n", (long)getpid());
            if (write(fd, b, (size_t)n) != n) { /* the pid is advisory only */ }
            close(fd);
            g_scriptLockHeld = 1;
            return 1;
        }
        // the lock exists: is its owner still a live bar?
        int ofd = open(g_scriptLockPath, O_RDONLY);
        long owner = 0;
        if (ofd >= 0) {
            char b[32];
            ssize_t r = read(ofd, b, sizeof(b) - 1);
            close(ofd);
            if (r > 0) { b[r] = 0; owner = strtol(b, NULL, 10); }
        }
        if (lockOwnerAlive(owner)) return 0;   // a live bar owns it: do not load
        // stale (the owner is gone, or it is not a bar any more): take it over
        unlink(g_scriptLockPath);
    }
    return 0;
}

static int scriptLockAcquire(void) {
    char path[600];
    if (!scriptLockPath(path, sizeof(path))) return 0;
    return scriptLockAcquireAt(path);
}

static void scriptLockRelease(void) {
    if (!g_scriptLockHeld) return;
    unlink(g_scriptLockPath);
    g_scriptLockHeld = 0;
}

// The lock's self-test, driven by the behaviour suite against a lock file IT
// seeds: a live bar's lock is refused, a dead owner's is taken over, and a lock
// left by a killed bar whose pid has been RECYCLED by an unrelated process is
// taken over too - that last case is the wedge, where the bar silently gives up
// on bar.align 1 for the rest of its life. Exercising the real acquire is the
// only way to prove the liveness judgement, so the suite never re-implements it.
// argv: --self-test-lock <lockfile>. The pid the suite wants to seed is the
// lock file's own content, so there is nothing else to pass.
static int scriptLockSelfTest(const char *path) {
    int got = scriptLockAcquireAt(path);
    long named = 0;
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%ld", &named) != 1) named = 0; fclose(f); }
    if (got) {
        // whatever the outcome, the lock must now name THIS process: a takeover
        // that left the dead pid in place would pass the next probe as stale too
        int ok = named == (long)getpid();
        printf("took=%d named=%ld self=%ld\n", got, named, (long)getpid());
        scriptLockRelease();
        printf("%s\n", ok ? "LOCK-OK" : "LOCK-BAD");
        return ok ? 0 : 1;
    }
    printf("took=0 named=%ld\n", named);
    printf("%s\n", "LOCK-REFUSED");
    return 1;
}

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
        g_scriptCheckAt = 0;   // probe the script state on the fresh connection
    }
    // SELF-HEAL: anything that unloads the KWin helper (a KWin restart, another
    // tool calling unloadScript) drops bar.align 1 WITHOUT telling the bar, and
    // it used to stay lost for the rest of the bar's life - measured three times
    // on this machine. Probe occasionally and reload ONLY when it is really gone.
    //
    // The `loaded == 0` test is what keeps this from stacking copies. KWin runs
    // every loadScript as a NEW instance of the same file, and the helper polls
    // the compositor's main thread every 250 ms, so a reload on a script that is
    // already loaded doubles the compositor's work - two copies froze the whole
    // desktop for a minute on 2026-10-24. A reload is therefore only ever issued
    // for a script KWin says is NOT loaded, and geomBusReloadScript unloads
    // first so even a race between two bar processes ends with one instance.
    // The probe is one small D-Bus call every 30 s, not a hot loop.
    if (now >= g_scriptCheckAt) {
        g_scriptCheckAt = now + 30;
        // Only the bar that owns org.chocobar.Geom may manage the helper. A
        // second bar (a dev build beside the released one, or a service restart
        // overlapping the old process) must not install, reload or unload a
        // compositor-side script that belongs to the other bar's session: two
        // pollers stacked in KWin is exactly what froze the desktop for a minute
        // on 2026-10-24. Such a bar reports it once and stays out of the way.
        int loaded = geomBusOwnsName() ? geomBusScriptLoaded(KW_SCRIPT_NAME) : -1;
        if (loaded == 0) {
            char sp[600];
            if (kwScriptPath(sp, sizeof(sp)) && scriptLockAcquire()) {
                // re-check under the lock: another bar process may have loaded it
                // between the probe above and the lock being taken
                if (geomBusScriptLoaded(KW_SCRIPT_NAME) == 0 &&
                    geomBusReloadScript(sp, KW_SCRIPT_NAME))
                    dbg("kwin script was unloaded; reloaded it (one instance)");
            }
        }
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

// THE BLUR-BEHIND HINT IS DELIBERATELY NOT SET. KWin's blur (the property
// Konsole/Dolphin set) composited with this desktop's blur effect turns the
// bar into a flat washed-out strip: measured on the session's own wallpaper,
// the bar's empty middle read std=8.4 with the hint set and std=25.0 one
// second after removing it, and the README's reference screenshot (the look
// the bar is built for) measures std=34 - the raw backdrop, not the blur.
// The config's "acrylic" is honored as the translucent tint itself: the
// warm glass the reference shows IS the wallpaper showing through 47% cream.
// This desktop's effect is a third-party one (better_blur_dx with Brightness
// and Saturation post-processing), and stock KWin blur would flatten the
// backdrop too, only less. A window that asks for no special treatment gets
// none, which is also why nothing here re-declares anything per resize.
static void setSizeHints(int x, int y);   // defined below, with the other window hints

static void resizeWindow(int w, int h) {
    if (w == g_w && h == g_h) return;
    g_w = w; g_h = h;
    XResizeWindow(g_dpy, g_win, (unsigned)g_w, (unsigned)g_h);
    allocSurface();
    applyShape();
    // the hints carry the size as well as the position, and a window manager
    // places a managed window from them on map: a resize must refresh them here
    // or the next map shows the OLD rectangle for a frame
    setSizeHints(g_x, g_y);
}

// Sizing is ONE operation, not four: the rounded XShape region is in window
// coordinates, so a resize that skips applyShape() leaves the window clipped to
// the OLD rounded rect, and a surface kept at the old size paints into freed
// memory. Every size change in this file goes through resizeWindow() so the
// three-step invariant (window, surface, shape) holds by construction.
// The effective bar height in LOGICAL px - the config's own unit - with ONE
// definition of the clamp: a height the bar would draw below 8 device px is
// raised to 24 DEVICE px (24/scale logical). resizeToContent() draws it and
// barYFor() derives y from it, so the two can never disagree: with the raw
// configured height a short bar's bottom edge would sit below the terminal's top
// edge and overlap it.
static double barHeightLogical(void) {
    double h = cbCfgHeight();
    if ((int)(h * g_scale + 0.5) < 8) h = 24 / g_scale;
    return h;
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
    int h = (int)(barHeightLogical() * g_scale + 0.5);
    resizeWindow(w, h);
}

static void moveWindow(int x, int y) {
    g_x = x; g_y = y;
    // keep WM_NORMAL_HINTS truthful: it is what a window manager reads when it
    // places the window again after a restart, so a stale value here would snap
    // the bar back to an old position for a moment.
    setSizeHints(x, y);
    XMoveWindow(g_dpy, g_win, g_x, g_y);
}

// ---- position -----------------------------------------------------------------------
// The bar's position is DERIVED, never user-dragged: x and width come from the
// terminal (bar.align), y follows the TERMINAL's top edge the way Windows does
// (p_ui.c: y = terminal.top - barHeight - gap), so the bar rides just above the
// terminal wherever it is. Nothing is persisted: a dragged bar was the Windows
// bar's affordance, and on Linux the alignment contract makes a manual position
// meaningless.
static int g_hidden;             // no room above the terminal: bar unmapped

// The y for the current tick, in device pixels. -1 = no room above the terminal
// (the bar hides, the Windows hideBar() behaviour). With align 1 but no usable
// report (missing, or older than 30 s) it returns the screen-top position, so a
// stopped helper or a KWin restart drops the bar to the top instead of pinning
// it to a terminal that no longer exists.
static int barYFor(void) {
    if (cbCfgAlign() == 1 && g_geom.valid && time(NULL) - g_geomAt < 30) {
        // g_geom carries LOGICAL pixels (KWin's coordinate space); the config's
        // height and gap are CSS px, so the arithmetic stays logical and only
        // the result is scaled - exactly p_ui.c's ef.top - bh - gap. bh is the
        // DRAWN height (barHeightLogical, the clamped one), so the strip's
        // bottom edge always sits the gap above the terminal's top edge.
        int yLog = g_geom.y - (int)(barHeightLogical() + 0.5) - cbCfgGap();
        if (yLog < 0) return -1;
        return (int)(yLog * g_scale + 0.5);
    }
    return (int)(cbCfgGap() * g_scale + 0.5);
}

static void positionInit(int screenW) {
    int aw = 0, ax = 0;
    if (applyAlign(&aw, &ax)) g_x = ax;
    else g_x = (screenW - g_w) / 2;
    int wantY = barYFor();
    g_y = wantY < 0 ? (int)(cbCfgGap() * g_scale + 0.5) : wantY;
    if (wantY < 0) g_hidden = 1;
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

// The pet chip's click handler (defined next to spawnDetached, below): start or
// stop the modules.pet.exePath process.
static void petToggle(void);

// The ONE definition of "this chip does something on click": the tokens chip
// (opens the dashboard) and the pet chip (toggles the pet process). Both are in
// the LEFT group, and the click handler below, the hover pill and the cursor
// shape all ask this function, so a right-group metric can never become a
// button by any one of those paths drifting. Right-group metrics are labels.
static int chipClickable(const Chip *c) {
    return c->align == CHIP_LEFT && (c->icon == SVG_DIAMOND || c->icon == SVG_BOW);
}

static void onButton(XButtonEvent *e) {
    dbg("ButtonPress button=%u x_root=%d y_root=%d", e->button, e->x_root, e->y_root);
    // any new press ends the gesture the menu may have marked for swallowing:
    // a fresh click is a fresh gesture, and its release must be honoured
    g_swallowRelease = 0;
    if (e->button == 1) {
        // a left click on the token chip opens the dashboard (the Windows bar's
        // behaviour); a left click on the pet chip toggles the pet process
        // (p_ui.c's CT_PET click); anywhere else is inert - the bar is not
        // draggable
        int hit = chipAt(e->x);
        if (hit >= 0 && chipClickable(&g_chips[hit])) {
            if (g_chips[hit].icon == SVG_DIAMOND) {
                // TOGGLE, the Windows dashToggle behaviour: a second click on
                // the tokens chip closes the board instead of opening a second
                // one. This is the "I can open so many dashboards and they
                // crash" report: the bar had no toggle at all, so every click
                // mapped another window.
                if (dashWin() != None) { dashClose(); paint(); }
                else g_dashPending = 1;
            } else if (g_chips[hit].icon == SVG_BOW) petToggle();
        }
    }
    // A button-3 PRESS does nothing here: the menu opens on the RELEASE, the
    // Windows parity (p_ui.c WM_RBUTTONUP -> showTrayMenu). Opening on the
    // press made the menu eat its own opening release - the press opened the
    // menu at the cursor, the menu's pointer grab then received the release,
    // which landed in the padding corner (menuHit -> -1) and closed the menu
    // a few milliseconds after it appeared: "pops out and immediately pops
    // back down".
}

static void onButtonRelease(XButtonEvent *e) {
    dbg("ButtonRelease button=%u x=%d y=%d", e->button, e->x, e->y);
    // the release of the gesture that DISMISSED the menu never opens anything:
    // the menu returns MENU_DISMISSED_BY_PRESS for it (Win32's TrackPopupMenu
    // consumes the dismissing click the same way, so the Windows bar sees no
    // WM_RBUTTONUP for it). Without the swallow the menu would bounce back open
    // on the very click that closed it.
    if (g_swallowRelease) {
        g_swallowRelease = 0;
        dbg("release swallowed (menu dismissal gesture)");
        return;
    }
    if (e->button != 3) return;
    // The release must still be over the bar. The X implicit grab delivers
    // every pointer event from the press until the last release to the window
    // that got the press, wherever the pointer wanders (measured: the press
    // window receives the release at coordinates outside its own bounds and
    // the window under the pointer receives nothing), so an off-bar drag's
    // release arrives HERE. The bounds check below is the rule's enforcement,
    // exactly as on Windows, which sends WM_RBUTTONUP only over the window -
    // it is not belt-and-braces: deleting it would let an off-bar release
    // open the menu.
    if (e->x < 0 || e->x >= g_w || e->y < 0 || e->y >= g_h) return;
    g_menuPending = 1;
    g_menuX = e->x_root;
    g_menuY = e->y_root;
}

// The chip's hover TITLE, the Windows bar's chipTitle() (p_ui.c:3064) ported
// one for one: every metric chip names itself ("CPU usage", "GPU usage",
// "Memory usage", "Volume", "Battery", "Local time", "CPU temperature"), the
// tokens chip says "Token usage today", the subscription chip says what its
// rotating window is, and the pet chip has no title (as on Windows, where
// CT_PET returns NULL - the chip's own on/off text is the whole message).
// The Linux front-end cannot ask the core for these strings, so the table is
// spelled here; it is the same list in the same order as p_ui.c's switch.
static const char *chipTitle(int idx) {
    if (idx < 0 || idx >= g_chipCount) return NULL;
    const Chip *c = &g_chips[idx];
    if (c->align == CHIP_LEFT) {
        if (c->icon == SVG_DIAMOND) return "Token usage today";
        if (c->icon == SVG_GAUGE) return g_subsTip[0] ? g_subsTip : "Subscription plan remaining";
        if (c->icon == SVG_BOW) return NULL;   // the pet chip's own text says it
        return NULL;
    }
    switch (c->icon) {
        case SVG_GPU: return "GPU usage";
        case SVG_CPU: return "CPU usage";
        case SVG_TEMP: return "CPU temperature";
        case SVG_RAM: return "Memory usage";
        case SVG_VOL:
        case SVG_MUTE: return "Volume";
        case SVG_BAT: return "Battery";
        case SVG_CLOCK: return "Local time";
        default: return NULL;
    }
}

static void onMotion(XMotionEvent *e) {
    // hover tracking only: the bar is not draggable, so motion never moves it.
    int hit = chipAt(e->x);
    // the hover PILL belongs to the clickable left-group chips only, so only
    // those repaint on hover (a right-group metric has no pill to draw)
    int pill = (hit >= 0 && chipClickable(&g_chips[hit])) ? hit : -1;
    if (pill != g_hover) { g_hover = pill; paint(); }
    // every chip gets its title tooltip, exactly as on Windows: the metric
    // labels on the right group are what tell a user "this number is the GPU"
    // (the user's own report: hovering a right-group chip must say what it is)
    // ROOT coordinates, not window ones: p_ui.c passes GetCursorPos (screen
    // space) and tipShow places the box 6 px right / 16 px below that point.
    // Handing it e->x/e->y put the tip at the window-relative offset measured
    // from the screen origin - it appeared in the top-left corner of the screen
    // instead of under the pointer.
    const char *tt = chipTitle(hit);
    if (tt) tipShow(tt, e->x_root, e->y_root, "", 12.0, 0);
    else tipHide();
    // the hand over the clickable chip, the arrow everywhere else (p_ui.c
    // WM_MOUSEMOVE does the same with LoadCursor(IDC_HAND/IDC_ARROW))
    cursorUpdate(pill >= 0);
}

static void onLeave(void) {
    if (g_hover != -1) { g_hover = -1; paint(); }
    tipHide();
    cursorUpdate(0);
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
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     ButtonMotionMask | PointerMotionMask |
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
    // sticky request lives in afterMap(), which every map of this window runs.
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

// The work every map of the bar must do, the startup map and a later one alike:
// wait (bounded) for the window manager to finish managing the window, then
// rewrite _NET_WM_STATE - KWin adds DEMANDS_ATTENTION to it when a window maps
// without user activity, and re-writing the property after the map is the
// client-side contract for "attention handled" - and send the EWMH all-desktops
// request. That request is a ClientMessage to the root window, not a property
// write, and it only works once the window is managed, which is why both live
// here instead of in createWindow(). A map that skips this work (the tick's,
// after a born-hidden start) leaves the bar on one desktop with attention set.
static void afterMap(void) {
    XEvent ev;
    XFlush(g_dpy);
    for (int i = 0; i < 200; i++) {   // bounded: never block the loop
        if (XCheckTypedWindowEvent(g_dpy, g_win, MapNotify, &ev)) break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    Atom wmState = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    Atom below = XInternAtom(g_dpy, "_NET_WM_STATE_BELOW", False);
    Atom skipTask = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom skipPager = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom sticky = XInternAtom(g_dpy, "_NET_WM_STATE_STICKY", False);
    Atom states[4] = { below, skipTask, skipPager, sticky };
    XChangeProperty(g_dpy, g_win, wmState, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, 4);
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

// ---- cursors ------------------------------------------------------------------
// The bar used to define NO cursor, so the pointer over it showed Xwayland's
// ROOT cursor. That root cursor is loaded by Xwayland at the raw XCURSOR_SIZE
// (60 here) while the desktop renders the same theme cursor SCALED by the
// output factor (60 * 1.59 = ~96 px) - so the cursor visibly SHRANK whenever it
// crossed the bar. Exactly the same symptom the Electron/ChatGPT app had before
// it was given Wayland fractional scaling: the app drew its cursor unscaled in
// a scaled world. The fix here is the X11 equivalent: load the theme cursor at
// the SCALED size and define it on the window, so the pointer over the bar is
// the cursor the desktop shows everywhere else.
//
// Windows does the same thing with LoadCursor(IDC_ARROW/IDC_HAND): the system
// picks the theme cursor at the system size, and the hand appears over the
// clickable chip. This mirrors that, with the hand switching on hover.
static Cursor g_curArrow;
static Cursor g_curHand;
static int g_curOn = 0;   // 0 = arrow, 1 = hand (what is currently defined)

// The desktop's cursor size: XCURSOR_SIZE from the environment (the same value
// KWin reads) multiplied by the DPI scale KWin applies when rendering it. A
// cursor theme carries fixed sizes, so libXcursor picks the closest one and the
// exact rounding does not matter - 60 * 1.59 lands on the theme's 96.
static int cursorBaseSize(void) {
    const char *s = getenv("XCURSOR_SIZE");
    if (s && *s) {
        int v = atoi(s);
        if (v > 0 && v < 512) return v;
    }
    return 24; // the X default; the theme is what matters, not this guess
}

// menu.c loads its own grab cursor from the same numbers: one definition of
// "how big the desktop cursor is", not two chances to disagree with KWin.
int cbCursorBaseSize(void) { return cursorBaseSize(); }
double cbCursorScale(void) { return g_scale; }

static Cursor loadCursor(const char *name) {
    // theme = NULL: libXcursor resolves XCURSOR_THEME from the environment, so
    // the bar shows the SAME theme the desktop shows.
    int want = (int)(cursorBaseSize() * g_scale + 0.5);
    XcursorImage *img = XcursorLibraryLoadImage(name, NULL, want);
    if (!img) return None; // no such shape: leave the root cursor, do not invent one
    Cursor c = XcursorImageLoadCursor(g_dpy, img);
    XcursorImageDestroy(img);
    return c;
}

static void cursorsInit(void) {
    g_curArrow = loadCursor("left_ptr");
    g_curHand = loadCursor("hand2");
    if (g_curArrow == None) return; // nothing to define: keep the root cursor
    XDefineCursor(g_dpy, g_win, g_curArrow);
    g_curOn = 0;
}

// The cursor follows the hover state: a hand over the clickable chip, the arrow
// everywhere else. Defining only on a CHANGE keeps this cheap (an X call per
// crossing, not per motion event).
static void cursorUpdate(int clickable) {
    if (g_curArrow == None) return;
    Cursor want = (clickable && g_curHand != None) ? g_curHand : g_curArrow;
    if (want == None) return;
    int on = (want == g_curHand) ? 1 : 0;
    if (on == g_curOn) return;
    XDefineCursor(g_dpy, g_win, want);
    g_curOn = on;
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
// The change token for the config file. The mtime alone is too coarse to tell
// two states of the file apart: it has whole-second resolution, so a rewrite
// that lands in the same second as the previous one is invisible, and a file
// that is being rewritten RIGHT NOW (truncated, not yet written) carries the
// same mtime as the finished one. The bar then reads the half-written file,
// the core keeps the previous config, and - because the token already moved -
// the finished file is never reloaded: the change is lost for good. The full
// stat identity (seconds AND nanoseconds AND the size) tells those states
// apart, which is the same precision the Windows bar gets from its FILETIME
// token.
typedef struct {
    time_t sec;
    long nsec;
    off_t size;
} CfgId;

static CfgId g_cfgId = { 0, 0, 0 };
static char g_cfgPath[512];

static int cfgId(CfgId *out) {
    struct stat st;
    if (stat(g_cfgPath, &st) != 0) return 0;
    out->sec = st.st_mtim.tv_sec;
    out->nsec = st.st_mtim.tv_nsec;
    out->size = st.st_size;
    return 1;
}

static int cfgIdSame(const CfgId *a, const CfgId *b) {
    return a->sec == b->sec && a->nsec == b->nsec && a->size == b->size;
}

// Reload the config and keep the change token in step with what was actually
// loaded. The token only moves when the file did not change WHILE it was
// being read: a file that is being rewritten is read half-written, the core
// keeps the previous config, and the token has to stay where it was for the
// next tick to see the finished file as a change worth reloading.
static void configReload(void);

static void reloadConfig(void) {
    wchar_t wide[1024];
    int wl = MultiByteToWideChar(CP_UTF8, 0, g_cfgPath, -1, wide, 1024);
    if (wl <= 0) return;
    cbLoadConfig(wide);
    // subs.rotateSec: how long each plan stays on the gauge chip (p_ui.c does
    // the same on Windows, right after loadConfig)
    cbSubsSetRotateSec(cbCfgSubsRotateSec());
    resolveFontFamily();
}

static void configReload(void) {
    CfgId before;
    int have = cfgId(&before);
    reloadConfig();
    if (!have) return;
    CfgId after;
    if (!cfgId(&after)) return;
    if (cfgIdSame(&before, &after)) g_cfgId = after;
}

// ---- the right-click menu and the dashboards --------------------------------------

// A dashboard is a second window driven by the bar's own loop: one connection,
// two windows, one pump, exactly the Windows shape (one GetMessage loop
// dispatching the bar and the board while the bar's WM_TIMER keeps ticking).
// A nested event loop here would freeze the bar's clock for as long as the
// board stays open.
static void runDashboard(void) {
    Rgb bgc = hexRgb(cbCfgPinkBg(), (Rgb){0.996, 0.969, 0.976});
    Rgb cardc = hexRgb(cbCfgPink(), (Rgb){0.941, 0.871, 0.894});
    Rgb fgc = hexRgb(cbCfgFg(), (Rgb){0.03, 0.03, 0.03});
    Rgb dimc = hexRgb(cbCfgFgDim(), (Rgb){0.35, 0.32, 0.27});
    Rgb divc = hexRgb(cbCfgDivider(), (Rgb){0.85, 0.80, 0.70});
    double ramp[5][3];
    for (int i = 0; i < 5; i++) {
        Rgb c = hexRgb(cbCfgHeat(i), (Rgb){0.9, 0.85, 0.8});
        ramp[i][0] = c.r; ramp[i][1] = c.g; ramp[i][2] = c.b;
    }
    double page[3] = { bgc.r, bgc.g, bgc.b };
    double card[3] = { cardc.r, cardc.g, cardc.b };
    double fg[3] = { fgc.r, fgc.g, fgc.b };
    double dim[3] = { dimc.r, dimc.g, dimc.b };
    double div[3] = { divc.r, divc.g, divc.b };
    int cw = cbCfgDashW() > 0 ? cbCfgDashW() : 900;
    int chh = cbCfgDashH() > 0 ? cbCfgDashH() : 520;
    Visual *dvis = DefaultVisual(g_dpy, g_screen);
    Colormap dcmap = DefaultColormap(g_dpy, g_screen);
    int ddepth = DefaultDepth(g_dpy, g_screen);
    Rgb pinkc = hexRgb(cbCfgPink(), (Rgb){0.941, 0.871, 0.894});
    Rgb pinkdc = hexRgb(cbCfgPinkDeep(), (Rgb){0.83, 0.58, 0.67});
    Rgb yellowc = hexRgb(cbCfgYellow(), (Rgb){0.72, 0.66, 0.42});
    double pink3[3] = { pinkc.r, pinkc.g, pinkc.b };
    double pinkd3[3] = { pinkdc.r, pinkdc.g, pinkdc.b };
    double yellow3[3] = { yellowc.r, yellowc.g, yellowc.b };
    dashOpen(g_dpy, g_screen, dvis, dcmap, ddepth, cw, chh, g_scale,
             g_fontFamily, page, card, fg, dim, div, ramp,
             pink3, pinkd3, yellow3, (long long)g_lastScan);
}

// Reap every child of the bar that has exited. The pet and the detached spawns
// are fire-and-forget, and a long-lived service that never waits keeps one
// defunct entry per invocation for as long as it runs. runTimed's child is
// spawned and waited inside one call - a corpse that outlives its kill budget
// is abandoned for exactly this loop to collect - and this runs from the same
// single event loop, so it never takes a child that call still owns.
static void reapChildren(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
}

// Resolve a program name to a path exec() can take, in the PARENT, before any
// fork. The child of a fork() in this threaded bar may call only
// async-signal-safe functions until exec() replaces it (POSIX): the malloc
// lock can be held by another thread at the instant of the fork, so a child
// that allocates - the PATH search execlp does - can deadlock before it ever
// reaches the exec. The search therefore happens here, where allocating is
// allowed.
// 1 = out holds a path exec() can take, 0 = nothing was found.
static int programPath(const char *name, char *out, size_t cap) {
    if (!name || !*name) return 0;
    if (strchr(name, '/')) {
        // an explicit path needs no search, and exec() takes it as it is
        if (strlen(name) >= cap) return 0;
        strcpy(out, name);
        return 1;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    const char *p = path;
    for (;;) {
        const char *sep = strchr(p, ':');
        size_t dlen = sep ? (size_t)(sep - p) : strlen(p);
        char dir[PATH_MAX];
        const char *d = ".";
        if (dlen) {
            if (dlen < sizeof dir) { memcpy(dir, p, dlen); dir[dlen] = 0; d = dir; }
            else d = NULL;
        }
        if (d) {
            char cand[PATH_MAX];
            if (snprintf(cand, sizeof cand, "%s/%s", d, name) < (int)sizeof cand &&
                access(cand, X_OK) == 0) {
                struct stat st;
                if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
                    if (strlen(cand) >= cap) return 0;
                    strcpy(out, cand);
                    return 1;
                }
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
    return 0;
}

// Run an external program without blocking the bar's loop: a config editor or a
// file manager must not freeze the strip. The program is resolved to a path
// exec() can take BEFORE the fork, so the child only has to exec it: execlp's
// PATH search allocates, and the child of a fork in this threaded bar may not
// allocate before the exec (see programPath).
static void spawnDetached(const char *argv0, const char *argv1) {
    char prog[PATH_MAX];
    if (!programPath(argv0, prog, sizeof prog)) {
        fprintf(stderr, "chocobar: cannot run %s: not found\n", argv0 ? argv0 : "");
        return;
    }
    pid_t pid = fork();
    if (pid != 0) return;
    setsid();
    char *args[3];
    args[0] = (char *)argv0;
    args[1] = argv1 ? (char *)argv1 : NULL;
    args[2] = NULL;
    execv(prog, args);
    _exit(127);
}

// The pet chip's click: start or stop the modules.pet.exePath process, the same
// toggle the Windows bar has (p_ui.c petToggle, CT_PET chip). The configured
// value is ONE path token and both halves agree on that: petPathUsable refuses
// a value that is not an absolute path to a regular executable file (a bare
// name, a relative path, a directory, or a command line that carries
// arguments), and the start execs that path directly - no shell - so the
// process the bar starts has the configured path as its EXECUTED file: argv[0]
// for a binary, and the first non-option token after the shebang interpreter
// for a script (the kernel hands it to the interpreter, and a shebang that
// carries an argument puts the script at argv[2]), and the matcher (the chip's
// own state source) sees the pet it started. Stop is a verified SIGTERM per
// matching pid (petProcessStop re-checks each cmdline).
//
// Known consequence, deliberate: a child cannot escape its parent's cgroup, so
// the pet the bar starts runs in chocobar.service's cgroup and inherits
// everything that unit applies: a service restart stops it (the default
// KillMode=control-group), it lives under the unit's MemoryMax, and it runs at
// the unit's Nice and IOSchedulingClass. A pet that needs more than the unit
// allows must be started by the user instead. The pet is a companion to the
// bar, and dying with a bar restart beats outliving one with no way to toggle
// it off.
// One pet-start handshake may be in flight at a time: the click parks the
// CLOEXEC pipe's read end here and returns, and the tick settles it
// (petHandshakeDrain) - no click ever waits on a fork child's exec, so a slow
// exec can never park the bar's single event loop with it. The park is BOUNDED:
// a child that never lands its exec (an unresponsive mount, a stopped child)
// holds the write end open, so the tick closes the pipe and reports an
// unsettled start once PET_HANDSHAKE_MS has passed. Without that deadline the
// pipe stays parked for ever, every later click is refused with no log line at
// all, and the stuck child is never reaped.
#define PET_HANDSHAKE_MS 5000
static int g_petPipe = -1;
static char g_petPipePath[512];
static struct timespec g_petPipeAt;   // when the handshake was parked

static void petToggle(void) {
    char petPath[512] = {0};
    const wchar_t *wp = cbCfgPetPath();
    if (!wp || !*wp) return;
    // the config string is wchar_t (UTF-32 here), so it MUST be converted rather
    // than cast - the same rule the metrics poll follows
    WideCharToMultiByte(65001 /*CP_UTF8*/, 0, wp, -1, petPath, sizeof(petPath), NULL, NULL);
    if (!petPathUsable(petPath)) return;

    if (petProcessRunning(petPath)) {
        int n = petProcessStop(petPath);
        fprintf(stderr, "chocobar: pet: stopped: %s (%d process%s)\n",
                petPath, n, n == 1 ? "" : "es");
        return;
    }
    // the matcher cannot see a fork whose exec has not landed yet, so a second
    // click inside that window would start a duplicate: one handshake at a time.
    // The refusal is LOGGED: a click the user made must never be silent, so a
    // pending handshake reports itself rather than swallowing the click.
    if (g_petPipe >= 0) {
        fprintf(stderr, "chocobar: pet: start of %s has not reported yet, "
                        "this click was ignored\n", g_petPipePath);
        return;
    }
    // The exec's outcome comes back over a CLOEXEC pipe: the write end closes
    // at a successful exec, and a failed exec writes the errno and exits. The
    // click never waits for it - the pipe is parked and the tick settles it,
    // which prints the one true line ("starting" only when the kernel took the
    // exec, "cannot start" when it did not) - so a slow exec leaves the bar
    // fully live while the handshake pends. The child itself may not
    // report anything: after a fork in this threaded bar it may only call
    // async-signal-safe functions until exec, and printing (dprintf) or
    // describing an error (strerror) allocates - a fork that lands while the
    // subs thread holds the allocator lock would leave the child stuck before
    // the exec, and the pet would silently never start.
    int pfd[2];
    if (pipe(pfd)) {
        fprintf(stderr, "chocobar: pet: cannot start %s: %s\n", petPath, strerror(errno));
        return;
    }
    // The write end must not survive the exec: its closing IS the success
    // signal. It is moved above the standard streams first, so the child's
    // redirect of 0/1/2 to /dev/null can never land on it.
    int wfd = fcntl(pfd[1], F_DUPFD, 10);
    if (wfd < 0) {
        int e = errno;
        close(pfd[0]);
        close(pfd[1]);
        fprintf(stderr, "chocobar: pet: cannot start %s: %s\n", petPath, strerror(e));
        return;
    }
    close(pfd[1]);
    fcntl(wfd, F_SETFD, FD_CLOEXEC);
    // the tick drains this pipe, so its read end may never block: O_NONBLOCK
    // or no start at all
    int fl = fcntl(pfd[0], F_GETFL, 0);
    if (fl < 0 || fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK) < 0) {
        int e = errno;
        close(pfd[0]);
        close(wfd);
        fprintf(stderr, "chocobar: pet: cannot start %s: %s\n", petPath, strerror(e));
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(pfd[0]);
        close(wfd);
        fprintf(stderr, "chocobar: pet: cannot start %s: %s\n", petPath, strerror(e));
        return;
    }
    if (pid == 0) {
        close(pfd[0]);
        setsid();
        // the pet is a GUI app: nothing it prints belongs on the bar's stderr
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        char *args[2] = { petPath, NULL };
        execv(petPath, args);
        // the exec was refused: hand the errno to the parent and leave.
        // write() and _exit() are async-signal-safe, and nothing else may run
        // in this child before it is gone.
        int e = errno;
        ssize_t ignored = write(wfd, &e, sizeof e);
        (void)ignored;
        _exit(127);
    }
    close(wfd);
    // the click's work is done: the handshake is parked for the tick, and a
    // slow exec holds nothing but this pipe. The park is stamped so the tick can
    // enforce its deadline.
    clock_gettime(CLOCK_MONOTONIC, &g_petPipeAt);
    snprintf(g_petPipePath, sizeof g_petPipePath, "%s", petPath);
    g_petPipe = pfd[0];
}

// Settle the parked pet-start handshake, from the tick: bytes on the pipe are
// the child's refused-exec errno, EOF is the write end dying with a successful
// exec, and EAGAIN means the exec is still in flight - the handshake settles
// whenever it settles, and the bar keeps ticking meanwhile. A park that has
// outlived PET_HANDSHAKE_MS is abandoned: closing the read end releases the
// child (its write end then fails or is inherited, and the ordinary child
// reaping collects it once it exits), the state clears so the next click can
// try again, and the log carries the truth - the start never reported, so
// nothing claims it started. The deadline never costs a working start: an exec
// that lands reports its outcome first, and this branch is only reached when
// nothing has been reported for the whole budget.
static void petHandshakeDrain(void) {
    if (g_petPipe < 0) return;
    int execErr = 0;
    ssize_t got = read(g_petPipe, &execErr, sizeof execErr);
    if (got < 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - g_petPipeAt.tv_sec) * 1000 +
                       (now.tv_nsec - g_petPipeAt.tv_nsec) / 1000000;
        if (elapsed < PET_HANDSHAKE_MS) return;
        close(g_petPipe);
        g_petPipe = -1;
        fprintf(stderr, "chocobar: pet: start of %s did not report within "
                        "%d ms, the start is not confirmed\n",
                g_petPipePath, PET_HANDSHAKE_MS);
        return;
    }
    close(g_petPipe);
    g_petPipe = -1;
    if (got == (ssize_t)sizeof execErr) {
        fprintf(stderr, "chocobar: pet: cannot start %s: %s\n",
                g_petPipePath, strerror(execErr));
        return;
    }
    fprintf(stderr, "chocobar: pet: starting: %s\n", g_petPipePath);
}

static void configDirOf(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.wizbar", home ? home : "/tmp");
}

// Run a short-lived helper to completion under a hard time budget. Returns the
// child's exit status (0..255), or -1 when the spawn failed or the budget ran
// out. systemctl --user is a local, fast operation (measured: under 10 ms), but a
// wedged one must not stall the bar's loop: the budget kills it and the caller
// reports the failure. SIGKILL is not instant - a child in uninterruptible
// sleep takes the signal only when the kernel releases it - so the kill gets
// the same budget to land, reaped without blocking, and a corpse that never
// arrives is left to the ordinary child reaping (reapChildren) instead of
// waited for. The budget BOUNDS the stall, it does not eliminate it: runTimed
// is synchronous inside the bar's single event loop, so a slow child delays
// whatever the loop was about to do (a menu open, a clock tick) by up to the
// budget, twice while a killed child lingers, and the toggle path pays it
// twice (is-enabled + enable). Bounded and deliberate.
//
// out/err (either may be NULL) receive the child's stdout and stderr, truncated
// and NUL-terminated. stdout is captured because systemctl reports a unit's
// state as a WORD there ("enabled", "disabled", "masked"), and its exit code
// cannot distinguish them - `is-enabled` exits 1 for "disabled" and for every
// generic failure alike. stderr carries the failure text ("Unit ... could not
// be found", "Failed to connect to user scope bus"); swallowing it made the
// common unit-file-not-installed case undiagnosable. A NULL buffer sends that
// stream to /dev/null: the bar's own output is a log line per real event, not
// another tool's chatter.
static void captureDrain(int fd, char *buf, size_t cap, size_t *len) {
    if (fd < 0 || !buf || cap == 0) return;
    char tmp[512];
    ssize_t n = read(fd, tmp, sizeof tmp);
    if (n <= 0) return;
    size_t take = (size_t)n;
    if (take > cap - 1 - *len) take = cap - 1 - *len;
    if (take == 0) return;
    memcpy(buf + *len, tmp, take);
    *len += take;
    buf[*len] = 0;
}

// One capture pipe. The read end is non-blocking so a quiet child cannot stall
// the poll loop below; 0 means "capture nothing", and the caller sends that
// stream to /dev/null instead.
static int captureOpen(int fds[2], char *buf, size_t cap) {
    fds[0] = fds[1] = -1;
    if (!buf || cap == 0) return 0;
    buf[0] = 0;
    if (pipe(fds)) { fds[0] = fds[1] = -1; return 0; }
    int fl = fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0) fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    return 1;
}

static int runTimed(const char *const argv[], long budgetMs,
                    char *out, size_t outCap, char *err, size_t errCap) {
    extern char **environ;
    pid_t pid = -1;
    int outfds[2], errfds[2];
    size_t outLen = 0, errLen = 0;
    int outOpen = captureOpen(outfds, out, outCap);
    int errOpen = captureOpen(errfds, err, errCap);
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa)) {
        if (outOpen) { close(outfds[0]); close(outfds[1]); }
        if (errOpen) { close(errfds[0]); close(errfds[1]); }
        return -1;
    }
    if (outOpen) {
        posix_spawn_file_actions_adddup2(&fa, outfds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, outfds[1]);
        posix_spawn_file_actions_addclose(&fa, outfds[0]);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    if (errOpen) {
        posix_spawn_file_actions_adddup2(&fa, errfds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&fa, errfds[1]);
        posix_spawn_file_actions_addclose(&fa, errfds[0]);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    int spawned = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    // the parent's copies of the write ends MUST go: while one is open the pipe
    // never reaches EOF, so the read below would block for ever once the child
    // is gone - and the bar's single event loop would hang with it
    if (outOpen) { close(outfds[1]); outfds[1] = -1; }
    if (errOpen) { close(errfds[1]); errfds[1] = -1; }
    if (spawned != 0 || pid < 0) {
        if (outOpen) close(outfds[0]);
        if (errOpen) close(errfds[0]);
        return -1;
    }
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        // drain both streams while waiting: their output is short, so one read
        // per poll keeps the buffers empty without blocking
        if (outOpen) captureDrain(outfds[0], out, outCap, &outLen);
        if (errOpen) captureDrain(errfds[0], err, errCap, &errLen);
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (outOpen) { captureDrain(outfds[0], out, outCap, &outLen); close(outfds[0]); }
            if (errOpen) { captureDrain(errfds[0], err, errCap, &errLen); close(errfds[0]); }
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        }
        if (r < 0) {
            if (outOpen) close(outfds[0]);
            if (errOpen) close(errfds[0]);
            return -1;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (elapsed >= budgetMs) {
            kill(pid, SIGKILL);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (;;) {
                if (outOpen) captureDrain(outfds[0], out, outCap, &outLen);
                if (errOpen) captureDrain(errfds[0], err, errCap, &errLen);
                pid_t kr = waitpid(pid, &status, WNOHANG);
                if (kr == pid || kr < 0) break;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long killWait = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
                if (killWait >= budgetMs) break;
                struct timespec kw = { 0, 2 * 1000 * 1000 };
                nanosleep(&kw, NULL);
            }
            if (outOpen) close(outfds[0]);
            if (errOpen) close(errfds[0]);
            return -1;
        }
        struct timespec w = { 0, 2 * 1000 * 1000 };
        nanosleep(&w, NULL);
    }
}

// The autostart state is the systemd user unit (the Linux Run key): the menu
// item "Start on login" toggles it, the same control the Windows tray's "Start
// with Windows" has over its own mechanism. The tri-state keeps a failed read
// from masquerading as "disabled": -1 = unknown, 0 = disabled, 1 = enabled.
//
// The DECIDING input is the state WORD `is-enabled` prints, never its exit code:
// systemctl exits 1 for ANY generic failure, not only for a disabled unit
// (measured: an unreachable user manager prints "Failed to connect to user scope
// bus" and exits 1, exactly like "disabled"). Reading the exit code therefore
// reports a broken or masked unit as disabled, shows "Start on login" unticked,
// and the toggle then flips the unit blind. Anything that is not a definite
// enabled/disabled word - "masked", "not-found", "static", an empty answer -
// is unknown, and the bar logs the answer and the child's error text.
static int autoStartEnabled(void) {
    const char *argv[] = { "systemctl", "--user", "is-enabled", "chocobar", NULL };
    char out[128], err[256];
    int rc = runTimed(argv, 500, out, sizeof out, err, sizeof err);
    char *word = out;
    while (*word == ' ' || *word == '\t' || *word == '\r' || *word == '\n') word++;
    word[strcspn(word, " \t\r\n")] = 0;
    if (!strcmp(word, "enabled") || !strcmp(word, "enabled-runtime")) return 1;
    if (!strcmp(word, "disabled")) return 0;
    if (rc < 0)
        fprintf(stderr, "chocobar: autostart state unknown: systemctl did not "
                        "run (spawn failure or timed out)\n");
    else
        fprintf(stderr, "chocobar: autostart state unknown (systemctl exit %d, "
                        "answer \"%s\"): %s\n", rc, word,
                *err ? err : "(no error text)");
    return -1;
}

static void autoStartSet(int on) {
    const char *argv[] = { "systemctl", "--user", on ? "enable" : "disable", "chocobar", NULL };
    char err[256];
    int rc = runTimed(argv, 500, NULL, 0, err, sizeof err);
    if (rc == 0) return;
    if (rc < 0)
        fprintf(stderr, "chocobar: autostart %s failed: systemctl did not run "
                        "(spawn failure or timed out)\n", on ? "enable" : "disable");
    else
        fprintf(stderr, "chocobar: autostart %s failed (systemctl exit %d): %s\n",
                on ? "enable" : "disable", rc, *err ? err : "(no error text)");
}

static void handleMenuChoice(int choice) {
    dbg("menu choice = %d", choice);
    switch (choice) {
        case MENU_TOKEN_DASH:
            // the menu toggles too, exactly like the chip (Windows dashToggle is
            // reached from both the chip and the tray item)
            if (dashWin() != None) { dashClose(); paint(); }
            else g_dashPending = 1;
            break;
        case MENU_SUBS_DASH:
            fprintf(stderr, "chocobar: the subscription board is not ported to Linux yet\n");
            break;
        case MENU_AUTO_START: {
            // the menu IS the control, like the Windows Run value: the config
            // file is never rewritten, so an edited file wins until toggled.
            // The state is tri-state: an unknown read must not flip the unit
            // blind (it already logged why the read failed).
            int st = autoStartEnabled();
            if (st < 0) break;
            autoStartSet(!st);
            break;
        }
        case MENU_UPDATES:
            // Opens the releases page rather than self-updating, exactly the
            // Windows item's behaviour (p_ui.c id 6): no download, no file
            // swap, no unsigned-binary trust question.
            spawnDetached("xdg-open",
                "https://github.com/mnsky-tyan/chocobar/releases/latest");
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
            configReload();
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
// KW_SCRIPT_NAME is defined above with the geometry state; the install path uses it too.

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
    // Tell KWin to load and start it. This goes through geomBusReloadScript so
    // the reinstall takes the SAME path as the self-heal: unload first, then
    // load, then start - exactly one instance, never a stacked second poller.
    // (A stack of copies doubles and triples the compositor's main-thread work;
    // two copies froze the desktop for a minute on 2026-10-24.)
    GeomBus bus = { -1 };
    int ok = geomBusConnect(&bus);
    // a bar that is already running owns org.chocobar.Geom and owns the
    // helper too; loading a second copy from here would stack a poller in
    // KWin and double the compositor's per-tick work. geomBusConnect only
    // succeeds once this process owns the name, so !ok here means either
    // another bar holds it or the bus/KWin is unreachable - geomBusLastError
    // tells the two apart and each has its own advice.
    if (ok) {
        ok = geomBusReloadScript(path, KW_SCRIPT_NAME);
        geomBusClose(&bus);
    } else if (strstr(geomBusLastError(), "already owns")) {
        fprintf(stderr, "chocobar: another Chocobar instance is running and owns the "
                        "geometry helper.\n  Stop it first (systemctl --user stop chocobar), "
                        "then re-run --install-geometry.\n");
        return 1;
    } else {
        fprintf(stderr, "chocobar: could not talk to KWin over D-Bus (%s).\n"
                        "  The script is installed; enable it in System Settings > "
                        "Window Management > KWin Scripts,\n  or log out and back in.\n",
                geomBusLastError());
        return 1;
    }
    if (!ok) {
        fprintf(stderr, "chocobar: could not talk to KWin over D-Bus.\n"
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
    // Unload over the same bus path the loader uses. A failure here is NOT
    // best-effort: geomBusConnect only succeeds once this process owns
    // org.chocobar.Geom, so failing means a running bar holds the name and this
    // process cannot unload anything. Deleting the file anyway and printing
    // "removed" is a false success - the compositor keeps running the script it
    // already loaded, now pointed at a missing file - so both outcomes are
    // reported truthfully and the file is only deleted when the unload path was
    // actually reachable.
    GeomBus bus = { -1 };
    if (!geomBusConnect(&bus)) {
        if (strstr(geomBusLastError(), "already owns"))
            fprintf(stderr, "chocobar: another Chocobar instance is running and owns the "
                            "geometry helper.\n  Stop it first (systemctl --user stop chocobar), "
                            "then re-run --remove-geometry.\n");
        else
            fprintf(stderr, "chocobar: could not talk to KWin over D-Bus (%s).\n"
                            "  %s was left in place; unlinking it would strand a loaded "
                            "helper on a missing file.\n", geomBusLastError(), path);
        return 1;
    }
    geomBusUnloadScript(KW_SCRIPT_NAME);
    geomBusClose(&bus);
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
        } else if (!strcmp(argv[i], "--self-test-lock") && i + 1 < argc) {
            // the behaviour suite's way to exercise the REAL acquire logic
            return scriptLockSelfTest(argv[i + 1]);
        } else if (!strcmp(argv[i], "--version")) {
            // the release workflow verifies the built asset the same way it
            // verifies the Windows exe: the binary itself must carry the tag
            printf("%s\n", CB_VER_STR);
            return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("chocobar-linux [--config PATH]\n"
                   "  floating status bar: cpu, ram, temp, volume, battery, clock, tokens\n"
                   "  right-click opens the menu; left-click on the tokens chip opens\n"
                   "  the dashboard (the bar itself is not draggable)\n"
                   "  --install-geometry  load the KWin helper that makes bar.align 1\n"
                   "                      (match the active terminal's width) work\n"
                   "  --remove-geometry   unload and delete that helper\n"
                   "  --version           print the version and exit\n"
                   "  --self-test-lock F   (internal) exercise the helper lock on F\n");
            return 0;
        }
    }
    if (!g_cfgPath[0]) resolveConfigPath();

    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);

    // core: config lock, cursors, config load (the same startup the macOS bar does)
    cbCoreInit();
    cbTokensInit();
    configReload();
    resolveFontFamily();
    // the provider fetch loop: it does nothing until a provider is enabled, and
    // its first cycle lands a few seconds in, so starting it before the window
    // exists costs nothing and the gauge chip already has data by first paint
    cbSubsStart();
    // The KDE Rounded Corners effect draws its own 1px black outline over the
    // client's pixels, which rides the bar's border. Exclude the bar by window
    // class so the border the bar paints is the border the user sees.
    cbKwinSelfHeal();

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
    // the tooltip surface draws on the same display/visual as the bar and menu
    tipInit(g_dpy, g_screen, g_visual, g_cmap, g_depth, g_scale);
    // initial metrics so the very first paint has real numbers
    linuxMetricsPrime();
    linuxMetricsPoll(&g_m);
    scanIfDue(time(NULL));
    buildChips();

    g_w = 10; g_h = (int)(barHeightLogical() * g_scale + 0.5);
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
    // the bar's OWN cursor, at the desktop's scaled size: without this the
    // pointer over the bar shows Xwayland's root cursor at the raw XCURSOR_SIZE
    // and visibly shrinks against the desktop's scaled one (measured via the
    // same symptom on an Electron app that lacked Wayland fractional scaling)
    cursorsInit();
    // g_hidden is positionInit()'s verdict that there is no room above the
    // terminal: the window must not be mapped AT ALL while it holds - mapping
    // here would composite the strip at the top for a frame or two before an
    // unmap could take it away. Skipping the first map is safe because every
    // property a window manager needs to manage the first map it sees - the
    // _NET_WM_STATE layer and desktop hints (createWindow) and the USPosition
    // rectangle in WM_NORMAL_HINTS (setSizeHints above) - is already on the
    // window: the tick that later finds room moves and maps it the same way and
    // then runs the same afterMap() work, so a bar mapped later is managed
    // exactly like one mapped here.
    if (!g_hidden) {
        XMapWindow(g_dpy, g_win);
        afterMap();
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
            // the board's events go to the board, everything else to the bar:
            // two windows, one queue, one pump (Windows dispatches both the same
            // way through its single GetMessage loop)
            // the tooltip is an override-redirect window with no event mask, so
            // the only events it can produce are Expose (it never takes input);
            // ignore them rather than letting them fall through to the bar
            if (tipWin() != None && ev.xany.window == tipWin()) continue;
            if (dashWin() != None && ev.xany.window == dashWin()) {
                if (dashEvent(&ev)) { dashClose(); paint(); }
                continue;
            }
            switch (ev.type) {
                case Expose: if (ev.xexpose.count == 0) onExpose(); break;
                case ButtonPress: onButton(&ev.xbutton); break;
                case ButtonRelease: onButtonRelease(&ev.xbutton); break;
                case MotionNotify: onMotion(&ev.xmotion); break;
                case LeaveNotify: onLeave(); break;
                case KeyPress: onKey(&ev.xkey); break;
                case ConfigureNotify:
                    // a foreign resize (an XWayland output-scale change or a
                    // WM re-layout) must go through the same helper so the
                    // surface AND the rounded shape follow the new size
                    resizeWindow(ev.xconfigure.width, ev.xconfigure.height);
                    paint();
                    break;
                default: break;
            }
        }
        if (g_menuPending) {
            g_menuPending = 0;
            Rgb bgc = hexRgb(cbCfgTint(), (Rgb){0.91, 0.85, 0.76});
            Rgb fgc = hexRgb(cbCfgFg(), (Rgb){0.03, 0.03, 0.03});
            Rgb divc = hexRgb(cbCfgDivider(), (Rgb){0.85, 0.80, 0.70});
            Rgb accc = hexRgb(cbCfgPinkDeep(), (Rgb){0.83, 0.58, 0.67});
            double bg3[3] = { bgc.r, bgc.g, bgc.b };
            double fg3[3] = { fgc.r, fgc.g, fgc.b };
            double dv3[3] = { divc.r, divc.g, divc.b };
            double ac3[3] = { accc.r, accc.g, accc.b };
            // no dim colour: the menu has no dim text (menu.h). The autostart
            // mark is read per open (not per paint): a subprocess per hover move
            // would be absurd, and the state cannot change while the menu is up.
            int startChecked = autoStartEnabled() == 1;
            int choice = menuRun(g_dpy, g_screen, g_visual, g_cmap, 32,
                                 g_menuX, g_menuY, g_scale, (int)fontSizePx(),
                                 g_fontFamily, bg3, 0.97, fg3, dv3, ac3,
                                 startChecked);
            if (choice == MENU_DISMISSED_BY_PRESS) {
                // the button that closed the menu is still held: its release
                // would bounce the menu back open, so the bar eats it
                g_swallowRelease = 1;
            } else {
                handleMenuChoice(choice);
            }
            if (g_petPipe >= 0) clock_gettime(CLOCK_MONOTONIC, &g_petPipeAt);
        }
        if (g_dashPending) {
            g_dashPending = 0;
            runDashboard();
        }
        petHandshakeDrain();
        reapChildren();
        time_t now = time(NULL);
        geomPump(now);
        if (sel == 0 && now != lastPaint) {
            lastPaint = now;
            // hot-reload the config when the file changes, then re-derive
            // everything. The token is the file's full identity, so a config
            // that is rewritten in the same second, or one that is rewritten
            // WHILE the bar is reading it, is still seen as a change.
            if (now - lastCfgCheck >= 2) {
                lastCfgCheck = now;
                CfgId cur;
                if (cfgId(&cur) && !cfgIdSame(&cur, &g_cfgId)) configReload();
            }
            linuxMetricsPoll(&g_m);
            scanIfDue(now);
            buildChips();
            resizeToContent();
            // the position is derived every tick, in every align mode: x and
            // width come from the terminal when it reports (and stay put when
            // it does not), y always comes from barYFor - the terminal's top
            // edge, or the screen top when align 1 has no usable report. The
            // width is resizeToContent()'s decision, made just above from the
            // same applyAlign() numbers: it is the ONE owner of the width.
            {
                int aw = 0, ax = 0;
                int aligned = applyAlign(&aw, &ax);
                int wantY = barYFor();
                if (wantY < 0) {
                    // no room above the terminal: Windows hides the bar
                    // (p_ui.c checks y < rcWork.top), and so does the port
                    if (!g_hidden) { g_hidden = 1; XUnmapWindow(g_dpy, g_win); }
                } else {
                    // position (and the size hints) BEFORE the map: a window
                    // manager places a managed window from the hints it reads on
                    // map, so mapping first shows it at the old position for a
                    // frame
                    int wantX = aligned ? ax : g_x;
                    if (wantX != g_x || wantY != g_y) moveWindow(wantX, wantY);
                    if (g_hidden) { g_hidden = 0; XMapWindow(g_dpy, g_win); afterMap(); }
                }
            }
            paint();
        }
    }
    tipFree();
    scriptLockRelease();
    geomBusClose(&g_bus);
    if (g_region) XFixesDestroyRegion(g_dpy, g_region);
    if (g_pic) XRenderFreePicture(g_dpy, g_pic);
    if (g_cr) cairo_destroy(g_cr);
    if (g_surf) cairo_surface_destroy(g_surf);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    return 0;
}
