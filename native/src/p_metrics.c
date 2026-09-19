// ------------------------------------------------------------- metrics ----
static int striContains(const wchar_t *hay, const wchar_t *needle);
typedef struct {
    double cpu, gpu, ram;
    double tempC;
    int tempOk;            // 1 = reading, 0 = dash
    int tempHot;
    int battPct, battAc, battValid;
    int volume;
    int petRunning;
    wchar_t clockText[80];
} Metrics;

static Metrics g_m;

// --- CPU via GetSystemTimes -------------------------------------------------
static ULARGE_INTEGER g_cpuPi, g_cpuPu, g_cpuPk; // previous idle/user/kernel
static int g_cpuHave = 0;

static void pollCpu(void) {
    FILETIME i, u, k;
    if (!GetSystemTimes(&i, &u, &k)) { g_m.cpu = 0; return; }
    ULARGE_INTEGER ci, cu, ck;
    ci.LowPart = i.dwLowDateTime;  ci.HighPart = i.dwHighDateTime;
    cu.LowPart = u.dwLowDateTime;  cu.HighPart = u.dwHighDateTime;
    ck.LowPart = k.dwLowDateTime;  ck.HighPart = k.dwHighDateTime;
    if (g_cpuHave) {
        ULONGLONG di = ci.QuadPart - g_cpuPi.QuadPart;
        ULONGLONG dt = (cu.QuadPart - g_cpuPu.QuadPart) + (ck.QuadPart - g_cpuPk.QuadPart);
        g_m.cpu = dt ? (double)(dt - di) * 100.0 / (double)dt : 0;
        if (g_m.cpu < 0) g_m.cpu = 0;
        if (g_m.cpu > 100) g_m.cpu = 100;
    }
    g_cpuHave = 1;
    g_cpuPi = ci; g_cpuPu = cu; g_cpuPk = ck;
}

// --- RAM via GlobalMemoryStatusEx -------------------------------------------
static void pollRam(void) {
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    g_m.ram = GlobalMemoryStatusEx(&ms) ? (double)ms.dwMemoryLoad : 0;
}

// --- Battery via GetSystemPowerStatus ----------------------------------------
static void pollBattery(void) {
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) { g_m.battValid = 0; return; }
    g_m.battAc = ps.ACLineStatus == 1;
    g_m.battValid = ps.BatteryLifePercent <= 100;
    g_m.battPct = ps.BatteryLifePercent;
}

// --- Volume via Core Audio endpoint ------------------------------------------
static const GUID g_CLSID_MMDeviceEnumerator = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID g_IID_IMMDeviceEnumerator  = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID g_IID_IAudioEndpointVolume = {0x5CDF2C82,0x841E,0x4546,{0x97,0x22,0x0C,0xF7,0x40,0x78,0x22,0x9A}};
static IAudioEndpointVolume *g_vol = NULL;

static void volumeInit(void) {
    IMMDeviceEnumerator *de = NULL;
    if (FAILED(CoCreateInstance(&g_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &g_IID_IMMDeviceEnumerator, (void **)&de))) return;
    IMMDevice *dev = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(de, eRender, eMultimedia, &dev))) {
        IMMDevice_Activate(dev, &g_IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&g_vol);
        IMMDevice_Release(dev);
    }
    IMMDeviceEnumerator_Release(de);
}

static void pollVolume(void) {
    if (!g_vol) return;
    float v = 0;
    if (SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(g_vol, &v)))
        g_m.volume = (int)(v * 100.0f + 0.5f);
}

// --- GPU via PDH GPU Engine counters -----------------------------------------
static PDH_HQUERY g_gpuQuery = NULL;
static PDH_HCOUNTER g_gpuCounter = NULL;
static int g_gpuFirst = 1;

static void gpuInit(void) {
    if (PdhOpenQueryW(NULL, 0, &g_gpuQuery) != ERROR_SUCCESS) return;
    PDH_STATUS st = PdhAddEnglishCounterW(g_gpuQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpuCounter);
    if (st != ERROR_SUCCESS) { g_gpuQuery = NULL; return; }
    PdhCollectQueryData(g_gpuQuery); // baseline sample
}

