// ------------------------------------------------------------ svg icons ----
// Chip icons: ONE stroke color each (theme.iconColor, default pinkDeep),
// drawn as the fewest lines that still read at 12x12 CSS px. The flattener
// walks each path once at startup; every frame strokes the polylines with a
// GDI geometric round pen.
//
// The battery is DYNAMIC: its inner fill tracks the current charge (icon
// color, warn red under 10%, full + zigzag bolt on AC). Drawn by
// svgDrawBatt, not from the static table.
//
// Path primitives supported by the flattener: M m L l H h V v A a Z z
// (+ implicit lineto after M/m). C/s curves were dropped when the icon set
// went line-only.

#include <math.h>

extern double g_iconOpacity; // theme.iconOpacity/100, defined in chocobar.c
RECT g_iconBoxes[64];        // icon bboxes this frame (p_ui fades them post-paint)
int g_iconBoxN = 0;

static void svgRecordBox(int x, int y) {
    if (g_iconOpacity >= 0.999 || g_iconBoxN >= 64) return;
    int bw = (int)(12 * g_scale + 0.5);
    g_iconBoxes[g_iconBoxN].left = x;
    g_iconBoxes[g_iconBoxN].top = y;
    g_iconBoxes[g_iconBoxN].right = x + bw;
    g_iconBoxes[g_iconBoxN].bottom = y + bw;
    g_iconBoxN++;
}

// icon ids (order = table below)
enum {
    SVG_BOW, SVG_DIAMOND, SVG_GAUGE, SVG_BOLT, SVG_CLOCK,
    SVG_GPU, SVG_CPU, SVG_TEMP, SVG_RAM, SVG_VOL, SVG_MUTE,
    SVG_BAT, SVG_COUNT
};

#define SVG_MAXSUB 10
#define SVG_MAXPTS 280
typedef struct { float x, y; } SvgPt;
typedef struct { SvgPt p[SVG_MAXPTS]; int n; int closed; } SvgSub;
typedef struct { SvgSub s[SVG_MAXSUB]; int nsub; } SvgPath;

