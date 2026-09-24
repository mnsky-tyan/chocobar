// ------------------------------------------------- subscription providers ----
// The chip mirrors src/subs.js (Electron): ChatGPT wham/usage and Z.ai quota/
// limit, lowest remaining window wins. Antigravity and the config-only generic
// provider (a REST quota endpoint declared entirely in subs.providers[]) have no
// Electron counterpart - they exist here only.
// Every enabled provider gets its OWN thread and they run concurrently: each
// fetch is independent (each writes only its own slot through the locked
// setters), so a cycle costs the slowest provider rather than the sum.
//
// Sharp edges (do not "simplify"):
//  - The Z.ai endpoint answers HTTP 200 with body {code:500} unless the full
//    ZCode identity header set is present - keep every header.
//  - The token cache / quota endpoints are rewritten live: reads may land
//    mid-write, so require the full body (Content-Length) and retry.
//  - Local midnight TZ: nothing here uses midnight (tokens does; see p_ui).

#include <winhttp.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

static void writeLogA(const char *s); // p_ui

#define WINHTTP_USER_AGENT_NATIVE L"WizBar/1.0"

static CRITICAL_SECTION g_subsLock;
static int g_subsLockInit = 0;
// Per-provider state: a provider that fails keeps its last good numbers
// marked stale; one provider failing must never blank or stale the others.
// label needs 24 wchars: "Claude/GPT week" is 15 but the group prefix is
// truncated before the window is appended, which used to garble every row of
// the antigravity panel ("CLAUDE AND GPT" lost its window suffix).
typedef struct { wchar_t label[24]; int pct; int rem; int used; int total; long long resetAt; } SubsWin;
static SubsWin g_subsWin[MAX_SUBS][MAX_GEN_WIN]; // last good windows per provider
static int g_subsWinN[MAX_SUBS];
static wchar_t g_subsPlan[MAX_SUBS][24]; // last good plan name per provider
// absolute prompt credits where the vendor reports them (Antigravity's local
// GetUserStatus does: availablePromptCredits / monthlyPromptCredits). The chip
// stays a percentage; the board head shows the numbers.
static int g_subsCredAvail[MAX_SUBS]; static int g_subsCredTotal[MAX_SUBS];
static int g_subsProvRem[MAX_SUBS];     // lowest remaining window pct, -1 = never fetched
static int g_subsProvStale[MAX_SUBS];   // last cycle failed but an older value is shown
static int g_subsThreadStarted = 0;

static const wchar_t *subsNz(const wchar_t *s) { return (s && *s) ? s : NULL; }
// readers defined further down (the fetch thread logs what it stored)
static void subsProvLabel(int i, wchar_t *out, int cb);
static void subsViewLabel(const Config *v, int i, wchar_t *out, int cb);
static void subsProvPlan(int i, wchar_t *out, int cb);
static int subsProvWins(int i, SubsWin *out, int max);
static int subsProvEnabled(int i);

