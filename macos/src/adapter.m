// macOS adapter: the platform layer the portable Chocobar core compiles against.
//
// WHY THIS FILE EXISTS
// The core parts macos/build.sh assembles compile UNCHANGED on macOS once the
// Windows SDK types are replaced: chocobar_core.h declares the Win32 surface
// and this file is its POSIX implementation. It implements the entire surface
// the core touches: heap, file I/O, wide-string helpers, UTF conversion and
// logging.
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
// closeThreadHandle is defined with the thread seam near the bottom of this
// file; CloseHandle needs it early so a thread handle is never mistaken for an
// fd-cast handle.
static int closeThreadHandle(HANDLE h);
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

static HANDLE openFor(const wchar_t *path, int flags, int mode) {
    const char *p = pathArg(path);
    if (!p[0]) return INVALID_HANDLE_VALUE;
    int fd = open(p, flags, mode);
    return fd < 0 ? INVALID_HANDLE_VALUE : FD_TO_HANDLE(fd);
}

HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, void *sa,
                   DWORD creation, DWORD flags, HANDLE tmpl) {
    (void)sa; (void)share; (void)tmpl; (void)flags;
    // The header's disposition constants ARE the contract (chocobar_core.h):
    // match each one explicitly and refuse what is not one, so a future caller
    // cannot get create-or-overwrite semantics out of a typo'd disposition.
    int oflags = 0;
    int isWrite = (access & GENERIC_WRITE) != 0;
    int isAppend = (access & FILE_APPEND_DATA) != 0; // FILE_APPEND_DATA
    int isRead = !isWrite && !isAppend;
    if (creation == CREATE_ALWAYS) oflags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (creation == OPEN_EXISTING) oflags = isAppend ? O_WRONLY | O_APPEND
                                                                 : (isRead ? O_RDONLY : O_RDWR);
    else if (creation == CREATE_NEW) oflags = O_WRONLY | O_CREAT | O_EXCL;
    else if (creation == OPEN_ALWAYS) oflags = O_RDWR | O_CREAT;
    else return INVALID_HANDLE_VALUE;   // unknown disposition: refuse, as Win32
    return openFor(path, oflags, 0644);
}

CB_BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, LPOVERLAPPED o) {
    (void)o;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    // Sequential from the descriptor's own position: the core seeks explicitly
    // via SetFilePointerEx before every read, so an offset here would fight it.
    ssize_t k = read(HANDLE_TO_FD(h), buf, n);
    if (k < 0) return FALSE;
    if (got) *got = (DWORD)k;
    return TRUE;
}

CB_BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *wrote, LPOVERLAPPED o) {
    (void)o;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
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
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    struct stat st;
    if (fstat(HANDLE_TO_FD(h), &st)) return FALSE;
    sz->QuadPart = st.st_size;
    return TRUE;
}

CB_BOOL CloseHandle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    // Thread handles are heap objects this adapter created, NOT fds; they are
    // joined and freed here. The test is a pointer COMPARISON against a registry
    // (see the thread section), never a dereference, so an fd-cast handle can
    // never be mistaken for a thread and crash the bar.
    if (closeThreadHandle(h)) return TRUE;
    return close(HANDLE_TO_FD(h)) == 0;
}

CB_BOOL DeleteFileW(const wchar_t *p) { const char *s = pathArg(p); return (CB_BOOL)(s[0] && unlink(s) == 0); }

