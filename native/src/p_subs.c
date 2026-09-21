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
// Per-provider state: a provider that fails keeps its last good numbers
// marked stale; one provider failing must never blank or stale the others.
typedef struct { wchar_t label[16]; int pct; int rem; int used; int total; long long resetAt; } SubsWin;
static SubsWin g_subsWin[MAX_SUBS][4];  // last good windows per provider
static int g_subsWinN[MAX_SUBS];
static wchar_t g_subsPlan[MAX_SUBS][24]; // last good plan name per provider
static int g_subsProvRem[MAX_SUBS];     // lowest remaining window pct, -1 = never fetched
static int g_subsProvStale[MAX_SUBS];   // last cycle failed but an older value is shown
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

// same, but the raw UTF-8 bytes (caller frees) - for values parsed in place
// (ISO timestamps), where a wide round-trip would only add work
static char *subsJstrRaw(const char *js, jsmntok_t *t, int obj, const char *key) {
    int k = jobjGet(js, t, obj, key);
    if (k < 0 || t[k].type != JSMN_STRING) return NULL;
    int len = t[k].end - t[k].start;
    char *out = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)len + 1);
    if (!out) return NULL;
    memcpy(out, js + t[k].start, len);
    out[len] = 0;
    return out;
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

static void subsSetState(int idx, int rem, int success) {
    if (idx < 0 || idx >= MAX_SUBS) return;
    EnterCriticalSection(&g_subsLock);
    if (success) { g_subsProvRem[idx] = rem; g_subsProvStale[idx] = 0; }
    else if (g_subsProvRem[idx] >= 0) g_subsProvStale[idx] = 1;
    else g_subsProvRem[idx] = -1; // never succeeded: keep the no-data marker
    LeaveCriticalSection(&g_subsLock);
}

static void subsSetWins(int idx, const SubsWin *w, int n) {
    if (idx < 0 || idx >= MAX_SUBS) return;
    if (n > 4) n = 4;
    EnterCriticalSection(&g_subsLock);
    memcpy(g_subsWin[idx], w, n * sizeof(SubsWin));
    g_subsWinN[idx] = n;
    LeaveCriticalSection(&g_subsLock);
}

// plan display name (Electron p.plan); kept across a failed cycle, like the
// windows, so a transient fetch error never blanks the panel head
static void subsSetPlan(int idx, const wchar_t *plan) {
    if (idx < 0 || idx >= MAX_SUBS) return;
    EnterCriticalSection(&g_subsLock);
    if (plan && *plan) lstrcpynW(g_subsPlan[idx], plan, 24);
    else g_subsPlan[idx][0] = 0;
    LeaveCriticalSection(&g_subsLock);
}

static int subsFetchChatgpt(int idx) {
    wchar_t *tok = subsChatgptToken(idx);
    if (!tok) { writeLogA("subs chatgpt: no auth token (open Codex once to refresh login)"); subsSetState(idx, 0, 0); return 0; }
    // access_token is a multi-KB JWT: build the header block at its full size
    int need = 32 + lstrlenW(tok) + 64;
    wchar_t *hdrs = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, need * sizeof(wchar_t));
    if (!hdrs) { wideFree(&tok); subsSetState(idx, 0, 0); return 0; }
    swprintf(hdrs, need, L"Authorization: Bearer %ls\r\nAccept: application/json", tok);
    wideFree(&tok);
    int status = 0, len = 0;
    char *body = subsHttpGet("chatgpt", L"node", L"chatgpt.com", L"/backend-api/wham/usage", hdrs, g_cfg.subsTimeoutMs, &status, &len);
    HeapFree(GetProcessHeap(), 0, hdrs);
    SubsWin wins[2];
    int nwin = 0;
    if (!body || (status != 200 && status != 207)) {
        HeapFree(GetProcessHeap(), 0, body ? (void *)body : 0);
        subsSetState(idx, 0, 0);
        return 0;
    }
    double rem = 100;
    wchar_t *pt = NULL;
    jsmntok_t t[512];
    jsmn_parser p;
    jsmn_init(&p);
    if (jsmn_parse(&p, body, len, t, 512) > 0 && t[0].type == JSMN_OBJECT) {
        pt = subsJstr(body, t, 0, "plan_type");
        int rl = jobjGet(body, t, 0, "rate_limit");
        rem = 100;
        if (rl >= 0 && t[rl].type == JSMN_OBJECT) {
            int reached = subsJint(body, t, rl, "limit_reached", 0);
            double lo = 100;
            int found = 0;
            const char *wnames[2] = { "primary_window", "secondary_window" };
            const wchar_t *wlabels[2] = { L"5h", L"week" };
            for (int i = 0; i < 2; i++) {
                int w = jobjGet(body, t, rl, wnames[i]);
                if (w < 0 || t[w].type != JSMN_OBJECT) continue;
                double used = subsJdouble(body, t, w, "used_percent", -1);
                if (used < 0) continue;
                if (used > 100) used = 100;
                if (used < 0) used = 0;
                lo = found ? (100 - used < lo ? 100 - used : lo) : (100 - used);
                found = 1;
                memset(&wins[nwin], 0, sizeof(SubsWin));
                lstrcpynW(wins[nwin].label, wlabels[i], 16);
                wins[nwin].pct = (int)(used + 0.5);
                wins[nwin].rem = (int)(100.0 - used + 0.5);
                wins[nwin].used = -1;
                wins[nwin].total = -1;
                double ra = subsJdouble(body, t, w, "reset_at", 0);
                if (ra > 0) wins[nwin].resetAt = (long long)(ra * 1000.0);
                nwin++;
            }
            rem = found ? lo : 100;
            if (reached) rem = 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, body);
    subsSetWins(idx, wins, nwin);
    subsSetState(idx, (int)(rem + 0.5), 1);
    subsSetPlan(idx, pt && *pt ? pt : L"chatgpt");
    wideFree(&pt);
    return 1;
}

