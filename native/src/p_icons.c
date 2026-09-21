// ------------------------------------------------------------ svg icons ----
// Chip icons: ONE stroke color each (theme.iconColor, default pinkDeep),
// flattened from 24-unit SVG paths once at startup, then rendered with GDI+
// (SmoothingModeAntiAlias8x8, round caps/joins) into per-icon premultiplied
// ARGB caches and AlphaBlended onto the bar. GDI Polyline stroking (the old
// renderer) has no anti-aliasing: circles came out lumpy and diagonals
// jagged - GDI+ is what makes the icons read smooth at 24 physical px.
//
// theme.iconOpacity is applied as the AlphaBlend source-constant-alpha (no
// per-pixel fade pass needed anymore). If gdiplus.dll fails to load the code
// falls back to the old GDI Polyline stroke.
//
// The battery is DYNAMIC: its inner fill tracks the current charge (icon
// color, warn red under 10%, full + zigzag bolt on AC). Drawn by
// svgDrawBatt, not from the static table.
//
// Path primitives supported by the flattener: M m L l H h V v A a C c S s Z z
// (+ implicit lineto after M/m). Arcs sample every 5 degrees so circles
// stay round at 2x DPI; C/c/S/s cubics are sampled too (the bow's loops).
//
// The facade below (gdip*) is also used by the dashboard painter for the
// donut pies and anti-aliased rounded cards.

#include <math.h>

extern double g_iconOpacity; // theme.iconOpacity/100, defined in chocobar.c
// icon ids (order = table below)
enum {
    SVG_BOW, SVG_DIAMOND, SVG_GAUGE, SVG_BOLT, SVG_CLOCK,
    SVG_GPU, SVG_CPU, SVG_TEMP, SVG_RAM, SVG_VOL, SVG_MUTE,
    SVG_BAT, SVG_COUNT
};

#define SVG_MAXSUB 12
#define SVG_MAXPTS 512
typedef struct { float x, y; } SvgPt;
typedef struct { SvgPt p[SVG_MAXPTS]; int n; int closed; } SvgSub;
typedef struct { SvgSub s[SVG_MAXSUB]; int nsub; } SvgPath;

// ---- flattened-path builder -------------------------------------------------
typedef struct {
    SvgPath *out;
    float cx, cy;        // current point
    float sx, sy;        // current subpath start
    float qx, qy;        // previous cubic control point (for S reflection)
    wchar_t prev;        // previous command letter (for S reflection)
    int open;            // current subpath index, -1 = none
} SvgCtx;

static void svgSubStart(SvgCtx *k, float x, float y) {
    if (k->out->nsub >= SVG_MAXSUB) return;
    SvgSub *s = &k->out->s[k->out->nsub];
    s->n = 0; s->closed = 0;
    k->open = k->out->nsub++;
    k->cx = k->sx = x; k->cy = k->sy = y;
    // the M point is a real vertex: closed strokes need it as p[0]
    s->p[0].x = x; s->p[0].y = y; s->n = 1;
}
static void svgPt(SvgCtx *k, float x, float y) {
    if (k->open < 0) { svgSubStart(k, x, y); return; }
    SvgSub *s = &k->out->s[k->open];
    if (s->n >= SVG_MAXPTS) return;
    s->p[s->n].x = x; s->p[s->n].y = y; s->n++;
    k->cx = x; k->cy = y;
}
static void svgLine(SvgCtx *k, float x, float y) { svgPt(k, x, y); }

// cubic bezier, sampled (the same 5-degree-ish step density as arcs)
static void svgCubic(SvgCtx *k, float x1, float y1, float x2, float y2, float x, float y) {
    float x0 = k->cx, y0 = k->cy;
    float dx = x - x0, dy = y - y0;
    float d1 = (float)fabs(x1 - x0) + (float)fabs(y1 - y0);
    float d2 = (float)fabs(x2 - x1) + (float)fabs(y2 - y1);
    float d3 = (float)fabs(x - x2) + (float)fabs(y - y2);
    float len = (float)fabs(dx) + (float)fabs(dy) + d1 + d2 + d3;
    // dense sampling: small icon loops (the bow) read polygonal at one point
    // per 6 units - 1.5 keeps the curves round at 12 CSS px
    int steps = (int)(len / 1.5f) + 10;
    if (steps > 160) steps = 160;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps, u = 1 - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        svgPt(k, a * x0 + b * x1 + c * x2 + d * x, a * y0 + b * y1 + c * y2 + d * y);
    }
    k->qx = x2; k->qy = y2;
    k->prev = L'C';
}

