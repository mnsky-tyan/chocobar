// Checks for the kwinrc self-heal (linux/src/selfheal.c), run by
// linux/tests/behaviour.sh: each case writes a kwinrc into its own throwaway
// HOME, calls cbKwinSelfHeal() and compares the whole file byte for byte.
//
// Build (fortify is what turns the old overshoot into an abort, so this is
// compiled with the same -O2 as the bar):
//   gcc -O2 -o selfheal_check linux/tests/selfheal_check.c linux/src/selfheal.c
//       -Ilinux/src $(pkg-config --cflags --libs dbus-1)
//   ./selfheal_check <scratch-directory>
//
// The session bus is pointed at a path that does not exist, so the KWin
// reconfigure call is a no-op and nothing here reaches a real session.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

int cbKwinSelfHeal(void);

static int passed = 0, failed = 0;
static char root[1024];

static void report(const char *name, int ok, const char *detail) {
    if (ok) { printf("PASS: %s\n", name); passed++; }
    else { printf("FAIL: %s%s%s\n", name, detail && *detail ? ": " : "", detail ? detail : ""); failed++; }
}

// the whole directory holds nothing but kwinrc afterwards: no temporary file
// was left behind by the replace
static int onlyKwinrc(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int ok = 1, seen = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strcmp(e->d_name, "kwinrc")) { seen = 1; continue; }
        ok = 0;
    }
    closedir(d);
    return ok && seen;
}