static void pollGpu(void) {
    if (!g_gpuQuery) { g_m.gpu = 0; return; }
    if (PdhCollectQueryData(g_gpuQuery) != ERROR_SUCCESS) { g_m.gpu = 0; return; }
    if (g_gpuFirst) { g_gpuFirst = 0; g_m.gpu = 0; return; } // need two samples
    DWORD size = 0, count = 0;
    PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE, &size, &count, NULL);
    if (!size) { g_m.gpu = 0; return; }
    PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!items) return;
    double sum = 0;
    if (PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < count; i++)
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS) sum += items[i].FmtValue.doubleValue;
    }
    HeapFree(GetProcessHeap(), 0, items);
    if (sum < 0) sum = 0;
    if (sum > 100) sum = 100;
    g_m.gpu = sum;
}

// --- CPU temperature via HWiNFO shared memory (SM then SM2) -------------------
// Mirrors the Electron reader's parsers exactly (layouts per HWiNFO's SDK).
static int striContains(const wchar_t *hay, const wchar_t *needle) {
    if (!hay || !needle || !*needle) return 0;
    wchar_t h[256], n[64];
    size_t i = 0;
    for (; hay[i] && i < 255; i++) h[i] = towlower(hay[i]);
    h[i] = 0;
    for (i = 0; needle[i] && i < 63; i++) n[i] = towlower(needle[i]);
    n[i] = 0;
    return wcsstr(h, n) != NULL;
}

typedef struct { double c; wchar_t label[64]; wchar_t sensor[64]; } TempRec;

static void pickCpuTemp(TempRec *list, int n, int *ok, double *outC, int *hot, int warnAt) {
    *ok = 0; *hot = 0;
    if (!n) return;
    int pkg = -1, cpuLbl = -1, hottest = -1;
    double hotV = -1;
    for (int i = 0; i < n; i++) {
        int onCpu = striContains(list[i].sensor, L"cpu") != 0;
        if (onCpu && striContains(list[i].label, L"package")) { pkg = i; break; }
        if (onCpu && !striContains(list[i].label, L"distance") &&
            (striContains(list[i].label, L"cpu") || striContains(list[i].label, L"tctl") || striContains(list[i].label, L"tdie"))) {
            if (cpuLbl < 0) cpuLbl = i;
        }
        if (onCpu && !striContains(list[i].label, L"distance") && list[i].c > hotV) { hotV = list[i].c; hottest = i; }
    }
    int pick = pkg >= 0 ? pkg : (cpuLbl >= 0 ? cpuLbl : hottest);
    if (pick < 0) return;
    *outC = list[pick].c;
    *ok = 1;
    *hot = warnAt > 0 && *outC >= warnAt;
}

