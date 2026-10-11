// Win32-compatible surface declarations for the portable Chocobar core.
//
// This header exists so `chocobar.c`, `p_utils.c`, `p_icons.c` and
// `p_tokens.c` compile UNCHANGED on macOS. The core is written against the
// Win32 spellings; this file declares them and macos/src/adapter.m implements
// them on top of POSIX. The core source is never edited to accommodate a
// platform, which is what keeps the Windows and macOS builds from drifting.
//
// Measured 2026-10-11: with exactly these declarations in place, the four core
// parts assemble into a 2849-line translation unit that compiles with zero
// errors under clang. See docs/reviews/2026-10-11-portable-core-boundary.md.
//
// NOT INCLUDED HERE, ON PURPOSE:
// - p_metrics.c and p_subs.c. They are wholly Win32-coupled at the type level
//   (PDH_*, IMMDeviceEnumerator, SYSTEM_POWER_STATUS, MEMORYSTATUSEX,
//   PROCESSENTRY32W, COM/GUID, winhttp.h) and need real per-OS implementations,
//   not declarations. A macOS front-end starts without them and grows them.
// - p_ui.c. The bar's rendering and window management are the part being
//   REWRITTEN for macOS, not shared.
// - GDI/GDI+ types. p_icons.c parses SVG paths purely, but its renderer is
//   GDI; the SVG parser is reused, the renderer is not.

#ifndef CHOCOBAR_CORE_H
#define CHOCOBAR_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

// ---- fundamental types -------------------------------------------------------

#ifndef MAX_PATH
#define MAX_PATH 1024
#endif

// BOOL is already defined by Objective-C (objc/objc.h, as `bool`), which every
// .m file in the macOS build pulls in through Foundation. Redefining it as int
// there is a hard error. The plain-C core translation unit has no objc header,
// so it needs the Windows spelling. Both spellings behave identically for
// everything the core does with them: assignment of TRUE/FALSE and truth tests.
#if defined(__OBJC__)
#import <objc/objc.h>
#else
typedef int BOOL;
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef size_t SIZE_T;
typedef unsigned char BYTE;
typedef void *HANDLE;
typedef void *LPVOID;
typedef const wchar_t *LPCWSTR;
typedef DWORD *LPDWORD;
typedef long long CB_LARGE;

typedef struct { long long QuadPart; } CB_LARGE_INTEGER;
typedef CB_LARGE_INTEGER LARGE_INTEGER;

typedef union { struct { DWORD LowPart, HighPart; }; ULONGLONG QuadPart; } ULARGE_INTEGER;

// HANDLE is a pointer, so the INVALID_HANDLE_VALUE sentinel must be a value no
// real descriptor can produce. -1 cast to a pointer satisfies that.
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_SIZE ((DWORD)-1)

typedef struct { int _reserved; } OVERLAPPED, *LPOVERLAPPED;

// 100-nanosecond ticks since 1601-01-01. The core does the epoch arithmetic
// itself (fileTimeToUnixMs), so only the representation matters.
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;

typedef struct {
    unsigned short wYear, wMonth, wDayOfWeek, wDay,
                  wHour, wMinute, wSecond, wMilliseconds;
} CB_SYSTEMTIME;
typedef CB_SYSTEMTIME SYSTEMTIME;

// A named critical section. The core uses these for config generation locking;
// on macOS the adapter provides a real one (pthread_mutex), not a no-op, so the
// pinning semantics survive.
typedef struct { void *_opaque; } CB_CRITICAL_SECTION;
typedef CB_CRITICAL_SECTION CRITICAL_SECTION;

typedef struct {
    unsigned long dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} CB_FILE_ATTRIBUTE_DATA;
typedef CB_FILE_ATTRIBUTE_DATA WIN32_FILE_ATTRIBUTE_DATA;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} CB_BY_HANDLE_FILE_INFORMATION;
typedef CB_BY_HANDLE_FILE_INFORMATION BY_HANDLE_FILE_INFORMATION;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    wchar_t cFileName[MAX_PATH];
} WIN32_FIND_DATAW;

// share modes (the core passes them but never acts on them; macOS opens
// without a share concept, so the adapter ignores the argument)
#define FILE_SHARE_READ       0x00000001
#define FILE_SHARE_WRITE      0x00000002
#define FILE_SHARE_DELETE     0x00000004

// file flags (also ignored by the adapter, declared so the core parses)
#define FILE_ATTRIBUTE_READONLY      0x00000001
#define FILE_FLAG_SEQUENTIAL_SCAN    0x08000000
#define FILE_FLAG_RANDOM_ACCESS      0x10000000
#define FILE_ATTRIBUTE_DIRECTORY     0x00000010
#define FILE_ATTRIBUTE_NORMAL        0x00000080

