/*
 * Battle voices (an Enhanced option): the party shout as they attack, draw
 * breath as they cast, grunt when a blow lands on them, groan as they fall,
 * and one of them breathes a little laugh when the battle is won.
 *
 * The voices are the port's own, made with Qwen3-TTS VoiceDesign (Apache-2.0)
 * from a written description of each character (tools/vo_gen.py), and ship in
 * the voices folder as <name>_<moment>_<n>.wav -- name chaz, alys, hahn, rune,
 * gryz, rika, demi, wren, raja, kyra, seth; moment attack, hurt, cast, ko,
 * win; n from 1. Files of the same names in the home folder's voices folder
 * take their place, so anyone can give a character their own voice.
 *
 * The moments come from the game's RAM, as for the rumble (rumble.c): the
 * acting fighter ($FF4142, fighter + 1, while the battle plays out) and their
 * command ($FF410A, 4 bytes a fighter: 1 attack, 2 technique, 3 skill); a
 * party fighter's hit reaction starting (routine 5, Fighters_Hit_Flags not
 * $FF); their HP reaching 0; the results starting. Not every swing or blow
 * gets a voice, so they don't tire; a falling member always does.
 */
#ifndef PSIV_BATTLEVOX_H
#define PSIV_BATTLEVOX_H

#include <stddef.h>
#include <stdint.h>

#define VOX_CHARS   11
#define VOX_MOMENTS 5
#define VOX_TAKES   8

void battlevox_set_enabled(int on);
int  battlevox_enabled(void);

/* the names in the files */
const char *battlevox_char_name(int c);      /* 0 Chaz .. 10 Seth (the game's order) */
const char *battlevox_moment_name(int m);    /* 0 attack, 1 hurt, 2 cast, 3 ko, 4 win */

/* a voice file's contents (a WAV); 1 if it was taken */
int  battlevox_add(int c, int m, int take, const uint8_t *wav, size_t len);
int  battlevox_count(void);                  /* takes loaded */

void battlevox_frame(void);                  /* once a frame, after the frame is drawn */
void battlevox_mix(float *samples, int frames);   /* add what is sounding (stereo) */
int  battlevox_sounding(void);
void battlevox_stop(void);                   /* silence (a rewind) */

#endif