CB_BOOL SetFilePointerEx(HANDLE h, CB_LARGE_INTEGER dist, CB_LARGE_INTEGER *out, DWORD method) {
    if (h == INVALID_HANDLE_VALUE) return FALSE;
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

// One definition of the st_size split and the mtime conversion: the warm-skip
// of the whole scanner compares cursor size+mtime against exactly these fields,
// so every site must compute them identically.
static FILETIME statMtimeToFileTime(const struct stat *st) {
    // Win32 FILETIME carries sub-second precision and the warm-skip compares
    // cursor mtime against exactly this field: keep the milliseconds the
    // timespec member exposes, or a store rewritten inside the same second as
    // the stored cursor warm-skips until its size changes. The timespec
    // member's spelling is the one platform difference: st_mtimespec is
    // Darwin's own; Linux/glibc spells it st_mtim (POSIX 2008) and its
    // st_mtime is a plain time_t with no nanoseconds.
#if defined(__APPLE__)
    return unixMsToFileTime((long long)st->st_mtimespec.tv_sec * 1000 + st->st_mtimespec.tv_nsec / 1000000);
#else
    return unixMsToFileTime((long long)st->st_mtim.tv_sec * 1000 + st->st_mtim.tv_nsec / 1000000);
#endif
}
static void statSizeToWin32(const struct stat *st, DWORD *low, DWORD *high) {
    *low = (DWORD)((unsigned long long)st->st_size & 0xffffffffu);
    *high = (DWORD)((unsigned long long)st->st_size >> 32);
}

DWORD GetFileSize(HANDLE h, LPDWORD high) {
    struct stat st;
    if (h == INVALID_HANDLE_VALUE || fstat(HANDLE_TO_FD(h), &st)) return INVALID_FILE_SIZE;
    DWORD lo = 0, hi = 0;
    statSizeToWin32(&st, &lo, &hi);
    if (high) *high = hi;
    return lo;
}

CB_BOOL GetFileInformationByHandle(HANDLE h, CB_BY_HANDLE_FILE_INFORMATION *fi) {
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    struct stat st;
    if (fstat(HANDLE_TO_FD(h), &st)) return FALSE;
    memset(fi, 0, sizeof(*fi));
    statSizeToWin32(&st, &fi->nFileSizeLow, &fi->nFileSizeHigh);
    fi->ftLastWriteTime = statMtimeToFileTime(&st);
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
    if (!p[0]) return INVALID_HANDLE_VALUE;
    // strip the trailing wildcard the core appends: "<dir>/*" (separators are
    // already translated by pathArg; accept a raw backslash form too)
    size_t n = strlen(p);
    while (n && (p[n - 1] == '*' || p[n - 1] == '/' || p[n - 1] == '\\')) n--;
    char dir[PATH_MAX];
    if (n >= sizeof(dir)) return INVALID_HANDLE_VALUE;
    memcpy(dir, p, n);
    dir[n] = 0;
    CB_FIND *f = (CB_FIND *)malloc(sizeof(CB_FIND));
    if (!f) return INVALID_HANDLE_VALUE;
    f->dir = opendir(n ? dir : "/");
    if (!f->dir) { free(f); return INVALID_HANDLE_VALUE; }
    if (!FindNextFileW((HANDLE)f, fd)) { FindClose((HANDLE)f); return INVALID_HANDLE_VALUE; }
    return (HANDLE)f;
}

CB_BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd) {
    CB_FIND *f = (CB_FIND *)h;
    if (!f || f == INVALID_HANDLE_VALUE || !f->dir) return FALSE;
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
        statSizeToWin32(&st, &fd->nFileSizeLow, &fd->nFileSizeHigh);
        fd->ftLastWriteTime = statMtimeToFileTime(&st);
        return TRUE;
    }
    closedir(f->dir);
    f->dir = NULL;
    return FALSE;
}

CB_BOOL FindClose(HANDLE h) {
    CB_FIND *f = (CB_FIND *)h;
    if (!f || f == INVALID_HANDLE_VALUE) return FALSE;
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
    fa->ftLastWriteTime = statMtimeToFileTime(&st);
    statSizeToWin32(&st, &fa->nFileSizeLow, &fa->nFileSizeHigh);
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
// wcsrchr must not call the identifier wcsrchr: the Win32 and the POSIX
// spelling are the same identifier, and this file's definition shadows libc's
// for every reference in the translation unit, so a wrapper that calls it
// recurses until the stack dies. Scan for the last occurrence directly.
wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    if (!s) return NULL;
    const wchar_t *last = NULL;
    for (const wchar_t *p = s;; p++) {
        if (*p == c) last = p;
        if (!*p) break;
    }
    return (wchar_t *)last;
}
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
    // bytes < 0 is the NUL-terminated whole string, and Win32 counts that
    // terminating NUL as INPUT: it is converted, written, and included in the
    // return, so a caller never has to terminate the result itself.
    int withNul = left < 0;
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
        return withNul ? total + 1 : total;
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
    if (withNul) {
        if (total >= outChars) return 0;   // the terminating NUL needs a slot too
        out[total++] = 0;
    }
    return total;
}