// SVG arc endpoint parameterization (spec F.6), sampled from the start angle
static void svgArc(SvgCtx *k, float rx, float ry, float phiDeg, int fa, int fs, float x2, float y2) {
    float x1 = k->cx, y1 = k->cy;
    if (rx == 0 || ry == 0) { svgLine(k, x2, y2); return; }
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    float phi = phiDeg * (3.14159265f / 180), cp = (float)cos(phi), sp = (float)sin(phi);
    float dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    float x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { float s2 = (float)sqrt(lam); rx *= s2; ry *= s2; }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float c = (den == 0) ? 0 : (float)sqrt(num / den);
    if (fa == fs) c = -c;
    float cxp = c * rx * y1p / ry, cyp = -c * ry * x1p / rx;
    float ccx = cp * cxp - sp * cyp + (x1 + x2) / 2;
    float ccy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    float dot = ux * vx + uy * vy;
    float nl = (float)sqrt(ux * ux + uy * uy) * (float)sqrt(vx * vx + vy * vy);
    float a = (nl == 0) ? 0 : (float)acos(dot / nl);
    if (ux * vy - uy * vx < 0) a = -a;
    if (!fs && a > 0) a -= 6.2831853f;
    if (fs && a < 0) a += 6.2831853f;
    float th1 = (float)atan2((y1 - ccy) / ry, (x1 - ccx) / rx); // start angle
    int steps = (int)(fabs(a) / (3.14159265f / 36)) + 2;        // every 5 degrees
    if (steps > 96) steps = 96;
    for (int i = 1; i <= steps; i++) {
        float t = th1 + a * (float)i / steps;
        float px = cp * (rx * (float)cos(t)) - sp * (ry * (float)sin(t)) + ccx;
        float py = sp * (rx * (float)cos(t)) + cp * (ry * (float)sin(t)) + ccy;
        svgPt(k, px, py);
    }
    k->cx = x2; k->cy = y2;
}

// ---- number scanner ----------------------------------------------------------
static int svgNum(const wchar_t *d, int *pi, float *out) {
    int i = *pi;
    while (d[i] == L' ' || d[i] == L',') i++;
    int neg = 0;
    if (d[i] == L'+') i++;
    else if (d[i] == L'-') { neg = 1; i++; }
    int any = 0;
    double v = 0;
    while (d[i] >= L'0' && d[i] <= L'9') { v = v * 10 + (d[i] - L'0'); i++; any = 1; }
    if (d[i] == L'.') {
        i++; any = 1;
        double f = 0.1;
        while (d[i] >= L'0' && d[i] <= L'9') { v += (d[i] - L'0') * f; f *= 0.1; i++; }
    }
    if (!any) return 0;
    *pi = i;
    *out = neg ? (float)-v : (float)v;
    return 1;
}

// ---- path walker -------------------------------------------------------------
static void svgWalk(SvgPath *out, const wchar_t *d) {
    SvgCtx k; memset(&k, 0, sizeof(k));
    k.out = out; k.open = -1;
    out->nsub = 0;
    int i = 0;
    wchar_t cmd = 0;
    while (d[i]) {
        while (d[i] == L' ') i++;
        wchar_t c = d[i];
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) {
            cmd = c; i++;
            if (cmd == L'Z' || cmd == L'z') {
                if (k.open >= 0) {
                    SvgSub *s = &out->s[k.open];
                    if (s->n < SVG_MAXPTS) { s->p[s->n] = s->p[0]; s->n++; }
                    s->closed = 1;
                }
                k.open = -1;
                k.cx = k.sx; k.cy = k.sy;
                continue;
            }
        }
        int rel = (cmd >= L'a');
        switch (cmd) {
        case L'M': case L'm': {
            float x, y;
            if (!svgNum(d, &i, &x)) break;
            if (!svgNum(d, &i, &y)) break;
            if (rel) { x += k.cx; y += k.cy; }
            svgSubStart(&k, x, y);
            cmd = rel ? L'l' : L'L'; // implicit lineto continuation
            break;
        }
        case L'L': case L'l': {
            float x, y;
            if (!svgNum(d, &i, &x)) break;
            if (!svgNum(d, &i, &y)) break;
            if (rel) { x += k.cx; y += k.cy; }
            svgLine(&k, x, y);
            break;
        }
        case L'H': case L'h': {
            float x;
            if (!svgNum(d, &i, &x)) break;
            if (rel) x += k.cx;
            svgLine(&k, x, k.cy);
            break;
        }
        case L'V': case L'v': {
            float y;
            if (!svgNum(d, &i, &y)) break;
            if (rel) y += k.cy;
            svgLine(&k, k.cx, y);
            break;
        }
        case L'A': case L'a': {
            float v[7];
            int ok = 1;
            for (int j = 0; j < 7; j++) {
                if (!svgNum(d, &i, &v[j])) { ok = 0; break; }
                if (rel && (j == 5 || j == 6)) { v[j] += (j == 5) ? k.cx : k.cy; }
            }
            if (ok) svgArc(&k, v[0], v[1], v[2], (int)v[3], (int)v[4], v[5], v[6]);
            k.prev = L'A';
            break;
        }
        case L'C': case L'c': {
            float v[6];
            int ok = 1;
            for (int j = 0; j < 6; j++) {
                if (!svgNum(d, &i, &v[j])) { ok = 0; break; }
                if (rel) v[j] += (j % 2 == 0) ? k.cx : k.cy;
            }
            if (ok) svgCubic(&k, v[0], v[1], v[2], v[3], v[4], v[5]);
            k.prev = L'C';
            break;
        }
        case L'S': case L's': {
            // smooth cubic: first control reflects the previous command's
            float c1x = k.cx, c1y = k.cy;
            if (k.prev == L'C' || k.prev == L'S') { c1x = 2 * k.cx - k.qx; c1y = 2 * k.cy - k.qy; }
            float v[4];
            int ok = 1;
            for (int j = 0; j < 4; j++) {
                if (!svgNum(d, &i, &v[j])) { ok = 0; break; }
                if (rel) v[j] += (j % 2 == 0) ? k.cx : k.cy;
            }
            if (ok) svgCubic(&k, c1x, c1y, v[0], v[1], v[2], v[3]);
            k.prev = L'S';
            break;
        }
        default:
            i++; // skip unknown
            break;
        }
    }
}

