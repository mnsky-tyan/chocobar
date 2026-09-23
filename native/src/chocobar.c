// chocobar.c - native Win32 Chocobar bar (v1.0 native rewrite, phase 1)
//
// One translation unit, zero runtime dependencies beyond Windows itself.
// Reads the SAME config file as the Electron build (%USERPROFILE%\.wizbar\
// config.json or --config <path>); keys it does not implement are ignored.
//
// Phase 1 scope: acrylic bar (DWM system backdrop + Direct2D), terminal
// follow with z-glue and move/size hands-off, chips (cpu/cputemp/ram/
// volume/battery/clock + shortcut/pet/custom), tray, hot-reload, template
// materialization, single instance, no-activate click behavior.
// Phase 2 (not here): token analytics + dashboards, subscription board,
// bluetooth battery, autostart writing.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define UNICODE
#define _UNICODE
#define INITGUID

#include <initguid.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dwrite.h>
#include <pdh.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wctype.h>
#include <stdint.h>
#include "jsmn.h"
#include "version.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define APP_CLASS   L"ChocobarBar"
#define APP_MUTEX   L"ChocobarSingleInstanceMutex"
#define WM_TRAY     (WM_APP + 1)
#define TIMER_METRICS 1
#define TIMER_FOLLOW  2
#define TIMER_CONFIG  3
// a live token source: the JSONL session store the in-process scan reads
typedef struct {
    int enabled;            // source flag inside tokens.sources.<name>
    const char *app;        // aggregate key ("pi", "zai") - matches the cache
    wchar_t *sessionsDir;   // ~ prefixed = profile relative, else absolute/UNC
} TokSource;

#define MAX_CUSTOM 16
#define MAX_SUBS 6
#define MAX_USER_ICONS 16

// a config-defined icon: name (referenced by modules.custom[].icon), SVG path
// data in the 24-unit viewBox the built-in icons use, and a stroke width.
typedef struct {
    wchar_t *name, *d;
    double w;
} UserIcon;

// ---------------------------------------------------------------- util ----
static void dbg(const char *fmt, ...) {
    (void)fmt;
}


// --------------------------------------------------------------- config ----
typedef struct {
    int enabled;
    wchar_t *icon, *label, *color, *title, *command;
    int toggle;
} CustomChip;

// subscription provider (chip fetcher; mirrors config.subs.providers)
typedef struct {
    int type;            // 0 = chatgpt, 1 = zai, 2 = antigravity
    int family;          // antigravity only: 0 = Gemini, 1 = GPT/Claude
    int enabled;
    wchar_t *label;
    wchar_t *authPath;     // chatgpt auth.json, antigravity pi auth.json
    wchar_t *clientId;     // google desktop oauth pair for the cloud fallback:
    wchar_t *clientSecret; // user config only, never compiled in or committed
    wchar_t *configPath;   // zai config.json
    wchar_t *vscdbPath;    // antigravity IDE fallback token store
    wchar_t *providerName; // zai provider key
} SubsProvider;

