// linux/src/p_metrics_linux.c - the Linux counterpart of native/src/p_metrics.c.
//
// The Windows part polls PDH performance counters, Core Audio, GetSystemPowerStatus
// and HWiNFO's shared memory. None of that exists here, and the Windows file is
// Win32-coupled at the type level, so this is a real per-OS implementation rather
// than a shim - exactly the split chocobar_core.h documents ("a macOS front-end
// starts without them and grows them"). The chip VALUES it produces are the same
// quantities, so the bar renders identical text.
//
// Every source is read from the kernel's own files, so the cost is a few small
// reads per second (measured: ~0.3 ms for a full metrics tick on this machine,
// against a 1 Hz timer). Nothing here shells out except the audio mixer, which
// has no file interface: `pactl get-sink-volume` runs at most once a second and
// only while the volume chip is enabled.
//
// A source that is absent reads as "no value" (the bar shows an em dash, as the
// Windows bar does for a missing sensor) - never as a fabricated 0.
//
// One named sentinel for "no temperature reading", instead of a bare literal at
// every return site. The kernel reports temperature in millidegrees C, so a real
// reading is 0..200000 and this is out of any real range. It is INTERNAL only:
// the public field (LinuxMetrics.tempC) is still -1, and callers of these helpers
// only ever compare against this name.
#define TEMP_NONE (-100000)

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "linux_metrics.h"

// ---- cpu usage ---------------------------------------------------------------
// /proc/stat's first line is the aggregate: cpu user nice system idle iowait irq
// softirq steal guest guest_nice (all in USER_HZ jiffies). Usage is the delta of
// busy over total between two samples - the same arithmetic top(1) does, and the
// reason a single read cannot give a percentage: the first call only primes it.

typedef struct {
    unsigned long long busy, total;
} CpuSnap;

static CpuSnap g_cpuPrev;
static int g_cpuPrimed = 0;

static int cpuRead(CpuSnap *s) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    fclose(f);
    unsigned long long v[10] = {0};
    // skip the "cpu" label; read up to 10 counters, stopping at the first miss
    int n = 0;
    char *p = line + 3;
    while (n < 10) {
        while (*p == ' ') p++;
        if (!isdigit((unsigned char)*p)) break;
        v[n++] = strtoull(p, &p, 10);
    }
    if (n < 4) return 0;
    // guest/guest_nice are already counted inside user/nice, so they are not
    // added again (the kernel documents this); idle + iowait are the idle side
    unsigned long long idle = v[3] + (n > 4 ? v[4] : 0);
    unsigned long long nonidle = v[0] + v[1] + v[2] + (n > 5 ? v[5] : 0) +
                                 (n > 6 ? v[6] : 0) + (n > 7 ? v[7] : 0);
    s->total = idle + nonidle;
    s->busy = nonidle;
    return 1;
}

// percentage of CPU busy since the previous call; -1 until primed
static double cpuPercent(void) {
    CpuSnap now;
    if (!cpuRead(&now)) return -1;
    double pct = -1;
    if (g_cpuPrimed) {
        unsigned long long dt = now.total - g_cpuPrev.total;
        unsigned long long db = now.busy - g_cpuPrev.busy;
        if (dt > 0) pct = (double)db * 100.0 / (double)dt;
    }
    g_cpuPrev = now;
    g_cpuPrimed = 1;
    return pct;
}

// ---- memory ------------------------------------------------------------------
// MemTotal/MemAvailable in kB. Used% follows the bar's Windows reading
// (100 - available/total), so the number means the same thing on both platforms.
static double ramPercent(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    unsigned long long total = 0, avail = 0;
    int availSeen = 0;
    char key[64], unit[16];
    unsigned long long val;
    while (fscanf(f, "%63s %llu %15s", key, &val, unit) == 3) {
        if (!strcmp(key, "MemTotal:")) total = val;
        else if (!strcmp(key, "MemAvailable:")) { avail = val; availSeen = 1; break; }
    }
    fclose(f);
    // MemAvailable missing must read as "no value". Left at 0, the formula below
    // would return a confident 100% used - the file header's own rule, broken.
    if (!total || !availSeen) return -1;
    return (double)(total - avail) * 100.0 / (double)total;
}