// ---- icon table --------------------------------------------------------------
// Each icon = up to 3 stroke paths in the ONE icon color. wmul scales the pen
// width for accents (the gauge ring reads better slightly thicker).
typedef struct { const wchar_t *d; float w; } SvgPart;
static const SvgPart SVG_P[][3] = {
    // bow (pet): two loops per side around a small knot (Electron ICONS.bow,
    // stroke-width 2). Rect/circle converted to path syntax; S = smooth cubic.
    { { L"M11 11C8.5 7.5 5.5 6.5 4.5 8S5 12 11 11"
        L"M13 11c2.5-3.5 5.5-4.5 6.5-3s-.5 4-6.5 3"
        L"M11 13c-2.5 3.5-5.5 4.5-6.5 3s.5-4 6.5-3"
        L"M13 13c2.5 3.5 5.5 4.5 6.5 3s-.5-4-6.5-3"
        L"M10.9 12a1.1 1.1 0 1 0 2.2 0a1.1 1.1 0 1 0-2.2 0Z", 0.9090909f } },
    // diamond (tokens): plain rhombus (Electron ICONS.diamond)
    { { L"M12 3L21 12L12 21L3 12Z", 1.0f } },
    // gauge (subs): ring + needle (Electron ICONS.gauge)
    { { L"M4 18A8 8 0 1 1 20 18", 1.0f }, { L"M12 14L16 10", 1.0f } },
    // bolt (shortcut)
    { { L"M13 2L4.5 13.5H11L9.5 22L19 10H12.5L13 2Z", 1.0f } },
    // clock (time): ring + hands
    { { L"M3.5 12A8.5 8.5 0 1 0 20.5 12A8.5 8.5 0 1 0 3.5 12Z", 1.0f }, { L"M12 7.5V12L15 14", 1.0f } },
    // gpu: card + fan + two vents + pins (Electron ICONS.gpu)
    { { L"M4.5 7H17.5A1.5 1.5 0 0 1 19 8.5V15.5A1.5 1.5 0 0 1 17.5 17H4.5A1.5 1.5 0 0 1 3 15.5V8.5A1.5 1.5 0 0 1 4.5 7Z"
        L"M6.6 12A2.4 2.4 0 1 0 11.4 12A2.4 2.4 0 1 0 6.6 12Z"
        L"M14 9.5V14.5M17 9.5V14.5M19 10V14M6 17V20M10 17V20", 1.0f } },
    // cpu: body + pins 2 per side (Electron ICONS.cpu)
    { { L"M7.5 6H16.5A1.5 1.5 0 0 1 18 7.5V16.5A1.5 1.5 0 0 1 16.5 18H7.5A1.5 1.5 0 0 1 6 16.5V7.5A1.5 1.5 0 0 1 7.5 6Z"
        L"M9 2V5M15 2V5M9 19V22M15 19V22M2 9H5M2 15H5M19 9H22M19 15H22", 1.0f } },
    // cpu temp: thermometer + stem (Electron ICONS.temp)
    { { L"M10 4A2 2 0 1 1 14 4V13.3A4.5 4.5 0 1 1 10 13.3Z", 1.0f },
      { L"M12 9.5V16", 1.0f } },
    // ram: stick + three chips + pins (Electron ICONS.ram)
    { { L"M4.5 8H19.5A1.5 1.5 0 0 1 21 9.5V15.5A1.5 1.5 0 0 1 19.5 17H4.5A1.5 1.5 0 0 1 3 15.5V9.5A1.5 1.5 0 0 1 4.5 8Z"
        L"M7 17V20M12 17V20M17 17V20M7 11V14M11 11V14M15 11V14", 1.0f } },
    // volume: speaker + waves (Electron ICONS.vol)
    { { L"M11 5L6.5 9H3V15H6.5L11 19Z", 1.0f },
      { L"M15.5 9.5A4 4 0 0 1 15.5 14.5M18 7A7.5 7.5 0 0 1 18 17", 1.0f } },
    // muted: speaker + X (Electron ICONS.mute)
    { { L"M11 5L6.5 9H3V15H6.5L11 19Z", 1.0f },
      { L"M16 9.5L21 14.5M21 9.5L16 14.5", 1.0f } },
    // battery: body + nub only; the fill is dynamic (svgDrawBatt, matches
    // Electron ICONS.batBody: innerX 4.3, innerW 12.4, y 9.7, h 5.1, rx 0.8)
    { { L"M4 8H17A1.5 1.5 0 0 1 18.5 9.5V15A1.5 1.5 0 0 1 17 16.5H4A1.5 1.5 0 0 1 2.5 15V9.5A1.5 1.5 0 0 1 4 8Z"
        L"M21.5 11V13.5", 1.0f } },
};

