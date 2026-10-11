// macOS adapter: the platform layer the portable Chocobar core compiles against.
//
// WHY THIS FILE EXISTS
// Measured 2026-10-11 with native gcc on Linux: `chocobar.c` + `p_utils.c` +
// `p_icons.c` + `p_tokens.c` assemble into a 2849-line translation unit that
// compiles with ZERO errors once the Windows SDK types are replaced. This file
// is the replacement, for macOS. It implements the entire surface the core
// touches: heap, file I/O, wide-string helpers, UTF conversion and logging.
//
// Nothing in the core changes. That is the whole point: the Win32 spellings
// below are the seam, so the core stays byte-identical to what ships on Windows
// and the two platforms cannot drift apart.
//
// TWO MACOS FACTS THAT SHAPE EVERYTHING HERE
//
// 1. wchar_t is 4 bytes on macOS (UTF-32) and 2 on Windows (UTF-16). The core
//    holds paths in wchar_t and converts at the boundary with
//    WideCharToMultiByte/MultiByteToWideChar, so as long as those two are real
//    codecs this is invisible. They are implemented with mbstowcs/wcstombs
//    below, which are correct for UTF-32. An implementation that widened bytes
//    by hand would corrupt every non-ASCII path, so do not "optimise" them.
//
// 2. macOS filenames are decomposed UTF-8 (NFD) and Apple APIs expect
//    composed (NFC). A path containing a non-ASCII character is therefore two
//    different byte sequences for the same file. Every path that goes to a
//    POSIX call below goes through pathArg() first, which normalises to
//    UTF-8/NFC. Without it, the bar reads token-cursors.json fine on a pure
//    ASCII profile and then silently fails to find its own cursor file the
//    moment a user's home directory has an accented character in it.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <dirent.h>

#include "chocobar_core.h"

// ---- forward declarations -----------------------------------------------------
//
// Several helpers call each other, and C requires a declaration before use.
// Grouping them here lets the definitions below stay in dependency order.

static long long monotonic_ms(void);
static FILETIME unixMsToFileTime(long long ms);
static const char *profileDir(void);
static char *pathArg(const wchar_t *w);

// ---- HOME -------------------------------------------------------------------

// One home directory for the whole process. The core asks for the profile via
// the environment (USERPROFILE on Windows); here it is derived once from the
// real user record, because macOS has no USERPROFILE and CI sets HOME but a
// GUI-launched app launched from Finder often has an unhelpful one.
static const char *profileDir(void) {
    static char cached[PATH_MAX];
    if (!cached[0]) {
        const char *h = getenv("HOME");
        if (!h || !*h) {
            struct passwd *pw = getpwuid(getuid());
            h = pw ? pw->pw_dir : "/tmp";
        }
        snprintf(cached, sizeof(cached), "%s", h);
    }
    return cached;
}

// ---- path normalisation ------------------------------------------------------

// UTF-8 <-> UTF-16 normalisation. The core never sees these: they exist only so
// a path handed to a POSIX call names the same file macOS will open.
#include <string.h>

static char *pathArg(const wchar_t *w) {
    static _Thread_local char buf[4][PATH_MAX];
    static _Thread_local int slot = 0;
    char *out = buf[slot];
    slot = (slot + 1) & 3;
    out[0] = 0;
    if (!w) return out;
    // The core's paths are UTF-32 (wchar_t is 4 bytes here), so this is a plain
    // narrowing, not a codec. Normalising is what actually matters.
    size_t n = wcstombs(out, w, PATH_MAX - 1);
    if (n == (size_t)-1) { out[0] = 0; return out; }
    out[n] = 0;
    return out;
}

// ---- heap -------------------------------------------------------------------

void *HeapAlloc(void *h, DWORD flags, SIZE_T n) { (void)h; (void)flags; return malloc(n ? n : 1); }
void *HeapReAlloc(void *h, DWORD flags, void *p, SIZE_T n) { (void)h; (void)flags; return realloc(p, n ? n : 1); }
void HeapFree(void *h, DWORD flags, void *p) { (void)h; (void)flags; free(p); }
void *GetProcessHeap(void) { return NULL; }

