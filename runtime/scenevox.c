/*
 * Voiced scenes: see scenevox.h.
 */
#include "scenevox.h"
#include "hdtext.h"
#include "m68k.h"
#include "bus.h"
#include "sfx.h"
#include "sound.h"
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PAGES 4096
#define KEY_MAX   256
#define GAIN      0.8f

typedef struct {
    char key[KEY_MAX];
    int clip;                         /* the clip starting here, -1: carries on the line before */
    int memory;                       /* a flashback page (index: a third column "memory") */
    int line_clip;                    /* the clip its line plays (its own, or the page's before), -1 none */
    float share_end;                  /* how far into that clip this page's words end (0..1, by letters) */
} Page;

static Page *pages;
static int npages, enabled = 1;
static int16_t **clip_pcm;
static int *clip_frames, nclips;
static int current = -1;              /* the page on screen */
static int last_page = -1;            /* the last page known (kept while no box is up) */
static int first_page;                /* the first page of dialogue (after the narration's cues) */
static float mood, mood_to;           /* the flashback look: now, and where it is going (0..1) */
/* auto-advance: once a voiced page's words have been said (and a beat), C */
static int auto_on, press_frames, pressed_page = -1;
static int line_clip_now = -1;        /* the clip of the line on screen, and frames since it started */
static long line_frames;
void scenevox_set_auto(int on) { auto_on = on; }
int  scenevox_auto(void) { return auto_on; }
uint8_t scenevox_pad(uint8_t user, uint8_t pad) {
    if (user) { press_frames = 0; return pad; }                /* the player's own buttons come first */
    if (press_frames > 0) { press_frames--; return (uint8_t)(pad | PAD_C); }
    return pad;
}
static int boxless;                   /* frames with no dialogue box */
float scenevox_memory(void) { return mood; }

/* what is sounding */
static const int16_t *play_p;
static int play_n, play_pos;
static float fade = 1.0f, fade_step;
static const int16_t *next_p;         /* waits for the one fading out */
static int next_n;

void scenevox_set_enabled(int on) { enabled = on; if (!on) scenevox_stop(); }

int scenevox_load(const char *index, size_t len, void *(*read)(const char *name, size_t *len)) {
    if (!pages) pages = calloc(MAX_PAGES, sizeof *pages);
    if (!clip_pcm) { clip_pcm = calloc(MAX_PAGES, sizeof *clip_pcm); clip_frames = calloc(MAX_PAGES, sizeof *clip_frames); }
    if (!pages || !clip_pcm || !clip_frames) return 0;
    const char *p = index, *end = index + len;
    while (p < end && npages < MAX_PAGES) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        const char *tab = memchr(p, '\t', (size_t)(eol - p));
        if (tab && p[0] != '#') {
            char name[256];
            size_t nl = (size_t)(tab - p);
            if (nl >= sizeof name) nl = sizeof name - 1;
            memcpy(name, p, nl);
            name[nl] = 0;
            Page *pg = &pages[npages];
            const char *tab2 = memchr(tab + 1, '\t', (size_t)(eol - tab - 1));
            pg->memory = tab2 && eol - tab2 > 6 && !strncmp(tab2 + 1, "memory", 6);
            size_t kl = (size_t)((tab2 ? tab2 : eol) - tab - 1);
            while (kl && (tab[kl] == '\r' || tab[kl] == ' ')) kl--;
            if (kl >= KEY_MAX) kl = KEY_MAX - 1;
            memcpy(pg->key, tab + 1, kl);
            pg->key[kl] = 0;
            pg->clip = -1;
            if (strcmp(name, "-")) {
                size_t n = 0;
                void *d = read(name, &n);
                int frames = 0;
                int16_t *pcm = d ? sfx_decode_wav(d, (long)n, &frames) : NULL;
                free(d);
                if (pcm) { clip_pcm[nclips] = pcm; clip_frames[nclips] = frames; pg->clip = nclips++; }
                else pg->clip = -2;                                  /* named, but missing: silent */
            }
            if (pg->key[0]) npages++;
        }
        p = eol + 1;
    }
    /* each page's share of its line, by letters: a line over two pages moves on
     * from the first about where its words end (auto-advance) */
    for (int i = 0; i < npages; ) {
        if (pages[i].key[0] == '@' || pages[i].clip < 0) { pages[i].line_clip = -1; i++; continue; }
        int j = i + 1, total = (int)strlen(pages[i].key);
        while (j < npages && pages[j].clip == -1 && pages[j].key[0] != '@') total += (int)strlen(pages[j].key), j++;
        int sum = 0;
        for (int k = i; k < j; k++) {
            sum += (int)strlen(pages[k].key);
            pages[k].line_clip = pages[i].clip;
            pages[k].share_end = total ? (float)sum / (float)total : 1.0f;
        }
        i = j;
    }
    first_page = 0;
    while (first_page < npages && pages[first_page].key[0] == '@') first_page++;
    /* PSIV_VOX_START=N (footage from a save mid-story): the story's place starts at
     * page N, as if the scenes before it had played */
    const char *st = getenv("PSIV_VOX_START");
    if (st && *st && atoi(st) >= 0 && atoi(st) < npages) last_page = atoi(st);
    return npages;
}