static void subsPathExpand(const wchar_t *in, wchar_t *out, int outCch) {
    // "~" -> %USERPROFILE%, keep absolute paths as-is
    if (in[0] == L'~' && (in[1] == L'/' || in[1] == L'\\' || in[1] == 0)) {
        wchar_t up[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
        if (!n || n >= MAX_PATH) { out[0] = 0; return; }
        lstrcpynW(out, up, outCch);
        // the remainder has to stay inside the caller's buffer: a long profile
        // plus a long path would otherwise run off the end of it
        int ul = lstrlenW(out);
        if (ul < outCch) lstrcpynW(out + ul, in + 1, outCch - ul);
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

// the raw UTF-8 bytes of ONE ALREADY SELECTED token (caller frees), NULL if it
// is not a string - for values parsed in place (ISO timestamps), where a wide
// round-trip would only add work
static char *subsJstrRawTok(const char *js, const jsmntok_t *t, int tok) {
    if (tok < 0 || t[tok].type != JSMN_STRING) return NULL;
    int len = t[tok].end - t[tok].start;
    char *out = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)len + 1);
    if (!out) return NULL;
    memcpy(out, js + t[tok].start, (size_t)len);
    out[len] = 0;
    return out;
}

// same, but the string value of obj[key] (caller frees), NULL if absent
static char *subsJstrRaw(const char *js, jsmntok_t *t, int obj, const char *key) {
    return subsJstrRawTok(js, t, jobjGet(js, t, obj, key));
}

// ---- ChatGPT (wham/usage) ---------------------------------------------------
// auth.json: { auth_mode: "chatgpt", tokens: { access_token: "..." } }
static wchar_t *subsChatgptToken(const Config *cfg, int idx) {
    wchar_t path[MAX_PATH];
    const wchar_t *rawAuth = subsNz(cfg->subsProviders[idx].authPath);
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
static wchar_t *subsZaiKey(const Config *cfg, int providerIdx, wchar_t **deviceMid) {
    const SubsProvider *sp = &cfg->subsProviders[providerIdx];
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
static char *subsHttpGet(const char *tag, const wchar_t *ua, const wchar_t *host, int port,
                         const wchar_t *path, const wchar_t *headers,
                         int insecure, int timeoutMs, int *outStatus, int *outLen) {
    *outStatus = 0; *outLen = 0;
    HINTERNET ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!ses) { writeLogA("subs: open failed"); return NULL; }
    char *result = NULL;
    HINTERNET con = NULL, req = NULL;
    do {
        WinHttpSetTimeouts(ses, 5000, timeoutMs, 5000, timeoutMs);
        con = WinHttpConnect(ses, host, port ? (INTERNET_PORT)port
                                                    : (insecure ? INTERNET_DEFAULT_HTTP_PORT
                                                                : INTERNET_DEFAULT_HTTPS_PORT), 0);
        if (!con) { writeLogA("subs: connect failed"); break; }
        req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, insecure ? 0 : WINHTTP_FLAG_SECURE);
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
    if (n > MAX_GEN_WIN) n = MAX_GEN_WIN;
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

// store the credit pair; -1 = unknown (the head then shows no numbers)
static void subsSetCredits(int idx, int avail, int total) {
    if (idx < 0 || idx >= MAX_SUBS) return;
    EnterCriticalSection(&g_subsLock);
    g_subsCredAvail[idx] = avail; g_subsCredTotal[idx] = total;
    LeaveCriticalSection(&g_subsLock);
}

void subsCredits(int i, int *avail, int *total) {
    if (i < 0 || i >= MAX_SUBS) { *avail = *total = -1; return; }
    EnterCriticalSection(&g_subsLock);
    *avail = g_subsCredAvail[i]; *total = g_subsCredTotal[i];
    LeaveCriticalSection(&g_subsLock);
}

static int subsFetchChatgpt(const Config *cfg, int idx) {
    wchar_t *tok = subsChatgptToken(cfg, idx);
    if (!tok) { writeLogA("subs chatgpt: no auth token (open Codex once to refresh login)"); subsSetState(idx, 0, 0); return 0; }
    // access_token is a multi-KB JWT: build the header block at its full size
    int need = 32 + lstrlenW(tok) + 64;
    wchar_t *hdrs = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, need * sizeof(wchar_t));
    if (!hdrs) { wideFree(&tok); subsSetState(idx, 0, 0); return 0; }
    swprintf(hdrs, need, L"Authorization: Bearer %ls\r\nAccept: application/json", tok);
    wideFree(&tok);
    int status = 0, len = 0;
    char *body = subsHttpGet("chatgpt", L"node", L"chatgpt.com", 0, L"/backend-api/wham/usage", hdrs, 0, cfg->subsTimeoutMs, &status, &len);
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

static int subsFetchZai(const Config *cfg, int idx) {
    wchar_t *mid = NULL;
    wchar_t *key = subsZaiKey(cfg, idx, &mid);
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
    char *body = subsHttpGet("zai", L"ZCode/3.11.2", L"api.z.ai", 0, L"/api/monitor/usage/quota/limit", hdrs, 0, cfg->subsTimeoutMs, &status, &len);

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
    SubsWin wins[MAX_GEN_WIN];
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
                    if (nwin < MAX_GEN_WIN) {
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
// The Google desktop OAuth pair is NOT compiled in: it is personal wiring and
// lives in the user config (subs.providers[].clientId / clientSecret, like
// authPath). The local language-server source needs no credentials at all;
// only the cloud fallback does, and it reports "not configured" without them.
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
static char *subsHttpPost(const char *tag, const wchar_t *ua, const wchar_t *host, int port, const wchar_t *path,
                          const wchar_t *headers, const char *body, int bodyLen,
                          int insecure, int timeoutMs, int *outStatus, int *outLen) {
    *outStatus = 0; *outLen = 0;
    HINTERNET ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!ses) { writeLogA("subs agy: open failed"); return NULL; }
    char *result = NULL;
    HINTERNET con = NULL, req = NULL;
    do {
        WinHttpSetTimeouts(ses, 5000, timeoutMs, 5000, timeoutMs);
        con = WinHttpConnect(ses, host, port ? (INTERNET_PORT)port
                                                    : (insecure ? INTERNET_DEFAULT_HTTP_PORT
                                                                : INTERNET_DEFAULT_HTTPS_PORT), 0);
        if (!con) { writeLogA("subs agy: connect failed"); break; }
        req = WinHttpOpenRequest(con, L"POST", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, insecure ? 0 : WINHTTP_FLAG_SECURE);
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
static int subsAgyReadAuth(const Config *cfg, int idx, AgyAuth *out, wchar_t *authPathOut, int cch) {
    memset(out, 0, sizeof(*out));
    const wchar_t *raw = subsNz(cfg->subsProviders[idx].authPath);
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
    const wchar_t *vraw = subsNz(cfg->subsProviders[idx].vscdbPath);
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
    // Copy the prefix before the antigravity object verbatim, then start the
    // search AT the object (ob, never buf): a sibling provider stored earlier
    // in the file (openai-codex also has access/refresh/expires) must never be
    // touched - only the antigravity object's own keys are replaced.
    int pre = (int)(ob - buf);
    memcpy(out, buf, (size_t)pre);
    o = pre;
    char *p = ob;
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
        // The old value must match the shape we are about to write: a JSON string
        // for the quoted keys, a number for expires. Anything else means the
        // store is not the shape this surgery assumes, so abort the whole write
        // (invariant: any doubt aborts) instead of splicing into invalid JSON.
        if (quote[bi]) {
            if (*v != '"') { HeapFree(GetProcessHeap(), 0, out); HeapFree(GetProcessHeap(), 0, buf); return; }
        } else if (!(*v == '-' || (*v >= '0' && *v <= '9'))) {
            HeapFree(GetProcessHeap(), 0, out); HeapFree(GetProcessHeap(), 0, buf); return;
        }
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
    // Atomic write-back: never truncate the live credential store. Write a
    // sibling temp file and rename it over the original, so an interruption
    // leaves the store intact; a refused rename (another process holds the
    // file open) is logged rather than silently dropping the rotated token.
    wchar_t tmp[MAX_PATH + 8];
    lstrcpynW(tmp, path, MAX_PATH);
    lstrcatW(tmp, L".tmp");
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        BOOL wrote = WriteFile(h, out, (DWORD)o, &wr, NULL);
        CloseHandle(h);
        // rename only when the temp holds the COMPLETE payload: a partial or
        // failed write must leave the live store untouched, not replace it
        // with a truncated file.
        if (wrote && wr == (DWORD)o) {
            if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
                writeLogA("subs agy: auth token write-back failed (store locked or read-only)");
                DeleteFileW(tmp);
            }
        } else {
            writeLogA("subs agy: auth temp file write incomplete");
            DeleteFileW(tmp);
        }
    } else {
        writeLogA("subs agy: auth temp file could not be created");
        DeleteFileW(tmp);
    }
    HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, buf);
}

// Refresh the access token; 1 = ok (token + refresh + expiry in out)
static int subsAgyRefresh(AgyAuth *a, const wchar_t *clientId, const wchar_t *clientSecret) {
    if (!clientId || !*clientId || !clientSecret || !*clientSecret) {
        writeLogA("subs agy: no OAuth pair in config (local source still works)");
        return 0;
    }
    char cid[256], cs[256];
    WideCharToMultiByte(CP_UTF8, 0, clientId, -1, cid, sizeof(cid), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, clientSecret, -1, cs, sizeof(cs), NULL, NULL);
    char body[2048];
    int bl = snprintf(body, sizeof(body),
        "{\"client_id\":\"%s\",\"client_secret\":\"%s\",\"refresh_token\":\"%ls\",\"grant_type\":\"refresh_token\"}",
        cid, cs, a->refresh);
    if (bl <= 0 || bl >= (int)sizeof(body)) return 0;
    int status = 0, len = 0;
    char *resp = subsHttpPost("token", AGY_UA, L"oauth2.googleapis.com", 0, L"/token",
                              L"Content-Type: application/json", body, bl, 0, 20000, &status, &len);
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


// one fetchAvailableModels call; runs on its own thread so the two endpoints
// are fetched concurrently instead of back to back
typedef struct {
    const wchar_t *host; const wchar_t *hdrs; const wchar_t *project;
    int timeoutMs; int st, bl; char *resp;
} AgyModelsJob;
static DWORD WINAPI agyModelsThread(LPVOID lp) {
    AgyModelsJob *j = (AgyModelsJob *)lp;
    char mbody[256];
    int ml = snprintf(mbody, sizeof(mbody), "{\"project\":\"%ls\"}", j->project);
    if (ml <= 0 || ml >= (int)sizeof(mbody)) { j->resp = NULL; return 0; }
    j->resp = subsHttpPost("models", AGY_UA, j->host, 0, L"/v1internal:fetchAvailableModels", j->hdrs,
                           mbody, ml, 0, j->timeoutMs, &j->st, &j->bl);
    return 0;
}

// per-family model keys in priority order, exactly the keys pi-quota-inject
// picks: the first key that carries a quotaInfo wins for that family
static const char *agyFamKeys[2][5] = {
    { "gemini-3.8-flash-tiered", "gemini-3.7-flash-tiered", "gemini-3.6-flash-high", "gemini-pro-agent", NULL },
    { "claude-sonnet-4-6", "claude-opus-4-6-thinking", "gpt-oss-120b-medium", NULL, NULL }
};

// Fetch /v1internal:fetchAvailableModels on BOTH endpoints CONCURRENTLY and
// merge them into the family slots: production first, then the daily/sandbox
// endpoint OVERWRITING it - byte-for-byte the Object.assign order of
// pi-quota-inject.mjs. Each call costs ~1 s of pure server latency for ~160 KB,
// so serialising them doubled the most expensive part of a subs cycle.
// Returns 1 if both endpoints were attempted, 0 if neither, -1 on a 401.
static int agyFetchModels(const wchar_t *const *hosts, const wchar_t *hdrs, const wchar_t *project,
                          int timeoutMs, int debug,
                          double *famRf, long long *famReset, int *famHave) {
    AgyModelsJob mj[2];
    HANDLE mth[2]; int started[2] = { 0, 0 };
    for (int h = 0; h < 2; h++) {
        memset(&mj[h], 0, sizeof(mj[h]));
        mj[h].host = hosts[h];
        mj[h].hdrs = hdrs; mj[h].project = project; mj[h].timeoutMs = timeoutMs;
        mth[h] = CreateThread(NULL, 0, agyModelsThread, &mj[h], 0, NULL);
        // the mask is PER HOST, not a count: a thread that could not start
        // leaves only ITS host to the inline fetch below, while a host whose
        // thread did start keeps that thread's result
        if (mth[h]) started[h] = 1; else break;
    }
    // join and close every thread that was created: a thread that is still
    // running keeps writing into mj[] (and, when this was called speculatively,
    // into the CALLER's stack frame) after this function returns
    for (int h = 0; h < 2; h++) {
        if (!started[h]) continue;
        WaitForSingleObject(mth[h], INFINITE);
        CloseHandle(mth[h]);
    }
    int attempted = 0, auth401 = 0;
    for (int h = 0; h < 2; h++) {
        int st = 0, bl = 0; char *resp;
        if (started[h]) { st = mj[h].st; bl = mj[h].bl; resp = mj[h].resp; }
        else {
            char mbody[256];
            int ml = snprintf(mbody, sizeof(mbody), "{\"project\":\"%ls\"}", project);
            if (ml <= 0 || ml >= (int)sizeof(mbody)) break;
            resp = subsHttpPost("models", AGY_UA, hosts[h], 0, L"/v1internal:fetchAvailableModels", hdrs,
                                mbody, ml, 0, timeoutMs, &st, &bl);
        }
        attempted = 1;
        if (debug) {
            char lb[160];
            sprintf(lb, "[wizbar] subs agy models host=%d st=%d bl=%d", h, st, bl);
            writeLogA(lb);
        }
        if (!resp) continue;
        if (st == 401) { auth401 = 1; HeapFree(GetProcessHeap(), 0, resp); continue; }
        if (st != 200) { HeapFree(GetProcessHeap(), 0, resp); continue; }
        jsmntok_t *t = NULL;
        int n = subsParseBig(resp, bl, &t);
        if (n > 0 && t[0].type == JSMN_OBJECT) {
            int models = jobjGet(resp, t, 0, "models");
            if (models >= 0 && t[models].type == JSMN_OBJECT) {
                int cnt = t[models].size; // object: size = number of KEYS
                int k = models + 1;
                for (int i = 0; i < cnt; i++) {
                    // object children are key+value PAIRS: advance 1 + span(value)
                    if (t[k + 1].type == JSMN_OBJECT) {
                        int klen = t[k].end - t[k].start;
                        const char *ks = resp + t[k].start;
                        for (int f = 0; f < 2; f++) {
                            int matched = 0;
                            for (int ki = 0; agyFamKeys[f][ki]; ki++) {
                                int kl = (int)strlen(agyFamKeys[f][ki]);
                                if (klen != kl || strncmp(ks, agyFamKeys[f][ki], klen) != 0) continue;
                                // tracked key: OVERWRITE the slot unconditionally,
                                // exactly like Object.assign in pi-quota-inject
                                // (the later endpoint's entry replaces the whole
                                // model, quotaInfo or not)
                                int q = jobjGet(resp, t, k + 1, "quotaInfo");
                                famHave[f] = q >= 0 && t[q].type == JSMN_OBJECT;
                                famRf[f] = -1;
                                famReset[f] = 0;
                                if (famHave[f]) {
                                    famRf[f] = subsJdouble(resp, t, q, "remainingFraction", -1);
                                    if (famRf[f] > 1) famRf[f] = 1;
                                    char *rt = subsJstrRaw(resp, t, q, "resetTime");
                                    if (rt) {
                                        famReset[f] = subsIsoToMs(rt, (int)strlen(rt));
                                        HeapFree(GetProcessHeap(), 0, rt);
                                    }
                                }
                                matched = 1;
                                break;
                            }
                            if (matched) break;
                        }
                    }
                    k += 1 + jtokSpan(t, k + 1);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, t);
        HeapFree(GetProcessHeap(), 0, resp);
    }
    return auth401 ? -1 : (attempted ? 1 : 0);
}

// the same fetch driven from a thread, so it can overlap loadCodeAssist
typedef struct {
    const wchar_t *const *hosts; const wchar_t *hdrs;
    wchar_t project[128];   // a COPY: the caller rewrites its own buffer
    int timeoutMs, debug;
    double famRf[2]; long long famReset[2]; int famHave[2];
    int result;
} AgyQuotaJob;
static DWORD WINAPI agyQuotaThread(LPVOID lp) {
    AgyQuotaJob *j = (AgyQuotaJob *)lp;
    j->result = agyFetchModels(j->hosts, j->hdrs, j->project, j->timeoutMs, j->debug,
                               j->famRf, j->famReset, j->famHave);
    return 0;
}

static int subsFetchAntigravity(const Config *cfg, int idx) {
    subsSetCredits(idx, -1, -1);
    AgyAuth auth;
    wchar_t authPath[MAX_PATH];
    if (!subsAgyReadAuth(cfg, idx, &auth, authPath, MAX_PATH)) {
        writeLogA("subs agy: no login (open Antigravity once)");
        subsSetState(idx, 0, 0);
        return 0;
    }
    // refresh a near-expired token before any quota call
    if (auth.hasRefresh && (!*auth.access || auth.expires < subsNowMs() + AGY_REFRESH_MARGIN_MS)) {
        const wchar_t *cid = idx >= 0 && idx < cfg->subsProviderCount ? cfg->subsProviders[idx].clientId : NULL;
        const wchar_t *cs  = idx >= 0 && idx < cfg->subsProviderCount ? cfg->subsProviders[idx].clientSecret : NULL;
        if (!subsAgyRefresh(&auth, cid, cs)) {
            writeLogA("subs agy: token refresh failed (re-login)");
            subsSetState(idx, 0, 0);
            return 0;
        }
        subsAgySaveAuth(authPath, auth.access, auth.refresh, auth.expires);
    }
    unsigned long long tAgy = GetTickCount64();
    if (!*auth.access) { subsSetState(idx, 0, 0); return 0; }
    wchar_t hdrs[4600];
    subsAgyHeaders(hdrs, 4600, auth.access);
    const wchar_t *hosts[2] = { L"cloudcode-pa.googleapis.com", L"daily-cloudcode-pa.sandbox.googleapis.com" };
    wchar_t plan[24] = L"Antigravity";
    // The models calls are the expensive half of the cycle (~1 s of pure server
    // latency each) and they need only a project id, which the auth file already
    // carries. Fire them NOW, with the id we have, so the loadCodeAssist round
    // trip below overlaps them instead of running back to back: a cycle then
    // costs the LONGER of the two rather than their sum. If loadCodeAssist comes
    // back with a DIFFERENT id the speculative result is discarded and fetched
    // again, so a stale id can never show another project's numbers.
    wchar_t usedProject[128]; usedProject[0] = 0;
    HANDLE qth = NULL; AgyQuotaJob qj;
    if (*auth.projectId) {
        memset(&qj, 0, sizeof(qj));
        qj.hosts = hosts; qj.hdrs = hdrs;
        lstrcpynW(qj.project, auth.projectId, 128);
        qj.timeoutMs = cfg->subsTimeoutMs; qj.debug = cfg->debug;
        lstrcpynW(usedProject, auth.projectId, 128);
        qth = CreateThread(NULL, 0, agyQuotaThread, &qj, 0, NULL);
    }
    // Plan label + project discovery. loadCodeAssist runs on EVERY cycle:
    // the stored projectId used to short-circuit it, so the panel kept the
    // fallback label instead of the account's real tier ("Google AI Pro").
    for (int h = 0; h < 2; h++) {
        int st = 0, bl = 0;
        // the length MUST be the full literal: a hard-coded count that was 4
        // short truncated the JSON, so Google answered 400 on every call
        const char *assistBody = "{\"metadata\":{\"ideType\":\"ANTIGRAVITY\",\"platform\":\"PLATFORM_UNSPECIFIED\",\"pluginType\":\"GEMINI\"}}";
        char *resp = subsHttpPost("assist", AGY_UA, hosts[h], 0, L"/v1internal:loadCodeAssist", hdrs,
            assistBody, (int)strlen(assistBody), 0, cfg->subsTimeoutMs, &st, &bl);
        if (cfg->debug) {
            char lb[160];
            sprintf(lb, "[wizbar] subs agy loadCodeAssist host=%d st=%d bl=%d resp=%s", h, st, bl, resp ? "ok" : "NULL");
            writeLogA(lb);
        }
        if (!resp) continue;
        if (st == 200) {
            jsmntok_t *t = NULL;
            int n = subsParseBig(resp, bl, &t);
            if (n > 0 && t[0].type == JSMN_OBJECT) {
                int paid = jobjGet(resp, t, 0, "paidTier");
                if (cfg->debug && paid < 0)
                    writeLogA("[wizbar] subs agy loadCodeAssist: 200 but no paidTier (plan stays generic)");
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
    if (cfg->debug) { char lb[80]; sprintf(lb, "[wizbar] subs agy: loadCodeAssist phase %llu ms", GetTickCount64() - tAgy); writeLogA(lb); }
    static const wchar_t *famNames[2] = { L"Gemini", L"Claude/GPT" };
    double famRf[2] = { -1, -1 };
    long long famReset[2] = { 0, 0 };
    int famHave[2] = { 0, 0 };
    // Collect the speculative fetch that overlapped loadCodeAssist. The thread
    // writes into qj, a local of THIS frame, so it is joined HERE - above every
    // early return below - and its lifetime can never depend on which branch
    // the code takes.
    int needModels = 1;
    if (qth) {
        WaitForSingleObject(qth, INFINITE);
        CloseHandle(qth);
        if (qj.result < 0) { // 401: the token is no good
            writeLogA("subs agy: models 401 (re-login)");
            subsSetState(idx, 0, 0);
            return 0;
        }
        if (lstrcmpW(usedProject, auth.projectId) == 0) {
            // loadCodeAssist confirmed the id we already fetched with
            famRf[0] = qj.famRf[0]; famRf[1] = qj.famRf[1];
            famReset[0] = qj.famReset[0]; famReset[1] = qj.famReset[1];
            famHave[0] = qj.famHave[0]; famHave[1] = qj.famHave[1];
            needModels = 0;
        }
    }
    if (!*auth.projectId) {
        writeLogA("subs agy: no project id");
        subsSetState(idx, 0, 0);
        return 0;
    }
    // THE quota source, matching the harness's /quota exactly (pi-quota ->
    // quota-axi -> pi-quota-inject.mjs): /v1internal:fetchAvailableModels on
    // BOTH endpoints, merged with the daily/sandbox endpoint OVERWRITING
    // production, then per family the first model key in priority order whose
    // (merged) entry carries a quotaInfo. retrieveUserQuotaSummary is what
    // reported gemini as a constant rf=1 untracked pool (the "100% while
    // /quota is correct" bug); the two endpoints carry DIFFERENT quota
    // figures and the sandbox is what actually serves Gemini. A quotaInfo may
    // carry only a resetTime and no remainingFraction (the Claude/GPT pool
    // between resets): the row stays on the board, its fraction renders as an
    // em dash.
    if (needModels) {
        int r = agyFetchModels(hosts, hdrs, auth.projectId, cfg->subsTimeoutMs, cfg->debug,
                               famRf, famReset, famHave);
        if (r < 0) {
            writeLogA("subs agy: models 401 (re-login)");
            subsSetState(idx, 0, 0);
            return 0;
        }
    }
    if (cfg->debug) {
        char lb[96];
        sprintf(lb, "[wizbar] subs agy: models phase %llu ms (overlapped=%d)",
                GetTickCount64() - tAgy, !needModels);
        writeLogA(lb);
    }
    SubsWin wins[2];
    int nw = 0, loRem = -1;
    for (int f = 0; f < 2; f++) {
        if (!famHave[f]) continue;
        memset(&wins[nw], 0, sizeof(SubsWin));
        lstrcpynW(wins[nw].label, famNames[f], 24);
        wins[nw].rem = famRf[f] < 0 ? -1 : (int)(famRf[f] * 100.0 + 0.5);
        wins[nw].pct = wins[nw].rem < 0 ? -1 : 100 - wins[nw].rem;
        wins[nw].used = -1;
        wins[nw].total = -1;
        wins[nw].resetAt = famReset[f];
        if (wins[nw].rem >= 0 && (loRem < 0 || wins[nw].rem < loRem)) loRem = wins[nw].rem;
        nw++;
    }
    if (nw > 0) {
        subsSetWins(idx, wins, nw);
        subsSetPlan(idx, plan);
        if (cfg->debug) {
            char lb[128];
            sprintf(lb, "[wizbar] subs agy: gemini=%d%% claude=%d%% (-1 = not reported)",
                    famRf[0] < 0 ? -1 : (int)(famRf[0] * 100.0 + 0.5),
                    famRf[1] < 0 ? -1 : (int)(famRf[1] * 100.0 + 0.5));
            writeLogA(lb);
        }
        // loRem -1 = both fractions unreported: rows still shown, chip em dash
        subsSetState(idx, loRem, 1);
        return 1;
    }
    subsSetState(idx, 0, 0);
    return 0;
}

// ---------------------------------------------------------------- generic ----
// A JSONPath subset good enough for a quota endpoint: "$.a.b[0].c". It walks the
// token tree directly - no string building, no allocation. Returns the token
// index or -1. Deliberately narrow: an unsupported path just yields nothing and
// the window is skipped, which is a visible config error rather than a crash.
static int subsJsonPath(const char *js, const jsmntok_t *t, int root, const char *path) {
    if (!path || path[0] != '$') return -1;
    int cur = root;
    const char *p = path + 1;
    while (*p && cur >= 0) {
        if (*p == '.') {
            p++;
            if (!*p) break;
            const char *ks = p;
            while (*p && *p != '.' && *p != '[') p++;
            int kl = (int)(p - ks);
            if (t[cur].type != JSMN_OBJECT) return -1;
            int n = t[cur].size, k = cur + 1, hit = -1;
            for (int i = 0; i < n; i++, k += 1 + jtokSpan(t, k + 1)) {
                if (t[k].type == JSMN_STRING && (t[k].end - t[k].start) == kl
                    && strncmp(js + t[k].start, ks, (size_t)kl) == 0) { hit = k + 1; break; }
            }
            // a parent with no children (or no matching one) yields nothing -
            // staying on the parent would feed an unrelated token to the
            // require guard and to every window path
            if (hit < 0) return -1;
            cur = hit;
        } else if (*p == '[') {
            p++;
            int idx = 0, any = 0;
            while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; any = 1; }
            if (*p == ']') p++;
            if (!any || t[cur].type != JSMN_ARRAY) return -1;
            // the array's own tokens run from cur+1 to cur+jtokSpan(t,cur); an
            // index past the last element is a config typo, not a licence to
            // keep walking into whatever token follows in document order
            int k = cur + 1, end = cur + jtokSpan(t, cur);
            for (int i = 0; i < idx; i++) {
                if (k >= end) return -1;
                k += jtokSpan(t, k);
            }
            if (k >= end) return -1;
            cur = k;
        } else break;
    }
    return cur;
}

// the token at a JSONPath, as a double. Non-numbers (a stringified "1234")
// are accepted, so an endpoint that quotes its counts still works.
static double subsPathDouble(const char *js, const jsmntok_t *t, int root, const char *path, double dflt) {
    int i = subsJsonPath(js, t, root, path);
    if (i < 0) return dflt;
    char buf[64];
    int n = t[i].end - t[i].start;
    if (n <= 0 || n >= (int)sizeof(buf)) return dflt;
    memcpy(buf, js + t[i].start, (size_t)n);
    buf[n] = 0;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return dflt;
    return v;
}

// resolve one auth entry and append its header line to the caller's blob,
// growing the blob as needed. The secret comes from a JSON file, an env var or
// the config itself and NONE of the three has a length the code may assume - a
// JWT is already past a kilobyte - so the line is sized from the secret
// instead of a fixed buffer that would truncate it mid-signature. The secret
// is read at fetch time and never written anywhere - the bar has no state
// that outlives the request.
static int subsGenAuthLine(const GenAuth *ga, wchar_t **buf, int *cap, int *used) {
    char *tmp = NULL; int tmpLen = 0;   // the secret as UTF-8 bytes
    wchar_t *val = NULL;                // ... and as wide chars
    if (ga->path && *ga->path) {
        wchar_t path[MAX_PATH];
        subsPathExpand(ga->path, path, MAX_PATH); // "~/..." like every other read
        int txtLen = 0;
        char *txt = subsReadFileUtf8(path, &txtLen);
        if (txt) {
            // the file is small (an auth.json); parse it in place
            jsmntok_t *tk = NULL;
            int n = subsParseBig(txt, (int)strlen(txt), &tk);
            int v = -1;
            if (n > 0) {
                // a key that starts with $ is a json path (a secret nested in
                // the file, "$.auth.token"); anything else is one flat key
                if (ga->key[0] == '$') v = subsJsonPath(txt, tk, 0, ga->key);
                else v = jobjGet(txt, tk, 0, ga->key);
            }
            if (v >= 0) {
                char *raw = subsJstrRawTok(txt, tk, v); // unescaped, or NULL
                if (raw) {
                    tmp = raw;
                    tmpLen = (int)strlen(raw);
                } else {
                    // a non-string token still has to be readable as a value
                    tmpLen = tk[v].end - tk[v].start;
                    if (tmpLen > 0) {
                        tmp = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)tmpLen + 1);
                        if (tmp) { memcpy(tmp, txt + tk[v].start, (size_t)tmpLen); tmp[tmpLen] = 0; }
                    }
                }
            }
            HeapFree(GetProcessHeap(), 0, tk);
            HeapFree(GetProcessHeap(), 0, txt);
        }
    } else if (ga->env && *ga->env) {
        DWORD en = GetEnvironmentVariableW(ga->env, NULL, 0); // required size, or 0
        if (en) {
            val = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)en * sizeof(wchar_t));
            if (val && !GetEnvironmentVariableW(ga->env, val, en)) {
                HeapFree(GetProcessHeap(), 0, val);
                val = NULL;
            }
        }
    } else if (ga->literal && *ga->literal) {
        int ln = lstrlenW(ga->literal);
        val = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((size_t)ln + 1) * sizeof(wchar_t));
        if (val) lstrcpynW(val, ga->literal, ln + 1);
    }
    if (tmp && !val && tmpLen > 0) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, tmp, tmpLen, NULL, 0);
        if (wn > 0) {
            val = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((size_t)wn + 1) * sizeof(wchar_t));
            if (val) { MultiByteToWideChar(CP_UTF8, 0, tmp, tmpLen, val, wn); val[wn] = 0; }
        }
    }
    HeapFree(GetProcessHeap(), 0, tmp);
    int ok = 0;
    if (val && *val) {
        // the header line needs room for the fixed parts, the whole secret,
        // the CRLF AND the terminator: swprintf returns -1 and leaves the line
        // half-written if it does not all fit
        int need = *used + lstrlenW(ga->header) + lstrlenW(ga->prefix) + lstrlenW(val) + 5;
        if (need >= *cap) {
            int nc = *cap;
            while (nc <= need) nc *= 2;
            wchar_t *nb = (wchar_t *)HeapReAlloc(GetProcessHeap(), 0, *buf, (size_t)nc * sizeof(wchar_t));
            if (nb) { *buf = nb; *cap = nc; }
            else { HeapFree(GetProcessHeap(), 0, val); return 0; }
        }
        int n = swprintf(*buf + *used, *cap - *used, L"%ls: %ls%ls\r\n",
                         ga->header, ga->prefix, val);
        if (n > 0) *used += n; // never let a negative return poison the length
        ok = 1;
    }
    HeapFree(GetProcessHeap(), 0, val);
    return ok;
}