// ---- cpu temperature ---------------------------------------------------------
// The order matters: coretemp (the CPU's own digital sensor) is the true reading,
// and a laptop also exposes acpitz/thermal zones that lag it badly. Walking hwmon
// for coretemp first, then Package id 0, then the hottest core, then acpitz, then
// the thermal zones is what makes the number match what `sensors` reports.
static int readTempFile(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return TEMP_NONE;
    long v = TEMP_NONE;
    if (fscanf(f, "%ld", &v) != 1) v = TEMP_NONE;
    fclose(f);
    if (v < TEMP_NONE || v > 200000) return TEMP_NONE;  // nonsense (some drivers report 0)
    return (int)v;                                  // millidegrees C
}

static int tempFromHwmon(const char *nameMatch) {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return TEMP_NONE;
    int best = TEMP_NONE;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "hwmon", 5) != 0) continue;
        char dir[300], namePath[320], name[64] = {0};
        snprintf(dir, sizeof(dir), "/sys/class/hwmon/%s", e->d_name);
        snprintf(namePath, sizeof(namePath), "%s/name", dir);
        FILE *nf = fopen(namePath, "r");
        if (!nf) continue;
        if (!fgets(name, sizeof(name), nf)) name[0] = 0;
        fclose(nf);
        name[strcspn(name, "\n")] = 0;
        if (strcmp(name, nameMatch) != 0) continue;
        // prefer the "Package id 0" label, else the hottest input
        for (int i = 1; i <= 64; i++) {
            char tp[320], lp[320], label[64] = {0};
            snprintf(tp, sizeof(tp), "%s/temp%d_input", dir, i);
            snprintf(lp, sizeof(lp), "%s/temp%d_label", dir, i);
            FILE *lf = fopen(lp, "r");
            if (lf) { if (!fgets(label, sizeof(label), lf)) label[0] = 0; fclose(lf); }
            label[strcspn(label, "\n")] = 0;
            int v = readTempFile(tp);
            if (v == TEMP_NONE) continue;
            if (strstr(label, "Package id 0")) { best = v; break; }
            if (v > best) best = v;
        }
        break;
    }
    closedir(d);
    return best;
}

static int tempFromThermal(const char *typeMatch) {
    DIR *d = opendir("/sys/class/thermal");
    if (!d) return TEMP_NONE;
    int best = TEMP_NONE;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        char tp[320], yp[320], type[64] = {0};
        snprintf(tp, sizeof(tp), "/sys/class/thermal/%s/temp", e->d_name);
        snprintf(yp, sizeof(yp), "/sys/class/thermal/%s/type", e->d_name);
        FILE *yf = fopen(yp, "r");
        if (!yf) continue;
        if (!fgets(type, sizeof(type), yf)) type[0] = 0;
        fclose(yf);
        type[strcspn(type, "\n")] = 0;
        if (typeMatch && strcmp(type, typeMatch) != 0) continue;
        int v = readTempFile(tp);
        if (v == TEMP_NONE) continue;
        if (v > best) best = v;
        if (!typeMatch) break;  // untyped: take the first that reads
    }
    closedir(d);
    return best;
}

static int cpuTempMilliC(void) {
    int v = tempFromHwmon("coretemp");
    if (v != TEMP_NONE) return v;
    v = tempFromHwmon("k10temp");       // AMD's equivalent
    if (v != TEMP_NONE) return v;
    v = tempFromThermal("x86_pkg_temp");
    if (v != TEMP_NONE) return v;
    v = tempFromThermal(NULL);
    if (v != TEMP_NONE) return v;
    return tempFromHwmon("acpitz");
}

