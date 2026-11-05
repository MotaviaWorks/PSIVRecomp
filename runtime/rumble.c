/*
 * Rumble: see rumble.h.
 *
 * Where the moments come from (names from ps4.constants.asm):
 *   - Obj_Fighters ($FF4400): 9 fighters of $40 bytes, the party (0-4) then
 *     the enemies (5-8); a slot whose first word is 0 is empty. +2 is the
 *     fighter's routine (bit 15: started), +4 points at its stats -- curr_hp
 *     +$E, max_hp +$10 -- and +$12 holds an enemy's ID.
 *   - A blow landing: the target's routine becomes 5, its hit reaction
 *     (Character_DamageAnimation / Enemy_DamageAnimation: the blink, the
 *     white flash of a critical) -- with the sword's slash sound 8 frames
 *     before, an enemy's blow's sound on the same frame. Fighters_Hit_Flags
 *     ($FF4150, a byte per fighter) says 0 hit, 1 critical, $FF missed or not
 *     a target; Battle_Heal_Damage_List ($FF415A, a word per fighter) holds
 *     the damage for some attacks already (swords), not yet for others (the
 *     boomerang, enemies' blows: 0 or -1). The HP only goes down about a
 *     second later, when the damage number has been shown
 *     (FighterShowDamage_DecreaseHP) -- that's when a party member falls.
 *   - A critical hit: one big blow with the white flash. (The game also
 *     plays the hit sound three more times, 4, 8 and 12 frames on -- but for
 *     swords and claws those start 8 frames before the flash, so beats in
 *     step with the flash would trail the sound.)
 *   - Enemy_Dead asks for SFXID_EnemyKilled ($B9) as an enemy dissolves --
 *     except the final bosses (Dark Force $82-$84, Profound Darkness
 *     $85-$87, Zio's last form $8C), which die their own way: for them, the
 *     HP reaching 0.
 *   - the sound queue (Sound_Index $FF500A, 4 slots): the game's requests,
 *     which the sound driver takes (and clears) in VInt. A slot counts when
 *     it changed since the driver last ran, so a request left waiting (the
 *     driver paused) isn't felt twice.
 */
#include "rumble.h"
#include "m68k.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE   0xEF00u           /* Game_Mode_Index: / 4 = 5 in a battle */
#define FIGHTERS    0x4400u
#define N_FIGHTERS  9
#define N_PARTY     5
#define HIT_FLAGS   0x4150u
#define DAMAGE      0x415Au
#define SOUND_QUEUE 0x500Au
#define R_HIT       5                 /* the hit reaction routine */

typedef struct {
    float lo, hi;                             /* peak per motor, 0..1 */
    uint16_t delay, attack, hold, release;    /* ms */
} Tap;

/* ------------------------------------------------------------------ the effects */
/*                                   low    high  delay attack hold release */
static const Tap ENEMY_HIT   = {0.40f, 0.85f,    0,    0,   45,  110};   /* times the hit's strength */
static const Tap PARTY_HIT   = {0.90f, 0.45f,    0,    0,   70,  200};   /* times the hit's strength */
static const Tap CRITICAL    = {1.00f, 1.00f,    0,    0,  110,  300};   /* with the white flash */
static const Tap PARTY_DOWN  = {1.00f, 0.70f,    0,    0,  200,  550};
static const Tap BOSS_DOWN   = {1.00f, 0.60f,    0,    0,  800, 1800};
static const Tap SAMPLE      = {0.80f, 0.60f,    0,    0,   90,  260};

/* Sounds the game asks for (SFXID_* / MusicID_*), up to three taps each,
 * shaped after the sound itself: its length and loudness over time, measured
 * from the original (rendered with psiv_headless --music ID:60 --wav). The
 * conveyor belt is left out: it loops for as long as the belt runs. */
