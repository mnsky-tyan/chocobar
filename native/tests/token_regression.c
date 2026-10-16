/* Console-only entry for the production parser, scan worker and UI drain.
   Never calls GUI startup, autostart, quota fetches or update checks. */
#define wWinMain chocobar_product_entry
#include "../src/chocobar_full.c"
#undef wWinMain

#define TEST_SCANS 4

typedef struct {
    long long total, appTotal, modelTotal, calls;
    int models;
    int apps;
    long long bytes;
} TestScan;

/* count -> alloc -> fill, the parse dance every probe needs. NULL means the
   text is not JSON (or the token array could not be allocated); the caller
   keeps ownership of `raw` and frees the returned tokens. */
static jsmntok_t *testParseJson(const char *raw, int len, int *outN) {
    jsmn_parser p; jsmn_init(&p);
    int nt = jsmn_parse(&p, raw, (size_t)len, NULL, 0);
    if (nt <= 0) return NULL;
    jsmntok_t *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t) * (size_t)(nt + 1));
    if (!t) return NULL;
    jsmn_init(&p);
    if (jsmn_parse(&p, raw, (size_t)len, t, (unsigned)nt) != nt) { HeapFree(GetProcessHeap(), 0, t); return NULL; }
    *outN = nt;
    return t;
}

static int testConfig(const wchar_t *profile) {
    wchar_t path[MAX_PATH];
    if (swprintf(path, MAX_PATH, L"%ls\\.wizbar\\config.json", profile) <= 0) return 1;
    int len = 0;
    char *raw = readFileUtf8(path, &len);
    if (!raw) return 2;
    stripLineComments(raw);
    int nt = 0;
    jsmntok_t *t = testParseJson(raw, len, &nt);
    int rc = 0;
    if (!t || t[0].type != JSMN_OBJECT) rc = 3;
    else parseConfigInto(&g_cfg, raw, t, 0);
    HeapFree(GetProcessHeap(), 0, t);
    HeapFree(GetProcessHeap(), 0, raw);
    if (!rc) g_cfgLoaded = 1;
    return rc;
}

/* Change only the seed stamp. For boundary tests replace its contents while
   keeping its mtime older than the source, so the mtime exclusion is not tested. */
static int testSeedChange(const wchar_t *profile, int replace) {
    wchar_t path[MAX_PATH];
    if (swprintf(path, MAX_PATH, L"%ls\\.wizbar\\token-cache.json", profile) <= 0) return 5;
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 5;
    FILETIME ft;
    BOOL ok = GetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    if (!ok) return 5;
    if (replace) {
        wchar_t next[MAX_PATH];
        if (swprintf(next, MAX_PATH, L"%ls\\seed-next.json", profile) <= 0 || !CopyFileW(next, path, FALSE)) return 5;
    }
    ULARGE_INTEGER q;
    q.LowPart = ft.dwLowDateTime; q.HighPart = ft.dwHighDateTime;
    q.QuadPart += 10000000ULL;
    ft.dwLowDateTime = q.LowPart; ft.dwHighDateTime = q.HighPart;
    h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 5;
    ok = SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ok ? 0 : 5;
}

static int testAppend(const wchar_t *profile) {
    wchar_t path[MAX_PATH], src[MAX_PATH];
    if (swprintf(path, MAX_PATH, L"%ls\\sessions\\usage.jsonl", profile) <= 0 ||
        swprintf(src, MAX_PATH, L"%ls\\append-record.json", profile) <= 0) return 6;
    int len = 0;
    char *raw = readFileUtf8(src, &len);
    if (!raw) return 6;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
    DWORD wrote = 0;
    BOOL ok = h != INVALID_HANDLE_VALUE && WriteFile(h, raw, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, raw);
    return ok ? 0 : 6;
}

/* Replace the live config from profile/config-on.json and reparse. Used by
   the source-toggle mode (a source re-enabled mid-run) and the estimate-toggle
   mode (estimateMissingUsage switched on mid-run). */
