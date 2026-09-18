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
    void *pad8_26; // CreateSurface..CheckDeviceState (not used)
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

HRESULT __stdcall DCompositionCreateDevice(IDXGIDevice *, const GUID *, void **);

static const GUID my_IID_IDXGIFactory2       = {0x50c83a1c,0xe072,0x4c48,{0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0}};

// shared state (declared here so every section below sees it)
static wchar_t g_cfgPath[MAX_PATH];
static double g_scale = 1.0; // display scale (dpi/96): config values are in DIPs
static int g_customState[MAX_CUSTOM];

// ------------------------------------------------------------- globals ----
static HWND g_bar;
static ID2D1Factory *g_d2f = NULL;
static ID3D11Device *g_d3d = NULL;
typedef struct ID2D1Image ID2D1Image; // mingw C headers never typedef this one
static IDXGISwapChain1 *g_swap = NULL;   // opaque; used through IDXGISwapChain
static ID2D1RenderTarget *g_rt = NULL;   // DXGI-surface render target
static IDCompositionDevice *g_dc = NULL;
static IDCompositionTarget *g_target = NULL;
static IDCompositionVisual *g_vis = NULL;
static IDWriteFactory *g_dwf = NULL;
static IDWriteTextFormat *g_tf = NULL;
static UINT g_rtW = 0, g_rtH = 0;
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
} Chip;

enum { CT_SHORTCUT, CT_PET, CT_CUSTOM, CT_GPU, CT_CPU, CT_CPUTEMP, CT_RAM, CT_VOLUME, CT_BATTERY, CT_CLOCK };
#define MAX_CHIPS 32
static Chip g_chips[MAX_CHIPS];
static int g_chipCount = 0;

// --------------------------------------------------------- d2d plumbing ----
static void releaseRenderTarget(void) {
    if (g_rt) { ID2D1RenderTarget_Release(g_rt); g_rt = NULL; }
    if (g_swap) { IDXGISwapChain_Release((IDXGISwapChain *)g_swap); g_swap = NULL; }
    g_rtW = g_rtH = 0;
}