typedef struct {
    int height, gap, fontSize, backgroundAlpha, align;
    wchar_t *tint, *backdrop, *fontFamily;

    wchar_t *fg, *fgDim, *pink, *pinkDeep, *divider, *warn, *pinkBg, *yellow, *good;

    int mGpu, mCpu, mCpuTemp, mRam, mVolume, mBattery, mClock;
    int cpuWarnAt, ramWarnAt, tempWarnAt;

    int shortcutEnabled;
    wchar_t *shortcutLabel, *shortcutCommand;

    int petEnabled;
    wchar_t *petLabel, *petExePath;

    CustomChip custom[MAX_CUSTOM];
    int customCount;

    wchar_t *terminalClassName;
    wchar_t *terminalTitle;   // substring match on the terminal's window title
    wchar_t *clockFormat;
    int showTray;
    int autoStart;            // fresh installs register the Run value (default on)
    int debug;                 // general.debug: extra scan logging

    int subsEnabled, subsIntervalMin, subsTimeoutMs, subsRotateSec;
    SubsProvider subsProviders[MAX_SUBS];
    int subsProviderCount;

    // make-everything-tunable surface
    wchar_t *iconColor;         // bar icon stroke color (default = pinkDeep)
    int iconOpacity;            // 0..100, icon alpha over the tint
    int barRadius;              // bar corner radius in CSS px (0 = square)
    wchar_t heatmap[5][12];     // dashboard ramp, hex colors light -> dark
    int dashW, dashH;           // token dashboard popup, CSS px
    int subsW, subsH;           // subscriptions board popup, CSS px
    wchar_t *tokenCachePath;    // override for ~/.wizbar/token-cache.json
    wchar_t tokensApps[16][20]; // harness allowlist for token stats (empty = all)
    int tokensAppCount;
    // tokens.labels: display-name overrides for the dashboard rows, keyed by
    // the raw source key (e.g. "pi" -> "pi-wsl" when the store is on WSL)
    wchar_t tokLabelKeys[16][20];
    wchar_t tokLabelVals[16][40];
    int tokLabelCount;
    int tokensEnabled;            // master switch: off = zero scans
    int tokensRescanSec;          // tokens.rescanMinutes -> seconds between scans
    TokSource tokSrc[4];          // JSONL session stores for the live scan
    int tokSrcCount;
    UserIcon icons[MAX_USER_ICONS]; // theme.icons[]: new named icons
    int iconCount;
} Config;

double g_scale = 1.0;        // display scale (dpi/96): config values are in DIPs (shared)
double g_iconOpacity = 1.0;  // theme.iconOpacity/100 (used by the icon renderer)

static Config g_cfg;
static FILETIME g_cfgMtime;
static int g_cfgLoaded = 0;

static wchar_t *jdup(const char *js, const jsmntok_t *t) {
    wchar_t *w = utf8ToWide(js + t->start, t->end - t->start);
    if (!w) return NULL;
    // decode JSON escapes in place (jsmn keeps \n \uXXXX ... literal, and
    // JSON.parse in the Electron app decodes them - parity requires it too)
    int r = 0, wr = 0;
    for (; w[r]; r++) {
        if (w[r] != L'\\') { w[wr++] = w[r]; continue; }
        r++;
        switch (w[r]) {
            case L'"':  w[wr++] = L'"';  break;
            case L'\\': w[wr++] = L'\\'; break;
            case L'/':  w[wr++] = L'/';  break;
            case L'b':  w[wr++] = L'\b'; break;
            case L'f':  w[wr++] = L'\f'; break;
            case L'n':  w[wr++] = L'\n'; break;
            case L'r':  w[wr++] = L'\r'; break;
            case L't':  w[wr++] = L'\t'; break;
            case L'u': {
                unsigned cp = 0;
                for (int k = 1; k <= 4; k++) {
                    wchar_t c = w[r + k];
                    unsigned d;
                    if (c >= L'0' && c <= L'9') d = (unsigned)(c - L'0');
                    else if (c >= L'a' && c <= L'f') d = (unsigned)(c - L'a' + 10);
                    else if (c >= L'A' && c <= L'F') d = (unsigned)(c - L'A' + 10);
                    else { d = 16; }
                    cp = cp * 16 + (d & 15);
                }
                r += 4;
                if (cp) w[wr++] = (wchar_t)cp; // invalid 0000: drop
                break;
            }
            case 0: w[wr++] = L'\\'; break;
            default: w[wr++] = w[r]; break;
        }
    }
    w[wr] = 0;
    return w;
}

// find key `key` (len `kl`) among the children of object token `obj`
// total token count of the subtree rooted at i (jsmn stores trees contiguously)
static int jtokSpan(const jsmntok_t *t, int i) {
    if (t[i].type == JSMN_PRIMITIVE || t[i].type == JSMN_STRING) return 1;
    int n = 1;
    for (int c = 0; c < t[i].size; c++) {
        // object children come in key+value PAIRS; array children are single tokens
        if (t[i].type == JSMN_OBJECT) n += 1 + jtokSpan(t, i + n + 1);
        else n += jtokSpan(t, i + n);
    }
    return n;
}