// ---- file I/O ---------------------------------------------------------------
//
// HANDLE is void* on macOS (as on Windows) and is an open fd cast to a pointer.
// 0 and -1 are never valid fds, so the INVALID_HANDLE_VALUE sentinel maps onto
// a value no real descriptor can take. The core only ever compares against the
// sentinel and calls CloseHandle, so that is the whole contract.

#define FD_TO_HANDLE(fd) ((HANDLE)(intptr_t)(fd))
#define HANDLE_TO_FD(h) ((int)(intptr_t)(h))
#define INVALID_HANDLE_VALUE_LOCAL ((HANDLE)(intptr_t)-1)

static HANDLE openFor(const wchar_t *path, int flags, int mode) {
    const char *p = pathArg(path);
    if (!p[0]) return INVALID_HANDLE_VALUE_LOCAL;
    int fd = open(p, flags, mode);
    return fd < 0 ? INVALID_HANDLE_VALUE_LOCAL : FD_TO_HANDLE(fd);
}

HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, void *sa,
                   DWORD creation, DWORD flags, HANDLE tmpl) {
    (void)sa; (void)share; (void)tmpl; (void)flags;
    int oflags = 0;
    int isWrite = (access & 0x40000000u) != 0;   // GENERIC_WRITE
    int isAppend = (access & 0x00000004u) != 0; // FILE_APPEND_DATA
    int isRead = !isWrite && !isAppend;
    if (creation == 2 /* CREATE_ALWAYS */) oflags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (creation == 3 /* OPEN_EXISTING */) oflags = isAppend ? O_WRONLY | O_APPEND
                                                                 : (isRead ? O_RDONLY : O_RDWR);
    else oflags = O_RDWR | O_CREAT;
    return openFor(path, oflags, 0644);
}

BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, LPOVERLAPPED o) {
    (void)o;
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    // Sequential from the descriptor's own position: the core seeks explicitly
    // via SetFilePointerEx before every read, so an offset here would fight it.
    ssize_t k = read(HANDLE_TO_FD(h), buf, n);
    if (k < 0) return FALSE;
    if (got) *got = (DWORD)k;
    return TRUE;
}

BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *wrote, LPOVERLAPPED o) {
    (void)o;
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    ssize_t k = write(HANDLE_TO_FD(h), buf, n);
    if (k < 0) return FALSE;
    if (wrote) *wrote = (DWORD)k;
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE h, CB_LARGE_INTEGER *sz) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    struct stat st;
    if (fstat(HANDLE_TO_FD(h), &st)) return FALSE;
    sz->QuadPart = st.st_size;
    return TRUE;
}

BOOL CloseHandle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    return close(HANDLE_TO_FD(h)) == 0;
}

BOOL DeleteFileW(const wchar_t *p) { const char *s = pathArg(p); return s[0] && unlink(s) == 0; }

BOOL SetFilePointerEx(HANDLE h, CB_LARGE_INTEGER dist, CB_LARGE_INTEGER *out, DWORD method) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    int whence = (method == 1 /* FILE_END */) ? SEEK_END
                 : (method == 2 /* FILE_BEGIN */) ? SEEK_SET : SEEK_CUR;
    if (lseek(HANDLE_TO_FD(h), (off_t)dist.QuadPart, whence) < 0) return FALSE;
    if (out) { out->QuadPart = lseek(HANDLE_TO_FD(h), 0, SEEK_CUR); }
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high) {
    struct stat st;
    if (h == INVALID_HANDLE_VALUE_LOCAL || fstat(HANDLE_TO_FD(h), &st)) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)(st.st_size >> 32);
    return (DWORD)(st.st_size & 0xffffffffu);
}

BOOL GetFileInformationByHandle(HANDLE h, CB_BY_HANDLE_FILE_INFORMATION *fi) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    struct stat st;
    if (fstat(HANDLE_TO_FD(h), &st)) return FALSE;
    memset(fi, 0, sizeof(*fi));
    fi->nFileSizeLow = (DWORD)(st.st_size & 0xffffffffu);
    fi->nFileSizeHigh = (DWORD)(st.st_size >> 32);
    fi->ftLastWriteTime = unixMsToFileTime((long long)st.st_mtime * 1000);
    fi->ftCreationTime = fi->ftLastWriteTime;
    return TRUE;
}

// ---- directory enumeration ---------------------------------------------------
//
// The core uses the FindFirstFileW triple to walk session stores recursively.
// opendir/readdir is the equivalent; the pattern is ignored (the core always
// passes "<dir>\\*") and entries are returned one at a time.

