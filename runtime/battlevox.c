/*
 * Battle voices: see battlevox.h.
 */
#include "battlevox.h"
#include "m68k.h"
#include "sfx.h"
#include "sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE        0xEF00u           /* Game_Mode_Index: / 4 = 5 in a battle */
#define BATTLE_ROUTINE_2 0x4108u           /* 0 while the battle plays out; results $1E-$26, $2D-$2F */
#define COMMAND_DATA     0x410Au           /* per fighter, 4 bytes: command, ID, target */
#define CURRENT_ACTOR    0x4142u           /* while the battle plays out: fighter + 1 */
#define FIGHTERS         0x4400u
#define HIT_FLAGS        0x4150u
#define CHAR_STATS       0xF500u
#define R_HIT            5

enum { M_ATTACK, M_HURT, M_CAST, M_KO, M_WIN };
static const char *const CHARS[VOX_CHARS] = {"chaz", "alys", "hahn", "rune", "gryz", "rika", "demi", "wren", "raja", "kyra", "seth"};
static const char *const MOMENTS[VOX_MOMENTS] = {"attack", "hurt", "cast", "ko", "win"};
/* how often a moment gets a voice */
static const float CHANCE[VOX_MOMENTS] = {0.7f, 0.8f, 1.0f, 1.0f, 1.0f};
#define GAIN 0.55f                          /* under the music and the blows */

static int enabled = 1, loaded;
static int16_t *pcm[VOX_CHARS][VOX_MOMENTS][VOX_TAKES];
static int frames[VOX_CHARS][VOX_MOMENTS][VOX_TAKES], ntakes[VOX_CHARS][VOX_MOMENTS], last_take[VOX_CHARS][VOX_MOMENTS];
static uint32_t rng = 0x6D2B79F5u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float rnd01(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }

void battlevox_set_enabled(int on) { enabled = on; if (!on) battlevox_stop(); }
int  battlevox_enabled(void) { return enabled; }
const char *battlevox_char_name(int c) { return c >= 0 && c < VOX_CHARS ? CHARS[c] : "?"; }
const char *battlevox_moment_name(int m) { return m >= 0 && m < VOX_MOMENTS ? MOMENTS[m] : "?"; }
int battlevox_count(void) { return loaded; }

int battlevox_add(int c, int m, int take, const uint8_t *wav, size_t len) {
    if (c < 0 || c >= VOX_CHARS || m < 0 || m >= VOX_MOMENTS || take < 0 || take >= VOX_TAKES) return 0;
    int n = 0;
    int16_t *p = sfx_decode_wav(wav, (long)len, &n);
    if (!p) return 0;
    if (pcm[c][m][take]) free(pcm[c][m][take]);
    else { loaded++; if (take + 1 > ntakes[c][m]) ntakes[c][m] = take + 1; }
    pcm[c][m][take] = p;
    frames[c][m][take] = n;
    return 1;
}

/* ------------------------------------------------------------------ playing */
typedef struct { const int16_t *p; int n, pos, ch; float fade, step; } Voice;
static Voice voice[4];

int battlevox_sounding(void) {
    for (int i = 0; i < 4; i++) if (voice[i].p) return 1;
    return 0;
}

void battlevox_stop(void) { memset(voice, 0, sizeof voice); }

static void say(int c, int m) {
    if (!enabled || c < 0 || c >= VOX_CHARS || !ntakes[c][m]) return;
    int n = ntakes[c][m], k = (int)(rnd() % (uint32_t)n);
    if (n > 1 && k == last_take[c][m]) k = (k + 1) % n;              /* never the same take twice running */
    if (!pcm[c][m][k]) return;
    last_take[c][m] = k;
    static int log_on = -1;                                             /* PSIV_VOX_LOG=1: each voice */
    if (log_on < 0) { const char *e = getenv("PSIV_VOX_LOG"); log_on = e && *e == '1'; }
    if (log_on) printf("voice: %s %s %d\n", CHARS[c], MOMENTS[m], k + 1);
    int slot = -1;
    for (int i = 0; i < 4; i++)                                         /* a character says one thing at a time */
        if (voice[i].p && voice[i].ch == c) voice[i].step = -1.0f / (0.025f * SOUND_RATE);
    for (int i = 0; i < 4 && slot < 0; i++) if (!voice[i].p) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < 4; i++) if (voice[i].pos > voice[slot].pos) slot = i;
    }
    voice[slot] = (Voice){pcm[c][m][k], frames[c][m][k], 0, c, 1.0f, 0.0f};
}

void battlevox_mix(float *samples, int n) {
    for (int i = 0; i < 4; i++) {
        Voice *v = &voice[i];
        if (!v->p) continue;
        for (int j = 0; j < n; j++) {
            if (v->pos >= v->n || v->fade <= 0.0f) { v->p = NULL; break; }
            float g = GAIN * v->fade;
            samples[j * 2] += (float)v->p[v->pos * 2] * g;
            samples[j * 2 + 1] += (float)v->p[v->pos * 2 + 1] * g;
            v->pos++;
            v->fade += v->step;
        }
    }
}