static int buildRenderTarget(HWND hwnd, UINT w, UINT h) {
    (void)hwnd;
    if (!g_d2f || !g_d3d || !w || !h) return 0;
    IDXGIDevice *xdgi = NULL;
    if (FAILED(ID3D11Device_QueryInterface(g_d3d, &my_IID_IDXGIDevice, (void **)&xdgi))) return 0;
    IDXGIAdapter *ad = NULL;
    IDXGIDevice_GetAdapter(xdgi, &ad);
    IDXGIFactory2 *fac = NULL;
    IDXGIAdapter_GetParent(ad, &my_IID_IDXGIFactory2, (void **)&fac);
    DXGI_SWAP_CHAIN_DESC1 d;
    memset(&d, 0, sizeof(d));
    d.Width = w; d.Height = h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // 87
    d.Stereo = 0;
    d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    d.BufferUsage = 0x20;                    // DXGI_USAGE_RENDER_TARGET_OUTPUT (0x40 would be BACKBUFFER)
    d.BufferCount = 2;
    d.Scaling = 0;                           // DXGI_SCALING_STRETCH
    d.SwapEffect = 4;                        // DXGI_SWAP_EFFECT_FLIP_DISCARD
    d.AlphaMode = 3;                         // DXGI_ALPHA_MODE_PREMULTIPLIED
    d.Flags = 0;
    IDXGISwapChain1 *sc1 = NULL;
    HRESULT hr = IDXGIFactory2_CreateSwapChainForComposition(fac, (IUnknown *)g_d3d, &d, NULL, &sc1);
    g_swap = (IDXGISwapChain1 *)sc1; // base IDXGISwapChain vtbl prefix is shared
    if (ad) IDXGIAdapter_Release(ad);
    if (fac) fac->lpVtbl->Release(fac);
    if (FAILED(hr)) { IDXGIDevice_Release(xdgi); return 0; }

    static const GUID my_IID_IDXGIResource = {0x0350b015,0x2b5b,0x4b3e,{0x91,0x01,0x9d,0x6e,0xa7,0x32,0xca,0xb8}};
    IDXGISurface *surf = NULL;
    IUnknown *bunk = NULL;
    static const GUID my_IID_IUNKNOWN = {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
    HRESULT hrgb = IDXGISwapChain_GetBuffer((IDXGISwapChain *)g_swap, 0, &my_IID_IUNKNOWN, (void **)&bunk);
    if (SUCCEEDED(hrgb)) {
        hrgb = bunk->lpVtbl->QueryInterface(bunk, &my_IID_IDXGISurface, (void **)&surf);
        bunk->lpVtbl->Release(bunk);
    }
    if (FAILED(hrgb)) {
        IDXGIDevice_Release(xdgi);
        return 0;
    }
    // plain CreateDxgiSurfaceRenderTarget rejects FLIP-model composition swapchains;
    // use the d2d1_1 device-context path via the real C macros (mingw provides them):
    ID2D1Device *d2dev = NULL;
    ID2D1DeviceContext *dctx = NULL;
    ID2D1Bitmap1 *d2bmp = NULL;
    HRESULT hr2 = ID2D1Factory1_CreateDevice((ID2D1Factory1 *)g_d2f, xdgi, &d2dev);
    if (SUCCEEDED(hr2)) hr2 = ID2D1Device_CreateDeviceContext(d2dev, D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dctx);
    if (SUCCEEDED(hr2)) {
        D2D1_BITMAP_PROPERTIES1 bp1;
        memset(&bp1, 0, sizeof(bp1));
        bp1.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bp1.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bp1.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        hr2 = ID2D1DeviceContext_CreateBitmapFromDxgiSurface(dctx, surf, &bp1, &d2bmp);
    }
    // mingw C mode has no ID2D1Image typedef; SetTarget takes it as void* vtbl-wise
    if (SUCCEEDED(hr2)) {
        ID2D1DeviceContext_SetTarget(dctx, (void *)d2bmp);
        union { ID2D1Image *img; void *v; } tchk; // sidestep the C typedef gap
        tchk.img = NULL;
        ID2D1DeviceContext_GetTarget(dctx, &tchk.img);

        char _b[80];
        sprintf(_b, "bmp=%p target=%p", (void *)d2bmp, (void *)tchk.img);
        writeLogA(_b);
        if (tchk.img) ((IUnknown *)tchk.img)->lpVtbl->Release((IUnknown *)tchk.img);
    }
    if (d2bmp) ((IUnknown *)d2bmp)->lpVtbl->Release((IUnknown *)d2bmp);
    IDXGISurface_Release(surf);
    IDXGIDevice_Release(xdgi);
    if (d2dev) ((IUnknown *)d2dev)->lpVtbl->Release((IUnknown *)d2dev);
    if (FAILED(hr2)) return 0;
    g_rt = (ID2D1RenderTarget *)dctx; // DC shares the RT vtbl prefix; macros in paint() stay valid
    g_rtW = w; g_rtH = h;
    return 1;
}

static int d2dInit(HWND hwnd) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&g_d2f))) return 0;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&g_dwf))) return 0;
    if (FAILED(g_dwf->lpVtbl->CreateTextFormat(g_dwf, g_cfg.fontFamily, NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            (FLOAT)(g_cfg.fontSize * g_scale), L"en-us", &g_tf))) return 0;
    IDWriteTextFormat_SetTextAlignment(g_tf, DWRITE_TEXT_ALIGNMENT_LEADING);

    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &g_d3d, &fl, NULL))) return 0;

    RECT rc; GetClientRect(hwnd, &rc);
    if (!buildRenderTarget(hwnd, rc.right - rc.left, rc.bottom - rc.top)) return 0;

    IDXGIDevice *xdgi = NULL;
    if (FAILED(ID3D11Device_QueryInterface(g_d3d, &my_IID_IDXGIDevice, (void **)&xdgi))) return 0;
    HRESULT hd = DCompositionCreateDevice(xdgi, &my_IID_IDCompositionDevice, (void **)&g_dc);
    IDXGIDevice_Release(xdgi);
    if (FAILED(hd) || !g_dc) return 0;
    if (FAILED(g_dc->lpVtbl->CreateTargetForHwnd(g_dc, hwnd, TRUE, &g_target))) return 0;
    if (FAILED(g_dc->lpVtbl->CreateVisual(g_dc, &g_vis))) return 0;
    g_vis->lpVtbl->SetContent(g_vis, (IUnknown *)g_swap);
    g_target->lpVtbl->SetRoot(g_target, g_vis);
    g_dc->lpVtbl->Commit(g_dc);
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

