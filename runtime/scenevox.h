/*
 * Voiced scenes (an optional voice pack, not part of the port): when a page
 * of dialogue that has a voice line appears, the line plays.
 *
 * A pack is a folder (voices/scenes in the home folder) with an index,
 * scenes.txt, and the clips it names. The index has a line per page of the
 * script: the clip that starts on that page (or "-" for a page that carries
 * on the line before it), a tab, and the page's words as letters and digits
 * only, lower case. tools: dist/scene_vo.py makes the clips and the index
 * from the disassembly's script.
 *
 * The page on screen is read the way the modern text reads it: the
 * dialogue font's letters in the dialogue box (hdtext_dialogue_key). Once
 * what has been typed so far fits only one page of the index (or is all of
 * one), that page is the one: a new line starts, and the one sounding fades
 * out. A line plays to its end even if the box is closed or skipped ahead.
 */
#ifndef PSIV_SCENEVOX_H
#define PSIV_SCENEVOX_H

#include <stddef.h>
#include <stdint.h>
#include "vdp.h"

void scenevox_set_enabled(int on);

/* the pack: its index's contents, and a function that reads a clip by its
 * file name (malloc'd contents, or NULL); the number of pages it voices */
int  scenevox_load(const char *index, size_t len, void *(*read)(const char *name, size_t *len));
int  scenevox_pages(void);                 /* the pages the loaded pack voices (0: none) */
void scenevox_unload(void);                /* forget the pack (before loading another) */

void scenevox_frame(const VdpLine *lines, int w, int h);   /* once a frame, after it is drawn */
void scenevox_mix(float *samples, int frames);
int  scenevox_sounding(void);
void scenevox_stop(void);

/* The party's quips (the pack's quips.txt: clip, a tab, who, a tab, the words), said as the voiced scenes
 * come on. scenevox_quip: one at random, never the same twice running -- who says it, the words, and its
 * sound, 16-bit stereo at SOUND_RATE (NULL if the pack has none). scenevox_say plays such a sound through
 * scenevox_mix; scenevox_gain is the level the mixer gives the voices, for a caller that plays it itself. */
int  scenevox_quips(void);
const int16_t *scenevox_quip(const char **who, const char **text, int *frames);
void scenevox_say(const int16_t *pcm, int frames);
float scenevox_gain(void);

/* How far the screen is in a flashback's look (0..1, eased): pages marked
 * "memory" in the index (a third column) bring it in, others take it out. */
float scenevox_memory(void);

/* Auto-advance (a button toggles it): on a voiced page, once its words have
 * been said -- for a line over two pages, its share of the clip -- and a
 * beat after, C is pressed for the player. The pad the game sees this frame:
 * `user` is what the player holds (any of it comes first), `pad` the rest. */
void scenevox_set_auto(int on);
int  scenevox_auto(void);
uint8_t scenevox_pad(uint8_t user, uint8_t pad);

#endif