// SM1: 40-byte header, NUL-terminated sensor array, reading elements
static void pollHwinfoSm(void) {
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM");
    if (!h) return;
    BYTE *p = (BYTE *)MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(h); return; }
    MEMORY_BASIC_INFORMATION mbi;
    DWORD total = 0;
    while (VirtualQuery(p + total, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           (BYTE *)mbi.BaseAddress == p + total && total < 64 * 1024 * 1024)
        total += (DWORD)mbi.RegionSize;
    do {
        if (total < 40 || *(uint32_t *)p != 0x10) break;
        DWORD sensorSize = *(DWORD *)(p + 32), readingSize = *(DWORD *)(p + 36);
        DWORD suffix = (readingSize - 56) / 9;
        if ((suffix != 16 && suffix != 32) || sensorSize != (suffix == 32 ? 512u : 256u)) break;
        int wide = suffix == 32;
        // sensor names
        wchar_t sensors[256][64];
        int sensorCount = 0;
        DWORD off = 40;
        for (int i = 0; i < 256; i++) {
            wchar_t name[64]; name[0] = 0;
            if (wide) { int n = 0; while (n < 63 && off + (DWORD)(2*n + 1) < total && p[off + 2*n]) { name[n] = *(wchar_t *)(p + off + 2*n); n++; } name[n] = 0; }
            else { int n = 0; while (n < 63 && off + n < total && p[off + n]) { name[n] = (wchar_t)p[off + n]; n++; } name[n] = 0; }
            if (!name[0]) break;
            lstrcpynW(sensors[i], name, 64);
            sensorCount = i + 1;
            off += sensorSize;
        }
        (void)sensorCount;
        DWORD labelBytes = suffix * 8;
        TempRec *recs = (TempRec *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TempRec) * 1024);
        if (!recs) break;
        int n = 0;
        for (DWORD start = off; n == 0 && start <= off + sensorSize; start += sensorSize) {
            for (int i = 0; i < 1024 && n < 1024; i++) {
                DWORD ro = start + (DWORD)i * readingSize;
                if (ro + 48 + labelBytes + suffix + 4 > total) break;
                wchar_t label[64]; label[0] = 0;
                if (wide) { int q = 0; while (q < 63 && ro + 48 + (DWORD)(2*q + 1) < total && p[ro + 48 + 2*q]) { label[q] = *(wchar_t *)(p + ro + 48 + 2*q); q++; } label[q] = 0; }
                else { int q = 0; while (q < 63 && ro + 48 + q < total && p[ro + 48 + q]) { label[q] = (wchar_t)p[ro + 48 + q]; q++; } label[q] = 0; }
                if (!label[0]) break; // zero-filled tail
                wchar_t sfx[8]; sfx[0] = 0;
                if (wide) { int q = 0; while (q < 7 && ro + 48 + labelBytes + (DWORD)(2*q + 1) < total && p[ro + 48 + labelBytes + 2*q]) { sfx[q] = *(wchar_t *)(p + ro + 48 + labelBytes + 2*q); q++; } sfx[q] = 0; }
                else { int q = 0; while (q < 7 && ro + 48 + labelBytes + q < total && p[ro + 48 + labelBytes + q]) { sfx[q] = (wchar_t)p[ro + 48 + labelBytes + q]; q++; } sfx[q] = 0; }
                if (!wcsstr(sfx, L"\u00B0")) continue;
                double v = *(double *)(p + ro + 16);
                if (!(v > 0 && v < 150)) continue;
                DWORD id = *(DWORD *)(p + ro + 48 + labelBytes + suffix);
                recs[n].c = (double)(int)(v * 10) / 10;
                lstrcpynW(recs[n].label, label, 64);
                if (id < 256) lstrcpynW(recs[n].sensor, sensors[id], 64);
                n++;
            }
        }
        HeapFree(GetProcessHeap(), 0, recs);
        pickCpuTemp(recs, n, &g_m.tempOk, &g_m.tempC, &g_m.tempHot, g_cfg.tempWarnAt);
    } while (0);
    UnmapViewOfFile(p);
    CloseHandle(h);
}

// SM2 (HWiNFO 8.24+): 48-byte header, byte-oriented latin-1 strings
static void pollHwinfoSm2(void) {
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
    if (!h) return;
    BYTE *p = (BYTE *)MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(h); return; }
    MEMORY_BASIC_INFORMATION mbi;
    DWORD total = 0;
    while (VirtualQuery(p + total, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           (BYTE *)mbi.BaseAddress == p + total && total < 64 * 1024 * 1024)
        total += (DWORD)mbi.RegionSize;
    do {
        if (total < 48 || *(uint32_t *)p != 0x53695748) break; // 'HWiS'
        DWORD sensorSize = *(DWORD *)(p + 24), sensorCount = *(DWORD *)(p + 28);
        DWORD readingsOff = *(DWORD *)(p + 32), readingSize = *(DWORD *)(p + 36), readingCount = *(DWORD *)(p + 40);
        if (sensorSize < 136 || readingSize < 320 || !sensorCount || !readingCount) break;
        if (readingsOff + readingCount * readingSize > total) break;
        TempRec *recs = (TempRec *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TempRec) * (readingCount ? readingCount : 1));
        if (!recs) break;
        int n = 0;
        for (DWORD j = 0; j < readingCount; j++) {
            DWORD rec = readingsOff + j * readingSize;
            char unit[9]; memcpy(unit, p + rec + 268, 8); unit[8] = 0;
            if (unit[0] != (char)0xB0 || unit[1] != 'C') continue; // latin-1 degree sign
            double v = *(double *)(p + rec + 284);
            if (!(v > 0 && v < 150)) continue;
            DWORD id = *(DWORD *)(p + rec);
            recs[n].c = (double)(int)(v * 10) / 10;
            int q = 0;
            for (; q < 63; q++) { char ch = *(char *)(p + rec + 12 + q); if (!ch) break; recs[n].label[q] = (wchar_t)(unsigned char)ch; }
            recs[n].label[q] = 0;
            if (id < sensorCount) {
                q = 0;
                for (; q < 63; q++) { char ch = *(char *)(p + 48 + id * sensorSize + 8 + q); if (!ch) break; recs[n].sensor[q] = (wchar_t)(unsigned char)ch; }
                recs[n].sensor[q] = 0;
            }
            n++;
        }
        pickCpuTemp(recs, n, &g_m.tempOk, &g_m.tempC, &g_m.tempHot, g_cfg.tempWarnAt);
        HeapFree(GetProcessHeap(), 0, recs);
    } while (0);
    UnmapViewOfFile(p);
    CloseHandle(h);
}