static void addChip(int type, int customIdx, const wchar_t *text, int warn, const wchar_t *colorOverride) {
    if (g_chipCount >= MAX_CHIPS) return;
    Chip *c = &g_chips[g_chipCount++];
    c->type = type; c->customIdx = customIdx; c->warn = warn;
    c->colorOverride = colorOverride;
    lstrcpynW(c->text, text ? text : L"", 96);
    c->r.left = c->r.right = c->r.top = c->r.bottom = 0;
}

static void buildChips(void) {
    g_chipCount = 0;
    if (g_cfg.shortcutEnabled) {
        const wchar_t *label = (g_cfg.shortcutLabel && *g_cfg.shortcutLabel) ? g_cfg.shortcutLabel : L"run";
        addChip(CT_SHORTCUT, 0, label, 0, NULL);
    }
    if (g_cfg.petEnabled) {
        wchar_t txt[96] = L"pet";
        if (g_cfg.petLabel && *g_cfg.petLabel) lstrcpynW(txt, g_cfg.petLabel, 80);
        lstrcatW(txt, g_m.petRunning ? L" on" : L" off");
        addChip(CT_PET, 0, txt, 0, NULL);
    }
    for (int i = 0; i < g_cfg.customCount; i++) {
        CustomChip *cc = &g_cfg.custom[i];
        if (!cc->enabled) continue;
        wchar_t txt[96] = L"";
        if (cc->icon && *cc->icon) { lstrcpynW(txt, cc->icon, 48); lstrcatW(txt, L" "); }
        if (cc->label && *cc->label) lstrcatW(txt, cc->label);
        if (cc->toggle) lstrcatW(txt, customStateGet(i) ? L" on" : L" off");
        if (!txt[0]) lstrcpynW(txt, L"chip", 96);
        addChip(CT_CUSTOM, i, txt, 0, cc->color);
    }
    wchar_t v[48];
    if (g_cfg.mGpu) { swprintf(v, 48, L"gpu %d%%", (int)(g_m.gpu + 0.5)); addChip(CT_GPU, 0, v, 0, NULL); }
    if (g_cfg.mCpu) {
        int hot = g_cfg.cpuWarnAt > 0 && g_m.cpu >= g_cfg.cpuWarnAt;
        swprintf(v, 48, L"cpu %d%%", (int)(g_m.cpu + 0.5));
        addChip(CT_CPU, 0, v, hot, hot ? g_cfg.warn : NULL);
    }
    if (g_cfg.mCpuTemp) {
        if (g_m.tempOk) {
            swprintf(v, 48, L"%d.%d\u00B0C", (int)g_m.tempC, (int)(g_m.tempC * 10) % 10);
            addChip(CT_CPUTEMP, 0, v, g_m.tempHot, g_m.tempHot ? g_cfg.warn : NULL);
        } else addChip(CT_CPUTEMP, 0, L"\u2014", 0, NULL);
    }
    if (g_cfg.mRam) {
        int hot = g_cfg.ramWarnAt > 0 && g_m.ram >= g_cfg.ramWarnAt;
        swprintf(v, 48, L"ram %d%%", (int)(g_m.ram + 0.5));
        addChip(CT_RAM, 0, v, hot, hot ? g_cfg.warn : NULL);
    }
    if (g_cfg.mVolume) { swprintf(v, 48, L"vol %d%%", g_m.volume); addChip(CT_VOLUME, 0, v, 0, NULL); }
    if (g_cfg.mBattery) {
        if (g_m.battValid) { swprintf(v, 48, L"batt %d%%", g_m.battPct); addChip(CT_BATTERY, 0, v, 0, NULL); }
        else addChip(CT_BATTERY, 0, L"batt ac", 0, NULL);
    }
    if (g_cfg.mClock) addChip(CT_CLOCK, 0, g_m.clockText, 0, NULL);
}

