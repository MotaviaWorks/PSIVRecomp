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
 *                                "smpl" chunk) is used if there is one; with
 *                                neither, it starts again from its beginning
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
 *
 * Wave Mode (wave.h) has a file of its own, a medley say:
 *
 *       wave = Battle Medley.wav     (or music/wave.wav)
 *       wave loop 6.4 220.0          as a song's; start and gain too
 *       wave cue 2 32.0              battle 2's part of the file begins at 32.0 s
 *
 * It starts with a run and plays to its end -- through the five battles, the
 * walks between them and the maps changing -- in place of every song the game
 * starts on the way. With cue lines, a battle starting takes the file to that
 * battle's part (a short dip, no click), unless it is in that part already or
 * within 8 s of reaching it. No file: a run plays the game's songs, as ever.
 */
#ifndef PSIV_MUSIC_H
#define PSIV_MUSIC_H

void music_set_folder(const char *dir);   /* NULL or "" = off */
void music_reset(void);                   /* the console reset */
void music_tick(void);                    /* each run of the music driver */
void music_wave_stop(void);               /* a Wave Mode run is over: its file fades out */
int  music_active(void);                  /* a file plays: the chips play the effects only */
int  music_enabled(void);                 /* a folder is set: files may play */
unsigned music_fx_fm(void);               /* the channels effects hold: as algol_fm_effects() */
unsigned music_fx_psg(void);
void music_render(float *left, float *right);   /* one output sample, the chips' scale */

#endif