// name: case id; before: the file's bytes; blen: its length; after: what it
// must hold afterwards (alen = -1: the file must be byte-identical); wantRc:
// the return value; mode: the mode the file is written with and must keep
static void run(const char *name, const char *before, size_t blen,
                const char *after, ssize_t alen, int wantRc, mode_t mode) {
    char home[1200], cfg[1240], path[1300];
    snprintf(home, sizeof home, "%s/%s", root, name);
    snprintf(cfg, sizeof cfg, "%s/.config", home);
    snprintf(path, sizeof path, "%s/kwinrc", cfg);
    mkdir(home, 0755);   // EEXIST on a rerun is fine; a real failure shows below
    mkdir(cfg, 0755);
    FILE *f = fopen(path, "wb");
    if (!f) { report(name, 0, "cannot write the case file"); return; }
    fwrite(before, 1, blen, f);
    fclose(f);
    chmod(path, mode);
    setenv("HOME", home, 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-selfheal-check", 1);
    int rc = cbKwinSelfHeal();
    static char got[65536];
    size_t glen = 0;
    f = fopen(path, "rb");
    if (f) { glen = fread(got, 1, sizeof got, f); fclose(f); }
    struct stat st;
    int same;
    if (alen < 0) same = (glen == blen && memcmp(got, before, blen) == 0);
    else same = (glen == (size_t)alen && memcmp(got, after, (size_t)alen) == 0);
    char detail[300];
    if (rc != wantRc) {
        snprintf(detail, sizeof detail, "returned %d, wanted %d", rc, wantRc);
        report(name, 0, detail);
        return;
    }
    if (!same) {
        snprintf(detail, sizeof detail, "file is %zu bytes and not what was expected", glen);
        report(name, 0, detail);
        return;
    }
    if (stat(path, &st) != 0 || (st.st_mode & 0777) != mode) {
        snprintf(detail, sizeof detail, "mode %o, wanted %o", (unsigned)(st.st_mode & 0777), (unsigned)mode);
        report(name, 0, detail);
        return;
    }
    if (!onlyKwinrc(cfg)) { report(name, 0, "a temporary file was left behind"); return; }
    report(name, 1, "");
}

// the common shape: both texts are plain strings
static void runz(const char *name, const char *before, const char *after, int wantRc, mode_t mode) {
    run(name, before, strlen(before), after, after ? (ssize_t)strlen(after) : -1, wantRc, mode);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: selfheal_check <scratch-directory>\n"); return 64; }
    snprintf(root, sizeof root, "%s", argv[1]);
    mkdir(root, 0755);

    // the value is kept and the class is appended to it
    runz("an existing exclusion list keeps every entry and gains the class",
        "[Round-Corners]\nExclusions=alpha,beta\nK=1\n", "[Round-Corners]\nExclusions=alpha,beta,chocobar\nK=1\n", 1, 0644);
    // blanks around the "=" are still that key
    runz("blanks around the equals sign keep the key and its value",
        "[Round-Corners]\nExclusions = alpha, beta\nK=1\n", "[Round-Corners]\nExclusions = alpha, beta,chocobar\nK=1\n", 1, 0644);
    // the case that aborted the bar: the group has no Exclusions key at all
    runz("a group with only the older ExclusionList key gains a new line and keeps the old one",
        "[Round-Corners]\nExclusionList=alpha\nK=1\n", "[Round-Corners]\nExclusions=chocobar\nExclusionList=alpha\nK=1\n", 1, 0644);
    runz("ExclusionList is never edited when both keys exist",
        "[Round-Corners]\nExclusionList=alpha\nExclusions=beta\nK=1\n", "[Round-Corners]\nExclusionList=alpha\nExclusions=beta,chocobar\nK=1\n", 1, 0644);
    // the key is matched as a whole word: a longer key that starts with it
    // is another key, so the class goes on a line of its own
    runz("a key that merely starts with Exclusions is another key and stays as it is",
        "[Round-Corners]\nExclusionsX=alpha\nK=1\n", "[Round-Corners]\nExclusions=chocobar\nExclusionsX=alpha\nK=1\n", 1, 0644);
    runz("a CRLF file keeps its line endings",
        "[Round-Corners]\r\nExclusions=alpha\r\nK=1\r\n", "[Round-Corners]\r\nExclusions=alpha,chocobar\r\nK=1\r\n", 1, 0644);
    runz("an empty value gains the class with no leading comma",
        "[Round-Corners]\nExclusions=\n", "[Round-Corners]\nExclusions=chocobar\n", 1, 0644);
    runz("a last line without a newline is not given one",
        "[Round-Corners]\nExclusions=alpha", "[Round-Corners]\nExclusions=alpha,chocobar", 1, 0644);
    runz("a key in a later group is not touched; the line goes into the effect's group",
        "[Round-Corners]\nK=1\n[Other]\nExclusions=alpha\n", "[Round-Corners]\nExclusions=chocobar\nK=1\n[Other]\nExclusions=alpha\n", 1, 0644);
    // the header found as a bare substring used to start the search inside
    // the earlier group, and its Exclusions key took the class
    runz("the header quoted in a comment of an earlier group is not the effect's group",
        "[Other]\n# the old [Round-Corners] effect\nExclusions=alpha\n[Round-Corners]\nK=1\n",
        "[Other]\n# the old [Round-Corners] effect\nExclusions=alpha\n[Round-Corners]\nExclusions=chocobar\nK=1\n", 1, 0644);
    runz("a header that does not end its line is not the effect's group",
        "[Round-Corners] old\nExclusions=alpha\n", NULL, 0, 0644);
    runz("a private file mode is kept through the replace",
        "[Round-Corners]\nExclusions=alpha\n", "[Round-Corners]\nExclusions=alpha,chocobar\n", 1, 0600);

    // nothing to do: the file must come back byte for byte and untouched
    runz("the class already present (any case) leaves the file alone",
        "[Round-Corners]\nExclusions=ChocoBar, alpha\n", NULL, 0, 0644);
    runz("no [Round-Corners] group leaves the file alone",
        "[Other]\nExclusions=alpha\n", NULL, 0, 0644);
    // a NUL byte: this is not the text it looks like
    run("a NUL byte inside the file leaves it alone",
        "[Round-Corners]\nExcl\0usions=alpha\n", 34, NULL, -1, 0, 0644);
    runz("a long list still finds the class at its end",
        "[Round-Corners]\nExclusions=one,two,three,four,five,six,seven,eight,nine,ten,eleven,twelve,"
        "thirteen,fourteen,fifteen,sixteen,seventeen,eighteen,nineteen,twenty,chocobar\n", NULL, 0, 0644);

    printf("\n--- selfheal: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
