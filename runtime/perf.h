/*
 * Where a frame's time goes (a diagnostic, off unless asked for).
 *
 * With PSIV_PERF=1 in the environment the app times each frame's parts and
 * writes a line to its log about once a second: the average and the longest
 * frame, how many ran over the 16.7 ms a frame has, and each part's share.
 * Presentation only: nothing here touches the game.
 */
#ifndef PSIV_PERF_H
#define PSIV_PERF_H

enum {
    PERF_VDP,          /* drawing the game's lines */
    PERF_SOUND,        /* the sound chips' frame and the mix */
    PERF_COMPOSE,      /* the port's own picture: battle depth, weather, text, menus */
    PERF_UPLOAD,       /* handing the picture to the graphics card */
    PERF_PRESENT,      /* drawing it to the window */
    PERF_PARTS
};

void perf_enable(int on);
int  perf_enabled(void);
void perf_begin(int part);
void perf_end(int part);
/* A frame's work is done (before the wait for the next one): mode is the
 * game's mode number. Returns a line to log about once a second, else NULL. */
const char *perf_frame_done(int mode);
void perf_frame_start(void);       /* after the wait */

#endif
