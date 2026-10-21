// linux/src/p_metrics_linux.c - the Linux counterpart of native/src/p_metrics.c.
//
// The Windows part polls PDH performance counters, Core Audio, GetSystemPowerStatus
// and HWiNFO's shared memory. None of that exists here, and the Windows file is
// Win32-coupled at the type level, so this is a real per-OS implementation rather
// than a shim - exactly the split chocobar_core.h documents ("a macOS front-end
// starts without them and grows them"). The chip VALUES it produces are the same
// quantities, so the bar renders identical text.
//
// Every source is read from the kernel's own files or PMU counters, so the cost
// is a few small reads per second (measured: ~0.3 ms for a full metrics tick on
// this machine, against a 1 Hz timer). Nothing here shells out except the audio
// mixer, which has no file interface: `pactl get-sink-volume` runs at most once
// a second and only while the volume chip is enabled. The i915 gpu path keeps
// its PMU counters open across ticks; the only per-tick cost there is one
// read(2) per engine.
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
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <unistd.h>

#include <linux/perf_event.h>

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

// Read one plain non-negative integer from a file (sysfs "type" files, counters).
// Separate from readTempFile above, whose range check is about millidegrees C and
// would silently mangle any other kind of number. Returns -1 when unreadable.
static long readIntFile(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
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
// Two sources, tried in order, because "GPU load" is reported differently by
// each driver:
//
//   1. amdgpu publishes a ready-made percentage in sysfs
//      (/sys/class/drm/cardN/device/gpu_busy_percent). Unprivileged, one read,
//      no state. Used when present.
//   2. i915 publishes no such file - only per-engine BUSY COUNTERS on its PMU
//      (perf type at /sys/bus/event_source/devices/i915/type, events named
//      rcs0-busy, vcs0-busy, ...). A counter is cumulative nanoseconds of engine
//      activity, so a percentage needs two reads and the wall time between them.
//
// The i915 route is the one thing here that is not a plain file read: opening a
// PMU event is gated by kernel.perf_event_paranoid, which is 4 on this machine
// and denies unprivileged access outright. It is NOT worked around by weakening
// that global setting - instead the binary carries a CAP_PERFMON file capability
// (see linux/README.md), so exactly this program and no other process gets the
// permission. Without the capability the open() fails with EACCES and the chip
// reports "no value" exactly as it did before: a missing capability degrades to
// an honest em dash, never to a fabricated 0.
//
// The quantity matches Windows: p_metrics.c sums PDH's per-engine
// \GPU Engine(*)\Utilization Percentage and clamps the total at 100, so summing
// every i915 engine's busy fraction and clamping is the same measurement, not an
// analogy. Engines that are not present on the chip are simply absent from
// /sys/bus/event_source/devices/i915/events and are skipped.
//
// Cost: a full tick with both sources present is two sysfs reads plus, per
// sample, read(2) on each open counter. The counters stay OPEN across ticks (a
// 1 Hz poll re-opening 7 PMU events every second would pay the open cost 7 times
// a second for no reason) and only the deltas are computed here.
#define GPU_ENGINES_MAX 16
#define GPU_ENGINE_NAMES { "rcs0-busy", "bcs0-busy", "vcs0-busy", "vcs1-busy", \
                           "vecs0-busy", "ccs0-busy", "ccs1-busy", "ccs2-busy", \
                           "ccs3-busy", "rcs1-busy", "bcs1-busy", "vcs2-busy", \
                           "vcs3-busy", "vcs4-busy", "vcs5-busy", "vcs6-busy" }

typedef struct {
    int fd;              // perf event fd, -1 = not open
    unsigned long long prev; // cumulative busy ns at the previous sample
    int primed;          // 0 until the first successful read
} GpuCounter;

static GpuCounter g_gpuCtr[GPU_ENGINES_MAX];
static int g_gpuCtrN = -1;        // -1 = not probed yet, 0 = no PMU source
static unsigned long long g_gpuPrevNs; // monotonic clock at the previous sample

static unsigned long long monoNs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ULL + (unsigned long long)t.tv_nsec;
}