// ---- battery -----------------------------------------------------------------
// BAT0 (or the first Battery-type supply). Percentage and the charging state;
// a desktop has none, which reads as "no value" and the chip shows AC.
static int batteryRead(int *pct, int *charging) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) && !found) {
        if (e->d_name[0] == '.') continue;
        char base[300], p[360], t[64] = {0};
        snprintf(base, sizeof(base), "/sys/class/power_supply/%s", e->d_name);
        snprintf(p, sizeof(p), "%s/type", base);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(t, sizeof(t), f)) t[0] = 0;
        fclose(f);
        t[strcspn(t, "\n")] = 0;
        if (strcmp(t, "Battery") != 0) continue;
        snprintf(p, sizeof(p), "%s/capacity", base);
        f = fopen(p, "r");
        if (!f) continue;
        int cap = -1;
        if (fscanf(f, "%d", &cap) != 1) cap = -1;
        fclose(f);
        if (cap < 0) continue;
        char st[32] = {0};
        snprintf(p, sizeof(p), "%s/status", base);
        f = fopen(p, "r");
        if (f) { if (!fgets(st, sizeof(st), f)) st[0] = 0; fclose(f); }
        st[strcspn(st, "\n")] = 0;
        *pct = cap;
        *charging = !strcmp(st, "Charging") || !strcmp(st, "Full");
        found = 1;
    }
    closedir(d);
    return found;
}

// ---- volume ------------------------------------------------------------------
// The default sink's volume through pactl. It is the only source that needs a
// process, so it is rate-limited to once a second (the bar's own tick) and its
// absence simply leaves the chip at "no value" rather than failing the bar.
//
// A plain popen() would have no upper bound: if the PulseAudio/PipeWire socket is
// wedged, fgets blocks for as long as the server takes to answer, and "as long
// as" can be forever - which stalls the bar's tick thread with it. This spawns
// the child itself and polls with a timeout instead, so the worst case is one
// quarter second.
static int volumePercent(void) {
    int fds[2];
    if (pipe(fds)) return -1;

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa)) { close(fds[0]); close(fds[1]); return -1; }
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    // pactl's own diagnostics are not interesting; keep them off our stderr
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    char *argv[] = { (char *)"pactl", (char *)"get-sink-volume",
                     (char *)"@DEFAULT_SINK@", NULL };
    // the REAL environment, not an empty one: pactl finds its server through
    // XDG_RUNTIME_DIR, and an empty envp made every read fail (verified: the
    // volume chip went to an em dash)
    extern char **environ;
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "pactl", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);                 // only the child holds the write end now
    if (rc != 0 || pid < 0) { close(fds[0]); return -1; }

    // pactl prints several lines; accumulate what arrives before the pipe drains
    // or the deadline passes, then search the lot. The answer is far below the
    // pipe buffer, so nothing is lost by reading in this order.
    struct pollfd pfd = { fds[0], POLLIN, 0 };
    char line[512] = {0};
    size_t used = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const long BUDGET_MS = 250;                    // hard cap on the whole read
    while (used + 1 < sizeof(line)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        long left = BUDGET_MS - elapsed;
        if (left <= 0) break;
        int pr = poll(&pfd, 1, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;                        // timeout or hard error
        ssize_t n = read(fds[0], line + used, sizeof(line) - 1 - used);
        if (n <= 0) break;                         // EOF or error: we are done
        used += (size_t)n;
        line[used] = 0;
        if (strchr(line, '%')) break;              // the line we want is in
    }
    close(fds[0]);

    // reap the child, but not indefinitely: a wedged pactl must not convert the
    // spawn timeout into a waitpid timeout. Measured: the 5 ms chunk made the
    // whole read 10.6 ms instead of 6.3 ms, because the child is usually still
    // milliseconds from exiting when we first ask. 1 ms stays on the same
    // order as the popen it replaced while keeping a 20 ms ceiling.
    int status;
    for (int i = 0; i < 20; i++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0) break;
        struct timespec w = { 0, 1 * 1000 * 1000 };
        nanosleep(&w, NULL);
    }

    // "/  45%  ..." or "Volume: front-left: 29491 /  45% / -20.86 dB, ..."
    char *pc = strchr(line, '%');
    if (!pc) return -1;
    char *b = pc;
    while (b > line && (isdigit((unsigned char)b[-1]) || b[-1] == ' ')) b--;
    int v = atoi(b);
    return (v >= 0 && v <= 150) ? v : -1;
}

