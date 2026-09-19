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
static double g_scale = 1.0; // display scale (dpi/96): config values are in DIPs
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
    int align;           // 1 = right group (metrics), 2 = left pinned group
    int iconYellow;      // tokens diamond renders yellow, others pink
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
    int px = (int)(g_cfg.fontSize * g_scale * 0.77 + 0.5); // GDI rasterizes ~30% taller/wider than DirectWrite at the same nominal px; calibrated against the live bars
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
    c->align = 1; c->iconYellow = 0;
    lstrcpynW(c->text, text ? text : L"", 96);
    c->r.left = c->r.right = c->r.top = c->r.bottom = 0;
}
static void addChip(int type, int customIdx, const wchar_t *text, int warn, const wchar_t *colorOverride) {
    addChipI(type, customIdx, text, warn, colorOverride, 0);
}

static long long g_tokensToday = -1;
static int g_tokensTick = 0;

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
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) { g_tokensToday = -1; return; }
    lstrcatW(path, L"\\.wizbar\\token-cache.json");
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
    const char *p = buf, *end = buf + got;
    while ((p = findStr(p, end, "\"ts\":")) != NULL) {
        p += 5;
        long long ts = parseLL(p, end);
        const char *next = findStr(p, end, "\"ts\":");
        const char *recEnd = next ? next : end;
        long long sum = 0;
        // Electron rowTotal: input + output + cacheRead + cacheWrite
        const char *keys[4] = { "\"input\":", "\"output\":", "\"cacheRead\":", "\"cacheWrite\":" };
        int lens[4] = { 8, 9, 12, 13 };
        for (int k = 0; k < 4; k++) {
            const char *f = findStr(p, recEnd, keys[k]);
            if (f) sum += parseLL(f + lens[k], recEnd);
        }
        if (ts >= midnight) total += sum;
        p = (next && next > p) ? next : p + 5;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    g_tokensToday = total;
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
    // left pinned group: pet, tokens, divider dash
    if (g_cfg.petEnabled) {
        wchar_t txt[16];
        int running = g_m.petRunning;
        lstrcpynW(txt, running ? L"on" : L"off", 16);
        addChipI(CT_PET, 0, txt, 0, running ? g_cfg.good : g_cfg.fgDim, 0xF004);
        g_chips[g_chipCount - 1].align = 2;
    }
    if (1) { // token chip (reads the Electron cache)
        wchar_t txt[32];
        fmtTokens(g_tokensToday, txt, 32);
        addChipI(CT_CUSTOM, -1, txt, 0, g_cfg.fgDim, 0xF0687);
        g_chips[g_chipCount - 1].align = 2;
        g_chips[g_chipCount - 1].iconYellow = 1;
    }
    {
        addChipI(CT_CUSTOM, -1, L"\u2014", 0, g_cfg.divider, 0);
        g_chips[g_chipCount - 1].align = 2;
    }
    // right metric group: icon + bare value, like the Electron bar
    wchar_t v[48];
    if (g_cfg.mGpu) { swprintf(v, 48, L"%d%%", (int)(g_m.gpu + 0.5)); addChipI(CT_GPU, 0, v, 0, NULL, 0xF08CA); }
    if (g_cfg.mCpu) {
        int hot = g_cfg.cpuWarnAt > 0 && g_m.cpu >= g_cfg.cpuWarnAt;
        swprintf(v, 48, L"%d%%", (int)(g_m.cpu + 0.5));
        addChipI(CT_CPU, 0, v, hot, hot ? g_cfg.warn : NULL, 0xF035B);
    }
    if (g_cfg.mCpuTemp) {
        if (g_m.tempOk) {
            if ((int)(g_m.tempC * 10) % 10 == 0) swprintf(v, 48, L"%d\u00B0C", (int)g_m.tempC);
            else swprintf(v, 48, L"%.1f\u00B0C", g_m.tempC);
            addChipI(CT_CPUTEMP, 0, v, g_m.tempHot, g_m.tempHot ? g_cfg.warn : NULL, 0xF05C3);
        } else addChipI(CT_CPUTEMP, 0, L"\u2014", 0, g_cfg.divider, 0);
    }
    if (g_cfg.mRam) {
        int hot = g_cfg.ramWarnAt > 0 && g_m.ram >= g_cfg.ramWarnAt;
        swprintf(v, 48, L"%d%%", (int)(g_m.ram + 0.5));
        addChipI(CT_RAM, 0, v, hot, hot ? g_cfg.warn : NULL, 0xF04B0);
    }
    if (g_cfg.mVolume) {
        int muted = g_m.volume == 0;
        swprintf(v, 48, L"%d%%", g_m.volume);
        addChipI(CT_VOLUME, 0, v, 0, muted ? g_cfg.fgDim : NULL, muted ? 0xF0581 : 0xF057E);
    }
    if (g_cfg.mBattery) {
        if (g_m.battValid) {
            int low = g_m.battPct <= 20;
            swprintf(v, 48, L"%d%%", g_m.battPct);
            addChipI(CT_BATTERY, 0, v, low, low ? g_cfg.warn : NULL, 0xF240);
        } else addChipI(CT_BATTERY, 0, L"AC", 0, g_cfg.fgDim, 0xF0427);
    }
    if (g_cfg.mClock) addChipI(CT_CLOCK, 0, g_m.clockText, 0, NULL, 0xF017);
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

    SetBkMode(g_memDc, TRANSPARENT);
    int textH = g_dibH;
    int pad = (int)(6.0f * (FLOAT)g_scale);

    // Electron geometry: bar padding 8px left / 12px right, 14px between
    // segments, 5px between icon and value (CSS px, x2 at this DPI)
    FLOAT hf = (FLOAT)g_dibH;
    FLOAT padL = 8.0f * (FLOAT)g_scale, padR = 12.0f * (FLOAT)g_scale;
    FLOAT segGap = 14.0f * (FLOAT)g_scale, icoGap = 5.0f * (FLOAT)g_scale;
    int iconW[MAX_CHIPS];
    for (int i = 0; i < g_chipCount; i++) {
        iconW[i] = 0;
        if (g_chips[i].iconCp) {
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

    for (int i = 0; i < g_chipCount; i++) {
        Chip *c = &g_chips[i];
        if (i == g_hover) {
            COLORREF pink = colorrefFromHex(g_cfg.pinkBg, 255);
            HBRUSH hb = CreateSolidBrush(pink);
            RECT hr = { c->r.left, 1, c->r.right, g_dibH - 1 };
            FillRect(g_memDc, &hr, hb);
            DeleteObject(hb);
        }
        const wchar_t *col = c->colorOverride;
        if (c->warn) col = g_cfg.warn;
        COLORREF vc = col ? colorrefFromHex(col, 255) : fgCr;
        int tx = c->r.left;
        SetTextColor(g_memDc, vc);
        if (c->iconCp) {
            wchar_t ico[8];
            chipIconText(c, ico);
            COLORREF ic = c->iconYellow ? colorrefFromHex(g_cfg.yellow, 255)
                                        : colorrefFromHex(g_cfg.pinkDeep, 255);
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


static int isTerminalHwnd(HWND h) {
    if (!h || h == g_bar) return 0;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return 0;
    if (g_cfg.terminalClassName && *g_cfg.terminalClassName) return !lstrcmpiW(cls, g_cfg.terminalClassName);
    return !lstrcmpiW(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") ||
           !lstrcmpiW(cls, L"ConsoleWindowClass") ||
           !lstrcmpiW(cls, L"VirtualConsoleClass") ||
           !lstrcmpiW(cls, L"mintty");
}

static HWND findTerminalByProbe(void) {
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
        swprintf(cmd, MAX_PATH + 31, L"/IM %s /F", base);
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
    if (GetGUIThreadInfo(tid, &gi) && (gi.flags & GUI_INMOVESIZE)) return;

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
        SetWindowPos(g_bar, g_term, target.left, target.top,
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
        if (c->customIdx < 0 || c->customIdx >= MAX_CUSTOM) break; // display-only chip
        CustomChip *cc = &g_cfg.custom[c->customIdx];
        if (!cc->command) break;
        if (cc->toggle) {
            g_customState[c->customIdx] = !g_customState[c->customIdx];
            wchar_t full[1100];
            swprintf(full, 1099, L"%s %s", cc->command, g_customState[c->customIdx] ? L"on" : L"off");
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
        repaintBar(hwnd);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = chipAt(p);
        if (h >= 0) chipClick(h);
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
    "// This build reads a subset of the Chocobar config format today:\r\n"
    "// bar look, metric chips, clock, shortcut/pet/custom chips, tray.\r\n"
    "{\r\n"
    "  \"bar\": { \"height\": 24, \"gap\": 8, \"fontSize\": 12,\r\n"
    "            \"backgroundTint\": \"#FBF2E2\", \"backgroundAlpha\": 110, \"backdrop\": \"acrylic\" },\r\n"
    "  \"theme\": { \"fg\": \"#080808\", \"pinkDeep\": \"#D493AA\", \"warn\": \"#A00000\" },\r\n"
    "  \"modules\": {\r\n"
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
    "  \"terminal\": { \"className\": \"\" },\r\n"
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
        swprintf(g_cfgPath, MAX_PATH, L"%s\\.wizbar\\config.json", home);
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

    g_bar = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
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

    SendMessageW(g_bar, WM_TIMER, TIMER_METRICS, 0); // prime metrics + first paint
    // layered windows never receive WM_PAINT: draw + UpdateLayeredWindow explicitly
    paint(g_bar);
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
