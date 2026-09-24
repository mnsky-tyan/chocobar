// live token usage: scan the JSONL session stores directly
//
// WHY: the bar used to read ONLY the Electron app's ~/.wizbar/token-cache.json.
// With the Electron bar gone nothing rewrites that file, so every number
// froze at the moment it died (the captain's "token dashboard is stale, still
// 0" - the cache's last record was 2h old and local midnight had rolled over).
//
// This file reads whatever session stores the user declares in
// tokens.sources[] - any harness that logs per-message usage as JSONL, with the
// six key NAMES taken from each source's `fields` (so a harness that spells them
// differently needs no code change) - and merges the NEW records into the
// aggregate the Electron cache already provides:
//
//   * the Electron cache is still the history seed (zcode/opencode/mimo live
//     in SQLite and stay as the cache last wrote them),
//   * only records with ts > the cache's newest ts are counted from the live
//     scan, so nothing is double-counted,
//   * a file is skipped entirely while its mtime is older than the cache's,
//     and a per-file byte cursor means a warm rescan reads only appends.
//
// Every record lands in the dashboard's own per-day buckets (aggRecord), so
// the cards, heatmap and tables are one set of numbers - this file only
// decides WHICH records reach them and how many bytes to skip next time.
//
// The cursor file is ~/.wizbar/token-cursors.json (best-effort: losing it only
// costs a re-read, never a wrong number, because the ts filter still applies).

// helpers that live in the later parts of the assembled translation unit
static void aggRecord(const char *app, int alen, long long ts, long long in, long long out,
                      long long cr, long long cw, const char *model, int mlen, const long long *bnd);
static long long parseLL(const char *p, const char *end);
static long long dashMidnightMs(const FILETIME *localMidnight, int daysBack);
static char *readFileUtf8(const wchar_t *path, DWORD *outLen);
static void stripLineComments(char *s);
static int jtokSpan(const jsmntok_t *t, int i);
static void writeLogA(const char *s);

// how many tokens the live scan folded in (the "+N" log line). Every number
// the bar shows is derived in p_ui.c from the shared per-day buckets, so this
// scan keeps no totals of its own to drift out of step with them.
long long g_tokAllLive = 0;
// scan diagnostics (one log line per rescan while general.debug is on)
int g_tokDbgFiles = 0, g_tokDbgHits = 0, g_tokDbgRead = 0, g_tokDbgStart = 0;

#define TOK_MAX_FILES 4096
typedef struct { char path[520]; long long size; long long mtimeMs; } TokCursor;
static TokCursor g_tokCursor[TOK_MAX_FILES];
static int g_tokCursorN = 0;
static int g_tokCursorDirty = 0;
static long long tokJll(const char *js, const jsmntok_t *t, int parent, const char *key, long long def);

static wchar_t g_tokCursorPath[MAX_PATH]; // ~/.wizbar/token-cursors.json
static long long g_cacheMaxTs = 0;     // newest ts the Electron cache holds
static long long g_cacheMtimeMs = 0;   // when that cache was last written

// ------------------------------------------------------------ small utils ----
// wide path -> utf8 (for the cursor file, which is plain JSON)
static int tokWideToUtf8(const wchar_t *w, char *out, int cb) {
    if (!w || !*w) { if (cb > 0) out[0] = 0; return 0; }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cb, NULL, NULL);
    if (n > 0) return n - 1;
    if (cb > 0) out[0] = 0;
    return 0;
}

// one record's fields, parsed out of a JSONL line
typedef struct { long long ts; const char *app; int appLen; long long in, out, cr, cw; const char *model; int modelLen; } TokRec;

// the six field names a source uses, pre-quoted for the substring search. Built
// once per scan (not per line) so the hot path stays a memcmp.
typedef struct { char k[6][32]; int len[6]; } TokKeys;
static void tokKeysBuild(const TokSource *s, TokKeys *tk) {
    static const char *dflt[6] = { "input", "output", "cacheRead", "cacheWrite", "timestamp", "model" };
    const char *src[6];
    src[0] = s->kIn; src[1] = s->kOut; src[2] = s->kCr;
    src[3] = s->kCw;  src[4] = s->kTs;  src[5] = s->kModel;
    for (int i = 0; i < 6; i++) {
        const char *v = (src[i] && *src[i]) ? src[i] : dflt[i];
        tk->len[i] = sprintf(tk->k[i], "\"%s\":", v);
    }
}

