// ---------------------------------------------------------------- util ----
static char *readFileUtf8(const wchar_t *path, DWORD *outLen) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); return NULL; }
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)size + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD got = 0;
    ReadFile(h, buf, size, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB) {
        memmove(buf, buf + 3, got - 2);
        got -= 3;
    }
    *outLen = got;
    return buf;
}

// strip // line comments (the config template is JSONC)
static void stripLineComments(char *s) {
    int inStr = 0;
    for (char *p = s; *p; p++) {
        if (*p == '"') inStr = !inStr;
        else if (!inStr && p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') *p++ = ' ';
            if (!*p) break;
        }
    }
}

static wchar_t *utf8ToWide(const char *s, int bytes) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, bytes, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(wchar_t) * (n + 1));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, bytes, w, n);
    w[n] = 0;
    return w;
}

static void wideFree(wchar_t **w) { if (*w) { HeapFree(GetProcessHeap(), 0, *w); *w = NULL; } }

static wchar_t *wideDup(const wchar_t *s) {
    if (!s) return NULL;
    size_t n = wcslen(s) + 1;
    wchar_t *w = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, n * sizeof(wchar_t));
    if (!w) return NULL;
    memcpy(w, s, n * sizeof(wchar_t));
    return w;
}

// "#RRGGBB" -> 0x00BBGGRR (COLORREF); -1 on parse failure
static int hexToColorref(const wchar_t *s) {
    if (!s || *s != L'#' || wcslen(s) < 7) return -1;
    wchar_t *end = NULL;
    long v = wcstol(s + 1, &end, 16);
    if (!end || *end) return -1;
    return (int)(GetRValue((COLORREF)v) | (GetGValue((COLORREF)v) << 8) | (GetBValue((COLORREF)v) << 16));
}
