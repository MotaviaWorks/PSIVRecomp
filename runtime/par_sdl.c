/*
 * The app's helper threads for par_rows (par.h).
 *
 * A job is a loop's rows, handed out sixteen at a time from a shared counter:
 * a slow core (a phone's small ones) simply takes fewer pieces. Each helper
 * takes one "go" a job and answers with one "done", and the caller waits for
 * every answer before it returns, so no helper is ever still in a job when
 * the next one is set up.
 */
#include "par.h"

#include <SDL3/SDL.h>

#define MAX_HELPERS 7
#define PIECE 16

static SDL_Semaphore *go, *done;
static SDL_AtomicInt next_row;
static ParFn job_fn;
static void *job_ctx;
static int job_rows, helpers;

static void work(void) {
    for (;;) {
        int y = SDL_AddAtomicInt(&next_row, PIECE);          /* the value before: this piece's first row */
        if (y >= job_rows) return;
        job_fn(job_ctx, y, y + PIECE < job_rows ? y + PIECE : job_rows);
    }
}

static int SDLCALL helper(void *arg) {
    (void)arg;
    for (;;) {
        SDL_WaitSemaphore(go);
        work();
        SDL_SignalSemaphore(done);
    }
    return 0;
}

static void run(ParFn fn, void *ctx, int rows) {
    job_fn = fn;
    job_ctx = ctx;
    job_rows = rows;
    SDL_SetAtomicInt(&next_row, 0);
    for (int i = 0; i < helpers; i++) SDL_SignalSemaphore(go);
    work();
    for (int i = 0; i < helpers; i++) SDL_WaitSemaphore(done);
}

/* The worker for par_async: jobs wait in a short line and run one at a time;
 * each job's "done" is written and read under the line's lock. */
#define MAX_JOBS 8
static struct { ParJob fn; void *ctx; int *done; } jobs[MAX_JOBS];
static int job_first, job_count;
static SDL_Mutex *job_lock;
static SDL_Semaphore *job_ready;

static int SDLCALL worker(void *arg) {
    (void)arg;
    for (;;) {
        SDL_WaitSemaphore(job_ready);
        SDL_LockMutex(job_lock);
        ParJob fn = jobs[job_first].fn;
        void *ctx = jobs[job_first].ctx;
        int *job_done = jobs[job_first].done;
        SDL_UnlockMutex(job_lock);
        fn(ctx);
        SDL_LockMutex(job_lock);
        *job_done = 1;
        job_first = (job_first + 1) % MAX_JOBS;
        job_count--;
        SDL_UnlockMutex(job_lock);
    }
    return 0;
}

static void async_run(ParJob fn, void *ctx, int *job_done) {
    SDL_LockMutex(job_lock);
    if (job_count == MAX_JOBS) {                             /* the line is full: here and now */
        SDL_UnlockMutex(job_lock);
        fn(ctx);
        *job_done = 1;
        return;
    }
    int i = (job_first + job_count) % MAX_JOBS;
    *job_done = 0;
    jobs[i].fn = fn;
    jobs[i].ctx = ctx;
    jobs[i].done = job_done;
    job_count++;
    SDL_UnlockMutex(job_lock);
    SDL_SignalSemaphore(job_ready);
}

static int async_check(const int *job_done) {
    SDL_LockMutex(job_lock);
    int d = *job_done;
    SDL_UnlockMutex(job_lock);
    return d;
}

static void worker_start(void) {
    if (job_lock) return;
    job_lock = SDL_CreateMutex();
    job_ready = SDL_CreateSemaphore(0);
    SDL_Thread *t = job_lock && job_ready ? SDL_CreateThread(worker, "psiv-load", NULL) : NULL;
    if (!t) return;
    SDL_DetachThread(t);
    par_async_runner = async_run;
    par_async_check = async_check;
}

void par_sdl_start(void) {
    worker_start();
    int n = SDL_GetNumLogicalCPUCores() - 1;
    const char *v = SDL_getenv("PSIV_THREADS");
    if (v && *v) n = SDL_atoi(v) - 1;
    if (n > MAX_HELPERS) n = MAX_HELPERS;
    if (n < 1 || helpers) return;
    go = SDL_CreateSemaphore(0);
    done = SDL_CreateSemaphore(0);
    if (!go || !done) return;
    for (int i = 0; i < n; i++) {
        SDL_Thread *t = SDL_CreateThread(helper, "psiv-draw", NULL);
        if (!t) break;
        SDL_DetachThread(t);
        helpers++;
    }
    if (helpers) par_runner = run;
}

int par_sdl_threads(void) { return helpers + 1; }