int WideCharToMultiByte(unsigned cp, DWORD flags, const wchar_t *w, int wideChars,
                        char *out, int outBytes, void *def, void *used) {
    (void)cp; (void)flags; (void)def; (void)used;
    if (!w) return 0;
    // wideChars < 0 is the NUL-terminated whole string, and the NUL is input for
    // this direction too: Win32 requires room for it and counts it in the return.
    int withNul = wideChars < 0;
    if (wideChars < 0) wideChars = (int)wcslen(w);
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    if (outBytes <= 0) {
        // Size query: the measured length excludes the terminator, so the
        // NUL-terminated (-1) form is one byte larger, exactly as Win32 reports.
        const wchar_t *p = w;
        size_t q = wcsrtombs(NULL, &p, 0, &st);
        if (q == (size_t)-1) return 0;
        return (int)q + withNul;
    }
    const wchar_t *p = w;
    // The bound is the DESTINATION byte count (outBytes), not the input wide
    // count: Win32 writes at most outBytes bytes and returns 0
    // (ERROR_INSUFFICIENT_BUFFER) when they do not fit. Passing the input count
    // here made tokUtf8Copy's shrink loop dead - it exists precisely because
    // this call must be able to refuse - and a long token then memcpy'd a
    // 60-byte conversion into a 20-byte app row, corrupting the heap. This is
    // the same class of bug as MultiByteToWideChar's, and it was caught by the
    // Linux seam harness, not by review: bounded conversion, refuse if short.
    //
    // wcsrtombs stops on a CHARACTER boundary and leaves the source pointer
    // inside the string when the bytes ran out, so unconsumed input - not the
    // byte count, which is short on every partial fill - is the refusal signal.
    // It resets the pointer to NULL only once the whole string has converted, so
    // a non-NULL pointer short of the end means it did not fit.
    size_t n = wcsrtombs(out, &p, (size_t)outBytes, &st);
    if (n == (size_t)-1) return 0;   // EILSEQ etc: Win32 also returns 0
    if (p != NULL && p < w + wideChars + withNul) return 0;   // input remained: did not fit
    // Terminate when there is room (always true on a -1 success, where the
    // terminator had to fit to convert), and report Win32's count: the byte
    // length for an explicit span, length+1 for the NUL-terminated form.
    // tokWideToUtf8 subtracts 1 for the -1 form and must land on strlen.
    if ((size_t)outBytes > n) out[n] = 0;
    return (int)n + withNul;
}

// ---- environment -------------------------------------------------------------

DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *out, DWORD cch) {
    char nbuf[256];
    size_t nl = wcstombs(nbuf, name, sizeof(nbuf) - 1);
    if (nl == (size_t)-1) return 0;
    nbuf[nl] = 0;
    const char *v = getenv(nbuf);
    if (!v) return 0;
    // Win32's contract for this API is exact: the size query and the
    // too-small-buffer case both return the required count INCLUDING the NUL,
    // and a too-small buffer leaves the destination untouched - never a
    // silent truncate. The canonical query-then-refill caller (p_subs.c)
    // allocates the queried count and would run one short otherwise.
    size_t wneed = mbstowcs(NULL, v, 0);   // wide chars, excluding the NUL
    if (wneed == (size_t)-1) return 0;
    if (!out || cch == 0) return (DWORD)wneed + 1;
    if ((size_t)cch < wneed + 1) return (DWORD)wneed + 1;   // refuse, untouched
    size_t need = mbstowcs(out, v, cch);
    if (need == (size_t)-1) return 0;
    out[need] = 0;
    return (DWORD)need;   // success count excludes the NUL, as Win32 documents
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

// Both time readers fill the same eight fields; keep the struct filling in one
// place so CB_SYSTEMTIME cannot grow a field under one of them.
static void fillSystemTime(CB_SYSTEMTIME *out, const struct tm *tmv) {
    out->wYear = (unsigned short)(tmv->tm_year + 1900);
    out->wMonth = (unsigned short)(tmv->tm_mon + 1);
    out->wDayOfWeek = (unsigned short)tmv->tm_wday;
    out->wDay = (unsigned short)tmv->tm_mday;
    out->wHour = (unsigned short)tmv->tm_hour;
    out->wMinute = (unsigned short)tmv->tm_min;
    out->wSecond = (unsigned short)tmv->tm_sec;
    out->wMilliseconds = 0;
}

void GetLocalTime(CB_SYSTEMTIME *st) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    fillSystemTime(st, &tmv);
}

