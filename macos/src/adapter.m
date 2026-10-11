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
// THREE MACOS FACTS THAT SHAPE EVERYTHING HERE
//
// 1. wchar_t is 4 bytes on macOS (UTF-32) and 2 on Windows (UTF-16). The core
//    holds paths in wchar_t and converts at the boundary with
//    WideCharToMultiByte/MultiByteToWideChar, so as long as those two are real
//    codecs this is invisible. They are implemented with mbstowcs/wcstombs
//    below, which are correct for UTF-32 UNDER A UTF-8 LOCALE - see the
//    locale constructor at the bottom of this file, which establishes one.
//    An implementation that widened bytes by hand would corrupt every non-ASCII
//    path, so do not "optimise" them.
//
// 2. The core joins paths with Win32 backslashes (p_tokens.c swprintf
//    "%ls\\%ls", subsPathExpand "~\\.wizbar\\..."). On macOS a backslash is an
//    ordinary filename character, so every one of those paths would be an
//    ENOENT. pathArg() therefore translates separator backslashes to slashes
//    at the POSIX boundary - the last place that still knows both conventions.
//    It does NOT do Unicode normalisation (NFC/NFD): macOS stores decomposed
//    filenames and a profile directory with a non-ASCII name would need
//    CFString normalisation here. That is a recorded limitation, not a
//    protection: pure-ASCII profiles are unaffected, and non-ASCII ones need
//    the normalisation pass added before they can be trusted.
//
// 3. Directory enumeration state lives in the HANDLE, not in a process global.
//    The core walks stores recursively (tokScanDir calls FindFirstFileW inside
//    its own FindNextFileW loop), so a single static DIR* would close the
//    parent's directory on the first nested call and silently drop every entry
//    after it. Each enumeration owns its state; the handle carries it.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
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
static long long adapterFileTimeToUnixMs(const FILETIME *ft);

// FILETIME is 100ns ticks since 1601-01-01. The core has its own static
// fileTimeToUnixMs for the other direction; this one is the adapter's, named
// differently so the two coexist in one binary without shadowing each other.
static long long adapterFileTimeToUnixMs(const FILETIME *ft) {
    unsigned long long ticks = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    return (long long)(ticks / 10000ull) - 11644473600000LL;
}

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

// ---- path boundary ------------------------------------------------------------

// Win32 path -> POSIX path. Translates separator backslashes to slashes; a
// double backslash is collapsed too, so "C:\\x\\y" and "~\\.wizbar" both become
// single-separator POSIX paths. A POSIX filename containing a literal backslash
// cannot be named through this boundary - the core never creates one, because
// every backslash it emits is a separator. Unicode normalisation is NOT done
// here (see the file header, fact 2).
static char *pathArg(const wchar_t *w) {
    static _Thread_local char buf[4][PATH_MAX];
    static _Thread_local int slot = 0;
    char *out = buf[slot];
    slot = (slot + 1) & 3;
    out[0] = 0;
    if (!w) return out;
    // The core's paths are UTF-32 (wchar_t is 4 bytes here), so this is a plain
    // narrowing, not a codec. In a non-UTF-8 locale wcstombs fails on every
    // non-ASCII byte; the constructor at the bottom of this file establishes a
    // UTF-8 locale so this narrowing is exact.
    size_t n = wcstombs(out, w, PATH_MAX - 1);
    if (n == (size_t)-1) { out[0] = 0; return out; }
    out[n] = 0;
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
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
#define INVALID_HANDLE_VALUE_LOCAL INVALID_HANDLE_VALUE

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

CB_BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, LPOVERLAPPED o) {
    (void)o;
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    // Sequential from the descriptor's own position: the core seeks explicitly
    // via SetFilePointerEx before every read, so an offset here would fight it.
    ssize_t k = read(HANDLE_TO_FD(h), buf, n);
    if (k < 0) return FALSE;
    if (got) *got = (DWORD)k;
    return TRUE;
}

