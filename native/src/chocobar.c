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
// a live token source: the JSONL session store the in-process scan reads.
// Declared by the user in tokens.sources[], so any harness that logs per-message
// usage as JSONL is supported without a code change. `app` is the aggregation
// key (and the tokens.labels lookup key); the field names default to the pi/zai
// shape and can be overridden per source for a harness that spells them
// differently.
typedef struct {
    int enabled;
    char app[24];           // aggregate key ("pi", "zai", anything)
    wchar_t *sessionsDir;   // ~ prefixed = profile relative, else absolute/UNC
    int recursive;          // descend into per-project subdirectories
    char kIn[24], kOut[24], kCr[24], kCw[24], kTs[24], kModel[24];
} TokSource;

#define MAX_TOK_SRC 8
#define MAX_CUSTOM 16
#define MAX_SUBS 6
#define MAX_USER_ICONS 16
#define MAX_GEN_AUTH 3
#define MAX_GEN_WIN 4
#define MAX_GEN_PATH 96

// one auth credential for a generic provider: where the secret comes from and
// which header carries it. Resolved at fetch time, never persisted.
typedef struct {
    wchar_t header[40];     // header name, e.g. "authorization"
    wchar_t prefix[48];     // prepended verbatim, e.g. "Bearer "
    wchar_t *path;          // JSON file to read the token from
    char key[MAX_GEN_PATH]; // path inside that file, e.g. "access_token"
    wchar_t *env;           // environment variable holding the token
    wchar_t *literal;       // token written straight into the config
} GenAuth;

// one quota window of a generic provider: label plus the paths that carry the
// numbers. A window with neither used nor remaining is skipped.
typedef struct {
    wchar_t label[24];
    char used[MAX_GEN_PATH];
    char remaining[MAX_GEN_PATH];
    char total[MAX_GEN_PATH];
    char reset[MAX_GEN_PATH];
} GenWin;

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

// jsmn helpers live with the config parser below; declared here so the source
// and provider parsers above can use them
static int jobjGet(const char *js, const jsmntok_t *t, int obj, const char *key);
static int jtokSpan(const jsmntok_t *t, int i);

// derive an aggregate key from a session-store path when the config does not
// name one: ~/.pi/agent/sessions -> "pi". Walks up past the store folder and
// its parent, then takes that directory with any leading dot stripped.
static void tokAppFromDir(const wchar_t *dir, char *out, int cch) {
    out[0] = 0;
    if (!dir || !*dir || cch < 2) return;
    wchar_t buf[MAX_PATH];
    lstrcpynW(buf, dir, MAX_PATH);
    for (int up = 0; up < 2; up++) { // drop "sessions", then its parent
        wchar_t *s = wcsrchr(buf, L'\\');
        wchar_t *f = wcsrchr(buf, L'/');
        wchar_t *last = (s > f) ? s : f;
        if (!last) { buf[0] = 0; break; }
        *last = 0;
    }
    wchar_t *s = wcsrchr(buf, L'\\');
    wchar_t *f = wcsrchr(buf, L'/');
    wchar_t *last = (s > f) ? s : f;
    const wchar_t *name = last ? last + 1 : buf;
    if (*name == L'.') name++;
    int i = 0;
    for (; name[i] && i < cch - 1; i++) out[i] = (char)name[i];
    out[i] = 0;
    if (!out[0]) lstrcpyA(out, "app");
}

// copy one usage-field name out of the config, falling back to the default
static void tokFieldCopy(char *dst, int cch, const char *js, jsmntok_t *t, int obj,
                         const char *key, const char *dflt) {
    lstrcpynA(dst, dflt, cch);
    int k = jobjGet(js, t, obj, key);
    if (k < 0 || t[k].type != JSMN_STRING) return;
    int n = t[k].end - t[k].start;
    if (n <= 0 || n >= cch) return;
    memcpy(dst, js + t[k].start, (size_t)n);
    dst[n] = 0;
}

// --------------------------------------------------------------- config ----
typedef struct {
    int enabled;
    wchar_t *icon, *label, *color, *title, *command;
    int toggle;
    // command-output chip: when intervalMs > 0 the command is polled and its
    // stdout becomes the chip text (the escape hatch for any metric the bar
    // does not know about). format is wrapped around the trimmed output.
    int intervalMs;
    wchar_t *format;
    double warnAbove, warnBelow;   // -1 = disabled
} CustomChip;

