// ------------------------------------------------------------ svg paths ----
// The SVG path data behind every chip icon, and the flattener that turns it
// into polylines, shared by every platform.
//
// WHAT IS SHARED AND WHAT IS NOT
// The flattener is pure arithmetic (no Win32 type, no drawing call): it walks
// an SVG path string and samples arcs/cubics into SvgPath point lists. The
// RENDERERS are per platform - p_icons.c strokes these points with GDI+/GDI
// onto a premultiplied DIB, the Linux bar strokes them with cairo. Path data
// written twice would drift; the icons are the product's look, so they are
// written once, here, and every platform renders the same geometry.
//
// This part is a PREFIX of the Windows translation unit (it sits where these
// declarations used to live, inside p_icons.c's old body), so p_icons.c keeps
// using them unchanged. It is compiled whole into the macOS and Linux builds
// via CHOCOBAR_PORTABLE, where the id enum below is the only thing a renderer
// needs to know about.
//
// Path primitives supported by the flattener: M m L l H h V v A a C c S s Z z
// (+ implicit lineto after M/m). Arcs sample every 5 degrees so circles stay
// round at 2x DPI; C/c/S/s cubics are sampled too (the bow's loops).

#include <math.h>
#include <wchar.h>

#include "svg_geometry.h"

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
    if (!any) {
        // svgWalk treats 0 as "break the switch, keep walking", so a failed
        // read must still land the cursor past the junk byte or the walk spins
        if (i == *pi && d[i]) i++;
        *pi = i;
        return 0;
    }
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
                k.prev = L'Z'; // a closepath is not C/S: reflection must not survive it
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
            k.prev = L'M'; // only C/S reflect; any other command clears the handle
            cmd = rel ? L'l' : L'L'; // implicit lineto continuation
            break;
        }
        case L'L': case L'l': {
            float x, y;
            if (!svgNum(d, &i, &x)) break;
            if (!svgNum(d, &i, &y)) break;
            if (rel) { x += k.cx; y += k.cy; }
            svgLine(&k, x, y);
            k.prev = L'L';
            break;
        }
        case L'H': case L'h': {
            float x;
            if (!svgNum(d, &i, &x)) break;
            if (rel) x += k.cx;
            svgLine(&k, x, k.cy);
            k.prev = L'H';
            break;
        }
        case L'V': case L'v': {
            float y;
            if (!svgNum(d, &i, &y)) break;
            if (rel) y += k.cy;
            svgLine(&k, k.cx, y);
            k.prev = L'V';
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
            k.prev = 0; // an unrecognised command clears the C/S reflection handle
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
    // gpu: card + big fan + two vents + pins (Electron ICONS.gpu). The fan is
    // the silhouette that reads as "graphics card" - it was 2.4r and got lost
    // next to the ram stick, so it is now 3.3r and the vents are wider apart.
    { { L"M4.5 7H17.5A1.5 1.5 0 0 1 19 8.5V15.5A1.5 1.5 0 0 1 17.5 17H4.5A1.5 1.5 0 0 1 3 15.5V8.5A1.5 1.5 0 0 1 4.5 7Z"
        L"M5.1 12A3.3 3.3 0 1 0 11.7 12A3.3 3.3 0 1 0 5.1 12Z"
        L"M14 9.6V14.4M16.8 9.6V14.4"
        L"M6.5 17V20M10.5 17V20", 1.0f } },
    // cpu: body + pins 2 per side (Electron ICONS.cpu)
    { { L"M7.5 6H16.5A1.5 1.5 0 0 1 18 7.5V16.5A1.5 1.5 0 0 1 16.5 18H7.5A1.5 1.5 0 0 1 6 16.5V7.5A1.5 1.5 0 0 1 7.5 6Z"
        L"M9 2V5M15 2V5M9 19V22M15 19V22M2 9H5M2 15H5M19 9H22M19 15H22", 1.0f } },
    // cpu temp: thermometer + stem (Electron ICONS.temp)
    { { L"M10 4A2 2 0 1 1 14 4V13.3A4.5 4.5 0 1 1 10 13.3Z", 1.0f },
      { L"M12 9.5V16", 1.0f } },
    // ram: stick + two chips + pins (Electron ICONS.ram). The bare vertical
    // lines are replaced by two filled-looking chip rectangles: "a stick with
    // chips on it" is what makes it read as memory next to the gpu card.
    { { L"M4.5 8H19.5A1.5 1.5 0 0 1 21 9.5V15.5A1.5 1.5 0 0 1 19.5 17H4.5A1.5 1.5 0 0 1 3 15.5V9.5A1.5 1.5 0 0 1 4.5 8Z"
        L"M6.8 10.3H11.4V13.7H6.8Z"
        L"M13.2 10.3H17.8V13.7H13.2Z"
        L"M7 17V20.2M12 17V20.2M17 17V20.2", 1.0f } },
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

// ---- flattening tables ------------------------------------------------------
static SvgFlat g_flat[SVG_COUNT][SVG_MAXPARTS];
static int g_flatN[SVG_COUNT];

// Flatten one path string into `out` at stroke width `w`. Ids index the table
// above (SVG_BOW..SVG_BAT); every renderer starts by calling this once per id.
// Windows calls it from svgInitAll() in p_icons.c; the Linux bar calls it once
// at startup and then strokes g_flat directly.
static void svgFlattenAll(void) {
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
}

#ifdef CHOCOBAR_PORTABLE
// The accessors a front-end in its own translation unit uses (svg_geometry.h
// declares them). The Windows build never compiles this block: it calls
// svgFlattenAll() and g_flat directly, inside the amalgamated translation unit.
void cbSvgFlattenAll(void) { svgFlattenAll(); }

const SvgFlat *cbSvgFlatById(int id, int *nparts) {
    if (id < 0 || id >= SVG_COUNT) { if (nparts) *nparts = 0; return NULL; }
    if (nparts) *nparts = g_flatN[id];
    return g_flat[id];
}
#endif