void GetSystemTime(CB_SYSTEMTIME *st) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    fillSystemTime(st, &tmv);
}

// Days since 1970-01-01 for a proleptic Gregorian date. Win32's
// SystemTimeToFileTime reads a SYSTEMTIME as UTC, and timegm is not uniformly
// declared across the clang/libc pair this file builds against, so the UTC
// reading is done arithmetically instead.
static long long civilDays(long long y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

CB_BOOL SystemTimeToFileTime(const CB_SYSTEMTIME *st, FILETIME *ft) {
    // Win32 applies NO time zone here: the FILETIME handed back is the
    // wall-clock fields read as if they were UTC, so it is a label rather than
    // an instant. dashMidnightMs relies on that frame - it subtracts whole
    // days of wall clock and lets LocalFileTimeToFileTime apply the zone once,
    // per day. A caller that wants the local instant must apply the zone
    // itself, exactly as it does on Windows.
    if (st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31
        || st->wHour > 23 || st->wMinute > 59 || st->wSecond > 59) return FALSE;
    long long secs = civilDays(st->wYear, st->wMonth, st->wDay) * 86400LL
                   + st->wHour * 3600 + st->wMinute * 60 + st->wSecond;
    *ft = unixMsToFileTime(secs * 1000 + st->wMilliseconds);
    return TRUE;
}

CB_BOOL LocalFileTimeToFileTime(const FILETIME *lft, FILETIME *ft) {
    // Win32 applies the zone FOR THE DATE BEING CONVERTED, so a whole-day
    // subtraction taken in the wall-clock frame still lands on the target
    // day's true local midnight when the zone offset changes in between. The
    // input's numeric value is a local wall clock read as if it were UTC, so
    // gmtime_r recovers those fields and mktime turns them into the instant
    // for that date's own offset.
    long long ms = adapterFileTimeToUnixMs(lft);
    long long sec = ms / 1000;
    int msec = (int)(ms % 1000);
    if (msec < 0) { msec += 1000; sec -= 1; }
    time_t t = (time_t)sec;
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return FALSE;
    tmv.tm_isdst = -1;
    time_t u = mktime(&tmv);
    if (u == (time_t)-1) return FALSE;
    *ft = unixMsToFileTime((long long)u * 1000 + msec);
    return TRUE;
}

long InterlockedExchange(long volatile *target, long value) {
    // Full exchange with release/acquire semantics, matching the Windows
    // intrinsic the core's worker/UI hand-off is written against.
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

long InterlockedCompareExchange(long volatile *target, long value, long compare) {
    // Same contract as the Windows intrinsic: store `value` only if *target ==
    // compare, and return the ORIGINAL value either way. The subs worker relies
    // on that return being the old value - it uses a test-and-clear
    // (compare 0, set 0) as an acquire of a kick flag, so a version that
    // returned whether it swapped would never see a pending kick at all.
    __atomic_compare_exchange_n(target, &compare, value, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return compare; // compare holds the value observed before the exchange
}

DWORD GetTickCount(void) { return (DWORD)((unsigned long long)(monotonic_ms() & 0xffffffffu)); }
ULONGLONG GetTickCount64(void) { return (ULONGLONG)monotonic_ms(); }
void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }

void GetSystemTimeAsFileTime(FILETIME *ft) {
    // The wall clock as a FILETIME, which is what subs uses it for (a true
    // epoch timestamp for the board footer and for OAuth token expiry).
    // unixMsToFileTime already handles the 1601 epoch shift, so this cannot
    // drift from the reverse conversion the core already uses.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *ft = unixMsToFileTime((long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

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
    // One logical write per line keeps concurrent writers from interleaving;
    // loop the write so a signal or a short write cannot drop half a line.
    char line[1200];
    int n = snprintf(line, sizeof(line), "%s\n", s);
    // snprintf returns the length the output WOULD have had; clamp to what
    // was actually written before it can become a write() byte count.
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    if (n > 0) {
        const char *lp = line;
        size_t left = (size_t)n;
        while (left > 0) {
            ssize_t w = write(fd, lp, left);
            if (w < 0) { if (errno == EINTR) continue; break; }
            if (w == 0) break;
            lp += w; left -= (size_t)w;
        }
    }
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
    // Same standard as WriteFile above: loop EINTR, fail on real errors, and
    // complete every destination write - an interrupted read must not report
    // success with a truncated destination left on disk.
    for (;;) {
        k = read(in, buf, sizeof(buf));
        if (k < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        if (k == 0) break;
        const char *bp = buf;
        size_t left = (size_t)k;
        while (left > 0) {
            ssize_t w = write(out, bp, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                ok = 0;
                break;
            }
            if (w == 0) { ok = 0; break; }
            bp += w; left -= (size_t)w;
        }
        if (!ok) break;
    }
    close(in); close(out);
    if (!ok) unlink(b);
    return (CB_BOOL)ok;
}

CB_BOOL GetFileTime(HANDLE h, FILETIME *c, FILETIME *a, FILETIME *w) {
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    struct stat st;
    if (fstat(HANDLE_TO_FD(h), &st)) return FALSE;
    FILETIME ft = statMtimeToFileTime(&st);
    if (c) *c = ft;
    if (a) *a = ft;
    if (w) *w = ft;
    return TRUE;
}

CB_BOOL MoveFileExW(const wchar_t *src, const wchar_t *dst, DWORD flags) {
    const char *a = pathArg(src), *b = pathArg(dst);
    // p_subs writes a token cache to a temp file and renames it into place so a
    // reader never sees a half-written file. rename(2) is atomic on POSIX and
    // already replaces the target, which IS MOVEFILE_REPLACE_EXISTING; without
    // that flag Windows would fail on an existing destination, so honour it
    // rather than silently succeeding with different semantics.
    if (!(flags & MOVEFILE_REPLACE_EXISTING)) {
        struct stat st;
        if (stat(b, &st) == 0) return FALSE; // destination exists: Windows would fail
    }
    return (CB_BOOL)(rename(a, b) == 0);
}

// ---- threads ------------------------------------------------------------------
//
// p_subs fans its enabled providers out over worker threads and joins them, so
// a cycle costs the slowest provider instead of the sum. The Win32 surface that
// needs is exactly three calls (CreateThread, WaitForSingleObject,
// WaitForMultipleObjects) plus CloseHandle to dispose the handle.
//
// HANDLE is void* here, and the file functions use fd-cast handles, so a thread
// handle must be distinguishable to CloseHandle WITHOUT dereferencing anything
// (dereferencing an fd-cast handle would segfault the bar). Every thread handle
// this file creates is registered below; CloseHandle compares the pointer
// against that registry and only then touches its fields.
typedef struct {
    pthread_t tid;
    int joined;
    int done;
    int refs;          // the handle owner + the thread itself
    pthread_mutex_t m;
    pthread_cond_t cv;
} AdapterThread;

// The registry is bounded, and a full registry is NOT an error: CreateThread
// returns NULL and the core's own fallback runs that fetch inline. The size
// still has to cover the real worst case or the fallback becomes the normal
// path. p_subs.c can have, at once: the cycle thread (1), one thread per
// enabled provider (MAX_SUBS = 6), each antigravity provider fanning out over
// two model hosts (6*2) plus one quota thread (6), and the update probe (1).
// That is 1 + 6 + 12 + 6 + 1 = 26; 32 leaves headroom for a future caller.
#define ADAPTER_THREADS_MAX 32
static AdapterThread *g_threads[ADAPTER_THREADS_MAX];
static pthread_mutex_t g_threadsLock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { LPTHREAD_START_ROUTINE fn; void *arg; AdapterThread *slot; } ThreadStart;

// Drop one owner of the thread record and free it when the last one goes. Two
// owners exist: the HANDLE (closed by the core) and the running thread (which
// releases its own when it finishes). Without the count, closing a handle to a
// thread that is still running would free the record out from under it.
static void dropThreadRef(AdapterThread *t) {
    pthread_mutex_lock(&g_threadsLock);
    int last = (--t->refs == 0);
    pthread_mutex_unlock(&g_threadsLock);
    if (last) {
        pthread_cond_destroy(&t->cv);
        pthread_mutex_destroy(&t->m);
        free(t);
    }
}

static void *threadTrampoline(void *p) {
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    // The Windows thread proc returns a DWORD exit code; nothing in the core
    // reads it (the subs providers report through their own locked setters),
    // so the value is dropped rather than invented.
    if (s.fn) s.fn(s.arg);
    // Signal completion BEFORE the thread exits, so a bounded wait can observe
    // it without racing the join. Pairing this with a real condition variable
    // (rather than polling a join) keeps the wait portable: pthread_tryjoin_np
    // is a Linux extension and the macOS build compiles this same file.
    AdapterThread *t = s.slot;
    pthread_mutex_lock(&t->m);
    t->done = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->m);
    dropThreadRef(t); // the thread's own reference
    return NULL;
}

HANDLE CreateThread(void *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn,
                    LPVOID arg, DWORD flags, DWORD *id) {
    (void)sa; (void)stack; (void)flags; (void)id;
    AdapterThread *t = (AdapterThread *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (pthread_mutex_init(&t->m, NULL) != 0) { free(t); return NULL; }
    if (pthread_cond_init(&t->cv, NULL) != 0) { pthread_mutex_destroy(&t->m); free(t); return NULL; }
    t->refs = 2; // the handle and the thread
    ThreadStart *s = (ThreadStart *)malloc(sizeof(*s));
    if (!s) { pthread_cond_destroy(&t->cv); pthread_mutex_destroy(&t->m); free(t); return NULL; }
    s->fn = fn; s->arg = arg; s->slot = t;
    if (pthread_create(&t->tid, NULL, threadTrampoline, s) != 0) {
        free(s);
        pthread_cond_destroy(&t->cv); pthread_mutex_destroy(&t->m);
        free(t);
        return NULL; // the caller falls back to running the fetch inline
    }
    pthread_mutex_lock(&g_threadsLock);
    int placed = 0;
    for (int i = 0; i < ADAPTER_THREADS_MAX; i++) {
        if (!g_threads[i]) { g_threads[i] = t; placed = 1; break; }
    }
    pthread_mutex_unlock(&g_threadsLock);
    if (!placed) {
        // No registry slot: detach so the thread's stack is reclaimed when it
        // ends, and drop ONLY the handle's reference - the running thread still
        // holds its own and frees the record itself. Destroying the mutex or
        // freeing here would pull the record out from under a live thread.
        pthread_detach(t->tid);
        dropThreadRef(t);
        return NULL;
    }
    return (HANDLE)t;
}

// Join one thread, waiting at most `ms` (INFINITE joins outright). Returns 1
// when the thread has finished, 0 on timeout.
static int joinThread(AdapterThread *t, DWORD ms) {
    if (t->joined) return 1;
    if (ms == INFINITE) {
        void *ret = NULL;
        if (pthread_join(t->tid, &ret) != 0) return 1; // already gone: treat as done
        t->joined = 1;
        return 1;
    }
    // Bounded wait on the completion flag. The core only ever passes INFINITE,
    // so this is the safety net, not the hot path.
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(ms / 1000);
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec += 1; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&t->m);
    while (!t->done) {
        if (pthread_cond_timedwait(&t->cv, &t->m, &deadline) != 0) break; // timeout or error
    }
    int done = t->done;
    pthread_mutex_unlock(&t->m);
    if (!done) return 0;
    void *ret = NULL;
    if (pthread_join(t->tid, &ret) == 0) t->joined = 1;
    return 1;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!h) return 0xFFFFFFFFu; // WAIT_FAILED
    AdapterThread *t = (AdapterThread *)h;
    int done = 0;
    pthread_mutex_lock(&g_threadsLock);
    for (int i = 0; i < ADAPTER_THREADS_MAX; i++)
        if (g_threads[i] == t) { done = 1; break; }
    pthread_mutex_unlock(&g_threadsLock);
    if (!done) return 0xFFFFFFFFu; // not one of ours: do not dereference
    return joinThread(t, ms) ? 0 /* WAIT_OBJECT_0 */ : 0x102u /* WAIT_TIMEOUT */;
}

DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, int waitAll, DWORD ms) {
    // The core only uses waitAll = TRUE (join every provider before the cycle
    // is reported complete). waitAll = FALSE would need first-one-wins
    // semantics that nothing calls, so it joins all and reports the first
    // index - refusing silently would be worse than doing the work.
    (void)waitAll;
    for (DWORD i = 0; i < n; i++) {
        if (WaitForSingleObject(hs[i], ms) != 0) return 0xFFFFFFFFu;
    }
    return 0;
}