// access rights (only the bits the core actually tests)
#define GENERIC_READ   0x80000000
#define GENERIC_WRITE  0x40000000
#define FILE_APPEND_DATA 0x00000004

// creation dispositions
#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5

// seek origins
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

// GetFileAttributesExW levels
#define GetFileExInfoStandard 0

// codepage
#define CP_UTF8 65001
#define CP_ACP 0

// ---- heap --------------------------------------------------------------------

void *HeapAlloc(void *heap, DWORD flags, SIZE_T bytes);
void *HeapReAlloc(void *heap, DWORD flags, void *ptr, SIZE_T bytes);
void HeapFree(void *heap, DWORD flags, void *ptr);
void *GetProcessHeap(void);

// ---- file I/O ----------------------------------------------------------------

HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, void *sa,
                   DWORD creation, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, void *buf, DWORD bytes, DWORD *got, LPOVERLAPPED o);
BOOL WriteFile(HANDLE h, const void *buf, DWORD bytes, DWORD *wrote, LPOVERLAPPED o);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *out, DWORD method);
DWORD GetFileSize(HANDLE h, LPDWORD high);
BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *fi);
BOOL GetFileAttributesExW(const wchar_t *path, int level, WIN32_FILE_ATTRIBUTE_DATA *fa);
BOOL CloseHandle(HANDLE h);
BOOL DeleteFileW(const wchar_t *path);
BOOL CopyFileW(const wchar_t *from, const wchar_t *to, BOOL failIfExists);
BOOL CreateDirectoryW(const wchar_t *path, void *sa);
BOOL GetFileTime(HANDLE h, FILETIME *created, FILETIME *accessed, FILETIME *written);
BOOL SetFileTime(HANDLE h, const FILETIME *created, const FILETIME *accessed, const FILETIME *written);

// ---- directory enumeration ---------------------------------------------------

HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *fd);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
BOOL FindClose(HANDLE h);

// ---- wide strings ------------------------------------------------------------

int lstrlenW(const wchar_t *s);
int lstrcmpW(const wchar_t *a, const wchar_t *b);
int lstrcmpiW(const wchar_t *a, const wchar_t *b);
int lstrcpyW(wchar_t *dst, const wchar_t *src);
int lstrcpynW(wchar_t *dst, const wchar_t *src, int cch);
int lstrcatW(wchar_t *dst, const wchar_t *src);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);

int lstrlenA(const char *s);
int lstrcmpA(const char *a, const char *b);
int lstrcmpiA(const char *a, const char *b);
int lstrcpyA(char *dst, const char *src);
int lstrcpynA(char *dst, const char *src, int cch);
int lstrcatA(char *dst, const char *src);

int MultiByteToWideChar(unsigned codepage, DWORD flags, const char *s, int bytes,
                        wchar_t *out, int outChars);
int WideCharToMultiByte(unsigned codepage, DWORD flags, const wchar_t *w, int wideChars,
                        char *out, int outBytes, void *def, void *used);

// ---- environment --------------------------------------------------------------

DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *out, DWORD cch);
BOOL SetEnvironmentVariableW(const wchar_t *name, const wchar_t *value);

// ---- time ---------------------------------------------------------------------

void GetLocalTime(SYSTEMTIME *st);
void GetSystemTime(SYSTEMTIME *st);
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft);
BOOL LocalFileTimeToFileTime(const FILETIME *lft, FILETIME *ft);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
void Sleep(DWORD ms);

// ---- critical sections ---------------------------------------------------------

void InitializeCriticalSection(CRITICAL_SECTION *cs);
void DeleteCriticalSection(CRITICAL_SECTION *cs);
void EnterCriticalSection(CRITICAL_SECTION *cs);
void LeaveCriticalSection(CRITICAL_SECTION *cs);

// ---- CRT ---------------------------------------------------------------------

// _atoi64 is a Microsoft CRT function the core uses to read a JSON number that
// can exceed int range (cursor byte offsets and mtimes). MinGW provides it;
// macOS does not, so the portable build gets this. The implementation lives in
// the adapter; this is only the declaration the core needs.
long long _atoi64(const char *s);

// ---- diagnostics -----------------------------------------------------------------

// The core's only logging channel. Declared here because the core calls it by
// name; implemented in the adapter.
// The core declares writeLogA as `static void writeLogA(const char *s);`
// forward, before it is defined in p_ui.c. On Windows that is legal because it
// is one translation unit. Split across files it would collide with the
// adapter's external definition, so the core's own forward declaration is left
// alone and the adapter's lives in a C++-safe spot below.
#ifdef __cplusplus
extern "C" {
#endif
void writeLogA(const char *s);
#ifdef __cplusplus
}
#endif

#endif // CHOCOBAR_CORE_H
