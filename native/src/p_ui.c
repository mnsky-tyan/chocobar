// ------------------------------------------------------------------ ui ----
// append-only diagnostics file; used for crashes and D2D failures only
static void writeLogA(const char *s) {
    HANDLE h = CreateFileW(L"C:\\Users\\tyanw\\.wizbar\\native.log",
                           FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD w;
    WriteFile(h, s, lstrlenA(s), &w, NULL);
    WriteFile(h, "\r\n", 2, &w, NULL);
    CloseHandle(h);
}
static LONG WINAPI crashHandler(EXCEPTION_POINTERS *e) {
    char b[80];
    sprintf(b, "crash %08lX addr %p", (unsigned long)e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionAddress);
    writeLogA(b);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void freeConfig(Config *c);
void ui_log(const char *s);
static void writeTemplate(void);
static void loadConfig(void);

// ---- DirectComposition, declared by hand (mingw's dcomp.h is C++-only) ----
typedef struct IDCompositionTarget IDCompositionTarget;
typedef struct IDCompositionSurface IDCompositionSurface;
typedef struct IDCompositionVisual IDCompositionVisual;
typedef struct IDCompositionDevice IDCompositionDevice;

static const GUID my_IID_IDCompositionDevice = {0xc37ea93a,0xe7aa,0x450d,{0xb1,0x6f,0x97,0x46,0xcb,0x04,0x07,0xf3}}; // dcomp.h DECLARE_INTERFACE_IID_ value
static const GUID my_IID_IDXGISurface        = {0xcafcb56c,0x6ac3,0x4889,{0xbf,0x47,0x9e,0x23,0xbb,0xd2,0x60,0xec}};
static const GUID my_IID_IDXGIDevice         = {0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};

// DirectComposition COM interfaces (mingw's dcomp.h is broken in C mode - duplicate
// overload members), slots verified against the dcomp.h interface declarations:
typedef struct IDCompositionDeviceVtbl {
    HRESULT (__stdcall *QueryInterface)(IDCompositionDevice *, const GUID *, void **);
    ULONG (__stdcall *AddRef)(IDCompositionDevice *);
    ULONG (__stdcall *Release)(IDCompositionDevice *);
    HRESULT (__stdcall *Commit)(IDCompositionDevice *);                                  // 3
    void *WaitForCommitCompletion, *GetFrameStatistics;                                  // 4-5
    HRESULT (__stdcall *CreateTargetForHwnd)(IDCompositionDevice *, HWND, BOOL, IDCompositionTarget **); // 6
    HRESULT (__stdcall *CreateVisual)(IDCompositionDevice *, IDCompositionVisual **);    // 7
    HRESULT (__stdcall *CreateSurface)(IDCompositionDevice *, UINT, UINT, int, int, IDCompositionSurface **); // 8 (w, h, format, alphaMode)
    void *pad9_26; // CreateVirtualSurface..CheckDeviceState (not used)
} IDCompositionDeviceVtbl;
typedef struct IDCompositionDevice { const IDCompositionDeviceVtbl *lpVtbl; } IDCompositionDevice;

typedef struct IDCompositionTargetVtbl {
    HRESULT (__stdcall *QueryInterface)(IDCompositionTarget *, const GUID *, void **);
    ULONG (__stdcall *AddRef)(IDCompositionTarget *);
    ULONG (__stdcall *Release)(IDCompositionTarget *);
    HRESULT (__stdcall *SetRoot)(IDCompositionTarget *, IDCompositionVisual *);          // 3
} IDCompositionTargetVtbl;
struct IDCompositionTarget { const IDCompositionTargetVtbl *lpVtbl; };

typedef struct IDCompositionVisualVtbl {
    HRESULT (__stdcall *QueryInterface)(IDCompositionVisual *, const GUID *, void **);
    ULONG (__stdcall *AddRef)(IDCompositionVisual *);
    ULONG (__stdcall *Release)(IDCompositionVisual *);
    void *pad3_14[12]; // SetOffsetX/Y x2, SetTransform x2, SetTransformParent, SetEffect, SetBitmapInterpolationMode, SetBorderMode, SetClip x2 (slots 3-14)
    HRESULT (__stdcall *SetContent)(IDCompositionVisual *, IUnknown *);                  // 15
} IDCompositionVisualVtbl;
struct IDCompositionVisual { const IDCompositionVisualVtbl *lpVtbl; };

typedef struct IDCompositionSurfaceVtbl { // slots per the dcomp.h interface block
    HRESULT (__stdcall *QueryInterface)(IDCompositionSurface *, const GUID *, void **);
    ULONG (__stdcall *AddRef)(IDCompositionSurface *);
    ULONG (__stdcall *Release)(IDCompositionSurface *);
    HRESULT (__stdcall *BeginDraw)(IDCompositionSurface *, const RECT *, const GUID *, void **, POINT *); // 3
    HRESULT (__stdcall *EndDraw)(IDCompositionSurface *);                                                 // 4
    void *pad5_7;                              // SuspendDraw, ResumeDraw, Scroll
    HRESULT (__stdcall *Resize)(IDCompositionSurface *, UINT, UINT, int);                                 // 8
} IDCompositionSurfaceVtbl;
struct IDCompositionSurface { const IDCompositionSurfaceVtbl *lpVtbl; };

HRESULT __stdcall DCompositionCreateDevice(IDXGIDevice *, const GUID *, void **);

static const GUID my_IID_IDXGIFactory2       = {0x50c83a1c,0xe072,0x4c48,{0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0}};

// shared state (declared here so every section below sees it)
static wchar_t g_cfgPath[MAX_PATH];
extern double g_scale; // display scale (dpi/96): defined in chocobar.c (shared with p_icons)
static int g_customState[MAX_CUSTOM];

// ------------------------------------------------------------- globals ----
static HWND g_bar;
// layered-window renderer: draw into a 32bpp premultiplied DIB, then
// UpdateLayeredWindow. No D2D/DComp - both fail to present on this machine.
static HDC g_memDc = NULL;
static HBITMAP g_dib = NULL;
static HGDIOBJ g_dibOld = NULL;
static void *g_bits = NULL;
static LONG g_dibW = 0, g_dibH = 0;
static HFONT g_font = NULL;
static HFONT g_fontOld = NULL;
static NOTIFYICONDATAW g_nid;
static int g_trayAdded = 0;
static int g_barVisible = 0;
static HWND g_term = NULL;
static RECT g_lastTarget;
static int g_haveTarget = 0;
static int g_hover = -1;
static int g_trackingMouse = 0;

static wchar_t *g_clockFmt = NULL;
static const wchar_t *clockFmt(void) { return g_clockFmt ? g_clockFmt : (g_clockFmt = wideDup(L"{MMM} {dd} ({Wkk}) {HH}:{mm}")); }
static void clockFmtReload(void) { wideFree(&g_clockFmt); g_clockFmt = wideDup(g_cfg.clockFormat); }

typedef struct {
    int type;            // CT_*
    int customIdx;
    RECT r;
    wchar_t text[96];
    int warn;
    const wchar_t *colorOverride;
    unsigned iconCp;     // Nerd Font codepoint drawn before text (0 = none)
    int iconSvg;         // vector icon id (p_icons) - preferred over iconCp
    const wchar_t *iconColorOverride; // per-chip icon color (bar.css .ico rules)
    int align;           // 1 = right group (metrics), 2 = left pinned group
} Chip;

enum { CT_SHORTCUT, CT_PET, CT_CUSTOM, CT_GPU, CT_CPU, CT_CPUTEMP, CT_RAM, CT_VOLUME, CT_BATTERY, CT_CLOCK };
#define MAX_CHIPS 32
static Chip g_chips[MAX_CHIPS];
static int g_chipCount = 0;

// --------------------------------------------------------- d2d plumbing ----
static int initRender(HWND hwnd) {
    (void)hwnd;
    g_memDc = CreateCompatibleDC(NULL);
    if (!g_memDc) return 0;
    // first quoted family from the config, else let GDI map a default
    wchar_t fam[64];
    const wchar_t *src = g_cfg.fontFamily;
    while (src && *src && *src != L'\'' && *src != L'-' && !iswalpha(*src)) src++;
    int n = 0;
    if (src && *src == L'\'') {
        src++;
        while (src[n] && src[n] != L'\'' && n < 63) { fam[n] = src[n]; n++; }
    } else {
        while (src && src[n] && src[n] != L',' && n < 63) { fam[n] = src[n]; n++; }
    }
    fam[n] = 0;
    // Electron bar.css: font-weight 600, font-size 11px CSS -> em px at DPI.
    // The Meslo Nerd Font family ships only Regular + Bold: Chromium maps the
    // 600 to Regular (no synthetic bolding below the 700 threshold), while GDI
    // rounds FW_SEMIBOLD up to Bold - which read too heavy. FW_NORMAL is the
    // Chromium-identical face.
    int px = (int)(g_cfg.fontSize * g_scale + 0.5);
    g_font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, fam[0] ? fam : L"Segoe UI");
    if (!g_font) return 0;
    g_fontOld = (HFONT)SelectObject(g_memDc, g_font);
    return 1;
}

// apply translucency: DWM acrylic backdrop, or plain opaque if backdrop=solid
static void applyBackdrop(HWND hwnd) {
    MARGINS m = {-1};
    DwmExtendFrameIntoClientArea(hwnd, &m);
    if (lstrcmpiW(g_cfg.backdrop, L"solid") != 0) {
        DWORD bt = 2; // DWMSBT_TRANSIENTWINDOW (acrylic)
        DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &bt, sizeof(bt));
    } else {
        DWORD bt = 1; // DWMSBT_NONE
        DwmSetWindowAttribute(hwnd, 38, &bt, sizeof(bt));
    }
}

// --------------------------------------------------------- chip building ----
static int customStateGet(int idx) { return idx >= 0 && idx < MAX_CUSTOM ? g_customState[idx] : 0; }

static void addChipI(int type, int customIdx, const wchar_t *text, int warn,
                     const wchar_t *colorOverride, unsigned iconCp) {
    if (g_chipCount >= MAX_CHIPS) return;
    Chip *c = &g_chips[g_chipCount++];
    c->type = type; c->customIdx = customIdx; c->warn = warn;
    c->colorOverride = colorOverride; c->iconCp = iconCp;
    c->iconSvg = -1;
    c->iconColorOverride = NULL;
    c->align = 1;
    lstrcpynW(c->text, text ? text : L"", 96);
    c->r.left = c->r.right = c->r.top = c->r.bottom = 0;
}
static void addChip(int type, int customIdx, const wchar_t *text, int warn, const wchar_t *colorOverride) {
    addChipI(type, customIdx, text, warn, colorOverride, 0);
}

static long long g_tokensToday = -1;
static int g_tokensTick = 0;

// ---- dashboard aggregation (filled by scanTokenCache) ----------------------
#define DASH_MAX_APPS 12
#define DASH_MAX_MODELS 64
#define DASH_MAX_DAYS 190
// token-cache record fields kept separately: the Electron dash shows the
// input/output/cache R/cache W/calls table columns, not one lumped sum
typedef struct { long long in, out, cr, cw, req; } TokAgg;
static TokAgg g_appAgg[DASH_MAX_APPS];
static char g_appName[DASH_MAX_APPS][20];
static int g_appCount = 0;
static char g_modelName[DASH_MAX_MODELS][48]; // "app|model", Electron byModel key
static char g_modelLabel[DASH_MAX_MODELS][48]; // first-seen raw model id (display)
static TokAgg g_modelAgg[DASH_MAX_MODELS];
static int g_modelCount = 0;
static TokAgg g_dayApp[DASH_MAX_DAYS][DASH_MAX_APPS]; // per-day per-app (day detail)
static long long g_tokWeek = 0, g_tokMonth = 0, g_tokAll = 0;
static long long g_dayTot[DASH_MAX_DAYS]; // [DASH_MAX_DAYS-1] = today
static long long g_lastScanMs = 0;
static unsigned long long g_tokDataVersion = 0;   // moves on every completed scan
static unsigned long long g_dashTokVersion = 0;   // version the open board shows
static int g_daySel = -1; // selected heatmap cell (daysBack), -1 = none

// blend two opaque colors, num/256 of the foreground
static COLORREF blendCr(COLORREF bg, COLORREF fg, int num) {
    int r = GetRValue(bg) + ((GetRValue(fg) - GetRValue(bg)) * num >> 8);
    int g2 = GetGValue(bg) + ((GetGValue(fg) - GetGValue(bg)) * num >> 8);
    int b = GetBValue(bg) + ((GetBValue(fg) - GetBValue(bg)) * num >> 8);
    return RGB(r, g2, b);
}

static void aggRecord(const char *app, int alen, long long ts,
                      long long vin, long long vout, long long vcr, long long vcw,
                      const char *model, int mlen, const long long *bnd) {
    if (alen <= 0) alen = 1;
    if (alen > 19) alen = 19;
    // tokens.appFilter: when set, only the listed harness apps are tracked
    if (g_cfg.tokensAppCount > 0) {
        int ok = 0;
        for (int i = 0; i < g_cfg.tokensAppCount && !ok; i++) {
            wchar_t wide[20];
            MultiByteToWideChar(CP_UTF8, 0, app, alen, wide, 20);
            wide[alen] = 0;
            if (lstrcmpiW(wide, g_cfg.tokensApps[i]) == 0) ok = 1;
        }
        if (!ok) return;
    }
    long long sum = vin + vout + vcr + vcw;
    int ai = -1;
    for (int i = 0; i < g_appCount; i++)
        if (memcmp(g_appName[i], app, alen) == 0 && g_appName[i][alen] == 0) { ai = i; break; }
    if (ai < 0 && g_appCount < DASH_MAX_APPS) {
        ai = g_appCount++;
        memcpy(g_appName[ai], app, alen);
        g_appName[ai][alen] = 0;
    }
    if (ai >= 0) {
        g_appAgg[ai].in += vin; g_appAgg[ai].out += vout;
        g_appAgg[ai].cr += vcr; g_appAgg[ai].cw += vcw;
        g_appAgg[ai].req++;
    }
    // byModel: Electron keys are "app|lowercased model" (case-insensitive
    // grouping), so the same model recorded under two casings merges
    if (model && mlen > 0 && ai >= 0) {
        // the key must fit g_modelName's 48-byte row: clamp to 46 chars + NUL
        if (mlen > 40) mlen = 40;
        char mk[48];
        int n = 0;
        memcpy(mk, app, alen); n = alen;
        mk[n++] = '|';
        int room = 46 - n;
        if (mlen > room) mlen = room;
        for (int i = 0; i < mlen; i++) {
            char c = model[i];
            mk[n + i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        n += mlen;
        mk[n] = 0;
        int mi = -1;
        for (int i = 0; i < g_modelCount; i++)
            if (strncmp(g_modelName[i], mk, 47) == 0) { mi = i; break; }
        if (mi < 0 && g_modelCount < DASH_MAX_MODELS) {
            mi = g_modelCount++;
            memcpy(g_modelName[mi], mk, (size_t)n + 1);
            memcpy(g_modelLabel[mi], model, (size_t)mlen); // display: first-seen raw casing
            g_modelLabel[mi][mlen] = 0;
        }
        if (mi >= 0) {
            g_modelAgg[mi].in += vin; g_modelAgg[mi].out += vout;
            g_modelAgg[mi].cr += vcr; g_modelAgg[mi].cw += vcw;
            g_modelAgg[mi].req++;
        }
    }
    // first boundary at or before ts: bnd[1] is today, bnd[DASH_MAX_DAYS] the
    // oldest bucket, bnd[0] (tomorrow) rejects later-dated records
    int lo = 0, hi = DASH_MAX_DAYS, j = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (bnd[mid] <= ts) { j = mid; hi = mid - 1; } else lo = mid + 1;
    }
    if (j >= 1) {
        int di = DASH_MAX_DAYS - j;
        g_dayTot[di] += sum;
        if (ai >= 0) {
            g_dayApp[di][ai].in += vin; g_dayApp[di][ai].out += vout;
            g_dayApp[di][ai].cr += vcr; g_dayApp[di][ai].cw += vcw;
            g_dayApp[di][ai].req++;
        }
    }
}

static const char *findStr(const char *p, const char *end, const char *needle) {
    int n = 0; while (needle[n]) n++;
    while (p + n <= end) {
        if (*p == needle[0] && memcmp(p, needle, n) == 0) return p;
        p++;
    }
    return NULL;
}

static long long parseLL(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == ':')) p++;
    int neg = 0; if (p < end && *p == '-') { neg = 1; p++; }
    long long v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return neg ? -v : v;
}