// Registered thread handles are released here; anything else is not a thread
// and is left for the fd path in CloseHandle.
//
// WINDOWS SEMANTICS, EXACTLY: closing a thread handle does NOT wait for the
// thread. The core relies on that - subsStart() closes the handle of a worker
// that runs FOREVER, and a CloseHandle that joined would block the bar's
// startup until the heat death of the universe (measured: the bar hung in
// pthread_join before it ever opened its X display, so no window appeared at
// all). So a live thread is DETACHED here and its record is freed by the
// thread itself when it finally ends; a finished one is reaped.
static int closeThreadHandle(HANDLE h) {
    if (!h) return 0;
    pthread_mutex_lock(&g_threadsLock);
    int idx = -1;
    for (int i = 0; i < ADAPTER_THREADS_MAX; i++)
        if (g_threads[i] == (AdapterThread *)h) { idx = i; break; }
    if (idx >= 0) g_threads[idx] = NULL;
    pthread_mutex_unlock(&g_threadsLock);
    if (idx < 0) return 0; // not a thread handle
    AdapterThread *t = (AdapterThread *)h;
    int done;
    pthread_mutex_lock(&t->m);
    done = t->done;
    pthread_mutex_unlock(&t->m);
    if (!done) {
        // Still running: detach so the thread's resources are reclaimed when it
        // ends, and let the thread free the record (it holds its own ref).
        pthread_detach(t->tid);
    } else {
        void *ret = NULL;
        if (!t->joined) pthread_join(t->tid, &ret);
        t->joined = 1;
    }
    dropThreadRef(t); // the handle's reference
    return 1;
}

