// ------------------------------------------------- subscription providers ----
// Mirrors src/subs.js (Electron) for the chip only: ChatGPT wham/usage and
// Z.ai quota/limit, lowest remaining window wins. Fetches run on a worker
// thread (WinHTTP is blocking); the UI timer just reads the latest state.
//
// Sharp edges (do not "simplify"):
//  - The Z.ai endpoint answers HTTP 200 with body {code:500} unless the full
//    ZCode identity header set is present - keep every header.
//  - The token cache / quota endpoints are rewritten live: reads may land
//    mid-write, so require the full body (Content-Length) and retry.
//  - Local midnight TZ: nothing here uses midnight (tokens does; see p_ui).

#include <winhttp.h>

static void writeLogA(const char *s); // p_ui

#define WINHTTP_USER_AGENT_NATIVE L"WizBar/1.0"

static CRITICAL_SECTION g_subsLock;
static int g_subsLockInit = 0;
static int g_subsRem = -1;   // lowest remaining window percent, -1 = never fetched
static int g_subsStale = 0;  // last cycle failed but an older value is shown
static int g_subsThreadStarted = 0;

static const wchar_t *subsNz(const wchar_t *s) { return (s && *s) ? s : NULL; }

static void subsPathExpand(const wchar_t *in, wchar_t *out, int outCch) {
    // "~" -> %USERPROFILE%, keep absolute paths as-is
    if (in[0] == L'~' && (in[1] == L'/' || in[1] == L'\\' || in[1] == 0)) {
        wchar_t up[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
        if (!n || n >= MAX_PATH) { out[0] = 0; return; }
        lstrcpynW(out, up, outCch);
        lstrcatW(out, in + 1);
    } else {
        lstrcpynW(out, in, outCch);
    }
}

static char *subsReadFileUtf8(const wchar_t *path, int *outLen) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL), got = 0;
    if (size == INVALID_FILE_SIZE || !size || size > 32 * 1024 * 1024) { CloseHandle(h); return NULL; }
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)size + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    BOOL ok = ReadFile(h, buf, size, &got, NULL) && got == size;
    CloseHandle(h);
    if (!ok) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    buf[got] = 0;
    *outLen = (int)got;
    return buf;
}

// jsmn helper: string value of obj[key] as wide (caller frees), NULL if absent
static wchar_t *subsJstr(const char *js, jsmntok_t *t, int obj, const char *key) {
    int k = jobjGet(js, t, obj, key);
    if (k < 0) return NULL;
    return jstrTok(js, t, k, NULL);
}