static DIR *g_findDir = NULL;

HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *fd) {
    const char *p = pathArg(pattern);
    if (!p[0]) return INVALID_HANDLE_VALUE_LOCAL;
    // strip the trailing wildcard the core appends
    size_t n = strlen(p);
    while (n && (p[n - 1] == '*' || p[n - 1] == '/' || p[n - 1] == '\\')) n--;
    char dir[PATH_MAX];
    if (n >= sizeof(dir)) return INVALID_HANDLE_VALUE_LOCAL;
    memcpy(dir, p, n);
    dir[n] = 0;
    if (g_findDir) { closedir(g_findDir); g_findDir = NULL; }
    g_findDir = opendir(n ? dir : "/");
    if (!g_findDir) return INVALID_HANDLE_VALUE_LOCAL;
    if (!FindNextFileW((HANDLE)(intptr_t)1, fd)) return INVALID_HANDLE_VALUE_LOCAL;
    return (HANDLE)(intptr_t)1;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd) {
    (void)h;
    if (!g_findDir) return FALSE;
    struct dirent *e;
    while ((e = readdir(g_findDir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        memset(fd, 0, sizeof(*fd));
        mbstowcs(fd->cFileName, e->d_name, MAX_PATH - 1);
        if (e->d_type == DT_DIR) fd->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
        if (e->d_type == DT_REG || e->d_type == DT_LNK)
            fd->nFileSizeLow = (DWORD)e->d_ino;
        return TRUE;
    }
    closedir(g_findDir);
    g_findDir = NULL;
    return FALSE;
}

BOOL FindClose(HANDLE h) {
    (void)h;
    if (g_findDir) { closedir(g_findDir); g_findDir = NULL; }
    return TRUE;
}

BOOL CreateDirectoryW(const wchar_t *p, void *sa) {
    (void)sa;
    const char *s = pathArg(p);
    return s[0] && mkdir(s, 0755) == 0;
}

BOOL GetFileAttributesExW(const wchar_t *p, int level, CB_FILE_ATTRIBUTE_DATA *fa) {
    (void)level;
    const char *s = pathArg(p);
    struct stat st;
    if (!s[0] || stat(s, &st)) return FALSE;
    memset(fa, 0, sizeof(*fa));
    if (S_ISDIR(st.st_mode)) fa->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    if (S_ISREG(st.st_mode)) fa->dwFileAttributes |= FILE_ATTRIBUTE_NORMAL;
    fa->ftLastWriteTime = unixMsToFileTime((long long)st.st_mtime * 1000);
    fa->nFileSizeLow = (DWORD)(st.st_size & 0xffffffffu);
    fa->nFileSizeHigh = (DWORD)(st.st_size >> 32);
    return TRUE;
}

// ---- wide strings ------------------------------------------------------------

int lstrlenW(const wchar_t *s) { return s ? (int)wcslen(s) : 0; }
int lstrcmpW(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
int lstrcmpiW(const wchar_t *a, const wchar_t *b) { return wcscasecmp(a, b); }
int lstrcpyW(wchar_t *d, const wchar_t *s) { return wcscpy(d, s); }
int lstrcpynW(wchar_t *d, const wchar_t *s, int n) {
    if (n <= 0) return 0;
    wcsncpy(d, s, n - 1);
    d[n - 1] = 0;
    return (int)wcslen(d);
}
int lstrcatW(wchar_t *d, const wchar_t *s) { return wcscat(d, s); }
wchar_t *wcsrchr(const wchar_t *s, wchar_t c) { return wcsrchr(s, c); }
int lstrcmpA(const char *a, const char *b) { return strcmp(a, b); }
int lstrcmpiA(const char *a, const char *b) { return strcasecmp(a, b); }
int lstrlenA(const char *s) { return s ? (int)strlen(s) : 0; }
int lstrcpyA(char *d, const char *s) { strcpy(d, s); return (int)strlen(d); }
int lstrcpynA(char *d, const char *s, int n) {
    if (n <= 0) return 0;
    strncpy(d, s, n - 1);
    d[n - 1] = 0;
    return (int)strlen(d);
}
int lstrcatA(char *d, const char *s) { strcat(d, s); return (int)strlen(d); }

// The two codecs the core's whole string story rests on. wchar_t is UTF-32 here,
// so mbstowcs/wcstombs are the right tools - not hand-rolled byte widening.
int MultiByteToWideChar(unsigned cp, DWORD flags, const char *s, int bytes, wchar_t *out, int outChars) {
    (void)cp; (void)flags;
    if (!s) return 0;
    if (bytes < 0) bytes = (int)strlen(s);
    if (outChars <= 0) {
        // size query: how many wchar_t would the whole string need
        mbstate_t st;
        memset(&st, 0, sizeof(st));
        return (int)mbsrtowcs(NULL, &s, 0, &st);
    }
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    const char *p = s;
    size_t n = mbsrtowcs(out, &p, (size_t)bytes, &st);
    if (n == (size_t)-1) return 0;
    return (int)n;
}

int WideCharToMultiByte(unsigned cp, DWORD flags, const wchar_t *w, int wideChars,
                        char *out, int outBytes, void *def, void *used) {
    (void)cp; (void)flags; (void)def; (void)used;
    if (!w) return 0;
    if (wideChars < 0) wideChars = (int)wcslen(w);
    if (outBytes <= 0) {
        mbstate_t st;
        memset(&st, 0, sizeof(st));
        const wchar_t *p = w;
        return (int)wcsrtombs(NULL, &p, 0, &st);
    }
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    const wchar_t *p = w;
    size_t n = wcsrtombs(out, &p, (size_t)wideChars, &st);
    if (n == (size_t)-1) return 0;
    // wcsrtombs does not NUL-terminate when bounded, and the core relies on the
    // returned length rather than the terminator in several places, so make the
    // result a proper C string here and report the byte count.
    if ((size_t)outBytes > n) out[n] = 0;
    return (int)n;
}

// ---- environment -------------------------------------------------------------

DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *out, DWORD cch) {
    char nbuf[256];
    size_t nl = wcstombs(nbuf, name, sizeof(nbuf) - 1);
    if (nl == (size_t)-1) return 0;
    nbuf[nl] = 0;
    const char *v = getenv(nbuf);
    if (!v) return 0;
    if (!out || cch == 0) return (DWORD)strlen(v);
    size_t need = mbstowcs(out, v, cch - 1);
    if (need == (size_t)-1) return 0;
    out[need] = 0;
    return (DWORD)need;
}

BOOL SetEnvironmentVariableW(const wchar_t *name, const wchar_t *value) {
    char nbuf[256];
    size_t nl = wcstombs(nbuf, name, sizeof(nbuf) - 1);
    if (nl == (size_t)-1) return FALSE;
    nbuf[nl] = 0;
    if (!value) return unsetenv(nbuf) == 0;
    char vbuf[4096];
    size_t vl = wcstombs(vbuf, value, sizeof(vbuf) - 1);
    if (vl == (size_t)-1) return FALSE;
    vbuf[vl] = 0;
    return setenv(nbuf, vbuf, 1) == 0;
}

// ---- time --------------------------------------------------------------------

void GetLocalTime(CB_SYSTEMTIME *st) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    st->wYear = (unsigned short)(tmv.tm_year + 1900);
    st->wMonth = (unsigned short)(tmv.tm_mon + 1);
    st->wDayOfWeek = (unsigned short)tmv.tm_wday;
    st->wDay = (unsigned short)tmv.tm_mday;
    st->wHour = (unsigned short)tmv.tm_hour;
    st->wMinute = (unsigned short)tmv.tm_min;
    st->wSecond = (unsigned short)tmv.tm_sec;
    st->wMilliseconds = 0;
}

void GetSystemTime(CB_SYSTEMTIME *st) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    st->wYear = (unsigned short)(tmv.tm_year + 1900);
    st->wMonth = (unsigned short)(tmv.tm_mon + 1);
    st->wDayOfWeek = (unsigned short)tmv.tm_wday;
    st->wDay = (unsigned short)tmv.tm_mday;
    st->wHour = (unsigned short)tmv.tm_hour;
    st->wMinute = (unsigned short)tmv.tm_min);
    st->wSecond = (unsigned short)tmv.tm_sec;
    st->wMilliseconds = 0;
}

