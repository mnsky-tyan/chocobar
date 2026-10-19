// ------------------------------------------------------------ svg icons ----
// Chip icons: ONE stroke color each (theme.iconColor, default pinkDeep).
// The PATH DATA and the flattener that samples it into polylines live in
// p_svg.c, shared with the macOS and Linux bars; this part is the Windows
// RENDERER only - it strokes those polylines with GDI+
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
// The facade below (gdip*) is also used by the dashboard painter for the
// donut pies and anti-aliased rounded cards.

#include "svg_geometry.h"

extern double g_iconOpacity; // theme.iconOpacity/100, defined in chocobar.c

// ---- config-defined icons -------------------------------------------------
// The bar's own icons are a static table, but NEW icons are wanted from the
// config (the buttons a user can add by name). A user icon keeps the same
// flattened-path representation, so it renders through the identical GDI+ AA
// path and the same per-icon DIB cache - only the id is shifted past SVG_COUNT.
#define SVG_USER_MAX MAX_USER_ICONS   // one capacity for config parse and renderer
#define SVG_USER_BASE 1000
#define SVG_ID_MAX (SVG_COUNT + SVG_USER_MAX)
// the stored name capacity, and the prefix length the store can actually hold
// (one less, for the NUL). Both the array bound and every compare derive from
// ICON_NAME_CCH so they cannot drift apart.
#define ICON_NAME_CCH 24
#define ICON_NAME_MATCH (ICON_NAME_CCH - 1)
static SvgFlat g_user[SVG_USER_MAX][SVG_MAXPARTS];
static int g_userN[SVG_USER_MAX];
static wchar_t g_userName[SVG_USER_MAX][ICON_NAME_CCH];
static int g_userCount = 0;

// name -> id among the config icons, -1 when not defined
int iconUserFind(const wchar_t *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < g_userCount; i++) {
        // the store holds names truncated to ICON_NAME_MATCH chars (see
        // iconUserAdd), so a longer config name registers a slot it could never
        // find with a full-string compare: match the stored prefix instead. A
        // stored name that did NOT hit the cap must match a name of the same
        // length (case-insensitively), so two distinct short names sharing a
        // prefix stay distinct.
        int stored = (int)wcslen(g_userName[i]);
        int hit = stored < ICON_NAME_MATCH
                      ? (stored == (int)wcslen(name) &&
                         _wcsnicmp(g_userName[i], name, ICON_NAME_MATCH) == 0)
                      : (_wcsnicmp(g_userName[i], name, ICON_NAME_MATCH) == 0);
        if (hit) return SVG_USER_BASE + i;
    }
    return -1;
}

// id -> (parts, count) for BOTH tables; NULL for an out-of-range id
static SvgFlat *flatSlot(int id, int *np) {
    if (id >= 0 && id < SVG_COUNT) { if (np) *np = g_flatN[id]; return g_flat[id]; }
    if (id >= SVG_USER_BASE && id < SVG_USER_BASE + SVG_USER_MAX) {
        int s = id - SVG_USER_BASE;
        if (np) *np = g_userN[s];
        return g_user[s];
    }
    if (np) *np = 0;
    return NULL;
}

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
static GpStatus (WINAPI *t_GdipFillPath)(GpGraphics *, GpBrush *, GpPath *);
static GpStatus (WINAPI *t_GdipAddPathArc)(GpPath *, float, float, float, float, float, float);
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
    GBIND(GdipClosePathFigure, "GdipClosePathFigure"); // flat API has no GdipCloseFigure
    GBIND(GdipCreatePen1, "GdipCreatePen1");
    GBIND(GdipDeletePen, "GdipDeletePen");
    GBIND(GdipSetPenStartCap, "GdipSetPenStartCap");
    GBIND(GdipSetPenEndCap, "GdipSetPenEndCap");
    GBIND(GdipSetPenLineJoin, "GdipSetPenLineJoin");
    GBIND(GdipDrawPath, "GdipDrawPath");
    GBIND(GdipDrawArc, "GdipDrawArc");
    GBIND(GdipDrawEllipse, "GdipDrawEllipse");
    GBIND(GdipFillEllipse, "GdipFillEllipse");
    GBIND(GdipFillPath, "GdipFillPath");
    GBIND(GdipAddPathArc, "GdipAddPathArc");
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
        (void *)t_GdipFillEllipse, (void *)t_GdipFillPath, (void *)t_GdipAddPathArc,
        (void *)t_GdipCreateSolidFill, (void *)t_GdipDeleteBrush };
    for (int i = 0; i < (int)(sizeof(tabs) / sizeof(tabs[0])); i++) {
        if (!tabs[i]) { writeLogA("[wizbar] gdiplus bind missing, falling back to GDI"); return; }
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
        t_GdipClosePathFigure(p);
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
static IconDib g_iconDib[SVG_ID_MAX];
static IconDib g_battDib;         // battery cache (charge bucket + ac + colors)
static IconDib g_ssDib;           // 2x supersample scratch (AA quality)
// g_battKey[4] is BOTH the key's own fifth element (always 1) and the "this
// cache has ever been filled" flag; name the role so the double duty is
// visible at every use
#define BATTKEY_VALID 1
static int g_battKey[6];          // pct bucket, ac, accent, warn, valid, warn-fill

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

// render one flattened path set into an icon DIB with GDI+ AA strokes
// Draw the icon into `g` at supersample factor ss (path units -> pixels).
static void iconStrokeAll(GpGraphics *g, int id, COLORREF color, float s, float penW) {
    unsigned argb = GDIP_ARGB(color);
    int nparts = 0;
    SvgFlat *fl = flatSlot(id, &nparts);
    if (!fl) return;
    for (int j = 0; j < nparts; j++) {
        SvgPath *sp = &fl[j].path;
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
        if (t_GdipCreatePen1(argb, (float)(penW * (fl[j].w ? fl[j].w : 1.0f)), 2 /*pixel*/, &pen) == 0) {
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
    int nparts = 0;
    SvgFlat *fl = flatSlot(id, &nparts);
    if (!fl) return;
    for (int j = 0; j < nparts; j++) {
        int w = (int)(wpen * (fl[j].w ? fl[j].w : 1.0f) + 0.5);
        if (w < 1) w = 1;
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)w, &lb, 0, NULL);
        if (!pen) return;
        HGDIOBJ old = SelectObject(hdc, pen);
        SvgPath *p = &fl[j].path;
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
        DeleteObject(pen);
    }
}

static void svgInitAll(void) {
    svgFlattenAll();   // p_svg.c: the shared path data, flattened once
    gdipInit();
}

// blit a cached icon DIB onto the target DC with the opacity alpha
static void iconBlit(HDC hdc, IconDib *d, int x, int y) {
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(g_iconOpacity * 255.0 + 0.5), AC_SRC_ALPHA };
    AlphaBlend(hdc, x, y, d->w, d->h, d->dc, 0, 0, d->w, d->h, bf);
}