// true epoch-ms of local midnight, daysBack days ago. The subtraction runs in
// the local wall-clock frame (exact calendar arithmetic, as dashColDate does);
// only the last step applies the time zone, so every day gets its own
// DST-correct boundary instead of a fixed 24h multiple of today's.
static long long dashMidnightMs(const FILETIME *localMidnight, int daysBack) {
    long long v = ((((long long)localMidnight->dwHighDateTime) << 32) | localMidnight->dwLowDateTime)
                - (long long)daysBack * 864000000000LL;
    FILETIME sub; sub.dwHighDateTime = (DWORD)(v >> 32); sub.dwLowDateTime = (DWORD)v;
    FILETIME ft;
    LocalFileTimeToFileTime(&sub, &ft);
    return ((((long long)ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000 - 11644473600000LL;
}

// today's raw (cache-exclusive) input+output, mirroring tokens.js; the
// Electron app rewrites this cache every rescan, we just read it
// every completed scan (data OR no-data) moves the version, so the open
// dashboard can repaint on change instead of on a fixed heartbeat
static void scanTokenCacheInner(void) {
    wchar_t path[MAX_PATH];
    path[0] = 0;
    if (g_cfg.tokenCachePath && *g_cfg.tokenCachePath) {
        // ~ prefix = relative to the profile dir; else absolute
        if (g_cfg.tokenCachePath[0] == L'~' && lstrlenW(g_cfg.tokenCachePath) < MAX_PATH - 2) {
            DWORD n = GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH);
            if (!n) { g_tokensToday = -1; return; }
            lstrcatW(path, g_cfg.tokenCachePath + 1);
        } else if (lstrlenW(g_cfg.tokenCachePath) < MAX_PATH) {
            lstrcpynW(path, g_cfg.tokenCachePath, MAX_PATH);
        }
    } else {
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH);
        if (!n || n >= MAX_PATH - 40) { g_tokensToday = -1; return; }
        lstrcatW(path, L"\\.wizbar\\token-cache.json");
    }
    if (!path[0]) { g_tokensToday = -1; return; }
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { g_tokensToday = -1; return; }
    DWORD size = GetFileSize(h, NULL), got = 0;
    if (size == INVALID_FILE_SIZE || !size) { CloseHandle(h); g_tokensToday = -1; return; }
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)size + 1);
    if (!buf) { CloseHandle(h); g_tokensToday = -1; return; }
    // the Electron app rewrites this file in place: a read can land mid-write.
    // require the full size, retry a few times before trusting the number.
    BOOL ok = FALSE;
    for (int attempt = 0; attempt < 4; attempt++) {
        size = GetFileSize(h, NULL);
        if (size == INVALID_FILE_SIZE || !size) break;
        ok = ReadFile(h, buf, size, &got, NULL) && got == size;
        if (ok) break;
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        Sleep(150);
    }
    CloseHandle(h);
    if (!ok) { HeapFree(GetProcessHeap(), 0, buf); g_tokensToday = -1; return; }
    buf[got] = 0;

    SYSTEMTIME st; GetLocalTime(&st);
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME localMidnight;
    SystemTimeToFileTime(&st, &localMidnight);
    long long midnight = dashMidnightMs(&localMidnight, 0);
    // one DST-exact boundary per heatmap bucket: bnd[j] opens the bucket that
    // holds day j-1, so a bucket never straddles a calendar day
    long long bnd[DASH_MAX_DAYS + 1];
    for (int j = 0; j <= DASH_MAX_DAYS; j++) bnd[j] = dashMidnightMs(&localMidnight, j - 1);
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    long long nowMs = ((((long long)nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime) / 10000 - 11644473600000LL;
    long long total = 0;
    memset(g_dayTot, 0, sizeof(g_dayTot));
    memset(g_appAgg, 0, sizeof(g_appAgg));
    memset(g_dayApp, 0, sizeof(g_dayApp));
    memset(g_modelAgg, 0, sizeof(g_modelAgg));
    g_appCount = 0;
    g_modelCount = 0;
    g_tokWeek = g_tokMonth = g_tokAll = 0;
    // records look like ["key",{"app":"<name>","ts":...,...}] - walk by the
    // app key (it precedes ts inside each record)
    const char *p = buf, *end = buf + got;
    while ((p = findStr(p, end, "\"app\":\"")) != NULL) {
        p += 7;
        const char *ae = p;
        while (ae < end && *ae != '"' && *ae && ae - p < 24) ae++;
        if (ae >= end || *ae != '"') break;
        int alen = (int)(ae - p);
        const char *tsp = findStr(ae, end, "\"ts\":");
        if (!tsp) break;
        long long ts = parseLL(tsp + 5, end);
        const char *next = findStr(tsp + 5, end, "\"app\":\"");
        const char *recEnd = next ? next : end;
        // per-field breakdown (the dash table columns); Electron rowTotal =
        // input + output + cacheRead + cacheWrite
        const char *keys[4] = { "\"input\":", "\"output\":", "\"cacheRead\":", "\"cacheWrite\":" };
        int lens[4] = { 8, 9, 12, 13 };
        long long fld[4] = { 0, 0, 0, 0 };
        long long sum = 0;
        for (int k = 0; k < 4; k++) {
            const char *f = findStr(ae, recEnd, keys[k]);
            if (f) { fld[k] = parseLL(f + lens[k], recEnd); sum += fld[k]; }
        }
        const char *mp = findStr(tsp + 5, recEnd, "\"model\":\"");
        const char *mv = NULL; int mlen = 0;
        if (mp) {
            mv = mp + 9;
            const char *me = mv;
            while (me < recEnd && *me != '"' && *me && me - mv < 40) me++;
            // ids longer than the 40-char cap keep their prefix: aggRecord's
            // room clamp truncates the key, dropping the record would silently
            // de-sync BY MODEL from every other section
            if (me > mv) mlen = (int)(me - mv);
        }
        if (ts >= midnight) total += sum;
        aggRecord(p, alen, ts, fld[0], fld[1], fld[2], fld[3], mv, mlen, bnd);
        if (ts >= nowMs - 7LL * 86400000LL) g_tokWeek += sum;
        if (ts >= nowMs - 30LL * 86400000LL) g_tokMonth += sum;
        g_tokAll += sum;
        p = next ? next : end;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    g_tokensToday = total;
    g_lastScanMs = (long long)GetTickCount64();
}

static void scanTokenCache(void) {
    scanTokenCacheInner();
    g_tokDataVersion++;
}

// token counts (renderer/dash.js fmt): B / M / k tiers
static void fmtTokens(long long n2, wchar_t *out, int cb) {
    if (n2 < 0) { lstrcpynW(out, L"\u2014", cb); return; }
    if (n2 >= 1000000000LL) swprintf(out, cb, L"%.2fB", n2 / 1e9);
    else if (n2 >= 1000000) swprintf(out, cb, L"%.1fM", n2 / 1e6);
    else if (n2 >= 1000) swprintf(out, cb, L"%.1fk", n2 / 1e3);
    else swprintf(out, cb, L"%lld", n2);
}

// renderer/subs.js fmtNum: M and k tiers only (no B), values rounded
static void fmtNum(long long n2, wchar_t *out, int cb) {
    if (n2 < 0) { lstrcpynW(out, L"\u2014", cb); return; }
    if (n2 >= 1000000LL) swprintf(out, cb, L"%.1fM", n2 / 1e6);
    else if (n2 >= 1000) swprintf(out, cb, L"%.1fk", n2 / 1e3);
    else swprintf(out, cb, L"%lld", n2);
}

// renderer/subs.js fmtReset: relative countdown to the window reset
// (resetAt is a true UTC epoch ms, as Date.now() is)
static void fmtReset(long long ms, wchar_t *out, int cb) {
    if (ms <= 0) { lstrcpynW(out, L"reset \u2014", cb); return; }
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    long long now = ((((long long)ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000 - 11644473600000LL;
    long long delta = ms - now;
    if (delta <= 0) { lstrcpynW(out, L"reset now", cb); return; }
    long long h = delta / 3600000LL, m = (delta % 3600000LL) / 60000LL, d = h / 24;
    if (d >= 1) swprintf(out, cb, L"reset in %lldd %lldh", d, h % 24);
    else if (h >= 1) swprintf(out, cb, L"reset in %lldh %lldm", h, m);
    else swprintf(out, cb, L"reset in %lldm", m);
}

static void buildChips(void) {
    g_chipCount = 0;
    if (++g_tokensTick >= 30) { g_tokensTick = 0; scanTokenCache(); }
    if (g_tokensToday < 0 && g_tokensTick == 1) scanTokenCache();
    // left pinned group: shortcut, pet, tokens, subs (Electron order)
    if (g_cfg.shortcutEnabled) {
        addChipI(CT_SHORTCUT, 0, g_cfg.shortcutLabel ? g_cfg.shortcutLabel : L"", 0, NULL, 0);
        g_chips[g_chipCount - 1].align = 2;
        g_chips[g_chipCount - 1].iconSvg = SVG_BOLT;
    }
    if (g_cfg.petEnabled) {
        wchar_t txt[16];
        int running = g_m.petRunning;
        lstrcpynW(txt, running ? L"on" : L"off", 16);
        addChipI(CT_PET, 0, txt, 0, running ? g_cfg.good : g_cfg.fgDim, 0);
        g_chips[g_chipCount - 1].align = 2;
        g_chips[g_chipCount - 1].iconSvg = SVG_BOW;
    }
    if (1) { // token chip (reads the Electron cache)
        wchar_t txt[32];
        fmtTokens(g_tokensToday, txt, 32);
        addChipI(CT_CUSTOM, -1, txt, 0, g_cfg.fgDim, 0);
        g_chips[g_chipCount - 1].align = 2;
        g_chips[g_chipCount - 1].iconSvg = SVG_DIAMOND;
        // bar.css #seg-tokens .ico { color: var(--yellow) }
        g_chips[g_chipCount - 1].iconColorOverride = g_cfg.yellow;
    }
    if (g_cfg.subsEnabled) {
        // Electron subs chip: percent with good/dim/warn, "stale" after a
        // failed cycle, em dash when there is no data at all.
        int stale = subsChipStale();
        int rem = subsChipRem();
        wchar_t txt[16];
        const wchar_t *col;
        if (rem == -1) {
            lstrcpynW(txt, L"\u2014", 16);
            col = g_cfg.fgDim;
        } else if (stale) {
            lstrcpynW(txt, L"stale", 16);
            col = g_cfg.fgDim;
        } else {
            swprintf(txt, 16, L"%d%%", rem);
            col = rem > 70 ? g_cfg.good : rem > 30 ? g_cfg.fgDim : g_cfg.warn;
        }
        addChipI(CT_CUSTOM, -1, txt, 0, col, 0);
        g_chips[g_chipCount - 1].align = 2;
        g_chips[g_chipCount - 1].iconSvg = SVG_GAUGE;
    }
    // right metric group: icon + bare value, like the Electron bar
    wchar_t v[48];
    if (g_cfg.mGpu) { swprintf(v, 48, L"%d%%", (int)(g_m.gpu + 0.5)); addChipI(CT_GPU, 0, v, 0, NULL, 0); g_chips[g_chipCount - 1].iconSvg = SVG_GPU; }
    if (g_cfg.mCpu) {
        int hot = g_cfg.cpuWarnAt > 0 && g_m.cpu >= g_cfg.cpuWarnAt;
        swprintf(v, 48, L"%d%%", (int)(g_m.cpu + 0.5));
        addChipI(CT_CPU, 0, v, hot, hot ? g_cfg.warn : NULL, 0); g_chips[g_chipCount - 1].iconSvg = SVG_CPU;
    }
    if (g_cfg.mCpuTemp) {
        if (g_m.tempOk) {
            if ((int)(g_m.tempC * 10) % 10 == 0) swprintf(v, 48, L"%d\u00B0C", (int)g_m.tempC);
            else swprintf(v, 48, L"%.1f\u00B0C", g_m.tempC);
            addChipI(CT_CPUTEMP, 0, v, g_m.tempHot, g_m.tempHot ? g_cfg.warn : NULL, 0xF05C3);
        } else addChipI(CT_CPUTEMP, 0, L"\u2014", 0, g_cfg.divider, 0);
        g_chips[g_chipCount - 1].iconSvg = SVG_TEMP;
    }
    if (g_cfg.mRam) {
        int hot = g_cfg.ramWarnAt > 0 && g_m.ram >= g_cfg.ramWarnAt;
        swprintf(v, 48, L"%d%%", (int)(g_m.ram + 0.5));
        addChipI(CT_RAM, 0, v, hot, hot ? g_cfg.warn : NULL, 0); g_chips[g_chipCount - 1].iconSvg = SVG_RAM;
    }
    if (g_cfg.mVolume) {
        int muted = g_m.volume == 0;
        swprintf(v, 48, L"%d%%", g_m.volume);
        addChipI(CT_VOLUME, 0, v, 0, muted ? g_cfg.fgDim : NULL, 0);
        g_chips[g_chipCount - 1].iconSvg = muted ? SVG_MUTE : SVG_VOL;
    }
    if (g_cfg.mBattery) {
        if (g_m.battValid) {
            int low = g_m.battPct <= 20;
            swprintf(v, 48, L"%d%%", g_m.battPct);
            addChipI(CT_BATTERY, 0, v, low, low ? g_cfg.warn : NULL, 0);
            g_chips[g_chipCount - 1].iconSvg = SVG_BAT; // fill tracks the charge
        } else addChipI(CT_BATTERY, 0, L"AC", 0, g_cfg.fgDim, 0);
    }
    if (g_cfg.mClock) {
        addChipI(CT_CLOCK, 0, g_m.clockText, 0, NULL, 0);
        g_chips[g_chipCount - 1].iconSvg = SVG_CLOCK;
    }
}

// ------------------------------------------------------------- painting ----
static FLOAT textWidth(const wchar_t *s) {
    if (!g_memDc || !g_font) return 0;
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(g_memDc, s, (int)wcslen(s), &sz);
    return (FLOAT)sz.cx;
}

static D2D1_COLOR_F colorFromHex(const wchar_t *hex, FLOAT alpha) {
    int c = hexToColorref(hex);
    if (c < 0) c = 0;
    D2D1_COLOR_F col;
    FLOAT r = (FLOAT)(c & 0xFF) / 255.0f, g = (FLOAT)((c >> 8) & 0xFF) / 255.0f, b = (FLOAT)((c >> 16) & 0xFF) / 255.0f;
    col.r = r * alpha; col.g = g * alpha; col.b = b * alpha; col.a = alpha; // premultiplied
    return col;
}

static int g_inPaint = 0;

static COLORREF colorrefFromHex(const wchar_t *hex, int alpha) {
    int c = hexToColorref(hex);
    if (c < 0) c = 0x000000;
    int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    // premultiplied for UpdateLayeredWindow (AC_SRC_ALPHA)
    return RGB(r * alpha / 255, g * alpha / 255, b * alpha / 255);
}

// glyph + trailing space for a chip icon (surrogate pair when astral)
static void chipIconText(const Chip *c, wchar_t *out) {
    unsigned cp = c->iconCp; int n = 0;
    if (cp < 0x10000) out[n++] = (wchar_t)cp;
    else {
        unsigned v2 = cp - 0x10000;
        out[n++] = (wchar_t)(0xD800 + (v2 >> 10));
        out[n++] = (wchar_t)(0xDC00 + (v2 & 0x3FF));
    }
    out[n++] = L' '; out[n] = 0;
}

// draw the bar into the premultiplied DIB and hand it to DWM (ULW)
extern double g_iconOpacity; // theme.iconOpacity/100 (definition in chocobar.c)

// premultiplied-DIB source-over blend with coverage (0..255): used for the
// rounded bar corners, the hover pill, and any other alpha-shaped paint
static DWORD pxBlend(DWORD dst, DWORD srcPm, int cov) {
    unsigned da = (dst >> 24) & 255, sa = (srcPm >> 24) & 255;
    unsigned outA = (sa > da) ? da + (((sa - da) * (unsigned)cov) >> 8)
                              : da - (((da - sa) * (unsigned)cov) >> 8);
    DWORD out = (outA & 255) << 24;
    for (int sh = 0; sh < 24; sh += 8) {
        int dc = (int)((dst >> sh) & 255), sc = (int)((srcPm >> sh) & 255);
        out |= (DWORD)((dc + ((sc - dc) * cov >> 8)) & 255) << sh;
    }
    return out;
}

// scale a premultiplied pixel's alpha/color by coverage (0..255): used to
// fade the bar tint out at the rounded corners
static DWORD pxScale(DWORD v, int cov) {
    unsigned a = ((((v >> 24) & 255)) * (unsigned)cov) / 255;
    DWORD out = (a & 255) << 24;
    for (int sh = 0; sh < 24; sh += 8)
        out |= (DWORD)(((((v >> sh) & 255)) * (unsigned)cov / 255) & 255) << sh;
    return out;
}

// anti-aliased coverage of one pixel against a rounded rect (radius rad)
static int roundCov(int xx, int yy, int l, int t, int r, int b, int rad) {
    if (rad < 1 || r - l < 2 * rad || b - t < 2 * rad) return 255;
    int lx = l + rad, rx = r - 1 - rad, ty = t + rad, by = b - 1 - rad;
    int dx = 0, dy = 0;
    if (xx < lx) dx = lx - xx; else if (xx > rx) dx = xx - rx;
    if (yy < ty) dy = ty - yy; else if (yy > by) dy = yy - by;
    if (!dx && !dy) return 255;
    float dist = sqrtf((float)dx * dx + (float)dy * dy);
    float f = (float)rad + 0.5f - dist;
    if (f <= 0) return 0;
    if (f >= 1) return 255;
    return (int)(f * 255.0f);
}

static void repaintBar(HWND hwnd) {
    if (!g_memDc || !g_font) return;
    RECT rc; GetClientRect(hwnd, &rc);
    LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 1 || h < 1) return;

    if (w != g_dibW || h != g_dibH || !g_dib) { // (re)size the DIB
        if (g_dib) {
            if (g_dibOld) { SelectObject(g_memDc, g_dibOld); g_dibOld = NULL; }
            DeleteObject(g_dib); g_dib = NULL; g_bits = NULL;
        }
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;      // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        g_dib = CreateDIBSection(g_memDc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!g_dib || !bits) return;
        g_dibOld = SelectObject(g_memDc, g_dib);
        SelectObject(g_memDc, g_font);
        g_bits = bits;
        g_dibW = w; g_dibH = h;
    }

    int bgA = g_cfg.backdrop && lstrcmpiW(g_cfg.backdrop, L"solid") == 0 ? 255 : g_cfg.backgroundAlpha;
    if (bgA < 0) bgA = 0; if (bgA > 255) bgA = 255;
    COLORREF bg = colorrefFromHex(g_cfg.tint, bgA);

    // fill the whole DIB with the premultiplied background (alpha included)
    COLORREF fgCr = colorrefFromHex(g_cfg.fg, 255);
    // DIB 32bpp memory order is B,G,R,A -> little-endian DWORD = A<<24|R<<16|G<<8|B
    DWORD bgPixel = (DWORD)(((DWORD)bgA << 24) | ((DWORD)GetRValue(bg) << 16) | ((DWORD)GetGValue(bg) << 8) | (DWORD)GetBValue(bg));
    DWORD *px = (DWORD *)g_bits;
    size_t total = (size_t)g_dibW * (size_t)g_dibH;
    for (size_t i = 0; i < total; i++) px[i] = bgPixel;

    // rounded bar corners (bar.css: border-radius 8px; corners are CSS, not
    // DWM, because the window is layered): fade the tint to transparent in
    // the four corner boxes
    int rad = (int)(g_cfg.barRadius * g_scale + 0.5); // theme.bar.radius, CSS px
    if (rad > 2 && g_dibW > 2 * rad && g_dibH > 2 * rad) {
        for (int yy = 0; yy < rad; yy++) {
            DWORD *rowT = px + (size_t)yy * g_dibW;
            DWORD *rowB = px + (size_t)(g_dibH - 1 - yy) * g_dibW;
            for (int xx = 0; xx < rad; xx++) {
                int cov = roundCov(xx, yy, 0, 0, g_dibW, g_dibH, rad);
                if (cov < 255) {
                    rowT[xx] = pxScale(rowT[xx], cov);
                    rowT[g_dibW - 1 - xx] = pxScale(rowT[g_dibW - 1 - xx], cov);
                    rowB[xx] = pxScale(rowB[xx], cov);
                    rowB[g_dibW - 1 - xx] = pxScale(rowB[g_dibW - 1 - xx], cov);
                }
            }
        }
    }

    SetBkMode(g_memDc, TRANSPARENT);
    int textH = g_dibH;

    // Electron geometry: bar padding 8px left / 12px right, 14px between
    // segments, 5px between icon and value (CSS px, x2 at this DPI)
    FLOAT hf = (FLOAT)g_dibH;
    FLOAT padL = 8.0f * (FLOAT)g_scale, padR = 12.0f * (FLOAT)g_scale;
    FLOAT segGap = 14.0f * (FLOAT)g_scale, icoGap = 5.0f * (FLOAT)g_scale;
    int iconW[MAX_CHIPS];
    int svgBox = (int)(12 * g_scale + 0.5);
    for (int i = 0; i < g_chipCount; i++) {
        iconW[i] = 0;
        if (g_chips[i].iconSvg >= 0) iconW[i] = svgBox;
        else if (g_chips[i].iconCp) {
            wchar_t ico[8];
            chipIconText(&g_chips[i], ico);
            iconW[i] = textWidth(ico);
        }
    }
    // right group grows leftward from the right edge
    FLOAT xr = (FLOAT)g_dibW - padR;
    for (int i = g_chipCount - 1; i >= 0; i--) {
        Chip *c = &g_chips[i];
        if (c->align != 1) continue;
        FLOAT cw = (FLOAT)(iconW[i] + (iconW[i] ? (int)icoGap : 0)) + textWidth(c->text);
        c->r.right = (LONG)xr;
        c->r.left = (LONG)(xr - cw);
        c->r.top = 0; c->r.bottom = (LONG)hf;
        xr -= cw + segGap;
    }
    // left pinned group grows rightward from the left edge
    FLOAT xl = padL;
    for (int i = 0; i < g_chipCount; i++) {
        Chip *c = &g_chips[i];
        if (c->align != 2) continue;
        FLOAT cw = (FLOAT)(iconW[i] + (iconW[i] ? (int)icoGap : 0)) + textWidth(c->text);
        c->r.left = (LONG)xl;
        c->r.right = (LONG)(xl + cw);
        c->r.top = 0; c->r.bottom = (LONG)hf;
        xl += cw + segGap;
    }

    // hover pill (bar.css .seg.clickable:hover): pink at 50% over the tint,
    // rounded 5px, inflated 5px horizontally / 2px vertically beyond the chip.
    // Only the pinned (clickable) chips get it - the metric segs are hover-
    // inert in the renderer.
    int pillRad = (int)(5 * g_scale + 0.5);
    int pillPadX = (int)(5 * g_scale + 0.5), pillPadY = (int)(2 * g_scale + 0.5);
    COLORREF pinkHover = colorrefFromHex(g_cfg.pink, 255);
    DWORD pillPm = ((DWORD)128 << 24) | ((DWORD)(GetRValue(pinkHover) * 128 / 255) << 16)
                 | ((DWORD)(GetGValue(pinkHover) * 128 / 255) << 8)
                 | (DWORD)(GetBValue(pinkHover) * 128 / 255);
    for (int i = 0; i < g_chipCount; i++) {
        Chip *c = &g_chips[i];
        if (i == g_hover && c->align == 2) {
            int pl = c->r.left - pillPadX, pt = c->r.top + pillPadY;
            int prr = c->r.right + pillPadX, pb = c->r.bottom - pillPadY;
            if (pl < 0) pl = 0; if (pt < 0) pt = 0;
            if (prr > g_dibW) prr = g_dibW; if (pb > g_dibH) pb = g_dibH;
            for (int yy = pt; yy < pb; yy++) {
                DWORD *row = px + (size_t)yy * g_dibW;
                for (int xx = pl; xx < prr; xx++) {
                    int cov = roundCov(xx, yy, pl, pt, prr, pb, pillRad);
                    if (cov > 0) row[xx] = pxBlend(row[xx], pillPm, cov);
                }
            }
        }
        const wchar_t *col = c->colorOverride;
        if (c->warn) col = g_cfg.warn;
        COLORREF vc = col ? colorrefFromHex(col, 255) : fgCr;
        // #seg-tokens:hover .val -> the value turns pinkDeep on hover
        if (i == g_hover && c->align == 2 && c->iconSvg == SVG_DIAMOND) vc = colorrefFromHex(g_cfg.pinkDeep, 255);
        int tx = c->r.left;
        SetTextColor(g_memDc, vc);
        if (c->iconSvg >= 0) {
            // single-color icon set; per-chip override, else theme.iconColor,
            // else pinkDeep (bar.css #seg-tokens .ico = yellow)
            const wchar_t *icol = (c->iconColorOverride && *c->iconColorOverride)
                ? c->iconColorOverride
                : ((g_cfg.iconColor && *g_cfg.iconColor) ? g_cfg.iconColor : g_cfg.pinkDeep);
            COLORREF acc = colorrefFromHex(icol, 255);
            int iy = (g_dibH - svgBox) / 2;
            if (c->iconSvg == SVG_BAT)
                svgDrawBatt(g_memDc, g_m.battPct, g_m.battAc, acc, colorrefFromHex(g_cfg.warn, 255),
                            colorrefFromHex(g_cfg.tint, 255), tx, iy);
            else
                svgDraw(g_memDc, c->iconSvg, acc, tx, iy);
            tx += iconW[i] + (int)icoGap;
        } else if (c->iconCp) {
            wchar_t ico[8];
            chipIconText(c, ico);
            const wchar_t *icol2 = (g_cfg.iconColor && *g_cfg.iconColor) ? g_cfg.iconColor : g_cfg.pinkDeep;
            COLORREF ic = colorrefFromHex(icol2, 255);
            SetTextColor(g_memDc, ic);
            RECT ir = { tx, 0, tx + iconW[i] + 8, textH };
            DrawTextW(g_memDc, ico, -1, &ir, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            tx += iconW[i] + (int)icoGap;
            SetTextColor(g_memDc, vc);
        }
        RECT tr = { tx, 0, c->r.right, textH };
        DrawTextW(g_memDc, c->text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }

    // GDI leaves ALPHA=0 in the reserved byte of every pixel it touches
    // (text, icons, hover fill): DWM would composite those pixels as fully
    // transparent - the bar shows tint but no content. Any pixel GDI drew
    // is a content pixel: make it opaque. (Background pixels keep bgA.)
    for (size_t i = 0; i < total; i++) {
        DWORD v = px[i];
        if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0) px[i] = v | 0xFF000000u;
    }

    // hand the buffer to DWM; keeps the current window position
    POINT ptSrc = {0, 0};
    SIZE sz = { g_dibW, g_dibH };
    BLENDFUNCTION bl = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (!UpdateLayeredWindow(hwnd, NULL, NULL, &sz, g_memDc, &ptSrc, 0, &bl, ULW_ALPHA)) {
        char _b[64];
        sprintf(_b, "ULW fail %08lX", (unsigned long)GetLastError());
        writeLogA(_b);
    }
}

static int g_paintHooked = 0;
static void paint(HWND hwnd) { repaintBar(hwnd); (void)g_paintHooked; }


static int wikilessContains(const wchar_t *hay, const wchar_t *needle) {
    if (!hay || !needle || !*needle) return 1;
    int nl = lstrlenW(needle);
    int hl = lstrlenW(hay);
    for (int i = 0; i + nl <= hl; i++) {
        if (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, hay + i, nl, needle, nl) == CSTR_EQUAL) return 1;
    }
    return 0;
}

static int isTerminalHwnd(HWND h) {
    if (!h || h == g_bar) return 0;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return 0;
    int classOk;
    if (g_cfg.terminalClassName && *g_cfg.terminalClassName) classOk = !lstrcmpiW(cls, g_cfg.terminalClassName);
    else classOk = !lstrcmpiW(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") ||
                   !lstrcmpiW(cls, L"ConsoleWindowClass") ||
                   !lstrcmpiW(cls, L"VirtualConsoleClass") ||
                   !lstrcmpiW(cls, L"mintty");
    if (!classOk) return 0;
    // configured title substring must match too (else any same-class window wins)
    if (g_cfg.terminalTitle && *g_cfg.terminalTitle) {
        wchar_t title[128];
        if (!GetWindowTextW(h, title, 128)) return 0;
        if (!wikilessContains(title, g_cfg.terminalTitle)) return 0;
    }
    return 1;
}

static HWND findTerminalByProbe(void) {
    // A configured className is AUTHORITATIVE: probe only that class and
    // never fall back to the generic list (otherwise a no-match config like
    // "don't follow anything" would silently attach to the first wt found).
    if (g_cfg.terminalClassName && *g_cfg.terminalClassName) {
        HWND h = FindWindowW(g_cfg.terminalClassName, NULL);
        if (h && isTerminalHwnd(h)) return h;
        // class+title: enumerate (FindWindowW only returns the first match)
        if (g_cfg.terminalTitle && *g_cfg.terminalTitle) {
            h = NULL;
            for (;;) {
                h = FindWindowExW(NULL, h, g_cfg.terminalClassName, NULL);
                if (!h) break;
                if (isTerminalHwnd(h)) return h;
            }
        }
        return NULL;
    }
    static const wchar_t *const classes[] = {
        L"CASCADIA_HOSTING_WINDOW_CLASS", L"ConsoleWindowClass",
        L"VirtualConsoleClass", L"mintty"
    };
    for (int i = 0; i < 4; i++) {
        HWND h = FindWindowW(classes[i], NULL);
        if (h && h != g_bar) return h;
    }
    return NULL;
}

static void hideBar(void) {
    if (g_barVisible && g_bar) { ShowWindow(g_bar, SW_HIDE); g_barVisible = 0; }
}

static void execCmd(const wchar_t *cmd) {
    if (!cmd || !*cmd) return;
    wchar_t params[1200];
    lstrcpynW(params, L"/c ", 1200);
    lstrcatW(params, cmd);
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    ShellExecuteExW(&sei);
}

static void petToggle(void) {
    if (!g_cfg.petExePath || !*g_cfg.petExePath) return;
    if (g_m.petRunning) {
        wchar_t base[MAX_PATH];
        lstrcpynW(base, pathBaseName(g_cfg.petExePath), MAX_PATH);
        wchar_t cmd[MAX_PATH + 32];
        swprintf(cmd, MAX_PATH + 31, L"/IM %ls /F", base);
        SHELLEXECUTEINFOW sei;
        memset(&sei, 0, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"open";
        sei.lpFile = L"taskkill.exe";
        sei.lpParameters = cmd;
        sei.nShow = SW_HIDE;
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        ShellExecuteExW(&sei);
    } else {
        SHELLEXECUTEINFOW sei;
        memset(&sei, 0, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"open";
        sei.lpFile = g_cfg.petExePath;
        sei.nShow = SW_SHOWNORMAL;
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        ShellExecuteExW(&sei);
    }
    g_m.petRunning = !g_m.petRunning;
}



static void followTick(void) {
    // STICKY like the Electron build's tracker: attach once, keep the terminal
    // until it dies, only then re-probe (foreground-wins). Re-targeting on
    // every foreground switch would overlap a second bar tracking another
    // terminal and yank the bar around while the user works in other apps.
    if (g_term && IsWindow(g_term)) { /* keep */ }
    else {
        HWND fg = GetForegroundWindow();
        if (isTerminalHwnd(fg)) g_term = fg;
        else if (!g_term) g_term = findTerminalByProbe();
    }
    if (!g_term) { hideBar(); return; }
    if (!IsWindow(g_term)) { g_term = NULL; g_haveTarget = 0; return; }

    // hands-off during the terminal's modal move/size loop
    GUITHREADINFO gi; memset(&gi, 0, sizeof(gi)); gi.cbSize = sizeof(gi);
    DWORD tid = GetWindowThreadProcessId(g_term, NULL);
    static DWORD s_lastMoveSync = 0;
    if (GetGUIThreadInfo(tid, &gi) && (gi.flags & GUI_INMOVESIZE)) {
        // Follow LIVE during border resizes: a full-rate SetWindowPos flood
        // starves the modal loop (the old freeze-until-release), but a light
        // 120ms throttle stays instant-feeling without starving anything.
        DWORD now = GetTickCount();
        if (now - s_lastMoveSync < 120) return;
        s_lastMoveSync = now;
    }

    RECT ef;
    if (DwmGetWindowAttribute(g_term, DWMWA_EXTENDED_FRAME_BOUNDS, &ef, sizeof(ef)) != S_OK)
        GetWindowRect(g_term, &ef);
    HMONITOR mon = MonitorFromWindow(g_term, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    int bh = (int)(g_cfg.height * g_scale + 0.5);
    int gap = (int)(g_cfg.gap * g_scale + 0.5);
    int y = ef.top - bh - gap;
    if (y < mi.rcWork.top || bh > mi.rcWork.bottom - mi.rcWork.top) { hideBar(); return; }

    RECT target = { ef.left, y, ef.left + (ef.right - ef.left), y + bh };
    if (!g_haveTarget || memcmp(&target, &g_lastTarget, sizeof(RECT)) != 0 || !g_barVisible) {
        g_lastTarget = target;
        g_haveTarget = 1;
        // insertAfter semantics: the bar sits DIRECTLY BELOW the terminal in z
        SetWindowPos(g_bar, HWND_TOPMOST, target.left, target.top, // keep always-on-top (Electron 'floating'); inserting after a normal window would clear it
                     target.right - target.left, target.bottom - target.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_barVisible = 1;
        InvalidateRect(g_bar, NULL, FALSE);
        UpdateWindow(g_bar); // force immediate WM_PAINT dispatch
    }
}

static void uiFontFamily(wchar_t *fam, int cb); // defined with the dashboard fonts

static void configCheckTick(void) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_cfgMtime) != 0) {
        g_cfgMtime = fa.ftLastWriteTime;
        loadConfig();
        // font may change with the config: rebuild it
        if (g_font) { SelectObject(g_memDc, g_fontOld); DeleteObject(g_font); g_font = NULL; }
        wchar_t fam[64];
        uiFontFamily(fam, 64);
        HFONT nf = CreateFontW(-(int)(g_cfg.fontSize * g_scale + 0.5), 0, 0, 0, FW_NORMAL,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, fam[0] ? fam : L"Segoe UI");
        if (nf) { g_font = nf; g_fontOld = (HFONT)SelectObject(g_memDc, g_font); }
        clockFmtReload();
        followTick();
        InvalidateRect(g_bar, NULL, FALSE);
    }
}

// ---- dashboard popups (token usage + subscriptions) ------------------------
// Electron opens frameless BrowserWindows (840x580 / 820x480 CSS, opaque
// pinkBg, Win11-rounded by DWM). The native panels mirror that: WS_POPUP,
// DWM-rounded corners, one panel at a time, chip click toggles.
static HWND g_dash = NULL;
static int g_dashType = 0;          // 0 = token usage, 1 = subs board
static void *g_dashBits = NULL;
static HBITMAP g_dashDib = NULL;
static HDC g_dashDc = NULL;
static HGDIOBJ g_dashOldBmp = NULL;
static LONG g_dashW = 0, g_dashH = 0;
static int g_dashPainted = -1;      // which type the current DIB shows

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#define DWMWCP_ROUND_NATIVE 2

static void dashRoundCorners(HWND h) {
    typedef HRESULT (WINAPI *PFN)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE m = GetModuleHandleW(L"dwmapi.dll");
    if (!m) m = LoadLibraryW(L"dwmapi.dll");
    if (!m) return;
    PFN f = (PFN)(void *)GetProcAddress(m, "DwmSetWindowAttribute");
    if (f) { DWORD pref = DWMWCP_ROUND_NATIVE; f(h, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref)); }
}

static void uiFontFamily(wchar_t *fam, int cb) {
    const wchar_t *src = g_cfg.fontFamily;
    int n = 0;
    while (src && *src && *src != L'\'' && *src != L'-' && !iswalpha(*src)) src++;
    if (src && *src == L'\'') {
        src++;
        while (src[n] && src[n] != L'\'' && n < cb - 1) { fam[n] = src[n]; n++; }
    } else {
        while (src && src[n] && src[n] != L',' && n < cb - 1) { fam[n] = src[n]; n++; }
    }
    fam[n] = 0;
    if (!fam[0]) lstrcpynW(fam, L"Segoe UI", cb);
}

static HFONT dashFont(int cssPx, int weight) {
    wchar_t fam[64];
    uiFontFamily(fam, 64);
    return CreateFontW(-(int)(cssPx * g_scale + 0.5), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, fam);
}

// ---- dashboard (ChocobarDash) ----------------------------------------------
// Pixel-parity port of the Electron dashboard/subscriptions windows
// (renderer/dash.css, subs.css). Layout constants are CSS px scaled by g_scale.
// Anti-aliased cards, donut pies and share bars come from the GDI+ facade in
// p_icons.c; text is GDI ClearType (the family has no true semibold: CSS 600
// renders as the Regular face in Chromium too).

static void tipHide(void);
static void tipShow(const wchar_t *text, int cx, int cy, int dark);

static const wchar_t *DASH_MONTHS[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                          L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
static const wchar_t *DASH_DAYS[7] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };

// per-paint theme resolution
typedef struct {
    COLORREF bg, card, head, zebra, fg, dim, divider, pinkDeep, pink, warn, good, yellow;
    COLORREF ramp[5];
} DashTheme;

#define DX(v) ((int)((v) * g_scale + 0.5))

static void dashResolveTheme(DashTheme *t) {
    t->bg = colorrefFromHex(g_cfg.pinkBg, 255);
    t->fg = colorrefFromHex(g_cfg.fg, 255);
    t->dim = colorrefFromHex(g_cfg.fgDim, 255);
    t->divider = colorrefFromHex(g_cfg.divider, 255);
    t->pinkDeep = colorrefFromHex(g_cfg.pinkDeep, 255);
    t->pink = colorrefFromHex(g_cfg.pink, 255);
    t->warn = colorrefFromHex(g_cfg.warn, 255);
    t->good = colorrefFromHex(g_cfg.good, 255);
    t->yellow = colorrefFromHex(g_cfg.yellow, 255);
    t->card = blendCr(t->bg, RGB(255, 255, 255), 64);  // rgba(255,255,255,.25)
    t->head = blendCr(t->card, RGB(255, 255, 255), 64); // stacked head tint
    t->zebra = blendCr(t->card, t->bg, 128);            // rgba(pinkBg,.5)
    for (int i = 0; i < 5; i++) t->ramp[i] = colorrefFromHex(g_cfg.heatmap[i], 255);
}

// GDI rounded-rect fallback (gdiplus.dll or any export missing): radius in device px
static void dashRoundRectGDI(HDC dc, int x, int y, int w, int h, int r, COLORREF fill, COLORREF border) {
    HBRUSH b = fill ? CreateSolidBrush(fill) : NULL; // fill 0 = border-only ring
    LOGBRUSH lb1; lb1.lbStyle = BS_SOLID; lb1.lbColor = border; lb1.lbHatch = 0;
    HPEN pen = border ? ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 1, &lb1, 0, NULL)
                      : (HPEN)GetStockObject(NULL_PEN); // border 0 = no outline (GDI+ parity)
    HGDIOBJ ob = SelectObject(dc, b ? b : GetStockObject(NULL_BRUSH)), op = SelectObject(dc, pen);
    RoundRect(dc, x, y, x + w, y + h, r * 2, r * 2);
    SelectObject(dc, ob); SelectObject(dc, op);
    if (b) DeleteObject(b);
    if (border) DeleteObject(pen);
}

// anti-aliased rounded card (GDI+; GDI RoundRect fallback)
static void dashCard(HDC dc, int x, int y, int w, int h, COLORREF fill, COLORREF border) {
    if (g_gdipOk && w > 4 && h > 4) {
        gdipRoundRect(dc, fill, border, x, y, w, h, DX(10));
        return;
    }
    dashRoundRectGDI(dc, x, y, w, h, DX(10), fill, border);
}

static void dashStr(HDC dc, int x, int y, const wchar_t *s, COLORREF cr, HFONT f) {
    SelectObject(dc, f);
    SetTextColor(dc, cr);
    RECT r = { x, y, x + 8000, y + DX(40) };
    DrawTextW(dc, s, -1, &r, DT_SINGLELINE | DT_LEFT);
}
static void dashStrR(HDC dc, int xRight, int y, const wchar_t *s, COLORREF cr, HFONT f) {
    SelectObject(dc, f);
    SetTextColor(dc, cr);
    RECT r = { xRight - 4000, y, xRight, y + DX(40) };
    DrawTextW(dc, s, -1, &r, DT_SINGLELINE | DT_RIGHT);
}
static int dashStrW(HDC dc, const wchar_t *s, HFONT f) {
    HGDIOBJ of = SelectObject(dc, f);
    SIZE ts; GetTextExtentPoint32W(dc, s, lstrlenW(s), &ts);
    SelectObject(dc, of);
    return ts.cx;
}
// 11px bold uppercase section head with 0.06em letter spacing
static void dashHead(HDC dc, int x, int y, const wchar_t *s, COLORREF cr, HFONT f) {
    SelectObject(dc, f);
    SetTextColor(dc, cr);
    SetTextCharacterExtra(dc, DX(0.7));
    RECT r = { x, y, x + 4000, y + DX(30) };
    DrawTextW(dc, s, -1, &r, DT_SINGLELINE | DT_LEFT);
    SetTextCharacterExtra(dc, 0);
}

// app dot color table (dash.css .app-*): EXACT names, like Electron's
// app-${app} class - any other harness falls through to the default dim dot
static COLORREF appDotColor(const char *app, DashTheme *t) {
    if (!strcmp(app, "zcode")) return t->pinkDeep;
    if (!strcmp(app, "zai")) return t->yellow;
    if (!strcmp(app, "opencode")) return RGB(0x7F, 0xA8, 0xA0);
    if (!strcmp(app, "mimo")) return RGB(0xB7, 0x9C, 0xE0);
    if (!strcmp(app, "pi")) return RGB(0x8F, 0xA8, 0xD4);
    return blendCr(t->card, t->dim, 140);
}
static void dashDot(HDC dc, int cx, int cy, int d, COLORREF cr) {
    if (g_gdipOk) {
        GpGraphics *g = NULL;
        if (t_GdipCreateFromHDC(dc, &g) == 0) {
            t_GdipSetSmoothingMode(g, 6);
            GpBrush *br = NULL;
            if (t_GdipCreateSolidFill(GDIP_ARGB(cr), &br) == 0) {
                t_GdipFillEllipse(g, br, (float)cx, (float)cy, (float)d, (float)d);
                t_GdipDeleteBrush(br);
            }
            t_GdipDeleteGraphics(g);
        }
        return;
    }
    HBRUSH b = CreateSolidBrush(cr);
    LOGBRUSH lb1; lb1.lbStyle = BS_SOLID; lb1.lbColor = cr; lb1.lbHatch = 0;
    HPEN pen = ExtCreatePen(PS_GEOMETRIC, 1, &lb1, 0, NULL);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    Ellipse(dc, cx, cy, cx + d, cy + d);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(pen);
}

// heatmap hit rects (physical px) + the days-back each holds
static RECT g_hmRect[26][7];
static int g_hmBack[26][7];
static RECT g_btnRefresh, g_btnClose; // physical px
static int g_btnHover = 0;            // 0 none, 1 refresh, 2 close
static int g_tbBottom = 0;            // titlebar bottom (physical px) for drag

// table column x positions (right-aligned edge offsets, CSS px)
#define COL_CALLS 36
#define COL_CW 56
#define COL_CR 56
#define COL_OUT 56
#define COL_IN 56

static void dashTableCols(int innerRight, int cache, int *xs) {
    // xs[0]=calls, xs[1]=cacheW, xs[2]=cacheR, xs[3]=output, xs[4]=input (right edges)
    xs[0] = innerRight;
    if (cache) {
        xs[1] = xs[0] - DX(COL_CALLS);
        xs[2] = xs[1] - DX(COL_CW);
        xs[3] = xs[2] - DX(COL_CR);
        xs[4] = xs[3] - DX(COL_OUT);
    } else {
        // no cache data: collapse only the cache R/W columns; input and
        // output stay on screen (calls remain rightmost)
        xs[3] = xs[0] - DX(COL_CALLS);
        xs[4] = xs[3] - DX(COL_OUT);
        xs[1] = xs[2] = 0;
    }
}

// one table row: share bar behind the first cell, dot, numbers right-aligned
static void dashTableRow(HDC dc, int x0, int innerW, int y, int rowH, int cache,
                         const int *xs, const wchar_t *label, COLORREF dot,
                         double share, TokAgg *a, DashTheme *t, HFONT f11, HFONT f9) {
    // zebra-less; share bar behind the label (dash.css .share i)
    if (share > 0.003) {
        int bw = (int)(share * innerW);
        if (bw < DX(3)) bw = DX(3);
        int bh = (int)(rowH * 0.7);
        int by = y + (rowH - bh) / 2;
        COLORREF c1 = blendCr(t->card, t->pink, 90);     // pink @35%
        COLORREF c2 = blendCr(t->card, t->pinkDeep, 90); // pinkDeep @35%
        if (g_gdipOk && bw > 4) {
            // horizontal gradient endpoints pre-blended (matches the CSS gradient)
            GRADIENT_RECT gr = { 0, 1 };
            TRIVERTEX tv[2];
            memset(tv, 0, sizeof(tv));
            tv[0].x = x0 + 2; tv[0].y = by;
            tv[0].Red = GetRValue(c1) << 8; tv[0].Green = GetGValue(c1) << 8; tv[0].Blue = GetBValue(c1) << 8; tv[0].Alpha = 0xFF00;
            tv[1].x = x0 + 2 + bw; tv[1].y = by + bh;
            tv[1].Red = GetRValue(c2) << 8; tv[1].Green = GetGValue(c2) << 8; tv[1].Blue = GetBValue(c2) << 8; tv[1].Alpha = 0xFF00;
            GradientFill(dc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_H);
        } else {
            HBRUSH b = CreateSolidBrush(c1);
            RECT fr = { x0 + 2, by, x0 + 2 + bw, by + bh };
            FillRect(dc, &fr, b);
            DeleteObject(b);
        }
    }
    dashDot(dc, x0 + 2, y + (rowH - DX(8)) / 2, DX(8), dot);
    SelectObject(dc, f11);
    SetTextColor(dc, t->fg);
    RECT lr = { x0 + 2 + DX(14), y, x0 + DX(150), y + rowH + 1 };
    DrawTextW(dc, label, -1, &lr, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
    wchar_t vs[32];
    fmtTokens(a->in, vs, 32); dashStrR(dc, xs[4], y, vs, t->fg, f11);
    fmtTokens(a->out, vs, 32); dashStrR(dc, xs[3], y, vs, t->fg, f11);
    if (cache) {
        fmtTokens(a->cr, vs, 32); dashStrR(dc, xs[2], y, vs, t->fg, f11);
        fmtTokens(a->cw, vs, 32); dashStrR(dc, xs[1], y, vs, t->fg, f11);
    }
    swprintf(vs, 32, L"%lld", a->req); dashStrR(dc, xs[0], y, vs, t->fg, f11);
}

static void dashTableHead(HDC dc, int x0, int innerW, int y, int cache,
                          const int *xs, DashTheme *t, HFONT f9, const wchar_t *firstCol) {
    dashStr(dc, x0 + 2, y, firstCol, t->dim, f9);
    wchar_t *in = L"INPUT", *out = L"OUTPUT", *cr = L"CACHE R", *cw = L"CACHE W", *ca = L"CALLS";
    dashStrR(dc, xs[4], y, in, t->dim, f9);
    dashStrR(dc, xs[3], y, out, t->dim, f9);
    if (cache) {
        dashStrR(dc, xs[2], y, cr, t->dim, f9);
        dashStrR(dc, xs[1], y, cw, t->dim, f9);
    }
    dashStrR(dc, xs[0], y, ca, t->dim, f9);
    (void)innerW;
}

// format a FILETIME (already local) as HH:MM:SS
static void dashFmtTime(long long msUtc, wchar_t *out, int cb) {
    if (msUtc < 0) { lstrcpynW(out, L"\x2014", cb); return; }
    FILETIME ft;
    long long v = msUtc * 10000LL;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    ft.dwLowDateTime = (DWORD)v;
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) { lstrcpynW(out, L"\x2014", cb); return; }
    swprintf(out, cb, L"%02lu:%02lu:%02lu", (unsigned long)st.wHour, (unsigned long)st.wMinute, (unsigned long)st.wSecond);
}

// current local wall-clock as an epoch-ms value that dashFmtTime (which
// reads a UTC FILETIME) renders back as local time-of-day
static long long dashLocalNowMs(void) {
    SYSTEMTIME now; GetLocalTime(&now);
    FILETIME ft;
    SystemTimeToFileTime(&now, &ft); // treats fields as UTC: matches dashFmtTime
    return ((((long long)ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000;
}

// day date for a heatmap column start (daysBack of the dow=0 cell)
static void dashColDate(int daysBack, SYSTEMTIME *out) {
    SYSTEMTIME now; GetLocalTime(&now);
    now.wHour = now.wMinute = now.wSecond = now.wMilliseconds = 0;
    FILETIME ft;
    SystemTimeToFileTime(&now, &ft); // stays in the local frame: FileTimeToSystemTime reads it back as local
    long long v = ((((long long)ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - (long long)daysBack * 864000000000LL;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    ft.dwLowDateTime = (DWORD)v;
    FileTimeToSystemTime(&ft, out);
}

static void paintDash(HWND hwnd) {
    if (!g_dashDc) {
        g_dashDc = CreateCompatibleDC(NULL);
        g_dashOldBmp = NULL;
    }
    RECT rc; GetClientRect(hwnd, &rc);
    LONG w = rc.right, h = rc.bottom;
    if (w < 1 || h < 1) return;
    if (w != g_dashW || h != g_dashH || !g_dashDib || g_dashPainted != g_dashType) {
        if (g_dashDib) { SelectObject(g_dashDc, g_dashOldBmp); DeleteObject(g_dashDib); g_dashDib = NULL; g_dashBits = NULL; }
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        g_dashDib = CreateDIBSection(g_dashDc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!g_dashDib || !bits) return;
        g_dashOldBmp = SelectObject(g_dashDc, g_dashDib);
        g_dashBits = bits;
        g_dashW = w; g_dashH = h;
        g_dashPainted = g_dashType;
    }
    HDC dc = g_dashDc;
    DashTheme t;
    dashResolveTheme(&t);
    // opaque panel background (pinkBg)
    {
        HBRUSH b = CreateSolidBrush(t.bg);
        RECT fr = { 0, 0, w, h };
        FillRect(dc, &fr, b);
        DeleteObject(b);
    }
    SetBkMode(dc, TRANSPARENT);
    HFONT fTitle = dashFont(15, FW_BOLD);
    HFONT fBody = dashFont(11, FW_NORMAL);
    HFONT fVal = dashFont(17, FW_BOLD);
    HFONT fS10 = dashFont(10, FW_NORMAL);
    HFONT fS9 = dashFont(9, FW_NORMAL);
    HFONT fBtn = dashFont(11, FW_NORMAL);
    HFONT fHead = dashFont(11, FW_BOLD);
    HFONT f13 = dashFont(13, FW_BOLD);
    HFONT f18 = dashFont(18, FW_BOLD);
    // CSS-700 elements that share a size with a CSS-600 one need their own
    // bold handle (Chromium maps 600 to Regular for Cascadia Mono)
    HFONT fPlan = dashFont(11, FW_BOLD);   // .plan-name, #day-detail .d-title
    HFONT fS10b = dashFont(10, FW_BOLD);   // .pill

    int padL = DX(18), padT = DX(14), gap = DX(12);
    COLORREF white = RGB(255, 255, 255);

    // ---- titlebar: logo + title, refresh/close buttons, hairline divider
    int ty = padT;
    {
        int logoBox = DX(14);
        const wchar_t *icol = (g_cfg.iconColor && *g_cfg.iconColor) ? g_cfg.iconColor : g_cfg.pinkDeep;
        COLORREF acc = colorrefFromHex(icol, 255);
        svgDraw(dc, g_dashType == 0 ? SVG_DIAMOND : SVG_GAUGE, acc, padL, ty + (DX(20) - logoBox) / 2);
        dashStr(dc, padL + logoBox + DX(10), ty, g_dashType == 0 ? L"Token Usage" : L"Subscriptions", t.fg, fTitle);
        // buttons right: [refresh] [close]
        int bx = w - padL;
        const wchar_t *cl = L"\x2715";
        int cw = dashStrW(dc, cl, fBtn) + DX(18);
        g_btnClose.right = bx; g_btnClose.left = bx - cw;
        g_btnClose.top = ty - DX(3); g_btnClose.bottom = ty + DX(11) + DX(6);
        bx -= cw + DX(6);
        const wchar_t *rf = L"refresh";
        int rw = dashStrW(dc, rf, fBtn) + DX(18);
        g_btnRefresh.right = bx; g_btnRefresh.left = bx - rw;
        g_btnRefresh.top = ty - DX(3); g_btnRefresh.bottom = ty + DX(11) + DX(6);
        bx -= rw + DX(6);
        for (int bi2 = 1; bi2 <= 2; bi2++) {
            RECT *br = bi2 == 1 ? &g_btnRefresh : &g_btnClose;
            const wchar_t *tx = bi2 == 1 ? rf : cl;
            if (g_btnHover == bi2) {
                dashCard(dc, br->left, br->top, br->right - br->left, br->bottom - br->top, t.bg, t.pink);
                dashStr(dc, br->left + DX(9), br->top + DX(3), tx, t.pinkDeep, fBtn);
            } else {
                dashStr(dc, br->left + DX(9), br->top + DX(3), tx, t.dim, fBtn);
            }
        }
        g_tbBottom = g_btnRefresh.bottom + DX(10) + 1;
        HPEN pen = CreatePen(PS_SOLID, 1, t.divider);
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, padL, g_tbBottom, NULL);
        LineTo(dc, w - padL, g_tbBottom);
        SelectObject(dc, op);
        DeleteObject(pen);
    }

    int y = g_tbBottom + gap;

    if (g_dashType == 0) {
        // ---- token usage panel
        int innerW = w - 2 * padL;
        if (g_tokensToday < 0 && g_tokAll == 0) {
            dashCard(dc, padL, y, innerW, DX(38), t.card, t.divider);
            dashStr(dc, padL + DX(14), y + DX(10), L"No token data. The token cache file could not be read \x2014 start Chocobar, or check tokens.cachePath.", t.dim, fBody);
        } else {
            // stat cards: Today / Last 7 / Last 30 / All time
            int cw2 = (innerW - 3 * DX(10)) / 4;
            int chh = DX(52);
            struct { const wchar_t *label; long long v; } cards[4] = {
                { L"Today", g_tokensToday < 0 ? 0 : g_tokensToday },
                { L"Last 7 days", g_tokWeek },
                { L"Last 30 days", g_tokMonth },
                { L"All time", g_tokAll } };
            for (int i = 0; i < 4; i++) {
                int cx = padL + i * (cw2 + DX(10));
                dashCard(dc, cx, y, cw2, chh, t.card, t.divider);
                dashStr(dc, cx + DX(12), y + DX(8), cards[i].label, t.dim, fS10);
                wchar_t vs[32];
                fmtTokens(cards[i].v, vs, 32);
                dashStr(dc, cx + DX(12), y + DX(8) + DX(13) + DX(3), vs, t.fg, fVal);
            }
            y += chh + gap;

            // plan usage section (when subs providers report windows)
            int planH = 0;
            int provIdx[MAX_SUBS];
            int provN = 0;
            int pn = g_cfg.subsProviderCount; if (pn > MAX_SUBS) pn = MAX_SUBS;
            for (int i = 0; i < pn; i++) {
                if (!g_cfg.subsEnabled || !subsProvEnabled(i)) continue;
                SubsWin tmp[4];
                int wn = subsProvWins(i, tmp, 4);
                if (wn < 0) wn = -wn;
                if (wn > 0) { // any reported window: absolute totals OR percent-only
                    provIdx[provN] = i;
                    provN++;
                }
            }
            if (provN > 0) {
                int secPadX = DX(12);
                int headH = DX(11) + DX(8);
                // dash.css .plan-row: first row 32 CSS px, every following row
                // +17 (dashed border-top + margin/padding) = 49
                planH = DX(10) + headH + DX(32) + (provN - 1) * DX(49) + DX(4) + DX(10);
                if (y + planH < h - DX(30)) {
                    dashCard(dc, padL, y, innerW, planH, t.card, t.divider);
                    dashHead(dc, padL + secPadX, y + DX(10), L"PLAN USAGE", t.dim, fHead);
                    int ry = y + DX(10) + headH;
                    for (int i = 0; i < provN; i++) {
                        // one advance per row: first DX(32), every following row
                        // DX(49) total, the dashed divider sitting at the row's
                        // top edge inside that pitch
                        int cy = ry + (i > 0 ? DX(16) : 0);
                        if (i > 0) {
                            // .plan-row + .plan-row: 1px dashed divider
                            HPEN dp = CreatePen(PS_DOT, 1, t.divider);
                            HGDIOBJ od = SelectObject(dc, dp);
                            MoveToEx(dc, padL + secPadX, ry, NULL);
                            LineTo(dc, padL + innerW - secPadX, ry);
                            SelectObject(dc, od);
                            DeleteObject(dp);
                        }
                        wchar_t label[48];
                        subsProvLabel(provIdx[i], label, 48);
                        SubsWin tmp[4];
                        int wn = subsProvWins(provIdx[i], tmp, 4);
                        if (wn < 0) wn = -wn;
                        long long used = 0, tot = 0;
                        int maxPct = 0;
                        for (int k = 0; k < wn; k++) {
                            if (tmp[k].used > 0) used += tmp[k].used;
                            if (tmp[k].total > 0) tot += tmp[k].total;
                            else if (tmp[k].pct > maxPct) maxPct = tmp[k].pct;
                        }
                        int pct = tot > 0 ? (int)((double)used / tot * 100.0 + 0.5) : maxPct;
                        if (pct > 100) pct = 100;
                        SelectObject(dc, fPlan);
                        dashStr(dc, padL + secPadX, cy, label, t.fg, fPlan);
                        wchar_t us[24], ts2[24], nums[72];
                        if (tot > 0) {
                            fmtTokens(used, us, 24);
                            fmtTokens(tot, ts2, 24);
                            swprintf(nums, 71, L"%ls / %ls \x00b7 %d%%", us, ts2, pct);
                        } else {
                            swprintf(nums, 71, L"%d%% used", pct);
                        }
                        dashStrR(dc, padL + innerW - secPadX, cy, nums, t.dim, fS10);
                        int barY = cy + DX(15);
                        int barW = innerW - 2 * secPadX;
                        COLORREF track = blendCr(t.card, white, 141);
                        dashCard(dc, padL + secPadX, barY, barW, DX(7), track, t.divider);
                        if (pct > 0) {
                            int fw = (int)((double)barW * pct / 100.0);
                            COLORREF c1 = blendCr(track, t.yellow, 141);
                            COLORREF c2 = blendCr(track, t.pinkDeep, 141);
                            if (pct >= 90) { c1 = c2 = blendCr(track, t.warn, 115); }
                            if (g_gdipOk && fw > 4) {
                                GRADIENT_RECT gr = { 0, 1 };
                                TRIVERTEX tv[2];
                                memset(tv, 0, sizeof(tv));
                                tv[0].x = padL + secPadX; tv[0].y = barY;
                                tv[0].Red = GetRValue(c1) << 8; tv[0].Green = GetGValue(c1) << 8; tv[0].Blue = GetBValue(c1) << 8; tv[0].Alpha = 0xFF00;
                                tv[1].x = padL + secPadX + fw; tv[1].y = barY + DX(7);
                                tv[1].Red = GetRValue(c2) << 8; tv[1].Green = GetGValue(c2) << 8; tv[1].Blue = GetBValue(c2) << 8; tv[1].Alpha = 0xFF00;
                                GradientFill(dc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_H);
                            } else {
                                HBRUSH b = CreateSolidBrush(c1);
                                RECT fr2 = { padL + secPadX, barY, padL + secPadX + fw, barY + DX(7) };
                                FillRect(dc, &fr2, b);
                                DeleteObject(b);
                            }
                        }
                        ry += (i == 0) ? DX(32) : DX(49);
                    }
                    y += planH + gap;
                } else planH = 0;
            }

            // daily usage section: heatmap + legend (+ optional day detail)
            int cell = DX(11), cgap = DX(3), pitch = cell + cgap;
            int rowLabW = DX(16) + DX(5);
            int weeks = 26;
            int secPadX = DX(12);
            SYSTEMTIME nowSt; GetLocalTime(&nowSt);
            int endDow = nowSt.wDayOfWeek; // 0 = Sun
            // quantile thresholds over nonzero days (dash.js renderHeatmap)
            long long nz[200]; int nnz = 0;
            for (int i = 0; i < DASH_MAX_DAYS && nnz < 200; i++) if (g_dayTot[i] > 0) nz[nnz++] = g_dayTot[i];
            for (int i = 1; i < nnz; i++) { long long v = nz[i]; int j = i - 1; while (j >= 0 && nz[j] > v) { nz[j + 1] = nz[j]; j--; } nz[j + 1] = v; }
            long long th[3] = { 1, 1, 1 };
            if (nnz) { th[0] = nz[nnz / 4]; th[1] = nz[nnz / 2]; th[2] = nz[nnz * 3 / 4]; }
            int hmH = 7 * pitch - cgap;
            int hasSel = g_daySel >= 0 && g_daySel < DASH_MAX_DAYS;
            int ddRows = 0, ddAll = 0;
            if (hasSel) {
                TokAgg *selRows = g_dayApp[DASH_MAX_DAYS - 1 - g_daySel];
                for (int i = 0; i < g_appCount; i++)
                    if (selRows[i].req > 0) ddAll++;
                ddRows = ddAll > 6 ? 6 : ddAll;
            }
            int ddFixed = DX(10) + DX(8) + DX(15) + DX(13);
            int tblReserve = DX(10) + DX(13) + 2 * DX(17) + DX(10);
            int secH = DX(10) + DX(11) + DX(8) + DX(13) + hmH + DX(2) + DX(10);
            if (hasSel) {
                int room = h - DX(24) - gap - tblReserve - y - secH;
                int fitRows = (room - ddFixed) / DX(17);
                if (fitRows < 0) fitRows = 0;
                if (ddRows > fitRows) ddRows = fitRows;
                if (ddRows <= 0) hasSel = 0;
            }
            int ddH = hasSel ? ddFixed + ddRows * DX(17) : 0;
            if (y + secH < h - DX(26)) {
                // fit gate stays on the no-selection height: the detail expands
                // the card only while the expansion fits, else it is clipped -
                // a selection must never collapse the section (the hit rects
                // would go stale and keep firing)
                if (hasSel && y + secH + ddH < h - DX(26)) secH += ddH;
                else hasSel = 0;
                dashCard(dc, padL, y, innerW, secH, t.card, t.divider);
                int hx = padL + secPadX;
                int hy = y + DX(10) + DX(11) + DX(8) + DX(13);
                dashHead(dc, hx, y + DX(10), L"DAILY USAGE", t.dim, fHead);
                // legend right: less [5 swatches] more
                {
                    const wchar_t *less = L"less", *more = L"more";
                    int sw2 = DX(10), sgap = DX(3);
                    int lw = dashStrW(dc, less, fS9) + DX(4) + 5 * sw2 + 4 * sgap + DX(4) + dashStrW(dc, more, fS9);
                    int lx = padL + innerW - secPadX - lw;
                    int ly2 = y + DX(10) + (DX(11) - sw2) / 2;
                    dashStr(dc, lx, ly2, less, t.dim, fS9);
                    int sx2 = lx + dashStrW(dc, less, fS9) + DX(4);
                    for (int i = 0; i < 5; i++) {
                        dashCard(dc, sx2, ly2, sw2, sw2, t.ramp[i], blendCr(t.ramp[i], RGB(0, 0, 0), 15));
                        sx2 += sw2 + sgap;
                    }
                    dashStr(dc, sx2, ly2, more, t.dim, fS9);
                }
                int lastMonth = -1;
                for (int wk = 0; wk < weeks; wk++) {
                    int daysBack0 = (weeks - 1 - wk) * 7 + endDow; // dow=0 cell
                    SYSTEMTIME cd;
                    dashColDate(daysBack0, &cd);
                    if ((int)cd.wMonth != lastMonth && cd.wDay <= 21) {
                        lastMonth = cd.wMonth;
                        wchar_t ml[16];
                        lstrcpynW(ml, DASH_MONTHS[(cd.wMonth - 1) % 12], 15);
                        dashStr(dc, hx + rowLabW + wk * pitch, hy - DX(13), ml, t.dim, fS9);
                    }
                    for (int dow = 0; dow < 7; dow++) {
                        int daysBack = daysBack0 - dow;
                        int cx = hx + rowLabW + wk * pitch;
                        int cy = hy + dow * pitch;
                        g_hmRect[wk][dow].left = cx; g_hmRect[wk][dow].top = cy;
                        g_hmRect[wk][dow].right = cx + cell; g_hmRect[wk][dow].bottom = cy + cell;
                        g_hmBack[wk][dow] = daysBack;
                        if (daysBack < 0 || daysBack >= DASH_MAX_DAYS) continue; // future
                        long long tot = g_dayTot[DASH_MAX_DAYS - 1 - daysBack];
                        int lv = 0;
                        if (tot > 0) lv = (tot <= th[0]) ? 1 : (tot <= th[1]) ? 2 : (tot <= th[2]) ? 3 : 4;
                        COLORREF cc = t.ramp[lv];
                        if (g_gdipOk) gdipRoundRect(dc, cc, blendCr(cc, RGB(0, 0, 0), 10), cx, cy, cell, cell, DX(2.5));
                        else dashRoundRectGDI(dc, cx, cy, cell, cell, DX(2.5), cc, blendCr(cc, RGB(0, 0, 0), 10));
                        if (daysBack == g_daySel)
                            dashCard(dc, cx - DX(1), cy - DX(1), cell + DX(2), cell + DX(2), 0, t.pinkDeep);
                    }
                }
                // sparse row labels: Sun + Fri
                dashStr(dc, hx, hy + 0 * pitch, L"Sun", t.dim, fS9);
                dashStr(dc, hx, hy + 5 * pitch, L"Fri", t.dim, fS9);
                // day detail (click a cell)
                if (hasSel) {
                    int ddy = y + secH - DX(10) - ddH;
                    HPEN dpen = CreatePen(PS_DOT, 1, t.divider);
                    HGDIOBJ op = SelectObject(dc, dpen);
                    MoveToEx(dc, padL + secPadX, ddy, NULL);
                    LineTo(dc, padL + innerW - secPadX, ddy);
                    SelectObject(dc, op);
                    DeleteObject(dpen);
                    ddy += DX(8);
                    SYSTEMTIME d2;
                    dashColDate(g_daySel, &d2);
                    TokAgg *dayRows = g_dayApp[DASH_MAX_DAYS - 1 - g_daySel];
                    long long tot = g_dayTot[DASH_MAX_DAYS - 1 - g_daySel];
                    int anyRec = ddRows > 0;
                    wchar_t dtitle[96];
                    if (anyRec) {
                        wchar_t dnum[24];
                        fmtTokens(tot, dnum, 24);
                        swprintf(dtitle, 95, L"%ls, %ls %lu, %lu \x2014 %ls tokens", DASH_DAYS[d2.wDayOfWeek % 7],
                                 DASH_MONTHS[(d2.wMonth - 1) % 12], (unsigned long)d2.wDay, (unsigned long)d2.wYear, dnum);
                    } else {
                        swprintf(dtitle, 95, L"%ls, %ls %lu, %lu \x2014 no usage", DASH_DAYS[d2.wDayOfWeek % 7],
                                 DASH_MONTHS[(d2.wMonth - 1) % 12], (unsigned long)d2.wDay, (unsigned long)d2.wYear);
                    }
                    dashStr(dc, padL + secPadX, ddy, dtitle, t.fg, fPlan);
                    ddy += DX(15);
                    // per-day cache presence from g_dayApp (Electron hasCacheData):
                    // cache columns + header only while the day carries cache
                    int cache = 0;
                    for (int i = 0; i < g_appCount; i++)
                        if (dayRows[i].cr + dayRows[i].cw > 0) { cache = 1; break; }
                    int xs[5];
                    dashTableCols(padL + innerW - secPadX, cache, xs);
                    if (anyRec) {
                        dashTableHead(dc, padL + secPadX, innerW, ddy, cache, xs, &t, fS9, L"APP");
                        ddy += DX(13);
                    }
                    // the reserve holds 6 rows: when the day has more
                    // apps, the last slot carries an overflow line instead of
                    // a silently dropped row
                    int ddLimit = ddAll > ddRows ? ddRows - 1 : ddRows;
                    int ddDrawn = 0;
                    for (int i = 0; i < g_appCount && ddDrawn < ddLimit
                         && ddy + DX(17) < y + secH; i++) {
                        TokAgg *a = &dayRows[i];
                        if (a->req == 0) continue;
                        wchar_t an[24];
                        MultiByteToWideChar(CP_UTF8, 0, g_appName[i], -1, an, 24);
                        dashDot(dc, padL + secPadX + 2, ddy + (DX(15) - DX(8)) / 2, DX(8), appDotColor(g_appName[i], &t));
                        SelectObject(dc, fBody);
                        SetTextColor(dc, t.fg);
                        RECT lr2 = { padL + secPadX + 2 + DX(14), ddy, padL + secPadX + DX(150), ddy + DX(15) + 1 };
                        DrawTextW(dc, an, -1, &lr2, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
                        wchar_t vs[32];
                        fmtTokens(a->in, vs, 32); dashStrR(dc, xs[4], ddy, vs, t.fg, fBody);
                        fmtTokens(a->out, vs, 32); dashStrR(dc, xs[3], ddy, vs, t.fg, fBody);
                        if (cache) {
                            fmtTokens(a->cr, vs, 32); dashStrR(dc, xs[2], ddy, vs, t.fg, fBody);
                            fmtTokens(a->cw, vs, 32); dashStrR(dc, xs[1], ddy, vs, t.fg, fBody);
                        }
                        swprintf(vs, 32, L"%lld", a->req); dashStrR(dc, xs[0], ddy, vs, t.fg, fBody);
                        ddDrawn++;
                        ddy += DX(17);
                    }
                    if (ddDrawn < ddAll) {
                        wchar_t more[32];
                        swprintf(more, 31, L"+%d more", ddAll - ddDrawn);
                        dashStr(dc, padL + secPadX + 2 + DX(14), ddy, more, t.dim, fS10);
                    }
                }
                y += secH + gap;
            } else {
                memset(g_hmRect, 0, sizeof(g_hmRect)); // no section: no live hit rects
            }

            // tables: By app + By model (2-col grid). Electron sorts by total
            // desc and caps the model table at 7 rows - mirror that.
            int colW2 = (innerW - DX(10)) / 2;
            int secPad = DX(10);
            int th2 = DX(9) + DX(4);
            int rowH = DX(19);
            int appOrder[DASH_MAX_APPS], appN = g_appCount;
            for (int i = 0; i < appN; i++) appOrder[i] = i;
            for (int i = 1; i < appN; i++) {
                int v = appOrder[i], j = i - 1;
                long long vt = g_appAgg[v].in + g_appAgg[v].out + g_appAgg[v].cr + g_appAgg[v].cw;
                while (j >= 0) {
                    int u = appOrder[j];
                    long long ut = g_appAgg[u].in + g_appAgg[u].out + g_appAgg[u].cr + g_appAgg[u].cw;
                    if (ut >= vt) break;
                    appOrder[j + 1] = appOrder[j];
                    j--;
                }
                appOrder[j + 1] = v;
            }
            int mdlOrder[DASH_MAX_MODELS], mdlN = g_modelCount;
            if (mdlN > 7) mdlN = 7;
            // every slot the sort loop touches must be defined: init ALL
            // entries, sort the full set, cap only the displayed rows at 7
            for (int i = 0; i < g_modelCount; i++) mdlOrder[i] = i;
            for (int i = 1; i < g_modelCount; i++) {
                int v = i, j = i - 1;
                long long vt = g_modelAgg[v].in + g_modelAgg[v].out + g_modelAgg[v].cr + g_modelAgg[v].cw;
                while (j >= 0) {
                    int u = mdlOrder[j];
                    long long ut = g_modelAgg[u].in + g_modelAgg[u].out + g_modelAgg[u].cr + g_modelAgg[u].cw;
                    if (ut >= vt) break;
                    mdlOrder[j + 1] = mdlOrder[j];
                    j--;
                }
                mdlOrder[j + 1] = v;
            }
            // cache columns are per table (Electron appCache vs modelCache):
            // apps over every app, models over the displayed top-7 only
            int appCache = 0;
            for (int i = 0; i < g_appCount; i++)
                if (g_appAgg[i].cr + g_appAgg[i].cw > 0) { appCache = 1; break; }
            int modelCache = 0;
            for (int i = 0; i < mdlN; i++)
                if (g_modelAgg[mdlOrder[i]].cr + g_modelAgg[mdlOrder[i]].cw > 0) { modelCache = 1; break; }
            int rows = appN > mdlN ? appN : mdlN;
            // fit EVERY row: tighten the row pitch (17..19 CSS px) before
            // dropping one, so the model table never loses a row silently.
            // Zero rows is allowed: the section header alone still fits and
            // draws, so the tables never vanish without a trace.
            int budget = h - DX(24) - y - secPad - th2 - secPad;
            int fitMax = budget / DX(17);
            if (rows > fitMax) rows = fitMax;
            if (rows < 0) rows = 0;
            if (rows > 0) {
                rowH = budget / rows;
                if (rowH > DX(19)) rowH = DX(19);
                if (rowH < DX(17)) rowH = DX(17);
            }
            int tblH = secPad + th2 + rows * rowH + secPad;
            if (y + tblH <= h - DX(24)) {
                for (int side = 0; side < 2; side++) {
                    int sx = padL + side * (colW2 + DX(10));
                    dashCard(dc, sx, y, colW2, tblH, t.card, t.divider);
                    dashHead(dc, sx + DX(12), y + secPad, side == 0 ? L"BY APP" : L"BY MODEL", t.dim, fHead);
                    int inner = colW2 - DX(24);
                    int xs[5];
                    int cache = side == 0 ? appCache : modelCache;
                    dashTableCols(sx + DX(12) + inner, cache, xs);
                    int ry = y + secPad + th2;
                    dashTableHead(dc, sx + DX(12), inner, ry, cache, xs, &t, fS9,
                                  side == 0 ? L"APP" : L"MODEL");
                    ry += th2;
                    if (side == 0) {
                        long long maxAll = 1;
                        for (int i = 0; i < g_appCount; i++) {
                            long long s = g_appAgg[i].in + g_appAgg[i].out + g_appAgg[i].cr + g_appAgg[i].cw;
                            if (s > maxAll) maxAll = s;
                        }
                        int appRows = appN < rows ? appN : rows;
                        for (int i = 0; i < appRows; i++) {
                            int ai = appOrder[i];
                            wchar_t an[24];
                            MultiByteToWideChar(CP_UTF8, 0, g_appName[ai], -1, an, 24);
                            long long s = g_appAgg[ai].in + g_appAgg[ai].out + g_appAgg[ai].cr + g_appAgg[ai].cw;
                            TokAgg a2 = g_appAgg[ai];
                            // even rows get the zebra wash (dash.css nth-child(even))
                            if (i & 1) {
                                HBRUSH b = CreateSolidBrush(t.zebra);
                                RECT fr2 = { sx + 1, ry, sx + colW2 - 1, ry + rowH };
                                FillRect(dc, &fr2, b);
                                DeleteObject(b);
                            }
                            dashTableRow(dc, sx + DX(12), inner, ry, rowH, cache, xs, an,
                                         appDotColor(g_appName[ai], &t), (double)s / maxAll, &a2, &t, fBody, fS9);
                            ry += rowH;
                        }
                        if (appN == 0 && rows > 0) {
                            dashStr(dc, sx + DX(12), ry + DX(2), L"No usage recorded yet.", t.dim, fBody);
                        }
                    } else {
                        long long maxAll = 1;
                        for (int i = 0; i < g_modelCount; i++) {
                            long long s = g_modelAgg[i].in + g_modelAgg[i].out + g_modelAgg[i].cr + g_modelAgg[i].cw;
                            if (s > maxAll) maxAll = s;
                        }
                        int mdlRows = mdlN < rows ? mdlN : rows;
                        for (int i = 0; i < mdlRows; i++) {
                            int mi = mdlOrder[i];
                            // label = the first-seen raw model casing; dot = app color
                            char mk[48];
                            lstrcpynA(mk, g_modelName[mi], 47);
                            char *bar = strchr(mk, '|');
                            const char *mp2 = g_modelLabel[mi];
                            wchar_t mn[48];
                            int mnw = MultiByteToWideChar(CP_UTF8, 0, mp2, -1, mn, 47);
                            mn[mnw > 0 ? mnw - 1 : 0] = 0; // force NUL even on truncation
                            long long s = g_modelAgg[mi].in + g_modelAgg[mi].out + g_modelAgg[mi].cr + g_modelAgg[mi].cw;
                            TokAgg a2 = g_modelAgg[mi];
                            if (i & 1) {
                                HBRUSH b = CreateSolidBrush(t.zebra);
                                RECT fr2 = { sx + 1, ry, sx + colW2 - 1, ry + rowH };
                                FillRect(dc, &fr2, b);
                                DeleteObject(b);
                            }
                            if (bar) *bar = 0;
                            COLORREF dc2 = appDotColor(mk, &t);
                            dashTableRow(dc, sx + DX(12), inner, ry, rowH, cache, xs, mn,
                                         dc2, (double)s / maxAll, &a2, &t, fBody, fS9);
                            ry += rowH;
                        }
                        if (mdlN == 0 && rows > 0) {
                            dashStr(dc, sx + DX(12), ry + DX(2), L"No model data.", t.dim, fBody);
                        }
                    }
                }
                y += tblH + gap;
            }
        }
        // footer: last scan time, right-aligned
        {
            wchar_t ftxt[40];
            if (g_lastScanMs) {
                unsigned ago = (unsigned)((GetTickCount64() - (unsigned long long)g_lastScanMs) / 1000);
                dashFmtTime(dashLocalNowMs() - (long long)ago * 1000, ftxt, 39);
            } else lstrcpynW(ftxt, L"\x2014", 39);
            wchar_t fl[64];
            swprintf(fl, 63, L"last scan %ls", ftxt);
            dashStrR(dc, w - padL, h - DX(10) - DX(12), fl, t.dim, fS9);
        }
    } else {
        // ---- subscriptions board: donut pies per plan window (subs.css)
        int innerW = w - 2 * padL;
        int pn = g_cfg.subsProviderCount; if (pn > MAX_SUBS) pn = MAX_SUBS;
        int shown = 0, enabledN = 0;
        for (int i = 0; i < pn; i++) if (subsProvEnabled(i)) enabledN++;
        int rows = (enabledN + 1) / 2; if (rows < 1) rows = 1;
        int colW2 = (innerW - gap) / 2;
        int panelH = (h - DX(30) - y - gap * (rows - 1)) / rows;
        for (int pi2 = 0; pi2 < pn; pi2++) {
            if (!g_cfg.subsEnabled || !subsProvEnabled(pi2)) continue; // master off or disabled: no panel (Electron parity)
            wchar_t label[48];
            subsProvLabel(pi2, label, 48);
            SubsWin wins[4];
            int wn = subsProvWins(pi2, wins, 4);
            int stale = wn < 0; if (wn < 0) wn = -wn;
            int px2 = padL + (shown % 2) * (colW2 + gap);
            int py2 = y + (shown / 2) * (panelH + gap);
            if (py2 + panelH > h - DX(30)) break;
            // panel: card + head (dot, name, pill) + body (pies) + foot
            dashCard(dc, px2, py2, colW2, panelH, t.card, t.divider);
            int headH = DX(40);
            dashCard(dc, px2, py2, colW2, headH, t.head, 0);
            // dashed head underline
            HPEN dpen = CreatePen(PS_DOT, 1, t.divider);
            HGDIOBJ op = SelectObject(dc, dpen);
            MoveToEx(dc, px2, py2 + headH, NULL);
            LineTo(dc, px2 + colW2, py2 + headH);
            SelectObject(dc, op);
            DeleteObject(dpen);
            // dot (heatmap ramp color per provider index)
            COLORREF dotc = t.ramp[pi2 % 5];
            dashDot(dc, px2 + DX(14), py2 + (headH - DX(9)) / 2, DX(9), dotc);
            dashStr(dc, px2 + DX(14) + DX(9) + DX(8), py2 + DX(11), label, t.fg, f13);
            // .panel-plan: plan name beside the provider label (subs.css)
            {
                wchar_t plan[24];
                subsProvPlan(pi2, plan, 24);
                if (!plan[0]) lstrcpynW(plan, L"\u2014", 24);
                int lx = px2 + DX(14) + DX(9) + DX(8) + dashStrW(dc, label, f13) + DX(8);
                dashStr(dc, lx, py2 + DX(14), plan, t.dim, fS10);
            }
            // status pill (subs.css .pill): stale / capped / near cap / ok
            {
                int lowest = 1000, anyw = 0;
                for (int k = 0; k < wn; k++) { if (wins[k].rem < lowest) lowest = wins[k].rem; anyw = 1; }
                const wchar_t *ps2 = NULL; COLORREF pc = t.dim, pbg = blendCr(t.head, t.dim, 31);
                if (stale) { ps2 = L"STALE"; pc = t.yellow; pbg = blendCr(t.head, t.yellow, 31); }
                else if (!anyw) ps2 = NULL;
                else if (lowest <= 0) { ps2 = L"CAPPED"; pc = t.warn; pbg = blendCr(t.head, t.warn, 20); }
                else if (lowest <= 10) { ps2 = L"NEAR CAP"; pc = t.warn; pbg = blendCr(t.head, t.warn, 20); }
                else { ps2 = L"OK"; pc = t.good; pbg = blendCr(t.head, t.good, 20); }
                if (ps2) {
                    int pw2 = dashStrW(dc, ps2, fS10b) + DX(16);
                    int ph2 = DX(18);
                    int plx = px2 + colW2 - DX(14) - pw2;
                    dashCard(dc, plx, py2 + (headH - ph2) / 2, pw2, ph2, pbg, pc);
                    SelectObject(dc, fS10b);
                    SetTextColor(dc, pc);
                    SetTextCharacterExtra(dc, DX(0.4));
                    RECT pr3 = { plx, py2 + (headH - ph2) / 2 + DX(2), plx + pw2, py2 + (headH - ph2) / 2 + ph2 };
                    DrawTextW(dc, ps2, -1, &pr3, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                    SetTextCharacterExtra(dc, 0);
                }
            }
            // pies
            int bodyY = py2 + headH + DX(14);
            if (wn == 0) {
                dashStr(dc, px2 + DX(14), bodyY + DX(12), stale ? L"stale \x2014 no data yet" : L"no windows reported", t.dim, fBody);
            }
            // every window gets a pie: shrink the pie to the body height the
            // way the provider rows compress (never drop the later windows)
            int bodyAvail = py2 + panelH - DX(34) - bodyY;
            int pieD = DX(96);
            if (wn > 0) {
                int fit = (bodyAvail - (wn - 1) * DX(16)) / wn;
                if (fit < pieD) pieD = fit;
            }
            // legible floor: pen width and text geometry derive from pieD
            if (pieD < DX(24)) pieD = DX(24);
            float pieScale = (float)pieD / (float)DX(96);
            HFONT fPct = pieD >= DX(56) ? f18 : (pieD >= DX(38) ? f13 : fS10);
            for (int k = 0; k < wn; k++) {
                int ky = bodyY + k * (pieD + DX(16));
                if (ky + pieD > py2 + panelH - DX(34)) break;
                int kx = px2 + DX(14);
                int rem = wins[k].rem;
                if (rem < 0) rem = 0; if (rem > 100) rem = 100;
                // track + arc (rotate -90: start at 12 o'clock, clockwise)
                COLORREF track = blendCr(t.card, t.dim, 31);
                if (g_gdipOk) {
                    float wpen = (float)DX(12.57) * pieScale;
                    float rr = (float)DX(36.4) * pieScale;
                    float cx2 = (float)kx + pieD / 2.0f, cy2 = (float)ky + pieD / 2.0f;
                    // track: full circle
                    {
                        GpGraphics *g2 = NULL;
                        if (t_GdipCreateFromHDC(dc, &g2) == 0) {
                            t_GdipSetSmoothingMode(g2, 6);
                            GpPen *pen = NULL;
                            if (t_GdipCreatePen1(GDIP_ARGB(track), wpen, 2, &pen) == 0) {
                                t_GdipDrawEllipse(g2, pen, cx2 - rr, cy2 - rr, 2 * rr, 2 * rr);
                                t_GdipDeletePen(pen);
                            }
                            t_GdipDeleteGraphics(g2);
                        }
                    }
                    if (rem > 0) gdipArcStroke(dc, t.pinkDeep, wpen, cx2 - rr, cy2 - rr, 2 * rr, 2 * rr, -90.0f, rem * 3.6f);
                }
                // center: "N%" pinkDeep + "left"
                wchar_t pctS[8];
                swprintf(pctS, 7, L"%d", rem);
                int pw2 = dashStrW(dc, pctS, fPct) + DX(8);
                int cx0 = kx + (pieD - pw2) / 2;
                dashStr(dc, cx0, ky + pieD / 2 - DX(15), pctS, t.pinkDeep, fPct);
                dashStr(dc, cx0 + dashStrW(dc, pctS, fPct) + DX(1), ky + pieD / 2 - DX(11), L"%", t.pinkDeep, fS10);
                dashStr(dc, kx + (pieD - dashStrW(dc, L"left", fS9)) / 2, ky + pieD / 2 + DX(6), L"left", t.dim, fS9);
                // meta right of the pie
                int mx = kx + pieD + DX(14);
                wchar_t up[40];
                // uppercase label
                {
                    wchar_t ul[20];
                    for (int ci = 0; wins[k].label[ci] && ci < 19; ci++) {
                        wchar_t ch = wins[k].label[ci];
                        ul[ci] = (ch >= L'a' && ch <= L'z') ? ch - 32 : ch;
                        ul[ci + 1] = 0;
                    }
                    lstrcpynW(up, ul, 39);
                }
                SelectObject(dc, f13);
                SetTextColor(dc, t.fg);
                SetTextCharacterExtra(dc, DX(0.7));
                // meta text tracks the pie: the same fractions of pieD the
                // uncompressed board (pieD 96) lays out at 24 / 50 / 66
                int metaY = ky + (int)(pieD * 0.25f);
                RECT mr3 = { mx, metaY, px2 + colW2 - DX(14), metaY + DX(20) };
                DrawTextW(dc, up, -1, &mr3, DT_SINGLELINE | DT_LEFT);
                SetTextCharacterExtra(dc, 0);
                wchar_t usedLine[72];
                if (wins[k].used >= 0 && wins[k].total > 0) {
                    wchar_t us2[24], ts3[24];
                    fmtNum(wins[k].used, us2, 24);
                    fmtNum(wins[k].total, ts3, 24);
                    swprintf(usedLine, 71, L"%ls / %ls used", us2, ts3);
                } else swprintf(usedLine, 71, L"%d%% used", wins[k].pct);
                dashStr(dc, mx, ky + (int)(pieD * 0.52f), usedLine, t.dim, fS10);
                // third line: relative reset time (subs.js fmtReset)
                wchar_t rst[40];
                fmtReset(wins[k].resetAt, rst, 40);
                dashStr(dc, mx, ky + (int)(pieD * 0.69f), rst, t.dim, fS10);
            }
            // panel foot: dashed top + fetched time right
            int fy = py2 + panelH - DX(26);
            HPEN fpen = CreatePen(PS_DOT, 1, t.divider);
            HGDIOBJ op2 = SelectObject(dc, fpen);
            MoveToEx(dc, px2 + DX(14), fy, NULL);
            LineTo(dc, px2 + colW2 - DX(14), fy);
            SelectObject(dc, op2);
            DeleteObject(fpen);
            unsigned ago = subsFetchedAgoSec();
            wchar_t ftim[16];
            if (ago != 0xFFFFFFFFu) dashFmtTime(dashLocalNowMs() - (long long)ago * 1000, ftim, 15);
            else lstrcpynW(ftim, L"\x2014", 15);
            wchar_t fl2[32];
            swprintf(fl2, 31, L"%ls", ftim);
            dashStrR(dc, px2 + colW2 - DX(14), fy + DX(6), fl2, t.dim, fS10);
            shown++;
        }
        if (!shown) {
            dashStr(dc, padL, y, g_cfg.subsEnabled ? L"No providers enabled." : L"Subscriptions are off (subs.enabled).", t.dim, fBody);
        }
    }

    DeleteObject(fTitle); DeleteObject(fBody); DeleteObject(fVal);
    DeleteObject(fS10); DeleteObject(fS9); DeleteObject(fBtn);
    DeleteObject(fHead); DeleteObject(f13); DeleteObject(f18);
    DeleteObject(fPlan); DeleteObject(fS10b);

    HDC wdc = GetDC(hwnd);
    BitBlt(wdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, wdc);
    if (g_dashType == 0) g_dashTokVersion = g_tokDataVersion;
}

static int dashPtInBtn(POINT p, int *which) {
    if (PtInRect(&g_btnRefresh, p)) { *which = 1; return 1; }
    if (PtInRect(&g_btnClose, p)) { *which = 2; return 1; }
    *which = 0;
    return 0;
}

static void dashTipCell(HWND hwnd, POINT p) {
    for (int wk = 0; wk < 26; wk++)
        for (int dow = 0; dow < 7; dow++) {
            if (!PtInRect(&g_hmRect[wk][dow], p)) continue;
            int daysBack = g_hmBack[wk][dow];
            if (daysBack < 0 || daysBack >= DASH_MAX_DAYS) { tipHide(); return; }
            int di = DASH_MAX_DAYS - 1 - daysBack;
            long long tot = g_dayTot[di];
            SYSTEMTIME d2;
            dashColDate(daysBack, &d2);
            wchar_t head[64], body[640];
            swprintf(head, 63, L"%ls, %ls %lu, %lu", DASH_DAYS[d2.wDayOfWeek % 7],
                     DASH_MONTHS[(d2.wMonth - 1) % 12], (unsigned long)d2.wDay, (unsigned long)d2.wYear);
            if (tot > 0) {
                wchar_t tn[24];
                fmtTokens(tot, tn, 24);
                lstrcpynW(body, tn, 639);
                lstrcatW(body, L" tokens");
                // 12 apps max, but keep room for a trailing "+N more" so a
                // long list is never silently cut short
                int cap = 640 - 24, skipped = 0;
                for (int i = 0; i < g_appCount; i++) {
                    TokAgg *a = &g_dayApp[di][i];
                    if (a->req == 0) continue;
                    wchar_t an[24], line[56];
                    MultiByteToWideChar(CP_UTF8, 0, g_appName[i], -1, an, 24);
                    wchar_t tn2[24];
                    fmtTokens(a->in + a->out + a->cr + a->cw, tn2, 24);
                    swprintf(line, 55, L"\n%ls: %ls (%lld)", an, tn2, a->req);
                    if (lstrlenW(body) + lstrlenW(line) >= cap) { skipped++; continue; }
                    lstrcatW(body, line);
                }
                if (skipped) {
                    wchar_t more[24];
                    swprintf(more, 23, L"\n+%d more", skipped);
                    lstrcatW(body, more);
                }
            } else {
                lstrcpynW(body, L"no usage", 639);
            }
            static wchar_t tipBuf[704];
            swprintf(tipBuf, 703, L"%ls\n%ls", head, body);
            POINT sp = p;
            ClientToScreen(hwnd, &sp);
            tipShow(tipBuf, sp.x, sp.y, 1);
            return;
        }
    tipHide();
}

static LRESULT CALLBACK dashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        // subs fetches land asynchronously (0.2-0.7s after a refresh click,
        // and on the periodic cycle); the token board only changes when a
        // scan produces new numbers, so the tick repaints on data change
        SetTimer(hwnd, 1, 500, NULL);
        return 0;
    case WM_TIMER:
        if (g_dashType == 1 || g_tokDataVersion != g_dashTokVersion) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paintDash(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); }
        return 0;
    case WM_RBUTTONUP:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCHITTEST: {
        LRESULT base = DefWindowProcW(hwnd, msg, wp, lp);
        if (base == HTCLIENT) {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &p);
            int bi2;
            if (p.y < g_tbBottom && !dashPtInBtn(p, &bi2)) return HTCAPTION; // title drag
        }
        return base;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            int bi2;
            SetCursor(dashPtInBtn(p, &bi2) ? LoadCursor(NULL, IDC_HAND) : LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int bi2 = 0;
        dashPtInBtn(p, &bi2);
        if (bi2 != g_btnHover) { g_btnHover = bi2; InvalidateRect(hwnd, NULL, FALSE); }
        if (g_dashType == 0) dashTipCell(hwnd, p);
        TRACKMOUSEEVENT te = { sizeof(te), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&te);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_btnHover) { g_btnHover = 0; InvalidateRect(hwnd, NULL, FALSE); }
        tipHide();
        return 0;
    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int bi2;
        if (dashPtInBtn(p, &bi2)) {
            if (bi2 == 1) { // refresh: rescan tokens / refetch subs (Electron parity)
                if (g_dashType == 0) scanTokenCache(); else subsRefetchNow();
                InvalidateRect(hwnd, NULL, FALSE);
            } else DestroyWindow(hwnd);
            return 0;
        }
        if (g_dashType == 0) {
            for (int wk = 0; wk < 26; wk++)
                for (int dow = 0; dow < 7; dow++) {
                    if (!PtInRect(&g_hmRect[wk][dow], p)) continue;
                    int daysBack = g_hmBack[wk][dow];
                    if (daysBack < 0 || daysBack >= DASH_MAX_DAYS) return 0;
                    g_daySel = (g_daySel == daysBack) ? -1 : daysBack;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_dash = NULL;
        g_dashPainted = -1;
        g_daySel = -1;
        tipHide();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void dashToggle(int type) {
    if (g_dash) {
        if (g_dashType == type) { DestroyWindow(g_dash); g_dash = NULL; return; }
        g_dashType = type;
        SetWindowTextW(g_dash, type == 0 ? L"Chocobar dashboard" : L"Chocobar subscriptions");
        // the other board has its own configured size: adopt it in place
        int tw = (int)((double)(type == 0 ? g_cfg.dashW : g_cfg.subsW) * g_scale + 0.5);
        int th2 = (int)((double)(type == 0 ? g_cfg.dashH : g_cfg.subsH) * g_scale + 0.5);
        int tsw = GetSystemMetrics(SM_CXSCREEN), tsh = GetSystemMetrics(SM_CYSCREEN);
        if (tw > tsw - 40) tw = tsw - 40;
        if (th2 > tsh - 40) th2 = tsh - 40;
        SetWindowPos(g_dash, NULL, (tsw - tw) / 2, (tsh - th2) / 2, tw, th2, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(g_dash, NULL, FALSE);
        SetForegroundWindow(g_dash);
        return;
    }
    scanTokenCache(); // fresh numbers for the panel
    int cw = (int)((double)(type == 0 ? g_cfg.dashW : g_cfg.subsW) * g_scale + 0.5);
    int ch = (int)((double)(type == 0 ? g_cfg.dashH : g_cfg.subsH) * g_scale + 0.5);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (cw > sw - 40) cw = sw - 40;
    if (ch > sh - 40) ch = sh - 40;
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dashProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ChocobarDash";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    g_dashType = type;
    // APPWINDOW: the Electron dash is a NORMAL window on purpose (taskbar +
    // Alt-Tab entry) so activation never falls through to the terminal.
    g_dash = CreateWindowExW(WS_EX_APPWINDOW, L"ChocobarDash", type == 0 ? L"Chocobar dashboard" : L"Chocobar subscriptions",
                             WS_POPUP | WS_VISIBLE | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, (sw - cw) / 2, (sh - ch) / 2, cw, ch,
                             NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_dash) return;
    dashRoundCorners(g_dash);
    SetForegroundWindow(g_dash);
    InvalidateRect(g_dash, NULL, FALSE);
}

// ---- hover tooltips (Electron: seg.title) -----------------------------------
static HWND g_tip = NULL;
static int g_tipOn = 0;
static HFONT g_tipFont = NULL;
static HFONT g_tipHeat = NULL;
static HFONT g_tipHeatB = NULL;
static HDC g_tipDc = NULL;
static HBITMAP g_tipDib = NULL;
static HGDIOBJ g_tipOldBmp = NULL;
static void *g_tipBits = NULL;
static LONG g_tipW = 0, g_tipH = 0;

static void tipHide(void) {
    if (g_tip && g_tipOn) ShowWindow(g_tip, SW_HIDE);
    g_tipOn = 0;
}

// dark = the dashboard's #heat-tip card (dash.css), light = a browser-style
// title tooltip for the bar chips
static void tipShow(const wchar_t *text, int cx, int cy, int dark) {
    if (!text || !*text) { tipHide(); return; }
    if (dark) {
        if (!g_tipHeat) {
            wchar_t fam[LF_FACESIZE];
            uiFontFamily(fam, LF_FACESIZE);
            int px = -(int)(10 * g_scale);
            g_tipHeat = CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, fam);
            g_tipHeatB = CreateFontW(px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, fam);
        }
    } else if (!g_tipFont) {
        g_tipFont = CreateFontW(-(int)(12 * g_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    if (!g_tip) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ChocobarTip";
        RegisterClassW(&wc);
        g_tip = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                L"ChocobarTip", L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!g_tip) return;
    }
    if (!g_tipDc) g_tipDc = CreateCompatibleDC(NULL);
    HFONT mf = dark ? g_tipHeat : g_tipFont;
    HFONT of = (HFONT)SelectObject(g_tipDc, mf);
    int padX = (int)((dark ? 9 : 7) * g_scale), padY = (int)((dark ? 6 : 4) * g_scale);
    int lineH = (int)(15 * g_scale);
    LONG w = 0, h = 0;
    if (dark) {
        // 10px font, 1.5 line-height (dash.css #heat-tip)
        int n = 1;
        for (const wchar_t *p = text; *p; p++) if (*p == L'\n') n++;
        wchar_t ln[512];
        int maxW = 0;
        const wchar_t *p = text;
        while (p) {
            const wchar_t *nl = wcschr(p, L'\n');
            int len = nl ? (int)(nl - p) : (int)wcslen(p);
            if (len > 511) len = 511;
            memcpy(ln, p, (size_t)len * sizeof(wchar_t));
            ln[len] = 0;
            RECT lm = { 0, 0, 8000, 0 };
            DrawTextW(g_tipDc, ln, -1, &lm, DT_CALCRECT | DT_NOPREFIX);
            if (lm.right > maxW) maxW = lm.right;
            p = nl ? nl + 1 : NULL;
        }
        w = maxW + 2 * padX;
        h = n * lineH + 2 * padY;
    } else {
        // multi-line aware measure: DrawText with DT_CALCRECT expands on \n
        RECT mr = { 0, 0, 8000, 0 };
        DrawTextW(g_tipDc, text, -1, &mr, DT_CALCRECT | DT_NOPREFIX);
        w = mr.right + 2 * padX;
        h = mr.bottom + 2 * padY;
    }
    SelectObject(g_tipDc, of);
    if (w != g_tipW || h != g_tipH || !g_tipDib) {
        if (g_tipDib) { SelectObject(g_tipDc, g_tipOldBmp); DeleteObject(g_tipDib); g_tipDib = NULL; g_tipBits = NULL; }
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_tipDib = CreateDIBSection(g_tipDc, &bi, DIB_RGB_COLORS, &g_tipBits, NULL, 0);
        if (!g_tipDib) return;
        g_tipOldBmp = SelectObject(g_tipDc, g_tipDib);
        g_tipW = w; g_tipH = h;
    }
    DWORD *px = (DWORD *)g_tipBits;
    if (dark) {
        // dark card, rounded 6px, no border (dash.css #heat-tip)
        int rr = (int)(6 * g_scale);
        if (rr > w / 2) rr = w / 2;
        if (rr > h / 2) rr = h / 2;
        for (LONG yy = 0; yy < h; yy++)
            for (LONG xx = 0; xx < w; xx++) {
                int ex = xx < rr ? rr - (int)xx : (xx >= w - rr ? (int)xx - (int)w + rr + 1 : 0);
                int ey = yy < rr ? rr - (int)yy : (yy >= h - rr ? (int)yy - (int)h + rr + 1 : 0);
                int corner = (ex > 0 && ey > 0 && ex * ex + ey * ey > rr * rr);
                px[yy * w + xx] = corner ? 0 : 0xFF241F16;
            }
        wchar_t ln[512];
        const wchar_t *p = text;
        int li = 0;
        while (p) {
            const wchar_t *nl = wcschr(p, L'\n');
            int len = nl ? (int)(nl - p) : (int)wcslen(p);
            if (len > 511) len = 511;
            memcpy(ln, p, (size_t)len * sizeof(wchar_t));
            ln[len] = 0;
            SelectObject(g_tipDc, li == 0 ? g_tipHeatB : g_tipHeat);
            SetBkMode(g_tipDc, TRANSPARENT);
            SetTextColor(g_tipDc, li == 0 ? RGB(0xF6, 0xD8, 0xE0) : RGB(0xF5, 0xF0, 0xD8));
            RECT tr = { padX, padY + li * lineH, w - padX, padY + (li + 1) * lineH };
            DrawTextW(g_tipDc, ln, -1, &tr, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE);
            p = nl ? nl + 1 : NULL;
            li++;
        }
    } else {
        // opaque white tooltip, gray hairline border, opaque alpha
        for (LONG yy = 0; yy < h; yy++)
            for (LONG xx = 0; xx < w; xx++) {
                int border = (xx == 0 || yy == 0 || xx == w - 1 || yy == h - 1);
                px[yy * w + xx] = border ? 0xFFC8C8C8 : 0xFFFFFFFF;
            }
        RECT tr = { padX, padY, w - padX, h };
        SelectObject(g_tipDc, g_tipFont);
        SetBkMode(g_tipDc, TRANSPARENT);
        SetTextColor(g_tipDc, RGB(31, 31, 31));
        DrawTextW(g_tipDc, text, -1, &tr, DT_LEFT | DT_NOPREFIX); // \n breaks lines
    }
    // GDI leaves ALPHA=0 on pixels it draws: repair them so the text is
    // opaque while the rounded corners stay transparent
    for (LONG i = 0; i < w * h; i++) {
        DWORD v = px[i];
        if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0) px[i] = v | 0xFF000000u;
    }
    // clamp to the screen, then place below-right of the cursor
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = cx + (int)(6 * g_scale), y = cy + (int)(16 * g_scale);
    if (x + w > sw) x = cx - w - (int)(6 * g_scale);
    if (y + h > sh) y = cy - h - (int)(14 * g_scale);
    if (x < 0) x = 0; if (y < 0) y = 0;
    SetWindowPos(g_tip, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    POINT ptSrc = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bl = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_tip, NULL, NULL, &sz, g_tipDc, &ptSrc, 0, &bl, ULW_ALPHA);
    ShowWindow(g_tip, SW_SHOWNOACTIVATE);
    g_tipOn = 1;
}

// Electron seg.title table
static const wchar_t *chipTitle(int idx) {
    static wchar_t buf[320];
    if (idx < 0 || idx >= g_chipCount) return NULL;
    Chip *c = &g_chips[idx];
    switch (c->type) {
    case CT_SHORTCUT:
        if (g_cfg.shortcutCommand && *g_cfg.shortcutCommand) {
            swprintf(buf, 319, L"Run: %ls", g_cfg.shortcutCommand);
            return buf;
        }
        return L"Shortcut (set modules.shortcut.command in the config)";
    case CT_PET:
        return NULL;
    case CT_CUSTOM:
        if (c->iconSvg == SVG_DIAMOND) return L"Token usage today";
        if (c->iconSvg == SVG_GAUGE) return L"Subscription plan remaining";
        if (c->customIdx >= 0 && c->customIdx < MAX_CUSTOM) {
            CustomChip *cc = &g_cfg.custom[c->customIdx];
            if (cc->title && *cc->title) return cc->title;
            if (cc->label && *cc->label) return cc->label;
            return L"Custom chip";
        }
        return NULL;
    case CT_GPU: return L"GPU usage";
    case CT_CPU: return L"CPU usage";
    case CT_CPUTEMP: return L"CPU temperature";
    case CT_RAM: return L"Memory usage";
    case CT_VOLUME: return L"Volume";
    case CT_BATTERY: return L"Battery";
    case CT_CLOCK: return L"Local time";
    }
    return NULL;
}

static int chipAt(POINT p) {
    for (int i = 0; i < g_chipCount; i++) {
        if (PtInRect(&g_chips[i].r, p)) return i;
    }
    return -1;
}

static void chipClick(int idx) {
    Chip *c = &g_chips[idx];
    switch (c->type) {
    case CT_SHORTCUT: execCmd(g_cfg.shortcutCommand); break;
    case CT_PET: petToggle(); break;
    case CT_CUSTOM: {
        if (c->customIdx < 0 || c->customIdx >= MAX_CUSTOM) {
            // display-only chip: the dashboard openers (diamond = token
            // usage, gauge = subscriptions board)
            if (c->iconSvg == SVG_DIAMOND) dashToggle(0);
            else if (c->iconSvg == SVG_GAUGE) dashToggle(1);
            break;
        }
        CustomChip *cc = &g_cfg.custom[c->customIdx];
        if (!cc->command) break;
        if (cc->toggle) {
            g_customState[c->customIdx] = !g_customState[c->customIdx];
            wchar_t full[1100];
            swprintf(full, 1099, L"%ls %ls", cc->command, g_customState[c->customIdx] ? L"on" : L"off");
            execCmd(full);
        } else execCmd(cc->command);
        InvalidateRect(g_bar, NULL, FALSE);
        break;
    }
    }
}

static void showTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Edit config");
    AppendMenuW(m, MF_STRING, 2, L"Open config folder");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Reload chocobar");
    AppendMenuW(m, MF_STRING, 4, L"Quit chocobar");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(hwnd); // required for correct menu dismissal
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (id == 1) {
        if (GetFileAttributesW(g_cfgPath) == INVALID_FILE_ATTRIBUTES) writeTemplate();
        SHELLEXECUTEINFOW sei; memset(&sei, 0, sizeof(sei)); sei.cbSize = sizeof(sei);
        sei.lpVerb = L"open"; sei.lpFile = g_cfgPath; sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    } else if (id == 2) {
        wchar_t dir[MAX_PATH]; lstrcpynW(dir, g_cfgPath, MAX_PATH);
        wchar_t *slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
        SHELLEXECUTEINFOW sei; memset(&sei, 0, sizeof(sei)); sei.cbSize = sizeof(sei);
        sei.lpVerb = L"open"; sei.lpFile = dir; sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    } else if (id == 3) {
        loadConfig();
        applyBackdrop(g_bar);
        clockFmtReload();
        followTick();
        InvalidateRect(g_bar, NULL, FALSE);
    } else if (id == 4) {
        PostQuitMessage(0);
    }
}

static void trayRemove(void) {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = 0;
}

static void trayAdd(HWND hwnd) {
    if (g_trayAdded || !g_cfg.showTray) return;
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"Chocobar", 128);
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) g_trayAdded = 1;
}


static void CALLBACK winEventProc(HWINEVENTHOOK h, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD tid, DWORD time);
static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) repaintBar(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_METRICS) {
            pollCpu(); pollRam(); pollBattery(); pollVolume(); pollGpu();
            pollHwinfoSm2(); pollHwinfoSm();
            if (!g_m.tempOk) { g_m.tempOk = 0; }
            g_m.petRunning = g_cfg.petEnabled && g_cfg.petExePath && *g_cfg.petExePath ? petRunning(g_cfg.petExePath) : 0;
            clockFmtReload();
            formatClock(clockFmt(), g_m.clockText, 80);
            buildChips();
            repaintBar(hwnd);
        } else if (wp == TIMER_FOLLOW) {
            followTick();
            return 0;
        } else if (wp == TIMER_CONFIG) {
            configCheckTick();
        }
        return 0;
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = chipAt(p);
        if (h != g_hover) {
            g_hover = h;
            repaintBar(hwnd);
        }
        // title tooltips (Electron: seg.title): the metric segs show the
        // small label, the pinned chips theirs; no chip = no tooltip
        const wchar_t *tt = chipTitle(h);
        if (tt) {
            POINT sp; GetCursorPos(&sp);
            tipShow(tt, sp.x, sp.y, 0);
        } else tipHide();
        if (!g_trackingMouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_trackingMouse = 1;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = -1;
        g_trackingMouse = 0;
        tipHide();
        repaintBar(hwnd);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = chipAt(p);
        if (h >= 0) chipClick(h);
        else if (g_term) {
            // the bar is the terminal's title-strip substitute: clicking the
            // strip (not a chip) raises the followed terminal
            SetForegroundWindow(g_term);
        }
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) showTrayMenu(hwnd);
        return 0;
    case WM_DESTROY:
        trayRemove();
        // the layered tip is topmost: it must die with the bar or it lingers
        // on screen looking like a second bar fragment
        tipHide();
        if (g_tip) { DestroyWindow(g_tip); g_tip = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CALLBACK winEventProc(HWINEVENTHOOK h, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD tid, DWORD time) {
    (void)h; (void)hwnd; (void)idObject; (void)idChild; (void)tid; (void)time;
    if (event == EVENT_SYSTEM_FOREGROUND) followTick();
}

static const char *g_template =
    "// Chocobar (native build) config. Saved on first run; hot-reloads on save.\r\n"
    "// Everything below is optional - delete a key and the built-in default applies.\r\n"
    "{\r\n"
    "  \"bar\": { \"height\": 24, \"gap\": 8, \"fontSize\": 12,\r\n"
    "            \"backgroundTint\": \"#FBF2E2\", \"backgroundAlpha\": 110, \"backdrop\": \"acrylic\",\r\n"
    "            \"radius\": 8 },\r\n"
    "  \"theme\": { \"fg\": \"#080808\", \"fgDim\": \"#5a5245\", \"pink\": \"#E8C7D0\", \"pinkDeep\": \"#D493AA\",\r\n"
    "              \"warn\": \"#A00000\", \"good\": \"#006400\", \"divider\": \"#D9CCB2\",\r\n"
    "              \"iconColor\": \"#D493AA\", \"iconOpacity\": 90,\r\n"
    "              \"heatmap\": [\"#F1ECD8\", \"#F6D8E0\", \"#EFB7C7\", \"#E28FB0\", \"#C95E8F\"] },\r\n"
    "  \"dashboard\": { \"width\": 840, \"height\": 580 },\r\n"
    "  \"tokens\": { \"appFilter\": [], \"cachePath\": \"\" },\r\n"
    "  \"modules\": {\r\n"
    "    \"gpu\": { \"enabled\": true },\r\n"
    "    \"cpu\":  { \"enabled\": true, \"warnAt\": 85 },\r\n"
    "    \"cputemp\": { \"enabled\": true, \"warnAt\": 85 },\r\n"
    "    \"ram\":  { \"enabled\": true, \"warnAt\": 90 },\r\n"
    "    \"volume\": { \"enabled\": true },\r\n"
    "    \"battery\": { \"enabled\": true },\r\n"
    "    \"clock\": { \"enabled\": true, \"format\": \"{MMM} {dd} ({Wkk}) {HH}:{mm}\" },\r\n"
    "    \"shortcut\": { \"enabled\": false, \"label\": \"\", \"command\": \"\" },\r\n"
    "    \"pet\": { \"enabled\": false, \"label\": \"\", \"exePath\": \"\" },\r\n"
    "    \"custom\": [\r\n"
    "      { \"enabled\": false, \"icon\": \"\", \"label\": \"Example\", \"command\": \"notepad.exe\", \"toggle\": false }\r\n"
    "    ]\r\n"
    "  },\r\n"
    "  \"subs\": { \"enabled\": false, \"intervalMinutes\": 2, \"fetchTimeoutMs\": 20000,\r\n"
    "             \"width\": 820, \"height\": 480,\r\n"
    "             \"providers\": [\r\n"
    "               { \"type\": \"chatgpt\", \"enabled\": false, \"label\": \"ChatGPT\", \"authPath\": \"~/.codex/auth.json\" },\r\n"
    "               { \"type\": \"zai\", \"enabled\": false, \"label\": \"Z.ai\", \"configPath\": \"~/.zcode/v2/config.json\", \"provider\": \"builtin:zai-coding-plan\" },\r\n"
    "               { \"type\": \"antigravity\", \"enabled\": false, \"label\": \"Antigravity\", \"authPath\": \"~/.pi/agent/auth.json\" }\r\n"
    "             ] },\r\n"
    "  \"terminal\": { \"className\": \"\", \"title\": \"\" },\r\n"
    "  \"general\": { \"showTray\": true }\r\n"
    "}\r\n";


void writeTemplate(void) {
    HANDLE h = CreateFileW(g_cfgPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    WriteFile(h, g_template, (DWORD)strlen(g_template), &wrote, NULL);
    CloseHandle(h);
}

void loadConfig(void) {
    DWORD len = 0;
    char *raw = readFileUtf8(g_cfgPath, &len);
    if (!raw) {
        writeTemplate();
        raw = readFileUtf8(g_cfgPath, &len);
        if (!raw) return;
    }
    stripLineComments(raw);
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = jsmn_parse(&parser, raw, (size_t)len, NULL, 0);
    if (ntok < 0) { HeapFree(GetProcessHeap(), 0, raw); return; }
    jsmntok_t *toks = (jsmntok_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(jsmntok_t) * (ntok + 1));
    if (!toks) { HeapFree(GetProcessHeap(), 0, raw); return; }
    jsmn_init(&parser);
    jsmn_parse(&parser, raw, (size_t)len, toks, (unsigned int)ntok);
    Config next;
    parseConfigInto(&next, raw, toks, 0);
    HeapFree(GetProcessHeap(), 0, toks);
    HeapFree(GetProcessHeap(), 0, raw);
    freeConfig(&g_cfg);
    g_cfg = next;
    g_cfgLoaded = 1;
    g_iconOpacity = g_cfg.iconOpacity / 100.0; // p_icons AlphaBlend constant
    // freeConfig above freed every string the chip array points at
    // (colorOverride / iconColorOverride): rebuild before any paint reads them
    buildChips();
}

// --------------------------------------------------------------- wWinMain ----
static void resolveConfigPath(void) {
    wchar_t *cmd = GetCommandLineW();
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(cmd, &argc);
    g_cfgPath[0] = 0;
    for (int i = 1; argv && i < argc; i++) {
        if (!wcsncmp(argv[i], L"--config", 8) && i + 1 < argc) {
            lstrcpynW(g_cfgPath, argv[i + 1], MAX_PATH);
            break;
        }
    }
    if (argv) LocalFree(argv);
    if (!g_cfgPath[0]) {
        wchar_t env[MAX_PATH];
        if (GetEnvironmentVariableW(L"WIZBAR_CONFIG", env, MAX_PATH) && env[0]) lstrcpynW(g_cfgPath, env, MAX_PATH);
    }
    if (!g_cfgPath[0]) {
        wchar_t home[MAX_PATH];
        GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        swprintf(g_cfgPath, MAX_PATH, L"%ls\\.wizbar\\config.json", home);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;
    SetUnhandledExceptionFilter(crashHandler);
    HANDLE mutex = CreateMutexW(NULL, TRUE, APP_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0; // single instance
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // per-monitor-v2 DPI awareness: DWM frame bounds and GetWindowRect must
    // agree (both physical) or the bar lands scaled wrong on high-DPI.
    typedef BOOL (WINAPI *SPDAC)(HANDLE);
    SPDAC setCtx = (SPDAC)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setCtx) setCtx((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    HDC sdc = GetDC(NULL);
    g_scale = (double)GetDeviceCaps(sdc, LOGPIXELSX) / 96.0;
    ReleaseDC(NULL, sdc);
    if (g_scale <= 0) g_scale = 1.0;

    resolveConfigPath();
    loadConfig();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &fa)) g_cfgMtime = fa.ftLastWriteTime;

    volumeInit();
    gpuInit();

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APP_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // WS_EX_TOPMOST: the Electron bar is always-on-top ('floating'); without
    // it the followed terminal raises over the bar and swallows every
    // click/hover meant for the chips
    g_bar = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        APP_CLASS, L"Chocobar", WS_POPUP,
        -2000, -2000, 800, (int)(g_cfg.height * g_scale + 0.5),
        NULL, NULL, hInst, NULL);
    if (!g_bar) return 1;

    if (!initRender(g_bar)) dbg("render init failed");

    SetTimer(g_bar, TIMER_METRICS, 1000, NULL);
    SetTimer(g_bar, TIMER_FOLLOW, 100, NULL);
    SetTimer(g_bar, TIMER_CONFIG, 1000, NULL);

    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                         NULL, winEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    (void)hook;

    trayAdd(g_bar);

    svgInitAll();
    SendMessageW(g_bar, WM_TIMER, TIMER_METRICS, 0); // prime metrics + first paint
    // layered windows never receive WM_PAINT: draw + UpdateLayeredWindow explicitly
    paint(g_bar);
    {
        char dbg[160];
        sprintf(dbg, "[wizbar] start %s %s: subs en=%d n=%d", __DATE__, __TIME__, g_cfg.subsEnabled, g_cfg.subsProviderCount);
        writeLogA(dbg);
    }
    subsStart();
    followTick();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_vol) IAudioEndpointVolume_Release(g_vol);
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
