// p_agg.c - dashboard aggregation, shared by the Windows and macOS bars.
//
// WHY THIS PART EXISTS
// Measured 2026-10-11: chocobar.c + p_utils.c + p_tokens.c compile off-Windows
// with zero errors, but the token AGGREGATION - the per-app, per-model and
// per-day buckets the bar displays - lived inside p_ui.c, the Windows renderer,
// which is not portable. So the code that PRODUCES usage records was portable
// while the code that TOTALS them was not, and the numbers were unreachable from
// macOS.
//
// This part is that aggregation, lifted out of p_ui.c unchanged. It is appended
// to the Windows translation unit exactly where it came from, so Windows is
// behaviourally identical, and it is compiled into the macOS build beside the
// portable core. One implementation, two platforms: two builds disagreeing about
// totals would be a worse bug than either being wrong alone.
//
// WHAT MOVED HERE
//   - the aggregation tables and their limits (DASH_MAX_*)
//   - aggRecord, the only writer of those tables
//   - the day-boundary arithmetic (aggDayIndex, dashMidnightMs,
//     dashDayRollover), which is where the DST correctness lives
//   - tokDeriveWindows, the stat cards
//   - appLabelW, the tokens.labels display-name mapping
//   - tokDrainPending, the apply step for one finished scan
//   - the JSON helpers the cache reader needs (findStr, parseLL)
//
// WHAT STAYED IN p_ui.c, AND WHY
//   - scanTokenCacheInner: reads the Electron cache through Win32 file APIs
//   - blendCr: COLORREF/GDI colour math, pure renderer
//   - the worker thread and the PostMessage hand-off
//   - everything that draws
//
// The seam is tokScanFinishedNotify(): on Windows the drain ends by posting a
// message to the bar window, on macOS there is no window message loop, so each
// platform supplies that one call.

// ---- shared scan state ----------------------------------------------------------
//
// These were p_ui.c statics. They move here because the drain below writes them
// and both platforms' front-ends read them. Types stay as they were on Windows
// (LONG for the interlocked flags) so the Windows build is unchanged.

static long long g_tokensToday = -1;
static int g_tokensTick = 0;
static unsigned long long g_tokDataVersion = 0;   // moves on every completed scan
static long long g_cacheMaxTsScan = 0;
static long long g_cacheMtimeScan = 0;
static DWORD g_cacheStamp[4];      // cache mtime+size stamp of the last full read
static int g_cacheReadDone = 0;    // a full read has completed at least once
static DWORD g_tokScanStart = 0;
static volatile long g_tokScanBusy = 0; // one scan at a time (busy ticks are skipped)
static volatile long g_tokScanDone = 0; // worker finished: the UI must drain
static long long g_tokScanAdded = 0;    // the "+N tokens" the worker folded in

// ---- platform seam ------------------------------------------------------------

// Called at the end of the drain so the platform can react to a completed scan.
// On Windows this posts the message that wakes the bar's paint; the macOS bar
// polls the aggregates from its timer, so it does nothing. Each platform
// defines it exactly once. The linkage differs by build ON PURPOSE: in the
// Windows single-TU build p_ui.c defines it static (so the forward decl here
// must be static too - declaring static then defining extern is C11 6.2.2p7
// UB); in the portable build the definition at the bottom of this file has
// external linkage and the decl must match.
#ifdef CHOCOBAR_PORTABLE
void tokScanFinishedNotify(void);
#else
static void tokScanFinishedNotify(void);
#endif

// ---- dashboard aggregation (filled by scanTokenCache) ----------------------
#define DASH_MAX_APPS 12
#define DASH_MAX_MODELS 64
#define DASH_MAX_DAYS 190
// displayed by-model rows (Electron capped at 7; this store has 13 models and
// a request asked for the space to be used, so the table reaches further)
#define DASH_MODEL_ROWS 13
// token-cache record fields kept separately: the Electron dash shows the
// input/output/cache R/cache W/calls table columns, not one lumped sum
typedef struct { long long in, out, cr, cw, req; } TokAgg;
// the total a row shows and ranks by: raw input+output plus the cache columns
// (the token convention the README's "Token accounting" section states). One
// home, so a fifth field cannot be added in eight places and missed in two.
static long long tokAggSum(const TokAgg *a) { return a->in + a->out + a->cr + a->cw; }
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
// Everything above these tables is the aggregation PRODUCER side, shared by
// both platforms. The CONSUMER is each platform's renderer: p_ui.c draws the
// dashboard from these tables on Windows (DASH_MODEL_ROWS, g_dash*, the day
// heatmap), main.m reads the totals through the cb* accessors on macOS.
// The tables' shape is the contract between the two front-ends.

