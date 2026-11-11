/*
 * A loop's rows on several processor cores.
 *
 * The port's own picture (battle depth, the modern menus) is drawn a pixel
 * at a time at up to four times the game's size: too much for one core on a
 * small machine. Its loops go through par_rows, which hands the rows out in
 * pieces to the calling thread and to the helper threads the front end has
 * started (par_sdl.c; none in the headless tools, where it all runs on the
 * caller). Presentation only: the game itself stays on one thread.
 */
#ifndef PSIV_PAR_H
#define PSIV_PAR_H

/* Rows y0 to y1 - 1 of the loop. Pieces run at the same time: each may write
 * only its own rows, and read only what no piece writes. */
typedef void (*ParFn)(void *ctx, int y0, int y1);

/* Run fn over rows 0 to rows - 1; returns when all of them are done. */
void par_rows(ParFn fn, void *ctx, int rows);

/* The front end's runner (NULL: all on the calling thread). */
extern void (*par_runner)(ParFn fn, void *ctx, int rows);

/* A job off the game's thread (reading a large file, which would hold a
 * frame up for longer than the sound queue lasts): par_async starts it and
 * returns; *done is 0 until it has finished (ask par_async_done, each
 * frame). Jobs run one after another. Without a front end's worker (the
 * headless tools) the job runs at once, on the caller, as it always did. */
typedef void (*ParJob)(void *ctx);
void par_async(ParJob fn, void *ctx, int *done);
int  par_async_done(const int *done);
extern void (*par_async_runner)(ParJob fn, void *ctx, int *done);
extern int (*par_async_check)(const int *done);

/* The app's helpers (par_sdl.c): one for each further core, up to seven;
 * PSIV_THREADS=N in the environment asks for N threads in all (1: none). */
void par_sdl_start(void);
int  par_sdl_threads(void);        /* threads drawing, the caller included */

#endif