// subscription provider (chip fetcher; mirrors config.subs.providers)
typedef struct {
    int type;            // 0 = chatgpt, 1 = zai, 2 = antigravity, 3 = generic
    int family;          // antigravity only: 0 = Gemini, 1 = GPT/Claude
    int enabled;
    wchar_t *label;
    wchar_t *authPath;     // chatgpt auth.json, antigravity pi auth.json
    wchar_t *clientId;     // google desktop oauth pair for the cloud fallback:
    wchar_t *clientSecret; // user config only, never compiled in or committed
    wchar_t *configPath;   // zai config.json
    wchar_t *vscdbPath;    // antigravity IDE fallback token store
    wchar_t *providerName; // zai provider key
    // ---- generic (type 3): a REST quota endpoint declared entirely in config
    wchar_t *url;          // https://host/path (http allowed with insecure)
    wchar_t *method;       // GET (default) or POST
    wchar_t *reqBody;      // POST body
    wchar_t *headerBlob;   // static "Name: value\r\n" lines, pre-joined
    int insecure;          // 1 = allow plain http (sends the token in clear)
    int expectStatus;      // 0 = any
    char requirePath[MAX_GEN_PATH]; // response must contain this path
    char planPath[MAX_GEN_PATH];    // plan display name
    int nAuth; GenAuth auth[MAX_GEN_AUTH];
    int nWin;  GenWin  win[MAX_GEN_WIN];
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

// The live config is swapped wholesale by loadConfig while the provider fetch
// threads (each holds a SubsProvider* across a multi-second HTTP call) and the
// command-chip poll read it, so it lives behind an indirection: a swap installs
// a new generation and retires the previous one, and the last reader frees it.
static Config g_cfgGen0;              // generation 0: the static buffer, never heap-freed
static Config *g_cfgCur = &g_cfgGen0;
#define g_cfg (*g_cfgCur)
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

// copy a JSON string into a bounded narrow buffer (generic paths/key names)
static void jstrCopyA(char *dst, int cch, const char *js, const jsmntok_t *t, int i,
                      const char *dflt) {
    if (dflt) lstrcpynA(dst, dflt, cch); else dst[0] = 0;
    if (i < 0 || t[i].type != JSMN_STRING) return;
    int n = t[i].end - t[i].start;
    if (n <= 0 || n >= cch) return;
    memcpy(dst, js + t[i].start, (size_t)n);
    dst[n] = 0;
}

// one auth entry of a generic provider: "header: prefix <value>" where the
// value comes from a JSON file, an environment variable, or the config itself.
static void genParseAuth(const char *js, const jsmntok_t *t, int obj, GenAuth *ga) {
    memset(ga, 0, sizeof(*ga));
    wchar_t *h = jstrTok(js, t, jobjGet(js, t, obj, "header"), L"Authorization");
    lstrcpynW(ga->header, h, 40); wideFree(&h);
    wchar_t *p = jstrTok(js, t, jobjGet(js, t, obj, "prefix"), L"Bearer ");
    lstrcpynW(ga->prefix, p, 48); wideFree(&p);
    ga->path   = jstrTok(js, t, jobjGet(js, t, obj, "path"), NULL);
    jstrCopyA(ga->key, sizeof(ga->key), js, t, jobjGet(js, t, obj, "key"), "access_token");
    ga->env    = jstrTok(js, t, jobjGet(js, t, obj, "env"), NULL);
    ga->literal = jstrTok(js, t, jobjGet(js, t, obj, "literal"), NULL);
}

// one quota window: a label plus the response paths carrying the numbers
static void genParseWin(const char *js, const jsmntok_t *t, int obj, GenWin *gw) {
    memset(gw, 0, sizeof(*gw));
    wchar_t *l = jstrTok(js, t, jobjGet(js, t, obj, "label"), L"");
    lstrcpynW(gw->label, l, 24); wideFree(&l);
    jstrCopyA(gw->used,      sizeof(gw->used),      js, t, jobjGet(js, t, obj, "used"),      "");
    jstrCopyA(gw->remaining, sizeof(gw->remaining), js, t, jobjGet(js, t, obj, "remaining"), "");
    jstrCopyA(gw->total,     sizeof(gw->total),     js, t, jobjGet(js, t, obj, "total"),     "");
    jstrCopyA(gw->reset,     sizeof(gw->reset),     js, t, jobjGet(js, t, obj, "reset"),     "");
}

// join static header lines into one \r\n-separated blob for WinHTTP
static wchar_t *genHeadersBlob(const char *js, const jsmntok_t *t, int obj) {
    int h = jobjGet(js, t, obj, "headers");
    if (h < 0 || t[h].type != JSMN_OBJECT) return NULL;
    int n = t[h].size, k = h + 1, cch = 256;
    wchar_t *blob = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)cch * sizeof(wchar_t));
    if (!blob) return NULL;
    int len = 0;
    for (int i = 0; i < n; i++) {
        wchar_t *nm = jstrTok(js, t, k, NULL);
        wchar_t *vl = jstrTok(js, t, k + 1, NULL);
        k += 1 + jtokSpan(t, k + 1);
        if (nm && vl) {
            int need = len + lstrlenW(nm) + lstrlenW(vl) + 8;
            if (need >= cch) {
                while (cch <= need) cch *= 2;
                wchar_t *nb = (wchar_t *)HeapReAlloc(GetProcessHeap(), 0, blob, (size_t)cch * sizeof(wchar_t));
                if (!nb) { wideFree(&nm); wideFree(&vl); wideFree(&blob); return NULL; }
                blob = nb;
            }
            len += swprintf(blob + len, cch - len, L"%ls: %ls\r\n", nm, vl);
        }
        wideFree(&nm); wideFree(&vl);
    }
    if (!len) { wideFree(&blob); return NULL; }
    return blob;
}