typedef struct { uint8_t id; const char *name; Tap t[3]; } SoundRumble;
static const SoundRumble SOUNDS[] = {
    /* battles */
    {0xB9, "enemy destroyed", {{0.70f, 0.35f, 0, 0, 150, 600}}},              /* 0.9 s, fading */
    {0xC0, "Megid",           {{0.75f, 0.40f, 0, 30, 600, 650}}},             /* 1.3 s */
    {0xC3, "Efess",           {{0.80f, 0.50f, 0, 30, 550, 250}}},             /* 0.8 s, even */
    {0xC7, "Gra",             {{0.65f, 0.15f, 0, 0, 120, 120}}},              /* 0.16 s */
    {0xEA, "black wave",      {{0.70f, 0.30f, 0, 380, 150, 500}}},            /* swells to 0.4 s, 1 s */
    {0xEC, "lightning",       {{0.15f, 0.30f, 100, 300, 100, 100},            /* a quiet build, */
                               {0.45f, 0.85f, 550, 0, 80, 150},               /* the crack, */
                               {0.35f, 0.60f, 650, 50, 700, 550}}},           /* the roll: to 2 s */
    {0xC2, "fire breath",     {{0.45f, 0.35f, 50, 100, 150, 100}}},           /* 0.35 s */
    {0xD4, "war cry",         {{0.55f, 0.20f, 0, 40, 550, 150}}},             /* 0.7 s */
    /* the field */
    {0xAD, "explosion",       {{1.00f, 0.70f, 0, 200, 300, 600},              /* a blast, */
                               {0.55f, 0.25f, 400, 300, 1800, 1000}}},        /* rumbling on: 3.5 s */
    {0xE6, "barrier broken",  {{0.80f, 0.90f, 0, 0, 60, 60},                  /* a crack, */
                               {0.50f, 0.70f, 200, 0, 80, 450}}},             /* then it shatters */
    {0xE3, "ship's engines",  {{0.60f, 0.30f, 0, 100, 400, 700}}},            /* 1.25 s */
    {0xDD, "stone slab",      {{0.50f, 0.20f, 0, 30, 300, 800}}},             /* 1.2 s, grinding down */
    {0xE4, "power down",      {{0.45f, 0.10f, 0, 50, 1300, 800}}},            /* 2.2 s */
    {0xE0, "elevator ride",   {{0.30f, 0.08f, 0, 100, 550, 200}}},            /* 0.8 s */
    {0xE5, "elevator doors",  {{0.35f, 0.15f, 0, 0, 120, 100}}},              /* 0.2 s */
    {0xE2, "door",            {{0.30f, 0.12f, 0, 50, 850, 400}}},             /* 1.3 s, sliding */
    {0xEE, "ice broken",      {{0.40f, 0.60f, 0, 0, 150, 500}}},              /* 0.7 s */
    {0xEF, "falling",         {{0.35f, 0.25f, 0, 100, 450, 120}}},            /* 0.65 s */
    {0xE1, "chest",           {{0.10f, 0.35f, 0, 0, 60, 150}}},               /* 0.3 s */
    {0xDE, "teleport",        {{0.08f, 0.30f, 0, 200, 250, 600}}},            /* 1.1 s */
    {0xF0, "telepipe",        {{0.08f, 0.30f, 0, 150, 300, 550}}},            /* 1 s */
};
#define N_SOUNDS (int)(sizeof SOUNDS / sizeof SOUNDS[0])

/* ------------------------------------------------------------------ playing */
typedef struct { Tap t; uint64_t start; uint8_t timed, begun; } Voice;
#define MAX_VOICES 32
static Voice voices[MAX_VOICES];
static int nvoices;
static int level = 2;
static const float GAIN[RUMBLE_LEVELS] = {0.0f, 0.45f, 0.75f, 1.0f};
static const char *const NAMES[RUMBLE_LEVELS] = {"off", "light", "normal", "strong"};
static float pending;                       /* the strongest tap that began in the last rumble_motors */
static int pending_ms;
static uint64_t pulse_end;                  /* the pulse a one-motor vibrator is playing */
static float pulse_strength;
static void (*log_line)(const char *);

void rumble_set_level(int l) { level = l < 0 ? 0 : l >= RUMBLE_LEVELS ? RUMBLE_LEVELS - 1 : l; if (!level) rumble_stop(); }
int rumble_get_level(void) { return level; }
const char *rumble_level_name(int l) { return NAMES[l < 0 || l >= RUMBLE_LEVELS ? 0 : l]; }
void rumble_set_log(void (*log)(const char *line)) { log_line = log; }

