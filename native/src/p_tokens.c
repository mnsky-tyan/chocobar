// live token usage: scan the JSONL session stores directly
//
// WHY: the bar used to read ONLY the Electron app's ~/.wizbar/token-cache.json.
// With the Electron bar gone nothing rewrites that file, so every number
// froze at the moment it died (a "token dashboard is stale, still 0" report -
// the cache's last record was 2h old and local midnight had rolled over).
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
// (aggRecord and dashMidnightMs are p_ui.c's: the drain owns the aggregates)
static long long parseLL(const char *p, const char *end);
static char *readFileUtf8(const wchar_t *path, int *outLen);
static void stripLineComments(char *s);
static int jtokSpan(const jsmntok_t *t, int i);
static void writeLogA(const char *s);

// how many tokens the live scan folded in (the "+N" log line). Every number
// the bar shows is derived in p_ui.c from the shared per-day buckets, so this
// scan keeps no totals of its own to drift out of step with them.
long long g_tokAllLive = 0;
// scan diagnostics (one log line per rescan while general.debug is on)
int g_tokDbgFiles = 0, g_tokDbgHits = 0, g_tokDbgRead = 0;

#define TOK_MAX_FILES 4096
#define TOK_CHARS_NONE (-1LL) // chars: the estimator has never counted this file
// Per-file cursor, persisted to ~/.wizbar/token-cursors.json. `chars` and
// `real` exist only for tokens.estimateMissingUsage: chars stays
// TOK_CHARS_NONE until the estimator's first count of the file (the key is
// absent only in a cursor written before the feature existed, which reads
// back as NONE), so a genuine total of 0 - a store whose lines carry no
// transcript text at all - stays distinct from "never counted" and resumes
// from its byte cursor instead of re-reading from byte zero on every append.
typedef struct {
    char path[520];
    long long size;
    long long mtimeMs;
    long long chars;   // transcript text counted so far; TOK_CHARS_NONE = never
    char real[80];     // comma-joined providers that DID report usage here
} TokCursor;
static TokCursor g_tokCursor[TOK_MAX_FILES];
static int g_tokCursorN = 0;
static int g_tokCursorDirty = 0;
static long long tokJll(const char *js, const jsmntok_t *t, int parent, const char *key, long long def);

static wchar_t g_tokCursorPath[MAX_PATH]; // ~/.wizbar/token-cursors.json
static long long g_cacheMaxTs = 0;     // newest ts the Electron cache holds
static int g_tokForceFullRead = 1;     // startup must rebuild memory-only aggregates
static int g_tokFullReadNow = 0;       // this walk is the forced full re-read
static long long g_cacheMtimeMs = 0;   // when that cache was last written

// ---------------------------------------------------------- pending records --
// The scan runs on a worker thread, but the dashboard's aggregates stay
// UI-thread-affine: parsing only STAGES records here and the UI thread
// applies them through aggRecord when the scan's done message lands
// (tokDrainPending, p_ui.c). One scan at a time (g_tokScanBusy), so this
// buffer needs no lock of its own.
typedef struct {
    char app[20];   // aggRecord clamps to 19 + NUL
    char model[41]; // ids longer than the cap keep their prefix
    short alen, mlen;
    long long ts, in, out, cr, cw;
    short est;      // this row was estimated, not reported (see tokEstimate)
} TokPend;
#define TOK_PEND_MIN 4096
static TokPend *g_tokPend = NULL;
static int g_tokPendN = 0, g_tokPendCap = 0;
static int g_tokPendSeedReset = 0; // seed re-read: the drain clears aggregates first

static void tokPendPush(const char *app, int alen, long long ts,
                        long long in, long long out, long long cr, long long cw,
                        const char *model, int mlen, int est) {
    if (alen < 0) alen = 0;
    if (alen > 19) alen = 19;
    if (mlen < 0) mlen = 0;
    if (mlen > 40) mlen = 40;
    if (g_tokPendN >= g_tokPendCap) {
        int cap = g_tokPendCap ? g_tokPendCap * 2 : TOK_PEND_MIN;
        TokPend *p = (TokPend *)(g_tokPend
            ? HeapReAlloc(GetProcessHeap(), 0, g_tokPend, (size_t)cap * sizeof(TokPend))
            : HeapAlloc(GetProcessHeap(), 0, (size_t)cap * sizeof(TokPend)));
        if (!p) return; // dropping records only under-counts, never corrupts
        g_tokPend = p;
        g_tokPendCap = cap;
    }
    TokPend *r = &g_tokPend[g_tokPendN++];
    memcpy(r->app, app, (size_t)alen);
    r->app[alen] = 0;
    r->alen = (short)alen;
    if (mlen) memcpy(r->model, model, (size_t)mlen);
    r->model[mlen] = 0;
    r->mlen = (short)mlen;
    r->ts = ts;
    r->in = in;
    r->out = out;
    r->cr = cr;
    r->cw = cw;
    r->est = (short)(est ? 1 : 0);
}