static int subsFetchZai(int idx) {
    wchar_t *mid = NULL;
    wchar_t *key = subsZaiKey(idx, &mid);
    if (!key) { subsSetState(idx, 0, 0); return 0; }
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

    if (!body) { subsSetState(idx, 0, 0); return 0; }
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
        subsSetState(idx, 0, 0);
        return 0;
    }
    SubsWin wins[4];
    int nwin = 0;
    double lo = 100;
    int limits = jobjGet(body, t, 0, "data");
    int dataObj = limits;
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
                    if (nwin < 4) {
                        int unit = subsJint(body, t, k, "unit", -1);
                        int num = subsJint(body, t, k, "number", 5);
                        memset(&wins[nwin], 0, sizeof(SubsWin));
                        if (unit == 3) swprintf(wins[nwin].label, 16, L"%dh", num ? num : 5);
                        else if (unit == 6) lstrcpynW(wins[nwin].label, L"week", 16);
                        else if (unit == 2) lstrcpynW(wins[nwin].label, L"day", 16);
                        else swprintf(wins[nwin].label, 16, L"unit%d", unit);
                        wins[nwin].pct = (int)(pct + 0.5);
                        wins[nwin].rem = (int)(100.0 - pct + 0.5);
                        wins[nwin].used = (int)(used + 0.5);
                        wins[nwin].total = (int)(total + 0.5);
                        double nr = subsJdouble(body, t, k, "nextResetTime", 0);
                        if (nr > 0) wins[nwin].resetAt = (long long)nr;
                        nwin++;
                    }
                }
                // advance past this element
                k += jtokSpan(t, k);
            }
            if (found) lo = lo < 0 ? 0 : (lo > 100 ? 100 : lo);
            else lo = 100;
        }
    }
    wchar_t *pt = dataObj >= 0 ? subsJstr(body, t, dataObj, "level") : NULL;
    HeapFree(GetProcessHeap(), 0, body);
    subsSetWins(idx, wins, nwin);
    subsSetState(idx, (int)(lo + 0.5), 1);
    subsSetPlan(idx, pt && *pt ? pt : L"coding");
    wideFree(&pt);
    return 1;
}

// ---- Antigravity (Google Cloud Code) ----------------------------------------
// Mirrors src/subs.js _fetchAntigravity: the pi auth.json OAuth entry refreshed
// with Google's public desktop-client creds, grouped quota summary (paid tier)
// with a per-model fallback on 403 SUBSCRIPTION_REQUIRED (free tier - NOT an
// auth failure). Sharp edges:
//  - auth.json "expires" is epoch MILLISECONDS (not seconds).
//  - The IDE fallback needle is "apiKey":"ya29...." - keep the ya29. prefix.
//  - MinGW swprintf is C99: %s = char*, %ls = wchar_t*.
#define AGY_CLIENT_ID     "REDACTED_OAUTH_CLIENT_ID"
#define AGY_CLIENT_SECRET "REDACTED_OAUTH_CLIENT_SECRET"
#define AGY_UA            L"antigravity/1.15.8 windows/amd64"
#define AGY_REFRESH_MARGIN_MS (5 * 60 * 1000)

// current wall clock in epoch ms (FILETIME = 100ns ticks since 1601-01-01)
static long long subsNowMs(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ull) - 11644473600000ll;
}