static int jobjGet(const char *js, const jsmntok_t *t, int obj, const char *key) {
    if (t[obj].type != JSMN_OBJECT) return -1;
    int kids = t[obj].size;
    int kl = (int)strlen(key);
    int k = obj + 1;
    for (int j = 0; j < kids; j++) {
        const jsmntok_t *kt = &t[k];
        if (kt->type == JSMN_STRING && kt->end - kt->start == kl &&
            memcmp(js + kt->start, key, kl) == 0) return k + 1;
        // advance past key + value subtree (object values hold key/value pairs)
        k += 1 + jtokSpan(t, k + 1);
    }
    return -1;
}

static int jintTok(const char *js, const jsmntok_t *t, int i, int def) {
    if (i < 0 || t[i].type != JSMN_PRIMITIVE) return def;
    char b[32]; int len = t[i].end - t[i].start;
    if (len <= 0 || len >= (int)sizeof(b)) return def;
    memcpy(b, js + t[i].start, len); b[len] = 0;
    return atoi(b);
}

// a JSON number read as a double (theme.icons[].w); atof is locale-stable
// enough for the plain decimals a config can carry
static double jdoubleTok(const char *js, const jsmntok_t *t, int i, double def) {
    if (i < 0 || t[i].type != JSMN_PRIMITIVE) return def;
    char b[32]; int len = t[i].end - t[i].start;
    if (len <= 0 || len >= (int)sizeof(b)) return def;
    memcpy(b, js + t[i].start, len); b[len] = 0;
    return atof(b);
}

static wchar_t *jstrTok(const char *js, const jsmntok_t *t, int i, const wchar_t *def) {
    if (i >= 0 && t[i].type == JSMN_STRING) {
        wchar_t *w = jdup(js, &t[i]);
        if (w) return w;
    }
    return def ? wideDup(def) : NULL;
}

static int jboolDefault(const char *js, const jsmntok_t *t, int i, int def) {
    if (i < 0 || t[i].type != JSMN_PRIMITIVE) return def;
    int len = t[i].end - t[i].start;
    if (len == 4 && memcmp(js + t[i].start, "true", 4) == 0) return 1;
    if (len == 5 && memcmp(js + t[i].start, "false", 5) == 0) return 0;
    return def;
}

static void freeConfig(Config *c) {
    wideFree(&c->tint); wideFree(&c->backdrop); wideFree(&c->fontFamily);
    wideFree(&c->fg); wideFree(&c->fgDim); wideFree(&c->pink); wideFree(&c->pinkDeep); wideFree(&c->divider);
    wideFree(&c->iconColor); wideFree(&c->tokenCachePath);
    wideFree(&c->yellow); wideFree(&c->good);
    wideFree(&c->warn); wideFree(&c->pinkBg);
    wideFree(&c->shortcutLabel); wideFree(&c->shortcutCommand);
    wideFree(&c->petLabel); wideFree(&c->petExePath);
    wideFree(&c->terminalClassName);
    wideFree(&c->clockFormat);
    for (int i = 0; i < c->customCount; i++) {
        wideFree(&c->custom[i].icon); wideFree(&c->custom[i].label);
        wideFree(&c->custom[i].color); wideFree(&c->custom[i].title);
        wideFree(&c->custom[i].command);
    }
    c->customCount = 0;
    for (int i = 0; i < c->subsProviderCount; i++) {
        wideFree(&c->subsProviders[i].clientId);
        wideFree(&c->subsProviders[i].clientSecret);
    }
    for (int i = 0; i < c->tokSrcCount; i++) wideFree(&c->tokSrc[i].sessionsDir);
    c->tokSrcCount = 0;
    for (int i = 0; i < c->iconCount; i++) {
        wideFree(&c->icons[i].name); wideFree(&c->icons[i].d);
    }
    c->iconCount = 0;
}