static void tokPendClear(void) { g_tokPendN = 0; }

// The arena only ever grows, and a cold seed re-read sizes it for every record
// the Electron cache holds (10k+ on a large store). Hand it back once a
// scan used a small fraction of that, so the bar's RSS tracks the warm steady
// state instead of the one-time peak.
static void tokPendShrink(void) {
    if (g_tokPendCap > TOK_PEND_MIN && g_tokPendN * 4 < g_tokPendCap) {
        HeapFree(GetProcessHeap(), 0, g_tokPend);
        g_tokPend = NULL;
        g_tokPendCap = 0;
    }
    g_tokPendN = 0;
}

// ------------------------------------------------------------ small utils ----
// wide path -> utf8 (for the cursor file, which is plain JSON)
static int tokWideToUtf8(const wchar_t *w, char *out, int cb) {
    if (!w || !*w) { if (cb > 0) out[0] = 0; return 0; }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cb, NULL, NULL);
    if (n > 0) return n - 1;
    if (cb > 0) out[0] = 0;
    return 0;
}

// ------------------------------------------------------- usage estimator --
// WHY: a route that is a real API answers with a usage block, so the numbers
// tokParseLine reads are exact. A route that is a local app or CLI driven
// through a bridge - CodeBuddy over workbuddy, the MiMo desktop, the agy CLI -
// has no API to answer, so its turns arrive with an all-zero usage object and
// tokScanFile would drop them, exactly as if the turn never happened.
//
// THE MODEL, both facts measured against 20,777 turns from a real pi store
// that DID report usage, counted the way tokTextLen counts them:
//   1. the prompt is roughly the whole transcript, at about 4 chars per token,
//      scaled by inFactor;
//   2. the prompt then SATURATES at saturateTokens and stops growing however
//      long the transcript gets, because the harness compacts a long
//      conversation before sending it. That cap is the important part: the raw
//      character count of a long session runs to tens of millions of tokens, so
//      without it a single turn estimates at millions - more than any model
//      accepts.
// Packed into one formula: prompt = min(transcriptChars / 4 * inFactor,
// saturateTokens). The completion side needs only outFactor: it is the turn's
// own reply text.
//
// ACCURACY, measured over the same 20,777 turns:
//   transcript size   median estimated / reported
//     under 200k        1.02
//     200k-400k         0.96
//     400k-1M           0.91
//     over 1M           1.00
//   overall aggregate 1.004
// A SINGLE turn is not trustworthy - compaction is a step, so a turn is either
// close or several times off and the stored record does not say which. Fit for
// per-model and per-app totals, which is everything the dashboard shows, and
// unfit for a per-session or per-day figure.
//
// Returns 0 when there is nothing to go on, so the caller can skip the turn
// instead of recording a fabricated zero.
static long long tokEstimate(long long transcriptChars, long long replyChars,
                             double inFactor, double saturate, double outFactor,
                             long long *outTokens) {
    if (transcriptChars <= 0 && replyChars <= 0) return 0;
    double prompt = ((double)transcriptChars / 4.0) * inFactor;
    if (prompt > saturate) prompt = saturate;
    long long pins = (long long)(prompt + 0.5);
    long long oins = (long long)(((double)replyChars / 4.0) * outFactor + 0.5);
    if (outTokens) *outTokens = oins;
    return pins;
}