// ISO-8601 "YYYY-MM-DDTHH:MM:SSZ" -> epoch ms (0 when unparseable).
// days_from_civil (Howard Hinnant): no libc date code, no TZ surprises.
static long long subsIsoToMs(const char *s, int len) {
    if (len < 19) return 0;
    for (int i = 0; i < 19; i++) {
        char c = s[i];
        int wantDigit = (i < 4 || i == 5 || i == 6 || i == 8 || i == 9 || i == 11 || i == 12 || i == 14 || i == 15 || i == 17 || i == 18);
        if (wantDigit && (c < '0' || c > '9')) return 0;
    }
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int mo = (s[5]-'0')*10 + (s[6]-'0');
    int d = (s[8]-'0')*10 + (s[9]-'0');
    int h = (s[11]-'0')*10 + (s[12]-'0');
    int mi = (s[14]-'0')*10 + (s[15]-'0');
    int se = (s[17]-'0')*10 + (s[18]-'0');
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return ((days * 86400 + h * 3600 + mi * 60 + se) * 1000ll);
}

// POST via WinHTTP (the GET helper above is GET-only); returns the body.
static char *subsHttpPost(const char *tag, const wchar_t *host, const wchar_t *path,
                          const wchar_t *headers, const char *body, int bodyLen,
                          int timeoutMs, int *outStatus, int *outLen) {
    *outStatus = 0; *outLen = 0;
    HINTERNET ses = WinHttpOpen(AGY_UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) ses = WinHttpOpen(AGY_UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!ses) { writeLogA("subs agy: open failed"); return NULL; }
    char *result = NULL;
    HINTERNET con = NULL, req = NULL;
    do {
        WinHttpSetTimeouts(ses, 5000, timeoutMs, 5000, timeoutMs);
        con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!con) { writeLogA("subs agy: connect failed"); break; }
        req = WinHttpOpenRequest(con, L"POST", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) break;
        if (headers && *headers) {
            WinHttpAddRequestHeaders(req, headers, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        }
        DWORD total = body && bodyLen > 0 ? (DWORD)bodyLen : 0;
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                (LPVOID)(body && bodyLen > 0 ? body : WINHTTP_NO_REQUEST_DATA),
                                total, total, 0)) {
            char dbg[80];
            sprintf(dbg, "subs agy %s: send err %lu", tag, GetLastError());
            writeLogA(dbg);
            break;
        }
        if (!WinHttpReceiveResponse(req, NULL)) {
            char dbg[80];
            sprintf(dbg, "subs agy %s: recv err %lu", tag, GetLastError());
            writeLogA(dbg);
            break;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        *outStatus = (int)status;
        int cap = 64 * 1024, len = 0;
        result = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
        if (!result) break;
        for (;;) {
            DWORD rd = 0;
            if (len + 8192 > cap) {
                int ncap = cap * 2;
                char *nb = (char *)HeapReAlloc(GetProcessHeap(), 0, result, ncap);
                if (!nb) break;
                result = nb; cap = ncap;
            }
            if (!WinHttpReadData(req, result + len, 8192, &rd)) break;
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

// Parse a possibly large JSON body (the models response is ~150KB): grow the
// token array until jsmn stops reporting NOMEM. Caller frees *outTok.
static int subsParseBig(const char *js, int len, jsmntok_t **outTok) {
    static const int sizes[3] = { 1024, 8192, 32768 };
    for (int s = 0; s < 3; s++) {
        jsmntok_t *t = (jsmntok_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)sizes[s] * sizeof(jsmntok_t));
        if (!t) return 0;
        jsmn_parser p;
        jsmn_init(&p);
        int n = jsmn_parse(&p, js, len, t, sizes[s]);
        if (n >= 0) { *outTok = t; return n; }
        HeapFree(GetProcessHeap(), 0, t);
        if (n != JSMN_ERROR_NOMEM) return 0;
    }
    return 0;
}

typedef struct {
    wchar_t access[4096];
    wchar_t refresh[512];
    long long expires;   // epoch ms
    wchar_t projectId[128];
    int hasRefresh;
    int fromVscdb;
} AgyAuth;

// auth.json: { "antigravity": { access, refresh, expires, projectId } }
// Fallback: the IDE state.vscdb needle "apiKey":"ya29...." (no refresh).
static int subsAgyReadAuth(int idx, AgyAuth *out, wchar_t *authPathOut, int cch) {
    memset(out, 0, sizeof(*out));
    const wchar_t *raw = subsNz(g_cfg.subsProviders[idx].authPath);
    if (!raw) raw = L"~/.pi/agent/auth.json";
    subsPathExpand(raw, authPathOut, cch);
    int len = 0;
    char *buf = subsReadFileUtf8(authPathOut, &len);
    if (buf) {
        jsmntok_t t[512];
        jsmn_parser p;
        jsmn_init(&p);
        if (jsmn_parse(&p, buf, len, t, 512) > 0 && t[0].type == JSMN_OBJECT) {
            int a = jobjGet(buf, t, 0, "antigravity");
            if (a >= 0 && t[a].type == JSMN_OBJECT) {
                wchar_t *acc = subsJstr(buf, t, a, "access");
                wchar_t *ref = subsJstr(buf, t, a, "refresh");
                wchar_t *pid = subsJstr(buf, t, a, "projectId");
                long long exp = (long long)subsJdouble(buf, t, a, "expires", 0);
                if (acc && *acc) lstrcpynW(out->access, acc, 4096);
                if (ref && *ref) { lstrcpynW(out->refresh, ref, 512); out->hasRefresh = 1; }
                if (pid && *pid) lstrcpynW(out->projectId, pid, 128);
                out->expires = exp;
                wideFree(&acc);
                wideFree(&ref);
                wideFree(&pid);
            }
        }
        HeapFree(GetProcessHeap(), 0, buf);
        if (*out->access || out->hasRefresh) return 1;
    }
    // IDE fallback: binary needle scan over the vscdb row blob
    const wchar_t *vraw = subsNz(g_cfg.subsProviders[idx].vscdbPath);
    if (vraw && *vraw) {
        wchar_t vpath[MAX_PATH];
        subsPathExpand(vraw, vpath, MAX_PATH);
        int vl = 0;
        char *vb = subsReadFileUtf8(vpath, &vl);
        if (vb) {
            const char *needle = "\"apiKey\":\"";
            int nl = 10;
            for (int i = 0; i + nl + 5 < vl; i++) {
                if (memcmp(vb + i, needle, nl) != 0) continue;
                int j = i + nl;
                if (j + 5 <= vl && strncmp(vb + j, "ya29.", 5) == 0) {
                    int e = j;
                    while (e < vl && vb[e] != '"' && vb[e] != '\\' && (e - j) < 4000) e++;
                    int tl = e - j;
                    if (tl > 5 && tl < 4000) {
                        MultiByteToWideChar(CP_UTF8, 0, vb + j, tl, out->access, 4096);
                        out->fromVscdb = 1;
                        HeapFree(GetProcessHeap(), 0, vb);
                        return 1;
                    }
                }
            }
            HeapFree(GetProcessHeap(), 0, vb);
        }
    }
    return (*out->access || out->hasRefresh) ? 1 : 0;
}

// Best-effort persist of a rotated token into the pi auth store. Targeted
// text surgery inside the "antigravity" object only - any doubt aborts the
// write (a corrupted auth.json would break the captain's tooling).
static void subsAgySaveAuth(const wchar_t *path, const wchar_t *access, const wchar_t *refresh,
                            long long expiresMs) {
    if (!path || !*path) return;
    int len = 0;
    char *buf = subsReadFileUtf8(path, &len);
    if (!buf) return;
    // locate the top-level "antigravity" key, then its object span
    const char *key = "\"antigravity\"";
    char *k = NULL;
    for (int i = 0; i + 14 < len; i++) {
        if (memcmp(buf + i, key, 13) == 0) { k = buf + i; break; }
    }
    if (!k) { HeapFree(GetProcessHeap(), 0, buf); return; }
    char *ob = k + 13;
    while (ob < buf + len && (*ob == ' ' || *ob == ':' || *ob == '\t' || *ob == '\r' || *ob == '\n')) ob++;
    if (ob >= buf + len || *ob != '{') { HeapFree(GetProcessHeap(), 0, buf); return; }
    int depth = 0;
    char *oe = ob;
    int inStr = 0;
    for (; oe < buf + len; oe++) {
        char c = *oe;
        if (inStr) { if (c == '\\') oe++; else if (c == '"') inStr = 0; continue; }
        if (c == '"') inStr = 1;
        else if (c == '{') depth++;
        else if (c == '}') { depth--; if (!depth) { oe++; break; } }
    }
    if (depth) { HeapFree(GetProcessHeap(), 0, buf); return; }
    char accN[4200], refN[600], expN[32];
    int al = WideCharToMultiByte(CP_UTF8, 0, access, -1, accN, 4200, NULL, NULL);
    int rl = WideCharToMultiByte(CP_UTF8, 0, refresh, -1, refN, 600, NULL, NULL);
    sprintf(expN, "%lld", expiresMs);
    if (al <= 0 || rl <= 0) { HeapFree(GetProcessHeap(), 0, buf); return; }
    // replace the three values inside the object span
    char *out = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)len + 8192);
    if (!out) { HeapFree(GetProcessHeap(), 0, buf); return; }
    int o = 0;
    char *p = buf;
    while (p < oe) {
        // find the next value for one of the three keys within the span
        const char *names[3] = { "\"access\"", "\"refresh\"", "\"expires\"" };
        const char *vals[3] = { accN, refN, expN };
        int vlen[3] = { al - 1, rl - 1, (int)strlen(expN) };
        int quote[3] = { 1, 1, 0 };
        char *best = NULL;
        int bi = -1;
        for (int i = 0; i < 3; i++) {
            char *f = NULL;
            for (char *q = p; q + 12 < oe; q++) {
                if (memcmp(q, names[i], strlen(names[i])) == 0) { f = q; break; }
            }
            if (f && (!best || f < best)) { best = f; bi = i; }
        }
        if (bi < 0) break;
        // copy up to the value start
        char *v = best + strlen(names[bi]);
        while (v < oe && (*v == ' ' || *v == ':' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
        if (v >= oe) break;
        int head = (int)(v - p);
        memcpy(out + o, p, head); o += head;
        if (quote[bi]) out[o++] = '"';
        memcpy(out + o, vals[bi], vlen[bi]); o += vlen[bi];
        if (quote[bi]) out[o++] = '"';
        // skip the old value
        if (quote[bi]) {
            if (*v == '"') {
                v++;
                while (v < oe && *v != '"') { if (*v == '\\') v++; v++; }
                if (v < oe) v++;
            }
        } else {
            while (v < oe && *v != ',' && *v != '}') v++;
        }
        p = v;
    }
    if (p < oe) { int rest = (int)(oe - p); memcpy(out + o, p, rest); o += rest; }
    int tail = (int)(buf + len - oe);
    if (tail > 0) { memcpy(out + o, oe, tail); o += tail; }
    out[o] = 0;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, out, o, &wr, NULL);
        CloseHandle(h);
    }
    HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, buf);
}