// ------------------------------------------------------------- painting ----
static FLOAT textWidth(const wchar_t *s) {
    if (!g_dwf || !g_tf) return 0;
    IDWriteTextLayout *lay = NULL;
    FLOAT hgt = (FLOAT)g_cfg.height * (FLOAT)g_scale;
    if (FAILED(g_dwf->lpVtbl->CreateTextLayout(g_dwf, s, (UINT32)wcslen(s), g_tf,
            10000.0f, hgt + 20, &lay))) return 0;
    DWRITE_TEXT_METRICS m;
    IDWriteTextLayout_GetMetrics(lay, &m);
    IDWriteTextLayout_Release(lay);
    return m.widthIncludingTrailingWhitespace;
}

static D2D1_COLOR_F colorFromHex(const wchar_t *hex, FLOAT alpha) {
    int c = hexToColorref(hex);
    if (c < 0) c = 0;
    D2D1_COLOR_F col;
    FLOAT r = (FLOAT)(c & 0xFF) / 255.0f, g = (FLOAT)((c >> 8) & 0xFF) / 255.0f, b = (FLOAT)((c >> 16) & 0xFF) / 255.0f;
    col.r = r * alpha; col.g = g * alpha; col.b = b * alpha; col.a = alpha; // premultiplied
    return col;
}