// stroke one icon in `color` into the premultiplied DIB at (x, ytop), 12*scale box
// id -> DIB cache slot: user icons live past the built-in id range but
// share one cache array (their slots follow the built-ins)
static IconDib *dibSlot(int id) {
    if (id >= 0 && id < SVG_COUNT) return &g_iconDib[id];
    if (id >= SVG_USER_BASE && id < SVG_USER_BASE + SVG_USER_MAX)
        return &g_iconDib[SVG_COUNT + (id - SVG_USER_BASE)];
    return NULL;
}

// flatten one config icon (name | SVG path data | stroke width). The path data
// may hold several subpaths in one "d" string - svgWalk starts a new subpath
// on every M. Returns the icon id, or -1 when the table is full.
int iconUserAdd(const wchar_t *name, const wchar_t *d, float w) {
    if (g_userCount >= SVG_USER_MAX || !name || !*name || !d || !*d) return -1;
    int slot = -1;
    for (int i = 0; i < g_userCount; i++) {
        // redefine = replace, but only when it IS the same name. The store can
        // hold ICON_NAME_MATCH chars, so a stored name that is shorter than the
        // cap must match an incoming name of the same length (case-insensitively);
        // only a stored name that hit the cap is a truncated prefix that a longer
        // incoming name may extend. Without this, two distinct names sharing
        // ICON_NAME_MATCH chars would silently collapse into one slot and the
        // first glyph would be lost.
        int stored = (int)wcslen(g_userName[i]);
        if (stored < ICON_NAME_MATCH) {
            if (stored == (int)wcslen(name) &&
                _wcsnicmp(g_userName[i], name, ICON_NAME_MATCH) == 0) { slot = i; break; }
        } else if (_wcsnicmp(g_userName[i], name, ICON_NAME_MATCH) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) { slot = g_userCount++; lstrcpynW(g_userName[slot], name, ICON_NAME_CCH); }
    for (int j = 0; j < SVG_MAXPARTS; j++) {
        memset(&g_user[slot][j], 0, sizeof(SvgFlat));
    }
    // one part: the whole d string (svgWalk splits on M into subpaths)
    svgWalk(&g_user[slot][0].path, d);
    g_user[slot][0].w = w > 0 ? w : 2.2f;
    g_userN[slot] = 1;
    IconDib *dib = dibSlot(SVG_USER_BASE + slot); // the old glyph's cache died with the old path data
    if (dib) iconDrop(dib);
    return SVG_USER_BASE + slot;
}

static void svgDraw(HDC hdc, int id, COLORREF color, int x, int y) {
    int nparts = 0;
    IconDib *d = dibSlot(id);
    if (!d || !flatSlot(id, &nparts) || !nparts || id == SVG_BAT) return; // bat is dynamic (svgDrawBatt)
    int bw = (int)(12 * g_scale + 0.5);
    if (g_gdipOk) {
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
                        int x, int y) {
    int bw = (int)(12 * g_scale + 0.5);
    int bucket = (pct < 0 ? 0 : (pct > 100 ? 100 : pct)) / 2;
    // the fill color switches on the raw pct (not the 2% bucket): key on the
    // warn fact too or 10/11% share bucket 5 and the color goes stale
    int key[6] = { bucket, ac ? 1 : 0, (int)accent, (int)warn, BATTKEY_VALID,
                   (pct < 10) ? 1 : 0 };
    if (g_gdipOk) {
        if (!g_battKey[4] || memcmp(g_battKey, key, sizeof(key)) != 0 || g_battDib.w != bw) {
            if (iconRenderGdip(&g_battDib, SVG_BAT, accent, bw, bw)) {
                float s = (float)g_scale / 2;
                // inner fill: Electron ICONS.batBody - innerX 4.3, innerW 12.4,
                // y 9.7, h 5.1, rx 0.8, opacity 0.75 (0.9 under 10%), 4% floor
                GpGraphics *g = NULL;
                if (t_GdipCreateFromHDC(g_battDib.dc, &g) == 0) {
                    t_GdipSetSmoothingMode(g, 6);
                    float fw = pct <= 0 ? 0.0f : 12.4f * (float)(bucket * 2) / 100.0f;
                    if (fw < 0.9f) fw = 0.9f; // never zero-width sliver floor
                    int low = (pct < 10) && !ac; // charging outranks the red fill too
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
                        t_GdipClosePathFigure(fp);
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
        HBRUSH br = CreateSolidBrush((pct < 10) && !ac ? warn : accent); // charging outranks the red fill here too
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
