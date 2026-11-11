/* Where a frame's time goes (perf.h). */
#include "perf.h"

#include <stdint.h>
#include <stdio.h>

#include <time.h>

/* (Windows: the C library's clock, 100 ns steps -- its headers' own clock
 * needs windows.h, which this compiler's settings do not take) */
static double now_ms(void) {
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#define BUDGET_MS (1000.0 / 59.92)
#define REPORT_FRAMES 60

static int on;
static double t0[PERF_PARTS], part[PERF_PARTS], frame_t0, sum, longest;
static int frames, over;

void perf_enable(int v) { on = v; frame_t0 = now_ms(); }
int  perf_enabled(void) { return on; }
void perf_begin(int p) { if (on) t0[p] = now_ms(); }
void perf_end(int p) { if (on) part[p] += now_ms() - t0[p]; }
void perf_frame_start(void) { if (on) frame_t0 = now_ms(); }

const char *perf_frame_done(int mode) {
    static char line[240];
    if (!on) return NULL;
    double ms = now_ms() - frame_t0;
    sum += ms;
    if (ms > longest) longest = ms;
    over += ms > BUDGET_MS;
    if (++frames < REPORT_FRAMES) return NULL;
    double n = (double)frames, others = 0;
    for (int i = 0; i < PERF_PARTS; i++) others += part[i];
    snprintf(line, sizeof line,
             "perf: mode %d: frame %.2f ms (longest %.2f, %d of %d over %.1f) = game %.2f + lines %.2f + sound %.2f"
             " + compose %.2f + upload %.2f + present %.2f",
             mode, sum / n, longest, over, frames, BUDGET_MS, (sum - others) / n, part[PERF_VDP] / n,
             part[PERF_SOUND] / n, part[PERF_COMPOSE] / n, part[PERF_UPLOAD] / n, part[PERF_PRESENT] / n);
    for (int i = 0; i < PERF_PARTS; i++) part[i] = 0;
    sum = longest = 0;
    frames = over = 0;
    return line;
}