// first boundary at or before ts: bnd[1] is today, bnd[DASH_MAX_DAYS] the
// oldest bucket, bnd[0] (tomorrow) rejects later-dated records. Returns 1 for
// today and one more per day back, or -1 outside the span.
static int aggDayIndex(long long ts, const long long *bnd) {
    int lo = 0, hi = DASH_MAX_DAYS, j = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (bnd[mid] <= ts) { j = mid; hi = mid - 1; } else lo = mid + 1;
    }
    // j == 0 is tomorrow's boundary (bnd[0]): not a bucket, so never return it
    // - a caller that indexed g_dayTot[DASH_MAX_DAYS - 0] would run one past
    // the end of both g_dayTot and g_dayApp's first dimension.
    return j > 0 ? j : -1;
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
            int wl = MultiByteToWideChar(CP_UTF8, 0, app, alen, wide, 20);
            // MultiByteToWideChar does NOT terminate on an explicit input
            // length: terminate at the RETURNED length, because the app key is
            // UTF-8 and one byte is no longer one wide character
            wide[wl > 0 ? wl : 0] = 0;
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
    // the app/model tables above are lifetime; this day table is a fixed
    // DASH_MAX_DAYS window, so a store holding records older than the window
    // makes the per-app/per-model totals and the day-derived stat cards
    // legitimately disagree
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
    return fileTimeToUnixMs(&ft);
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
// g_tokensToday is UI-thread-affine like the aggregates: the worker hands its
// "today" number over through these and the drain publishes it.
static long long g_tokInnerToday = 0;
static int g_tokInnerTodayValid = 0;
static void tokTodaySet(long long v) { g_tokInnerToday = v; g_tokInnerTodayValid = 1; }