// pi: {"type":"message","message":{"role":"assistant","model":"...","usage":{...},"timestamp":123}}
// zai: {"type":"assistant","usage":{...},"model":"...","timestamp":123} (flat)
// Both are found the same way: the keys are searched for anywhere in the line,
// so a harness that nests them differently still parses. The key NAMES come
// from the source's tokens.sources[].fields, so only the shape has to match.
static int tokParseLine(const char *ln, int len, const TokKeys *tk, TokRec *out) {
    out->ts = 0; out->app = NULL; out->appLen = 0;
    out->in = out->out = out->cr = out->cw = 0; out->model = NULL; out->modelLen = 0;
    // cheap pre-filter: no input field = nothing to count
    const char *u = NULL;
    for (int i = 0; i + tk->len[0] <= len; i++) {
        if (memcmp(ln + i, tk->k[0], tk->len[0]) == 0) { u = ln + i; break; }
    }
    if (!u) return 0;
    const char *end = ln + len;
    // timestamp: epoch-ms integer on the MESSAGE (verified against 1402 real
    // records). The line-level top-level "timestamp" is an ISO STRING and comes
    // first, so skip string-valued keys and take the first numeric one.
    const char *tp = NULL;
    for (int i = 0; i + tk->len[4] <= len; ) {
        if (memcmp(ln + i, tk->k[4], tk->len[4]) == 0) {
            const char *v = ln + i + tk->len[4];
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            if (v < end && *v == '"') { i += tk->len[4]; continue; } // ISO string: not it
            tp = v;
            break;
        }
        i++;
    }
    if (!tp) return 0;
    out->ts = parseLL(tp, end);
    if (out->ts <= 0) return 0;
    // model: the marker stops at the colon, so the value's opening quote
    // has to be stepped over before the closing one is scanned for
    const char *mp = NULL;
    for (int i = 0; i + tk->len[5] <= len; i++) if (memcmp(ln + i, tk->k[5], tk->len[5]) == 0) { mp = ln + i + tk->len[5]; break; }
    if (mp) {
        while (mp < end && (*mp == ' ' || *mp == '\t')) mp++;
        if (mp < end && *mp == '"') {
            mp++;
            const char *me = mp;
            while (me < end && *me != '"' && me - mp < 48) me++;
            if (me > mp) { out->model = mp; out->modelLen = (int)(me - mp); }
        }
    }
    // usage numbers: cacheRead/cacheWrite are breakdown columns, input/output
    // are raw (the TOKEN CONVENTION in AGENTS.md)
    long long *dst[4] = { &out->in, &out->out, &out->cr, &out->cw };
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i + tk->len[k] <= len; i++) {
            if (memcmp(ln + i, tk->k[k], tk->len[k]) == 0) { *dst[k] = parseLL(ln + i + tk->len[k], end); break; }
        }
    }
    return 1;
}