typedef struct { SvgPath path; float w; } SvgFlat;
#define SVG_MAXPARTS 3
static SvgFlat g_flat[SVG_COUNT][SVG_MAXPARTS];
static int g_flatN[SVG_COUNT];

// ---- GDI+ flat API (loaded once, optional) -----------------------------------
typedef int GpStatus;
typedef void GpGraphics;
typedef void GpPath;
typedef void GpPen;
typedef void GpBrush;
typedef struct { float X, Y; } GpPointF;
typedef struct { unsigned int GdiplusVersion; void *DebugEventCallback; int SuppressBackgroundThread; int SuppressExternalCodecs; } GdiplusStartupInput;

static HMODULE g_gdipMod = NULL;
static int g_gdipOk = 0;
static GpStatus (WINAPI *t_GdiplusStartup)(void **, GdiplusStartupInput *, void *);
static void (WINAPI *t_GdipDeleteGraphics)(GpGraphics *);
static GpStatus (WINAPI *t_GdipCreateFromHDC)(HDC, GpGraphics **);
static GpStatus (WINAPI *t_GdipSetSmoothingMode)(GpGraphics *, int);
static GpStatus (WINAPI *t_GdipCreatePath)(int, GpPath **);
static GpStatus (WINAPI *t_GdipDeletePath)(GpPath *);
static GpStatus (WINAPI *t_GdipStartPathFigure)(GpPath *);
static GpStatus (WINAPI *t_GdipAddPathLine2)(GpPath *, GpPointF *, int);
static GpStatus (WINAPI *t_GdipClosePathFigure)(GpPath *);
static GpStatus (WINAPI *t_GdipCreatePen1)(unsigned int argb, float width, int unit, GpPen **);
static GpStatus (WINAPI *t_GdipDeletePen)(GpPen *);
static GpStatus (WINAPI *t_GdipSetPenStartCap)(GpPen *, int);
static GpStatus (WINAPI *t_GdipSetPenEndCap)(GpPen *, int);
static GpStatus (WINAPI *t_GdipSetPenLineJoin)(GpPen *, int);
static GpStatus (WINAPI *t_GdipDrawPath)(GpGraphics *, GpPen *, GpPath *);
static GpStatus (WINAPI *t_GdipDrawArc)(GpGraphics *, GpPen *, float, float, float, float, float, float);
static GpStatus (WINAPI *t_GdipDrawEllipse)(GpGraphics *, GpPen *, float, float, float, float);
static GpStatus (WINAPI *t_GdipFillEllipse)(GpGraphics *, GpBrush *, float, float, float, float);
static GpStatus (WINAPI *t_GdipFillRectangleI)(GpGraphics *, GpBrush *, int, int, int, int);
static GpStatus (WINAPI *t_GdipFillPath)(GpGraphics *, GpBrush *, GpPath *);
static GpStatus (WINAPI *t_GdipAddPathArc)(GpPath *, float, float, float, float, float, float);
static GpStatus (WINAPI *t_GdipCloseFigure)(GpPath *);
static GpStatus (WINAPI *t_GdipCreateSolidFill)(unsigned int argb, GpBrush **);
static GpStatus (WINAPI *t_GdipDeleteBrush)(GpBrush *);

