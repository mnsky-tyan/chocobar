// linux_metrics.h - the values the Linux bar's metric chips show.
//
// Mirrors the fields native/src/p_metrics.c fills on Windows (its Metrics struct)
// so the renderer's rules - warn thresholds, the em dash for a missing sensor,
// green while charging - are written once and behave the same. Invalid readings
// are negative (or battValid = 0), never a fabricated 0: a fanless laptop with no
// temp sensor must show "-", not "0 C".
#ifndef LINUX_METRICS_H
#define LINUX_METRICS_H

#include <time.h>

typedef struct {
    double cpu;        // percent, -1 until primed / unreadable
    double ram;        // percent, -1 unreadable
    double tempC;      // degrees C, -1 no sensor
    int volume;        // percent 0..150, -1 no mixer
    int battPct;       // percent when battValid
    int battAc;        // 1 = on mains / charging / full
    int battValid;     // 0 = no battery (desktop)
    double gpu;        // percent, -1 no source (i915 PMU needs CAP_PERFMON)
    int petRunning;    // 1 = the pet process is alive (modules.pet.exePath)
    time_t clockNow;   // local wall clock for the clock chip
} LinuxMetrics;

void linuxMetricsPoll(LinuxMetrics *m);
void linuxMetricsPrime(void);

// the pet process (modules.pet.exePath): is it alive, and stop it. Implemented
// in p_metrics_linux.c because the /proc walk lives there; the bar's click
// handler calls both.
int petProcessRunning(const char *exePath);
int petProcessStop(const char *exePath);

#endif