CB_BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *wrote, LPOVERLAPPED o) {
    (void)o;
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    // Win32's non-overlapped WriteFile writes the whole buffer; a bare write()
    // can short-write (pipes, signals), and a truncated cursor JSON would
    // silently lose cursors. Loop until the buffer is done or an error lands.
    const char *p = (const char *)buf;
    DWORD left = n;
    while (left > 0) {
        ssize_t k = write(HANDLE_TO_FD(h), p, left);
        if (k < 0) {
            if (errno == EINTR) continue;
            return FALSE;
        }
        if (k == 0) return FALSE;
        p += k; left -= (DWORD)k;
    }
    if (wrote) *wrote = n;
    return TRUE;
}

CB_BOOL GetFileSizeEx(HANDLE h, CB_LARGE_INTEGER *sz) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    struct stat st;
    if (fstat(HANDLE_TO_FD(h), &st)) return FALSE;
    sz->QuadPart = st.st_size;
    return TRUE;
}

CB_BOOL CloseHandle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    return close(HANDLE_TO_FD(h)) == 0;
}

CB_BOOL DeleteFileW(const wchar_t *p) { const char *s = pathArg(p); return (CB_BOOL)(s[0] && unlink(s) == 0); }

CB_BOOL SetFilePointerEx(HANDLE h, CB_LARGE_INTEGER dist, CB_LARGE_INTEGER *out, DWORD method) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    // The header's constants ARE the contract: FILE_BEGIN 0, FILE_CURRENT 1,
    // FILE_END 2. An earlier mapping here was shifted by one for all three
    // (1->SEEK_END, 2->SEEK_SET, 0 falling into SEEK_CUR), which made every
    // warm scan seek to pos+from instead of from - the scanner counted the
    // wrong bytes or gave up. Use the named constants so this cannot drift
    // from the header again.
    int whence = (method == FILE_BEGIN) ? SEEK_SET
                 : (method == FILE_END) ? SEEK_END : SEEK_CUR;
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

CB_BOOL GetFileInformationByHandle(HANDLE h, CB_BY_HANDLE_FILE_INFORMATION *fi) {
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
// The core uses the FindFirstFileW triple to walk session stores RECURSIVELY:
// tokScanDir calls FindFirstFileW from inside its own FindNextFileW loop. The
// enumeration state therefore lives in the handle, never in a global - a
// static DIR* would be closed by the nested call and every parent entry after
// it would be silently dropped. The handle is a heap CB_FIND whose DIR carries
// its directory fd, so each entry's size/mtime comes from fstatat with no
// extra path work.
//
// The core also fills ftLastWriteTime and nFileSizeLow/High straight from the
// enumeration and uses them as the warm-skip test (cursor size+mtime equality)
// - an earlier version left the timestamps zero and stored the inode as the
// size, which made the skip impossible and re-opened every file every rescan.

typedef struct {
    DIR *dir;
} CB_FIND;

HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *fd) {
    const char *p = pathArg(pattern);
    if (!p[0]) return INVALID_HANDLE_VALUE_LOCAL;
    // strip the trailing wildcard the core appends: "<dir>/*" (separators are
    // already translated by pathArg; accept a raw backslash form too)
    size_t n = strlen(p);
    while (n && (p[n - 1] == '*' || p[n - 1] == '/' || p[n - 1] == '\\')) n--;
    char dir[PATH_MAX];
    if (n >= sizeof(dir)) return INVALID_HANDLE_VALUE_LOCAL;
    memcpy(dir, p, n);
    dir[n] = 0;
    CB_FIND *f = (CB_FIND *)malloc(sizeof(CB_FIND));
    if (!f) return INVALID_HANDLE_VALUE_LOCAL;
    f->dir = opendir(n ? dir : "/");
    if (!f->dir) { free(f); return INVALID_HANDLE_VALUE_LOCAL; }
    if (!FindNextFileW((HANDLE)f, fd)) { FindClose((HANDLE)f); return INVALID_HANDLE_VALUE_LOCAL; }
    return (HANDLE)f;
}