// The UI-thread apply of one finished scan - the ONLY writer of the
// dashboard's aggregates (aggRecord). Runs when the worker's done message
// lands; the 1s metrics tick calls it too, so a lost message only delays
// the numbers by a tick instead of freezing them forever.
static void tokDrainPending(void) {
    if (g_tokScanDone != 1) return;
    g_tokScanDone = 0;
    // the buckets' frame date and the boundary array built just below must come
    // from the SAME instant: the scan is off-thread, so local midnight can pass
    // between the scan start's dashDayRollover and this drain, which would file
    // a whole scan's records one day too old
    dashDayRollover();
    long long added = g_tokScanAdded;
    long long bnd[DASH_MAX_DAYS + 1];
    SYSTEMTIME st; GetLocalTime(&st);
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME localMidnight;
    SystemTimeToFileTime(&st, &localMidnight);
    // one DST-exact boundary per heatmap bucket: bnd[j] opens the bucket that
    // holds day j-1, so a bucket never straddles a calendar day
    for (int j = 0; j <= DASH_MAX_DAYS; j++) bnd[j] = dashMidnightMs(&localMidnight, j - 1);
    if (g_tokPendSeedReset) { // a full re-read re-derives from scratch
        memset(g_dayTot, 0, sizeof(g_dayTot));
        memset(g_appAgg, 0, sizeof(g_appAgg));
        memset(g_dayApp, 0, sizeof(g_dayApp));
        memset(g_modelAgg, 0, sizeof(g_modelAgg));
        g_appCount = 0;
        g_modelCount = 0;
        g_tokPendSeedReset = 0;
    }
    for (int i = 0; i < g_tokPendN; i++) {
        TokPend *r = &g_tokPend[i];
        aggRecord(r->app, r->alen, r->ts, r->in, r->out, r->cr, r->cw, r->model, r->mlen, bnd);
    }
    tokPendShrink(); // the applied scan's high-water mark is no longer needed
    if (g_tokInnerTodayValid) { g_tokensToday = g_tokInnerToday; g_tokInnerTodayValid = 0; }
    // the stat cards read the same day buckets the heatmap draws, so the two
    // can never disagree once the window slides past a midnight
    tokDeriveWindows();
    if (added > 0) {
        char lb[128];
        sprintf(lb, "[wizbar] token live scan: +%lld tokens (live all-time %lld), today=%lld", added, g_tokAllLive, g_tokensToday);
        writeLogA(lb);
    }
    g_tokDataVersion++;
    // Release the busy flag with the same interlocked store the Windows drain
    // has always used: the scan worker's CompareExchange on this flag pairs
    // with it, and dropping the barrier here silently changed the Windows
    // binary when this code was extracted (volatile stores carry release
    // semantics on MSVC x86-64 in practice, but that is a compiler property,
    // not something the extraction was entitled to assume).
    InterlockedExchange(&g_tokScanBusy, 0);
    tokScanFinishedNotify();
    if (g_cfg.debug) { // total scan duration (the bar stayed responsive throughout)
        DWORD dt = GetTickCount() - g_tokScanStart;
        if (dt >= 15) {
            SYSTEMTIME nst; GetLocalTime(&nst);
            char lb[140];
            sprintf(lb, "[wizbar] token scan took %lu ms (off-thread) tick=%d period=%d at %02d:%02d:%02d",
                    (unsigned long)dt, g_tokensTick, g_cfg.tokensRescanSec, nst.wHour, nst.wMinute, nst.wSecond);
            writeLogA(lb);
        }
    }
}

// tokens.enabled = 0 is a master switch, not a pause: drop every aggregate and
// the live cursors, so a re-enable is one clean full re-read instead of
// resuming from cursors that skipped what arrived while off. Extracted from
// p_ui.c's scanTokenCache so BOTH platforms take the same reset - the macOS
// scan previously returned without resetting, leaving stale totals painted and
// stale cursors on disk. Memory-only; the drain applies it inline.
static void tokMasterOffReset(void) {
    g_tokPendSeedReset = 1;
    g_tokInnerToday = -1;
    g_tokInnerTodayValid = 1;
    g_cacheReadDone = 0; // a later re-enable must re-read the seed
    tokLiveReset();
    InterlockedExchange(&g_tokScanDone, 1);
    tokDrainPending();
}


// ---- macOS bridge accessors -----------------------------------------------------
//
// The core is built as one translation unit and is almost entirely `static`, so
// the macOS bar cannot see any of it. These accessors are the entire surface it
// gets. They are compiled ONLY for the portable build (CHOCOBAR_PORTABLE), so
// the Windows translation unit is byte-for-byte unchanged and cannot grow a
// symbol that collides with anything.
//
// WHAT THIS BLOCK ACTUALLY DOES: cbTok*/cbCfg* are pure reads of state the
// shared code already computed. cbCoreInit, cbTokensInit, cbLoadConfig and
// cbScanTokens are NOT reads - they initialise the config lock, run the
// production scan, and install a whole config generation. The macOS bar calls
// them from its main thread only; cbScanTokens pins the config generation for
// the walk exactly as the Windows worker does, so the day a scan moves off the
// main thread the pinning is already right.

#ifdef CHOCOBAR_PORTABLE

long long cbTokAll(void) { return g_tokAll; }
int cbTokAppCount(void) { return g_appCount; }
const char *cbTokAppName(int i) { return (i >= 0 && i < g_appCount) ? g_appName[i] : ""; }
long long cbTokAppSum(int i) { return (i >= 0 && i < g_appCount) ? tokAggSum(&g_appAgg[i]) : 0; }

int cbTokSourceCount(void) { return g_cfg.tokSrcCount; }