// case-insensitive prefix compare (a hand-edited config writes HTTP://; URL
// schemes are ASCII by definition, so folding A-Z is exact)
static int subsPreI(const wchar_t *s, const wchar_t *p) {
    while (*p) {
        wchar_t a = *s, b = *p;
        if (a >= L'A' && a <= L'Z') a += 32;
        if (b >= L'A' && b <= L'Z') b += 32;
        if (a != b) return 0;
        s++;
        p++;
    }
    return *s != 0; // the remainder must exist: "https" is not "https://"
}

// one quota url into the pieces WinHTTP wants: the server name, the port
// and the request path (query included). The SCHEME decides TLS, and a config
// "insecure" flag may only AUTHORIZE the cleartext scheme - it must never turn
// an https url into one. Returns 0 (and logs why) for anything else.
static int subsCrackUrl(const wchar_t *url, wchar_t *host, int cchHost, int *port,
                        wchar_t *path, int cchPath, int *tls) {
    host[0] = 0; path[0] = 0; *port = 0; *tls = 0;
    if (!url || !*url) return 0;
    const wchar_t *rest;
    if (subsPreI(url, L"http://")) { *tls = 0; rest = url + 7; }
    else if (subsPreI(url, L"https://")) { *tls = 1; rest = url + 8; }
    else { writeLogA("subs gen: unsupported url scheme"); return 0; }
    // the host runs to the first '/', ':' (an explicit port) or end of string
    const wchar_t *p = rest;
    while (*p && *p != L'/' && *p != L':') p++;
    int hl = (int)(p - rest);
    if (hl <= 0 || hl >= cchHost) { writeLogA("subs gen: url host is empty or too long"); return 0; }
    memcpy(host, rest, (size_t)hl * sizeof(wchar_t));
    host[hl] = 0;
    // an explicit :port (a bare colon with no digits is a config typo)
    if (*p == L':') {
        p++;
        int n = 0, digits = 0;
        while (*p >= L'0' && *p <= L'9') {
            n = n * 10 + (*p - L'0');
            p++;
            digits++;
            if (n > 65535) { writeLogA("subs gen: url port out of range"); return 0; }
        }
        if (!digits || n <= 0) { writeLogA("subs gen: url port is not a number"); return 0; }
        *port = n;
    }
    // the path keeps its query string; "http://host" on its own means "/"
    if (*p == L'/') lstrcpynW(path, p, cchPath);
    else lstrcpynW(path, L"/", cchPath);
    return 1;
}