/* the page this text is, or -1 (not yet sure, or not voiced). A page whose
 * words are all typed is that page -- unless a longer page starts the same
 * way and the typing may still be going: then only once it has stopped
 * (`settled`). Otherwise, the one page that starts so, from 8 letters.
 *
 * Where the story is counts: the pages just ahead of the last one known (a
 * scene going on, or the next one in the index's story order) are "near".
 * Most of the game's lines aren't voiced, and many start like a voiced one
 * from another scene ("I've got to find her" -- "I've got to report...") or
 * are the very same short words ("What?", "Rune!", "Huh?"): away from the
 * story's place, a page that starts so is taken from 20 letters, and all of
 * a page only when it is 12 letters or more. (Every page of the script typed
 * through this: 250 lines set off a wrong voice by 8 letters anywhere, 1 so.) */
#define NEAR 12
static int which_page(const char *typed, int n, int settled) {
    if (n < 2) return -1;
    int found = -1, count = 0, exact = -1, exact_d = 1 << 30, longer_d = 1 << 30;
    for (int i = 0; i < npages; i++) {
        if (pages[i].key[0] == '@' || strncmp(pages[i].key, typed, (size_t)n)) continue;
        /* how far ahead in the story: from the last page known, or with none
         * yet (a game just loaded), from the first scene */
        int d = last_page >= 0 ? (i - last_page + npages) % npages : i - first_page;   /* can be in two scenes ("What?"): */
        if (!pages[i].key[n]) {                                     /* all of this page: the same words */
            if (d < exact_d) { exact_d = d; exact = i; }            /* the one next after the last page */
            continue;
        }
        if (d < longer_d) longer_d = d;
        found = i;
        count++;
    }
    /* "Ah!" may be all of a page in a later scene but only the start of the
     * page that comes next here: while the typing pauses after it, wait */
    if (exact >= 0 && longer_d < exact_d) return -1;
    if (exact >= 0) {
        if (count && !settled) return -1;
        return exact_d <= NEAR || n >= 12 ? exact : -1;              /* short words: only near */
    }
    if (count == 1 && n >= (longer_d <= NEAR ? 8 : 20)) return found;   /* only one page starts so */
    return -1;
}

/* Narration with no dialogue box -- the title screen's story, the new
 * game's prologue -- follows the game's own counter instead: TextCounter
 * ($FFED90) ticks through the title's lines and counts the prologue's. An
 * index row whose words are "@M:N" plays when Game_Mode_Index / 4 is M and
 * the counter becomes N. On the title screen (mode 1) the counter serves
 * two parts in turn: the story's fading lines ("@T:tick") and the
 * prologue's scrolling lines ("@P:lines shown"), whose step timer
 * ($FFED92) has just been set ($60) as each line goes up. */