// Refresh the access token; 1 = ok (token + refresh + expiry in out)
static int subsAgyRefresh(AgyAuth *a) {
    char body[2048];
    int bl = snprintf(body, sizeof(body),
        "{\"client_id\":\"%s\",\"client_secret\":\"%s\",\"refresh_token\":\"%ls\",\"grant_type\":\"refresh_token\"}",
        AGY_CLIENT_ID, AGY_CLIENT_SECRET, a->refresh);
    if (bl <= 0 || bl >= (int)sizeof(body)) return 0;
    int status = 0, len = 0;
    char *resp = subsHttpPost("token", L"oauth2.googleapis.com", L"/token",
                              L"Content-Type: application/json", body, bl, 20000, &status, &len);
    if (!resp || (status != 200 && status != 207)) {
        char dbg[64];
        sprintf(dbg, "subs agy: token refresh HTTP %d", status);
        writeLogA(dbg);
        HeapFree(GetProcessHeap(), 0, resp ? (void *)resp : 0);
        return 0;
    }
    jsmntok_t t[128];
    jsmn_parser p;
    jsmn_init(&p);
    int ok = 0;
    if (jsmn_parse(&p, resp, len, t, 128) > 0 && t[0].type == JSMN_OBJECT) {
        wchar_t *acc = subsJstr(resp, t, 0, "access_token");
        wchar_t *ref = subsJstr(resp, t, 0, "refresh_token");
        double ein = subsJdouble(resp, t, 0, "expires_in", 3600);
        if (acc && *acc) {
            lstrcpynW(a->access, acc, 4096);
            a->expires = subsNowMs() + (long long)(ein * 1000.0);
            if (ref && *ref) lstrcpynW(a->refresh, ref, 512);
            a->hasRefresh = 1;
            ok = 1;
        }
        wideFree(&acc);
        wideFree(&ref);
    }
    HeapFree(GetProcessHeap(), 0, resp);
    return ok;
}