// Fetch a provider declared entirely in config: one REST call, then the quota
// windows lifted out of the response by JSON path. This is the escape hatch for
// any subscription service the bar has no built-in adapter for.
static int subsFetchGeneric(const Config *cfg, int idx) {
    if (idx < 0 || idx >= MAX_SUBS) return 0; // every caller clamps to MAX_SUBS
    const SubsProvider *sp = &cfg->subsProviders[idx];
    if (!sp->url || !*sp->url) { subsSetState(idx, 0, 0); return 0; }

    // host + port + path, and TLS only from the scheme
    wchar_t host[256], path[1024]; int port = 0, tls = 0;
    if (!subsCrackUrl(sp->url, host, 256, &port, path, 1024, &tls)) {
        subsSetState(idx, 0, 0);
        return 0;
    }
    // the flag authorizes the cleartext scheme; without it an http url is
    // refused rather than sent with the token in the clear
    int insecure = !tls;
    if (!tls && !sp->insecure) {
        writeLogA("subs gen: http url needs insecure: true");
        subsSetState(idx, 0, 0);
        return 0;
    }

    // resolve the auth entries into the header blob
    wchar_t *hdrs = NULL;
    int hlen = 512, used = 0;
    hdrs = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)hlen * sizeof(wchar_t));
    if (!hdrs) { subsSetState(idx, 0, 0); return 0; }
    hdrs[0] = 0;
    for (int i = 0; i < sp->nAuth; i++)
        subsGenAuthLine(&sp->auth[i], &hdrs, &hlen, &used);
    // static headers from the config, appended after the resolved auth
    if (sp->headerBlob && *sp->headerBlob) {
        int need = used + lstrlenW(sp->headerBlob) + 1;
        if (need >= hlen) {
            while (hlen <= need) hlen *= 2;
            wchar_t *nb = (wchar_t *)HeapReAlloc(GetProcessHeap(), 0, hdrs, (size_t)hlen * sizeof(wchar_t));
            if (!nb) { HeapFree(GetProcessHeap(), 0, hdrs); subsSetState(idx, 0, 0); return 0; }
            hdrs = nb;
        }
        used += swprintf(hdrs + used, hlen - used, L"%ls", sp->headerBlob);
    }

    unsigned long long t0 = GetTickCount64();
    int st = 0, bl = 0; char *resp = NULL;
    int isPost = (sp->method && lstrcmpiW(sp->method, L"POST") == 0);
    if (isPost) {
        char *body = NULL; int bodyLen = 0;
        if (sp->reqBody && *sp->reqBody) {
            int wl = WideCharToMultiByte(CP_UTF8, 0, sp->reqBody, -1, NULL, 0, NULL, NULL);
            body = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)wl);
            if (!body) { HeapFree(GetProcessHeap(), 0, hdrs); subsSetState(idx, 0, 0); return 0; }
            // the conversion reports the terminator: Content-Length must be the
            // bytes actually sent, or the request carries a trailing NUL
            bodyLen = WideCharToMultiByte(CP_UTF8, 0, sp->reqBody, -1, body, wl, NULL, NULL);
            if (bodyLen > 0) bodyLen--;
        }
        // a generic endpoint is a third-party quota API: it gets the bar's own
        // identity, never the Antigravity UA the Google calls use
        resp = subsHttpPost("gen", L"chocobar", host, port, path, hdrs, body, bodyLen, insecure, cfg->subsTimeoutMs, &st, &bl);
        if (body) HeapFree(GetProcessHeap(), 0, body);
    } else {
        resp = subsHttpGet("gen", L"chocobar", host, port, path, hdrs, insecure, cfg->subsTimeoutMs, &st, &bl);
    }
    HeapFree(GetProcessHeap(), 0, hdrs);
    if (cfg->debug) {
        char lb[160];
        // the host is user config, so the wide string can be far longer than the
        // line: format it bounded - a precision AND the buffer bound mean a long
        // host truncates the log line instead of tripping the analyzer
        snprintf(lb, sizeof(lb), "[wizbar] subs gen %.92ls: st=%d bl=%d %llu ms", host, st, bl, GetTickCount64() - t0);
        writeLogA(lb);
    }
    if (!resp) { subsSetState(idx, 0, 0); return 0; }
    if (st == 401 || st == 403) {
        writeLogA("subs gen: 401/403 (auth rejected)");
        HeapFree(GetProcessHeap(), 0, resp);
        subsSetState(idx, 0, 0);
        return 0;
    }
    if (st < 200 || st >= 300) {
        HeapFree(GetProcessHeap(), 0, resp);
        subsSetState(idx, 0, 0);
        return 0;
    }

    jsmntok_t *tk = NULL;
    int n = subsParseBig(resp, bl, &tk);
    if (n <= 0) {
        HeapFree(GetProcessHeap(), 0, tk);
        HeapFree(GetProcessHeap(), 0, resp);
        subsSetState(idx, 0, 0);
        return 0;
    }
    // a 200 can still be an error envelope (the zai gateway does exactly that):
    // "require" names a path that must be present for the body to count
    if (sp->requirePath[0] && subsJsonPath(resp, tk, 0, sp->requirePath) < 0) {
        writeLogA("subs gen: response missing the required path");
        HeapFree(GetProcessHeap(), 0, tk);
        HeapFree(GetProcessHeap(), 0, resp);
        subsSetState(idx, 0, 0);
        return 0;
    }

    SubsWin wins[MAX_GEN_WIN];
    int nw = 0, loRem = -1;
    for (int i = 0; i < sp->nWin && nw < MAX_GEN_WIN; i++) {
        const GenWin *gw = &sp->win[i];
        double usedV = -1, remV = -1, totV = -1;
        if (gw->used[0])      usedV = subsPathDouble(resp, tk, 0, gw->used, -1);
        if (gw->remaining[0]) remV  = subsPathDouble(resp, tk, 0, gw->remaining, -1);
        if (gw->total[0])     totV  = subsPathDouble(resp, tk, 0, gw->total, -1);
        // a window with no numbers at all is skipped, not shown as an em dash
        if (usedV < 0 && remV < 0 && totV < 0) continue;
        // derive whichever of the three is missing from the other two
        if (totV >= 0 && usedV >= 0 && remV < 0) remV = totV - usedV;
        if (totV >= 0 && remV >= 0 && usedV < 0) usedV = totV - remV;
        if (usedV >= 0 && remV >= 0 && totV < 0) totV = usedV + remV;
        memset(&wins[nw], 0, sizeof(SubsWin));
        lstrcpynW(wins[nw].label, gw->label, 24);
        wins[nw].used  = usedV < 0 ? -1 : (long long)usedV;
        wins[nw].total = totV < 0 ? -1 : (long long)totV;
        wins[nw].rem = (remV < 0 || totV <= 0) ? -1 : (int)(remV * 100.0 / totV + 0.5);
        if (wins[nw].rem < 0) wins[nw].rem = -1;
        else if (wins[nw].rem > 100) wins[nw].rem = 100;
        wins[nw].pct = wins[nw].rem < 0 ? -1 : 100 - wins[nw].rem;
        if (gw->reset[0]) {
            int r = subsJsonPath(resp, tk, 0, gw->reset);
            if (r >= 0) {
                char *rt = subsJstrRawTok(resp, tk, r);
                if (rt) {
                    wins[nw].resetAt = subsIsoToMs(rt, (int)strlen(rt));
                    HeapFree(GetProcessHeap(), 0, rt);
                }
            }
        }
        if (wins[nw].rem >= 0 && (loRem < 0 || wins[nw].rem < loRem)) loRem = wins[nw].rem;
        nw++;
    }
    HeapFree(GetProcessHeap(), 0, tk);
    HeapFree(GetProcessHeap(), 0, resp);

    if (nw > 0) {
        subsSetWins(idx, wins, nw);
        // the panel head is the config label (a plan name would be one more
        // path to configure); "generic" only when even that is missing
        subsSetPlan(idx, (sp->label && *sp->label) ? sp->label : L"generic");
        subsSetState(idx, loRem, 1);
        return 1;
    }
    subsSetState(idx, 0, 0);
    return 0;
}

