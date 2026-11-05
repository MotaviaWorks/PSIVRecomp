/*
 * Instrument banks from a SNES game you own: its samples, decoded from your
 * copy of the ROM when the port starts (nothing from it ships), for Algol to
 * play in place of its own synthesized instruments.
 *
 * A game is recognised by its header and the way it stores its samples (a
 * "recipe"); each known game comes with a voicing: which of its samples
 * plays each of Algol's instruments and drums. algol.ini can change any of
 * them: "bank lead = 35" (PSIV_ALGOL_LOG=1 lists the bank's samples).
 */
#ifndef PSIV_SNESBANK_H
#define PSIV_SNESBANK_H

typedef struct {
    float *pcm;          /* at 32 kHz (the S-DSP's own rate at pitch $1000), -1..1 */
    int len, loop;       /* the loop's start in samples; -1 = played once */
    float root;          /* its pitch in Hz at that rate */
    float level;         /* the level the game's envelope sustains at (0..1) */
    float decay;         /* the game's sustain decay: time constant in s (0 = it holds) */
    float gain;          /* evens out the samples' loudness */
} BankSample;

/* Recognise the ROM and decode its samples; 0 if it isn't a game we know
 * (or the file can't be read). A loaded bank stays until the next load. */
int snesbank_load(const char *rom_path);
const char *snesbank_name(void);          /* "" when none */
int snesbank_count(void);
const BankSample *snesbank_sample(int i);

/* The bank's voicing for one of Algol's instruments or drums, by name
 * ("lead", "bass", ..., "kick", "snare", "hat", "crash", "tom"): a sample
 * index, -1 for none (Algol's own sound stays); and a transposition in
 * semitones for an instrument. */
int  snesbank_voice(const char *name, float *transpose);
void snesbank_set_voice(const char *name, int sample, float transpose);

#endif