static void subsAgyHeaders(wchar_t *hdrs, int cch, const wchar_t *token) {
    swprintf(hdrs, cch,
        L"Authorization: Bearer %ls\r\n"
        L"Content-Type: application/json\r\n"
        L"Accept: text/event-stream\r\n"
        L"User-Agent: antigravity/1.15.8 windows/amd64\r\n"
        L"X-Goog-Api-Client: google-cloud-sdk vscode_cloudshelleditor/0.1\r\n"
        L"Client-Metadata: {\"ideType\":\"ANTIGRAVITY\",\"platform\":\"PLATFORM_UNSPECIFIED\",\"pluginType\":\"GEMINI\"}",
        token);
}

static int subsFetchAntigravity(int idx) {
    AgyAuth auth;
    wchar_t authPath[MAX_PATH];
    if (!subsAgyReadAuth(idx, &auth, authPath, MAX_PATH)) {
        writeLogA("subs agy: no login (open Antigravity once)");
        subsSetState(idx, 0, 0);
        return 0;
    }
    // refresh a near-expired token before any quota call
    if (auth.hasRefresh && (!*auth.access || auth.expires < subsNowMs() + AGY_REFRESH_MARGIN_MS)) {
        if (!subsAgyRefresh(&auth)) {
            writeLogA("subs agy: token refresh failed (re-login)");
            subsSetState(idx, 0, 0);
            return 0;
        }
        subsAgySaveAuth(authPath, auth.access, auth.refresh, auth.expires);
    }
    if (!*auth.access) { subsSetState(idx, 0, 0); return 0; }
    wchar_t hdrs[4600];
    subsAgyHeaders(hdrs, 4600, auth.access);
    const wchar_t *hosts[2] = { L"cloudcode-pa.googleapis.com", L"daily-cloudcode-pa.sandbox.googleapis.com" };
    wchar_t plan[24] = L"Antigravity";
    // plan label + project discovery
    if (!*auth.projectId) {
        for (int h = 0; h < 2 && !*auth.projectId; h++) {
            int st = 0, bl = 0;
            char *resp = subsHttpPost("assist", hosts[h], L"/v1internal:loadCodeAssist", hdrs,
                "{\"metadata\":{\"ideType\":\"ANTIGRAVITY\",\"platform\":\"PLATFORM_UNSPECIFIED\",\"pluginType\":\"GEMINI\"}",
                92, g_cfg.subsTimeoutMs, &st, &bl);
            if (!resp) continue;
            if (st == 200) {
                jsmntok_t *t = NULL;
                int n = subsParseBig(resp, bl, &t);
                if (n > 0 && t[0].type == JSMN_OBJECT) {
                    int paid = jobjGet(resp, t, 0, "paidTier");
                    if (paid >= 0 && t[paid].type == JSMN_OBJECT) {
                        wchar_t *nm = subsJstr(resp, t, paid, "name");
                        if (nm && *nm) lstrcpynW(plan, nm, 24);
                        wideFree(&nm);
                    }
                    if (!*plan) {
                        int cur = jobjGet(resp, t, 0, "currentTier");
                        if (cur >= 0 && t[cur].type == JSMN_OBJECT) {
                            wchar_t *nm = subsJstr(resp, t, cur, "name");
                            if (nm && *nm) lstrcpynW(plan, nm, 24);
                            wideFree(&nm);
                        }
                    }
                    wchar_t *pid = subsJstr(resp, t, 0, "cloudaicompanionProject");
                    if (pid && *pid) lstrcpynW(auth.projectId, pid, 128);
                    wideFree(&pid);
                }
                HeapFree(GetProcessHeap(), 0, t);
            }
            HeapFree(GetProcessHeap(), 0, resp);
            if (st == 200 || st == 403) break; // definitive answer
        }
    }
    if (!*auth.projectId) {
        writeLogA("subs agy: no project id");
        subsSetState(idx, 0, 0);
        return 0;
    }
    SubsWin wins[4];
    int nwin = 0;
    double lo = 100;
    // grouped summary first (paid plans); 403 SUBSCRIPTION_REQUIRED = free
    // tier, NOT an auth failure -> silent per-model fallback
    int summaryDefinitive = 0;
    for (int h = 0; h < 2 && !summaryDefinitive; h++) {
        int st = 0, bl = 0;
        char *resp = subsHttpPost("summary", hosts[h], L"/v1internal:retrieveUserQuotaSummary", hdrs,
                                  "{}", 2, g_cfg.subsTimeoutMs, &st, &bl);
        if (!resp) continue;
        if (st == 403) { summaryDefinitive = 1; HeapFree(GetProcessHeap(), 0, resp); break; }
        if (st == 401) {
            writeLogA("subs agy: summary 401 (re-login)");
            HeapFree(GetProcessHeap(), 0, resp);
            subsSetState(idx, 0, 0);
            return 0;
        }
        if (st != 200) { summaryDefinitive = 1; HeapFree(GetProcessHeap(), 0, resp); break; }
        jsmntok_t *t = NULL;
        int n = subsParseBig(resp, bl, &t);
        if (n > 0 && t[0].type == JSMN_OBJECT) {
            int groups = jobjGet(resp, t, 0, "groups");
            if (groups >= 0 && t[groups].type == JSMN_ARRAY) {
                int cnt = t[groups].size;
                int k = groups + 1;
                for (int i = 0; i < cnt && nwin < 4; i++) {
                    jsmntok_t *e = &t[k];
                    if (e->type == JSMN_OBJECT) {
                        wchar_t shortName[16] = L"Models";
                        wchar_t *dn = subsJstr(resp, t, k, "displayName");
                        if (dn && *dn) {
                            lstrcpynW(shortName, dn, 16);
                            // "Gemini Models" -> "Gemini"
                            wchar_t *sp = wcsstr(shortName, L" Models");
                            if (!sp) sp = wcsstr(shortName, L" models");
                            if (sp) *sp = 0;
                        }
                        wideFree(&dn);
                        int buckets = jobjGet(resp, t, k, "buckets");
                        if (buckets >= 0 && t[buckets].type == JSMN_ARRAY) {
                            int bc = t[buckets].size;
                            int bk = buckets + 1;
                            for (int b = 0; b < bc && nwin < 4; b++) {
                                jsmntok_t *be = &t[bk];
                                if (be->type == JSMN_OBJECT) {
                                    double rf = subsJdouble(resp, t, bk, "remainingFraction", -1);
                                    if (rf >= 0) {
                                        if (rf > 1) rf = 1;
                                        double rem = rf * 100.0;
                                        if (rem < lo) lo = rem;
                                        memset(&wins[nwin], 0, sizeof(SubsWin));
                                        wchar_t *win = subsJstr(resp, t, bk, "window");
                                        if (win && lstrcmpiW(win, L"weekly") == 0)
                                            swprintf(wins[nwin].label, 16, L"%ls week", shortName);
                                        else if (win)
                                            swprintf(wins[nwin].label, 16, L"%ls %ls", shortName, win);
                                        else
                                            swprintf(wins[nwin].label, 16, L"%ls win", shortName);
                                        wideFree(&win);
                                        wins[nwin].pct = (int)(100.0 - rem + 0.5);
                                        wins[nwin].rem = (int)(rem + 0.5);
                                        wins[nwin].used = -1;
                                        wins[nwin].total = -1;
                                        char *rt = subsJstrRaw(resp, t, bk, "resetTime");
                                        if (rt) {
                                            char raw[40];
                                            int rl = (int)strlen(rt);
                                            if (rl > 39) rl = 39;
                                            memcpy(raw, rt, rl);
                                            raw[rl] = 0;
                                            wins[nwin].resetAt = subsIsoToMs(raw, rl);
                                        } else {
                                            wins[nwin].resetAt = 0;
                                        }
                                        nwin++;
                                    }
                                }
                                bk += jtokSpan(t, bk);
                            }
                        }
                    }
                    k += jtokSpan(t, k);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, t);
        HeapFree(GetProcessHeap(), 0, resp);
        if (nwin > 0) {
            subsSetWins(idx, wins, nwin);
            subsSetState(idx, (int)(lo + 0.5), 1);
            subsSetPlan(idx, plan);
            return 1;
        }
        break; // 200 but no groups: nothing to retry on the other base
    }
    // per-model fallback: the binding constraint across every non-internal
    // model (internal and chat_* keys are IDE plumbing, not user quota)
    for (int h = 0; h < 2; h++) {
        int st = 0, bl = 0;
        char mbody[256];
        int ml = snprintf(mbody, sizeof(mbody), "{\"project\":\"%ls\"}", auth.projectId);
        if (ml <= 0 || ml >= (int)sizeof(mbody)) break;
        char *resp = subsHttpPost("models", hosts[h], L"/v1internal:fetchAvailableModels", hdrs,
                                  mbody, ml, g_cfg.subsTimeoutMs, &st, &bl);
        if (!resp) continue;
        if (st == 401) {
            writeLogA("subs agy: models 401 (re-login)");
            HeapFree(GetProcessHeap(), 0, resp);
            subsSetState(idx, 0, 0);
            return 0;
        }
        if (st != 200) { HeapFree(GetProcessHeap(), 0, resp); break; }
        jsmntok_t *t = NULL;
        int n = subsParseBig(resp, bl, &t);
        double minRem = -1;
        long long minReset = 0;
        if (n > 0 && t[0].type == JSMN_OBJECT) {
            int models = jobjGet(resp, t, 0, "models");
            if (models >= 0 && t[models].type == JSMN_OBJECT) {
                int cnt = t[models].size; // object: size = number of KEYS
                int k = models + 1;
                for (int i = 0; i < cnt; i++) {
                    // k = key token, k+1 = value token (object children are PAIRS)
                    if (t[k + 1].type == JSMN_OBJECT) {
                        int klen = t[k].end - t[k].start;
                        int isChat = klen > 5 && strncmp(resp + t[k].start, "chat_", 5) == 0;
                        int isInternal = subsJint(resp, t, k + 1, "isInternal", 0);
                        int q = jobjGet(resp, t, k + 1, "quotaInfo");
                        if (!isChat && !isInternal && q >= 0 && t[q].type == JSMN_OBJECT) {
                            double rf = subsJdouble(resp, t, q, "remainingFraction", -1);
                            if (rf >= 0) {
                                if (rf > 1) rf = 1;
                                double rem = rf * 100.0;
                                if (minRem < 0 || rem < minRem) {
                                    minRem = rem;
                                    char *rt = subsJstrRaw(resp, t, q, "resetTime");
                                    minReset = rt ? subsIsoToMs(rt, (int)strlen(rt)) : 0;
                                }
                            }
                        }
                    }
                    k += 1 + jtokSpan(t, k + 1);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, t);
        HeapFree(GetProcessHeap(), 0, resp);
        if (minRem < 0) break;
        memset(&wins[0], 0, sizeof(SubsWin));
        lstrcpynW(wins[0].label, L"models", 16);
        wins[0].pct = (int)(100.0 - minRem + 0.5);
        wins[0].rem = (int)(minRem + 0.5);
        wins[0].used = -1;
        wins[0].total = -1;
        wins[0].resetAt = minReset;
        subsSetWins(idx, wins, 1);
        subsSetState(idx, (int)(minRem + 0.5), 1);
        subsSetPlan(idx, plan);
        return 1;
    }
    subsSetState(idx, 0, 0);
    return 0;
}

static LONG g_subsKick = 0; // board refresh button wakes the cycle early
static volatile unsigned long long g_subsFetchedTick = 0; // cycle end (GetTickCount64)
void subsRefetchNow(void) { InterlockedExchange(&g_subsKick, 1); }
// seconds since the last completed fetch cycle (for the board footer)
unsigned subsFetchedAgoSec(void) {
    unsigned long long t = g_subsFetchedTick;
    if (!t) return 0xFFFFFFFFu;
    return (unsigned)((GetTickCount64() - t) / 1000ull);
}

static DWORD WINAPI subsThreadProc(LPVOID lp) {
    (void)lp;
    for (;;) {
        if (g_cfg.subsEnabled) { // master switch off: no polls, no requests
            int anyEnabled = 0;
            int n = g_cfg.subsProviderCount;
            if (n > MAX_SUBS) n = MAX_SUBS;
            for (int i = 0; i < n; i++) {
                if (!g_cfg.subsProviders[i].enabled) continue;
                anyEnabled = 1;
                if (g_cfg.subsProviders[i].type == 0) subsFetchChatgpt(i);
                else if (g_cfg.subsProviders[i].type == 1) subsFetchZai(i);
                else subsFetchAntigravity(i);
            }
            if (!anyEnabled) {
                for (int i = 0; i < n; i++) subsSetState(i, -1, 0); // no-data marker
            }
            g_subsFetchedTick = GetTickCount64();
        }
        // sleep the interval, but a kick (refresh button) breaks out early
        int ivl = g_cfg.subsIntervalMin > 0 ? g_cfg.subsIntervalMin * 60000 : 120000;
        for (int waited = 0; waited < ivl; waited += 250) {
            if (InterlockedCompareExchange(&g_subsKick, 0, 0)) break;
            Sleep(250);
        }
        InterlockedExchange(&g_subsKick, 0);
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

// chip state readers (UI thread): lowest remaining across providers with data
static int subsChipRem(void) {
    if (!g_subsLockInit) return -1;
    EnterCriticalSection(&g_subsLock);
    int r = -1;
    for (int i = 0; i < MAX_SUBS; i++)
        if (g_subsProvRem[i] >= 0 && (r < 0 || g_subsProvRem[i] < r)) r = g_subsProvRem[i];
    int stale = 0;
    for (int i = 0; i < MAX_SUBS; i++) if (g_subsProvStale[i]) stale = 1;
    LeaveCriticalSection(&g_subsLock);
    return stale ? -2 : r; // -2 = stale, -1 = no data
}

static int subsChipStale(void) {
    if (!g_subsLockInit) return 0;
    EnterCriticalSection(&g_subsLock);
    int s = 0;
    for (int i = 0; i < MAX_SUBS; i++) if (g_subsProvStale[i]) s = 1;
    LeaveCriticalSection(&g_subsLock);
    return s;
}

// ---- subs board (dashboard) readers ----------------------------------------
// provider display label: config label, else the type default (Electron does
// `p.label || 'ChatGPT'` / 'Z.ai')
static void subsProvLabel(int i, wchar_t *out, int cb) {
    const wchar_t *l = i >= 0 && i < g_cfg.subsProviderCount && g_cfg.subsProviders[i].label
                           ? g_cfg.subsProviders[i].label : NULL;
    if (!l) l = (i >= 0 && i < g_cfg.subsProviderCount && g_cfg.subsProviders[i].type == 1) ? L"Z.ai"
              : (i >= 0 && i < g_cfg.subsProviderCount && g_cfg.subsProviders[i].type == 2) ? L"Antigravity"
              : L"ChatGPT";
    lstrcpynW(out, l, cb);
}
// provider plan name (Electron p.plan); empty until the first successful fetch
static void subsProvPlan(int i, wchar_t *out, int cb) {
    out[0] = 0;
    if (i < 0 || i >= MAX_SUBS || !g_subsLockInit) return;
    EnterCriticalSection(&g_subsLock);
    lstrcpynW(out, g_subsPlan[i], cb);
    LeaveCriticalSection(&g_subsLock);
}
static int subsProvEnabled(int i) {
    return i >= 0 && i < g_cfg.subsProviderCount && g_cfg.subsProviders[i].enabled;
}
// copy one provider's last good windows; returns count
static int subsProvWins(int i, SubsWin *out, int max) {
    if (!g_subsLockInit || i < 0 || i >= MAX_SUBS) return 0;
    EnterCriticalSection(&g_subsLock);
    int n = g_subsWinN[i];
    if (n > max) n = max;
    memcpy(out, g_subsWin[i], n * sizeof(SubsWin));
    int stale = g_subsProvStale[i];
    LeaveCriticalSection(&g_subsLock);
    return stale ? -n : n; // negative = stale
}