// a type-3 provider: url, optional auth list, static headers, quota windows.
// Everything a new service needs lives here, so wiring one up is a config edit.
static void genParse(SubsProvider *sp, const char *js, const jsmntok_t *t, int obj) {
    sp->url       = jstrTok(js, t, jobjGet(js, t, obj, "url"), NULL);
    sp->method    = jstrTok(js, t, jobjGet(js, t, obj, "method"), L"GET");
    sp->reqBody   = jstrTok(js, t, jobjGet(js, t, obj, "body"), NULL);
    sp->insecure  = jboolDefault(js, t, jobjGet(js, t, obj, "insecure"), 0);
    sp->expectStatus = (int)jintTok(js, t, jobjGet(js, t, obj, "expectStatus"), 0);
    jstrCopyA(sp->requirePath, sizeof(sp->requirePath), js, t,
              jobjGet(js, t, obj, "require"), "");
    jstrCopyA(sp->planPath, sizeof(sp->planPath), js, t,
              jobjGet(js, t, obj, "planPath"), "");
    // auth may be one object or a list of them (api key + bearer, say)
    int a = jobjGet(js, t, obj, "auth");
    if (a >= 0) {
        if (t[a].type == JSMN_OBJECT) {
            genParseAuth(js, t, a, &sp->auth[0]);
            sp->nAuth = 1;
        } else if (t[a].type == JSMN_ARRAY) {
            int n = t[a].size; if (n > MAX_GEN_AUTH) n = MAX_GEN_AUTH;
            int k = a + 1;
            for (int i = 0; i < n && sp->nAuth < MAX_GEN_AUTH; i++) {
                if (t[k].type == JSMN_OBJECT)
                    genParseAuth(js, t, k, &sp->auth[sp->nAuth++]);
                k += jtokSpan(t, k);
            }
        }
    }
    sp->headerBlob = genHeadersBlob(js, t, obj);
    // windows[]: a label plus the paths carrying the numbers
    int w = jobjGet(js, t, obj, "windows");
    if (w >= 0 && t[w].type == JSMN_ARRAY) {
        int n = t[w].size; if (n > MAX_GEN_WIN) n = MAX_GEN_WIN;
        int k = w + 1;
        for (int i = 0; i < n && sp->nWin < MAX_GEN_WIN; i++) {
            if (t[k].type == JSMN_OBJECT)
                genParseWin(js, t, k, &sp->win[sp->nWin++]);
            k += jtokSpan(t, k);
        }
    }
}

