// selfheal.c - the desktop-side fixups the bar performs at startup.
//
// WHY: the bar lives at the lower layer and is not draggable, so its appearance
// is partly decided by the compositor. One KDE effect (KDE-Rounded-Corners) draws
// a 1px black outline on every window it manages, INCLUDING inactive ones, on
// top of the client pixels. For the bar - which paints its own border in the
// theme divider colour - that outline is visible as a black line riding the
// border. The fix is the effect's own exclusion list, keyed on window class.
//
// HOW: kwinrc is a KConfig INI file. The effect reads [Round-Corners]
// Exclusions (KConfig's string list = comma separated in one line). The bar
// appends "chocobar" if the key does not already contain it, then asks KWin to
// reconfigure over D-Bus with qdbus6/systemd-free code - the bar links libdbus
// already (geomdbus.c uses the same library).
//
// SAFETY: the edit is one line - the class appended to the value, every other
// entry of the value kept - and nothing else is rewritten. A file that cannot
// be parsed, has no [Round-Corners] group, carries a NUL byte, or is owned by
// root is left untouched and reported as "nothing done". No line is ever
// removed. The new text replaces the file through a sibling temporary file
// and rename, so a crash or a full disk can never leave kwinrc truncated.
//
// MEASURED 2026-11-25, on the code this replaced: it dropped the old value
// (Exclusions=alpha,beta came back as Exclusions=,chocobar), it took a key
// that merely starts with Exclusions for this one and rewrote that line, it
// took the group's name quoted in a comment for the group and edited an
// earlier group's key, it lost the CRLF ending and left a last line without a
// newline unedited, and with no Exclusions key at all it passed snprintf a
// bound larger than its buffer, which glibc's fortify turns into a process
// abort - the bar died on every start of such a profile.
// linux/tests/selfheal_check.c holds each case.
#include "selfheal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // getuid
#include <sys/stat.h>
#include <dbus/dbus.h>

// The bar's own window class (XClassHint in main.c): the exclusion matches it.
#define CB_WIN_CLASS "chocobar"
#define CB_GROUP_HEADER "[Round-Corners]"
#define CB_EXCL_KEY "Exclusions"

// The key we append to. The effect's config declares Exclusions (v0.10+); an
// older build (0.7) wrote ExclusionList, which the current effect ignores, so
// that key is left as-is and Exclusions is the one this module edits.
static void cbPath(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) { out[0] = 0; return; }
    snprintf(out, cap, "%s/.config/kwinrc", home);
}

// Is the class already one of the value's entries (a comma-separated list;
// the effect compares each trimmed entry case-insensitively)? The whole value
// is examined, so a list longer than any buffer cannot hide the class.
static int hasClass(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ',' || s[i] == ' ' || s[i] == '\t')) i++;
        size_t start = i;
        while (i < n && s[i] != ',') i++;
        size_t end = i;
        while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\r')) end--;
        if (end - start == sizeof(CB_WIN_CLASS) - 1 &&
            strncasecmp(s + start, CB_WIN_CLASS, end - start) == 0) return 1;
    }
    return 0;
}

// The effect's group header, only where KConfig reads one: starting a line
// (or the file) with nothing behind it but blanks. A plain substring search
// also found the name inside a comment of an earlier group, and the edit then
// landed on a key of that group.
static char *findGroup(const char *buf) {
    const size_t k = sizeof(CB_GROUP_HEADER) - 1;
    for (const char *p = buf; (p = strstr(p, CB_GROUP_HEADER)) != NULL; p++) {
        if (p != buf && p[-1] != '\n') continue;
        size_t q = k;
        while (p[q] == ' ' || p[q] == '\t') q++;
        if (p[q] == '\n' || p[q] == '\r' || p[q] == 0) return (char *)p;
    }
    return NULL;
}

// The line p..lineEnd is an Exclusions line: the key, optional blanks, "=".
// Returns the value's start, or NULL. The key must be the WHOLE word before
// the "=": a longer key (ExclusionsX) shares the prefix, and the old search
// took that line for this key and rewrote it.
static char *exclValue(char *p, char *lineEnd) {
    size_t k = sizeof(CB_EXCL_KEY) - 1;
    if ((size_t)(lineEnd - p) <= k || strncmp(p, CB_EXCL_KEY, k) != 0) return NULL;
    char *q = p + k;
    while (q < lineEnd && (*q == ' ' || *q == '\t')) q++;
    return (q < lineEnd && *q == '=') ? q + 1 : NULL;
}