// --- Pet presence via Toolhelp32 snapshot ------------------------------------
static wchar_t *pathBaseName(const wchar_t *p) {
    const wchar_t *b = p, *q = p;
    for (; *q; q++) if (*q == L'\\' || *q == L'/') b = q + 1;
    return (wchar_t *)b;
}

static int g_petDiagDone = 0;
static void writeLogA(const char *s); // p_ui
static int petRunning(const wchar_t *exePath) {
    if (!exePath || !*exePath) return 0;
    wchar_t base[MAX_PATH];
    lstrcpynW(base, pathBaseName(exePath), MAX_PATH);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    int found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, base) == 0) { found = 1; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!found && !g_petDiagDone) {   // TEMP
        g_petDiagDone = 1;
        char dbg[160];
        int bl = lstrlenW(base);
        sprintf(dbg, "pet: not found base wlen=%d head=%04X %04X", bl,
                bl > 0 ? (unsigned)base[0] : 0, bl > 1 ? (unsigned)base[1] : 0);
        writeLogA(dbg);
    }
    return found;
}

// --- Clock format ------------------------------------------------------------
static const wchar_t *const kMonths[12] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
static const wchar_t *const kWeekdays[7] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};

// supports {MMM} {dd} {Wkk} {HH} {mm} {ss}; anything else passes through
static void formatClock(const wchar_t *fmt, wchar_t *out, size_t outn) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t *o = out, *oend = out + outn - 1;
    for (const wchar_t *f = fmt; *f && o < oend; f++) {
        if (*f != L'{') { *o++ = *f; continue; }
        const wchar_t *e = wcschr(f, L'}');
        if (!e) { *o++ = *f; continue; }
        wchar_t tok[16];
        size_t n = (size_t)(e - f - 1);
        if (n >= sizeof(tok) / sizeof(wchar_t)) { *o++ = *f; continue; }
        memcpy(tok, f + 1, n * sizeof(wchar_t)); tok[n] = 0;
        const wchar_t *rep = NULL;
        wchar_t num[8]; num[0] = 0;
        if (!wcscmp(tok, L"MMM")) rep = kMonths[st.wMonth - 1];
        else if (!wcscmp(tok, L"Wkk")) rep = kWeekdays[st.wDayOfWeek == 0 ? 6 : st.wDayOfWeek - 1];
        else if (!wcscmp(tok, L"dd")) { swprintf(num, 8, L"%02d", (int)st.wDay); rep = num; }
        else if (!wcscmp(tok, L"HH")) { swprintf(num, 8, L"%02d", (int)st.wHour); rep = num; }
        else if (!wcscmp(tok, L"mm")) { swprintf(num, 8, L"%02d", (int)st.wMinute); rep = num; }
        else if (!wcscmp(tok, L"ss")) { swprintf(num, 8, L"%02d", (int)st.wSecond); rep = num; }
        if (!rep) { *o++ = *f; continue; }
        for (const wchar_t *r = rep; *r && o < oend; r++) *o++ = *r;
        f = e;
    }
    *o = 0;
}
