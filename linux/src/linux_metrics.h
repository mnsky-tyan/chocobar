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
// handler calls both. The pet is the process whose EXECUTED file is the
// configured path: argv[0] equal to the path (or its realpath), or an
// interpreter argv[0] with the path at the FIRST token after it that is not one
// of the interpreter's own options - the kernel shebang rule is
// `<interpreter> [optional-arg] <script>`, so a shebang that carries an
// argument puts the script at argv[2] - with a relative token resolved against
// that process's own cwd. The scan stops at that token: every token after it
// is an argument to the script, never the file the kernel executed, so
// `python3 report.py <path>` runs report.py and is not the pet. An option that
// takes a command rather than a script ("-c") ends the scan too, so
// `sh -c <path>` is not the pet either. The interpreter is matched by basename
// plus an optional version suffix (python3.14, node20), because that is the
// name the kernel gives the live process; `env` is not in the list, because env
// replaces ITSELF with the interpreter it resolved.
// A process that merely carries the path as an argument is not the pet. The
// configured value must be ONE ABSOLUTE path to a regular executable file - a
// bare name, a relative path, a directory, or a command line carrying arguments
// is refused for both matching and starting.
int petProcessRunning(const char *exePath);
int petProcessStop(const char *exePath);
int petPathUsable(const char *exePath);

#endif
