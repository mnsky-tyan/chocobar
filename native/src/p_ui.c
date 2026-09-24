// ------------------------------------------------------------------ ui ----
// append-only diagnostics file; used for crashes and D2D failures only.
// The path follows the user profile (the config lives there too) - never a
// hardcoded home, or a release build on any other machine logs nowhere.
static void writeLogA(const char *s) {
    static int ensured = 0;
    wchar_t path[MAX_PATH], home[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) && home[0])
        swprintf(path, MAX_PATH, L"%ls\\.wizbar\\native.log", home);
    else {
        GetTempPathW(MAX_PATH, path);
        lstrcatW(path, L"chocobar-native.log");
    }
    if (!ensured) { // the folder does not exist on a first run
        wchar_t dir[MAX_PATH];
        lstrcpynW(dir, path, MAX_PATH);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) { *slash = 0; CreateDirectoryW(dir, NULL); }
        ensured = 1;
    }
    // FILE_APPEND_DATA puts every write at EOF, so one WriteFile per line is
    // what keeps the subs fetch threads from interleaving each other's lines,
    // and the shared handle is what stops a second open from failing outright
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    char line[512];
    int n = 0;
    while (s[n] && n < (int)sizeof(line) - 3) { line[n] = s[n]; n++; }
    line[n++] = '\r'; line[n++] = '\n';
    DWORD w;
    WriteFile(h, line, (DWORD)n, &w, NULL);
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
static HWND g_owner = NULL; // the bar's owner window (the followed terminal)
static int g_subsRotIdx = 0;    // rotating subs chip position
static DWORD g_subsRotTick = 0; // last rotation (GetTickCount)
static int g_subsRotSec = 60;   // subs.rotateSec, applied by loadConfig
static wchar_t g_subsRotTip[96]; // tooltip for the rotating subs chip
static int subsChipRotated(wchar_t *txt, int cb, wchar_t *tip, int tipCb);
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
    const wchar_t *tipOverride; // per-chip tooltip (rotating subs chip)
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
// ------------------------------------------------- command-output chips ----
// A custom chip with intervalMs > 0 runs its command on a background thread and
// shows the trimmed stdout. This is the escape hatch for any metric the bar has
// no reader for: one config entry and the command is the whole implementation.
// The poll NEVER runs on the UI thread - a command that takes a second must not
// hitch the bar, so the text is stored here and the chip just reads it.
static wchar_t g_customText[MAX_CUSTOM][96];
// the command's raw trimmed output, kept beside the formatted chip text: the
// warn band is a band on the captured NUMBER, and a format like "temp $v"
// parses as 0 inside the formatted string
static wchar_t g_customRaw[MAX_CUSTOM][64];
static volatile long long g_customNextPoll[MAX_CUSTOM];
static int g_customPollInit = 0;

// loadConfig() installs a new config generation the poll thread must not see
// half-written, so its reads pin the generation (cfgPin/cfgUnpin). This lock
// covers only the published text, which the poll thread writes from its own
// thread while the chip build reads it.
static CRITICAL_SECTION g_cfgCustomLock;
static void customLock(void) { EnterCriticalSection(&g_cfgCustomLock); }
static void customUnlock(void) { LeaveCriticalSection(&g_cfgCustomLock); }

static DWORD WINAPI customPollThread(LPVOID lp);

// One thread walks every command chip. It is started from loadConfig - the one
// point a startup, a config save and a tray Reload all go through - so a chip
// added while the bar is already running is polled without a restart.
static void customPollStart(void) {
    if (g_customPollInit) return;
    const Config *v = cfgPin();
    int any = 0;
    for (int i = 0; i < v->customCount; i++)
        if (v->custom[i].enabled && v->custom[i].intervalMs > 0) { any = 1; break; }
    cfgUnpin();
    if (!any) return;
    HANDLE h = CreateThread(NULL, 0, customPollThread, NULL, 0, NULL);
    if (!h) return;
    CloseHandle(h); // it runs until quit; the chips read its text, never its handle
    g_customPollInit = 1;
}

// run `command` through cmd.exe (so PATH lookup, pipes and redirects all work)
// and capture what it writes to stdout. Returns a trimmed, bounded copy.
static int customRunCapture(const wchar_t *command, wchar_t *out, int cch) {
    out[0] = 0;
    if (!command || !*command) return 0;
    wchar_t cmd[2048];
    swprintf(cmd, 2048, L"cmd.exe /c %ls", command);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    STARTUPINFOW si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = NULL;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    // Run the command in the user's profile, not the bar's own directory: the
    // bar can be launched from a UNC cwd (a WSL path, a network share) and
    // cmd.exe refuses to run at all with one - it just prints its cwd and exits,
    // which reads as "the chip never updates".
    wchar_t cwd[MAX_PATH]; cwd[0] = 0;
    DWORD cl = GetEnvironmentVariableW(L"USERPROFILE", cwd, MAX_PATH);
    if (!cl || cl >= MAX_PATH) cwd[0] = 0;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, cwd[0] ? cwd : NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return 0;
    }
    CloseHandle(wr); // the child owns it now; closing ours is what ends the pipe
    char buf[4096];
    DWORD got = 0;
    char accb[4000]; int accbLen = 0; // raw bytes, decoded once at the end
    // One deadline for the whole capture, checked on EVERY iteration. EOF on
    // the pipe only arrives once EVERY inherited write handle is gone, so a
    // command that either leaves a background child holding stdout (a
    // `start /b ...` chain, a daemon it spawned) or just keeps writing never
    // ends the pipe by itself. A blocking ReadFile would wedge this thread,
    // and with it every other command chip; so would checking the cap only
    // while the pipe happened to be empty, which let a chatty command spin
    // here at 100% of a core forever and never reach the kill net below.
    unsigned long long tStart = GetTickCount64();
    for (;;) {
        if (GetTickCount64() - tStart >= 5000) break;
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break; // EOF / broken pipe
        if (avail == 0) { Sleep(25); continue; }
        if (!ReadFile(rd, buf, sizeof(buf) - 1, &got, NULL) || got == 0) break;
        if (accbLen < (int)sizeof(accb) - 1) { // bound the copy, never the read
            int n = (int)got;
            if (n > (int)sizeof(accb) - 1 - accbLen) n = (int)sizeof(accb) - 1 - accbLen;
            memcpy(accb + accbLen, buf, (size_t)n);
            accbLen += n;
        }
        // a full buffer means more output is already waiting: yield so the
        // drain cannot spin the pipe at 100% of a core until the cap fires
        if (got >= sizeof(buf) - 1) Sleep(25);
    }
    accb[accbLen] = 0;
    CloseHandle(rd);
    // a redirected cmd.exe pipe carries the console's OEM bytes, not UTF-8, so
    // decode UTF-8 first and fall back to the OEM codepage when that pass
    // produced replacement characters (GBK on a Chinese-locale box would
    // otherwise render as U+FFFD and read 0 to the warn band)
    wchar_t acc[4096];
    int accLen = MultiByteToWideChar(CP_UTF8, 0, accb, accbLen, acc, 4096 - 1);
    if ((accLen <= 0 && accbLen > 0) || (accLen > 0 && wcschr(acc, L'\uFFFD')))
        accLen = MultiByteToWideChar(CP_OEMCP, 0, accb, accbLen, acc, 4096 - 1);
    if (accLen < 0) accLen = 0;
    acc[accLen] = 0;
    // never wait forever on a hung command: spend what is left of the same cap,
    // then kill it - an abandoned child would otherwise pile up one process per
    // poll on every chip that misbehaves
    unsigned long long spent = GetTickCount64() - tStart;
    if (WaitForSingleObject(pi.hProcess, spent >= 5000 ? 0 : (DWORD)(5000 - spent)) != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // one value per chip: leading whitespace and blank lines are skipped, then
    // the FIRST line wins - the chip is drawn single-line and the warn band
    // parses this same line with _wtof, so a two-line stdout must never reach
    // it. Its trailing spaces and tabs are trimmed off.
    wchar_t *p = acc;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
    int len = 0;
    while (p[len] && p[len] != L'\r' && p[len] != L'\n') len++;
    p[len] = 0; // everything after the first line is discarded
    while (len > 0 && (p[len-1] == L' ' || p[len-1] == L'\t')) p[--len] = 0;
    if (len <= 0) return 0;
    lstrcpynW(out, p, cch);
    return 1;
}

// apply the format around the captured value. "$v" is the placeholder; a format
// without it is used verbatim (the command printed its own text). Takes the
// strings, not the config entry: the poll thread owns its copy.
static void customApplyFormat(const wchar_t *f, const wchar_t *val, wchar_t *out, int cch) {
    if (!f || !*f) { lstrcpynW(out, val, cch); return; }
    const wchar_t *ph = wcsstr(f, L"$v");
    if (!ph) { lstrcpynW(out, f, cch); return; }
    int head = (int)(ph - f);
    if (head < 0) head = 0;
    if (head > cch - 1) head = cch - 1;
    wcsncpy(out, f, (size_t)head); out[head] = 0;
    wcsncat(out, val, (size_t)(cch - head - 1));
    wcsncat(out, ph + 2, (size_t)(cch - lstrlenW(out) - 1));
}

// Copy the fields this chip needs out of a pinned config generation, then run
// the command on the copy: a reload retires that generation the instant the pin
// is released, and this code keeps running on its own memory.
static int customChipCopy(int ci, wchar_t *command, int cchCmd, wchar_t *format, int cchFmt) {
    const Config *v = cfgPin();
    int ok = 0;
    if (ci >= 0 && ci < v->customCount) {
        const CustomChip *cc = &v->custom[ci];
        if (cc->enabled && cc->intervalMs > 0 && cc->command && *cc->command) {
            lstrcpynW(command, cc->command, cchCmd);
            lstrcpynW(format, cc->format ? cc->format : L"", cchFmt);
            ok = 1;
        }
    }
    cfgUnpin();
    return ok;
}

static void customPollOne(int ci) {
    wchar_t command[1024], format[256], val[512], txt[96];
    if (!customChipCopy(ci, command, 1024, format, 256)) return;
    if (!customRunCapture(command, val, 512)) return; // keep the last good text
    customApplyFormat(format, val, txt, 96);
    customLock();
    lstrcpynW(g_customText[ci], txt, 96);
    lstrcpynW(g_customRaw[ci], val, 64);
    customUnlock();
}

static int customIntervalMs(int ci) {
    const Config *v = cfgPin();
    int ms = ci >= 0 && ci < v->customCount ? v->custom[ci].intervalMs : 0;
    cfgUnpin();
    return ms;
}

static DWORD WINAPI customPollThread(LPVOID lp) {
    (void)lp;
    for (int i = 0; i < MAX_CUSTOM; i++) {
        int iv = customIntervalMs(i);
        if (iv > 0) customPollOne(i); // first read happens immediately
        g_customNextPoll[i] = GetTickCount64() + iv;
    }
    for (;;) {
        Sleep(100);
        unsigned long long now = GetTickCount64();
        const Config *v = cfgPin();
        int n = v->customCount;
        cfgUnpin();
        for (int i = 0; i < n; i++) {
            int iv = customIntervalMs(i);
            if (iv <= 0) continue;
            if (now < (unsigned long long)g_customNextPoll[i]) continue;
            g_customNextPoll[i] = (long long)now + iv;
            customPollOne(i);
        }
    }
    return 0;
}

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
// how many tokens the live scan folded in (the "+N" log line). Every
// displayed total is derived from the per-day buckets below, so the live scan
// keeps no totals of its own.
extern long long g_tokAllLive;
void tokLiveSeed(long long cacheMaxTs, long long cacheMtimeMs);
long tokLiveScan(void);
void tokLiveReset(void); // tokens.enabled off: forget cursors + live counters
long long subsFetchedEpochMs(void);
void subsCredits(int i, int *avail, int *total);
void tokLiveInit(void);

// newest ts + mtime seen in the cache file, for the live scan's seed boundary
static long long g_cacheMaxTsScan = 0;
static long long g_cacheMtimeScan = 0;
static DWORD g_cacheStamp[4];      // cache mtime+size stamp of the last full read
static int g_cacheReadDone = 0;    // a full read has completed at least once
static long long g_tokensToday = -1;
static int g_tokensTick = 0;