static LONG g_subsKick = 0; // board refresh button wakes the cycle early
static volatile unsigned long long g_subsFetchedTick = 0; // cycle end (GetTickCount64)
// wall clock of the cycle end. The board footer renders THIS directly: mixing a
// truncated GetTickCount64 age with the wall clock made the footer's seconds
// field oscillate (41 -> 42 -> 41) on every repaint.
static volatile long long g_subsFetchedEpoch = 0;

// Local wall clock in the frame dashFmtTime renders (it reinterprets its input
// as UTC), so the board footer shows the captain's local time. subsNowMs() is a
// TRUE UTC epoch and would print UTC - 8 hours off in HKT.
static long long subsLocalStampMs(void) {
    SYSTEMTIME now; GetLocalTime(&now);
    FILETIME ft;
    SystemTimeToFileTime(&now, &ft);
    return ((((long long)ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000;
}
void subsRefetchNow(void) { InterlockedExchange(&g_subsKick, 1); }
// seconds since the last completed fetch cycle (kept for callers that want an age)
unsigned subsFetchedAgoSec(void) {
    unsigned long long t = g_subsFetchedTick;
    if (!t) return 0xFFFFFFFFu;
    return (unsigned)((GetTickCount64() - t) / 1000ull);
}
// wall-clock epoch ms of the last completed cycle, 0 = never fetched
long long subsFetchedEpochMs(void) { return g_subsFetchedEpoch; }

// one provider's fetch, dispatched by type. Runs on its own thread when
// more than one provider is enabled (see the cycle below): the cycle time is
// then the slowest provider instead of the sum of all of them.
static void subsProviderFetch(const Config *cfg, int i) {
    switch (cfg->subsProviders[i].type) {
        case 0:  subsFetchChatgpt(cfg, i);    break;
        case 1:  subsFetchZai(cfg, i);        break;
        case 3:  subsFetchGeneric(cfg, i);    break;
        default: subsFetchAntigravity(cfg, i); break;
    }
}

typedef struct { int idx; const Config *cfg; } SubsJob;
static DWORD WINAPI subsProviderThread(LPVOID lp) {
    SubsJob *j = (SubsJob *)lp;
    subsProviderFetch(j->cfg, j->idx);
    return 0;
}

static DWORD WINAPI subsThreadProc(LPVOID lp) {
    (void)lp;
    for (;;) {
        // ONE pin per cycle, taken before the master switch and released after
        // the interval is read: every config access in this loop goes through
        // it. Every fetch holds a SubsProvider* for the whole (multi-second)
        // request, so a reload that lands mid-fetch retires that generation
        // instead of freeing it, and the whole cycle reads one consistent
        // snapshot.
        const Config *view = cfgPin();
        if (view->subsEnabled) { // master switch off: no polls, no requests
            int anyEnabled = 0;
            unsigned long long tCycle = GetTickCount64();
            int n = view->subsProviderCount;
            if (n > MAX_SUBS) n = MAX_SUBS;
            // Fan the enabled providers out over their own threads. Every fetch
            // is independent (each writes only its own slot through the locked
            // setters), so the wall time of a cycle drops from the SUM of the
            // providers to the slowest one - 5.9s -> 2.6s for the captain's
            // three providers at this step alone. The whole cycle measured
            // 1.9s once the antigravity model calls and loadCodeAssist
            // overlapped those fetches too (the perf commit, 3c533f2).
            HANDLE th[MAX_SUBS]; SubsJob jobs[MAX_SUBS]; int nth = 0;
            for (int i = 0; i < n; i++) {
                if (!view->subsProviders[i].enabled) continue;
                anyEnabled = 1;
                if (view->debug) {
                    char lb[64];
                    sprintf(lb, "[wizbar] subs cycle: provider %d type %d enter", i, view->subsProviders[i].type);
                    writeLogA(lb);
                }
                jobs[nth].idx = i;
                jobs[nth].cfg = view;
                HANDLE h = CreateThread(NULL, 0, subsProviderThread, &jobs[nth], 0, NULL);
                if (h) th[nth++] = h;
                else subsProviderFetch(view, i); // could not spawn: do it inline
            }
            if (nth > 1) {
                WaitForMultipleObjects((DWORD)nth, th, TRUE, INFINITE);
                for (int i = 0; i < nth; i++) CloseHandle(th[i]);
            } else if (nth == 1) {
                // a lone provider already ran to completion on its thread
                WaitForSingleObject(th[0], INFINITE);
                CloseHandle(th[0]);
            }
            if (!anyEnabled) {
                for (int i = 0; i < n; i++) subsSetState(i, -1, 0); // no-data marker
            }
            g_subsFetchedTick = GetTickCount64();
            if (view->debug) {
                char lb[64];
                sprintf(lb, "[wizbar] subs cycle: total %llu ms", GetTickCount64() - tCycle);
                writeLogA(lb);
            }
            g_subsFetchedEpoch = subsLocalStampMs();
            if (view->debug) { // one line per provider: what the board will show
                for (int i = 0; i < n; i++) {
                    if (!view->subsProviders[i].enabled) continue;
                    SubsWin w[MAX_GEN_WIN];
                    int wn = subsProvWins(i, w, MAX_GEN_WIN);
                    int stale = wn < 0; if (wn < 0) wn = -wn;
                    wchar_t plan[24]; subsProvPlan(i, plan, 24);
                    if (!plan[0]) lstrcpynW(plan, L"\u2014", 24);
                    char line[400];
                    wchar_t lab[64]; subsViewLabel(view, i, lab, 64);
                    int off = sprintf(line, "[wizbar] subs[%d] %ls plan=%ls wins=%d stale=%d ::", i,
                                      lab, plan, wn, stale);
                    for (int k = 0; k < wn && off < 360; k++)
                        off += snprintf(line + off, sizeof(line) - (size_t)off,
                                        " [%ls rem=%d pct=%d used=%d total=%d]",
                                        w[k].label, w[k].rem, w[k].pct, w[k].used, w[k].total);
                    writeLogA(line);
                }
            }
        }
        // sleep the interval, but a kick (refresh button) breaks out early
        int ivl = view->subsIntervalMin > 0 ? view->subsIntervalMin * 60000 : 120000;
        cfgUnpin(); // the sleeps below read no config: let a retired generation go
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
    // the lock MUST exist before the worker's first subsSet* call - the fetch
    // thread can enter it within microseconds of starting (C0000005 if not)
    if (!g_subsLockInit) {
        InitializeCriticalSection(&g_subsLock);
        g_subsLockInit = 1;
    }
    HANDLE h = CreateThread(NULL, 0, subsThreadProc, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

// chip state readers (UI thread): lowest remaining across providers with data.
//
// "stale" here means the chip has NOTHING to show, not that some provider
// failed its last poll. A provider is marked stale when a fetch fails, but its
// last known windows are still on the board and still worth reporting - so a
// single timeout used to blank the whole chip to "stale" while the dashboard
// (which asks per provider) happily showed fresh, correct numbers. The chip is
// stale only when NO provider has a usable remaining value.
static int subsChipRem(void) {
    if (!g_subsLockInit) return -1;
    // scope to the real providers: unused slots are zero-init globals (rem=0,
    // stale=0) that are never reset, so looping MAX_SUBS would read them as a
    // live provider at 0% and drown every real provider's value. A declared
    // but DISABLED slot is the same shape: subsThreadProc skips it, so its
    // rem=0 would read as a live provider sitting at 0% and hold the chip in
    // the "not stale" branch forever.
    int pn = g_cfg.subsProviderCount; if (pn > MAX_SUBS) pn = MAX_SUBS;
    EnterCriticalSection(&g_subsLock);
    int r = -1;
    for (int i = 0; i < pn; i++) {
        if (!subsProvEnabled(i)) continue;
        if (g_subsProvRem[i] >= 0 && (r < 0 || g_subsProvRem[i] < r)) r = g_subsProvRem[i];
    }
    LeaveCriticalSection(&g_subsLock);
    return r; // -1 = no provider has a number
}

static int subsChipStale(void) {
    if (!g_subsLockInit) return 0;
    // scope to the real providers: an unused slot zero-inits to stale=0, which
    // would otherwise count as "live" and make stale unreachable - the same
    // holds for a declared-but-disabled slot, which never fetches either
    int pn = g_cfg.subsProviderCount; if (pn > MAX_SUBS) pn = MAX_SUBS;
    EnterCriticalSection(&g_subsLock);
    // stale only if every provider that HAS a value failed its last fetch, or
    // nothing has ever succeeded. Any live provider clears it.
    int anyValue = 0, anyLive = 0;
    for (int i = 0; i < pn; i++) {
        if (!subsProvEnabled(i) || g_subsProvRem[i] < 0) continue;
        anyValue = 1;
        if (!g_subsProvStale[i]) anyLive = 1;
    }
    LeaveCriticalSection(&g_subsLock);
    return anyValue && !anyLive;
}

// ---- subs board (dashboard) readers ----------------------------------------
// provider display label: config label, else the type default (Electron does
// `p.label || 'ChatGPT'` / 'Z.ai'). The view variant reads a PINNED config
// generation: a fetch thread must report the label of the generation it
// actually walked, never the live one a concurrent reload may have reordered.
static void subsViewLabel(const Config *v, int i, wchar_t *out, int cb) {
    const wchar_t *l = i >= 0 && i < v->subsProviderCount && v->subsProviders[i].label
                           ? v->subsProviders[i].label : NULL;
    int it = i >= 0 && i < v->subsProviderCount ? v->subsProviders[i].type : 0;
    if (it == 2) l = L"Antigravity";
    else if (!l || !*l) l = it == 1 ? L"Z.ai" : (it == 3 ? L"generic" : L"ChatGPT");
    lstrcpynW(out, l, cb);
}
static void subsProvLabel(int i, wchar_t *out, int cb) { subsViewLabel(&g_cfg, i, out, cb); }
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