static void gdipInit(void) {
    if (g_gdipMod) return;
    g_gdipMod = LoadLibraryW(L"gdiplus.dll");
    if (!g_gdipMod) return;
#define GBIND(f, name) t_##f = (void *)GetProcAddress(g_gdipMod, name)
    GBIND(GdiplusStartup, "GdiplusStartup");
    GBIND(GdipDeleteGraphics, "GdipDeleteGraphics");
    GBIND(GdipCreateFromHDC, "GdipCreateFromHDC");
    GBIND(GdipSetSmoothingMode, "GdipSetSmoothingMode");
    GBIND(GdipCreatePath, "GdipCreatePath");
    GBIND(GdipDeletePath, "GdipDeletePath");
    GBIND(GdipStartPathFigure, "GdipStartPathFigure");
    GBIND(GdipAddPathLine2, "GdipAddPathLine2");
    GBIND(GdipClosePathFigure, "GdipClosePathFigure");
    GBIND(GdipCreatePen1, "GdipCreatePen1");
    GBIND(GdipDeletePen, "GdipDeletePen");
    GBIND(GdipSetPenStartCap, "GdipSetPenStartCap");
    GBIND(GdipSetPenEndCap, "GdipSetPenEndCap");
    GBIND(GdipSetPenLineJoin, "GdipSetPenLineJoin");
    GBIND(GdipDrawPath, "GdipDrawPath");
    GBIND(GdipDrawArc, "GdipDrawArc");
    GBIND(GdipDrawEllipse, "GdipDrawEllipse");
    GBIND(GdipFillEllipse, "GdipFillEllipse");
    GBIND(GdipFillRectangleI, "GdipFillRectangleI");
    GBIND(GdipFillPath, "GdipFillPath");
    GBIND(GdipAddPathArc, "GdipAddPathArc");
    GBIND(GdipCloseFigure, "GdipClosePathFigure"); // flat API has no GdipCloseFigure
    GBIND(GdipCreateSolidFill, "GdipCreateSolidFill");
    GBIND(GdipDeleteBrush, "GdipDeleteBrush");
#undef GBIND
    // every binding must resolve: a single NULL would crash the first paint
    void *tabs[] = { (void *)t_GdiplusStartup, (void *)t_GdipDeleteGraphics,
        (void *)t_GdipCreateFromHDC, (void *)t_GdipSetSmoothingMode, (void *)t_GdipCreatePath,
        (void *)t_GdipDeletePath, (void *)t_GdipStartPathFigure, (void *)t_GdipAddPathLine2,
        (void *)t_GdipClosePathFigure, (void *)t_GdipCreatePen1, (void *)t_GdipDeletePen,
        (void *)t_GdipSetPenStartCap, (void *)t_GdipSetPenEndCap, (void *)t_GdipSetPenLineJoin,
        (void *)t_GdipDrawPath, (void *)t_GdipDrawArc, (void *)t_GdipDrawEllipse,
        (void *)t_GdipFillEllipse, (void *)t_GdipFillRectangleI, (void *)t_GdipFillPath, (void *)t_GdipAddPathArc,
        (void *)t_GdipCloseFigure, (void *)t_GdipCreateSolidFill, (void *)t_GdipDeleteBrush };
    for (int i = 0; i < (int)(sizeof(tabs) / sizeof(tabs[0])); i++) {
        if (!tabs[i]) { writeLogA("gdiplus bind missing, falling back to GDI"); return; }
    }
    GdiplusStartupInput in;
    in.GdiplusVersion = 1;
    in.DebugEventCallback = NULL;
    in.SuppressBackgroundThread = 0;
    in.SuppressExternalCodecs = 0;
    void *tok = NULL;
    if (t_GdiplusStartup(&tok, &in, NULL) == 0) g_gdipOk = 1;
}

#define GDIP_ARGB(cr) (0xFF000000u | ((DWORD)GetRValue(cr) << 16) | ((DWORD)GetGValue(cr) << 8) | (DWORD)GetBValue(cr))

// anti-aliased round-rect fill (+optional 1px border) on an opaque surface
static void gdipRoundRect(HDC hdc, COLORREF fill, COLORREF border, int x, int y, int w, int h, int r) {
    if (!g_gdipOk || w < 2 || h < 2) return;
    GpGraphics *g = NULL;
    if (t_GdipCreateFromHDC(hdc, &g) != 0) return;
    t_GdipSetSmoothingMode(g, 6); // AntiAlias8x8
    GpPath *p = NULL;
    if (t_GdipCreatePath(0, &p) == 0) {
        float fr = (float)r;
        if (fr * 2 > w) fr = w / 2.0f;
        if (fr * 2 > h) fr = h / 2.0f;
        t_GdipAddPathArc(p, (float)x, (float)y, 2 * fr, 2 * fr, 180, 90);
        t_GdipAddPathArc(p, (float)x + w - 2 * fr, (float)y, 2 * fr, 2 * fr, 270, 90);
        t_GdipAddPathArc(p, (float)x + w - 2 * fr, (float)y + h - 2 * fr, 2 * fr, 2 * fr, 0, 90);
        t_GdipAddPathArc(p, (float)x, (float)y + h - 2 * fr, 2 * fr, 2 * fr, 90, 90);
        t_GdipCloseFigure(p);
        if (fill) {
            GpBrush *br = NULL;
            if (t_GdipCreateSolidFill(GDIP_ARGB(fill), &br) == 0) {
                t_GdipFillPath(g, br, p);
                t_GdipDeleteBrush(br);
            }
        }
        if (border) {
            GpPen *pen = NULL;
            if (t_GdipCreatePen1(GDIP_ARGB(border), 1.0f, 2 /*pixel*/, &pen) == 0) {
                t_GdipDrawPath(g, pen, p);
                t_GdipDeletePen(pen);
            }
        }
        t_GdipDeletePath(p);
    }
    t_GdipDeleteGraphics(g);
}

// anti-aliased donut arc (subs pies): elliptical arc with a round-cap pen
static void gdipArcStroke(HDC hdc, COLORREF cr, float width,
                          float x, float y, float w, float h, float startDeg, float sweepDeg) {
    if (!g_gdipOk || sweepDeg <= 0) return;
    GpGraphics *g = NULL;
    if (t_GdipCreateFromHDC(hdc, &g) != 0) return;
    t_GdipSetSmoothingMode(g, 6);
    GpPen *pen = NULL;
    if (t_GdipCreatePen1(GDIP_ARGB(cr), width, 2 /*pixel*/, &pen) == 0) {
        t_GdipSetPenStartCap(pen, 2); // round
        t_GdipSetPenEndCap(pen, 2);
        t_GdipDrawArc(g, pen, x, y, w, h, startDeg, sweepDeg);
        t_GdipDeletePen(pen);
    }
    t_GdipDeleteGraphics(g);
}