static unsigned cue_mode = 0xFFFF, cue_count = 0xFFFF;
static void play_clip(int c, int wait);
static void cue_play(const char *key);
static void cues(void) {
    unsigned mode = (unsigned)(m68k_ram[0xEF00] << 8 | m68k_ram[0xEF01]) / 4;
    unsigned count = (unsigned)(m68k_ram[0xED90] << 8 | m68k_ram[0xED91]);
    unsigned timer = (unsigned)(m68k_ram[0xED92] << 8 | m68k_ram[0xED93]);
    if (mode == cue_mode && count == cue_count) return;
    static int log_on = -1;
    if (log_on < 0) { const char *e = getenv("PSIV_CUE_LOG"); log_on = e && *e == '1'; }
    if (log_on) printf("cue: mode %u counter %u timer %u\n", mode, count, timer);
    cue_mode = mode;
    cue_count = count;
    char key[32];
    if (mode == 1) snprintf(key, sizeof key, "@%c:%u", timer >= 90 ? 'P' : 'T', count);
    else snprintf(key, sizeof key, "@%u:%u", mode, count);
    cue_play(key);
}

/* a cued clip, if the index has one */
static void cue_play(const char *key) {
    static int log_on = -1;
    if (log_on < 0) { const char *e = getenv("PSIV_CUE_LOG"); log_on = e && *e == '1'; }
    for (int i = 0; i < npages; i++)
        if (pages[i].key[0] == '@' && !strcmp(pages[i].key, key) && pages[i].clip >= 0) {
            if (log_on) printf("cue: play %s L%llu\n", key, (unsigned long long)frame_logical);
            play_clip(pages[i].clip, 1);                            /* narration: after the line before */
            last_page = i;                                          /* the story's place: what comes next is near */
            break;
        }
}

/* The new game's panorama ("AW 2284. Monster attacks...", dialogue 17
 * $3A-$41): RunText2 writes each screen's four lines into Plane_A_Buffer
 * at fixed rows, starting with tiles $4200, $4280, $4300, $4380 (palette 2)
 * -- the first time screen 1, the second screen 2 ("@A:1", "@A:2"). */
static void panorama_cues(void) {
    static int was, screen;
    unsigned mode = (unsigned)(m68k_ram[0xEF00] << 8 | m68k_ram[0xEF01]) / 4;
    unsigned a = (unsigned)(m68k_ram[0x840A] << 8 | m68k_ram[0x840B]), d = (unsigned)(m68k_ram[0x888A] << 8 | m68k_ram[0x888B]);
    unsigned b = (unsigned)(m68k_ram[0x858A] << 8 | m68k_ram[0x858B]), c = (unsigned)(m68k_ram[0x870A] << 8 | m68k_ram[0x870B]);
    int now = mode == 3 && a == 0x4200 && b == 0x4280 && c == 0x4300 && d == 0x4380;
    if (mode != 3) screen = 0;
    if (now && !was && screen < 2) {
        char key[16];
        snprintf(key, sizeof key, "@A:%d", ++screen);
        cue_play(key);
    }
    was = now;
}