// ---- flattened-path builder -------------------------------------------------
typedef struct {
    SvgPath *out;
    float cx, cy;        // current point
    float sx, sy;        // current subpath start
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
    if (lam > 1) { float s = (float)sqrt(lam); rx *= s; ry *= s; }
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
    int steps = (int)(fabs(a) / (3.14159265f / 18)) + 2;
    if (steps > 72) steps = 72;
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
typedef struct { const wchar_t *d; int wmul; } SvgPart;
static const SvgPart SVG_P[][3] = {
    // bow (pet): two wings + knot
    { { L"M3 7 L11.2 12 L3 17 Z M21 7 L12.8 12 L21 17 Z M9.8 9.4 h4.4 v5.2 h-4.4 Z", 1 } },
    // diamond (tokens): gem outline + girdle + top facets
    { { L"M4.5 9 L8.5 4 H15.5 L19.5 9 L12 20 Z M4.5 9 H19.5 M8.5 4 L12 9 L15.5 4", 1 } },
    // gauge (subs): ring + needle (a meter, not a C)
    { { L"M12 4.5 A7.5 7.5 0 1 0 12.01 4.5 Z", 1 }, { L"M12 12 L16.6 16.6", 1 }, { L"M10.8 12 a1.2 1.2 0 1 0 2.4 0 a1.2 1.2 0 1 0 -2.4 0 Z", 1 } },
    // bolt (shortcut)
    { { L"M13 2.5 L5 13.5 H10.6 L9.4 21.5 L19 9.5 H13.2 Z", 1 } },
    // clock (time): ring + hands
    { { L"M12 4 A8 8 0 1 0 12.01 4 Z", 1 }, { L"M12 7.4 V12.4 L15.4 14.4", 1 } },
    // gpu: card body + two fans + pins
    { { L"M5 7.5 H18.4 A1.6 1.6 0 0 1 20 9.1 V14.9 A1.6 1.6 0 0 1 18.4 16.5 H5 A1.6 1.6 0 0 1 3.4 14.9 V9.1 A1.6 1.6 0 0 1 5 7.5 Z M8.4 12 a1.9 1.9 0 1 0 3.8 0 a1.9 1.9 0 1 0 -3.8 0 Z M14.6 12 a1.9 1.9 0 1 0 3.8 0 a1.9 1.9 0 1 0 -3.8 0 Z M8 16.5 V19 M12 16.5 V19 M16 16.5 V19", 1 } },
    // cpu: body + core + pins (2 per side)
    { { L"M7.6 7.6 H16.4 A1.1 1.1 0 0 1 17.5 8.7 V15.3 A1.1 1.1 0 0 1 16.4 16.4 H7.6 A1.1 1.1 0 0 1 6.5 15.3 V8.7 A1.1 1.1 0 0 1 7.6 7.6 Z M9.7 9.7 H14.3 V14.3 H9.7 Z M9.5 3.6 V6.2 M14.5 3.6 V6.2 M9.5 17.8 V20.4 M14.5 17.8 V20.4 M3.6 9.5 H6.2 M3.6 14.5 H6.2 M17.8 9.5 H20.4 M17.8 14.5 H20.4", 1 } },
    // cpu temp: thermometer + one steam dash
    { { L"M10.1 4.6 A1.9 1.9 0 0 1 13.9 4.6 V13.3 A4.3 4.3 0 1 1 10.1 13.3 Z M12 9 V14 M17.4 7 H20 M17.4 11 H20 M17.4 15 H20", 1 } },
    // ram: stick + three chips + pins
    { { L"M4.6 7.6 H19.4 A1.4 1.4 0 0 1 20.8 9 V15 A1.4 1.4 0 0 1 19.4 16.4 H4.6 A1.4 1.4 0 0 1 3.2 15 V9 A1.4 1.4 0 0 1 4.6 7.6 Z M6.4 10 H8.8 V14 H6.4 Z M10.8 10 H13.2 V14 H10.8 Z M15.2 10 H17.6 V14 H15.2 Z M7 16.4 V19 M12 16.4 V19 M17 16.4 V19", 1 } },
    // volume: speaker + waves
    { { L"M11.2 5.2 L6.8 9.2 H3.4 V14.8 H6.8 L11.2 18.8 Z M15 9.6 A4.2 4.2 0 0 1 15 14.4 M17.6 7.2 A7.6 7.6 0 0 1 17.6 16.8", 1 } },
    // muted: speaker + X
    { { L"M11.2 5.2 L6.8 9.2 H3.4 V14.8 H6.8 L11.2 18.8 Z M15.8 9.8 L20.8 14.8 M20.8 9.8 L15.8 14.8", 1 } },
    // battery: body + nub only; the fill is dynamic (svgDrawBatt)
    { { L"M4.4 8.2 H17.6 A1.7 1.7 0 0 1 19.3 9.9 V14.1 A1.7 1.7 0 0 1 17.6 15.8 H4.4 A1.7 1.7 0 0 1 2.7 14.1 V9.9 A1.7 1.7 0 0 1 4.4 8.2 Z M21.3 10.4 V13.6", 1 } },
};

typedef struct { SvgPath path; int wmul; } SvgFlat;
#define SVG_MAXPARTS 3
static SvgFlat g_flat[SVG_COUNT][SVG_MAXPARTS];
static int g_flatN[SVG_COUNT];

static void svgInitAll(void) {
    for (int i = 0; i < SVG_COUNT; i++) {
        int n = 0;
        for (int j = 0; j < SVG_MAXPARTS; j++) {
            if (!SVG_P[i][j].d) break;
            memset(&g_flat[i][j].path, 0, sizeof(SvgPath));
            svgWalk(&g_flat[i][j].path, SVG_P[i][j].d);
            g_flat[i][j].wmul = SVG_P[i][j].wmul;
            n++;
        }
        g_flatN[i] = n;
    }
}

// stroke one icon in `color` into the premultiplied DIB at (x, ytop), 12*scale box
static void svgDraw(HDC hdc, int id, COLORREF color, int x, int y) {
    if (id < 0 || id >= SVG_COUNT || id == SVG_BAT) return;
    float s = (float)g_scale / 2;           // 24-unit box -> 12 CSS px -> physical
    int wpen = (int)(1.1f * g_scale + 0.5); // base stroke ~2.2/24 * box
    if (wpen < 1) wpen = 1;
    LOGBRUSH lb; lb.lbStyle = BS_SOLID; lb.lbColor = color; lb.lbHatch = 0;
    for (int j = 0; j < g_flatN[id]; j++) {
        int w = g_flat[id][j].wmul * wpen;
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
    svgRecordBox(x, y);
}

// battery with a charge-level fill: icon color (warn red under 10%), full +
// zigzag bolt on AC. Body outline comes from the static SVG_BAT part.
static void svgDrawBatt(HDC hdc, int pct, int ac, COLORREF accent, COLORREF warn,
                        COLORREF lineCr, int x, int y) {
    (void)lineCr;
    float s = (float)g_scale / 2;
    int yb = (int)(12 * g_scale + 0.5);
    (void)yb;
    // inner fill: 24-unit box x 4.9..17.1 (12.2 wide), y 9.7..14.3
    int ix0 = x + (int)(4.9f * s + 0.5f), iy0 = y + (int)(9.7f * s + 0.5f);
    int iy1 = y + (int)(14.3f * s + 0.5f);
    float fw = ac ? 12.2f : (pct <= 0 ? 0.0f : 12.2f * (float)pct / 100.0f);
    int ix1 = ix0 + (int)(fw * s);
    if (ix1 > ix0) {
        HBRUSH br = CreateSolidBrush(pct <= 10 && !ac ? warn : accent);
        RECT fr = { ix0, iy0, ix1, iy1 };
        FillRect(hdc, &fr, br);
        DeleteObject(br);
    }
    if (ac) { // bolt zigzag over the full fill (contrast color from the caller)
        LOGBRUSH lb; lb.lbStyle = BS_SOLID; lb.lbColor = lineCr; lb.lbHatch = 0;
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                (DWORD)(g_scale >= 1.5 ? 2 : 1), &lb, 0, NULL);
        if (pen) {
            HGDIOBJ old = SelectObject(hdc, pen);
            POINT zz[4] = {
                { x + (int)(11.9f * s), y + (int)(9.2f * s) },
                { x + (int)(9.3f * s), y + (int)(12.6f * s) },
                { x + (int)(11.6f * s), y + (int)(12.6f * s) },
                { x + (int)(9.9f * s), y + (int)(15.2f * s) },
            };
            Polyline(hdc, zz, 4);
            SelectObject(hdc, old);
            DeleteObject(pen);
        }
    }
    svgRecordBox(x, y);
    if (g_flatN[SVG_BAT] > 0) {
        int wpen = (int)(1.1f * g_scale + 0.5);
        if (wpen < 1) wpen = 1;
        LOGBRUSH lb; lb.lbStyle = BS_SOLID; lb.lbColor = accent; lb.lbHatch = 0;
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)wpen, &lb, 0, NULL);
        if (pen) {
            HGDIOBJ old = SelectObject(hdc, pen);
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            SvgPath *p = &g_flat[SVG_BAT][0].path;
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
}