// read [from, EOF) of a session file and hand every complete line's record to
// aggRecord. Returns the new cursor (EOF) or -1 on a read error.
static long long tokScanFile(const wchar_t *path, long long from, const char *appName,
                             const long long *bnd, const TokKeys *tk) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz; if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return -1; }
    long long size = (long long)sz.QuadPart;
    if (from > size) from = size; // truncated: restart from scratch
    if (from < 0) from = 0;
    long long toRead = size - from;
    if (toRead <= 0) { CloseHandle(h); return size; }
    // a tail read can start mid-line: skip to the first newline
    long long start = from;
    if (from > 0) {
        char probe[1];
        LARGE_INTEGER li; li.QuadPart = from - 1;
        SetFilePointerEx(h, li, NULL, FILE_BEGIN);
        DWORD got = 0;
        if (ReadFile(h, probe, 1, &got, NULL) && got == 1) {
            if (probe[0] != '\n') { // walk forward to the next newline
                char chunk[512];
                long long p = from;
                while (p < size) {
                    li.QuadPart = p; SetFilePointerEx(h, li, NULL, FILE_BEGIN);
                    DWORD g2 = 0;
                    if (!ReadFile(h, chunk, (DWORD)((size - p < 512) ? (size - p) : 512), &g2, NULL) || !g2) break;
                    int found = -1;
                    for (DWORD i = 0; i < g2; i++) if (chunk[i] == '\n') { found = (int)i; break; }
                    if (found >= 0) { p += found + 1; break; }
                    p += g2;
                }
                start = p;
            }
        }
    }
    toRead = size - start;
    if (toRead <= 0) { CloseHandle(h); return size; }
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)toRead + 1);
    if (!buf) { CloseHandle(h); return -1; }
    LARGE_INTEGER li; li.QuadPart = start;
    SetFilePointerEx(h, li, NULL, FILE_BEGIN);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)toRead, &got, NULL);
    CloseHandle(h);
    if (!ok || got != (DWORD)toRead) { HeapFree(GetProcessHeap(), 0, buf); return -1; }
    buf[got] = 0;
    // split on newlines; a trailing partial line is left for the next rescan
    // (its bytes stay uncounted because the cursor is not advanced past it)
    char *p = buf, *e = buf + got;
    long long consumed = 0;
    while (p < e) {
        char *nl = (char *)memchr(p, '\n', (size_t)(e - p));
        if (!nl) break; // partial tail: do not advance the cursor past it
        int len = (int)(nl - p);
        if (len > 0) {
            TokRec r;
            if (tokParseLine(p, len, tk, &r) && r.ts > g_cacheMaxTs) {
                long long fld[4] = { r.in, r.out, r.cr, r.cw };
                long long sum = fld[0] + fld[1] + fld[2] + fld[3];
                aggRecord(appName, (int)strlen(appName), r.ts, fld[0], fld[1], fld[2], fld[3],
                          r.model, r.modelLen, bnd);
                g_tokAllLive += sum;
            }
        }
        consumed += len + 1;
        p = nl + 1;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return start + consumed;
}

// ---------------------------------------------------------- cursor file ----
static void tokCursorLoad(void) {
    g_tokCursorN = 0;
    DWORD len = 0;
    char *raw = readFileUtf8(g_tokCursorPath, &len);
    if (g_cfg.debug) {
        char lb[200];
        sprintf(lb, "[wizbar] tokCursorLoad: file=%s len=%lu", raw ? "ok" : "missing", (unsigned long)len);
        writeLogA(lb);
    }
    if (!raw) return;
    stripLineComments(raw);
    jsmn_parser p; jsmn_init(&p);
    int nt = jsmn_parse(&p, raw, (size_t)len, NULL, 0);
    if (nt <= 0) { HeapFree(GetProcessHeap(), 0, raw); return; }
    jsmntok_t *t = (jsmntok_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(jsmntok_t) * (nt + 1));
    if (!t) { HeapFree(GetProcessHeap(), 0, raw); return; }
    jsmn_init(&p);
    jsmn_parse(&p, raw, (size_t)len, t, (unsigned int)nt);
    if (nt > 0 && t[0].type == JSMN_OBJECT) {
        int cnt = t[0].size; // object children are key/value PAIRS
        int k = 1;
        for (int i = 0; i < cnt && g_tokCursorN < TOK_MAX_FILES; i++) {
            if (t[k].type == JSMN_STRING && t[k + 1].type == JSMN_OBJECT) {
                char pathA[520];
                int pl = t[k].end - t[k].start;
                if (pl > 0 && pl < 519) {
                    memcpy(pathA, raw + t[k].start, pl); pathA[pl] = 0;
                    TokCursor *c = &g_tokCursor[g_tokCursorN++];
                    memset(c, 0, sizeof(*c));
                    lstrcpynA(c->path, pathA, 520);
                    c->size = tokJll(raw, t, k + 1, "size", 0);
                    c->mtimeMs = tokJll(raw, t, k + 1, "mtime", 0);
                }
            }
            k += 1 + jtokSpan(t, k + 1);
        }
    }
    HeapFree(GetProcessHeap(), 0, t);
    HeapFree(GetProcessHeap(), 0, raw);
}