// Step over one JSON string. Returns the position after the closing quote and
// reports the string's length in CHARACTERS, so an escaped quote or a \uXXXX
// escape counts once rather than as its bytes.
static const char *tokJsonStr(const char *p, const char *e, int *chars) {
    *chars = 0;
    if (p >= e || *p != '"') return p;
    p++;
    while (p < e && *p != '"') {
        if (*p == '\\' && p + 1 < e) {
            if (p[1] == 'u' && p + 5 < e) { p += 6; (*chars)++; continue; }  // \uXXXX = 1 char
            p += 2; (*chars)++; continue;                                  // so is any escape
        }
        p++;
        (*chars)++;
    }
    return (p < e) ? p + 1 : e;
}

// Step over one JSON value of any kind (object, array, string, or primitive),
// so a caller can walk a structure without parsing all of it.
static const char *tokJsonSkip(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
    if (p >= e) return e;
    if (*p == '"') { int n; return tokJsonStr(p, e, &n); }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            if (*p == '"') { int n; p = tokJsonStr(p, e, &n); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { p++; depth--; if (depth <= 0) break; continue; }
            p++;
        }
        return p;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != '\n') p++;
    return p;
}

// The first non-blank position at or after p, so a value's start can be
// captured before tokJsonSkip walks over it.
static const char *tokJsonValStart(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// Characters of text one JSON value contributes. The reference this mirrors
// takes the RAW BYTE SPAN of a value rather than its decoded JSON:
//   * a string is its characters, counting each escape once;
//   * an array is its members' raw JSON joined by spaces, which is the span
//     without the two brackets ('[a,b]' and 'a b' have the same inner length);
//   * anything else keeps its raw JSON.
// This stays byte-level on purpose: the estimator's constants are fitted to
// THESE counts, so the bar needs neither a JSON parser nor a character-set
// decode, and a turn is estimated the same way on every machine.
static int tokValChars(const char *vs, const char *ve) {
    if (!vs || !ve || ve <= vs) return 0;
    if (*vs == '"') { int n = 0; tokJsonStr(vs, ve, &n); return n; }
    if (*vs == '[') return (int)(ve - vs) - 2;   // drop the brackets
    return (int)(ve - vs);
}

// Characters of transcript text a JSONL line's message carries: its "content"
// plus the text of every "sections" entry. Returns the total and reports the
// content-only length separately, because the completion half of an estimate
// is just the turn's own reply.
//
// WHY ANCHORED ON "message": a compaction record embeds a whole systemMessage
// with its own content and sections, and the first "content": in such a line is
// inside a tool schema. Taking the message object's span keeps that out - 284
// real compaction lines each carrying ~150k characters would otherwise be
// counted twice. Anchoring is also what makes it cheap: content is the first
// key of every message, so the first "content": inside the span is the right
// one and the search stops there.
static int tokTextLen(const char *ln, int len, int *replyChars) {
    const char *e = ln + len;
    if (replyChars) *replyChars = 0;
    const char *mo = NULL;      // the message object's opening brace
    for (const char *p = ln; p + 11 <= e; p++) {
        if (memcmp(p, "\"message\":{", 11) == 0) { mo = p + 10; break; }
    }
    if (!mo) return 0;
    // the message object's full span. mo points AT the brace, so tokJsonSkip
    // walks the whole object and stops after it; ms is its contents.
    const char *me = tokJsonSkip(mo, e);
    const char *ms = mo + 1;

    // "content":<value> -> the value's characters. content is the FIRST key of
    // every pi message, so the first match inside the span is the right one; the
    // search for sections resumes from AFTER it, so a "content" nested inside a
    // tool input can never be picked up by mistake.
    int total = 0;
    const char *afterContent = ms;
    for (const char *p = ms; p + 10 <= me; p++) {
        if (memcmp(p, "\"content\":", 10) == 0) {
            const char *vs = tokJsonValStart(p + 10, me);
            const char *ve = tokJsonSkip(vs, me);
            int v = tokValChars(vs, ve);
            total += v;
            if (replyChars) *replyChars += v;
            afterContent = ve;
            break;
        }
    }

    // "sections":{...} -> sum the string VALUES; the keys ("preamble",
    // "tools", "rules") are structure, not transcript text, and counting them
    // on every line of a long session would skew a whole archive.
    const char *s = NULL;
    for (const char *p = afterContent; p + 11 <= me; p++) {
        if (memcmp(p, "\"sections\":", 11) == 0) { s = p + 11; break; }
    }
    if (s) {
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        if (s < e && *s == '{') {
            s++;
            while (s < e && *s != '}') {
                while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == ',')) s++;
                if (s >= e || *s != '"') break;
                int kn;
                s = tokJsonStr(s, e, &kn);          // the key: not text
                while (s < e && (*s == ' ' || *s == '\t')) s++;
                if (s >= e || *s != ':') break;
                s++;
                while (s < e && (*s == ' ' || *s == '\t')) s++;
                if (s < e && *s == '"') {
                    int vn;
                    s = tokJsonStr(s, e, &vn);
                    total += vn;
                } else {
                    s = tokJsonSkip(s, e);
                }
            }
        }
    }
    return total;
}