// release every generic allocation; called from the config teardown so a
// reload cannot leak the previous set of paths
static void genFree(SubsProvider *sp) {
    wideFree(&sp->url); wideFree(&sp->method); wideFree(&sp->reqBody);
    wideFree(&sp->headerBlob);
    for (int i = 0; i < sp->nAuth; i++) {
        wideFree(&sp->auth[i].path);
        wideFree(&sp->auth[i].env);
        wideFree(&sp->auth[i].literal);
    }
    sp->nAuth = 0;
    sp->nWin = 0;
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
        wideFree(&c->custom[i].command); wideFree(&c->custom[i].format);
    }
    c->customCount = 0;
    for (int i = 0; i < c->subsProviderCount; i++) {
        SubsProvider *sp = &c->subsProviders[i];
        wideFree(&sp->clientId);
        wideFree(&sp->clientSecret);
        genFree(sp);
    }
    for (int i = 0; i < c->tokSrcCount; i++) wideFree(&c->tokSrc[i].sessionsDir);
    c->tokSrcCount = 0;
    for (int i = 0; i < c->iconCount; i++) {
        wideFree(&c->icons[i].name); wideFree(&c->icons[i].d);
    }
    c->iconCount = 0;
}

// ---- config lifetime -------------------------------------------------------
// A reader pins the generation it is about to walk; loadConfig retires the
// generation it replaces and the last reader out frees it, so an in-flight
// fetch can never read a provider struct the UI thread has already freed.
static CRITICAL_SECTION g_cfgGenLock;
static long g_cfgGenRefs = 0;
// generations a reader may still walk, newest last (empty whenever nobody pins)
static Config **g_cfgRetired = NULL;
static int g_cfgRetiredN = 0, g_cfgRetiredCap = 0;

static const Config *cfgPin(void) {
    EnterCriticalSection(&g_cfgGenLock);
    const Config *v = g_cfgCur;
    g_cfgGenRefs++;
    LeaveCriticalSection(&g_cfgGenLock);
    return v;
}

static void cfgUnpin(void) {
    EnterCriticalSection(&g_cfgGenLock);
    if (--g_cfgGenRefs <= 0) {
        g_cfgGenRefs = 0;
        while (g_cfgRetiredN > 0) {
            Config *r = g_cfgRetired[--g_cfgRetiredN];
            freeConfig(r);
            if (r != &g_cfgGen0) HeapFree(GetProcessHeap(), 0, r);
        }
    }
    LeaveCriticalSection(&g_cfgGenLock);
}

// remember a replaced generation until its last reader is done
static void cfgRetire(Config *old) {
    if (g_cfgRetiredN == g_cfgRetiredCap) {
        int cap = g_cfgRetiredCap ? g_cfgRetiredCap * 2 : 8;
        Config **p = (Config **)HeapReAlloc(GetProcessHeap(), 0, g_cfgRetired,
                                            (SIZE_T)cap * sizeof(Config *));
        if (!p) return; // cannot be tracked: lose it rather than free a live one
        g_cfgRetired = p;
        g_cfgRetiredCap = cap;
    }
    g_cfgRetired[g_cfgRetiredN++] = old;
}