// JSON number token -> long long (jsmn primitives are strings in the buffer)
static long long tokJll(const char *js, const jsmntok_t *t, int parent, const char *key, long long def) {
    if (parent < 0 || t[parent].type != JSMN_OBJECT) return def;
    int cnt = t[parent].size;
    int k = parent + 1;
    int kl = (int)strlen(key);
    for (int i = 0; i < cnt; i++) {
        if (t[k].type == JSMN_STRING && t[k].end - t[k].start == kl
            && memcmp(js + t[k].start, key, kl) == 0) {
            if (t[k + 1].type == JSMN_PRIMITIVE) {
                char b[32]; int l = t[k + 1].end - t[k + 1].start;
                if (l > 0 && l < 31) { memcpy(b, js + t[k + 1].start, l); b[l] = 0; return _atoi64(b); }
            }
            return def;
        }
        k += 1 + jtokSpan(t, k + 1);
    }
    return def;
}

static void tokCursorSave(void) {
    if (!g_tokCursorN) return;
    HANDLE h = CreateFileW(g_tokCursorPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[700];
    DWORD wrote = 0;
    WriteFile(h, "{\n", 2, &wrote, NULL);
    for (int i = 0; i < g_tokCursorN; i++) {
        int n = snprintf(line, sizeof(line), "  \"%s\": {\"size\": %lld, \"mtime\": %lld}%s\n",
                         g_tokCursor[i].path, g_tokCursor[i].size, g_tokCursor[i].mtimeMs,
                         i + 1 < g_tokCursorN ? "," : "");
        if (n > 0) WriteFile(h, line, (DWORD)n, &wrote, NULL);
    }
    WriteFile(h, "}\n", 2, &wrote, NULL);
    CloseHandle(h);
}

static TokCursor *tokCursorFind(const char *path) {
    for (int i = 0; i < g_tokCursorN; i++)
        if (lstrcmpA(g_tokCursor[i].path, path) == 0) return &g_tokCursor[i];
    return NULL;
}

static void tokCursorSet(const char *path, long long size, long long mtimeMs) {
    TokCursor *c = tokCursorFind(path);
    if (!c) {
        if (g_tokCursorN >= TOK_MAX_FILES) return;
        c = &g_tokCursor[g_tokCursorN++];
        memset(c, 0, sizeof(*c));
        lstrcpynA(c->path, path, 520);
    }
    c->size = size; c->mtimeMs = mtimeMs;
    g_tokCursorDirty = 1;
}

// ------------------------------------------------------------- directory ----
static void tokScanDir(const wchar_t *dir, int recursive, const char *appName, const long long *bnd,
                       const TokKeys *tk) {
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!recursive) continue;
            if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0) continue;
            wchar_t sub[MAX_PATH];
            swprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            tokScanDir(sub, 1, appName, bnd, tk);
            continue;
        }
        int nl = lstrlenW(fd.cFileName);
        if (nl < 6 || lstrcmpiW(fd.cFileName + nl - 6, L".jsonl") != 0) continue;
        wchar_t full[MAX_PATH];
        swprintf(full, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        // mtime+size come straight from the directory enumeration: a separate
        // GetFileAttributesExW per file is another WSL-redirector round trip
        long long mt = ((long long)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime) / 10000 - 11644473600000LL;
        long long fsz = (long long)fd.nFileSizeLow + ((long long)fd.nFileSizeHigh << 32);
        if (mt && g_cacheMtimeMs && mt <= g_cacheMtimeMs) continue; // the Electron cache already covers it
        char pathA[520];
        tokWideToUtf8(full, pathA, 520);
        TokCursor *c = tokCursorFind(pathA);
        // Unchanged since the last scan: skip it. Opening one file over the WSL
        // redirector costs ~25ms, and re-opening all 21 active files every
        // rescan is what blocked the bar for ~600ms and made refresh lag.
        if (c && c->size == fsz && c->mtimeMs == mt) { g_tokDbgFiles++; g_tokDbgHits++; continue; }
        long long from = c ? c->size : 0;
        if (c) g_tokDbgHits++;
        g_tokDbgFiles++;
        long long neu = tokScanFile(full, from, appName, bnd, tk);
        if (neu >= 0) { g_tokDbgRead += (int)(neu - from); tokCursorSet(pathA, neu, mt); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// --------------------------------------------------------------- driver ----
// tokens.enabled flipped off: forget every cursor, in memory AND on disk, so
// the next enable is one clean full re-read instead of resuming from cursors
// that silently skipped everything written to the session stores while off.
void tokLiveReset(void) {
    memset(g_tokCursor, 0, sizeof(g_tokCursor));
    g_tokCursorN = 0;
    g_tokCursorDirty = 0;
    g_tokAllLive = 0;
    if (g_tokCursorPath[0]) DeleteFileW(g_tokCursorPath);
}

// Record the seed (the Electron cache) so the live scan only counts what is
// newer, and nothing already in the cache is counted twice.
void tokLiveSeed(long long cacheMaxTs, long long cacheMtimeMs) {
    g_cacheMaxTs = cacheMaxTs;
    g_cacheMtimeMs = cacheMtimeMs;
}

// Scan every enabled JSONL session store. Returns the number of records added.
long tokLiveScan(void) {
    if (!g_cfgLoaded) return 0;
    if (!(g_cfg.tokensEnabled)) return 0;
    long long bnd[191]; // must match DASH_MAX_DAYS (p_ui.c) + 1
    SYSTEMTIME st; GetLocalTime(&st);
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME fm; SystemTimeToFileTime(&st, &fm);
    for (int j = 0; j <= 190; j++) bnd[j] = dashMidnightMs(&fm, j - 1);
    long long before = g_tokAllLive;
    // Self-heal: if the in-memory set was lost (an early config reload used to
    // reset it) but the file still holds entries, reload it. Without this the
    // scan re-reads every active file from byte 0 and DOUBLE COUNTS them.
    if (!g_tokCursorN) tokCursorLoad();
    for (int i = 0; i < g_cfg.tokSrcCount; i++) {
        TokSource *s = &g_cfg.tokSrc[i];
        if (!s->enabled || !s->sessionsDir || !*s->sessionsDir) continue;
        wchar_t dir[MAX_PATH];
        // the one bounded "~" expansion in the binary: it refuses a profile
        // that does not fit and truncates the rest into the room that is left,
        // so appending can never run off the buffer (or hand it a negative
        // count)
        subsPathExpand(s->sessionsDir, dir, MAX_PATH);
        // pi nests its sessions one directory per project; a flat store has
        // no subdirectories, so recursing is harmless there and required here.
        // The key is per source (recursive), not a hardcoded harness name.
        int recursive = s->recursive;
        TokKeys tk;
        tokKeysBuild(s, &tk);
        tokScanDir(dir, recursive, s->app, bnd, &tk);
    }
    if (g_tokCursorDirty) { tokCursorSave(); g_tokCursorDirty = 0; }
    if (g_cfg.debug) {
        char lb[160];
        sprintf(lb, "[wizbar] token live scan: files=%d cursorHits=%d bytesRead=%d cursors=%d",
                g_tokDbgFiles, g_tokDbgHits, g_tokDbgRead, g_tokCursorN);
        writeLogA(lb);
        g_tokDbgFiles = g_tokDbgHits = g_tokDbgRead = 0;
    }
    return (long)(g_tokAllLive - before);
}

void tokLiveInit(void) {
    if (g_cfg.debug) writeLogA("[wizbar] tokLiveInit called");
    // ~/.wizbar/token-cursors.json
    subsPathExpand(L"~\\.wizbar\\token-cursors.json", g_tokCursorPath, MAX_PATH);
    tokCursorLoad();
}