static void rebindVisual(void) {
    // after the swapchain is (re)created, the composition tree must point at it again
    if (!g_dc || !g_target || !g_vis || !g_swap) return;
    g_vis->lpVtbl->SetContent(g_vis, (IUnknown *)g_swap);
    g_target->lpVtbl->SetRoot(g_target, g_vis);
    g_dc->lpVtbl->Commit(g_dc);
}
static int g_inPaint = 0;
static void paint(HWND hwnd) {
    (void)hwnd;
    if (!g_rt || !g_swap || !g_dc) return;
    if (g_inPaint) return; // nested BeginDraw would corrupt D2D state
    g_inPaint = 1;
    RECT rc; GetClientRect(hwnd, &rc);
    FLOAT w = (FLOAT)(rc.right - rc.left), h = (FLOAT)(rc.bottom - rc.top);
    if (w < 1 || h < 1) return;

    D2D1_COLOR_F transparent = {0, 0, 0, 0};
    ID2D1RenderTarget_Clear(g_rt, &transparent);

    FLOAT ta = (FLOAT)g_cfg.backgroundAlpha / 255.0f;
    if (ta < 0) ta = 0; if (ta > 1) ta = 1;
    int solid = lstrcmpiW(g_cfg.backdrop, L"solid") == 0;
    D2D1_COLOR_F tint = colorFromHex(g_cfg.tint, solid ? 1.0f : ta);

    D2D1_MATRIX_3X2_F ident = { 1, 0, 0, 1, 0, 0 };
    D2D1_BRUSH_PROPERTIES bp = { 1.0f, ident };
    goto endpaint; /* BISECT-C: tint math only, no brushes */
    ID2D1SolidColorBrush *brush = NULL, *pinkBrush = NULL, *fgBrush = NULL, *warnBrush = NULL;
    ID2D1RenderTarget_CreateSolidColorBrush(g_rt, &tint, &bp, &brush);
    D2D1_COLOR_F pink = colorFromHex(g_cfg.pinkDeep, 0.35f);
    ID2D1RenderTarget_CreateSolidColorBrush(g_rt, &pink, &bp, &pinkBrush);
    D2D1_COLOR_F fg = colorFromHex(g_cfg.fg, 1.0f);
    ID2D1RenderTarget_CreateSolidColorBrush(g_rt, &fg, &bp, &fgBrush);
    D2D1_COLOR_F warn = colorFromHex(g_cfg.warn, 1.0f);
    ID2D1RenderTarget_CreateSolidColorBrush(g_rt, &warn, &bp, &warnBrush);
    if (!brush || !pinkBrush || !fgBrush || !warnBrush) goto endpaint;


    // tint wash over the acrylic backdrop
    D2D1_RECT_F full = { 0, 0, w, h };
    ID2D1RenderTarget_FillRectangle(g_rt, &full, (ID2D1Brush *)brush);

    // layout chips right-aligned
    FLOAT pad = 6.0f * (FLOAT)g_scale;
    FLOAT x = w - pad;
    for (int i = g_chipCount - 1; i >= 0; i--) {
        Chip *c = &g_chips[i];
        FLOAT tw = textWidth(c->text);
        FLOAT cw = tw + pad * 2;
        c->r.right = (LONG)x;
        c->r.left = (LONG)(x - cw);
        c->r.top = 0; c->r.bottom = (LONG)h;
        x -= cw + pad;
    }

    for (int i = 0; i < g_chipCount; i++) {
        Chip *c = &g_chips[i];
        if (i == g_hover) {
            D2D1_RECT_F hr = { (FLOAT)c->r.left, 2, (FLOAT)c->r.right, h - 2 };
            ID2D1RenderTarget_FillRectangle(g_rt, &hr, (ID2D1Brush *)pinkBrush);
        }
        const wchar_t *col = c->colorOverride;
        if (c->warn) col = g_cfg.warn;
        ID2D1SolidColorBrush *tb = fgBrush;
        ID2D1SolidColorBrush *own = NULL;
        if (col) {
            D2D1_COLOR_F oc = colorFromHex(col, 1.0f);
            ID2D1RenderTarget_CreateSolidColorBrush(g_rt, &oc, &bp, &own);
            tb = own;
        }
        FLOAT tw = textWidth(c->text);
        FLOAT tx = (FLOAT)c->r.left + pad;
        IDWriteTextLayout *lay = NULL;
        if (0 && g_dwf && g_tf && SUCCEEDED(g_dwf->lpVtbl->CreateTextLayout(g_dwf, c->text,
                (UINT32)wcslen(c->text), g_tf, w, h + 20, &lay))) {
            DWRITE_TEXT_METRICS m;
            ID2D1RenderTarget_BeginDraw(g_rt); /* never reached: bisect */
            FLOAT ty = (h - m.height) / 2.0f;
            D2D1_RECT_F tr = { tx, ty, tx + tw + 8, ty + m.height + 2 };
            /* TEXT-BISECT */ (void)tr;
            IDWriteTextLayout_Release(lay);
        }
        if (own) ID2D1SolidColorBrush_Release(own);
    }

endpaint:
    if (brush) ID2D1SolidColorBrush_Release(brush);
    if (pinkBrush) ID2D1SolidColorBrush_Release(pinkBrush);
    if (fgBrush) ID2D1SolidColorBrush_Release(fgBrush);
    if (warnBrush) ID2D1SolidColorBrush_Release(warnBrush);
    {
        D2D1_TAG t1 = 0, t2 = 0;
        HRESULT he = ID2D1RenderTarget_EndDraw(g_rt, &t1, &t2);
        HRESULT hp = IDXGISwapChain_Present((IDXGISwapChain *)g_swap, 1, 0);
        if (FAILED(he) || FAILED(hp)) {
            char _b[80];
            sprintf(_b, "paint end=%08lX present=%08lX chips=%d", (unsigned long)he, (unsigned long)hp, g_chipCount);
            writeLogA(_b);
        }
    }
    g_dc->lpVtbl->Commit(g_dc);
    g_inPaint = 0;
}

