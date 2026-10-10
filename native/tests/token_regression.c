/* Console-only entry for the production parser, scan worker and UI drain.
   Never calls GUI startup, autostart, quota fetches or update checks. */
#define wWinMain chocobar_product_entry
#include "../src/chocobar_full.c"
#undef wWinMain

#define TEST_SCANS 4

typedef struct {
    long long total, appTotal, modelTotal, calls;
    int models, bytes;
} TestScan;

static int testConfig(const wchar_t *profile) {
    wchar_t path[MAX_PATH];
    if (swprintf(path, MAX_PATH, L"%ls\\.wizbar\\config.json", profile) <= 0) return 1;
    int len = 0;
    char *raw = readFileUtf8(path, &len);
    if (!raw) return 2;
    stripLineComments(raw);
    jsmn_parser p; jsmn_init(&p);
    int nt = jsmn_parse(&p, raw, (size_t)len, NULL, 0);
    if (nt <= 0) { HeapFree(GetProcessHeap(), 0, raw); return 3; }
    jsmntok_t *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t) * (nt + 1));
    if (!t) { HeapFree(GetProcessHeap(), 0, raw); return 4; }
    jsmn_init(&p);
    int parsed = jsmn_parse(&p, raw, (size_t)len, t, (unsigned)nt);
    int rc = 0;
    if (parsed != nt || t[0].type != JSMN_OBJECT) rc = 3;
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

static int testEnableSource(const wchar_t *profile) {
    wchar_t path[MAX_PATH], src[MAX_PATH];
    if (swprintf(path, MAX_PATH, L"%ls\\.wizbar\\config.json", profile) <= 0 ||
        swprintf(src, MAX_PATH, L"%ls\\config-on.json", profile) <= 0 || !CopyFileW(src, path, FALSE)) return 9;
    freeConfig(&g_cfg);
    return testConfig(profile);
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
    if (!refresh && !boundary && !append && !toggle && !sourceToggle && wcscmp(mode, L"normal") != 0) return 7;
    int n = toggle ? 4 : (refresh || boundary || append || sourceToggle) ? 3 : 2;
    for (int pass = 0; pass < n; pass++) {
        if (toggle) g_cfg.tokensEnabled = pass == 1 ? 0 : 1;
        int before = g_tokDbgRead;
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
        for (int i = 0; i < g_appCount; i++) {
            scans[pass].appTotal += tokAggSum(&g_appAgg[i]);
            scans[pass].calls += g_appAgg[i].req;
        }
        for (int i = 0; i < g_modelCount; i++) scans[pass].modelTotal += tokAggSum(&g_modelAgg[i]);
        *count = pass + 1;
        if (pass == 0 && (refresh || boundary)) rc = testSeedChange(profile, boundary);
        if (pass == 0 && append) rc = testAppend(profile);
        if (pass == 0 && sourceToggle) rc = testEnableSource(profile);
        if (rc) return rc;
    }
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 4) return 64;
    FILE *out = _wfopen(argv[2], L"wb");
    if (!out) return 65;
    TestScan scans[TEST_SCANS] = {0};
    int count = 0;
    int rc = testRun(argv[1], argv[3], scans, &count);
    fprintf(out, "{\"exit\":%d,\"scans\":[", rc);
    for (int i = 0; i < count; i++) {
        TestScan *s = &scans[i];
        fprintf(out, "%s{\"total\":%lld,\"app_total\":%lld,\"model_total\":%lld,\"calls\":%lld,\"models\":%d,\"bytes\":%d}",
                i ? "," : "", s->total, s->appTotal, s->modelTotal, s->calls, s->models, s->bytes);
    }
    fprintf(out, "]}\n");
    if (fclose(out) != 0) return 66;
    return rc;
}