// one record's fields, parsed out of a JSONL line
typedef struct {
    long long ts;
    const char *app; int appLen;
    long long in, out, cr, cw;
    const char *model; int modelLen;
    const char *prov; int provLen; // "provider" (pi) - the route the turn travelled
} TokRec;

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
    out->prov = NULL; out->provLen = 0;
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
    // provider: which route carried the turn. pi spells it "provider" and keeps
    // "api" as the sub-protocol; the estimator keys on this to tell a bridged
    // route (workbuddy, mimo-desktop, antigravity-cli) from an API one.
    for (int i = 0; i + 12 <= len; i++) {
        if (memcmp(ln + i, "\"provider\":", 11) == 0) {
            const char *v = ln + i + 11;
            while (v < ln + len && (*v == ' ' || *v == '\t')) v++;
            if (v < ln + len && *v == '"') {
                v++;
                const char *e = v;
                while (e < ln + len && *e != '"' && e - v < 32) e++;
                if (e > v) { out->prov = v; out->provLen = (int)(e - v); }
            }
            break;
        }
    }
    // usage numbers: cacheRead/cacheWrite are breakdown columns, input/output
    // are raw (the TOKEN CONVENTION in the README's "Token accounting")
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
//
// With cfg->tokEstimate on, a turn whose provider reported nothing at all is
// filled in from the transcript instead of dropped - see tokEstimate. The
// decision is per ROUTE, not per turn: within this file the providers that did
// report are collected, and a blank turn is estimated only when its own
// provider is not among them. That matters because one session routinely mixes
// a reporting route with a bridged one - 824 of 1,028 real CodeBuddy turns sit
// in such files, where a per-file rule would discard every one as an abort.
static long long tokScanFile(const wchar_t *path, long long from, const char *appName,
                             const TokKeys *tk, const Config *cfg, TokCursor *cur) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz; if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return -1; }
    long long size = (long long)sz.QuadPart;
    if (from > size) {
        // truncated: the file SHRANK, so the running transcript count no longer
        // describes it. Drop the estimator state instead of carrying a length the
        // file no longer has - a later append then re-accumulates from here, and
        // nothing is counted twice.
        from = size;
        if (cur) cur->chars = TOK_CHARS_NONE;
    }
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
    const int estimating = cfg->tokEstimate ? 1 : 0;
    // transcript text counted so far in this file: an appended read continues
    // the running total from the cursor, a re-read from byte zero restarts it.
    long long chars = (from > 0 && cur && cur->chars > 0) ? cur->chars : 0;
    char real[80] = "";                  // providers here that DID report
    if (from > 0 && cur && cur->real[0]) lstrcpynA(real, cur->real, 80);
    int realSeen = (int)strlen(real);
    while (p < e) {
        char *nl = (char *)memchr(p, '\n', (size_t)(e - p));
        if (!nl) break; // partial tail: do not advance the cursor past it
        int len = (int)(nl - p);
        if (len > 0) {
            // The transcript size is the text of EVERY line so far, not only the
            // ones carrying usage - a user turn and a tool result both go into
            // the prompt. One scan per line, which is why it sits behind the
            // config flag; the file is already being read end to end.
            int replyChars = 0;
            int lineChars = estimating ? tokTextLen(p, len, &replyChars) : 0;
            TokRec r;
            if (tokParseLine(p, len, tk, &r) && r.ts > g_cacheMaxTs) {
                long long fld[4] = { r.in, r.out, r.cr, r.cw };
                long long sum = fld[0] + fld[1] + fld[2] + fld[3];
                if (sum > 0) {
                    // a real number: this route reports, so it has an API behind it.
                    // Remember it, so a later blank turn from the SAME route is read
                    // as an abort rather than as a bridged one.
                    if (estimating && r.provLen > 0) {
                        char pv[40]; int pl = r.provLen < 39 ? r.provLen : 39;
                        memcpy(pv, r.prov, (size_t)pl); pv[pl] = 0;
                        // already listed? a substring test is enough at <10 entries
                        if (!strstr(real, pv)) {
                            int need = (realSeen ? 1 : 0) + pl;
                            if (realSeen + need < (int)sizeof(real) - 1) {
                                if (realSeen) real[realSeen++] = ',';
                                memcpy(real + realSeen, pv, (size_t)pl);
                                realSeen += pl; real[realSeen] = 0;
                            }
                        }
                    }
                    // staged, not aggregated: the aggregates are UI-thread-affine
                    // and this runs on the scan worker (the drain applies them)
                    tokPendPush(appName, (int)strlen(appName), r.ts, fld[0], fld[1], fld[2], fld[3],
                                r.model, r.modelLen, 0);
                    g_tokAllLive += sum;
                } else if (estimating) {
                    // No usage at all. Estimate it unless this route reports
                    // elsewhere in the file, in which case the turn was aborted
                    // before anything was billed and inventing tokens for it
                    // would over-count.
                    char pv[40] = "";
                    int blind = 1;
                    if (r.provLen > 0) {
                        int pl = r.provLen < 39 ? r.provLen : 39;
                        memcpy(pv, r.prov, (size_t)pl); pv[pl] = 0;
                        if (realSeen && strstr(real, pv)) blind = 0;
                    }
                    if (blind) {
                        long long oout = 0;
                        long long pin = tokEstimate(chars + lineChars, replyChars,
                                                    cfg->tokEstIn, cfg->tokEstSat,
                                                    cfg->tokEstOut, &oout);
                        if (pin > 0) {
                            tokPendPush(appName, (int)strlen(appName), r.ts,
                                        pin, oout, 0, 0, r.model, r.modelLen, 1);
                            g_tokAllLive += pin + oout;
                        }
                    }
                }
            }
            // this line's text is now part of the transcript the NEXT turn sends
            if (estimating) chars += lineChars;
        }
        consumed += len + 1;
        p = nl + 1;
    }
    // persist what the estimator learned about this file, so an appended read
    // next rescan continues the count and keeps the same answer
    if (estimating && cur) { cur->chars = chars; lstrcpynA(cur->real, real, 80); }
    HeapFree(GetProcessHeap(), 0, buf);
    return start + consumed;
}