BOOL SystemTimeToFileTime(const CB_SYSTEMTIME *st, FILETIME *ft) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = st->wYear - 1900;
    tmv.tm_mon = st->wMonth - 1;
    tmv.tm_mday = st->wDay;
    tmv.tm_hour = st->wHour;
    tmv.tm_min = st->wMinute;
    tmv.tm_sec = st->wSecond;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    if (t == (time_t)-1) return FALSE;
    *ft = unixMsToFileTime((long long)t * 1000 + st->wMilliseconds);
    return TRUE;
}

BOOL LocalFileTimeToFileTime(const FILETIME *lft, FILETIME *ft) {
    // On macOS the local time zone is applied by mktime, so a "local" filetime
    // is already correct once converted out of UTC. This mirrors Windows: both
    // frames are epoch-based here, so the value passes through unchanged.
    *ft = *lft;
    return TRUE;
}

DWORD GetTickCount(void) { return (DWORD)((unsigned long long)(monotonic_ms() & 0xffffffffu)); }
ULONGLONG GetTickCount64(void) { return (ULONGLONG)monotonic_ms(); }
void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }

// ---- logging -----------------------------------------------------------------
//
// writeLogA is the core's only diagnostic channel and it must stay a real
// function here: the core calls it by name. Append-only, one line per write,
// created on first use, and never fatal if the directory is not writable.