// One PMU event open. attr configuration is what the kernel's events/ files
// spell out: type from the i915 PMU directory, config from the event's own file.
static int gpuOpenEvent(unsigned type, unsigned long long config) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type = type;
    a.size = sizeof(a);
    a.config = config;
    a.disabled = 1;          // enabled explicitly once, then left running
    a.exclude_kernel = 1;    // utilisation is about what user work drove
    a.exclude_hv = 1;
    // pid = -1, cpu = 0 asks for the whole GPU, not one process's share. Engines
    // are not per-CPU on an iGPU, so any single CPU id is equivalent here.
    return (int)syscall(__NR_perf_event_open, &a, -1, 0, -1, 0);
}

// Probe the i915 PMU once. Returns the number of engines opened (0 = no usable
// source, in which case the sysfs path is used or the chip stays hidden).
static int gpuProbeI915(void) {
    static const char *names[] = GPU_ENGINE_NAMES;
    extern void writeLogA(const char *s); // the core's log (chocobar_core.h)
    long nt = readIntFile("/sys/bus/event_source/devices/i915/type");
    if (nt <= 0) return 0;
    unsigned type = (unsigned)nt;
    int n = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && n < GPU_ENGINES_MAX; i++) {
        char p[256];
        snprintf(p, sizeof(p), "/sys/bus/event_source/devices/i915/events/%s", names[i]);
        // the file holds "config=0x1234"; read it as hex
        FILE *f = fopen(p, "r");
        if (!f) continue;               // engine absent on this chip
        char line[64] = {0};
        int ok = fgets(line, sizeof(line), f) != NULL;
        fclose(f);
        if (!ok) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        unsigned long long cfg = strtoull(eq + 1, NULL, 0);
        int fd = gpuOpenEvent(type, cfg);
        if (fd < 0) {
            // EACCES = no CAP_PERFMON (the common, unprivileged case). Stop
            // probing: every further engine would fail identically. Say so once
            // in the log - a permanently absent chip with no explanation is the
            // kind of silence that reads as a bug in the bar.
            if (errno == EACCES || errno == EPERM) {
                if (n == 0)
                    writeLogA("[wizbar] gpu: i915 PMU needs CAP_PERFMON "
                              "(sudo setcap cap_perfmon+ep $(command -v chocobar-linux)) - "
                              "the gpu chip stays hidden until then");
                break;
            }
            continue;                    // engine present but unusable
        }
        g_gpuCtr[n].fd = fd;
        g_gpuCtr[n].prev = 0;
        g_gpuCtr[n].primed = 0;
        n++;
    }
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            ioctl(g_gpuCtr[i].fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(g_gpuCtr[i].fd, PERF_EVENT_IOC_ENABLE, 0);
        }
        g_gpuPrevNs = monoNs();
    }
    return n;
}

// Cumulative-busy-counter -> percentage. The first call only primes (a counter
// delta needs a previous sample, exactly like cpuPercent above). Each engine
// contributes its busy fraction; the sum is clamped at 100 to match Windows.
static int gpuI915Percent(void) {
    if (g_gpuCtrN < 0) g_gpuCtrN = gpuProbeI915();
    if (g_gpuCtrN <= 0) return -1;
    unsigned long long now = monoNs();
    unsigned long long wall = now - g_gpuPrevNs;
    if (wall == 0) return -1;
    double sum = 0;
    int got = 0;
    for (int i = 0; i < g_gpuCtrN; i++) {
        unsigned long long v = 0;
        if (read(g_gpuCtr[i].fd, &v, sizeof(v)) != (ssize_t)sizeof(v)) continue;
        if (g_gpuCtr[i].primed) {
            unsigned long long d = v - g_gpuCtr[i].prev; // counters only grow
            sum += 100.0 * (double)d / (double)wall;
            got = 1;
        }
        g_gpuCtr[i].prev = v;
        g_gpuCtr[i].primed = 1;
    }
    g_gpuPrevNs = now;
    if (!got) return -1;   // first sample: no delta yet, show nothing
    if (sum < 0) sum = 0;
    if (sum > 100) sum = 100;
    return (int)(sum + 0.5);
}

// amdgpu's ready-made percentage, and the ordered attempt at both sources.
static int gpuSysfsPercent(void) {
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

static int gpuPercent(void) {
    int v = gpuSysfsPercent();
    if (v >= 0) return v;
    return gpuI915Percent();
}

// ---- pet presence ------------------------------------------------------------
// The pet chip is "is this process alive" (p_ui.c builds it from
// modules.pet.exePath; Windows reads a Toolhelp32 snapshot). On Linux a process
// is named in /proc/<pid>/comm, which is the executable's basename truncated to
// 15 characters - the same rule pgrep applies, and the reason a match against
// the basename alone is the honest comparison.
//
// A script target (the pet is python3 /path/pet.py) never matches on comm:
// the kernel names the INTERPRETER ("python3"), not the script. So the primary
// test is argv[0] EQUALITY against exePath, which is exact for both a launcher
// that sets its own argv[0] (`exec -a "$0"`, what the shipped little-remielle
// wrapper does) and an interpreter holding the script as argv[1]. comm equality
// stays as the second test for a direct executable, skipped for interpreter
// names. Never a substring: stopping a process can only ever hit the configured
// target - the same rule as signalling a captured PID after re-checking it.
int petProcessRunning(const char *exePath);
int petProcessStop(const char *exePath);

// 1 = this pid IS the pet target, by argv[0] equality (see above). comm is the
// fallback for a plain executable, whose kernel name IS the basename.
static int petIsInterpreter(const char *want) {
    static const char *const kInterp[] = {
        "python", "python2", "python3", "sh", "bash", "zsh", "dash", "fish",
        "node", "bun", "deno", "perl", "ruby", "java", "env", NULL
    };
    for (int i = 0; kInterp[i]; i++)
        if (!strcasecmp(want, kInterp[i])) return 1;
    return 0;
}

static int petPidMatches(const char *exePath, const char *want, const char *pidName) {
    char p[300];
    snprintf(p, sizeof(p), "/proc/%s/cmdline", pidName);
    FILE *f = fopen(p, "r");
    if (!f) return 0;              // the process may have exited already
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n > 0) {
        buf[n] = 0;
        // argv[0] is the first token; trailing args are irrelevant, so a wrapper
        // that forwards arguments still matches. A `sh -c <path>` launcher has
        // the path as an ARGUMENT with "sh" as argv[0], so it never matches -
        // that is the whole point of comparing argv[0] and not "any token".
        if (!strcmp(buf, exePath)) return 1;
    }
    // the direct-executable case: the kernel names the binary (truncated to 15
    // chars), never the script it interprets
    if (petIsInterpreter(want)) return 0;
    snprintf(p, sizeof(p), "/proc/%s/comm", pidName);
    f = fopen(p, "r");
    if (!f) return 0;
    char comm[64] = {0};
    int got = fgets(comm, sizeof(comm), f) != NULL;
    fclose(f);
    if (!got) return 0;
    comm[strcspn(comm, "\n")] = 0;
    // case-insensitive, like lstrcmpiW on Windows
    return strcasecmp(comm, want) == 0;
}

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
        if (petPidMatches(exePath, want, e->d_name)) found = 1;
    }
    closedir(d);
    return found;
}

// Stop every process that is the pet target: SIGTERM, re-verifying the match
// against the pid's own cmdline immediately before the signal (the pid could
// have been recycled between the scan and the kill). Returns how many were
// signalled. Never SIGKILLs: the pet must save its position/settings first,
// and the kill is always a verified one, never a bare process-name pattern.
int petProcessStop(const char *exePath) {
    if (!exePath || !*exePath) return 0;
    const char *b = exePath, *q;
    for (q = exePath; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    char want[16] = {0};
    snprintf(want, sizeof(want), "%s", b);
    if (!want[0]) return 0;

    DIR *d = opendir("/proc");
    if (!d) return 0;
    int stopped = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        if (!petPidMatches(exePath, want, e->d_name)) continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid > 1 && kill(pid, SIGTERM) == 0) stopped++;
    }
    closedir(d);
    return stopped;
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
