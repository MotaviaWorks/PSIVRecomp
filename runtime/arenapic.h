/*
 * The Boss Arena's pictures: each boss as its battle shows it -- the battle's
 * backdrop, the enemies and their effects; not the party, the windows or the
 * numbers -- animated as it idles while the command menu waits.
 *
 * Taken from the player's own ROM at runtime, like the portraits: the arena
 * stages each fight unseen once (arena_set_scan), and this records the
 * frames the port draws, layer by layer (frame_lines): plane B, plane A's
 * low-priority cells (where the game draws enemies; the party, windows and
 * numbers are high priority) and the sprites. The loop is found and kept,
 * packed small for a cache file in the player's PSIV folder. Nothing of the
 * game ships with the port.
 */
#ifndef PSIV_ARENAPIC_H
#define PSIV_ARENAPIC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int w, h;
    int nframes;                  /* distinct pictures */
    uint8_t *px;                  /* nframes x w x h palette indices (0-63) */
    uint16_t *pal;                /* nframes x 64 CRAM words (9-bit BGR) */
    int nruns;                    /* the loop: which picture, for how many frames */
    uint8_t *run_frame, *run_len;
    int total;                    /* the loop's length in frames */
} ArenaPic;

/* Recording: begin, then once a frame (after it is drawn) until it returns 1 */
void arenapic_capture_begin(void);
int  arenapic_capture_frame(void);
/* the recording packed for the cache file (malloc'd; the caller frees it); 0 if nothing was recorded */
int  arenapic_capture_pack(uint8_t **out, size_t *len);

/* a cache file's bytes back into a picture; 1 on success */
int  arenapic_unpack(const uint8_t *data, size_t len, ArenaPic *pic);
void arenapic_free(ArenaPic *pic);
/* the picture to show at frame t of the loop (60 a second) */
int  arenapic_frame_at(const ArenaPic *pic, uint64_t t);

#endif
