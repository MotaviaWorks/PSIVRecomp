/*
 * Your own music (an option): audio files that play in place of the game's
 * songs -- a remix of your own, say. Nothing is shipped: you put the files
 * in the music folder (git-ignored):
 *
 *   music/8F.wav                 the song's ID in hex (PSIV_MUSIC_LOG=1
 *                                prints each song's ID as it starts)
 *   music/music.ini              or any name, and how it loops:
 *       8F = Meet Them Head-On (my remix).wav
 *       8F loop 12.5             loop back to 12.5 s at the end (or
 *       8F loop 12.5 98.0        from 98.0 s); a WAV's own loop (its
 *                                "smpl" chunk) is used if there is one
 *       8F gain -3               dB
 *       8F start 0.025           where it begins (default: its first sound;
 *                                an MP3 decoded to WAV keeps the encoder's
 *                                ~25 ms delay: add it to the loop points too)
 *
 * A loop's seam is blended over its last 15 ms into the same point a pass
 * earlier, so a rendered loop jumps without a click.
 *
 * WAV: 16/24/32-bit PCM or float, any rate, mono or stereo. The game's music
 * driver still runs; while a file plays, the chips (or Algol) play only the
 * channels a sound effect holds. The file starts when the song does, fades
 * with the game's fade-out, pauses with it, and stops when the game stops
 * the music or starts another song.
 */
#ifndef PSIV_MUSIC_H
#define PSIV_MUSIC_H

void music_set_folder(const char *dir);   /* NULL or "" = off */
void music_reset(void);                   /* the console reset */
void music_tick(void);                    /* each run of the music driver */
int  music_active(void);                  /* a file plays: the chips play the effects only */
unsigned music_fx_fm(void);               /* the channels effects hold: as algol_fm_effects() */
unsigned music_fx_psg(void);
void music_render(float *left, float *right);   /* one output sample, the chips' scale */

#endif