// ---------------------------------------------------------- cursor file ----
// The cursor file is machine-written JSON, and the only characters a stored
// path or provider list can carry that JSON forbids raw are backslash (every
// Windows path separator) and quote: escape on write, decode on read.
static int tokJsonEsc(char *out, int cap, const char *s) {
    int n = 0;
    for (; *s; s++) {
        int need = (*s == '\\' || *s == '"') ? 2 : 1;
        if (n + need > cap - 1) return -1;
        if (need == 2) out[n++] = '\\';
        out[n++] = *s;
    }
    out[n] = 0;
    return n;
}

// decode tokJsonEsc's two escapes in place; returns the decoded length
static int tokJsonUnesc(char *s, int n) {
    int r = 0, w = 0;
    while (r < n) {
        if (s[r] == '\\' && r + 1 < n && (s[r + 1] == '"' || s[r + 1] == '\\')) r++;
        s[w++] = s[r++];
    }
    return w;
}

// cfg is the caller's pinned generation: this also runs on the scan worker.
static void tokCursorLoad(const Config *cfg) {
    g_tokCursorN = 0;
    int len = 0;
    char *raw = readFileUtf8(g_tokCursorPath, &len);
    if (cfg->debug) {
        char lb[200];
        sprintf(lb, "[wizbar] tokCursorLoad: file=%s len=%d", raw ? "ok" : "missing", len);
        writeLogA(lb);
    }
    if (!raw) return;
    stripLineComments(raw);
    jsmn_parser p; jsmn_init(&p);
    int nt = jsmn_parse(&p, raw, (size_t)len, NULL, 0);
    if (nt <= 0) {
        if (cfg->debug) {
            char lb[200];
            sprintf(lb, "[wizbar] tokCursorLoad: parse FAILED len=%d", len);
            writeLogA(lb);
        }
        HeapFree(GetProcessHeap(), 0, raw);
        return;
    }
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
                    pl = tokJsonUnesc(pathA, pl); pathA[pl] = 0;
                    TokCursor *c = &g_tokCursor[g_tokCursorN++];
                    memset(c, 0, sizeof(*c));
                    lstrcpynA(c->path, pathA, 520);
                    c->size = tokJll(raw, t, k + 1, "size", 0);
                    c->mtimeMs = tokJll(raw, t, k + 1, "mtime", 0);
                    // estimator state, absent in a cursor written before the
                    // feature existed: an absent chars reads back as
                    // TOK_CHARS_NONE ("nothing known yet") and the next full
                    // re-read rebuilds them
                    c->chars = tokJll(raw, t, k + 1, "chars", TOK_CHARS_NONE);
                    jstrCopyA(c->real, (int)sizeof(c->real), raw, t,
                              jobjGet(raw, t, k + 1, "real"), "");
                    c->real[tokJsonUnesc(c->real, (int)strlen(c->real))] = 0;
                }
            }
            k += 1 + jtokSpan(t, k + 1);
        }
    }
    HeapFree(GetProcessHeap(), 0, t);
    HeapFree(GetProcessHeap(), 0, raw);
}

