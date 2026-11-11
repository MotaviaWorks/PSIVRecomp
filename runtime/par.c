/* A loop's rows on several processor cores (par.h). */
#include "par.h"

#include <stddef.h>

void (*par_runner)(ParFn fn, void *ctx, int rows);

void (*par_async_runner)(ParJob fn, void *ctx, int *done);
int (*par_async_check)(const int *done);

void par_async(ParJob fn, void *ctx, int *done) {
    if (par_async_runner) { par_async_runner(fn, ctx, done); return; }
    fn(ctx);
    *done = 1;
}

int par_async_done(const int *done) { return par_async_check ? par_async_check(done) : *done; }

void par_rows(ParFn fn, void *ctx, int rows) {
    if (rows <= 0) return;
    if (par_runner && rows >= 32) par_runner(fn, ctx, rows);
    else fn(ctx, 0, rows);
}