// ---- dashboard data -----------------------------------------------------------------
// The token dashboard is the same board on every platform, so its DATA is shared
// even though its painting is not. These read the aggregates tokDeriveWindows and
// aggRecord already build; a front-end that grew its own totals would disagree
// with the Windows dashboard about the same store.
long long cbTokToday(void) { return g_tokensToday; }
long long cbTokWeek(void) { return g_tokWeek; }
long long cbTokMonth(void) { return g_tokMonth; }

int cbTokModelCount(void) { return g_modelCount; }
// the display label is the raw model id first seen for this "app|model" key
const char *cbTokModelLabel(int i) { return (i >= 0 && i < g_modelCount) ? g_modelLabel[i] : ""; }
long long cbTokModelSum(int i) { return (i >= 0 && i < g_modelCount) ? tokAggSum(&g_modelAgg[i]) : 0; }
int cbTokModelReqs(int i) { return (i >= 0 && i < g_modelCount) ? (int)g_modelAgg[i].req : 0; }

int cbTokAppReqs(int i) { return (i >= 0 && i < g_appCount) ? (int)g_appAgg[i].req : 0; }
long long cbTokAppIn(int i) { return (i >= 0 && i < g_appCount) ? g_appAgg[i].in : 0; }
long long cbTokAppOut(int i) { return (i >= 0 && i < g_appCount) ? g_appAgg[i].out : 0; }
long long cbTokAppCr(int i) { return (i >= 0 && i < g_appCount) ? g_appAgg[i].cr : 0; }
long long cbTokAppCw(int i) { return (i >= 0 && i < g_appCount) ? g_appAgg[i].cw : 0; }

// The heatmap is the per-day totals, oldest first. DASH_MAX_DAYS covers 190 days
// (about 27 weeks); the caller draws the last `weeks * 7` of them.
int cbTokDayCount(void) { return DASH_MAX_DAYS; }
// index 0 = oldest; the last entry is today. Returns the day's total tokens.
long long cbTokDayTotal(int i) {
    if (i < 0 || i >= DASH_MAX_DAYS) return 0;
    return g_dayTot[i];
}

int cbCfgTokensEnabled(void) { return g_cfg.tokensEnabled; }
int cbCfgRescanSec(void) { return g_cfg.tokensRescanSec; }
int cbCfgHeight(void) { return g_cfg.height; }
int cbCfgGap(void) { return g_cfg.gap; }

// The scan, start to finish, for a platform with no worker thread.
//
// The staged records are applied through the PRODUCTION drain, so the
// aggregates are built by exactly the code that builds them on Windows. There is
// no Electron cache on macOS, so the seed is empty and every record counts -
// which is the correct behaviour for a first run with nothing to reconcile
// against.
long long cbScanTokens(void) {
    if (!g_cfgLoaded) return 0;
    // The drain derives its debug duration from this stamp, and the Windows
    // scan starter sets it before its own enabled check - so the master-off
    // path below is stamped too, exactly as it is there.
    g_tokScanStart = GetTickCount();
    if (!g_cfg.tokensEnabled || !g_cfg.tokSrcCount) { tokMasterOffReset(); return 0; }
    // Hold a generation alive for the walk, exactly as the Windows worker does
    // ("a worker must hold a generation alive"): a config reload during the
    // scan must retire, not free, the generation being read.
    const Config *cfg = cfgPin();
    tokPendClear();
    // Re-derive the aggregates from scratch ONLY when this scan is a full
    // re-read (process start, or after a master-off/tokLiveReset). Every scan
    // used to set this, which wiped the aggregates and re-added only what the
    // warm-skipped walk staged - the totals would have collapsed to the last
    // scan's new records exactly once the cursor fast path worked. Windows
    // parity: scanTokenCache sets this only on the seed/reset paths.
    g_tokPendSeedReset = g_tokForceFullRead;
    g_tokScanAdded = 0;
    g_tokInnerToday = -1;
    g_tokInnerTodayValid = 1;
    g_cacheMaxTsScan = 0;
    g_cacheMtimeScan = 0;
    tokLiveSeed(0, 0);
    g_tokScanAdded = tokLiveScan(cfg);
    g_tokScanDone = 1;       // the drain early-returns unless this is set
    tokDrainPending();       // reads g_cfg (debug/rescan) - unpin only after
    cfgUnpin();
    return g_tokScanAdded;
}

