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
// SAFETY: the edit is a single key append, nothing else is rewritten; a file
// that cannot be parsed, has no [Round-Corners] group, or is owned by root is
// left untouched and reported as "nothing done". No line is ever removed.
#include "selfheal.h"

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

// Is the class already in the key's value (case-insensitive substring, exactly
// what the effect itself does with each entry)?
static int hasClass(const char *val) {
    if (!val) return 0;
    for (const char *p = val; *p; ) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - s);
        while (n && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
        if (n == sizeof(CB_WIN_CLASS) - 1 && strncasecmp(s, CB_WIN_CLASS, n) == 0) return 1;
    }
    return 0;
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
    buf[n] = 0;
    fclose(f);

    // Find the effect's group; if it does not exist we cannot invent the user's
    // other keys, so do nothing (the bar still runs, the effect stays as-is).
    char *grp = strstr(buf, CB_GROUP_HEADER);
    if (!grp) { free(buf); return 0; }
    char *grpEnd = grp + sizeof(CB_GROUP_HEADER) - 1;
    char *nextGrp = strstr(grpEnd, "\n[");
    size_t grpLen = nextGrp ? (size_t)(nextGrp - grp) : strlen(grp);

    // find the Exclusions key inside the group
    char *keyPos = NULL;
    size_t lineLen = sizeof(CB_EXCL_KEY) - 1;
    for (char *p = grpEnd; p < grp + grpLen; ) {
        char *eol = strchr(p, '\n');
        size_t segLen = eol ? (size_t)(eol - p) : strlen(p);
        if (segLen >= lineLen && strncmp(p, CB_EXCL_KEY, lineLen) == 0) { keyPos = p; break; }
        if (!eol) break;
        p = eol + 1;
    }

    int changed = 0;
    if (keyPos) {
        char *eol = strchr(keyPos, '\n');
        if (!eol) { free(buf); return 0; }
        char *val = keyPos + lineLen;
        if (*val == '=') val++;
        size_t valLen = (size_t)(eol - val);
        char tmp[512];
        size_t cpy = valLen < sizeof(tmp) - 1 ? valLen : sizeof(tmp) - 1;
        memcpy(tmp, val, cpy);
        tmp[cpy] = 0;
        if (hasClass(tmp)) { free(buf); return 0; }   // already excluded
        // append to the value, preserving every other entry
        char *out = (char *)malloc(n + sizeof(CB_WIN_CLASS) + 4);
        if (!out) { free(buf); return 0; }
        size_t head = (size_t)(keyPos - buf) + lineLen + 1;
        memcpy(out, buf, head);
        if (valLen > 0) out[head++] = ',';
        memcpy(out + head, CB_WIN_CLASS, sizeof(CB_WIN_CLASS) - 1);
        head += sizeof(CB_WIN_CLASS) - 1;
        memcpy(out + head, eol, strlen(eol));
        out[head + strlen(eol)] = 0;
        FILE *w = fopen(path, "wb");
        if (w) { fputs(out, w); fclose(w); changed = 1; }
        free(out);
    } else {
        // no Exclusions key: insert one right after the group header. Placing it
        // after the header keeps any future keys the user added ahead of it.
        char *out = (char *)malloc(n + sizeof(CB_EXCL_KEY) + sizeof(CB_WIN_CLASS) + 8);
        if (!out) { free(buf); return 0; }
        size_t head = (size_t)(grpEnd - buf);
        memcpy(out, buf, head);
        int w = snprintf(out + head, n + 64, "\n%s=%s", CB_EXCL_KEY, CB_WIN_CLASS);
        memcpy(out + head + w, buf + head, n - (size_t)head + 1);
        FILE *fh = fopen(path, "wb");
        if (fh) { fputs(out, fh); fclose(fh); changed = 1; }
        free(out);
    }
    free(buf);
    if (!changed) return 0;
    kwinReconfigure();
    return 1;
}