// ---- icon raster cache --------------------------------------------------------
// One premultiplied ARGB DIB per icon id, redrawn only when the color (or the
// battery's charge bucket) changes. Painted to the bar with AlphaBlend so
// theme.iconOpacity rides the SourceConstantAlpha for free.
typedef struct {
    HBITMAP bm; void *bits; HDC dc;
    int w, h;
    COLORREF color; int valid;
} IconDib;
static IconDib g_iconDib[SVG_COUNT];
static IconDib g_battDib;         // battery cache (charge bucket + ac + colors)
static IconDib g_ssDib;           // 2x supersample scratch (AA quality)
static int g_battKey[7];          // pct bucket, ac, accent, warn, line, valid, warn-fill

static void iconDrop(IconDib *d) {
    if (d->dc) { DeleteDC(d->dc); d->dc = NULL; }
    if (d->bm) { DeleteObject(d->bm); d->bm = NULL; }
    d->bits = NULL; d->valid = 0;
}

static int iconDibMake(IconDib *d, int w, int h) {
    if (d->bm && d->w == w && d->h == h) return 1;
    iconDrop(d);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    d->dc = CreateCompatibleDC(NULL);
    d->bm = CreateDIBSection(d->dc, &bi, DIB_RGB_COLORS, &d->bits, NULL, 0);
    if (!d->bm || !d->bits) { iconDrop(d); return 0; }
    SelectObject(d->dc, d->bm);
    d->w = w; d->h = h;
    return 1;
}

static void iconDibPremultiply(IconDib *d);

// render one flattened path set into an icon DIB with GDI+ AA strokes
// Draw the icon into `g` at supersample factor ss (path units -> pixels).
static void iconStrokeAll(GpGraphics *g, int id, COLORREF color, float s, float penW) {
    unsigned argb = GDIP_ARGB(color);
    for (int j = 0; j < g_flatN[id]; j++) {
        SvgPath *sp = &g_flat[id][j].path;
        GpPath *gp = NULL;
        if (t_GdipCreatePath(0, &gp) != 0) continue;
        for (int si = 0; si < sp->nsub; si++) {
            SvgSub *ss = &sp->s[si];
            if (ss->n < 2) continue;
            if (si > 0) t_GdipStartPathFigure(gp);
            GpPointF pts[SVG_MAXPTS];
            int n = 0;
            for (int i = 0; i < ss->n; i++) {
                pts[n].X = ss->p[i].x * s;
                pts[n].Y = ss->p[i].y * s;
                n++;
            }
            t_GdipAddPathLine2(gp, pts, n);
            if (ss->closed) t_GdipClosePathFigure(gp);
        }
        GpPen *pen = NULL;
        if (t_GdipCreatePen1(argb, (float)(penW * (g_flat[id][j].w ? g_flat[id][j].w : 1.0f)), 2 /*pixel*/, &pen) == 0) {
            t_GdipSetPenStartCap(pen, 2); // LineCapRound
            t_GdipSetPenEndCap(pen, 2);
            t_GdipSetPenLineJoin(pen, 2); // LineJoinRound
            t_GdipDrawPath(g, pen, gp);
            t_GdipDeletePen(pen);
        }
        t_GdipDeletePath(gp);
    }
}

static int iconRenderGdip(IconDib *d, int id, COLORREF color, int w, int h) {
    if (!iconDibMake(d, w, h)) return 0;
    memset(d->bits, 0, (size_t)w * h * 4);
    float s = (float)g_scale / 2; // 24-unit box -> 12 CSS px
    float wpen = 1.1f * (float)g_scale; // exact CSS stroke: 2.2 units / 24-unit viewBox
    // Supersample 2x: GDI+ edge AA at 1:1 reads blocky next to Chromium's
    // SVG renderer. One color per icon, so the downscale only averages the
    // coverage (alpha); the RGB stays the icon color.
    const int SS = 2;
    if (iconDibMake(&g_ssDib, w * SS, h * SS)) {
        memset(g_ssDib.bits, 0, (size_t)w * SS * h * SS * 4);
        GpGraphics *g = NULL;
        if (t_GdipCreateFromHDC(g_ssDib.dc, &g) == 0) {
            t_GdipSetSmoothingMode(g, 6); // AntiAlias8x8
            iconStrokeAll(g, id, color, s * SS, wpen * SS);
            t_GdipDeleteGraphics(g);
            unsigned *src = (unsigned *)g_ssDib.bits;
            unsigned *dst = (unsigned *)d->bits;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned a = 0;
                    for (int dy = 0; dy < SS; dy++)
                        for (int dx = 0; dx < SS; dx++)
                            a += (src[((y * SS + dy) * (w * SS)) + x * SS + dx] >> 24) & 0xFF;
                    a /= SS * SS;
                    dst[y * w + x] = (a << 24) | (GDIP_ARGB(color) & 0x00FFFFFFu);
                }
            }
            return 1;
        }
    }
    // fallback: draw 1:1 straight into the cache DIB
    GpGraphics *g = NULL;
    if (t_GdipCreateFromHDC(d->dc, &g) != 0) return 0;
    t_GdipSetSmoothingMode(g, 6); // AntiAlias8x8
    iconStrokeAll(g, id, color, s, wpen);
    t_GdipDeleteGraphics(g);
    return 1;
}