CB_BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd) {
    CB_FIND *f = (CB_FIND *)h;
    if (!f || f == INVALID_HANDLE_VALUE_LOCAL || !f->dir) return FALSE;
    struct dirent *e;
    while ((e = readdir(f->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        // Real attributes, straight from the directory: the core's warm-skip
        // compares cursor size+mtime against exactly these two fields.
        struct stat st;
        if (fstatat(dirfd(f->dir), e->d_name, &st, 0)) continue;
        int isDir = (e->d_type == DT_DIR)
                    || (e->d_type == DT_UNKNOWN && S_ISDIR(st.st_mode));
        memset(fd, 0, sizeof(*fd));
        if (isDir) fd->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
        if (mbstowcs(fd->cFileName, e->d_name, MAX_PATH - 1) == (size_t)-1)
            continue;   // undecodable name: skip it rather than hand up an empty string
        fd->nFileSizeLow = (DWORD)((unsigned long long)st.st_size & 0xffffffffu);
        fd->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
        fd->ftLastWriteTime = unixMsToFileTime((long long)st.st_mtime * 1000);
        return TRUE;
    }
    closedir(f->dir);
    f->dir = NULL;
    return FALSE;
}

CB_BOOL FindClose(HANDLE h) {
    CB_FIND *f = (CB_FIND *)h;
    if (!f || f == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    if (f->dir) closedir(f->dir);
    free(f);
    return TRUE;
}

CB_BOOL CreateDirectoryW(const wchar_t *p, void *sa) {
    (void)sa;
    const char *s = pathArg(p);
    return (CB_BOOL)(s[0] && mkdir(s, 0755) == 0);
}

CB_BOOL GetFileAttributesExW(const wchar_t *p, int level, CB_FILE_ATTRIBUTE_DATA *fa) {
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
//
// NOTE ON THE RESERVED SURFACE: several functions below (lstrcpy*/lstrcat*,
// CopyFileW, SetFileTime, GetFileTime, CreateDirectoryW, GetSystemTime,
// GetTickCount64, Sleep, ...) have zero call sites in the parts the macOS build
// compiles today. They are kept because chocobar_core.h declares the Win32
// surface and the pending p_metrics/p_subs ports compile against exactly that
// surface; deleting the implementations would leave declared-but-undefined
// symbols that link fine until the day they are called. They are untested
// code - treat any first real caller with suspicion and add it to the
// self-test.

int lstrlenW(const wchar_t *s) { return s ? (int)wcslen(s) : 0; }
int lstrcmpW(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
int lstrcmpiW(const wchar_t *a, const wchar_t *b) { return wcscasecmp(a, b); }
int lstrcpyW(wchar_t *d, const wchar_t *s) { wcscpy(d, s); return (int)wcslen(d); }
int lstrcpynW(wchar_t *d, const wchar_t *s, int n) {
    if (n <= 0) return 0;
    wcsncpy(d, s, n - 1);
    d[n - 1] = 0;
    return (int)wcslen(d);
}
int lstrcatW(wchar_t *d, const wchar_t *s) { wcscat(d, s); return (int)wcslen(d); }
// wcsrchr must forward to the libc function, not to itself: the Win32 spelling
// and the POSIX spelling are the same identifier here, so a same-named wrapper
// that calls wcsrchr(...) recurses forever. Alias the libc one first.
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
static wchar_t *(*const cb_wcsrchr)(const wchar_t *, wchar_t) = wcsrchr;
wchar_t *wcsrchr(const wchar_t *s, wchar_t c) { return cb_wcsrchr(s, c); }
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

// The core's fileTimeToUnixMs is static to its translation unit, so the adapter
// cannot call it and must not redefine it. This is the same arithmetic, used
// only to turn a POSIX time_t into the FILETIME the core's stat calls hand
// back. The core converts the other direction itself when it needs epoch ms.

// The two codecs the core's whole string story rests on. wchar_t is UTF-32 here,
// so mbstowcs/wcstombs are the right tools - not hand-rolled byte widening.
int MultiByteToWideChar(unsigned cp, DWORD flags, const char *s, int bytes, wchar_t *out, int outChars) {
    (void)cp; (void)flags;
    if (!s) return 0;
    // Win32 contract: convert EXACTLY `bytes` input bytes (bytes < 0 = the
    // NUL-terminated whole string), write at most `outChars` wide chars,
    // return 0 on invalid input or when the result does not fit.
    //
    // mbsrtowcs cannot express the input-side bound - it converts until the
    // NUL or the output limit, whichever comes first. The core passes
    // mid-buffer SPANS (jdup hands utf8ToWide a byte range inside the raw
    // config text; aggRecord hands an app key inside a session line), so a
    // conversion that runs to the NUL turns every span into the rest of the
    // buffer: sources resolved to garbage paths and the scan found zero files.
    // This mbrtowc loop is the only spelling that honours both bounds.
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    const char *p = s;
    int left = bytes;
    if (left < 0) left = (int)strlen(s);
    if (outChars <= 0) {
        // size query: how many wchar_t exactly `left` bytes would need
        int total = 0;
        while (left > 0) {
            wchar_t wc;
            size_t k = mbrtowc(&wc, p, (size_t)left, &st);
            if (k == (size_t)-1 || k == (size_t)-2) return 0;   // invalid | incomplete
            if (k == 0) { total++; break; }                     // an embedded NUL is one char
            total++; p += k; left -= (int)k;
        }
        return total;
    }
    int total = 0;
    while (left > 0 && total < outChars) {
        wchar_t wc;
        size_t k = mbrtowc(&wc, p, (size_t)left, &st);
        if (k == (size_t)-1 || k == (size_t)-2) return 0;
        if (k == 0) { out[total++] = 0; break; }
        out[total++] = wc; p += k; left -= (int)k;
    }
    if (left > 0) return 0;   // input remained but the destination is full
    return total;
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
    // The bound is the DESTINATION byte count (outBytes), not the input wide
    // count: Win32 writes at most outBytes bytes and returns 0
    // (ERROR_INSUFFICIENT_BUFFER) when they do not fit. Passing the input count
    // here made tokUtf8Copy's shrink loop dead - it exists precisely because
    // this call must be able to refuse - and a long token then memcpy'd a
    // 60-byte conversion into a 20-byte app row, corrupting the heap. This is
    // the same class of bug as MultiByteToWideChar's, and it was caught by the
    // Linux seam harness, not by review: bounded conversion, refuse if short.
    size_t n = wcsrtombs(out, &p, (size_t)outBytes, &st);
    if (n == (size_t)-1) return 0;   // EILSEQ etc: Win32 also returns 0
    if (n >= (size_t)outBytes && p < w + wideChars) return 0;   // did not fit
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

CB_BOOL SetEnvironmentVariableW(const wchar_t *name, const wchar_t *value) {
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
    st->wMinute = (unsigned short)tmv.tm_min;
    st->wSecond = (unsigned short)tmv.tm_sec;
    st->wMilliseconds = 0;
}

CB_BOOL SystemTimeToFileTime(const CB_SYSTEMTIME *st, FILETIME *ft) {
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

CB_BOOL LocalFileTimeToFileTime(const FILETIME *lft, FILETIME *ft) {
    // Correct ONLY as a pair with this adapter's SystemTimeToFileTime, which
    // applies the local zone via mktime: what the caller built as "local
    // wall-clock labelled FILETIME" is already the instant, so the pass-through
    // is exact. dashMidnightMs uses that pair and is correct. A caller that
    // uses LocalFileTimeToFileTime WITHOUT that SystemTimeToFileTime first -
    // wallNowLocalMs in p_utils.c is the standing example - wants the Windows
    // local->UTC flip and would get a zone-shifted value instead. Do not bring
    // such a caller across without re-deriving its frame.
    *ft = *lft;
    return TRUE;
}

long InterlockedExchange(long volatile *target, long value) {
    // Full exchange with release/acquire semantics, matching the Windows
    // intrinsic the core's worker/UI hand-off is written against.
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
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

CB_BOOL CopyFileW(const wchar_t *from, const wchar_t *to, CB_BOOL failIfExists) {
    const char *a = pathArg(from), *b = pathArg(to);
    int in = open(a, O_RDONLY);
    if (in < 0) return FALSE;
    // failIfExists is the Windows contract: CREATE_NEW semantics for the
    // destination, not a silent overwrite.
    int out = open(b, O_WRONLY | O_CREAT | (failIfExists ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) { close(in); return FALSE; }
    char buf[65536];
    ssize_t k;
    int ok = 1;
    while ((k = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)k) != k) { ok = 0; break; }
    }
    close(in); close(out);
    if (!ok) unlink(b);
    return (CB_BOOL)ok;
}

CB_BOOL GetFileTime(HANDLE h, FILETIME *c, FILETIME *a, FILETIME *w) {
    if (h == INVALID_HANDLE_VALUE_LOCAL) return FALSE;
    struct stat st;
    if (fstat(HANDLE_TO_FD(h), &st)) return FALSE;
    FILETIME ft = unixMsToFileTime((long long)st.st_mtime * 1000);
    if (c) *c = ft;
    if (a) *a = ft;
    if (w) *w = ft;
    return TRUE;
}

CB_BOOL SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w) {
    (void)h; (void)c; (void)a;
    if (!w) return FALSE;
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)(adapterFileTimeToUnixMs(w) / 1000);
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

static FILETIME unixMsToFileTime(long long ms) {
    long long ticks = (ms + 11644473600000LL) * 10000LL;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(ticks & 0xffffffffLL);
    ft.dwHighDateTime = (DWORD)((ticks >> 32) & 0xffffffffLL);
    return ft;
}

// ---- wide env for the core -----------------------------------------------------

// Establish the locale the codecs assume, and export USERPROFILE for the core's
// path logic. Constructors here run before main(); the order between this file's
// two initialisers does not matter because both are in this one function.
//
// Without this, a .app launched from Finder inherits the C locale, where
// mbstowcs/wcstombs reject every byte >= 0x80: non-ASCII paths and env values
// would fail silently (pathArg -> "", open -> ENOENT). Try explicit UTF-8
// locales first because they do not depend on the user's environment; the
// ambient locale is the last resort, and the outcome is logged so a future
// encoding failure is attributable from native.log alone.
__attribute__((constructor)) static void exportProfile(void) {
    setenv("USERPROFILE", profileDir(), 1);

    static const char *const candidates[] = { "en_US.UTF-8", "UTF-8", "C.UTF-8", "" };
    for (int i = 0; i < 4; i++) {
        // "" means "take the ambient environment's locale"
        if (setlocale(LC_ALL, candidates[i])) {
            char lb[96];
            snprintf(lb, sizeof(lb), "[wizbar] mac adapter: locale set to %s",
                     i == 3 ? (getenv("LANG") ? getenv("LANG") : "(ambient)") : candidates[i]);
            writeLogA(lb);
            return;
        }
    }
    writeLogA("[wizbar] mac adapter: WARNING no UTF-8 locale available - non-ASCII paths will fail");
}

// ---- critical sections --------------------------------------------------------------
//
// These are real, not no-ops. The core pins a config generation (cfgPin) so a
// reload cannot pull a generation out from under a reader mid-walk; that is a
// correctness guarantee the macOS build must keep too, or a config save could
// hand a half-retired Config pointer to a scan in progress.

void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (m) {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &a);
        pthread_mutexattr_destroy(&a);
    } else {
        // A NULL _opaque silently turns every Enter/Leave into a no-op, which
        // would quietly strip the config-generation guarantee. Say so once.
        writeLogA("[wizbar] mac adapter: WARNING critical section init failed (out of memory)");
    }
    cs->_opaque = m;
}

void DeleteCriticalSection(CRITICAL_SECTION *cs) {
    if (cs->_opaque) { pthread_mutex_destroy((pthread_mutex_t *)cs->_opaque); free(cs->_opaque); cs->_opaque = NULL; }
}

void EnterCriticalSection(CRITICAL_SECTION *cs) {
    if (cs->_opaque) pthread_mutex_lock((pthread_mutex_t *)cs->_opaque);
}

void LeaveCriticalSection(CRITICAL_SECTION *cs) {
    if (cs->_opaque) pthread_mutex_unlock((pthread_mutex_t *)cs->_opaque);
}

// ---- CRT -------------------------------------------------------------------------

// The core's only non-standard C dependency: a 64-bit string-to-integer used to
// read JSON numbers that exceed int range (cursor byte offsets, mtimes).
// strtoll is the portable equivalent and has the same semantics for a plain
// digit string, including the "return 0 on overflow-free garbage" behaviour
// _atoi64 documents.
long long _atoi64(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }
