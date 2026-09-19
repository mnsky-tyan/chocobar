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
    // Chromium renders DirectWrite semibold; GDI needs FW_SEMIBOLD to match.
    int px = (int)(g_cfg.fontSize * g_scale + 0.5);
    g_font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
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
#define DASH_MAX_DAYS 190
static long long g_appToday[DASH_MAX_APPS], g_appWeek[DASH_MAX_APPS];
static long long g_appMonth[DASH_MAX_APPS], g_appAll[DASH_MAX_APPS];
static char g_appName[DASH_MAX_APPS][20];
static int g_appCount = 0;
static long long g_tokWeek = 0, g_tokMonth = 0, g_tokAll = 0;
static long long g_dayTot[DASH_MAX_DAYS]; // [DASH_MAX_DAYS-1] = today
static long long g_lastScanMs = 0;

static void aggRecord(const char *app, int alen, long long ts, long long sum, long long midnight) {
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
    long long day = (ts - midnight) / 86400000LL; // 0 = today, -n = n days ago
    int ai = -1;
    for (int i = 0; i < g_appCount; i++)
        if (memcmp(g_appName[i], app, alen) == 0 && g_appName[i][alen] == 0) { ai = i; break; }
    if (ai < 0 && g_appCount < DASH_MAX_APPS) {
        ai = g_appCount++;
        memcpy(g_appName[ai], app, alen);
        g_appName[ai][alen] = 0;
    }
    if (ai >= 0) {
        if (day == 0) g_appToday[ai] += sum;
        if (day >= -6) g_appWeek[ai] += sum;
        if (day >= -29) g_appMonth[ai] += sum;
        g_appAll[ai] += sum;
    }
    if (day >= -(DASH_MAX_DAYS - 1) && day <= 0)
        g_dayTot[DASH_MAX_DAYS - 1 + (int)day] += sum;
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

// today's raw (cache-exclusive) input+output, mirroring tokens.js; the
// Electron app rewrites this cache every rescan, we just read it
static void scanTokenCache(void) {
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
    FILETIME lft, ft;
    SystemTimeToFileTime(&st, &lft);        // treats fields as UTC
    LocalFileTimeToFileTime(&lft, &ft);     // apply the real TZ bias
    long long midnight = ((((long long)ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000 - 11644473600000LL;
    long long total = 0;
    memset(g_dayTot, 0, sizeof(g_dayTot));
    g_appCount = 0;
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
        long long sum = 0;
        // Electron rowTotal: input + output + cacheRead + cacheWrite
        const char *keys[4] = { "\"input\":", "\"output\":", "\"cacheRead\":", "\"cacheWrite\":" };
        int lens[4] = { 8, 9, 12, 13 };
        for (int k = 0; k < 4; k++) {
            const char *f = findStr(ae, recEnd, keys[k]);
            if (f) sum += parseLL(f + lens[k], recEnd);
        }
        if (ts >= midnight) total += sum;
        aggRecord(p, alen, ts, sum, midnight);
        if (ts >= midnight - 6LL * 86400000LL) g_tokWeek += sum;
        if (ts >= midnight - 29LL * 86400000LL) g_tokMonth += sum;
        g_tokAll += sum;
        p = next ? next : end;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    g_tokensToday = total;
    g_lastScanMs = (long long)GetTickCount64();
}

static void fmtTokens(long long n2, wchar_t *out, int cb) {
    if (n2 < 0) { lstrcpynW(out, L"\u2014", cb); return; }
    if (n2 >= 1000000000LL) swprintf(out, cb, L"%.2fB", n2 / 1e9);
    else if (n2 >= 1000000) swprintf(out, cb, L"%.1fM", n2 / 1e6);
    else if (n2 >= 1000) swprintf(out, cb, L"%.1fk", n2 / 1e3);
    else swprintf(out, cb, L"%lld", n2);
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
        if (g_dib) { DeleteObject(g_dib); g_dib = NULL; g_bits = NULL; }
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
        SelectObject(g_memDc, g_dib);    // memDc keeps the font selected? reselect below
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
    g_iconBoxN = 0;

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
    int pad = (int)(6.0f * (FLOAT)g_scale);

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
            // single-color icon set; theme.iconColor, else pinkDeep
            const wchar_t *icol = (g_cfg.iconColor && *g_cfg.iconColor) ? g_cfg.iconColor : g_cfg.pinkDeep;
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
    // theme.iconOpacity: fade the freshly drawn icons (boxes recorded by svgDraw)
    if (g_iconBoxN > 0) {
        int cov = (int)(g_iconOpacity * 255.0);
        for (int b = 0; b < g_iconBoxN; b++) {
            RECT *bx = &g_iconBoxes[b];
            for (int yy = bx->top > 0 ? bx->top : 0; yy < bx->bottom && yy < g_dibH; yy++)
                for (int xx = bx->left > 0 ? bx->left : 0; xx < bx->right && xx < g_dibW; xx++) {
                    DWORD v2 = px[(size_t)yy * g_dibW + xx];
                    if ((v2 & 0xFF000000u) == 0xFF000000u) px[(size_t)yy * g_dibW + xx] = pxScale(v2, cov);
                }
        }
        g_iconBoxN = 0;
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

static void configCheckTick(void) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_cfgMtime) != 0) {
        g_cfgMtime = fa.ftLastWriteTime;
        loadConfig();
        // font may change with the config: rebuild it
        if (g_font) { SelectObject(g_memDc, g_fontOld); DeleteObject(g_font); g_font = NULL; }
        HFONT nf = CreateFontW(-(int)(g_cfg.fontSize * g_scale + 0.5), 0, 0, 0, FW_NORMAL,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
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

static void dashText(HDC dc, int x, int y, const wchar_t *s, COLORREF cr,
                     HFONT f, int rightAlign, int maxW) {
    SelectObject(dc, f);
    SetTextColor(dc, cr);
    RECT r = { x, y, rightAlign ? x + maxW : x + 4000, y + (int)(40 * g_scale) };
    DrawTextW(dc, s, -1, &r, DT_SINGLELINE | DT_LEFT | (rightAlign ? DT_RIGHT : 0));
}

// heatmap ramp: theme.heatmap from the config (COLORREF resolved at paint)
static COLORREF dashRamp[5];

static void paintDash(HWND hwnd) {
    if (!g_dashDc) {
        g_dashDc = CreateCompatibleDC(NULL);
        g_dashOldBmp = NULL;
    }
    RECT rc; GetClientRect(hwnd, &rc);
    LONG w = rc.right, h = rc.bottom;
    if (w < 1 || h < 1) return;
    if (w != g_dashW || h != g_dashH || !g_dashDib || g_dashPainted != g_dashType) {
        if (g_dashDib) { DeleteObject(g_dashDib); g_dashDib = NULL; g_dashBits = NULL; }
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
    COLORREF bg = colorrefFromHex(g_cfg.pinkBg, 255);
    COLORREF card = colorrefFromHex(g_cfg.tint, 255);
    COLORREF fg = colorrefFromHex(g_cfg.fg, 255);
    COLORREF dim = colorrefFromHex(g_cfg.fgDim, 255);
    COLORREF pink = colorrefFromHex(g_cfg.pinkDeep, 255);
    COLORREF warn = colorrefFromHex(g_cfg.warn, 255);
    // opaque panel background
    {
        HBRUSH b = CreateSolidBrush(bg);
        RECT fr = { 0, 0, w, h };
        FillRect(dc, &fr, b);
        DeleteObject(b);
    }
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < 5; i++) dashRamp[i] = colorrefFromHex(g_cfg.heatmap[i], 255);
    HFONT fTitle = dashFont(15, FW_BOLD);
    HFONT fBody = dashFont(11, FW_SEMIBOLD);
    HFONT fVal = dashFont(17, FW_BOLD);
    HFONT fSmall = dashFont(10, FW_NORMAL);

    int pad = (int)(18 * g_scale);
    if (g_dashType == 0) {
        // ---- token usage panel
        dashText(dc, pad, (int)(12 * g_scale), L"Token usage", fg, fTitle, 0, 0);
        wchar_t sub[64];
        if (g_lastScanMs) {
            swprintf(sub, 63, L"last scan %lld s ago", (long long)((GetTickCount64() - (unsigned long long)g_lastScanMs) / 1000));
        } else lstrcpynW(sub, L"no scan yet", 64);
        dashText(dc, pad + (int)(120 * g_scale), (int)(18 * g_scale), sub, dim, fSmall, 0, 0);

        // stat cards: Today / Last 7 days / Last 30 days / All time
        struct { const wchar_t *label; long long v; } cards[4] = {
            { L"Today", g_tokensToday }, { L"Last 7 days", g_tokWeek },
            { L"Last 30 days", g_tokMonth }, { L"All time", g_tokAll } };
        int cy = (int)(46 * g_scale), chh = (int)(54 * g_scale), gap = (int)(10 * g_scale);
        int cw = (w - 2 * pad - 3 * gap) / 4;
        for (int i = 0; i < 4; i++) {
            int cx = pad + i * (cw + gap);
            // stat card: tint fill, small rounded corners
            int rr = (int)(4 * g_scale);
            HBRUSH b = CreateSolidBrush(card);
            LOGBRUSH lb1; lb1.lbStyle = BS_SOLID; lb1.lbColor = card; lb1.lbHatch = 0;
            HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 1, &lb1, 0, NULL);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
            RoundRect(dc, cx, cy, cx + cw, cy + chh, rr * 2, rr * 2);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(b); DeleteObject(pen);
            dashText(dc, cx + (int)(12 * g_scale), cy + (int)(7 * g_scale), cards[i].label, dim, fSmall, 0, 0);
            wchar_t vs[32];
            fmtTokens(cards[i].v, vs, 32);
            dashText(dc, cx + (int)(12 * g_scale), cy + (int)(24 * g_scale), vs, fg, fVal, 0, 0);
        }

        // heatmap: 26 weeks x 7 days, 11px cells, 3px gaps
        long long nz[150]; int nnz = 0;
        for (int i = 0; i < DASH_MAX_DAYS && nnz < 150; i++) if (g_dayTot[i] > 0) nz[nnz++] = g_dayTot[i];
        for (int i = 1; i < nnz; i++) { long long v = nz[i]; int j = i - 1; while (j >= 0 && nz[j] > v) { nz[j + 1] = nz[j]; j--; } nz[j + 1] = v; }
        long long th[3] = { 1, 1, 1 };
        if (nnz) { th[0] = nz[nnz / 4]; th[1] = nz[nnz / 2]; th[2] = nz[nnz * 3 / 4]; }
        int cell = (int)(11 * g_scale), cgap = (int)(3 * g_scale);
        int hy = (int)(118 * g_scale);
        int weeks = 26;
        int rowLabW = (int)(21 * g_scale);
        for (int wk = 0; wk < weeks; wk++) {
            for (int dow = 0; dow < 7; dow++) {
                int idx = DASH_MAX_DAYS - 1 - ((weeks - 1 - wk) * 7 + (6 - dow));
                int cx = pad + rowLabW + wk * (cell + cgap);
                int cyy = hy + dow * (cell + cgap);
                if (idx < 0) continue;
                long long t = g_dayTot[idx];
                COLORREF cc = card; // level 0
                if (t > 0) {
                    int lv = (t <= th[0]) ? 1 : (t <= th[1]) ? 2 : (t <= th[2]) ? 3 : 4;
                    cc = dashRamp[lv];
                }
                HBRUSH b = CreateSolidBrush(cc);
                RECT fr = { cx, cyy, cx + cell, cyy + cell };
                FillRect(dc, &fr, b);
                DeleteObject(b);
            }
        }
        // legend: less [][][][][] more
        int ly = hy + 7 * (cell + cgap) + (int)(2 * g_scale);
        dashText(dc, pad + rowLabW, ly, L"less", dim, fSmall, 0, 0);
        int lx = pad + rowLabW + (int)(28 * g_scale);
        for (int i = 0; i < 5; i++) {
            HBRUSH b = CreateSolidBrush(dashRamp[i]);
            RECT fr = { lx + i * (cell / 2 + cgap / 2), ly + (int)(2 * g_scale),
                        lx + i * (cell / 2 + cgap / 2) + cell / 2, ly + (int)(2 * g_scale) + cell / 2 };
            FillRect(dc, &fr, b);
            DeleteObject(b);
        }
        dashText(dc, lx + 5 * (cell / 2 + cgap / 2) + (int)(4 * g_scale), ly, L"more", dim, fSmall, 0, 0);

        // apps table: all-time per app, share bar
        int ay = ly + (int)(30 * g_scale);
        dashText(dc, pad, ay, L"Apps", fg, fBody, 0, 0);
        ay += (int)(22 * g_scale);
        long long maxAll = 1;
        for (int i = 0; i < g_appCount; i++) if (g_appAll[i] > maxAll) maxAll = g_appAll[i];
        int rowH = (int)(22 * g_scale);
        int barW = (int)(240 * g_scale);
        for (int i = 0; i < g_appCount && ay + rowH < h - (int)(8 * g_scale); i++) {
            int ry = ay + i * rowH;
            wchar_t an[24];
            MultiByteToWideChar(CP_UTF8, 0, g_appName[i], -1, an, 24);
            dashText(dc, pad, ry + (int)(3 * g_scale), an, fg, fBody, 0, 0);
            // share bar
            int bx = pad + (int)(140 * g_scale);
            int by = ry + (int)(5 * g_scale), bh = (int)(8 * g_scale);
            HBRUSH track = CreateSolidBrush(colorrefFromHex(g_cfg.divider, 255));
            RECT fr = { bx, by, bx + barW, by + bh };
            FillRect(dc, &fr, track);
            DeleteObject(track);
            long long v = g_appAll[i];
            int fw = (int)((double)barW * (double)v / (double)maxAll);
            if (fw > 0) {
                HBRUSH b = CreateSolidBrush(pink);
                RECT fr2 = { bx, by, bx + fw, by + bh };
                FillRect(dc, &fr2, b);
                DeleteObject(b);
            }
            wchar_t vs[32];
            fmtTokens(v, vs, 32);
            dashText(dc, bx + barW + (int)(12 * g_scale), ry + (int)(3 * g_scale), vs, dim, fBody, 0, 0);
        }
        if (g_appCount == 0) {
            dashText(dc, pad, ay, g_tokensToday >= 0 ? L"No usage recorded yet." : L"Token usage tracking is off (tokens.enabled).", dim, fBody, 0, 0);
        }
    } else {
        // ---- subscriptions board
        dashText(dc, pad, (int)(12 * g_scale), L"Subscriptions", fg, fTitle, 0, 0);
        int y = (int)(52 * g_scale);
        int shown = 0;
        int n = g_cfg.subsProviderCount; if (n > MAX_SUBS) n = MAX_SUBS;
        for (int i = 0; i < n; i++) {
            if (!subsProvEnabled(i)) continue; // disabled: no panel (Electron parity)
            wchar_t label[48];
            subsProvLabel(i, label, 48);
            wchar_t head[80];
            SubsWin wins[4];
            int wn = subsProvWins(i, wins, 4);
            int stale = wn < 0;
            if (wn < 0) wn = -wn;
            swprintf(head, 79, L"%ls%ls", label, stale ? L"  (stale)" : L"");
            dashText(dc, pad, y, head, stale ? warn : fg, fBody, 0, 0);
            y += (int)(24 * g_scale);
            if (wn == 0) {
                dashText(dc, pad + (int)(16 * g_scale), y, L"\x2014  no data yet", dim, fBody, 0, 0);
                y += (int)(26 * g_scale);
            }
            int barW = w - 2 * pad - (int)(252 * g_scale); // leave room for the used/total text
            for (int k = 0; k < wn; k++) {
                dashText(dc, pad + (int)(16 * g_scale), y, wins[k].label, fg, fBody, 0, 0);
                int bx = pad + (int)(90 * g_scale);
                int by = y + (int)(4 * g_scale), bh = (int)(10 * g_scale);
                HBRUSH track = CreateSolidBrush(colorrefFromHex(g_cfg.divider, 255));
                RECT fr = { bx, by, bx + barW, by + bh };
                FillRect(dc, &fr, track);
                DeleteObject(track);
                int fw = (int)((double)barW * (double)wins[k].pct / 100.0);
                if (fw > 0) {
                    HBRUSH b = CreateSolidBrush(wins[k].pct >= 90 ? warn : pink);
                    RECT fr2 = { bx, by, bx + fw, by + bh };
                    FillRect(dc, &fr2, b);
                    DeleteObject(b);
                }
                wchar_t rs[64];
                if (wins[k].used >= 0 && wins[k].total > 0) {
                    wchar_t us[24], ts2[24];
                    fmtTokens(wins[k].used, us, 24);
                    fmtTokens(wins[k].total, ts2, 24);
                    swprintf(rs, 63, L"%ls / %ls \x2014 %d%% used", us, ts2, wins[k].pct);
                } else swprintf(rs, 63, L"%d%% used", wins[k].pct);
                dashText(dc, bx + barW + (int)(12 * g_scale), y, rs, wins[k].pct >= 90 ? warn : dim, fBody, 0, 0);
                y += (int)(24 * g_scale);
            }
            y += (int)(10 * g_scale);
            shown++;
        }
        if (!shown) dashText(dc, pad, y, g_cfg.subsEnabled ? L"No providers enabled." : L"Subscriptions are off (subs.enabled).", dim, fBody, 0, 0);
    }
    DeleteObject(fTitle); DeleteObject(fBody); DeleteObject(fVal); DeleteObject(fSmall);

    HDC wdc = GetDC(hwnd);
    BitBlt(wdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, wdc);
}

static LRESULT CALLBACK dashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
    case WM_DESTROY:
        g_dash = NULL;
        g_dashPainted = -1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void dashToggle(int type) {
    if (g_dash) {
        if (g_dashType == type) { DestroyWindow(g_dash); g_dash = NULL; return; }
        g_dashType = type;
        SetWindowTextW(g_dash, type == 0 ? L"Chocobar dashboard" : L"Chocobar subscriptions");
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
    g_dash = CreateWindowExW(0, L"ChocobarDash", type == 0 ? L"Chocobar dashboard" : L"Chocobar subscriptions",
                             WS_POPUP | WS_VISIBLE, (sw - cw) / 2, (sh - ch) / 2, cw, ch,
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
static HDC g_tipDc = NULL;
static HBITMAP g_tipDib = NULL;
static HGDIOBJ g_tipOldBmp = NULL;
static void *g_tipBits = NULL;
static LONG g_tipW = 0, g_tipH = 0;

static void tipHide(void) {
    if (g_tip && g_tipOn) ShowWindow(g_tip, SW_HIDE);
    g_tipOn = 0;
}

static void tipShow(const wchar_t *text, int cx, int cy) {
    if (!text || !*text) { tipHide(); return; }
    if (!g_tipFont) {
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
    HFONT of = (HFONT)SelectObject(g_tipDc, g_tipFont);
    SIZE ts; GetTextExtentPoint32W(g_tipDc, text, lstrlenW(text), &ts);
    SelectObject(g_tipDc, of);
    int padX = (int)(7 * g_scale), padY = (int)(4 * g_scale);
    LONG w = ts.cx + 2 * padX, h = ts.cy + 2 * padY;
    if (w != g_tipW || h != g_tipH || !g_tipDib) {
        if (g_tipDib) DeleteObject(g_tipDib);
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
    DrawTextW(g_tipDc, text, -1, &tr, DT_SINGLELINE | DT_LEFT);
    // clamp to the screen, then place below-right of the cursor
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = cx + (int)(6 * g_scale), y = cy + (int)(16 * g_scale);
    if (x + w > sw) x = cx - w - (int)(6 * g_scale);
    if (y + h > sh) y = cy - h - (int)(14 * g_scale);
    if (x < 0) x = 0; if (y < 0) y = 0;
    SetWindowPos(g_tip, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    POINT ptSrc = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bl = { AC_SRC_OVER, 0, 255, 0 };
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
        } else if (0) {
            followTick();
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
        { char dbg[48]; sprintf(dbg, "mm h=%d x=%d", h, (int)(short)LOWORD(lp)); writeLogA(dbg); }
        const wchar_t *tt = chipTitle(h);
        if (tt) {
            POINT sp; GetCursorPos(&sp);
            tipShow(tt, sp.x, sp.y);
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
    "              \"iconColor\": \"#D493AA\", \"iconOpacity\": 100,\r\n"
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
    "               { \"type\": \"zai\", \"enabled\": false, \"label\": \"Z.ai\", \"configPath\": \"~/.zcode/v2/config.json\", \"provider\": \"builtin:zai-coding-plan\" }\r\n"
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