/* ------------------------------------------------------------------ the moments */
static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }

static struct { int valid; uint32_t stats; int hp; unsigned routine; } seen[5];
static unsigned last_actor, last_r2;
static int fought;                                     /* the battle has played out a turn */
static int pending_c = -1, pending_m, pending_wait;    /* an attack shout waits for the swing */

static int char_of(int k) {
    uint32_t f = FIGHTERS + 0x40u * (uint32_t)k;
    if (!r16(f)) return -1;
    unsigned stats = r16(f + 6);
    if ((r16(f + 4) & 0xFF) != 0xFF || stats < CHAR_STATS || stats >= CHAR_STATS + VOX_CHARS * 0x80 || (stats - CHAR_STATS) % 0x80) return -1;
    return (int)((stats - CHAR_STATS) / 0x80);
}

/* a low-priority sprite on screen: someone mid-swing */
static int swinging(void);

void battlevox_frame(void) {
    if (r16(GAME_MODE) / 4 != 5) {
        memset(seen, 0, sizeof seen);
        last_actor = last_r2 = 0;
        fought = 0;
        pending_c = -1;
        return;
    }
    if (!enabled || !loaded) return;
    unsigned r2 = r16(BATTLE_ROUTINE_2) & 0x7FFF;
    /* a party fighter's turn: a shout with the swing, a breath as they cast */
    unsigned actor = r2 == 0 ? r16(CURRENT_ACTOR) : 0;
    if (actor >= 1 && actor <= 9) fought = 1;
    if (actor != last_actor && actor >= 1 && actor <= 5) {
        int k = (int)actor - 1, c = char_of(k);
        unsigned cmd = r8(COMMAND_DATA + (unsigned)k * 4);
        if (c >= 0 && cmd == 1 && rnd01() < CHANCE[M_ATTACK]) { pending_c = c; pending_m = M_ATTACK; pending_wait = 30; }
        else if (c >= 0 && (cmd == 2 || cmd == 3)) { pending_c = c; pending_m = M_CAST; pending_wait = 4; }
    }
    last_actor = actor;
    if (pending_c >= 0) {
        if (pending_m == M_CAST ? --pending_wait <= 0 : (swinging() || --pending_wait <= 0)) {
            say(pending_c, pending_m);
            pending_c = -1;
        }
    }
    /* blows landing on the party, and anyone falling */
    int hit[5], nhit = 0;
    for (int k = 0; k < 5; k++) {
        uint32_t f = FIGHTERS + 0x40u * (uint32_t)k;
        uint32_t stats = (uint32_t)r16(f + 4) << 16 | r16(f + 6);
        int c = char_of(k);
        if (c < 0) { seen[k].valid = 0; continue; }
        int hp = (int16_t)r16((stats & 0xFFFF) + 0xE);
        unsigned routine = r16(f + 2) & 0x7FFF;
        if (seen[k].valid && seen[k].stats == stats) {
            if (routine == R_HIT && seen[k].routine != R_HIT && r8(HIT_FLAGS + (unsigned)k) != 0xFF && hp > 0) hit[nhit++] = k;
            if (seen[k].hp > 0 && hp <= 0) say(c, M_KO);
        }
        seen[k].valid = 1;
        seen[k].stats = stats;
        seen[k].hp = hp;
        seen[k].routine = routine;
    }
    if (nhit && rnd01() < CHANCE[M_HURT]) say(char_of(hit[rnd() % (uint32_t)nhit]), M_HURT);  /* one voice for a group hit */
    /* won: one of those standing */
    int results = (r2 >= 0x1E && r2 <= 0x26) || (r2 >= 0x2D && r2 <= 0x2F);
    int was = (last_r2 >= 0x1E && last_r2 <= 0x26) || (last_r2 >= 0x2D && last_r2 <= 0x2F);
    if (results && !was && fought) {
        int up[5], nup = 0;
        for (int k = 0; k < 5; k++) if (seen[k].valid && seen[k].hp > 0) up[nup++] = k;
        if (nup) say(char_of(up[rnd() % (uint32_t)nup]), M_WIN);
    }
    last_r2 = r2;
}

#include "vdp.h"
static int swinging(void) {
    uint32_t sat = (uint32_t)(vdp.reg[5] & 0x7E) << 9;
    int link = 0, count = 0;
    do {
        const uint8_t *s = &vdp.vram[(sat + (uint32_t)link * 8) & 0xFFFF];
        const uint8_t *c = &vdp.sat_cache[((uint32_t)link * 8) & 0x3FF];
        int sy = ((c[0] << 8 | c[1]) & 0x3FF) - 128, sx = ((s[6] << 8 | s[7]) & 0x1FF) - 128;
        if (!(s[4] & 0x80) && sx > -32 && sx < 352 && sy > -32 && sy < 224) return 1;
        link = c[3] & 0x7F;
    } while (link && ++count < 80);
    return 0;
}