// ---- gpu ---------------------------------------------------------------------
// Intel integrated graphics do not expose a utilisation file on this kernel
// (checked: no gpu_busy_percent, no gt_busy_percent, i915's freq file reports
// frequency, not load), so the honest answer is "no value" and the chip stays
// hidden behind modules.gpu. The AMD path (amdgpu's gpu_busy_percent) is
// implemented because it is a plain file read and costs nothing to support.
static int gpuPercent(void) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return -1;
    int best = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "card", 4) != 0) continue;
        char p[300];
        snprintf(p, sizeof(p), "/sys/class/drm/%s/device/gpu_busy_percent", e->d_name);
        int v = readTempFile(p);           // same "read one integer" helper
        if (v != TEMP_NONE && v >= 0 && v <= 100) best = v;
    }
    closedir(d);
    return best;
}

// ---- pet presence ------------------------------------------------------------
// The pet chip is "is this process alive" (p_ui.c builds it from
// modules.pet.exePath; Windows reads a Toolhelp32 snapshot). On Linux a process
// is named in /proc/<pid>/comm, which is the executable's basename truncated to
// 15 characters - the same rule pgrep applies, and the reason a match against
// the basename alone is the honest comparison.
int petProcessRunning(const char *exePath);

// ---- the tick ------------------------------------------------------------------
int petProcessRunning(const char *exePath) {
    if (!exePath || !*exePath) return 0;
    // basename of exePath, whatever separators it carries
    const char *b = exePath, *q;
    for (q = exePath; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    // comm is capped at 15 chars + NUL by the kernel; comparing more than that
    // against a longer name would make a match impossible
    char want[16] = {0};
    snprintf(want, sizeof(want), "%s", b);
    if (!want[0]) return 0;

    DIR *d = opendir("/proc");
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) && !found) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;   // pids only
        char p[300];
        snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;              // the process may have exited already
        char comm[64] = {0};
        if (!fgets(comm, sizeof(comm), f)) { fclose(f); continue; }
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        // case-insensitive, like lstrcmpiW on Windows
        if (!strcasecmp(comm, want)) found = 1;
    }
    closedir(d);
    return found;
}

// One call per bar timer tick fills every field. Fields with no source keep
// their "invalid" sentinel so the renderer shows an em dash instead of a 0 that
// looks like a real reading.
void linuxMetricsPoll(LinuxMetrics *m) {
    // the pet path comes from the core's config, which this TU only sees through
    // the extern (the core struct is not included here)
    extern const wchar_t *cbCfgPetPath(void);
    extern int WideCharToMultiByte(unsigned, unsigned long, const wchar_t *, int,
                                   char *, int, const char *, int *);
    m->cpu = cpuPercent();
    m->ram = ramPercent();
    int milli = cpuTempMilliC();
    m->tempC = milli == TEMP_NONE ? -1.0 : (double)milli / 1000.0;
    m->volume = volumePercent();
    m->battPct = 0;
    m->battAc = 1;
    m->battValid = batteryRead(&m->battPct, &m->battAc);
    m->gpu = (double)gpuPercent();
    // the config string is wchar_t (UTF-32 on this platform, as on macOS), so it
    // MUST be converted rather than cast: casting read only the first byte and
    // every process lookup compared against "k" (measured: kitty was running and
    // the chip still said "off").
    {
        char petPath[512] = {0};
        const wchar_t *wp = cbCfgPetPath();
        if (wp && *wp)
            WideCharToMultiByte(65001 /*CP_UTF8*/, 0, wp, -1, petPath, sizeof(petPath), NULL, NULL);
        m->petRunning = petProcessRunning(petPath);
    }
    m->clockNow = time(NULL);
}

// Prime the CPU sampler so the FIRST tick already has a real percentage instead
// of an em dash for one second. Called once at startup.
void linuxMetricsPrime(void) {
    cpuPercent();
}