// GDI+ writes straight (non-premultiplied) ARGB: convert in place. Must run
// AFTER all drawing - a later GDI+ pass would treat the buffer as straight.
static void iconDibPremultiply(IconDib *d) {
    DWORD *pxb = (DWORD *)d->bits;
    int n = d->w * d->h;
    for (int i = 0; i < n; i++) {
        DWORD v = pxb[i];
        unsigned a = (v >> 24) & 255;
        if (a == 255) continue;
        if (a == 0) { pxb[i] = 0; continue; }
        DWORD out = a << 24;
        for (int sh = 0; sh < 24; sh += 8)
            out |= ((((v >> sh) & 255) * a + 127) / 255) << sh;
        pxb[i] = out;
    }
}

// legacy GDI Polyline stroke (fallback when gdiplus.dll is unavailable)
static void iconStrokeGdi(HDC hdc, int id, COLORREF color, int x, int y) {
    float s = (float)g_scale / 2;
    int wpen = (int)(1.1f * g_scale + 0.5);
    if (wpen < 1) wpen = 1;
    LOGBRUSH lb; lb.lbStyle = BS_SOLID; lb.lbColor = color; lb.lbHatch = 0;
    for (int j = 0; j < g_flatN[id]; j++) {
        int w = (int)(wpen * (g_flat[id][j].w ? g_flat[id][j].w : 1.0f) + 0.5);
        if (w < 1) w = 1;
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)w, &lb, 0, NULL);
        if (!pen) return;
        HGDIOBJ old = SelectObject(hdc, pen);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        SvgPath *p = &g_flat[id][j].path;
        for (int si = 0; si < p->nsub; si++) {
            SvgSub *ss = &p->s[si];
            if (ss->n < 2) continue;
            POINT pts[SVG_MAXPTS + 1];
            for (int i = 0; i < ss->n; i++) {
                pts[i].x = x + (int)(ss->p[i].x * s + 0.5f);
                pts[i].y = y + (int)(ss->p[i].y * s + 0.5f);
            }
            int n2 = ss->n;
            if (ss->closed && n2 < SVG_MAXPTS + 1) { pts[n2] = pts[0]; n2++; }
            Polyline(hdc, pts, n2);
        }
        SelectObject(hdc, old);
        SelectObject(hdc, ob);
        DeleteObject(pen);
    }
}

static void svgInitAll(void) {
    for (int i = 0; i < SVG_COUNT; i++) {
        int n = 0;
        for (int j = 0; j < SVG_MAXPARTS; j++) {
            if (!SVG_P[i][j].d) break;
            memset(&g_flat[i][j].path, 0, sizeof(SvgPath));
            svgWalk(&g_flat[i][j].path, SVG_P[i][j].d);
            g_flat[i][j].w = SVG_P[i][j].w;
            n++;
        }
        g_flatN[i] = n;
    }
    gdipInit();
}

// blit a cached icon DIB onto the target DC with the opacity alpha
static void iconBlit(HDC hdc, IconDib *d, int x, int y) {
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(g_iconOpacity * 255.0 + 0.5), AC_SRC_ALPHA };
    AlphaBlend(hdc, x, y, d->w, d->h, d->dc, 0, 0, d->w, d->h, bf);
}

// stroke one icon in `color` into the premultiplied DIB at (x, ytop), 12*scale box
static void svgDraw(HDC hdc, int id, COLORREF color, int x, int y) {
    if (id < 0 || id >= SVG_COUNT || id == SVG_BAT) return;
    int bw = (int)(12 * g_scale + 0.5);
    if (g_gdipOk) {
        IconDib *d = &g_iconDib[id];
        if (!d->valid || d->color != color || d->w != bw) {
            if (!iconRenderGdip(d, id, color, bw, bw)) return;
            iconDibPremultiply(d); // once: render pass wrote straight ARGB
            d->color = color;
            d->valid = 1;
        }
        iconBlit(hdc, d, x, y);
    } else {
        iconStrokeGdi(hdc, id, color, x, y);
    }
}