void scenevox_frame(const VdpLine *lines, int w, int h) {
    if (!enabled || !npages) return;
    cues();
    panorama_cues();
    char typed[KEY_MAX];
    int n = hdtext_dialogue_key(lines, w, h, typed, sizeof typed);
    /* a flashback's look follows its pages; through a flashback's silent
     * steps (no box) it holds, for a few seconds */
    if (n == 0 && ++boxless > 240) mood_to = 0.0f;
    if (n) boxless = 0;
    mood += (mood_to - mood) * 0.06f;
    if (mood < 0.002f && mood_to == 0.0f) mood = 0.0f;
    if (n == 0) { current = -1; return; }                           /* no box: the next one is new */
    static int last_n, still;                                       /* how long the typing has stood still */
    still = n == last_n ? still + 1 : 0;
    last_n = n;
    int pg = which_page(typed, n, still >= 10);
    line_frames++;
    if (still == 10) {                                              /* PSIV_VOX_LOG: each page as its typing settles */
        static int settle_log = -1;
        if (settle_log < 0) { const char *e = getenv("PSIV_VOX_LOG"); settle_log = e && *e == '1'; }
        if (settle_log) printf("scene: text \"%.48s\" L%llu\n", typed, (unsigned long long)frame_logical);
    }
    if (auto_on && current >= 0 && pages[current].line_clip >= 0 && pages[current].line_clip == line_clip_now
        && still >= 10 && pressed_page != current && press_frames == 0) {
        long need = (long)(pages[current].share_end * (float)clip_frames[line_clip_now] * 60.0f / (float)SOUND_RATE) + 24;
        if (line_frames >= need) { press_frames = 3; pressed_page = current; }
    }
    /* PSIV_AUTO_ALL=N (for footage): with auto-advance on, a page with no voice
     * of its own -- unvoiced, or not in the pack -- moves on N frames after its
     * typing stops (and every N frames after that, if the game didn't take it) */
    static int all_n = -1;
    if (all_n < 0) { const char *e = getenv("PSIV_AUTO_ALL"); all_n = e && *e ? atoi(e) : 0; }
    if (auto_on && all_n > 0 && still >= all_n && still % all_n == 0 && press_frames == 0 && !scenevox_sounding()
        && (current < 0 || pressed_page == current || pages[current].line_clip < 0)) {
        press_frames = 3;
        if (getenv("PSIV_VOX_LOG")) printf("scene: unvoiced page moved on L%llu\n", (unsigned long long)frame_logical);
    }
    if (pg < 0 || pg == current) return;
    current = pg;
    last_page = pg;
    mood_to = pages[pg].memory ? 1.0f : 0.0f;
    static int log_on = -1;
    if (log_on < 0) { const char *e = getenv("PSIV_VOX_LOG"); log_on = e && *e == '1'; }
    if (log_on) printf("scene: page %d clip %d (%s) L%llu\n", pg, pages[pg].clip, pages[pg].key, (unsigned long long)frame_logical);
    if (pages[pg].clip < 0) return;                                 /* carries on the line before */
    play_clip(pages[pg].clip, 0);
    line_clip_now = pages[pg].clip;
    line_frames = 0;
}

/* a clip: after the one sounding (wait), or fading that one out first */
static void play_clip(int c, int wait) {
    if (play_p) {
        if (!wait) fade_step = -1.0f / (0.06f * SOUND_RATE);
        next_p = clip_pcm[c];
        next_n = clip_frames[c];
    } else {
        play_p = clip_pcm[c]; play_n = clip_frames[c]; play_pos = 0; fade = 1.0f; fade_step = 0.0f;
    }
}

int scenevox_sounding(void) { return play_p != NULL || next_p != NULL; }

void scenevox_stop(void) { play_p = next_p = NULL; current = last_page = -1; mood = mood_to = 0.0f; }

int scenevox_pages(void) { return npages; }

void scenevox_unload(void) {
    scenevox_stop();
    for (int i = 0; i < nclips; i++) { free(clip_pcm[i]); clip_pcm[i] = NULL; }
    nclips = npages = first_page = 0;
    line_clip_now = pressed_page = -1;
    press_frames = 0;
    line_frames = 0;
    boxless = 0;
}

void scenevox_mix(float *samples, int frames) {
    for (int j = 0; j < frames; j++) {
        if (!play_p && next_p) {
            play_p = next_p; play_n = next_n; play_pos = 0; fade = 1.0f; fade_step = 0.0f;
            next_p = NULL;
        }
        if (!play_p) break;
        if (play_pos >= play_n || fade <= 0.0f) { play_p = NULL; continue; }
        float g = GAIN * fade;
        samples[j * 2] += (float)play_p[play_pos * 2] * g;
        samples[j * 2 + 1] += (float)play_p[play_pos * 2 + 1] * g;
        play_pos++;
        fade += fade_step;
    }
}