static int testSwapConfig(const wchar_t *profile) {
    wchar_t path[MAX_PATH], src[MAX_PATH];
    if (swprintf(path, MAX_PATH, L"%ls\\.wizbar\\config.json", profile) <= 0 ||
        swprintf(src, MAX_PATH, L"%ls\\config-on.json", profile) <= 0 || !CopyFileW(src, path, FALSE)) return 9;
    freeConfig(&g_cfg);
    return testConfig(profile);
}

/* The antigravity provider must never invent a credential path. It shipped a
   default pointing at another agent tool's auth store, so a config naming no
   path silently read that file - and a refreshed token is written back to
   whatever path is configured, so a wrong guess rewrites a stranger's store.
   The vendor file is planted in the scratch profile, so a silent fallback is
   observable rather than theoretical. */
static int agyProbeMain(const wchar_t *profile) {
    if (lstrlenW(profile) >= MAX_PATH - 50 || !SetEnvironmentVariableW(L"USERPROFILE", profile)) return 1;
    InitializeCriticalSection(&g_cfgCustomLock);
    InitializeCriticalSection(&g_cfgGenLock);

    /* plant a store the old default would have reached for */
    wchar_t dir[MAX_PATH], sub[MAX_PATH], file[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\.pi", profile); CreateDirectoryW(dir, NULL);
    swprintf(sub, MAX_PATH, L"%ls\\agent", dir); CreateDirectoryW(sub, NULL);
    swprintf(file, MAX_PATH, L"%ls\\auth.json", sub);
    const char *body = "{\"antigravity\":{\"access\":\"ya29.PLANTED\",\"refresh\":\"r1\",\"expires\":9999999999999}}";
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 2;
    DWORD wrote; WriteFile(h, body, (DWORD)strlen(body), &wrote, NULL); CloseHandle(h);
    /* and the store the explicit case names, so that leg proves a positive
       read of exactly the user-named path rather than only a clean miss */
    wchar_t own[MAX_PATH];
    if (swprintf(own, MAX_PATH, L"%ls\\.wizbar\\antigravity.json", profile) <= 0) return 2;
    const char *ownBody = "{\"antigravity\":{\"access\":\"ya29.EXPLICIT\",\"refresh\":\"r2\",\"expires\":9999999999999}}";
    h = CreateFileW(own, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 2;
    WriteFile(h, ownBody, (DWORD)strlen(ownBody), &wrote, NULL); CloseHandle(h);

    struct { const wchar_t *label; const char *json; } cases[] = {
        { L"empty_authPath",  "{\"subs\":{\"enabled\":true,\"providers\":[{\"type\":\"antigravity\",\"enabled\":true,\"authPath\":\"\"}]}}" },
        { L"absent_authPath", "{\"subs\":{\"enabled\":true,\"providers\":[{\"type\":\"antigravity\",\"enabled\":true}]}}" },
        { L"explicit_own",    "{\"subs\":{\"enabled\":true,\"providers\":[{\"type\":\"antigravity\",\"enabled\":true,\"authPath\":\"~/.wizbar/antigravity.json\"}]}}" },
    };
    int rc = 0;
    for (int i = 0; i < 3; i++) {
        int len = (int)strlen(cases[i].json);
        char *raw = HeapAlloc(GetProcessHeap(), 0, len + 1);
        memcpy(raw, cases[i].json, len + 1);
        stripLineComments(raw);
        int nt = 0;
        jsmntok_t *t = testParseJson(raw, (int)strlen(raw), &nt);
        if (!t) { rc = 3; break; }
        parseConfigInto(&g_cfg, raw, t, 0);
        g_cfgLoaded = 1;
        AgyAuth auth; wchar_t resolved[MAX_PATH];
        int found = subsAgyReadAuth(&g_cfg, 0, &auth, resolved, MAX_PATH);
        /* The reader keeps the separator the config used, so the vendor store it
           must never reach is spelled with EITHER separator: a backslash-only
           needle never matched the forward-slash form and left this leg
           asserting nothing. */
        int usedOther = wcsstr(resolved, L"\\.pi\\") != NULL || wcsstr(resolved, L".pi/") != NULL;
        /* the two no-path cases must resolve nothing; the explicit case must
           read exactly the user-named planted store, never the vendor one */
        if (i < 2 && (found || usedOther)) rc = 4;
        wchar_t want[MAX_PATH];
        if (i == 2 && (swprintf(want, MAX_PATH, L"%ls/.wizbar/antigravity.json", profile) <= 0
                       || !found || usedOther || wcscmp(resolved, want) != 0)) rc = 5;
        printf("%ls found=%d resolved=\"%ls\"\n", cases[i].label, found, resolved);
        HeapFree(GetProcessHeap(), 0, t);
        HeapFree(GetProcessHeap(), 0, raw);
    }
    DeleteFileW(file); RemoveDirectoryW(sub); RemoveDirectoryW(dir);
    DeleteFileW(own);
    return rc;
}

/* Omitting a key must keep its built-in default. parseConfigInto installs every
   default, then the per-section loop used to wideFree the field and pass it back
   as jstrTok's own default - so wideFree zeroed it and an absent key erased the
   default (the bar painted black, the font fell back, backdrop=solid stopped
   working). The shipped template promises "delete a key and the built-in default
   applies", so this is the executable form of that promise. */
static int defaultsProbeMain(void) {
    static const char *js = "{\"bar\":{\"height\":30},\"theme\":{},\"modules\":{},\"tokens\":{\"enabled\":false}}";
    int len = (int)strlen(js);
    char *raw = HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (!raw) return 1;
    memcpy(raw, js, len + 1);
    int nt = 0;
    jsmntok_t *t = testParseJson(raw, len, &nt);
    if (!t) { HeapFree(GetProcessHeap(), 0, raw); return 3; }
    Config c; memset(&c, 0, sizeof(c));
    parseConfigInto(&c, raw, t, 0);
    struct { const wchar_t *label; const wchar_t *got; const wchar_t *want; } cases[] = {
        { L"backgroundTint", c.tint,       L"#FBF2E2" },
        { L"backdrop",       c.backdrop,   L"acrylic" },
        { L"fontFamily",     c.fontFamily, L"Cascadia Mono" },
    };
    int rc = 0;
    for (int i = 0; i < 3; i++) {
        int ok = cases[i].got && wcscmp(cases[i].got, cases[i].want) == 0;
        printf("%ls=%ls\n", cases[i].label, cases[i].got ? cases[i].got : L"(NULL)");
        if (!ok) rc = 4;
    }
    printf("verdict=%s\n", rc ? "DEFAULTS_LOST" : "DEFAULTS_KEPT");
    freeConfig(&c);
    HeapFree(GetProcessHeap(), 0, t);
    HeapFree(GetProcessHeap(), 0, raw);
    return rc;
}

static int testRun(const wchar_t *profile, const wchar_t *mode, TestScan *scans, int *count) {
    if (lstrlenW(profile) >= MAX_PATH - 50 || !SetEnvironmentVariableW(L"USERPROFILE", profile)) return 1;
    InitializeCriticalSection(&g_cfgCustomLock);
    InitializeCriticalSection(&g_cfgGenLock);
    tokLiveInit(); /* Product startup ordering: load cursors before config. */
    int rc = testConfig(profile);
    if (rc) return rc;
    int refresh = wcscmp(mode, L"refresh") == 0;
    int boundary = wcscmp(mode, L"boundary") == 0;
    int append = wcscmp(mode, L"append") == 0;
    int toggle = wcscmp(mode, L"toggle") == 0;
    int sourceToggle = wcscmp(mode, L"source-toggle") == 0;
    int estToggle = wcscmp(mode, L"estimate-toggle") == 0;
    if (!refresh && !boundary && !append && !toggle && !sourceToggle && !estToggle && wcscmp(mode, L"normal") != 0) return 7;
    int n = toggle ? 4 : (refresh || boundary || append || sourceToggle || estToggle) ? 3 : 2;
    if (n > TEST_SCANS) return 2; // scans[pass] is written unguarded below
    for (int pass = 0; pass < n; pass++) {
        if (toggle) g_cfg.tokensEnabled = pass == 1 ? 0 : 1;
        long long before = g_tokDbgRead;
        scanTokenCache();
        DWORD start = GetTickCount();
        while (g_cfg.tokensEnabled && InterlockedCompareExchange(&g_tokScanDone, 0, 0) != 1) {
            if (GetTickCount() - start > 10000) return 8;
            Sleep(5);
        }
        scans[pass].bytes = g_tokDbgRead - before;
        tokDrainPending();
        scans[pass].total = g_tokAll;
        scans[pass].models = g_modelCount;
        scans[pass].apps = g_appCount;
        for (int i = 0; i < g_appCount; i++) {
            scans[pass].appTotal += tokAggSum(&g_appAgg[i]);
            scans[pass].calls += g_appAgg[i].req;
        }
        for (int i = 0; i < g_modelCount; i++) scans[pass].modelTotal += tokAggSum(&g_modelAgg[i]);
        *count = pass + 1;
        if (pass == 0 && (refresh || boundary)) rc = testSeedChange(profile, boundary);
        if (pass == 0 && append) rc = testAppend(profile);
        if (pass == 0 && sourceToggle) rc = testSwapConfig(profile);
        /* the estimate-toggle path: append a record while the estimator is off
           (the cursor records no transcript total), then switch it on. The
           next scan must count ONLY the appended record - the cursor-repair
           re-read establishes the transcript total without re-staging rows
           the aggregates already hold. */
        if (pass == 0 && estToggle) { rc = testAppend(profile); if (!rc) rc = testSwapConfig(profile); }
        if (rc) return rc;
    }
    return 0;
}

/* Drive the production scan + UI drain, then report the day bucket the dashboard
   renders, so a timestamp-offset bug that moves a record across midnight is
   observable end to end. The console harness otherwise exposes no day split. */
static int dayProbeMain(const wchar_t *profile) {
    if (lstrlenW(profile) >= MAX_PATH - 50 || !SetEnvironmentVariableW(L"USERPROFILE", profile)) return 1;
    InitializeCriticalSection(&g_cfgCustomLock);
    InitializeCriticalSection(&g_cfgGenLock);
    tokLiveInit();
    int rc = testConfig(profile);
    if (rc) return rc;
    scanTokenCache();
    DWORD start = GetTickCount();
    while (g_cfg.tokensEnabled && InterlockedCompareExchange(&g_tokScanDone, 0, 0) != 1) {
        if (GetTickCount() - start > 10000) return 8;
        Sleep(5);
    }
    tokDrainPending();
    /* g_dayTot runs oldest-first, so the last slot is today, exactly what the
       dashboard's Today card shows (p_ui.c reads the same index). */
    printf("TODAY=%lld\n", g_dayTot[DASH_MAX_DAYS - 1]);
    printf("ALL=%lld\n", g_tokAll);
    fflush(stdout);
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && wcscmp(argv[1], L"--agy-probe") == 0) return agyProbeMain(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"--day-probe") == 0) return dayProbeMain(argv[2]);
    if (argc == 2 && wcscmp(argv[1], L"--defaults-probe") == 0) return defaultsProbeMain();
    if (argc != 4) return 64;
    FILE *out = _wfopen(argv[2], L"wb");
    if (!out) return 65;
    TestScan scans[TEST_SCANS] = {0};
    int count = 0;
    int rc = testRun(argv[1], argv[3], scans, &count);
    fprintf(out, "{\"exit\":%d,\"scans\":[", rc);
    for (int i = 0; i < count; i++) {
        TestScan *s = &scans[i];
        fprintf(out, "%s{\"total\":%lld,\"app_total\":%lld,\"model_total\":%lld,\"calls\":%lld,\"models\":%d,\"apps\":%d,\"bytes\":%lld}",
                i ? "," : "", s->total, s->appTotal, s->modelTotal, s->calls, s->models, s->apps, s->bytes);
    }
    fprintf(out, "]}\n");
    if (fclose(out) != 0) return 66;
    return rc;
}