// battery with a charge-level fill: the fill width tracks pct and turns
// warn red under 10% exactly as Electron ICONS.batBody does (charging only
// adds the zigzag bolt, it never widens the fill). Body outline comes from
// the static SVG_BAT part.
static void svgDrawBatt(HDC hdc, int pct, int ac, COLORREF accent, COLORREF warn,
                        COLORREF lineCr, int x, int y) {
    int bw = (int)(12 * g_scale + 0.5);
    int bucket = (pct < 0 ? 0 : (pct > 100 ? 100 : pct)) / 2;
    // the fill color switches on the raw pct (not the 2% bucket): key on the
    // warn fact too or 10/11% share bucket 5 and the color goes stale
    int key[7] = { bucket, ac ? 1 : 0, (int)accent, (int)warn, (int)lineCr, 1,
                   (pct < 10) ? 1 : 0 };
    if (g_gdipOk) {
        if (!g_battKey[5] || memcmp(g_battKey, key, sizeof(key)) != 0 || g_battDib.w != bw) {
            if (iconRenderGdip(&g_battDib, SVG_BAT, accent, bw, bw)) {
                float s = (float)g_scale / 2;
                // inner fill: Electron ICONS.batBody - innerX 4.3, innerW 12.4,
                // y 9.7, h 5.1, rx 0.8, opacity 0.75 (0.9 under 10%), 4% floor
                GpGraphics *g = NULL;
                if (t_GdipCreateFromHDC(g_battDib.dc, &g) == 0) {
                    t_GdipSetSmoothingMode(g, 6);
                    float fw = pct <= 0 ? 0.0f : 12.4f * (float)(bucket * 2) / 100.0f;
                    if (fw < 0.9f) fw = 0.9f; // never zero-width sliver floor
                    int low = (pct < 10);
                    COLORREF fc = low ? warn : accent;
                    unsigned int alpha = low ? 230u : 191u; // 0.9 / 0.75
                    GpPath *fp = NULL;
                    if (t_GdipCreatePath(0, &fp) == 0) {
                        float fx = 4.3f * s, fy = 9.7f * s;
                        float fw2 = fw * s, fh = 5.1f * s, fr = 0.8f * s;
                        if (fr * 2 > fw2) fr = fw2 / 2;
                        if (fr * 2 > fh) fr = fh / 2;
                        t_GdipAddPathArc(fp, fx, fy, 2 * fr, 2 * fr, 180, 90);
                        t_GdipAddPathArc(fp, fx + fw2 - 2 * fr, fy, 2 * fr, 2 * fr, 270, 90);
                        t_GdipAddPathArc(fp, fx + fw2 - 2 * fr, fy + fh - 2 * fr, 2 * fr, 2 * fr, 0, 90);
                        t_GdipAddPathArc(fp, fx, fy + fh - 2 * fr, 2 * fr, 2 * fr, 90, 90);
                        t_GdipCloseFigure(fp);
                        GpBrush *br = NULL;
                        unsigned int argb = alpha << 24 | ((unsigned int)GetRValue(fc) << 16)
                                         | ((unsigned int)GetGValue(fc) << 8) | (unsigned int)GetBValue(fc);
                        if (t_GdipCreateSolidFill(argb, &br) == 0) {
                            t_GdipFillPath(g, br, fp);
                            t_GdipDeleteBrush(br);
                        }
                        t_GdipDeletePath(fp);
                    }
                    if (ac) { // zigzag bolt over the fill (Electron batCharge path, stroke 1.8)
                        GpPen *pen = NULL;
                        float boltW = (float)(1.8 / 2.2) * 1.1f * g_scale;
                        if (t_GdipCreatePen1(GDIP_ARGB(accent), boltW, 2, &pen) == 0) {
                            t_GdipSetPenLineJoin(pen, 2);
                            GpPath *p2 = NULL;
                            if (t_GdipCreatePath(0, &p2) == 0) {
                                GpPointF zz[4] = {
                                    { 12.2f * s, 8.9f * s }, { 9.6f * s, 12.4f * s },
                                    { 12.0f * s, 12.4f * s }, { 9.9f * s, 15.7f * s }
                                };
                                t_GdipAddPathLine2(p2, zz, 4);
                                t_GdipDrawPath(g, pen, p2);
                                t_GdipDeletePath(p2);
                            }
                            t_GdipDeletePen(pen);
                        }
                    }
                    t_GdipDeleteGraphics(g);
                }
                iconDibPremultiply(&g_battDib); // once, after fill + zigzag
                memcpy(g_battKey, key, sizeof(key));
            }
        }
        iconBlit(hdc, &g_battDib, x, y);
        return;
    }
    // GDI fallback: outline + plain rect fill (same inner box as the GDI+ path)
    float s = (float)g_scale / 2;
    int ix0 = x + (int)(4.3f * s + 0.5f), iy0 = y + (int)(9.7f * s + 0.5f);
    int iy1 = y + (int)(14.8f * s + 0.5f);
    float fw = pct <= 0 ? 0.0f : 12.4f * (float)pct / 100.0f;
    if (fw < 0.9f) fw = 0.9f;
    int ix1 = ix0 + (int)(fw * s);
    if (ix1 > ix0) {
        HBRUSH br = CreateSolidBrush(pct < 10 ? warn : accent);
        RECT fr = { ix0, iy0, ix1, iy1 };
        FillRect(hdc, &fr, br);
        DeleteObject(br);
    }
    if (ac) {
        LOGBRUSH lb; lb.lbStyle = BS_SOLID; lb.lbColor = accent; lb.lbHatch = 0;
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                (DWORD)((1.8 / 2.2) * 1.1 * g_scale + 0.5), &lb, 0, NULL);
        if (pen) {
            HGDIOBJ old = SelectObject(hdc, pen);
            POINT zz[4] = {
                { x + (int)(12.2f * s), y + (int)(8.9f * s) },
                { x + (int)(9.6f * s), y + (int)(12.4f * s) },
                { x + (int)(12.0f * s), y + (int)(12.4f * s) },
                { x + (int)(9.9f * s), y + (int)(15.7f * s) },
            };
            Polyline(hdc, zz, 4);
            SelectObject(hdc, old);
            DeleteObject(pen);
        }
    }
    iconStrokeGdi(hdc, SVG_BAT, accent, x, y);
}