static void parseConfigInto(Config *c, const char *js, jsmntok_t *t, int root) {
    memset(c, 0, sizeof(*c));
    c->height = 24; c->gap = 8; c->fontSize = 12; c->backgroundAlpha = 110;
    c->tint = wideDup(L"#FBF2E2"); c->backdrop = wideDup(L"acrylic");
    c->fontFamily = wideDup(L"Cascadia Mono");
    c->fg = wideDup(L"#080808"); c->fgDim = wideDup(L"#5a5245"); c->pink = wideDup(L"#E8C7D0"); c->pinkDeep = wideDup(L"#D493AA");
    c->iconColor = wideDup(L""); // empty = follow pinkDeep
    c->iconOpacity = 90; // Electron: .seg svg { opacity: 0.9 }
    c->barRadius = 8;
    lstrcpynW(c->heatmap[0], L"#F1ECD8", 12); lstrcpynW(c->heatmap[1], L"#F6D8E0", 12);
    lstrcpynW(c->heatmap[2], L"#EFB7C7", 12); lstrcpynW(c->heatmap[3], L"#E28FB0", 12);
    lstrcpynW(c->heatmap[4], L"#C95E8F", 12);
    // sized to the captain's 1440x900 CSS desktop: wide enough that the two
    // tables are not squeezed into half-width columns, tall enough that the
    // content-fit never has to grow it past the screen
    c->dashW = 900; c->dashH = 520; c->subsW = 880; c->subsH = 580;
    c->tokenCachePath = wideDup(L"");
    c->tokensAppCount = 0;
    c->tokLabelCount = 0;
    c->tokensEnabled = 1;
    c->tokensRescanSec = 60;
    c->tokSrcCount = 0;
    c->iconCount = 0;
    c->divider = wideDup(L"#D9CCB2"); c->warn = wideDup(L"#A00000");
    c->pinkBg = wideDup(L"#FEF7F9");
    c->yellow = wideDup(L"#B8A96A"); c->good = wideDup(L"#006400");
    c->mGpu = c->mCpu = c->mCpuTemp = c->mRam = c->mVolume = c->mBattery = c->mClock = 1;
    c->cpuWarnAt = 85; c->ramWarnAt = 90; c->tempWarnAt = 85;
    c->showTray = 1;
    c->autoStart = 1;
    c->debug = 0;
    c->clockFormat = wideDup(L"{MMM} {dd} ({Wkk}) {HH}:{mm}");
    c->subsEnabled = 0; c->subsIntervalMin = 2; c->subsTimeoutMs = 20000; c->subsProviderCount = 0;
    c->subsRotateSec = 60;

    if (root < 0 || t[root].type != JSMN_OBJECT) return;
    int bar = jobjGet(js, t, root, "bar");
    if (bar >= 0) {
        c->height     = jintTok(js, t, jobjGet(js, t, bar, "height"), c->height);
        c->gap        = jintTok(js, t, jobjGet(js, t, bar, "gap"), c->gap);
        c->fontSize   = jintTok(js, t, jobjGet(js, t, bar, "fontSize"), c->fontSize);
        c->backgroundAlpha = jintTok(js, t, jobjGet(js, t, bar, "backgroundAlpha"), c->backgroundAlpha);
        wideFree(&c->tint);
        c->tint  = jstrTok(js, t, jobjGet(js, t, bar, "backgroundTint"), c->tint);
        wideFree(&c->backdrop);
        c->backdrop = jstrTok(js, t, jobjGet(js, t, bar, "backdrop"), c->backdrop);
        wideFree(&c->fontFamily);
        c->fontFamily = jstrTok(js, t, jobjGet(js, t, bar, "fontFamily"), c->fontFamily);
        c->align      = jintTok(js, t, jobjGet(js, t, bar, "align"), 0) == 2 ? 2 : 1; // 1=right 2=left
        c->barRadius  = jintTok(js, t, jobjGet(js, t, bar, "radius"), c->barRadius);
        if (c->barRadius < 0) c->barRadius = 0; if (c->barRadius > 26) c->barRadius = 26;
    }
    int theme = jobjGet(js, t, root, "theme");
    if (theme >= 0) {
        wideFree(&c->fg);       c->fg       = jstrTok(js, t, jobjGet(js, t, theme, "fg"), c->fg);
        wideFree(&c->fgDim);    c->fgDim    = jstrTok(js, t, jobjGet(js, t, theme, "fgDim"), c->fgDim);
        wideFree(&c->pink); c->pink = jstrTok(js, t, jobjGet(js, t, theme, "pink"), c->pink);
        wideFree(&c->iconColor); c->iconColor = jstrTok(js, t, jobjGet(js, t, theme, "iconColor"), c->iconColor);
        c->iconOpacity = jintTok(js, t, jobjGet(js, t, theme, "iconOpacity"), c->iconOpacity);
        if (c->iconOpacity < 0) c->iconOpacity = 0; if (c->iconOpacity > 100) c->iconOpacity = 100;
        {
            int hm = jobjGet(js, t, theme, "heatmap");
            if (hm >= 0 && t[hm].type == JSMN_ARRAY) {
                int cnt = t[hm].size; if (cnt > 5) cnt = 5;
                int k = hm + 1;
                for (int i = 0; i < cnt; i++) {
                    if (t[k].type == JSMN_STRING) {
                        wchar_t *s = jdup(js, &t[k]);
                        if (s) { lstrcpynW(c->heatmap[i], s, 12); wideFree(&s); }
                    }
                    k += jtokSpan(t, k);
                }
            }
        }
        // theme.icons[]: new named icons, referenced by modules.custom[].icon.
        // Each entry is {name, d, w}: an SVG path in the same 24-unit viewBox
        // the built-in icons use, stroked with width w. Defining a name twice
        // replaces it (hot-reload friendly).
        c->iconCount = 0;
        int ic = jobjGet(js, t, theme, "icons");
        if (ic >= 0 && t[ic].type == JSMN_ARRAY) {
            int cnt = t[ic].size; if (cnt > MAX_USER_ICONS) cnt = MAX_USER_ICONS;
            int k = ic + 1;
            for (int i = 0; i < cnt; i++) {
                if (t[k].type == JSMN_OBJECT && c->iconCount < MAX_USER_ICONS) {
                    UserIcon *ui = &c->icons[c->iconCount];
                    memset(ui, 0, sizeof(*ui));
                    ui->name = jstrTok(js, t, jobjGet(js, t, k, "name"), NULL);
                    ui->d = jstrTok(js, t, jobjGet(js, t, k, "d"), NULL);
                    ui->w = jdoubleTok(js, t, jobjGet(js, t, k, "w"), 2.2);
                    if (ui->name && *ui->name && ui->d && *ui->d) c->iconCount++;
                    else { wideFree(&ui->name); wideFree(&ui->d); }
                }
                k += jtokSpan(t, k);
            }
        }
        wideFree(&c->pinkDeep); c->pinkDeep = jstrTok(js, t, jobjGet(js, t, theme, "pinkDeep"), c->pinkDeep);
        wideFree(&c->divider);  c->divider  = jstrTok(js, t, jobjGet(js, t, theme, "divider"), c->divider);
        wideFree(&c->warn);     c->warn     = jstrTok(js, t, jobjGet(js, t, theme, "warn"), c->warn);
        wideFree(&c->pinkBg);   c->pinkBg   = jstrTok(js, t, jobjGet(js, t, theme, "pinkBg"), c->pinkBg);
        wideFree(&c->yellow);   c->yellow   = jstrTok(js, t, jobjGet(js, t, theme, "yellow"), c->yellow);
        wideFree(&c->good);     c->good     = jstrTok(js, t, jobjGet(js, t, theme, "good"), c->good);
    }
    int modules = jobjGet(js, t, root, "modules");
    if (modules >= 0) {
        int m;
        m = jobjGet(js, t, modules, "gpu");      c->mGpu     = m < 0 ? 1 : jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 1);
        m = jobjGet(js, t, modules, "cpu");      c->mCpu     = m < 0 ? 1 : jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 1);
        if (m >= 0) c->cpuWarnAt = jintTok(js, t, jobjGet(js, t, m, "warnAt"), c->cpuWarnAt);
        m = jobjGet(js, t, modules, "cputemp");  c->mCpuTemp = m < 0 ? 1 : jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 1);
        if (m >= 0) c->tempWarnAt = jintTok(js, t, jobjGet(js, t, m, "warnAt"), c->tempWarnAt);
        m = jobjGet(js, t, modules, "ram");      c->mRam     = m < 0 ? 1 : jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 1);
        if (m >= 0) c->ramWarnAt = jintTok(js, t, jobjGet(js, t, m, "warnAt"), c->ramWarnAt);
        m = jobjGet(js, t, modules, "volume");   c->mVolume  = m < 0 ? 1 : jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 1);
        m = jobjGet(js, t, modules, "battery");  c->mBattery = m < 0 ? 1 : jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 1);
        m = jobjGet(js, t, modules, "clock");
        if (m >= 0) {
            c->mClock = jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 1);
            int f = jobjGet(js, t, m, "format");
            if (f >= 0) { wideFree(&c->clockFormat); c->clockFormat = jstrTok(js, t, f, c->clockFormat); }
        }
        m = jobjGet(js, t, modules, "shortcut");
        if (m >= 0) {
            c->shortcutEnabled = jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 0);
            c->shortcutLabel   = jstrTok(js, t, jobjGet(js, t, m, "label"), L"");
            c->shortcutCommand = jstrTok(js, t, jobjGet(js, t, m, "command"), L"");
        }
        m = jobjGet(js, t, modules, "pet");
        if (m >= 0) {
            c->petEnabled = jboolDefault(js, t, jobjGet(js, t, m, "enabled"), 0);
            c->petLabel   = jstrTok(js, t, jobjGet(js, t, m, "label"), L"");
            c->petExePath = jstrTok(js, t, jobjGet(js, t, m, "exePath"), L"");
        }
        int arr = jobjGet(js, t, modules, "custom");
        if (arr >= 0 && t[arr].type == JSMN_ARRAY) {
            int n = t[arr].size;
            if (n > MAX_CUSTOM) n = MAX_CUSTOM;
            int k = arr + 1;
            for (int j = 0; j < n; j++) {
                jsmntok_t *e = &t[k];
                int en = -1;
                if (e->type == JSMN_OBJECT) {
                    CustomChip *cc = &c->custom[c->customCount];
                    memset(cc, 0, sizeof(*cc));
                    cc->enabled = 1; cc->toggle = 0;
                    en = jobjGet(js, t, k, "enabled");   cc->enabled = jboolDefault(js, t, en, 1);
                    en = jobjGet(js, t, k, "toggle");    cc->toggle  = jboolDefault(js, t, en, 0);
                    cc->icon    = jstrTok(js, t, jobjGet(js, t, k, "icon"), L"");
                    cc->label   = jstrTok(js, t, jobjGet(js, t, k, "label"), L"");
                    cc->color   = jstrTok(js, t, jobjGet(js, t, k, "color"), L"");
                    cc->title   = jstrTok(js, t, jobjGet(js, t, k, "title"), NULL);
                    cc->command = jstrTok(js, t, jobjGet(js, t, k, "command"), L"");
                    c->customCount++;
                }
                // advance k past this element
                k += jtokSpan(t, k);
            }
        }
    }
    int subs = jobjGet(js, t, root, "subs");
    if (subs >= 0 && t[subs].type == JSMN_OBJECT) {
        c->subsEnabled = jboolDefault(js, t, jobjGet(js, t, subs, "enabled"), 0);
        c->subsIntervalMin = jintTok(js, t, jobjGet(js, t, subs, "intervalMinutes"), c->subsIntervalMin);
        c->subsTimeoutMs = jintTok(js, t, jobjGet(js, t, subs, "fetchTimeoutMs"), c->subsTimeoutMs);
        // how long each plan stays on the gauge chip before rotating
        c->subsRotateSec = jintTok(js, t, jobjGet(js, t, subs, "rotateSec"), c->subsRotateSec);
        if (c->subsRotateSec < 5) c->subsRotateSec = 5;
        if (c->subsRotateSec > 3600) c->subsRotateSec = 3600;
        c->subsW = jintTok(js, t, jobjGet(js, t, subs, "width"), c->subsW);
        c->subsH = jintTok(js, t, jobjGet(js, t, subs, "height"), c->subsH);
        if (c->subsW < 280) c->subsW = 280; if (c->subsH < 180) c->subsH = 180;
    }
    // token stats surface: which cache file + which harness apps to count
    int toks = jobjGet(js, t, root, "tokens");
    if (toks >= 0 && t[toks].type == JSMN_OBJECT) {
        c->tokensEnabled = jboolDefault(js, t, jobjGet(js, t, toks, "enabled"), 1);
        // minutes -> seconds, clamped 5..3600: the metrics tick is 1s, so this
        // is the tick count between token scans
        int rm = jintTok(js, t, jobjGet(js, t, toks, "rescanMinutes"), 1);
        if (rm < 1) rm = 1;
        if (rm > 60) rm = 60;
        c->tokensRescanSec = rm * 60;
        wideFree(&c->tokenCachePath);
        c->tokenCachePath = jstrTok(js, t, jobjGet(js, t, toks, "cachePath"), c->tokenCachePath);
        int af = jobjGet(js, t, toks, "appFilter");
        if (af >= 0 && t[af].type == JSMN_ARRAY) {
            int cnt = t[af].size; if (cnt > 16) cnt = 16;
            int k = af + 1;
            c->tokensAppCount = 0;
            for (int i = 0; i < cnt; i++) {
                if (t[k].type == JSMN_STRING) {
                    wchar_t *s = jdup(js, &t[k]);
                    if (s) {
                        lstrcpynW(c->tokensApps[c->tokensAppCount], s, 20);
                        c->tokensAppCount++;
                        wideFree(&s);
                    }
                }
                k += jtokSpan(t, k);
            }
        }
        // tokens.labels: { "pi": "pi-wsl" } - dashboard display names. The
        // aggregation keys stay raw (cursors, cache records, the app filter all
        // match on them); only the rendered row text is swapped.
        int lb = jobjGet(js, t, toks, "labels");
        c->tokLabelCount = 0; // re-parsed on every reload
        if (lb >= 0 && t[lb].type == JSMN_OBJECT) {
            int cnt = t[lb].size; if (cnt > 16) cnt = 16;
            int k = lb + 1;
            for (int i = 0; i < cnt; i++) {
                if (t[k].type == JSMN_STRING && t[k + 1].type == JSMN_STRING) {
                    wchar_t *key = jdup(js, &t[k]);
                    wchar_t *val = jdup(js, &t[k + 1]);
                    if (key && val) {
                        lstrcpynW(c->tokLabelKeys[c->tokLabelCount], key, 20);
                        lstrcpynW(c->tokLabelVals[c->tokLabelCount], val, 40);
                        c->tokLabelCount++;
                    }
                    wideFree(&key); wideFree(&val);
                }
                k += 1 + jtokSpan(t, k + 1); // key + whole value subtree
            }
        }
        // tokens.sources[]: the session stores the live scan reads. Only the
        // JSONL ones are scanned in-process (pi nests per project, zai is flat);
        // the SQLite stores (zcode/opencode) keep coming from the cache seed.
        int srcs = jobjGet(js, t, toks, "sources");
        if (srcs >= 0 && t[srcs].type == JSMN_OBJECT) {
            static const struct { const char *key; const char *app; } known[] = {
                { "zai", "zai" }, { "pi", "pi" },
            };
            for (unsigned s = 0; s < sizeof(known) / sizeof(known[0]) && c->tokSrcCount < 4; s++) {
                int se = jobjGet(js, t, srcs, known[s].key);
                if (se < 0 || t[se].type != JSMN_OBJECT) continue;
                TokSource *ts = &c->tokSrc[c->tokSrcCount];
                memset(ts, 0, sizeof(*ts));
                ts->app = known[s].app;
                ts->enabled = jboolDefault(js, t, jobjGet(js, t, se, "enabled"), 1);
                ts->sessionsDir = jstrTok(js, t, jobjGet(js, t, se, "sessionsDir"), NULL);
                if (ts->sessionsDir && *ts->sessionsDir) c->tokSrcCount++;
                else wideFree(&ts->sessionsDir);
            }
        }
    }
    // dashboard popup size
    int dash = jobjGet(js, t, root, "dashboard");
    if (dash >= 0 && t[dash].type == JSMN_OBJECT) {
        c->dashW = jintTok(js, t, jobjGet(js, t, dash, "width"), c->dashW);
        c->dashH = jintTok(js, t, jobjGet(js, t, dash, "height"), c->dashH);
        if (c->dashW < 360) c->dashW = 360; if (c->dashH < 240) c->dashH = 240;
    }
        int arr = jobjGet(js, t, subs, "providers");
        if (arr >= 0 && t[arr].type == JSMN_ARRAY) {
            int n2 = t[arr].size;
            if (n2 > MAX_SUBS) n2 = MAX_SUBS;
            int k = arr + 1;
            for (int j = 0; j < n2; j++) {
                jsmntok_t *e = &t[k];
                if (e->type == JSMN_OBJECT && c->subsProviderCount < MAX_SUBS) {
                    SubsProvider *sp = &c->subsProviders[c->subsProviderCount];
                    memset(sp, 0, sizeof(*sp));
                    int en = jobjGet(js, t, k, "enabled"); sp->enabled = jboolDefault(js, t, en, 1);
                    wchar_t *ty = subs == -1 ? NULL : jstrTok(js, t, jobjGet(js, t, k, "type"), L"chatgpt");
                    sp->type = (ty && lstrcmpiW(ty, L"zai") == 0) ? 1 : (ty && lstrcmpiW(ty, L"antigravity") == 0) ? 2 : 0;
                    wideFree(&ty);
                    sp->label        = jstrTok(js, t, jobjGet(js, t, k, "label"), L"");
                    sp->authPath     = jstrTok(js, t, jobjGet(js, t, k, "authPath"), L"");
                    sp->clientId     = jstrTok(js, t, jobjGet(js, t, k, "clientId"), L"");
                    sp->clientSecret = jstrTok(js, t, jobjGet(js, t, k, "clientSecret"), L"");
                    sp->configPath   = jstrTok(js, t, jobjGet(js, t, k, "configPath"), L"");
                    sp->vscdbPath    = jstrTok(js, t, jobjGet(js, t, k, "vscdbPath"), L"");
                    sp->providerName = jstrTok(js, t, jobjGet(js, t, k, "provider"), L"");
                    c->subsProviderCount++;
                }
                // advance k past this element
                k += jtokSpan(t, k);
            }
        }
    int term = jobjGet(js, t, root, "terminal");
    if (term >= 0) {
        c->terminalClassName = jstrTok(js, t, jobjGet(js, t, term, "className"), L"");
        c->terminalTitle = jstrTok(js, t, jobjGet(js, t, term, "title"), L"");
    }
    int general = jobjGet(js, t, root, "general");
    if (general >= 0) {
        c->showTray = jboolDefault(js, t, jobjGet(js, t, general, "showTray"), 1);
        c->autoStart = jboolDefault(js, t, jobjGet(js, t, general, "autoStart"), 1);
        c->debug = jboolDefault(js, t, jobjGet(js, t, general, "debug"), 0);
    }
}