// Replace path's contents with data[0..len) without ever leaving it truncated
// or empty: the text goes to a sibling temporary file (the same directory, so
// the rename is atomic) with the original's mode, and replaces the original
// only once it is complete on disk. 1 = replaced, 0 = nothing was changed.
static int replaceFile(const char *path, const char *data, size_t len, mode_t mode) {
    char tmp[1200];
    int w = snprintf(tmp, sizeof tmp, "%s.chocobar-%ld.tmp", path, (long)getpid());
    if (w <= 0 || w >= (int)sizeof tmp) return 0;
    // created with the original's mode and only when no such file exists, so
    // nothing planted at that name is followed, overwritten or world-readable
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) return 0;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmp); return 0; }
    int ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    // the text is on disk before the rename can be; a file system without a
    // sync is not a reason to leave kwinrc unedited
    if (fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP) ok = 0;
    ok = (fchmod(fileno(f), mode) == 0) && ok;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

// qdbus's exact job, in ~40 lines: ask KWin to re-read its configuration.
// The bar already has a session-bus connection (geomdbus.c), but this runs
// before that one is up, so it opens and closes its own.
static void kwinReconfigure(void) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!c || dbus_error_is_set(&err)) { dbus_error_free(&err); if (c) dbus_connection_unref(c); return; }
    DBusMessage *msg = dbus_message_new_method_call("org.kde.KWin", "/KWin",
                                                    "org.kde.KWin", "reconfigure");
    if (msg) {
        dbus_message_set_no_reply(msg, TRUE);
        dbus_connection_send(c, msg, NULL);
        dbus_connection_flush(c);
        dbus_message_unref(msg);
    }
    dbus_connection_unref(c);
}

int cbKwinSelfHeal(void) {
    char path[1024];
    cbPath(path, sizeof(path));
    if (!path[0]) return 0;

    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (st.st_uid != getuid()) return 0;   // only ever touch the user's own file

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) { fclose(f); return 0; }   // a sane INI, nothing huge
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return 0; }
    fclose(f);
    // every edit below works on NUL-terminated text; a NUL inside means this
    // is not the INI it looks like, and rewriting it would cut it at that byte
    if (memchr(buf, 0, (size_t)n)) { free(buf); return 0; }
    buf[n] = 0;

    // Find the effect's group; if it does not exist we cannot invent the user's
    // other keys, so do nothing (the bar still runs, the effect stays as-is).
    char *grp = findGroup(buf);
    if (!grp) { free(buf); return 0; }
    char *grpEnd = grp + sizeof(CB_GROUP_HEADER) - 1;
    char *nextGrp = strstr(grpEnd, "\n[");
    size_t grpLen = nextGrp ? (size_t)(nextGrp - grp) : strlen(grp);

    // find the Exclusions line inside the group, whole lines only
    char *val = NULL;
    for (char *p = grpEnd; p < grp + grpLen; ) {
        char *eol = strchr(p, '\n');
        char *lineEnd = eol ? eol : p + strlen(p);
        val = exclValue(p, lineEnd);
        if (val) break;
        if (!eol) break;
        p = eol + 1;
    }

    char *out = NULL;
    size_t outLen = 0;
    if (val) {
        char *eol = strchr(val, '\n');
        if (!eol) eol = val + strlen(val);
        size_t valLen = (size_t)(eol - val);
        // a CRLF file keeps its CR: it closes the line, it is not part of the value
        int crlf = (valLen > 0 && val[valLen - 1] == '\r');
        if (crlf) valLen--;
        if (hasClass(val, valLen)) { free(buf); return 0; }   // already excluded
        // the line up to the value, the WHOLE old value, the class, the rest of the file
        outLen = (size_t)(val - buf) + valLen + 1 + (sizeof(CB_WIN_CLASS) - 1) + (crlf ? 1 : 0) + strlen(eol);
        out = (char *)malloc(outLen + 1);
        if (!out) { free(buf); return 0; }
        size_t head = (size_t)(val - buf);
        memcpy(out, buf, head);
        memcpy(out + head, val, valLen); head += valLen;
        if (valLen > 0) out[head++] = ',';
        memcpy(out + head, CB_WIN_CLASS, sizeof(CB_WIN_CLASS) - 1); head += sizeof(CB_WIN_CLASS) - 1;
        if (crlf) out[head++] = '\r';
        memcpy(out + head, eol, strlen(eol) + 1); head += strlen(eol);
        outLen = head;
    } else {
        // no Exclusions key: insert one right after the group header. Placing it
        // after the header keeps any future keys the user added ahead of it.
        static const char ins[] = "\n" CB_EXCL_KEY "=" CB_WIN_CLASS;
        size_t head = (size_t)(grpEnd - buf);
        outLen = head + sizeof(ins) - 1 + ((size_t)n - head);
        out = (char *)malloc(outLen + 1);
        if (!out) { free(buf); return 0; }
        memcpy(out, buf, head);
        memcpy(out + head, ins, sizeof(ins) - 1); head += sizeof(ins) - 1;
        memcpy(out + head, grpEnd, (size_t)n - (size_t)(grpEnd - buf) + 1);
    }
    free(buf);
    if (!replaceFile(path, out, outLen, st.st_mode & 0777)) { free(out); return 0; }
    free(out);
    kwinReconfigure();
    return 1;
}