// install a freshly parsed generation (caller keeps no reference to it)
static void cfgInstall(Config *next) {
    EnterCriticalSection(&g_cfgGenLock);
    Config *old = g_cfgCur;
    g_cfgCur = next;
    if (g_cfgGenRefs > 0) cfgRetire(old); // readers still walk it
    else {
        freeConfig(old);
        if (old != &g_cfgGen0) HeapFree(GetProcessHeap(), 0, old);
    }
    LeaveCriticalSection(&g_cfgGenLock);
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
                    // command-output chip: intervalMs > 0 polls the command and
                    // its stdout becomes the chip text; format wraps it and
                    // warnAbove/warnBelow colour it. All three are optional.
                    cc->intervalMs = (int)jintTok(js, t, jobjGet(js, t, k, "intervalMs"), 0);
                    if (cc->intervalMs < 0) cc->intervalMs = 0;
                    if (cc->intervalMs > 0 && cc->intervalMs < 1000) cc->intervalMs = 1000;
                    cc->format = jstrTok(js, t, jobjGet(js, t, k, "format"), NULL);
                    cc->warnAbove = jdoubleTok(js, t, jobjGet(js, t, k, "warnAbove"), -1);
                    cc->warnBelow = jdoubleTok(js, t, jobjGet(js, t, k, "warnBelow"), -1);
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
        // tokens.sources[]: EVERY session store the live scan reads, declared
        // by the user. An array of { app, path, enabled, recursive, fields }
        // entries, so a new harness needs a config line and nothing else.
        int srcs = jobjGet(js, t, toks, "sources");
        if (srcs >= 0 && t[srcs].type == JSMN_ARRAY) {
            int n2 = t[srcs].size;
            if (n2 > MAX_TOK_SRC) n2 = MAX_TOK_SRC;
            int k = srcs + 1;
            for (int j = 0; j < n2 && c->tokSrcCount < MAX_TOK_SRC; j++) {
                if (t[k].type == JSMN_OBJECT) {
                    TokSource *ts = &c->tokSrc[c->tokSrcCount];
                    memset(ts, 0, sizeof(*ts));
                    ts->enabled = jboolDefault(js, t, jobjGet(js, t, k, "enabled"), 1);
                    ts->sessionsDir = jstrTok(js, t, jobjGet(js, t, k, "path"), NULL);
                    // app: explicit, else the dot-directory above the store
                    // (~/.pi/agent/sessions -> "pi"). It is the aggregation key
                    // and the tokens.labels lookup key.
                    wchar_t *app = jstrTok(js, t, jobjGet(js, t, k, "app"), NULL);
                    if (app && *app) {
                        int i2 = 0;
                        for (; app[i2] && i2 < 23; i2++) ts->app[i2] = (char)app[i2];
                        ts->app[i2] = 0;
                    } else {
                        tokAppFromDir(ts->sessionsDir, ts->app, sizeof(ts->app));
                    }
                    wideFree(&app);
                    // recursion defaults ON: a flat store simply has no
                    // subdirectories to descend into, a nested one needs it.
                    ts->recursive = jboolDefault(js, t, jobjGet(js, t, k, "recursive"), 1);
                    // field names default to the pi/zai shape; override for a
                    // harness that spells them differently (prompt_tokens etc.)
                    int fl = jobjGet(js, t, k, "fields");
                    if (fl >= 0 && t[fl].type == JSMN_OBJECT) {
                        tokFieldCopy(ts->kIn,    sizeof(ts->kIn),    js, t, fl, "input",     "input");
                        tokFieldCopy(ts->kOut,   sizeof(ts->kOut),   js, t, fl, "output",    "output");
                        tokFieldCopy(ts->kCr,    sizeof(ts->kCr),    js, t, fl, "cacheRead", "cacheRead");
                        tokFieldCopy(ts->kCw,    sizeof(ts->kCw),    js, t, fl, "cacheWrite","cacheWrite");
                        tokFieldCopy(ts->kTs,    sizeof(ts->kTs),    js, t, fl, "timestamp", "timestamp");
                        tokFieldCopy(ts->kModel, sizeof(ts->kModel), js, t, fl, "model",     "model");
                    } else {
                        lstrcpyA(ts->kIn, "input"); lstrcpyA(ts->kOut, "output");
                        lstrcpyA(ts->kCr, "cacheRead"); lstrcpyA(ts->kCw, "cacheWrite");
                        lstrcpyA(ts->kTs, "timestamp"); lstrcpyA(ts->kModel, "model");
                    }
                    if (ts->sessionsDir && *ts->sessionsDir && ts->app[0]) c->tokSrcCount++;
                    else wideFree(&ts->sessionsDir);
                }
                // advance k past this element
                k += jtokSpan(t, k);
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
                    sp->type = (ty && lstrcmpiW(ty, L"zai") == 0) ? 1
                             : (ty && lstrcmpiW(ty, L"antigravity") == 0) ? 2
                             : (ty && lstrcmpiW(ty, L"generic") == 0) ? 3 : 0;
                    wideFree(&ty);
                    sp->label        = jstrTok(js, t, jobjGet(js, t, k, "label"), L"");
                    sp->authPath     = jstrTok(js, t, jobjGet(js, t, k, "authPath"), L"");
                    sp->clientId     = jstrTok(js, t, jobjGet(js, t, k, "clientId"), L"");
                    sp->clientSecret = jstrTok(js, t, jobjGet(js, t, k, "clientSecret"), L"");
                    sp->configPath   = jstrTok(js, t, jobjGet(js, t, k, "configPath"), L"");
                    sp->vscdbPath    = jstrTok(js, t, jobjGet(js, t, k, "vscdbPath"), L"");
                    sp->providerName = jstrTok(js, t, jobjGet(js, t, k, "provider"), L"");
                    if (sp->type == 3) genParse(sp, js, t, k);
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