// JSON number token -> long long (jsmn primitives are strings in the buffer).
// The key lookup is jobjGet's job - it already walks the same key/value pairs.
static long long jllTok(const char *js, const jsmntok_t *t, int i, long long def); // chocobar

static long long tokJll(const char *js, const jsmntok_t *t, int parent, const char *key, long long def) {
    return jllTok(js, t, jobjGet(js, t, parent, key), def);
}

static void tokCursorSave(void) {
    if (!g_tokCursorN) return;
    HANDLE h = CreateFileW(g_tokCursorPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[1344]; // escaped path + escaped real + three %lld + format overhead
    DWORD wrote = 0;
    WriteFile(h, "{\n", 2, &wrote, NULL);
    for (int i = 0; i < g_tokCursorN; i++) {
        char escP[1040], escR[160]; // 2x a full path / real list, plus NUL
        if (tokJsonEsc(escP, (int)sizeof(escP), g_tokCursor[i].path) < 0) continue;
        if (tokJsonEsc(escR, (int)sizeof(escR), g_tokCursor[i].real) < 0) continue;
        int n = snprintf(line, sizeof(line),
                         "  \"%s\": {\"size\": %lld, \"mtime\": %lld, \"chars\": %lld, \"real\": \"%s\"}%s\n",
                         escP, g_tokCursor[i].size, g_tokCursor[i].mtimeMs,
                         g_tokCursor[i].chars, escR,
                         i + 1 < g_tokCursorN ? "," : "");
        // snprintf returns the length the output WOULD have had: positive and
        // larger than the buffer exactly on truncation, so clamp to what was
        // actually written before it can become a WriteFile byte count
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
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

// find a cursor or create it; a fresh entry has never been counted
static TokCursor *tokCursorGet(const char *path) {
    TokCursor *c = tokCursorFind(path);
    if (!c) {
        if (g_tokCursorN >= TOK_MAX_FILES) return NULL;
        c = &g_tokCursor[g_tokCursorN++];
        memset(c, 0, sizeof(*c));
        lstrcpynA(c->path, path, 520);
        c->chars = TOK_CHARS_NONE;
    }
    return c;
}

static void tokCursorSet(const char *path, long long size, long long mtimeMs) {
    TokCursor *c = tokCursorGet(path);
    if (!c) return;
    c->size = size; c->mtimeMs = mtimeMs;
    g_tokCursorDirty = 1;
}

// ------------------------------------------------------------- directory ----
static void tokScanDir(const wchar_t *dir, int recursive, const char *appName,
                       const TokKeys *tk, const Config *cfg) {
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
            tokScanDir(sub, 1, appName, tk, cfg);
            continue;
        }
        int nl = lstrlenW(fd.cFileName);
        if (nl < 6 || lstrcmpiW(fd.cFileName + nl - 6, L".jsonl") != 0) continue;
        wchar_t full[MAX_PATH];
        swprintf(full, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        // mtime+size come straight from the directory enumeration: a separate
        // GetFileAttributesExW per file is another WSL-redirector round trip
        long long mt = fileTimeToUnixMs(&fd.ftLastWriteTime);
        long long fsz = (long long)fd.nFileSizeLow + ((long long)fd.nFileSizeHigh << 32);
        if (mt && g_cacheMtimeMs && mt <= g_cacheMtimeMs) continue; // the Electron cache already covers it
        char pathA[520];
        tokWideToUtf8(full, pathA, 520);
        TokCursor *c = tokCursorFind(pathA);
        // Unchanged since the last scan: skip it. Opening one file over the WSL
        // redirector costs ~25ms, and re-opening all 21 active files every
        // rescan is what blocked the bar for ~600ms and made refresh lag.
        if (c && !g_tokFullReadNow && c->size == fsz && c->mtimeMs == mt) { g_tokDbgFiles++; g_tokDbgHits++; continue; }
        long long from = (c && !g_tokFullReadNow) ? c->size : 0;
        // The estimator counts a file's transcript from byte zero and carries the
        // running total in the cursor. A cursor with no total yet (TOK_CHARS_NONE;
        // a real total of 0 is a store whose lines carry no transcript text) would
        // otherwise resume as if the transcript were tiny: re-read the file once.
        if (cfg->tokEstimate && c && c->size > 0 && c->chars < 0) from = 0;
        if (c) g_tokDbgHits++;
        g_tokDbgFiles++;
        // the cursor must exist BEFORE the read so the count this scan just made
        // (chars, real) lands in it instead of being lost to a fresh memset
        long long neu = tokScanFile(full, from, appName, tk, cfg, tokCursorGet(pathA));
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
    g_tokForceFullRead = 1; // no cursors left: the next scan re-reads from byte 0
    g_tokAllLive = 0;
    if (g_tokCursorPath[0]) DeleteFileW(g_tokCursorPath);
}

// Record the seed (the Electron cache) so the live scan only counts what is
// newer, and nothing already in the cache is counted twice.
void tokLiveSeed(long long cacheMaxTs, long long cacheMtimeMs) {
    // A boundary change invalidates the live half. An actual seed re-read
    // does too, even if its maximum timestamp stayed the same: the UI drain
    // will clear the aggregates before applying this scan. Warm scans that
    // keep both the boundary and aggregates must stay incremental, or replay
    // would add the whole history again on every timer tick.
    if (g_cacheMaxTs != cacheMaxTs) g_tokForceFullRead = 1;
    if (g_tokPendSeedReset) g_tokForceFullRead = 1;
    g_cacheMaxTs = cacheMaxTs;
    g_cacheMtimeMs = cacheMtimeMs;
}

// Scan every enabled JSONL session store. Records are STAGED (tokPendPush)
// for the UI thread's drain; the return is the token sum they carried.
// cfg is the pinned config generation: this runs on a worker thread.
long tokLiveScan(const Config *cfg) {
    if (!g_cfgLoaded) return 0;
    if (!cfg->tokensEnabled) return 0;
    long long before = g_tokAllLive;
    // Self-heal: if the in-memory set was lost (an early config reload used to
    // reset it) but the file still holds entries, reload it. Without this the
    // scan re-reads every active file from byte 0 and DOUBLE COUNTS them.
    if (!g_tokCursorN) tokCursorLoad(cfg);
    // A seed (re)read means the live half is rebuilt from scratch, so this scan
    // has to re-stage every record above the boundary - not just the appended
    // tail. One full read; the cursors then resume normally on later scans.
    int fullRead = g_tokForceFullRead;
    g_tokForceFullRead = 0;
    g_tokFullReadNow = fullRead; // tokScanDir reads this for the walk below
    for (int i = 0; i < cfg->tokSrcCount; i++) {
        const TokSource *s = &cfg->tokSrc[i];
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
        tokScanDir(dir, recursive, s->app, &tk, cfg);
    }
    g_tokFullReadNow = 0; // the walk is done; later rescans resume from cursors
    if (g_tokCursorDirty) { tokCursorSave(); g_tokCursorDirty = 0; }
    if (cfg->debug) {
        if (fullRead) writeLogA("[wizbar] token live scan: FORCED full re-read (history rebuild)");
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
    tokCursorLoad(&g_cfg);
}