CB_BOOL SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w) {
    (void)h; (void)c; (void)a;
    if (!w) return FALSE;
    struct timespec ts[2];
    long long ms = adapterFileTimeToUnixMs(w);
    // floor-divide so a pre-1970 (negative) mtime still yields a non-negative
    // tv_nsec; a raw % would hand futimens an invalid negative nanosecond field
    long long sec = ms / 1000;
    long long ns = ms % 1000;
    if (ns < 0) { ns += 1000; sec -= 1; }
    ts[0].tv_sec = (time_t)sec;
    // carry the sub-second part the read side (statMtimeToFileTime) preserves,
    // so a write-then-warm-skip round trip compares at the same granularity
    ts[0].tv_nsec = (long)(ns * 1000000);
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
// path logic. The constructor here runs before main(); the order between the two
// initialisations below does not matter because both are in this one function.
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
        // "" means "take the ambient environment's locale", and setlocale
        // SUCCEEDS with the plain C locale when the environment names none -
        // the Finder-launch case this loop exists for. Trust the RESULT, not
        // the non-NULL return: any installed locale whose name carries UTF-8
        // is the only acceptable outcome, and the log must say what was
        // actually installed, not what LANG claimed.
        const char *got = setlocale(LC_ALL, candidates[i]);
        if (got && (strstr(got, "UTF-8") || strstr(got, "utf8"))) {
            char lb[96];
            snprintf(lb, sizeof(lb), "[wizbar] mac adapter: locale set to %s", got);
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