// ---- dashboard aggregation (filled by scanTokenCache) ----------------------
#define DASH_MAX_APPS 12
#define DASH_MAX_MODELS 64
#define DASH_MAX_DAYS 190
// displayed by-model rows (Electron capped at 7; this store has 13 models and
// the captain asked for the space to be used, so the table reaches further)
#define DASH_MODEL_ROWS 13
// token-cache record fields kept separately: the Electron dash shows the
// input/output/cache R/cache W/calls table columns, not one lumped sum
typedef struct { long long in, out, cr, cw, req; } TokAgg;
static TokAgg g_appAgg[DASH_MAX_APPS];
static char g_appName[DASH_MAX_APPS][20];
// tokens.labels maps a raw source key to its dashboard display name (e.g.
// "pi" -> "pi-wsl" when the store lives on WSL). The aggregation keys stay
// raw; only rendered text is swapped, and the row dot colour still keys on
// the raw name so an app keeps its colour.
static void appLabelW(int ai, wchar_t *out, int cb) {
    wchar_t key[24];
    MultiByteToWideChar(CP_UTF8, 0, g_appName[ai], -1, key, 24);
    for (int i = 0; i < g_cfg.tokLabelCount; i++) {
        if (lstrcmpiW(key, g_cfg.tokLabelKeys[i]) == 0) {
            lstrcpynW(out, g_cfg.tokLabelVals[i], cb);
            return;
        }
    }
    lstrcpynW(out, key, cb);
}
static int g_appCount = 0;
static char g_modelName[DASH_MAX_MODELS][48]; // "app|model", Electron byModel key
static char g_modelLabel[DASH_MAX_MODELS][48]; // first-seen raw model id (display)
static TokAgg g_modelAgg[DASH_MAX_MODELS];
static int g_modelCount = 0;
static TokAgg g_dayApp[DASH_MAX_DAYS][DASH_MAX_APPS]; // per-day per-app (day detail)
static long long g_tokWeek = 0, g_tokMonth = 0, g_tokAll = 0;
static long long g_dayTot[DASH_MAX_DAYS]; // [DASH_MAX_DAYS-1] = today
// wall clock as the epoch ms dashFmtTime renders back as local time
static long long dashWallNowMs(void) {
    SYSTEMTIME now; GetLocalTime(&now);
    FILETIME ft;
    SystemTimeToFileTime(&now, &ft); // treats fields as UTC: matches dashFmtTime
    return ((((long long)ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000;
}

static long long g_lastScanMs = 0;      // GetTickCount64 of the last scan
static long long g_lastScanEpoch = 0;   // wall clock of the last scan (dashWallNowMs)
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

// first boundary at or before ts: bnd[1] is today, bnd[DASH_MAX_DAYS] the
// oldest bucket, bnd[0] (tomorrow) rejects later-dated records. Returns 1 for
// today and one more per day back, or -1 outside the span.
static int aggDayIndex(long long ts, const long long *bnd) {
    int lo = 0, hi = DASH_MAX_DAYS, j = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (bnd[mid] <= ts) { j = mid; hi = mid - 1; } else lo = mid + 1;
    }
    return j;
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
    int j = aggDayIndex(ts, bnd);
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

// local midnight the day-indexed arrays are currently framed at
static long long g_dashMidnight = 0;

// A local midnight makes every day-indexed array one bucket older. The live
// scan indexes each record with a freshly built boundary array, so only the
// heatmap arrays need aging here - on the unchanged-cache fast path this
// rollover is the only thing that moves them into today's frame.
static void dashDayRollover(void) {
    SYSTEMTIME st; GetLocalTime(&st);
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME fm; SystemTimeToFileTime(&st, &fm);
    long long mid = dashMidnightMs(&fm, 0);
    if (g_dashMidnight && mid > g_dashMidnight) {
        int days = (int)((mid - g_dashMidnight + 43200000LL) / 86400000LL);
        if (days >= DASH_MAX_DAYS) {
            memset(g_dayTot, 0, sizeof(g_dayTot));
            memset(g_dayApp, 0, sizeof(g_dayApp));
        } else if (days > 0) {
            // g_dayTot/g_dayApp run oldest-first ([DASH_MAX_DAYS-1] = today), so
            // the buckets move toward LOWER indices
            memmove(g_dayTot, g_dayTot + days, (DASH_MAX_DAYS - days) * sizeof(g_dayTot[0]));
            memset(g_dayTot + DASH_MAX_DAYS - days, 0, (size_t)days * sizeof(g_dayTot[0]));
            memmove(g_dayApp, g_dayApp + days, (DASH_MAX_DAYS - days) * sizeof(g_dayApp[0]));
            memset(g_dayApp + DASH_MAX_DAYS - days, 0, (size_t)days * sizeof(g_dayApp[0]));
        }
    }
    g_dashMidnight = mid;
}

// The four stat cards are derived from the SAME per-day buckets the heatmap
// draws: today is today's bucket, Last 7/Last 30 the in-window day sums, All
// time every bucket. Those buckets are aged at each local midnight, so a card
// can never keep counting a day that has already fallen out of the heatmap the
// way a cache-side base captured once did.
static void tokDeriveWindows(void) {
    long long today = g_dayTot[DASH_MAX_DAYS - 1];
    g_tokWeek = g_tokMonth = g_tokAll = 0;
    for (int i = 0; i < DASH_MAX_DAYS; i++) {
        long long v = g_dayTot[i];
        if (!v) continue;
        int back = DASH_MAX_DAYS - 1 - i;
        if (back < 7) g_tokWeek += v;
        if (back < 30) g_tokMonth += v;
        g_tokAll += v;
    }
    // -1 = nothing was read at all: the chip shows an em dash and the board
    // shows its empty state instead of a wall of zeros
    if (g_tokAll > 0 || g_tokensToday >= 0) g_tokensToday = today;
}

// today's raw (cache-exclusive) input+output, mirroring tokens.js; the
// Electron app rewrites this cache every rescan, we just read it
// every completed scan (data OR no-data) moves the version, so the open
// dashboard can repaint on change instead of on a fixed heartbeat
static void scanTokenCacheInner(void) {
    // tokens.enabled is the master switch: off = zero scans (the cache file is
    // never opened), no chip, and the board shows the master-off empty state
    if (!g_cfg.tokensEnabled) {
        g_tokensToday = -1;
        g_cacheReadDone = 0; // a later re-enable must re-read the seed
        // zero scans, zero dashboard data: drop every aggregate, and the live
        // cursors with them, so a re-enable is one clean full re-read instead
        // of resuming from cursors that skipped what arrived while off
        memset(g_dayTot, 0, sizeof(g_dayTot));
        memset(g_dayApp, 0, sizeof(g_dayApp));
        memset(g_appAgg, 0, sizeof(g_appAgg));
        memset(g_modelAgg, 0, sizeof(g_modelAgg));
        g_appCount = g_modelCount = 0;
        tokLiveReset();
        return;
    }
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
    // The Electron cache is a 10MB JSON: re-reading and needle-walking it on
    // every rescan is the single biggest CPU line in the whole bar. It only
    // changes when the Electron app writes it (rarely, now that it is retired),
    // so remember its size+mtime and skip the read while they are unchanged -
    // the live session scan below is the cheap incremental half that still
    // runs every rescan.
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { g_tokensToday = -1; return; }
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileInformationByHandle(h, &fi)) {
        g_cacheMtimeScan = (((long long)fi.ftLastWriteTime.dwHighDateTime) << 32 | fi.ftLastWriteTime.dwLowDateTime) / 10000 - 11644473600000LL;
        if (g_cacheReadDone && fi.ftLastWriteTime.dwLowDateTime == g_cacheStamp[0]
            && fi.ftLastWriteTime.dwHighDateTime == g_cacheStamp[1]
            && fi.nFileSizeLow == g_cacheStamp[2] && fi.nFileSizeHigh == g_cacheStamp[3]) {
            CloseHandle(h);
            return; // unchanged: keep the aggregate from the last full read
        }
    }
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
    long long total = 0;
    long long tsScanMax = 0; // newest ts in the cache (the live scan's boundary)
    memset(g_dayTot, 0, sizeof(g_dayTot));
    memset(g_appAgg, 0, sizeof(g_appAgg));
    memset(g_dayApp, 0, sizeof(g_dayApp));
    memset(g_modelAgg, 0, sizeof(g_modelAgg));
    g_appCount = 0;
    g_modelCount = 0;
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
        if (ts > tsScanMax) tsScanMax = ts;
        p = next ? next : end;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    g_tokensToday = total;
    g_lastScanMs = (long long)GetTickCount64();
    g_lastScanEpoch = dashWallNowMs();
    // remember the seed boundary so the live session scan only counts records
    // NEWER than anything the cache already holds (no double counting)
    g_cacheMaxTsScan = tsScanMax;
    g_cacheStamp[0] = fi.ftLastWriteTime.dwLowDateTime;
    g_cacheStamp[1] = fi.ftLastWriteTime.dwHighDateTime;
    g_cacheStamp[2] = fi.nFileSizeLow;
    g_cacheStamp[3] = fi.nFileSizeHigh;
    g_cacheReadDone = 1;
}



static DWORD g_tokScanStart = 0;
static void scanTokenCache(void) {
    g_tokScanStart = GetTickCount();
    dashDayRollover();
    // the Electron cache is the history seed: read it, then fold in whatever
    // the live session stores hold that is NEWER (p_tokens.c). Without the
    // live half every number freezes the moment the Electron app stops
    // writing the file - which is exactly what the captain saw.
    long long added = 0;
    scanTokenCacheInner();
    if (g_cfg.tokensEnabled && g_cfg.tokSrcCount) {
        tokLiveSeed(g_cacheMaxTsScan, g_cacheMtimeScan);
        added = tokLiveScan();
    }
    // the stat cards read the same day buckets the heatmap draws, so the two
    // can never disagree once the window slides past a midnight
    tokDeriveWindows();
    if (added > 0) {
        char lb[128];
        sprintf(lb, "[wizbar] token live scan: +%lld tokens (live all-time %lld), today=%lld", added, g_tokAllLive, g_tokensToday);
        writeLogA(lb);
    }
    g_tokDataVersion++;
    if (g_cfg.debug) { // how long the UI thread was blocked by this scan
        DWORD dt = GetTickCount() - g_tokScanStart;
        if (dt >= 15) {
            SYSTEMTIME nst; GetLocalTime(&nst);
            char lb[140];
            sprintf(lb, "[wizbar] token scan took %lu ms (blocked the bar) tick=%d period=%d at %02d:%02d:%02d",
                    dt, g_tokensTick, g_cfg.tokensRescanSec, nst.wHour, nst.wMinute, nst.wSecond);
            writeLogA(lb);
        }
    }
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

// uppercased window label, as the panel head shows it (subs.css text-transform)
static void subsWinUpper(const SubsWin *w, wchar_t *up, int cb) {
    int i;
    for (i = 0; w->label[i] && i < cb - 1; i++) {
        wchar_t ch = w->label[i];
        up[i] = (ch >= L'a' && ch <= L'z') ? (wchar_t)(ch - 32) : ch;
    }
    up[i] = 0;
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

// short type name for the debug chip dump. tokens/subs are CT_CUSTOM chips
// distinguished by their icon, exactly like the click handler does.
static const char *chipTypeName(const Chip *c) {
    if (c->type == CT_CUSTOM) {
        if (c->iconSvg == SVG_DIAMOND) return "tokens";
        if (c->iconSvg == SVG_GAUGE) return "subs";
        if (c->customIdx >= 0) return "custom";
        return "custom?";
    }
    switch (c->type) {
    case CT_SHORTCUT: return "shortcut";
    case CT_PET: return "pet";
    case CT_GPU: return "gpu";
    case CT_CPU: return "cpu";
    case CT_CPUTEMP: return "cputemp";
    case CT_RAM: return "ram";
    case CT_VOLUME: return "volume";
    case CT_BATTERY: return "battery";
    case CT_CLOCK: return "clock";
    default: return "?";
    }
}

static int g_dbgChipsN = -1; // re-dump whenever the chip set changes

static void buildChips(void) {
    g_chipCount = 0;
    // the scan period comes from tokens.rescanMinutes (seconds, 60..3600):
    // a hard-coded 30 ignored the config and scanned twice as often as asked
    if (++g_tokensTick >= g_cfg.tokensRescanSec) { g_tokensTick = 0; scanTokenCache(); }
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
    if (g_cfg.tokensEnabled) { // token chip (reads the Electron cache)
        wchar_t txt[32];
        fmtTokens(g_tokensToday, txt, 32);
        addChipI(CT_CUSTOM, -1, txt, 0, g_cfg.fgDim, 0);
        g_chips[g_chipCount - 1].align = 2;
        g_chips[g_chipCount - 1].iconSvg = SVG_DIAMOND;
        // bar.css #seg-tokens .ico { color: var(--yellow) }
        g_chips[g_chipCount - 1].iconColorOverride = g_cfg.yellow;
    }
    if (g_cfg.subsEnabled) {
        // The gauge chip is the bar's meter for the PLAN WEEK LIMITS: it shows
        // one plan's weekly remaining % at a time and rotates every
        // subs.rotateSec (default 60s), so "8% chatgpt" becomes "3% zcode"
        // becomes "100% gemini" without opening the board. The lowest
        // remaining window wins when a provider reports no weekly window.
        int stale = subsChipStale();
        wchar_t txt[16];
        const wchar_t *col;
        int rem = subsChipRem();
        if (rem == -1) {
            lstrcpynW(txt, L"\u2014", 16);
            col = g_cfg.fgDim;
        } else if (stale) {
            lstrcpynW(txt, L"stale", 16);
            col = g_cfg.fgDim;
        } else {
            rem = subsChipRotated(&txt[0], 16, g_subsRotTip, 96);
            col = rem > 70 ? g_cfg.good : rem > 30 ? g_cfg.fgDim : g_cfg.warn;
        }
        addChipI(CT_CUSTOM, -1, txt, 0, col, 0);
        g_chips[g_chipCount - 1].align = 2;
        g_chips[g_chipCount - 1].iconSvg = SVG_GAUGE;
        g_chips[g_chipCount - 1].tipOverride = g_subsRotTip;
    }
    // user-defined buttons: modules.custom[] entries become clickable chips
    // in the same left pinned group. Their icon resolves by name against the
    // built-in set first, then the config's theme.icons[]; a single character
    // falls back to a font glyph (a nerd-font codepoint still works).
    for (int ci = 0; ci < g_cfg.customCount; ci++) {
        CustomChip *cc = &g_cfg.custom[ci];
        if (!cc->enabled) continue;
        wchar_t txt[96];
        int warn = 0;
        int st = customStateGet(ci);
        customLock();
        if (cc->intervalMs > 0) {
            // command-output chip: the text is whatever the command last printed
            lstrcpynW(txt, g_customText[ci], 96);
            // warn band: outside [warnBelow, warnAbove] the text goes warn colour.
            // The band is on the command's RAW output, not the formatted text -
            // "temp $v" and "$v C" both parse as 0 in the formatted string. An
            // empty raw string means no poll has produced a value yet, and
            // _wtof reads THAT as 0 too, so the band would colour the chip from
            // the very first paint a "warnBelow" that 0 happens to fall under.
            if ((cc->warnAbove >= 0 || cc->warnBelow >= 0) && g_customRaw[ci][0]) {
                double v = _wtof(g_customRaw[ci]);
                if (cc->warnAbove >= 0 && v > cc->warnAbove) warn = 1;
                if (cc->warnBelow >= 0 && v < cc->warnBelow) warn = 1;
            }
        } else if (cc->toggle) swprintf(txt, 96, L"%ls %ls", cc->label ? cc->label : L"", st ? L"on" : L"off");
        else lstrcpynW(txt, cc->label ? cc->label : L"", 96);
        customUnlock();
        addChipI(CT_CUSTOM, ci, txt, warn, cc->color && *cc->color ? cc->color : NULL, 0);
        Chip *c = &g_chips[g_chipCount - 1];
        c->align = 2;
        // built-in name -> id
        int svg = -1;
        const wchar_t *ic = cc->icon ? cc->icon : L"";
        struct { const wchar_t *n; int id; } named[] = {
            { L"bolt", SVG_BOLT }, { L"bow", SVG_BOW }, { L"diamond", SVG_DIAMOND },
            { L"gauge", SVG_GAUGE }, { L"clock", SVG_CLOCK }, { L"gpu", SVG_GPU },
            { L"cpu", SVG_CPU }, { L"temp", SVG_TEMP }, { L"ram", SVG_RAM },
            { L"vol", SVG_VOL }, { L"mute", SVG_MUTE },
        };
        for (unsigned k = 0; k < sizeof(named) / sizeof(named[0]); k++)
            if (lstrcmpiW(ic, named[k].n) == 0) { svg = named[k].id; break; }
        if (svg < 0) svg = iconUserFind(ic); // config-defined icon
        if (svg >= 0) c->iconSvg = svg;
        else if (ic[0]) { // one glyph (or a U+XXXX codepoint) as a font icon
            wchar_t ch[2] = { ic[0], 0 };
            if (ic[0] == L'U' && ic[1] == L'+') {
                unsigned long cp = wcstoul(ic + 2, NULL, 16);
                if (cp >= 0x10000 && cp <= 0x10FFFF) {
                    unsigned v2 = cp - 0x10000;
                    ch[0] = (wchar_t)(0xD800 + (v2 >> 10));
                    ch[1] = (wchar_t)(0xDC00 + (v2 & 0x3FF));
                } else if (cp) ch[0] = (wchar_t)cp;
            }
            // a surrogate pair occupies one code point: encode via iconCp
            unsigned cp2 = ch[1] ? ((unsigned)(ch[0] - 0xD800) << 10) + (ch[1] - 0xDC00) + 0x10000
                                 : (unsigned)ch[0];
            c->iconCp = cp2;
        }
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
            // charging reads green (theme.good), like the Electron bar's
            // .seg-battery.on value, and green WINS over the low warning:
            // plugged in means the charge is rising, so red would be a lie
            addChipI(CT_BATTERY, 0, v, low, g_m.battAc ? g_cfg.good : (low ? g_cfg.warn : NULL), 0);
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
    if (bgA < 0) bgA = 0;
    if (bgA > 255) bgA = 255;
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

    // Electron geometry: bar padding 16px left / 18px right, 14px between
    // segments, 5px between icon and value (CSS px, x2 at this DPI). The
    // padding is what keeps the first/last chip off the window edge - the
    // rounded corners need it too.
    FLOAT hf = (FLOAT)g_dibH;
    FLOAT padL = 16.0f * (FLOAT)g_scale, padR = 18.0f * (FLOAT)g_scale;
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
    if (g_cfg.debug && g_chipCount != g_dbgChipsN) { // chip rects, for a click poster
        char lb[900]; int off = sprintf(lb, "[wizbar] chips:");
        for (int q = 0; q < g_chipCount && off < 860; q++)
            off += sprintf(lb + off, " [%s %ld..%ld]", chipTypeName(&g_chips[q]), g_chips[q].r.left, g_chips[q].r.right);
        writeLogA(lb);
        g_dbgChipsN = g_chipCount;
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
            if (pl < 0) pl = 0;
            if (pt < 0) pt = 0;
            if (prr > g_dibW) prr = g_dibW;
            if (pb > g_dibH) pb = g_dibH;
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
    // the command is config data (a shortcut or a custom chip) of any length:
    // copy it into the room that is left instead of appending it unbounded
    lstrcpynW(params + 3, cmd, 1200 - 3);
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
    if (!IsWindow(g_term)) {
        g_term = NULL; g_haveTarget = 0;
        // the owner window died: drop the pointer so the bar is never left
        // owned by a dead hwnd (it is hidden anyway)
        if (g_owner) { SetWindowLongPtrW(g_bar, GWLP_HWNDPARENT, 0); g_owner = NULL; }
        return;
    }

    // Own the bar by the terminal: an owned window rides in its owner's band,
    // stays out of the taskbar/Alt+Tab, and is hidden when the owner is
    // minimized. Only re-parent on an actual change - it is an expensive
    // cross-process operation that also repositions in z.
    if (g_owner != g_term) {
        LONG_PTR prev = SetWindowLongPtrW(g_bar, GWLP_HWNDPARENT, (LONG_PTR)g_term);
        if (prev || GetLastError() == 0) g_owner = g_term; // a failed set leaves the old owner
    }

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
    // The bar is pinned directly above the terminal in z so the two move
    // together: raise the terminal and the bar rides with it, cover the
    // terminal and the bar is covered too. Re-insert on drift (raising the
    // terminal walks it over the bar) - a plain z-order probe, no extra work
    // on the unchanged fast path.
    int drifted = !g_haveTarget || memcmp(&target, &g_lastTarget, sizeof(RECT)) != 0 || !g_barVisible;
    if (!drifted) {
        HWND below = GetWindow(g_bar, GW_HWNDPREV);
        drifted = (below != g_term);
    }
    if (drifted) {
        g_lastTarget = target;
        g_haveTarget = 1;
        SetWindowPos(g_bar, g_term, target.left, target.top, // insertAfter = the terminal: same band, immediately above it
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
static int g_dashContentH = 0;   // painted content bottom (device px) -> content-fit resize
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
// width of s as it will actually be DRAWN: GetTextExtentPoint32W ignores
// SetTextCharacterExtra, so measuring without it under-reports by
// len * extra and clips the last character of the widest label.
static int dashStrWEx(HDC dc, const wchar_t *s, HFONT f, int extra) {
    HGDIOBJ of = SelectObject(dc, f);
    int oe = SetTextCharacterExtra(dc, extra);
    SIZE ts; GetTextExtentPoint32W(dc, s, lstrlenW(s), &ts);
    SetTextCharacterExtra(dc, oe);
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
// refresh-button debounce: while set the button reads "..." and ignores
// clicks, so a restless double-click cannot fire two scans in a row
static unsigned long long g_btnBusyUntil = 0;
static long long g_subsBusyEpoch = 0;
static int g_btnHover = 0;            // 0 none, 1 refresh, 2 close
static int g_tbBottom = 0;            // titlebar bottom (physical px) for drag

// table columns: sized from the MEASURED widest value per column, not fixed
// CSS widths - the fixed widths were tuned on the old wide board and the
// narrower one let "10063" (calls) run into "2.84B" (cache R). need[] is
// indexed like xs: [0]=calls [1]=cacheW [2]=cacheR [3]=output [4]=input, in
// physical px, pre-measured by dashTableRowNeeds.
static void dashTableCols(int innerRight, int cacheR, int cacheW, const int *need, int *xs) {
    int x = innerRight;
    xs[0] = x; x -= need[0] + DX(12);
    if (cacheW && need[1] > 0) { xs[1] = x; x -= need[1] + DX(12); } else xs[1] = 0;
    if (cacheR && need[2] > 0) { xs[2] = x; x -= need[2] + DX(12); } else xs[2] = 0;
    xs[3] = x; x -= need[3] + DX(12);
    xs[4] = x;
}

// fold one row's formatted values into need[] (max width per column)
static void dashTableRowNeeds(HDC dc, HFONT f, const TokAgg *a, int *need) {
    wchar_t vs[32];
    int w;
    fmtTokens(a->in, vs, 32); w = dashStrW(dc, vs, f); if (w > need[4]) need[4] = w;
    fmtTokens(a->out, vs, 32); w = dashStrW(dc, vs, f); if (w > need[3]) need[3] = w;
    if (a->cr > 0) { fmtTokens(a->cr, vs, 32); w = dashStrW(dc, vs, f); if (w > need[2]) need[2] = w; }
    if (a->cw > 0) { fmtTokens(a->cw, vs, 32); w = dashStrW(dc, vs, f); if (w > need[1]) need[1] = w; }
    swprintf(vs, 32, L"%lld", a->req); w = dashStrW(dc, vs, f); if (w > need[0]) need[0] = w;
}
static void dashTableHeadNeeds(HDC dc, HFONT f, int *need) {
    static const wchar_t *hd[5] = { L"CALLS", L"CACHE W", L"CACHE R", L"OUTPUT", L"INPUT" };
    for (int i = 0; i < 5; i++) {
        int w = dashStrW(dc, hd[i], f);
        if (w > need[i]) need[i] = w;
    }
}

// one table row: share bar behind the first cell, dot, numbers right-aligned
static void dashTableRow(HDC dc, int x0, int innerW, int y, int rowH,
                         const int *xs, const wchar_t *label, COLORREF dot,
                         double share, TokAgg *a, DashTheme *t, HFONT f11, HFONT f9) {
    (void)f9;
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
    // the name takes every pixel left of the INPUT column: a fixed DX(150)
    // ellipsized real model ids ("xiaomi/mimo-x-flash...") even in a 500px card
    RECT lr = { x0 + 2 + DX(14), y, xs[4] - DX(6), y + rowH + 1 };
    DrawTextW(dc, label, -1, &lr, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
    wchar_t vs[32];
    fmtTokens(a->in, vs, 32); dashStrR(dc, xs[4], y, vs, t->fg, f11);
    fmtTokens(a->out, vs, 32); dashStrR(dc, xs[3], y, vs, t->fg, f11);
    if (xs[2]) { fmtTokens(a->cr, vs, 32); dashStrR(dc, xs[2], y, vs, t->fg, f11); }
    if (xs[1]) { fmtTokens(a->cw, vs, 32); dashStrR(dc, xs[1], y, vs, t->fg, f11); }
    swprintf(vs, 32, L"%lld", a->req); dashStrR(dc, xs[0], y, vs, t->fg, f11);
}

static void dashTableHead(HDC dc, int x0, int y,
                          const int *xs, DashTheme *t, HFONT f9, const wchar_t *firstCol) {
    dashStr(dc, x0 + 2, y, firstCol, t->dim, f9);
    wchar_t *in = L"INPUT", *out = L"OUTPUT", *cr = L"CACHE R", *cw = L"CACHE W", *ca = L"CALLS";
    dashStrR(dc, xs[4], y, in, t->dim, f9);
    dashStrR(dc, xs[3], y, out, t->dim, f9);
    if (xs[2]) dashStrR(dc, xs[2], y, cr, t->dim, f9);
    if (xs[1]) dashStrR(dc, xs[1], y, cw, t->dim, f9);
    dashStrR(dc, xs[0], y, ca, t->dim, f9);
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

// the board must never grow past the screen: the create path, the
// size-toggle path and the content-fit resize all bound the height here
static int dashMaxH(void) {
    return GetSystemMetrics(SM_CYSCREEN) - 40;
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
        // "..." while a refresh is in flight; keep the SAME measured width so
        // the button never reflows under the cursor mid-click
        const wchar_t *rf = GetTickCount64() < g_btnBusyUntil ? L"..." : L"refresh";
        int rw = dashStrW(dc, L"refresh", fBtn) + DX(18);
        g_btnRefresh.right = bx; g_btnRefresh.left = bx - rw;
        g_btnRefresh.top = ty - DX(3); g_btnRefresh.bottom = ty + DX(11) + DX(6);
        bx -= rw + DX(6);
        // Centre each label in its frame on BOTH axes. The frame is DX(20)
        // tall and GDI's tmHeight is the whole line box, so the old fixed
        // +DX(3) left the wording riding high in the frame (the "..."/"x"
        // glyphs read as pinned to the top edge). Measure the SAME font the
        // text is drawn with (fBtn), centre on the frame's mid-line.
        TEXTMETRICW tm2; memset(&tm2, 0, sizeof(tm2));
        HGDIOBJ ofm = SelectObject(dc, fBtn);
        GetTextMetricsW(dc, &tm2);
        SelectObject(dc, ofm);
        for (int bi2 = 1; bi2 <= 2; bi2++) {
            RECT *br = bi2 == 1 ? &g_btnRefresh : &g_btnClose;
            const wchar_t *tx = bi2 == 1 ? rf : cl;
            // Horizontal: the refresh frame is deliberately sized for the
            // word "refresh" so it never reflows under the cursor mid-click,
            // which left the busy "..." huddled at the LEFT edge of a wide
            // pill. Centre the advance width instead of padding DX(9).
            int txOff = ((br->right - br->left) - dashStrW(dc, tx, fBtn)) / 2;
            // Vertical: GDI's y is the top of the line box, so centre the
            // ink block (ascent+descent). The ellipsis is the exception -
            // its ink is ONLY the dots sitting on the baseline (about a
            // third of the descent tall), so centring the block left them
            // reading low; centre the baseline plus half a dot instead.
            int ell = (bi2 == 1 && rf[0] == L'.');
            int dotH = tm2.tmDescent / 3; if (dotH < 2) dotH = 2;
            int tyOff = ell ? ((br->bottom - br->top) / 2) - tm2.tmAscent + dotH / 2
                            : ((br->bottom - br->top) - (tm2.tmAscent + tm2.tmDescent)) / 2;
            if (g_btnHover == bi2) {
                dashCard(dc, br->left, br->top, br->right - br->left, br->bottom - br->top, t.bg, t.pink);
                dashStr(dc, br->left + txOff, br->top + tyOff, tx, t.pinkDeep, fBtn);
            } else {
                dashStr(dc, br->left + txOff, br->top + tyOff, tx, t.dim, fBtn);
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
    // Content-fit: every fit decision measures against the CONFIG height, not
    // the window's actual height, so the window can later shrink to its
    // painted content (g_dashContentH) without the shrink dropping a row and
    // oscillating.
    int vh = (int)((double)(g_dashType == 0 ? g_cfg.dashH : g_cfg.subsH) * g_scale + 0.5);

    if (g_dashType == 0) {
        // ---- token usage panel
        int innerW = w - 2 * padL;
        if (g_tokensToday < 0 && g_tokAll == 0) {
            dashCard(dc, padL, y, innerW, DX(38), t.card, t.divider);
            // Two very different reasons for an empty board: the master switch
            // is off (nothing is ever scanned) vs the cache file is unreadable.
            const wchar_t *why = g_cfg.tokensEnabled
                ? L"No token data. The token cache file could not be read \x2014 start Chocobar, or check tokens.cachePath."
                : L"Token usage is off. Set tokens.enabled to true in the config to scan again.";
            dashStr(dc, padL + DX(14), y + DX(10), why, t.dim, fBody);
            g_dashContentH = y + DX(38) + DX(14); // fit the empty board to its single card
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


            // daily usage section: heatmap + legend (+ optional day detail)
            int rowLabW = DX(16) + DX(5);
            int weeks = 26; // heatmapWeeks is Electron-only; the native board is fixed at 26
            int secPadX = DX(12);
            // Fill the card: 26 cells at a fixed DX(11) left the right half of
            // the card blank, which is most of why the board read as cramped.
            // Grow the cell to the available width, capped so it never turns
            // into chunky bricks.
            int cgap = DX(3);
            int cell = (innerW - 2 * secPadX - rowLabW) / weeks - cgap;
            if (cell > DX(16)) cell = DX(16);
            if (cell < DX(9)) cell = DX(9);
            int pitch = cell + cgap;
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
                int room = vh - DX(24) - gap - tblReserve - y - secH;
                int fitRows = (room - ddFixed) / DX(17);
                if (fitRows < 0) fitRows = 0;
                if (ddRows > fitRows) ddRows = fitRows;
                if (ddRows <= 0) hasSel = 0;
            }
            int ddH = hasSel ? ddFixed + ddRows * DX(17) : 0;
            if (y + secH < vh - DX(26)) {
                // fit gate stays on the no-selection height: the detail expands
                // the card only while the expansion fits, else it is clipped -
                // a selection must never collapse the section (the hit rects
                // would go stale and keep firing)
                if (hasSel && y + secH + ddH < vh - DX(26)) secH += ddH;
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
                    // each cache column only while THAT column has data
                    int cacheR = 0, cacheW = 0;
                    for (int i = 0; i < g_appCount; i++) {
                        if (dayRows[i].cr > 0) cacheR = 1;
                        if (dayRows[i].cw > 0) cacheW = 1;
                    }
                    int xs[5];
                    int need[5] = { 0, 0, 0, 0, 0 };
                    for (int i = 0; i < g_appCount; i++) dashTableRowNeeds(dc, fBody, &dayRows[i], need);
                    dashTableHeadNeeds(dc, fS9, need);
                    dashTableCols(padL + innerW - secPadX, cacheR, cacheW, need, xs);
                    if (anyRec) {
                        dashTableHead(dc, padL + secPadX, ddy, xs, &t, fS9, L"APP");
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
                        appLabelW(i, an, 24);
                        dashDot(dc, padL + secPadX + 2, ddy + (DX(15) - DX(8)) / 2, DX(8), appDotColor(g_appName[i], &t));
                        SelectObject(dc, fBody);
                        SetTextColor(dc, t.fg);
                        RECT lr2 = { padL + secPadX + 2 + DX(14), ddy, padL + secPadX + DX(150), ddy + DX(15) + 1 };
                        DrawTextW(dc, an, -1, &lr2, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
                        wchar_t vs[32];
                        fmtTokens(a->in, vs, 32); dashStrR(dc, xs[4], ddy, vs, t.fg, fBody);
                        fmtTokens(a->out, vs, 32); dashStrR(dc, xs[3], ddy, vs, t.fg, fBody);
                        if (xs[2]) { fmtTokens(a->cr, vs, 32); dashStrR(dc, xs[2], ddy, vs, t.fg, fBody); }
                        if (xs[1]) { fmtTokens(a->cw, vs, 32); dashStrR(dc, xs[1], ddy, vs, t.fg, fBody); }
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
            // desc and caps the model table at 7 rows; this store carries 13
            // distinct models, so the cap is DASH_MODEL_ROWS and the freed
            // rows fill what used to be blank space.
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
            if (mdlN > DASH_MODEL_ROWS) mdlN = DASH_MODEL_ROWS;
            // every slot the sort loop touches must be defined: init ALL
            // entries, sort the full set, cap only the displayed rows
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
            // cache columns are per TABLE and per COLUMN (Electron appCache vs
            // modelCache): an all-zero cacheW column is dropped, never drawn
            // empty. apps over every app, models over the displayed rows only
            int appCacheR = 0, appCacheW = 0;
            for (int i = 0; i < g_appCount; i++) {
                if (g_appAgg[i].cr > 0) appCacheR = 1;
                if (g_appAgg[i].cw > 0) appCacheW = 1;
            }
            int modelCacheR = 0, modelCacheW = 0;
            for (int i = 0; i < mdlN; i++) {
                if (g_modelAgg[mdlOrder[i]].cr > 0) modelCacheR = 1;
                if (g_modelAgg[mdlOrder[i]].cw > 0) modelCacheW = 1;
            }
            int rows = appN > mdlN ? appN : mdlN;
            // fit EVERY row: tighten the row pitch (17..19 CSS px) before
            // dropping one, so the model table never loses a row silently.
            // Zero rows is allowed: the section header alone still fits and
            // draws, so the tables never vanish without a trace.
            int budget = vh - DX(24) - y - secPad - th2 - secPad;
            int fitMax = budget / DX(17);
            if (rows > fitMax) rows = fitMax;
            if (rows < 0) rows = 0;
            if (rows > 0) {
                rowH = budget / rows;
                if (rowH > DX(19)) rowH = DX(19);
                if (rowH < DX(17)) rowH = DX(17);
            }
            int tblH = secPad + th2 + rows * rowH + secPad;
            if (y + tblH <= vh - DX(24)) {
                for (int side = 0; side < 2; side++) {
                    int sx = padL + side * (colW2 + DX(10));
                    dashCard(dc, sx, y, colW2, tblH, t.card, t.divider);
                    dashHead(dc, sx + DX(12), y + secPad, side == 0 ? L"BY APP" : L"BY MODEL", t.dim, fHead);
                    int inner = colW2 - DX(24);
                    int xs[5];
                    int cacheR = side == 0 ? appCacheR : modelCacheR;
                    int cacheW = side == 0 ? appCacheW : modelCacheW;
                    int need[5] = { 0, 0, 0, 0, 0 };
                    if (side == 0) {
                        for (int i = 0; i < g_appCount; i++) dashTableRowNeeds(dc, fBody, &g_appAgg[i], need);
                    } else {
                        for (int i = 0; i < g_modelCount; i++) dashTableRowNeeds(dc, fBody, &g_modelAgg[i], need);
                    }
                    dashTableHeadNeeds(dc, fS9, need);
                    dashTableCols(sx + DX(12) + inner, cacheR, cacheW, need, xs);
                    int ry = y + secPad + th2;
                    dashTableHead(dc, sx + DX(12), ry, xs, &t, fS9,
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
                            appLabelW(ai, an, 24);
                            long long s = g_appAgg[ai].in + g_appAgg[ai].out + g_appAgg[ai].cr + g_appAgg[ai].cw;
                            TokAgg a2 = g_appAgg[ai];
                            // even rows get the zebra wash (dash.css nth-child(even))
                            if (i & 1) {
                                HBRUSH b = CreateSolidBrush(t.zebra);
                                RECT fr2 = { sx + 1, ry, sx + colW2 - 1, ry + rowH };
                                FillRect(dc, &fr2, b);
                                DeleteObject(b);
                            }
                            dashTableRow(dc, sx + DX(12), inner, ry, rowH, xs, an,
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
                            dashTableRow(dc, sx + DX(12), inner, ry, rowH, xs, mn,
                                         dc2, (double)s / maxAll, &a2, &t, fBody, fS9);
                            ry += rowH;
                        }
                        if (mdlN == 0 && rows > 0) {
                            dashStr(dc, sx + DX(12), ry + DX(2), L"No model data.", t.dim, fBody);
                        } else if (g_modelCount > mdlRows) {
                            wchar_t more[32];
                            swprintf(more, 31, L"+%d more models", g_modelCount - mdlRows);
                            dashStr(dc, sx + DX(12), ry + DX(2), more, t.dim, fS10);
                        }
                    }
                }
                y += tblH + gap;
            }
        }
        // footer: last scan time, right-aligned under the content (not glued
        // to the window's bottom edge, which the content-fit resize moves).
        {
            wchar_t ftxt[40];
            // render the stored stamp directly: subtracting a TRUNCATED
            // GetTickCount64 age from the wall clock made the seconds field
            // oscillate (41 -> 42 -> 41) every repaint
            if (g_lastScanEpoch) dashFmtTime(g_lastScanEpoch, ftxt, 39);
            else lstrcpynW(ftxt, L"\x2014", 39);
            wchar_t fl[64];
            swprintf(fl, 63, L"last scan %ls", ftxt);
            int fy = y;
            if (fy > h - DX(16)) fy = h - DX(16);
            dashStrR(dc, w - padL, fy, fl, t.dim, fS9);
            g_dashContentH = fy + DX(10) + DX(12);
        }
    } else {
        // ---- subscriptions board: donut pies per plan window (subs.css)
        int innerW = w - 2 * padL;
        int pn = g_cfg.subsProviderCount; if (pn > MAX_SUBS) pn = MAX_SUBS;
        int shown = 0;
        // how many panels will actually draw: the board must show EVERY enabled
        // provider (0 = nothing configured, 5 is the supported max). The old
        // fixed-height rows fit only four on this screen, so the fifth was
        // silently dropped - count first, then size the row to fit.
        int en = 0;
        if (g_cfg.subsEnabled)
            for (int pi2 = 0; pi2 < pn; pi2++)
                if (subsProvEnabled(pi2)) en++;
        // ---- subscriptions board: one full-width panel per provider, that
        // provider's windows laid out left-to-right inside it.
        //
        // The old 2-column grid gave a 4-window provider (antigravity) half the
        // window's width and then silently DROPPED its last window when the
        // stacked pies stopped fitting - which is exactly why only three
        // antigravity rows appeared. Full-width rows with horizontal cells
        // show every window the provider reports.
        int headH = DX(40), footH = DX(26), bodyH = DX(112);
        // Compress the row (body first, then footer, then head - the pies keep
        // their share longest) until every enabled provider's panel fits the
        // screen. The floors keep a 5-panel board legible.
        if (en > 0) {
            int avail = dashMaxH() - y - DX(24);
            for (;;) {
                if (en * (headH + bodyH + footH) + (en - 1) * gap <= avail) break;
                if (bodyH > DX(72)) { bodyH -= DX(4); continue; }
                if (footH > DX(16)) { footH -= DX(2); continue; }
                if (headH > DX(28)) { headH -= DX(2); continue; }
                break; // nothing left to give: maxPanels below guards the rest
            }
        }
        int panelH = headH + bodyH + footH;
        // the stack is bounded by the SCREEN, not the window: the content-fit
        // grows the board to its content, so a panel that fits on screen must
        // still be drawn (clipping by the current height would drop panels the
        // board was about to grow enough to show). The stack is
        // n*(panelH+gap) - gap (no trailing gap), so the division has to add
        // the gap back: without it a 5-panel stack that the compression loop
        // just made fit still lost its last panel to the truncation.
        int maxPanels = (dashMaxH() - y - DX(24) + gap) / (panelH + gap);
        if (maxPanels < 1) maxPanels = 1;
        for (int pi2 = 0; pi2 < pn; pi2++) {
            if (!g_cfg.subsEnabled || !subsProvEnabled(pi2)) continue; // master off or disabled: no panel (Electron parity)
            if (shown >= maxPanels) {
                if (g_cfg.debug) {
                    char lb[128];
                    sprintf(lb, "[wizbar] subs board: %d of %d panels fit the screen (%d px body)",
                            maxPanels, pn, dashMaxH() - y - DX(24));
                    writeLogA(lb);
                }
                break;
            }
            wchar_t label[48];
            subsProvLabel(pi2, label, 48);
            SubsWin wins[MAX_GEN_WIN];
            int wn = subsProvWins(pi2, wins, MAX_GEN_WIN);
            int stale = wn < 0; if (wn < 0) wn = -wn;
            int px2 = padL;
            int py2 = y + shown * (panelH + gap);
            // panel: card + head (dot, name, pill) + body (cells) + foot
            dashCard(dc, px2, py2, innerW, panelH, t.card, t.divider);
            dashCard(dc, px2, py2, innerW, headH, t.head, 0);
            // dashed head underline
            HPEN dpen = CreatePen(PS_DOT, 1, t.divider);
            HGDIOBJ op = SelectObject(dc, dpen);
            MoveToEx(dc, px2, py2 + headH, NULL);
            LineTo(dc, px2 + innerW, py2 + headH);
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
                // absolute credits ride the head line (dashboard only): the chip
                // stays a percentage per the captain's rule
                wchar_t head2[80];
                int cav = 0, ctot = 0;
                subsCredits(pi2, &cav, &ctot);
                if (cav >= 0 && ctot > 0) {
                    wchar_t a2[24], b2[24];
                    fmtNum(cav, a2, 24); fmtNum(ctot, b2, 24);
                    swprintf(head2, 79, L"%ls \u00b7 %ls of %ls credits", plan, a2, b2);
                } else lstrcpynW(head2, plan, 79);
                int lx = px2 + DX(14) + DX(9) + DX(8) + dashStrW(dc, label, f13) + DX(8);
                dashStr(dc, lx, py2 + DX(14), head2, t.dim, fS10);
            }
            // status pill (subs.css .pill): stale / capped / near cap / ok
            {
                int lowest = 1000, anyw = 0;
                // rem -1 = the backend reports no fraction for this pool right
                // now (Claude/GPT between resets): the row exists but must not
                // drag the pill to CAPPED
                for (int k = 0; k < wn; k++) { if (wins[k].rem >= 0 && wins[k].rem < lowest) lowest = wins[k].rem; anyw = 1; }
                const wchar_t *ps2 = NULL; COLORREF pc = t.dim, pbg = blendCr(t.head, t.dim, 31);
                if (stale) { ps2 = L"STALE"; pc = t.yellow; pbg = blendCr(t.head, t.yellow, 31); }
                else if (!anyw) ps2 = NULL;
                else if (lowest <= 0) { ps2 = L"CAPPED"; pc = t.warn; pbg = blendCr(t.head, t.warn, 20); }
                else if (lowest <= 10) { ps2 = L"NEAR CAP"; pc = t.warn; pbg = blendCr(t.head, t.warn, 20); }
                else { ps2 = L"OK"; pc = t.good; pbg = blendCr(t.head, t.good, 20); }
                if (ps2) {
                    int pw2 = dashStrW(dc, ps2, fS10b) + DX(16);
                    int ph2 = DX(18);
                    int plx = px2 + innerW - DX(14) - pw2;
                    dashCard(dc, plx, py2 + (headH - ph2) / 2, pw2, ph2, pbg, pc);
                    SelectObject(dc, fS10b);
                    SetTextColor(dc, pc);
                    SetTextCharacterExtra(dc, DX(0.4));
                    RECT pr3 = { plx, py2 + (headH - ph2) / 2 + DX(2), plx + pw2, py2 + (headH - ph2) / 2 + ph2 };
                    DrawTextW(dc, ps2, -1, &pr3, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                    SetTextCharacterExtra(dc, 0);
                }
            }
            // body: one cell per window, left to right
            int bodyY = py2 + headH + DX(4);
            if (wn == 0) {
                dashStr(dc, px2 + DX(14), bodyY + DX(14), stale ? L"stale \x2014 no data yet" : L"no windows reported", t.dim, fBody);
            }
            int cols = wn > 0 ? wn : 1;
            int cellW = (innerW - DX(28)) / cols;
            // The pie takes only what is LEFT of the label: measure the widest
            // window label ("CLAUDE/GPT WEEK") and reserve exactly that plus a
            // gutter. A fixed pie size clipped antigravity's four windows to
            // "CLAUDE/C"; the pie then scales its pen and text off pieD.
            int labNeed = 0;
            for (int k = 0; k < wn; k++) {
                wchar_t up[20];
                subsWinUpper(&wins[k], up, 20);
                // the label is drawn with DX(0.7) character extra: measure the
                // same way or the widest label loses its last character
                int w2 = dashStrWEx(dc, up, f13, DX(0.7));
                if (w2 > labNeed) labNeed = w2;
            }
            int pieD = cellW - labNeed - DX(20);
            if (pieD > DX(96)) pieD = DX(96);
            if (pieD > bodyH - DX(8)) pieD = bodyH - DX(8);
            // legible floor: pen width and text geometry derive from pieD
            if (pieD < DX(24)) pieD = DX(24);
            float pieScale = (float)pieD / (float)DX(96);
            HFONT fPct = pieD >= DX(56) ? f18 : (pieD >= DX(38) ? f13 : fS10);
            for (int k = 0; k < wn; k++) {
                int kx = px2 + DX(14) + k * cellW;
                int ky = bodyY + (bodyH - pieD) / 2;
                int rem = wins[k].rem;
                int unk = rem < 0; // no fraction reported: em dash, empty ring
                if (rem < 0) rem = 0;
                if (rem > 100) rem = 100;
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
                // center: "N%" pinkDeep + "left", the ink centered in the ring
                wchar_t pctS[8];
                if (unk) swprintf(pctS, 7, L"\u2014"); else swprintf(pctS, 7, L"%d", rem);
                int nw = dashStrW(dc, pctS, fPct);
                int inkW = unk ? nw : nw + DX(1) + dashStrW(dc, L"%", fS10);
                int cx0 = kx + (pieD - inkW) / 2;
                int cyc = ky + pieD / 2;
                // centre the INK on the ring, not the glyph box: GDI puts the
                // ink ~11 CSS px below the draw origin, so the old fixed
                // offsets (15/11/6) left the number 3.5 CSS px low and the
                // caption dragged the pair lower still (measured +6.5 CSS off
                // centre in the captain's screenshot)
                dashStr(dc, cx0, cyc - DX(18.5), pctS, t.pinkDeep, fPct);
                if (!unk) dashStr(dc, cx0 + nw + DX(1), cyc - DX(14.5), L"%", t.pinkDeep, fS10);
                dashStr(dc, kx + (pieD - dashStrW(dc, L"left", fS9)) / 2, cyc + DX(2.5), L"left", t.dim, fS9);
                // meta right of the pie, clipped to its own cell
                int mx = kx + pieD + DX(12);
                wchar_t up[20];
                subsWinUpper(&wins[k], up, 20);
                SelectObject(dc, f13);
                SetTextColor(dc, t.fg);
                SetTextCharacterExtra(dc, DX(0.7));
                // meta text tracks the pie: the same fractions of pieD the
                // uncompressed board (pieD 96) lays out at 24 / 50 / 66
                int metaY = ky + (int)(pieD * 0.25f);
                RECT mr3 = { mx, metaY, kx + cellW - DX(4), metaY + DX(20) };
                DrawTextW(dc, up, -1, &mr3, DT_SINGLELINE | DT_LEFT);
                SetTextCharacterExtra(dc, 0);
                wchar_t usedLine[72];
                if (wins[k].used >= 0 && wins[k].total > 0) {
                    // the vendor reports absolute credits (zcode quota/limit):
                    // show what is LEFT, which is the number that matters, and
                    // not just the percentage of it
                    wchar_t lf[24], ts3[24];
                    long long left = (long long)wins[k].total - wins[k].used;
                    if (left < 0) left = 0;
                    fmtNum(left, lf, 24);
                    fmtNum(wins[k].total, ts3, 24);
                    swprintf(usedLine, 71, L"%ls left of %ls", lf, ts3);
                } else if (wins[k].pct < 0) swprintf(usedLine, 71, L"reset-only pool"); // no fraction reported
                else swprintf(usedLine, 71, L"%d%% used", wins[k].pct);
                dashStr(dc, mx, ky + (int)(pieD * 0.52f), usedLine, t.dim, fS10);
                // third line: relative reset time (subs.js fmtReset)
                wchar_t rst[40];
                fmtReset(wins[k].resetAt, rst, 40);
                dashStr(dc, mx, ky + (int)(pieD * 0.69f), rst, t.dim, fS10);
            }
            // panel foot: dashed top + fetched time right
            int fy = py2 + panelH - footH;
            HPEN fpen = CreatePen(PS_DOT, 1, t.divider);
            HGDIOBJ op2 = SelectObject(dc, fpen);
            MoveToEx(dc, px2 + DX(14), fy, NULL);
            LineTo(dc, px2 + innerW - DX(14), fy);
            SelectObject(dc, op2);
            DeleteObject(fpen);
            // render the stored cycle stamp directly (see subsFetchedEpochMs):
            // deriving it from a truncated tick age made it oscillate
            long long fepoch = subsFetchedEpochMs();
            wchar_t ftim[16];
            if (fepoch) dashFmtTime(fepoch, ftim, 15);
            else lstrcpynW(ftim, L"\x2014", 15);
            wchar_t fl2[32];
            swprintf(fl2, 31, L"%ls", ftim);
            dashStrR(dc, px2 + innerW - DX(14), fy + DX(6), fl2, t.dim, fS10);
            shown++;
        }
        if (!shown) {
            dashStr(dc, padL, y, g_cfg.subsEnabled ? L"No providers enabled." : L"Subscriptions are off (subs.enabled).", t.dim, fBody);
        }
        // panels are fixed-height rows, so the stack's bottom is the content edge
        if (shown) g_dashContentH = y + shown * panelH + (shown - 1) * gap + DX(14);
        // 0 providers: the board still shrinks to its single "not configured"
        // line (it used to keep the previous board's height)
        else g_dashContentH = y + DX(38) + DX(14);
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
                    appLabelW(i, an, 24);
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

// Latch the dash window to its painted content. The window is created at the
// config height and the content is laid out from the top, so a board shorter
// than that leaves a trailing blank. Resizing AFTER the window is on screen
// reads as "bolted together" (the top appears, then the lower part lags in),
// so the create path paints while hidden and fits before ShowWindow.
static int g_dbgFitLogged = 0;
static void dashFitToContent(void) {
    if (!g_dash || !IsWindow(g_dash) || g_dashContentH <= 0) return;
    RECT dr; GetWindowRect(g_dash, &dr);
    int curH = dr.bottom - dr.top;
    int want = g_dashContentH;
    int lo = (int)(g_dashType == 0 ? g_cfg.dashH : g_cfg.subsH) * g_scale * 45 / 100;
    if (want < lo) want = lo;
    // never leave the screen: a board that grows past the bottom edge has no
    // size grip and no scroll, so its last panel would be unreachable
    int hi = dashMaxH();
    if (want > hi) want = hi;
    if (want < curH - DX(4) || want > curH + DX(4)) {
        int nx = dr.left, ny = dr.top;
        HMONITOR mon = MonitorFromWindow(g_dash, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            int bottom = ny + want;
            if (bottom > mi.rcWork.bottom) ny = mi.rcWork.bottom - want;
            if (ny < mi.rcWork.top) ny = mi.rcWork.top;
        }
        SetWindowPos(g_dash, NULL, nx, ny, dr.right - dr.left, want, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_cfg.debug && !g_dbgFitLogged) {
        char lb[160];
        sprintf(lb, "[wizbar] dash fit: type=%d contentH=%d cfgH=%d curH=%d want=%d lo=%d",
                g_dashType, g_dashContentH, (int)((g_dashType == 0 ? g_cfg.dashH : g_cfg.subsH) * g_scale),
                curH, want, lo);
        writeLogA(lb);
        g_dbgFitLogged = 1;
    }
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
        if (g_btnBusyUntil && (subsFetchedEpochMs() != g_subsBusyEpoch
                               || GetTickCount64() > g_btnBusyUntil)) {
            g_btnBusyUntil = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (g_dashType == 1 || g_tokDataVersion != g_dashTokVersion) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paintDash(hwnd);
        EndPaint(hwnd, &ps);
        dashFitToContent(); // the window is on screen: shrink now, not on a tick
        return 0;
    }
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
        if (g_cfg.debug) {
            char lb[200];
            sprintf(lb, "[wizbar] dash click p=(%ld,%ld) rf=(%ld..%ld, %ld..%ld) cl=(%ld..%ld)",
                    p.x, p.y, g_btnRefresh.left, g_btnRefresh.right, g_btnRefresh.top, g_btnRefresh.bottom,
                    g_btnClose.left, g_btnClose.right);
            writeLogA(lb);
        }
        if (dashPtInBtn(p, &bi2)) {
            if (bi2 == 1) { // refresh: rescan tokens / refetch subs (Electron parity)
                unsigned long long nowt = GetTickCount64();
                if (nowt < g_btnBusyUntil) return 0; // already refreshing
                g_subsBusyEpoch = subsFetchedEpochMs();
                // subs completes on its worker thread (epoch change clears the
                // busy state); tokens runs inline, so it needs a cooldown FLOOR
                // - a warm scan finishes in ~150 ms and without the floor the
                // very next click would re-scan
                g_btnBusyUntil = nowt + (g_dashType == 0 ? 1500ull : 12000ull);
                InvalidateRect(hwnd, NULL, FALSE);
                UpdateWindow(hwnd); // paint the "..." before the blocking scan
                if (g_dashType == 0) scanTokenCache();
                else subsRefetchNow();
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
    case WM_ACTIVATE:
        if (g_cfg.debug) { char lb[80]; sprintf(lb, "[wizbar] dash WM_ACTIVATE wp=%d", (int)wp); writeLogA(lb); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); }
        return 0;
    case WM_RBUTTONUP:
        if (g_cfg.debug) writeLogA("[wizbar] dash WM_RBUTTONUP");
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_cfg.debug) writeLogA("[wizbar] dash WM_DESTROY");
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
        g_dbgFitLogged = 0;
        SetWindowTextW(g_dash, type == 0 ? L"Chocobar dashboard" : L"Chocobar subscriptions");
        // the other board has its own configured size: adopt it in place
        int tw = (int)((double)(type == 0 ? g_cfg.dashW : g_cfg.subsW) * g_scale + 0.5);
        int th2 = (int)((double)(type == 0 ? g_cfg.dashH : g_cfg.subsH) * g_scale + 0.5);
        int tsw = GetSystemMetrics(SM_CXSCREEN);
        if (tw > tsw - 40) tw = tsw - 40;
        if (th2 > dashMaxH()) th2 = dashMaxH();
        // HWND_TOP raises the board above the terminal and every other normal
        // window; SWP_NOACTIVATE keeps the keyboard with the terminal. Both are
        // needed - SetForegroundWindow alone stole the captain's keystrokes,
        // and SW_SHOWNA alone left the board sunk behind his windows.
        SetWindowPos(g_dash, HWND_TOP, (tsw - tw) / 2, (dashMaxH() + 40 - th2) / 2, tw, th2, SWP_NOACTIVATE);
        InvalidateRect(g_dash, NULL, FALSE);
        UpdateWindow(g_dash); // repaint + content-fit at the new size, once
        return;
    }
    scanTokenCache(); // fresh numbers for the panel
    g_dbgFitLogged = 0;
    int cw = (int)((double)(type == 0 ? g_cfg.dashW : g_cfg.subsW) * g_scale + 0.5);
    int ch = (int)((double)(type == 0 ? g_cfg.dashH : g_cfg.subsH) * g_scale + 0.5);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    if (cw > sw - 40) cw = sw - 40;
    if (ch > dashMaxH()) ch = dashMaxH();
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dashProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ChocobarDash";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    g_dashType = type;
    // TOOLWINDOW: the dashboard must NOT put a button in the taskbar (the
    // captain's ask - the blank default icon there read as a second app).
    // Tool windows still take the foreground normally, so clicking the board
    // keeps working; it just stays out of the taskbar and Alt-Tab.
    // WS_VISIBLE is deliberately absent: the first paint (and the content-fit
    // resize it triggers) happens while the window is still hidden, so the
    // captain sees ONE window at its final size instead of a board that grows
    // into place.
    // OWNED by the bar (the bar is itself owned by the followed terminal), with
    // NO WS_EX_TOOLWINDOW. Two reasons, both load-bearing:
    //   1. An owned popup never gets a taskbar button or an Alt-Tab entry - the
    //      captain's ask - so hiding the taskbar icon no longer costs anything.
    //   2. WS_EX_TOOLWINDOW made Windows SKIP this window when choosing the next
    //      window to activate: the moment the window above the board was
    //      minimized or closed, activation fell through to the terminal and
    //      Windows raised the TERMINAL over the dash. That is the "dashboard
    //      sinks to the bottom layer" bug, and it is documented in this repo's
    //      own history (main.js: the Electron dash is "A NORMAL window,
    //      deliberately"). As an owned normal window the dash stays a candidate
    //      and is raised with the terminal instead of being demoted below it.
    // Ownership also pins the board above the bar, and the bar above the
    // terminal, so it can never sink behind the window it follows.
    g_dash = CreateWindowExW(0, L"ChocobarDash", type == 0 ? L"Chocobar dashboard" : L"Chocobar subscriptions",
                             WS_POPUP, (sw - cw) / 2, (dashMaxH() + 40 - ch) / 2, cw, ch,
                             g_bar, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_dash) return;
    dashRoundCorners(g_dash);
    InvalidateRect(g_dash, NULL, FALSE);
    UpdateWindow(g_dash); // forces the WM_PAINT -> paint + fit, off screen
    // Raise the board to the top of the z-order WITHOUT activating it.
    // HWND_TOP lifts it above the terminal and every other normal window (the
    // captain's "it sinks behind my windows"), while SWP_NOACTIVATE keeps the
    // keyboard where he was typing - the old SetForegroundWindow here made his
    // next keystrokes (" like") land on the dashboard instead of his editor.
    // The bar stays topmost and above the board; a click on the board still
    // activates it (the buttons and title-drag need that), and Esc closes it.
    SetWindowPos(g_dash, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
    if (x < 0) x = 0;
    if (y < 0) y = 0;
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
        if (c->iconSvg == SVG_GAUGE) return c->tipOverride && *c->tipOverride ? c->tipOverride : L"Subscription plan remaining";
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

// ---- themed context menu --------------------------------------------------
// A stock TrackPopupMenu paints with the system menu colours and font, which
// reads as a stale gray window next to the beige/pink bar. Owner-drawing the
// rows and giving the menu a background brush themes the whole popup with the
// bar's palette and font (the same tokens the dashboards use).
typedef struct { const wchar_t *label; int cmd; } MenuItem;

// Build the chip's rotation list: one entry per enabled provider, that
// provider's WEEKLY window when it reports one (the vendor's own headline
// quota), else its lowest window. Advances one entry per subs.rotateSec.
// Returns the entry's remaining % and writes "<plan> <window> N% left".
static int subsChipRotated(wchar_t *txt, int cb, wchar_t *tip, int tipCb) {
    int pn = g_cfg.subsProviderCount; if (pn > MAX_SUBS) pn = MAX_SUBS;
    int pick[MAX_SUBS]; // (provider << 8) | window
    int n = 0;
    for (int i = 0; i < pn; i++) {
        if (!subsProvEnabled(i)) continue;
        SubsWin w[MAX_GEN_WIN];
        int wn = subsProvWins(i, w, MAX_GEN_WIN);
        if (wn < 0) wn = -wn;
        if (wn <= 0) continue;
        int best = -1;
        for (int k = 0; k < wn; k++) {
            if (wcsstr(w[k].label, L"week") || wcsstr(w[k].label, L"WEEK")) { best = k; break; }
            // prefer a row with a real fraction; rem -1 rows are reset-only
            if (best < 0 || (w[k].rem >= 0 && w[k].rem < w[best].rem)) best = k;
        }
        if (best >= 0 && n < MAX_SUBS) pick[n++] = (i << 8) | best;
    }
    if (!n) { if (tip) lstrcpynW(tip, L"No subscription windows", tipCb); if (txt) lstrcpynW(txt, L"\u2014", cb); return -1; }
    if (g_subsRotSec < 5) g_subsRotSec = 5;
    // the set of providers with data shrinks between paints (one drops to no
    // data), so the persistent index is clamped to what this call filled
    if (g_subsRotIdx >= n) g_subsRotIdx %= n;
    DWORD now = GetTickCount();
    if (now - g_subsRotTick >= (DWORD)g_subsRotSec * 1000u) {
        g_subsRotTick = now;
        g_subsRotIdx = (g_subsRotIdx + 1) % n;
    }
    int pi2 = pick[g_subsRotIdx] >> 8, k = pick[g_subsRotIdx] & 0xFF;
    SubsWin w[MAX_GEN_WIN];
    int wn = subsProvWins(pi2, w, MAX_GEN_WIN);
    if (wn < 0) wn = -wn;
    if (k >= wn) k = 0;
    int rem = w[k].rem;
    // The chip stays a percentage (it is a meter); the absolute credit
    // count belongs on the board rows and in this tooltip, which is where the
    // captain asked to see it.
    int hasNum = (w[k].used >= 0 && w[k].total > 0);
    long long left = hasNum ? ((long long)w[k].total - w[k].used) : 0;
    if (left < 0) left = 0;
    if (tip) {
        wchar_t plan[24]; subsProvPlan(pi2, plan, 24);
        if (!plan[0]) lstrcpynW(plan, L"\u2014", 24);
        if (hasNum) {
            wchar_t lf[24], ts3[24];
            fmtNum(left, lf, 24);
            fmtNum(w[k].total, ts3, 24);
            swprintf(tip, tipCb, L"%ls %ls: %d%% left (%ls of %ls credits)", plan, w[k].label, rem, lf, ts3);
        } else {
            swprintf(tip, tipCb, L"%ls %ls: %d%% left", plan, w[k].label, rem);
        }
    }
    if (txt) {
        if (rem < 0) lstrcpynW(txt, L"\u2014", cb); // pool between resets
        else swprintf(txt, cb, L"%d%%", rem);
    }
    return rem;
}

static const MenuItem kMenuItems[] = {
    { L"Token dashboard",        10 }, { L"Subscription dashboard", 11 }, { NULL, 0 },
    { L"Start with Windows",      5 }, { NULL, 0 },
    { L"Edit config",              1 }, { L"Open config folder",      2 }, { NULL, 0 },
    { L"Check for updates",        6 }, { L"Reload chocobar",         3 }, { L"Quit chocobar", 4 }, { NULL, 0 },
};
#define MENU_N ((int)(sizeof(kMenuItems) / sizeof(kMenuItems[0])))

// ---- autostart ("Start with Windows") --------------------------------------
// The HKCU Run value is the runtime source of truth and the tray menu is its
// only control: no admin prompt (HKCU), no installer, survives reboots. The
// general.autoStart config key is the FRESH-INSTALL default (applied when the
// template is first written); the bar never rewrites the user's config, so the
// menu toggle sticks until the user edits the file themselves.
static const wchar_t *AUTOSTART_NAME = L"Chocobar";

static int autoStartEnabled(void) {
    wchar_t val[MAX_PATH + 2];
    DWORD n = (DWORD)sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     AUTOSTART_NAME, RRF_RT_REG_SZ, NULL, val, &n) != ERROR_SUCCESS) return 0;
    return *val ? 1 : 0;
}

static void autoStartSet(int on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n && n < MAX_PATH) {
            wchar_t quoted[MAX_PATH + 3];
            swprintf(quoted, MAX_PATH + 3, L"\"%ls\"", exe);
            RegSetValueExW(k, AUTOSTART_NAME, 0, REG_SZ, (const BYTE *)quoted,
                           (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(k, AUTOSTART_NAME);
    }
    RegCloseKey(k);
}

// Menu width: the widest row label + padding + room for the autostart check
// at the RIGHT edge. A fixed DX(210) left the drawer wider than its content
// (the captain's "a little bit too big"); this tracks the labels instead.
static int menuWidthPx(void) {
    static int cached = 0;
    static double cachedScale = 0;
    if (cached && cachedScale == g_scale) return cached;
    HDC dc = GetDC(NULL);
    HFONT f = dashFont(9, FW_NORMAL);
    HGDIOBJ of = SelectObject(dc, f);
    int mx = 0;
    for (int i = 0; i < MENU_N; i++) {
        if (!kMenuItems[i].label) continue;
        SIZE ts; GetTextExtentPoint32W(dc, kMenuItems[i].label, lstrlenW(kMenuItems[i].label), &ts);
        if (ts.cx > mx) mx = ts.cx;
    }
    SelectObject(dc, of);
    DeleteObject(f);
    ReleaseDC(NULL, dc);
    cached = mx + DX(8) + DX(18) + DX(8); // label + gutter + check + right pad
    if (cached < DX(96)) cached = DX(96);
    cachedScale = g_scale;
    return cached;
}

static void showTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    // Same items as the Electron bar menu (main.js buildChocobarMenu); each row
    // carries its index as itemData so WM_MEASUREITEM/WM_DRAWITEM can theme it.
    for (int i = 0; i < MENU_N; i++) {
        if (kMenuItems[i].label)
            AppendMenuW(m, MF_OWNERDRAW | MF_STRING, (UINT_PTR)kMenuItems[i].cmd, (LPCWSTR)(UINT_PTR)i);
        else
            AppendMenuW(m, MF_OWNERDRAW | MF_SEPARATOR, 0, (LPCWSTR)(UINT_PTR)i);
    }
    DashTheme dt; dashResolveTheme(&dt);
    MENUINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = CreateSolidBrush(dt.bg); // leaks the brush by design: the
    // menu must keep a valid brush until dismissal (deleting it earlier paints
    // with a garbage handle). One 4-byte GDI object per opened menu.
    SetMenuInfo(m, &mi);
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(hwnd); // required for correct menu dismissal
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (id == 10) {
        dashToggle(0);
    } else if (id == 11) {
        dashToggle(1);
    } else if (id == 1) {
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
    } else if (id == 5) {
        autoStartSet(!autoStartEnabled()); // the menu IS the control
    } else if (id == 6) {
        // Opens the releases page rather than self-updating: no download, no
        // file swap, no unsigned-binary trust question. The bar knows its own
        // version (version.h), so the page is all the user needs to compare.
        SHELLEXECUTEINFOW sei; memset(&sei, 0, sizeof(sei)); sei.cbSize = sizeof(sei);
        sei.lpVerb = L"open";
        sei.lpFile = L"https://github.com/mnsky-tyan/chocobar/releases/latest";
        sei.nShow = SW_SHOWNORMAL;
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

// Tray mark: a minimal pink tile with the bar inside it - the app IS a bar,
// so the mark depicts one (a rounded pill). Pink on light or dark taskbars,
// and it survives 16px because the shape carries the mark, no fine detail.
// Alpha lives in the DIB, so no AND mask is needed.
static HICON makeBarIcon(int px) {
    const double pad = px * 0.10;         // tile margin, fraction of size
    const double r = px * 0.24;           // tile corner radius
    const double hx = px / 2.0 - pad;     // tile half extent
    const double bh = px * 0.105;         // pill corner radius (= half height)
    const double iw = px * 0.42;          // pill width
    const double base[3] = { 0xD4, 0x93, 0xAA };  // theme pinkDeep
    const double mark[3] = { 0xF7, 0xE2, 0xE9 };  // pill: near-white pink

    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = px;
    bi.bV5Height = -px;                    // top-down rows
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC sdc = GetDC(NULL);
    HBITMAP bm = CreateDIBSection(sdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, sdc);
    if (!bm || !bits) return NULL;
    unsigned *px32 = (unsigned *)bits;
    for (int y = 0; y < px; y++) {
        for (int x = 0; x < px; x++) {
            double cx = x + 0.5 - px / 2.0, cy = y + 0.5 - px / 2.0;
            // rounded-tile signed distance: each half-distance is clamped at
            // zero FIRST, or a negative axis inflates the length and the
            // sides pinch inward (petal silhouette)
            double qx = fabs(cx) - hx + r, qy = fabs(cy) - hx + r;
            double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            double d = sqrt(ox * ox + oy * oy) + (qx > qy ? qx : qy) - r;
            // 1px feather: alpha ramps across the edge, no jaggies at any size
            double a = 1.0 - (d - 0.5);
            if (a < 0) a = 0; else if (a > 1) a = 1;
            // the pill: a horizontal capsule inside the tile
            double bx = fabs(cx) - (iw / 2.0 - bh);
            double box = bx > 0 ? bx : 0;
            double dm = sqrt(box * box + cy * cy) - bh;
            double m = 1.0 - (dm - 0.5) / 0.8;
            if (m < 0) m = 0; else if (m > 1) m = 1;
            double c0 = base[0] + (mark[0] - base[0]) * m;
            double c1 = base[1] + (mark[1] - base[1]) * m;
            double c2 = base[2] + (mark[2] - base[2]) * m;
            unsigned A = (unsigned)(a * 255.0 + 0.5);
            unsigned R = (unsigned)(c0 + 0.5), G = (unsigned)(c1 + 0.5), B = (unsigned)(c2 + 0.5);
            px32[y * px + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = bm;
    ii.hbmMask = CreateBitmap(px, px, 1, 1, NULL);
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    DeleteObject(bm);                     // the icon keeps its own copy
    return h;
}

static void trayAdd(HWND hwnd) {
    if (g_trayAdded || !g_cfg.showTray) return;
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = makeBarIcon(32);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
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
            // content-fit: the dash windows are created at the config height and
            // paint their content from the top, so a data change that shrinks
            // the content leaves a trailing blank. Latch the window to the
            // painted content (measured against the config height, so this
            // cannot drop a row and re-shrink forever).
            dashFitToContent();
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
    // owner-drawn menu rows: same palette + font as the bar and dashboards
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        if (mi->CtlType != ODT_MENU) break;
        int idx = (int)mi->itemData;
        if (idx < 0 || idx >= MENU_N) { mi->itemWidth = menuWidthPx(); mi->itemHeight = DX(18); return TRUE; }
        if (!kMenuItems[idx].label) { mi->itemWidth = menuWidthPx(); mi->itemHeight = DX(5); }
        else { mi->itemWidth = menuWidthPx(); mi->itemHeight = DX(21); }
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        if (di->CtlType != ODT_MENU) break;
        int idx = (int)di->itemData;
        if (idx < 0 || idx >= MENU_N) break;
        DashTheme t; dashResolveTheme(&t);
        RECT r = di->rcItem;
        if (!kMenuItems[idx].label) { // separator: divider hairline
            HBRUSH bd = CreateSolidBrush(t.divider);
            RECT lr = { r.left + DX(10), r.top + r.bottom / 2 - 1, r.right - DX(10), r.top + r.bottom / 2 + 1 };
            FillRect(di->hDC, &lr, bd);
            DeleteObject(bd);
            return TRUE;
        }
        int sel = (di->itemState & ODS_SELECTED) ? 1 : 0;
        HBRUSH b = CreateSolidBrush(sel ? blendCr(t.bg, t.pink, 96) : t.bg);
        FillRect(di->hDC, &r, b);
        DeleteObject(b);
        if (sel) { // leading marker ties the row to the bar's own hover pill
            HBRUSH mb = CreateSolidBrush(t.pinkDeep);
            RECT mr = { r.left, r.top + DX(4), r.left + DX(2), r.bottom - DX(4) };
            FillRect(di->hDC, &mr, mb);
            DeleteObject(mb);
        }
        HFONT f = dashFont(9, FW_NORMAL);
        HGDIOBJ of = SelectObject(di->hDC, f);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, sel ? t.pinkDeep : t.fg);
        // "Start with Windows" carries a check at the RIGHT edge, clear of the
        // label: the Run value is the state, so the mark is read live
        int tick = (kMenuItems[idx].cmd == 5 && autoStartEnabled());
        if (tick) {
            HFONT fc = dashFont(9, FW_BOLD);
            SelectObject(di->hDC, fc);
            RECT cr = { r.right - DX(18), r.top, r.right - DX(6), r.bottom };
            DrawTextW(di->hDC, L"\u2713", -1, &cr, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
            SelectObject(di->hDC, f);
            DeleteObject(fc);
        }
        RECT tr = { r.left + DX(8), r.top, r.right - (tick ? DX(24) : DX(8)), r.bottom };
        DrawTextW(di->hDC, kMenuItems[idx].label, -1, &tr,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        SelectObject(di->hDC, of);
        DeleteObject(f);
        return TRUE;
    }
    // A hand cursor over the chips that actually do something (the pinned
    // toggles and the two dashboard openers); the plain arrow everywhere else.
    // Electron gets this from CSS cursor:pointer on .seg.clickable.
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            int ci = chipAt(p);
            int clickable = 0;
            if (ci >= 0) {
                Chip *c = &g_chips[ci];
                clickable = c->type == CT_SHORTCUT || c->type == CT_PET
                    || (c->type == CT_CUSTOM && (c->iconSvg == SVG_DIAMOND
                                              || c->iconSvg == SVG_GAUGE
                                              || c->customIdx >= 0));
            }
            SetCursor(LoadCursor(NULL, clickable ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
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
    // Right-click on the bar itself opens the same menu the tray icon does
    // (Electron: bar.js 'contextmenu' -> buildChocobarMenu). Without this the
    // bar is the only Chocobar surface with no menu at all.
    case WM_RBUTTONUP:
        showTrayMenu(hwnd);
        return 0;
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
    "  \"bar\": { \"height\": 30, \"gap\": 12, \"fontSize\": 10, \"fontFamily\": \"Segoe Print\", \"align\": 1,\r\n"
    "            \"backgroundTint\": \"#E8D8C3\", \"backgroundAlpha\": 120, \"backdrop\": \"acrylic\", \"radius\": 8 },\r\n"
    "  \"theme\": { \"fg\": \"#080808\", \"fgDim\": \"#5a5245\", \"pink\": \"#F0DEE4\", \"pinkDeep\": \"#D493AA\", \"pinkBg\": \"#FEF7F9\",\r\n"
    "              \"yellow\": \"#D8C77A\", \"warn\": \"#A00000\", \"good\": \"#006400\", \"divider\": \"#D9CCB2\",\r\n"
    "              \"iconColor\": \"#D493AA\", \"iconOpacity\": 90,\r\n"
    "              \"heatmap\": [\"#F1ECD8\", \"#F6D8E0\", \"#EFB7C7\", \"#E28FB0\", \"#C95E8F\"] },\r\n"
    "  \"dashboard\": { \"width\": 900, \"height\": 520 },\r\n"
    "  \"tokens\": { \"enabled\": false, \"appFilter\": [], \"cachePath\": \"\",\r\n"
    "    // sources[]: every session store the live scan reads, up to 8. Add a harness by\r\n"
    "    // adding an entry - nothing is compiled in. app = the aggregation key (and the\r\n"
    "    // labels key); path = the store; recursive descends into per-project subdirectories\r\n"
    "    // (default on, harmless for a flat store); fields renames the usage keys for a\r\n"
    "    // harness that spells them differently. Shipped off: set tokens.enabled and the\r\n"
    "    // source you want - nothing is read until you do.\r\n"
    "    \"sources\": [\r\n"
    "      { \"app\": \"pi\",   \"path\": \"~/.pi/agent/sessions\", \"enabled\": false, \"recursive\": true },\r\n"
    "      { \"app\": \"zai\",  \"path\": \"~/.zai/agent/sessions\", \"enabled\": false }\r\n"
    "    ],\r\n"
    "    \"labels\": { \"pi\": \"pi-wsl\" } },\r\n"
    "  \"modules\": {\r\n"
    "    \"gpu\": { \"enabled\": true },\r\n"
    "    \"cpu\":  { \"enabled\": true, \"warnAt\": 85 },\r\n"
    "    \"cputemp\": { \"enabled\": true, \"warnAt\": 85 },\r\n"
    "    \"ram\":  { \"enabled\": true, \"warnAt\": 90 },\r\n"
    "    \"volume\": { \"enabled\": true },\r\n"
    "    \"battery\": { \"enabled\": true },\r\n"
    "    \"clock\": { \"enabled\": true, \"format\": \"{MMM} {dd}  {HH}:{mm}\" },\r\n"
    "    \"shortcut\": { \"enabled\": false, \"label\": \"\", \"command\": \"\" },\r\n"
    "    \"pet\": { \"enabled\": false, \"label\": \"\", \"exePath\": \"\" },\r\n"
    "    \"custom\": [\r\n"
    "      { \"enabled\": false, \"icon\": \"\", \"label\": \"Example\", \"command\": \"notepad.exe\", \"toggle\": false },\r\n"
    "      // command-output chip: poll a command and show its stdout. format wraps the value\r\n"
    "      // ($v = the trimmed output); warnAbove / warnBelow colour it outside that band.\r\n"
    "      { \"enabled\": false, \"icon\": \"gpu\", \"label\": \"\",\r\n"
    "        \"command\": \"nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader\",\r\n"
    "        \"intervalMs\": 5000, \"format\": \"$v C\", \"warnAbove\": 80 }\r\n"
    "    ]\r\n"
    "  },\r\n"
    "  \"subs\": { \"enabled\": false, \"intervalMinutes\": 2, \"fetchTimeoutMs\": 20000,\r\n"
    "             \"width\": 880, \"height\": 580,\r\n"
    "             \"providers\": [\r\n"
    "               { \"type\": \"chatgpt\", \"enabled\": false, \"label\": \"ChatGPT\", \"authPath\": \"~/.codex/auth.json\" },\r\n"
    "               { \"type\": \"zai\", \"enabled\": false, \"label\": \"Z.ai\", \"configPath\": \"~/.zcode/v2/config.json\", \"provider\": \"builtin:zai-coding-plan\" },\r\n"
    "               // one entry = one panel with two rows: Gemini and Claude/GPT, straight from\r\n"
    "               // fetchAvailableModels on both Google endpoints (daily wins), the same source the\r\n"
    "               // harness's /quota uses - no IDE or language server required.\r\n"
    "               { \"type\": \"antigravity\", \"enabled\": false, \"authPath\": \"~/.pi/agent/auth.json\" },\r\n"
    "               // generic: ANY rest quota endpoint, declared entirely here. url + optional\r\n"
    "               // auth (a token from a file, an env var, or the config itself) + windows[]\r\n"
    "               // with JSON paths into the response. paths are $.a.b[0].c. A window needs\r\n"
    "               // any two of used / remaining / total; the third is derived.\r\n"
    "               { \"type\": \"generic\", \"enabled\": false, \"label\": \"MyPlan\",\r\n"
    "                 \"url\": \"https://api.example.com/v1/quota\", \"method\": \"GET\",\r\n"
    "                 \"auth\": { \"header\": \"Authorization\", \"prefix\": \"Bearer \",\r\n"
    "                            \"path\": \"~/.example/auth.json\", \"key\": \"access_token\" },\r\n"
    "                 \"headers\": { \"Accept\": \"application/json\" },\r\n"
    "                 \"windows\": [\r\n"
    "                   { \"label\": \"5h\",  \"used\": \"$.data.five_hour.used\",\r\n"
    "                     \"total\": \"$.data.five_hour.limit\", \"reset\": \"$.data.five_hour.resets_at\" },\r\n"
    "                   { \"label\": \"week\", \"remaining\": \"$.data.weekly.remaining\",\r\n"
    "                     \"total\": \"$.data.weekly.limit\" }\r\n"
    "                 ] }\r\n"
    "             ] },\r\n"
    "  \"terminal\": { \"className\": \"\", \"title\": \"\" },\r\n"
    "  \"general\": { \"showTray\": true, \"autoStart\": true }\r\n"
    "  // autoStart registers the HKCU Run value on FIRST run only; after that the\r\n"
    "  // tray menu's \"Start with Windows\" item is the control (the bar never rewrites this file)\r\n"
    "}\r\n";

void writeTemplate(void) {
    HANDLE h = CreateFileW(g_cfgPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    WriteFile(h, g_template, (DWORD)strlen(g_template), &wrote, NULL);
    CloseHandle(h);
}

void loadConfig(void) {
    int freshInstall = 0;
    DWORD len = 0;
    char *raw = readFileUtf8(g_cfgPath, &len);
    if (!raw) {
        writeTemplate();
        freshInstall = 1;
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
    Config *next = (Config *)HeapAlloc(GetProcessHeap(), 0, sizeof(Config));
    if (!next) { HeapFree(GetProcessHeap(), 0, toks); HeapFree(GetProcessHeap(), 0, raw); return; }
    parseConfigInto(next, raw, toks, 0);
    HeapFree(GetProcessHeap(), 0, toks);
    HeapFree(GetProcessHeap(), 0, raw);
    // Installing the new generation retires the old one instead of freeing it:
    // the provider fetch threads and the command poll both walk the live one.
    cfgInstall(next);
    g_cfgLoaded = 1;
    // A first run (template just written) registers the Run value per
    // general.autoStart; every later run leaves the registry to the menu
    if (freshInstall && g_cfg.autoStart) autoStartSet(1);
    g_iconOpacity = g_cfg.iconOpacity / 100.0; // p_icons AlphaBlend constant
    g_subsRotSec = g_cfg.subsRotateSec;
    // theme.icons[] -> the icon engine (a name defined again replaces its slot,
    // and a removed name simply stops resolving)
    for (int i = 0; i < g_cfg.iconCount; i++)
        iconUserAdd(g_cfg.icons[i].name, g_cfg.icons[i].d, (float)g_cfg.icons[i].w);
    // freeConfig above freed every string the chip array points at
    // (colorOverride / iconColorOverride): rebuild before any paint reads them
    buildChips();
    // A command chip added by this config (a save or a tray Reload, not only a
    // fresh start) gets its poll thread here - the one point every reload reaches
    customPollStart();
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
    writeLogA("chocobar " CB_VER_STR " start");
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
    // The command poll publishes its text under this lock, and the config swap
    // below retires generations the fetch threads may still hold.
    InitializeCriticalSection(&g_cfgCustomLock);
    InitializeCriticalSection(&g_cfgGenLock);
    // The cursor file MUST be loaded before the config: loadConfig() rebuilds
    // the chips, which runs the first token scan, and that scan is what
    // populates the cursors. Loading them afterwards wiped the in-memory set,
    // so the very next rescan re-read every active file from byte 0 and
    // DOUBLE COUNTED every live record (today jumped 2x within a minute).
    tokLiveInit();
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

    // NO WS_EX_TOOLWINDOW and NO WS_EX_TOPMOST. A tool window sits in a
    // band above ordinary windows, so it would keep floating over unrelated
    // windows no matter where SetWindowPos put it (the captain's screenshot
    // showed the bar painted across a Brave window while its terminal sat
    // behind it). Making the bar an OWNED window of the terminal instead keeps
    // it out of the taskbar/Alt+Tab and pins it in the terminal's own band.
    g_bar = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_LAYERED,
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