void cbTokensInit(void) { tokLiveInit(); }
// Config load, mirroring p_ui.c's loadConfig minus the Windows-only tail (there
// is no Run value to register, and the icon/custom-chip machinery the Windows
// build starts here is not part of the first macOS bar). Everything that decides
// WHAT the numbers mean - readFileUtf8, stripLineComments, jsmn, and
// parseConfigInto, where every default lives - is production code.
//
// KNOWN DIVERGENCE, recorded 2026-10-11: p_ui.c's loadConfig also writes a
// first-run config template when the file is missing; this copy does not, so a
// fresh macOS profile runs on defaults until the user creates a config (the
// Windows one keeps working, since the file shape is identical). Factoring the
// read-strip-parse-install core into one shared function is planned work, not
// something to rush into the same pass as a behaviour fix.
//
// g_cfgGen0 is zero-filled, so a load that can neither read nor parse the file
// must still leave a generation installed, or the whole bar runs on a zeroed
// config (tokens off, gap 0, no scan) while the log line above claims defaults.
// parseConfigInto is where every default lives, so the defaults generation is
// that same parser fed an empty document; a generation already installed is
// kept, which is what the "keeping previous" log line below promises.
static int cfgDefaultsInstall(void) {
    if (g_cfgLoaded) return 1;
    static const char empty[] = "{}";
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = jsmn_parse(&parser, empty, sizeof(empty) - 1, NULL, 0);
    if (ntok < 0) return 0;
    jsmntok_t *toks = (jsmntok_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(jsmntok_t) * (ntok + 1));
    if (!toks) return 0;
    jsmn_init(&parser);
    jsmn_parse(&parser, empty, sizeof(empty) - 1, toks, (unsigned int)ntok);
    Config *next = (Config *)HeapAlloc(GetProcessHeap(), 0, sizeof(Config));
    if (!next) { HeapFree(GetProcessHeap(), 0, toks); return 0; }
    parseConfigInto(next, empty, toks, 0);
    HeapFree(GetProcessHeap(), 0, toks);
    cfgInstall(next);
    g_cfgLoaded = 1;
    return 1;
}

int cbLoadConfig(const wchar_t *path) {
    int len = 0;
    char *raw = readFileUtf8(path, &len);
    if (!raw) {
        writeLogA("[wizbar] mac: no config at the configured path - running on defaults");
        return cfgDefaultsInstall();
    }
    stripLineComments(raw);
    jsmn_parser parser;
    jsmn_init(&parser);
    // The counting pass only sizes the array; the fill pass is the binding
    // verdict, because it is the one that rejects what the counter tolerates
    // (an unmatched bracket, a primitive used as a key). Handing a
    // partially-filled array to parseConfigInto walks uninitialised tokens, so
    // its return value decides, and a failed count simply sizes the array to
    // one slot for the fill pass to reject again.
    int ntok = jsmn_parse(&parser, raw, (size_t)len, NULL, 0);
    if (ntok < 0) ntok = 0;
    jsmntok_t *toks = (jsmntok_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(jsmntok_t) * (ntok + 1));
    if (!toks) { HeapFree(GetProcessHeap(), 0, raw); return 0; }
    jsmn_init(&parser);
    ntok = jsmn_parse(&parser, raw, (size_t)len, toks, (unsigned int)ntok);
    if (ntok < 0) {
        char lb[96];
        sprintf(lb, "[wizbar] mac: config parse failed (jsmn %d) - keeping previous", ntok);
        writeLogA(lb);
        HeapFree(GetProcessHeap(), 0, toks);
        HeapFree(GetProcessHeap(), 0, raw);
        return cfgDefaultsInstall();
    }
    Config *next = (Config *)HeapAlloc(GetProcessHeap(), 0, sizeof(Config));
    if (!next) { HeapFree(GetProcessHeap(), 0, toks); HeapFree(GetProcessHeap(), 0, raw); return 0; }
    parseConfigInto(next, raw, toks, 0);
    HeapFree(GetProcessHeap(), 0, toks);
    HeapFree(GetProcessHeap(), 0, raw);
    cfgInstall(next);
    g_cfgLoaded = 1;
    return 1;
}