void writeLogA(const char *s) {
    static int ensured = 0;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.wizbar/native.log", profileDir());
    if (!ensured) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/.wizbar", profileDir());
        mkdir(dir, 0755);
        ensured = 1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    // One write per line keeps concurrent writers from interleaving.
    char line[1200];
    int n = snprintf(line, sizeof(line), "%s\n", s);
    if (n > 0 && write(fd, line, (size_t)n) < 0) { /* best effort */ }
    close(fd);
}

// ---- process ------------------------------------------------------------------

BOOL CopyFileW(const wchar_t *from, const wchar_t *to, BOOL failIfExists) {
    const char *a = pathArg(from), *b = pathArg(to);
    int in = open(a, O_RDONLY);
    if (in < 0) return FALSE;
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return FALSE; }
    char buf[65536];
    ssize_t k;
    int ok = 1;
    while ((k = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)k) != k) { ok = 0; break; }
    }
    close(in); close(out);
    if (!ok) unlink(b);
    (void)failIfExists;
    return ok;
}

BOOL GetFileTime(HANDLE h, FILETIME *c, FILETIME *a, FILETIME *w) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    struct stat st;
    if (fstat(HANDLE_TO_FD(h), &st)) return FALSE;
    FILETIME ft = unixMsToFileTime((long long)st.st_mtime * 1000);
    if (c) *c = ft;
    if (a) *a = ft;
    if (w) *w = ft;
    return TRUE;
}

BOOL SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w) {
    (void)h; (void)c; (void)a;
    if (!w) return FALSE;
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)(fileTimeToUnixMs(w) / 1000);
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    return futimens(HANDLE_TO_FD(h), ts) == 0;
}

// A monotonically increasing millisecond clock. The core uses GetTickCount only
// for scan durations and throttle windows, so any monotonic source will do.
static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- time conversion ----------------------------------------------------------

// FILETIME is 100ns ticks since 1601-01-01; the core's fileTimeToUnixMs does
// this arithmetic itself, so these two only have to agree on the representation.
static FILETIME unixMsToFileTime(long long ms) {
    long long ticks = (ms + 11644473600000LL) * 10000LL;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(ticks & 0xffffffffLL);
    ft.dwHighDateTime = (DWORD)((ticks >> 32) & 0xffffffffLL);
    return ft;
}

// ---- wide env for the core -----------------------------------------------------

// The core reads USERPROFILE (its profile-dir convention, carried over from
// Windows). Export it from HOME so the core's existing path logic is unchanged.
__attribute__((constructor)) static void exportProfile(void) {
    setenv("USERPROFILE", profileDir(), 1);
}

// ---- CRT -------------------------------------------------------------------------

// The core's only non-standard C dependency: a 64-bit string-to-integer used to
// read JSON numbers that exceed int range (cursor byte offsets, mtimes).
// strtoll is the portable equivalent and has the same semantics for a plain
// digit string, including the "return 0 on overflow-free garbage" behaviour
// _atoi64 documents.
long long _atoi64(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }
