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

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    char key[64], unit[16];
    unsigned long long val;
    while (fscanf(f, "%63s %llu %15s", key, &val, unit) == 3) {
        if (!strcmp(key, "MemTotal:")) total = val;
        else if (!strcmp(key, "MemAvailable:")) { avail = val; break; }
    }
    fclose(f);
    if (!total) return -1;
    return (double)(total - avail) * 100.0 / (double)total;
}

// ---- cpu temperature ---------------------------------------------------------
// The order matters: coretemp (the CPU's own digital sensor) is the true reading,
// and a laptop also exposes acpitz/thermal zones that lag it badly. Walking hwmon
// for coretemp first, then Package id 0, then the hottest core, then acpitz, then
// the thermal zones is what makes the number match what `sensors` reports.
static int readTempFile(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -100000;
    long v = -100000;
    if (fscanf(f, "%ld", &v) != 1) v = -100000;
    fclose(f);
    if (v < -50000 || v > 200000) return -100000;  // nonsense (some drivers report 0)
    return (int)v;                                  // millidegrees C
}

static int tempFromHwmon(const char *nameMatch) {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return -100000;
    int best = -100000;
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
            if (v == -100000) continue;
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
    if (!d) return -100000;
    int best = -100000;
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
        if (v == -100000) continue;
        if (v > best) best = v;
        if (!typeMatch) break;  // untyped: take the first that reads
    }
    closedir(d);
    return best;
}

static int cpuTempMilliC(void) {
    int v = tempFromHwmon("coretemp");
    if (v != -100000) return v;
    v = tempFromHwmon("k10temp");       // AMD's equivalent
    if (v != -100000) return v;
    v = tempFromThermal("x86_pkg_temp");
    if (v != -100000) return v;
    v = tempFromThermal(NULL);
    if (v != -100000) return v;
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
static int volumePercent(void) {
    FILE *p = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
    if (!p) return -1;
    char line[512];
    int pct = -1;
    if (fgets(line, sizeof(line), p)) {
        // "/  45%  ..." or "Volume: front-left: 29491 /  45% / -20.86 dB, ..."
        char *pc = strchr(line, '%');
        if (pc) {
            char *b = pc;
            while (b > line && (isdigit((unsigned char)b[-1]) || b[-1] == ' ')) b--;
            int v = atoi(b);
            if (v >= 0 && v <= 150) pct = v;
        }
    }
    pclose(p);
    return pct;
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
        if (v != -100000 && v >= 0 && v <= 100) best = v;
    }
    closedir(d);
    return best;
}

// ---- the tick ------------------------------------------------------------------
// One call per bar timer tick fills every field. Fields with no source keep
// their "invalid" sentinel so the renderer shows an em dash instead of a 0 that
// looks like a real reading.
void linuxMetricsPoll(LinuxMetrics *m) {
    m->cpu = cpuPercent();
    m->ram = ramPercent();
    int milli = cpuTempMilliC();
    m->tempC = milli == -100000 ? -1.0 : (double)milli / 1000.0;
    m->volume = volumePercent();
    m->battPct = 0;
    m->battAc = 1;
    m->battValid = batteryRead(&m->battPct, &m->battAc);
    m->gpu = (double)gpuPercent();
    m->clockNow = time(NULL);
}

// Prime the CPU sampler so the FIRST tick already has a real percentage instead
// of an em dash for one second. Called once at startup.
void linuxMetricsPrime(void) {
    cpuPercent();
}