// The macOS bar has no window message loop, so the drain's notify is a no-op:
// the bar's timer reads the aggregates directly.
void tokScanFinishedNotify(void) { }

// Startup: the config-generation lock is static in the core TU and the Windows
// entry point initialises it in wWinMain before anything reads config state.
// The macOS bar must do the same before its first cbLoadConfig, or cfgInstall's
// EnterCriticalSection would lock an uninitialised mutex. Call once, at launch.
void cbCoreInit(void) { InitializeCriticalSection(&g_cfgGenLock); }

// ---- front-end accessors (Linux bar) --------------------------------------
// The Linux bar paints the same strip from the same config, so it needs the
// geometry, the palette and the module switches. These are reads of g_cfg
// (the installed generation), like the cbCfg* block above; strings are the
// config's own pointers, valid while that generation lives, and the bar copies
// what it keeps.
//
// WHY A FLAT LIST OF GETTERS AND NOT THE STRUCT: Config's layout belongs to the
// core and changes with it. A getter cannot drift out of step with a field
// rename the way a mirrored struct would, and it keeps the front-ends from
// reaching into the parser's types.
int cbCfgFontSize(void) { return g_cfg.fontSize; }
int cbCfgAlign(void) { return g_cfg.barAlign; }
int cbCfgBarRadius(void) { return g_cfg.barRadius; }
int cbCfgBgAlpha(void) { return g_cfg.backgroundAlpha; }
int cbCfgIconOpacity(void) { return g_cfg.iconOpacity; }
const wchar_t *cbCfgTint(void) { return g_cfg.tint; }
const wchar_t *cbCfgFontFamily(void) { return g_cfg.fontFamily; }
const wchar_t *cbCfgClockFormat(void) { return g_cfg.clockFormat; }
const wchar_t *cbCfgFg(void) { return g_cfg.fg; }
const wchar_t *cbCfgFgDim(void) { return g_cfg.fgDim; }
const wchar_t *cbCfgPink(void) { return g_cfg.pink; }
const wchar_t *cbCfgPinkDeep(void) { return g_cfg.pinkDeep; }
const wchar_t *cbCfgWarn(void) { return g_cfg.warn; }
const wchar_t *cbCfgGood(void) { return g_cfg.good; }
const wchar_t *cbCfgYellow(void) { return g_cfg.yellow; }
const wchar_t *cbCfgDivider(void) { return g_cfg.divider; }
int cbCfgDashW(void) { return g_cfg.dashW; }
int cbCfgDashH(void) { return g_cfg.dashH; }
const wchar_t *cbCfgPinkBg(void) { return g_cfg.pinkBg; }
// theme.heatmap[5]: the daily-heatmap ramp, light to dark
const wchar_t *cbCfgHeat(int i) { return (i >= 0 && i < 5) ? g_cfg.heatmap[i] : L""; }
const wchar_t *cbCfgIconColor(void) { return g_cfg.iconColor; }
// module switches: which chips the bar shows at all
int cbCfgModCpu(void) { return g_cfg.mCpu; }
int cbCfgModRam(void) { return g_cfg.mRam; }
int cbCfgModTemp(void) { return g_cfg.mCpuTemp; }
int cbCfgModVol(void) { return g_cfg.mVolume; }
int cbCfgModBatt(void) { return g_cfg.mBattery; }
int cbCfgModClock(void) { return g_cfg.mClock; }
int cbCfgModGpu(void) { return g_cfg.mGpu; }
// warn thresholds (0 or negative = the threshold is off, as on Windows)
int cbCfgCpuWarnAt(void) { return g_cfg.cpuWarnAt; }
int cbCfgRamWarnAt(void) { return g_cfg.ramWarnAt; }
int cbCfgTempWarnAt(void) { return g_cfg.tempWarnAt; }

#endif // CHOCOBAR_PORTABLE