static void note(const char *fmt, ...) {
    if (!log_line) return;
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log_line(b);
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* start an effect (scaled), `delay` ms after the next rumble_motors */
static void play(const Tap *t, float scale, int delay) {
    if (!level || nvoices >= MAX_VOICES || scale <= 0.0f) return;
    Voice *v = &voices[nvoices++];
    v->t = *t;
    v->t.lo = clamp01(t->lo * scale);
    v->t.hi = clamp01(t->hi * scale);
    v->t.delay = (uint16_t)(t->delay + delay);
    v->timed = v->begun = 0;
}

static float envelope(const Tap *t, float ms) {
    if (ms < 0.0f) return 0.0f;
    if (ms < (float)t->attack) return ms / (float)t->attack;
    ms -= (float)t->attack;
    if (ms < (float)t->hold) return 1.0f;
    ms -= (float)t->hold;
    if (ms < (float)t->release) {                       /* a quadratic tail: rings out */
        float x = 1.0f - ms / (float)t->release;
        return x * x;
    }
    return 0.0f;
}

void rumble_motors(uint64_t now, float *low, float *high) {
    float keep_lo = 1.0f, keep_hi = 1.0f;               /* motors add up and saturate: 1 - prod(1 - a) */
    int n = 0;
    pending = 0.0f;
    for (int i = 0; i < nvoices; i++) {
        Voice v = voices[i];
        if (!v.timed) { v.start = now + v.t.delay; v.timed = 1; }
        if (now >= v.start) {
            float ms = (float)(now - v.start);
            if (ms >= (float)(v.t.attack + v.t.hold + v.t.release)) continue;   /* over */
            if (!v.begun) {                             /* a pulse for one-motor vibrators */
                float s = v.t.lo > v.t.hi ? v.t.lo : v.t.hi;
                if (s > pending) { pending = s; pending_ms = v.t.attack + v.t.hold + v.t.release / 2; }
                v.begun = 1;
            }
            float e = envelope(&v.t, ms);
            keep_lo *= 1.0f - v.t.lo * e;
            keep_hi *= 1.0f - v.t.hi * e;
        }
        voices[n++] = v;
    }
    nvoices = n;
    *low = (1.0f - keep_lo) * GAIN[level];
    *high = (1.0f - keep_hi) * GAIN[level];
}

int rumble_take_pulse(uint64_t now, float *strength, int *ms) {
    if (pending <= 0.0f || !level) return 0;
    float s = pending;
    pending = 0.0f;
    if (now < pulse_end && s < pulse_strength) return 0;    /* a stronger one is still going */
    if (pending_ms < 20) pending_ms = 20;                    /* shorter isn't felt */
    if (pending_ms > 500) pending_ms = 500;                  /* many phone motors can't go softer: a
                                                                long, gentle effect would be a long buzz */
    pulse_end = now + (uint64_t)pending_ms;
    pulse_strength = s;
    *strength = clamp01(s * GAIN[level]);
    *ms = pending_ms;
    return 1;
}

void rumble_stop(void) {
    nvoices = 0;
    pending = 0.0f;
    pulse_end = 0;
}

void rumble_test(void) {
    play(&SAMPLE, 1.0f, 0);
}

/* ------------------------------------------------------------------ the game */
static struct { uint32_t stats; int16_t hp; uint16_t routine; uint8_t valid; } seen[N_FIGHTERS];
static uint8_t queue_left[4];

static uint16_t ram16(uint32_t a) {
    a &= 0xFFFFu;
    return (uint16_t)(m68k_ram[a] << 8 | m68k_ram[(a + 1) & 0xFFFFu]);
}

static void sound(uint8_t id) {
    for (int i = 0; i < N_SOUNDS; i++) {
        if (SOUNDS[i].id != id) continue;
        note("sound $%02X (%s)", id, SOUNDS[i].name);
        for (int k = 0; k < 3 && (SOUNDS[i].t[k].hold || SOUNDS[i].t[k].release); k++) play(&SOUNDS[i].t[k], 1.0f, 0);
        return;
    }
}

void rumble_frame(void) {
    static int every_sound = -1;                        /* testing: PSIV_RUMBLE_SOUNDS=1 logs every request */
    if (every_sound < 0) every_sound = getenv("PSIV_RUMBLE_SOUNDS") != NULL;
    for (int i = 0; i < 4; i++) {                       /* this frame's sound requests */
        uint8_t id = m68k_ram[SOUND_QUEUE + i];
        if (!id || id == queue_left[i]) continue;
        if (every_sound) note("request $%02X", id);
        sound(id);
    }
    if (ram16(GAME_MODE) / 4 != 5) {                    /* not a battle */
        memset(seen, 0, sizeof seen);
        return;
    }
    float enemy = 0.0f, party = 0.0f;                   /* the hardest hit on each side this frame */
    int n_enemy = 0, n_party = 0, enemy_crit = 0, party_crit = 0, fallen = 0, boss = 0;
    for (int k = 0; k < N_FIGHTERS; k++) {
        uint32_t f = FIGHTERS + 0x40u * (uint32_t)k;
        uint32_t stats = (uint32_t)ram16(f + 4) << 16 | ram16(f + 6);
        if (!ram16(f) || (stats & 0xFF0000u) != 0xFF0000u) {   /* an empty slot */
            seen[k].valid = 0;
            continue;
        }
        int hp = (int16_t)ram16(stats + 0xE), max = (int16_t)ram16(stats + 0x10);
        uint16_t routine = ram16(f + 2) & 0x7FFFu;
        if (seen[k].valid && seen[k].stats == stats) {
            uint8_t flag = m68k_ram[HIT_FLAGS + k];
            if (routine == R_HIT && seen[k].routine != R_HIT && flag != 0xFF) {   /* a blow lands */
                /* the damage, if the game has worked it out yet (a sword's
                 * has; a boomerang's or an enemy's comes later): stronger the
                 * more of the target's HP it takes */
                int dmg = (int16_t)ram16(DAMAGE + 2u * (uint32_t)k);
                float size = max > 0 && dmg > 0 ? (float)dmg / (float)max : -1.0f;
                if (k < N_PARTY) {
                    float s = size < 0.0f ? 0.8f : clamp01(0.55f + 1.35f * size);   /* a third of their HP: full */
                    if (s > party) party = s;
                    n_party++;
                    party_crit |= flag == 1;
                } else {
                    float s = size < 0.0f ? 0.7f : clamp01(0.5f + 1.0f * size);     /* half its HP: full */
                    if (s > enemy) enemy = s;
                    n_enemy++;
                    enemy_crit |= flag == 1;
                }
                if (size < 0.0f) note("hit %s %d (%d HP)%s", k < N_PARTY ? "party" : "enemy", k, max,
                                      flag == 1 ? ", critical" : "");
                else note("hit %s %d: %d of %d HP%s", k < N_PARTY ? "party" : "enemy", k, dmg, max,
                          flag == 1 ? ", critical" : "");
            }
            if (seen[k].hp > 0 && hp <= 0) {             /* the damage shown: someone falls */
                uint16_t id = ram16(f + 0x12);
                if (k < N_PARTY) fallen = 1;
                else boss |= (id >= 0x82 && id <= 0x87) || id == 0x8C;
                note("%s %d down", k < N_PARTY ? "party" : "enemy", k);
            }
        }
        seen[k].stats = stats;
        seen[k].hp = (int16_t)hp;
        seen[k].routine = routine;
        seen[k].valid = 1;
    }
    /* several at once (a technique, the boomerang): a little more */
    if (n_enemy) play(enemy_crit ? &CRITICAL : &ENEMY_HIT, clamp01(enemy + 0.08f * (float)(n_enemy - 1)), 0);
    if (n_party) play(party_crit ? &CRITICAL : &PARTY_HIT, clamp01(party + 0.08f * (float)(n_party - 1)), 0);
    if (fallen) play(&PARTY_DOWN, 1.0f, 0);
    if (boss) play(&BOSS_DOWN, 1.0f, 0);
}

void rumble_after_vint(void) {
    memcpy(queue_left, &m68k_ram[SOUND_QUEUE], sizeof queue_left);
}