// ---- ChatGPT (wham/usage) ---------------------------------------------------
// auth.json: { auth_mode: "chatgpt", tokens: { access_token: "..." } }
static wchar_t *subsChatgptToken(int idx) {
    wchar_t path[MAX_PATH];
    const wchar_t *rawAuth = subsNz(g_cfg.subsProviders[idx].authPath);
    if (!rawAuth) rawAuth = L"~/.codex/auth.json";
    subsPathExpand(rawAuth, path, MAX_PATH);
    int len = 0;
    char *buf = subsReadFileUtf8(path, &len);

    wchar_t *tok = NULL;
    jsmntok_t t[256];
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, buf, len, t, 256);
    if (n > 0 && t[0].type == JSMN_OBJECT) {
        int am = jobjGet(buf, t, 0, "auth_mode");
        if (am >= 0 && t[am].type == JSMN_STRING) {
            wchar_t *mode = subsJstr(buf, t, 0, "auth_mode");
            if (!mode || lstrcmpiW(mode, L"chatgpt") != 0) { wideFree(&mode); HeapFree(GetProcessHeap(), 0, buf); return NULL; }
            wideFree(&mode);
        }
        int toks = jobjGet(buf, t, 0, "tokens");
        if (toks >= 0 && t[toks].type == JSMN_OBJECT) {
            tok = subsJstr(buf, t, toks, "access_token");
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return tok;
}

// ---- Z.ai key ---------------------------------------------------------------
// config.json: { provider: { "builtin:zai-coding-plan": { enabled, options: { apiKey } } } }
static wchar_t *subsZaiKey(int providerIdx, wchar_t **deviceMid) {
    SubsProvider *sp = &g_cfg.subsProviders[providerIdx];
    wchar_t path[MAX_PATH];
    const wchar_t *rawCfg = subsNz(sp->configPath);
    if (!rawCfg) rawCfg = L"~/.zcode/v2/config.json";
    subsPathExpand(rawCfg, path, MAX_PATH);
    int len = 0;
    char *buf = subsReadFileUtf8(path, &len);

    wchar_t *key = NULL;
    jsmntok_t t[512];
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, buf, len, t, 512);
    if (n > 0 && t[0].type == JSMN_OBJECT) {
        int prov = jobjGet(buf, t, 0, "provider");
        if (prov >= 0 && t[prov].type == JSMN_OBJECT) {
            int cnt = t[prov].size;
            int k = prov + 1;
            for (int i = 0; i < cnt; i++) {
                jsmntok_t *kt = &t[k];
                if (kt->type != JSMN_STRING) continue;
                int nameLen = kt->end - kt->start;
                wchar_t *wn = utf8ToWide(buf + kt->start, nameLen);
                int match;
                if (sp->providerName && *sp->providerName) match = wn && lstrcmpiW(wn, sp->providerName) == 0;
                else match = wn && lstrcmpiW(wn, L"builtin:zai-coding-plan") == 0;
                if (!match) { wideFree(&wn); k += 1 + jtokSpan(t, k + 1); continue; }
                wideFree(&wn);
                int entry = k + 1;
                if (entry >= n || t[entry].type != JSMN_OBJECT) { k += 1 + jtokSpan(t, k + 1); continue; }
                int en = jobjGet(buf, t, entry, "enabled");
                if (en >= 0 && t[en].type == JSMN_PRIMITIVE) {
                    if (buf[t[en].start] == 'f') break; // enabled: false
                }
                int opts = jobjGet(buf, t, entry, "options");
                if (opts >= 0 && t[opts].type == JSMN_OBJECT) {
                    key = subsJstr(buf, t, opts, "apiKey");
                }
                break;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    // deviceMid sits next to the config: telemetry-state.json
    if (deviceMid) {
        *deviceMid = NULL;
        wchar_t dir[MAX_PATH], tpath[MAX_PATH * 2];
        wcsncpy_s(dir, MAX_PATH, path, _TRUNCATE);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (!slash) slash = wcsrchr(dir, L'/');
        if (slash) {
            *slash = 0;
            swprintf(tpath, MAX_PATH * 2, L"%ls\\telemetry-state.json", dir);
            int tl = 0;
            char *tb = subsReadFileUtf8(tpath, &tl);
            if (tb) {
                jsmntok_t tt[128];
                jsmn_parser tp2;
                jsmn_init(&tp2);
                if (jsmn_parse(&tp2, tb, tl, tt, 128) > 0 && tt[0].type == JSMN_OBJECT) {
                    *deviceMid = subsJstr(tb, tt, 0, "deviceMid");
                }
                HeapFree(GetProcessHeap(), 0, tb);
            }
        }
    }
    return key;
}

// ---- HTTP GET via WinHTTP ---------------------------------------------------
static char *subsHttpGet(const char *tag, const wchar_t *ua, const wchar_t *host, const wchar_t *path, const wchar_t *headers,
                         int timeoutMs, int *outStatus, int *outLen) {
    *outStatus = 0; *outLen = 0;
    HINTERNET ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!ses) { writeLogA("subs: open failed"); return NULL; }
    char *result = NULL;
    HINTERNET con = NULL, req = NULL;
    do {
        WinHttpSetTimeouts(ses, 5000, timeoutMs, 5000, timeoutMs);
        con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!con) { writeLogA("subs: connect failed"); break; }
        req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) break;
        if (headers && *headers) {
            WinHttpAddRequestHeaders(req, headers, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            char dbg[80];
            sprintf(dbg, "subs %s: send err %lu", tag, GetLastError());
            writeLogA(dbg);
            break;
        }
        if (!WinHttpReceiveResponse(req, NULL)) {
            char dbg[80];
            sprintf(dbg, "subs %s: recv err %lu", tag, GetLastError());
            writeLogA(dbg);
            break;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        *outStatus = (int)status;
        int cap = 32 * 1024, len = 0;
        result = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
        if (!result) break;
        for (;;) {
            DWORD rd = 0;
            if (len + 4096 > cap) {
                int ncap = cap * 2;
                char *nb = (char *)HeapReAlloc(GetProcessHeap(), 0, result, ncap);
                if (!nb) break;
                result = nb; cap = ncap;
            }
            if (!WinHttpReadData(req, result + len, 4096, &rd)) break;
            if (!rd) break;
            len += (int)rd;
        }
        result[len] = 0;
        *outLen = len;
    } while (0);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return result;
}

static double subsJdouble(const char *js, jsmntok_t *t, int obj, const char *key, double dflt) {
    int k = jobjGet(js, t, obj, key);
    if (k < 0 || t[k].type != JSMN_PRIMITIVE) return dflt;
    int isNum = 0;
    for (int i = t[k].start; i < t[k].end; i++) {
        char c = js[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+') { isNum = 1; break; }
    }
    if (!isNum) return dflt; // null / true / false
    char tmp[32];
    int len = t[k].end - t[k].start;
    if (len >= 32) len = 31;
    memcpy(tmp, js + t[k].start, len);
    tmp[len] = 0;
    return atof(tmp);
}

static int subsJint(const char *js, jsmntok_t *t, int obj, const char *key, int dflt) {
    return (int)subsJdouble(js, t, obj, key, (double)dflt);
}

static void subsSetState(int rem, int success) {
    EnterCriticalSection(&g_subsLock);
    if (success) { g_subsRem = rem; g_subsStale = 0; }
    else if (g_subsRem >= 0) g_subsStale = 1;
    else g_subsRem = -1; // never succeeded: keep the no-data marker
    LeaveCriticalSection(&g_subsLock);
}

static int subsFetchChatgpt(int idx) {
    wchar_t *tok = subsChatgptToken(idx);
    if (!tok) { writeLogA("subs chatgpt: no auth token (open Codex once to refresh login)"); subsSetState(0, 0); return 0; }
    // access_token is a multi-KB JWT: build the header block at its full size
    int need = 32 + lstrlenW(tok) + 64;
    wchar_t *hdrs = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, need * sizeof(wchar_t));
    if (!hdrs) { wideFree(&tok); subsSetState(0, 0); return 0; }
    swprintf(hdrs, need, L"Authorization: Bearer %ls\r\nAccept: application/json", tok);
    wideFree(&tok);
    int status = 0, len = 0;
    char *body = subsHttpGet("chatgpt", L"node", L"chatgpt.com", L"/backend-api/wham/usage", hdrs, g_cfg.subsTimeoutMs, &status, &len);
    HeapFree(GetProcessHeap(), 0, hdrs);
    if (!body || (status != 200 && status != 207)) {
        HeapFree(GetProcessHeap(), 0, body ? (void *)body : 0);
        subsSetState(0, 0);
        return 0;
    }
    double rem = 100;
    jsmntok_t t[512];
    jsmn_parser p;
    jsmn_init(&p);
    if (jsmn_parse(&p, body, len, t, 512) > 0 && t[0].type == JSMN_OBJECT) {
        int rl = jobjGet(body, t, 0, "rate_limit");
        rem = 100;
        if (rl >= 0 && t[rl].type == JSMN_OBJECT) {
            int reached = subsJint(body, t, rl, "limit_reached", 0);
            double lo = 100;
            int found = 0;
            const char *wins[2] = { "primary_window", "secondary_window" };
            for (int i = 0; i < 2; i++) {
                int w = jobjGet(body, t, rl, wins[i]);
                if (w < 0 || t[w].type != JSMN_OBJECT) continue;
                double used = subsJdouble(body, t, w, "used_percent", -1);
                if (used < 0) continue;
                if (used > 100) used = 100;
                if (used < 0) used = 0;
                lo = found ? (100 - used < lo ? 100 - used : lo) : (100 - used);
                found = 1;
            }
            rem = found ? lo : 100;
            if (reached) rem = 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, body);
    subsSetState((int)(rem + 0.5), 1);
    return 1;
}

static int subsFetchZai(int idx) {
    wchar_t *mid = NULL;
    wchar_t *key = subsZaiKey(idx, &mid);
    if (!key) { subsSetState(0, 0); return 0; }
    wchar_t hdrs[1600];
    swprintf(hdrs, 1600,
        L"authorization: %ls\r\n"
        L"Accept: application/json\r\n"
        L"x-api-key: %ls\r\n"
        L"X-ZCode-App-Version: 3.11.2\r\n"
        L"X-ZCode-Agent: glm\r\n"
        L"X-Title: Z Code@electron\r\n"
        L"HTTP-Referer: https://zcode.z.ai\r\n"
        L"X-Release-Channel: production\r\n"
        L"X-Client-Language: en-US\r\n"
        L"X-Client-Timezone: UTC\r\n"
        L"X-Platform: win32-x64\r\n"
        L"X-Os-Category: win32\r\n"
        L"X-Os-Version: 10.0.0\r\n"
        L"X-Device-Mid: %ls",
        key, key, (mid && *mid) ? mid : L"");
    wideFree(&key);
    wideFree(&mid);
    int status = 0, len = 0;
    char *body = subsHttpGet("zai", L"ZCode/3.11.2", L"api.z.ai", L"/api/monitor/usage/quota/limit", hdrs, g_cfg.subsTimeoutMs, &status, &len);

    if (!body) { subsSetState(0, 0); return 0; }
    jsmntok_t t[1024];
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, body, len, t, 1024);
    int code = n > 0 ? subsJint(body, t, 0, "code", 200) : 0;
    int okShape = n > 0 && t[0].type == JSMN_OBJECT && code == 200 &&
                  subsJint(body, t, 0, "success", 1) != 0;
    if (status != 200 || !okShape) {
        char dbg[64];
        sprintf(dbg, "subs zai: reject code=%d", code);
        writeLogA(dbg);
        HeapFree(GetProcessHeap(), 0, body);
        subsSetState(0, 0);
        return 0;
    }
    double lo = 100;
    int limits = jobjGet(body, t, 0, "data");
    if (limits >= 0 && t[limits].type == JSMN_OBJECT) {
        limits = jobjGet(body, t, limits, "limits");
        if (limits >= 0 && t[limits].type == JSMN_ARRAY) {
            int cnt = t[limits].size;
            int k = limits + 1;
            int found = 0;
            for (int i = 0; i < cnt; i++) {
                jsmntok_t *e = &t[k];
                if (e->type == JSMN_OBJECT) {
                    double used = subsJdouble(body, t, k, "currentValue", 0);
                    double total = subsJdouble(body, t, k, "usage", 0);
                    double pct = total > 0 ? (used / total) * 100.0 : 0;
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    double rem = 100 - pct;
                    lo = found ? (rem < lo ? rem : lo) : rem;
                    found = 1;
                }
                // advance past this element
                k += jtokSpan(t, k);
            }
            if (found) lo = lo < 0 ? 0 : (lo > 100 ? 100 : lo);
            else lo = 100;
        }
    }
    HeapFree(GetProcessHeap(), 0, body);
    subsSetState((int)(lo + 0.5), 1);
    return 1;
}

static DWORD WINAPI subsThreadProc(LPVOID lp) {
    (void)lp;
    for (;;) {
        int any = 0;
        int n = g_cfg.subsProviderCount;
        if (n > MAX_SUBS) n = MAX_SUBS;
        for (int i = 0; i < n; i++) {
            if (!g_cfg.subsProviders[i].enabled) continue;
            int ok = g_cfg.subsProviders[i].type == 0 ? subsFetchChatgpt(i) : subsFetchZai(i);
        }
        if (!any) subsSetState(-1, 0); // nothing succeeded: no-data marker
        Sleep(g_cfg.subsIntervalMin > 0 ? g_cfg.subsIntervalMin * 60000 : 120000);
    }
    return 0;
}

static void subsStart(void) {
    if (g_subsThreadStarted) return;
    g_subsThreadStarted = 1;
    if (!g_subsLockInit) {
        InitializeCriticalSection(&g_subsLock);
        g_subsLockInit = 1;
    }
    HANDLE h = CreateThread(NULL, 0, subsThreadProc, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

// chip state readers (UI thread)
static int subsChipRem(void) {
    if (!g_subsLockInit) return -1;
    EnterCriticalSection(&g_subsLock);
    int r = g_subsStale ? -2 : g_subsRem;
    LeaveCriticalSection(&g_subsLock);
    return r; // -2 = stale (old value lives in g_subsRem), -1 = no data
}

static int subsChipStale(void) {
    if (!g_subsLockInit) return 0;
    EnterCriticalSection(&g_subsLock);
    int s = g_subsStale;
    LeaveCriticalSection(&g_subsLock);
    return s;
}