// ------------------------------------------------------- terminal follow ----
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

static void followTick(void) {
    // the foreground window wins when it is a supported terminal; otherwise
    // keep the last followed terminal, or probe for one (same rule as the
    // Electron build's probe order).
    HWND fg = GetForegroundWindow();
    if (isTerminalHwnd(fg)) g_term = fg;
    if (g_term && !IsWindow(g_term)) { g_term = NULL; g_haveTarget = 0; }
    if (!g_term) g_term = findTerminalByProbe();
    if (!g_term) { hideBar(); return; }

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

static void CALLBACK winEventProc(HWINEVENTHOOK h, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD tid, DWORD time) {
    (void)h; (void)hwnd; (void)idObject; (void)idChild; (void)tid; (void)time;
    if (event == EVENT_SYSTEM_FOREGROUND) followTick();
}

// --------------------------------------------------------------- actions ----
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

static void chipClick(int idx) {
    Chip *c = &g_chips[idx];
    switch (c->type) {
    case CT_SHORTCUT: execCmd(g_cfg.shortcutCommand); break;
    case CT_PET: petToggle(); break;
    case CT_CUSTOM: {
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

// ------------------------------------------------------------------ tray ----
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

static void trayRemove(void) {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = 0;
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

// --------------------------------------------------------------- config ----
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

static void configCheckTick(void) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_cfgMtime) != 0) {
        g_cfgMtime = fa.ftLastWriteTime;
        loadConfig();
        applyBackdrop(g_bar);
        if (g_tf) { IDWriteTextFormat_Release(g_tf); g_tf = NULL; }
        g_dwf->lpVtbl->CreateTextFormat(g_dwf, g_cfg.fontFamily, NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            (FLOAT)(g_cfg.fontSize * g_scale), L"en-us", &g_tf);
        clockFmtReload();
        followTick();
        InvalidateRect(g_bar, NULL, FALSE);
    }
}

// ---------------------------------------------------------------- wndproc ----
static int chipAt(POINT p) {
    for (int i = 0; i < g_chipCount; i++) {
        if (PtInRect(&g_chips[i].r, p)) return i;
    }
    return -1;
}

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
        if (g_swap && wp != SIZE_MINIMIZED) {
            RECT rc; GetClientRect(hwnd, &rc);
            UINT cw = (UINT32)(rc.right - rc.left), ch = (UINT32)(rc.bottom - rc.top);
            if (cw && ch && (cw != g_rtW || ch != g_rtH)) {
                releaseRenderTarget();
                buildRenderTarget(hwnd, cw, ch);
                rebindVisual();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
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
            InvalidateRect(hwnd, NULL, FALSE);
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
            InvalidateRect(hwnd, NULL, FALSE);
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
        InvalidateRect(hwnd, NULL, FALSE);
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
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        APP_CLASS, L"Chocobar", WS_POPUP,
        -2000, -2000, 800, (int)(g_cfg.height * g_scale + 0.5),
        NULL, NULL, hInst, NULL);
    if (!g_bar) return 1;

    applyBackdrop(g_bar);
    if (!d2dInit(g_bar)) dbg("d2d init failed");

    SetTimer(g_bar, TIMER_METRICS, 1000, NULL);
    SetTimer(g_bar, TIMER_FOLLOW, 100, NULL);
    SetTimer(g_bar, TIMER_CONFIG, 1000, NULL);

    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                         NULL, winEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    (void)hook;

    trayAdd(g_bar);

    SendMessageW(g_bar, WM_TIMER, TIMER_METRICS, 0); // prime metrics + first paint
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
