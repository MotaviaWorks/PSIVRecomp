/*
 * The journal: see journal.h.
 */
#include "journal.h"
#include "m68k.h"

#include <stddef.h>

#define EVENT_FLAGS 0xF100u              /* Event_Flags: 32 bytes */
#define TOWN_FLAGS  0xF160u              /* Town_Flags */
#define TOWN_AIEDO  7                    /* the Hunters' Guild's town */

/* a condition: every term holds; a term is any of its flags (-1: none) */
#define JT 4
#define JA 3
typedef struct { int16_t any[JT][JA]; } Cond;
typedef struct { Cond when; JournalStep s; } Step;
typedef struct { Cond when; const char *next, *hint; } Stage;
typedef struct {
    uint8_t taken, completed, fee, from, until;    /* HuntersGuildData's flags */
    const char *name, *about, *reward;
    int stages;
    const Stage *stage;                            /* the first: just taken */
} Job;

#include "journal_data.h"                         /* STEPS, JOBS */

#define N_STEPS (int)(sizeof STEPS / sizeof STEPS[0])
#define N_JOBS  (int)(sizeof JOBS / sizeof JOBS[0])

static int bit(unsigned base, int n) {
    return n >= 0 && n < 256 && (m68k_ram[base + (unsigned)(n >> 3)] >> (7 - (n & 7))) & 1;
}
static int flag(int n) { return bit(EVENT_FLAGS, n); }

static int holds(const Cond *c) {
    for (int t = 0; t < JT && c->any[t][0] >= 0; t++) {
        int ok = 0;
        for (int a = 0; a < JA && c->any[t][a] >= 0; a++) ok |= flag(c->any[t][a]);
        if (!ok) return 0;
    }
    return 1;
}

static int story_begun(void) {          /* a game is on: any flag at all (a new game sets one at once) */
    for (unsigned i = 0; i < 32; i++)
        if (m68k_ram[EVENT_FLAGS + i]) return 1;
    return 0;
}

int journal_steps(void) { return N_STEPS; }
const JournalStep *journal_step(int i) { return i >= 0 && i < N_STEPS ? &STEPS[i].s : NULL; }
int journal_reached(int i) { return i >= 0 && i < N_STEPS && story_begun() && holds(&STEPS[i].when); }

int journal_story(void) {
    if (!story_begun()) return -1;
    for (int i = N_STEPS - 1; i >= 0; i--)
        if (holds(&STEPS[i].when)) return i;
    return -1;
}

int journal_jobs(void) { return N_JOBS; }

void journal_job(int j, JournalJob *out) {
    const Job *b = &JOBS[j];
    out->name = b->name;
    out->about = b->about;
    out->reward = b->reward;
    out->next = out->hint = NULL;
    if (!story_begun()) out->state = JOB_UNSEEN;
    else if (flag(b->fee)) out->state = JOB_DONE;
    else if (flag(b->completed)) out->state = JOB_REPORT;
    else if (flag(b->taken)) {
        out->state = JOB_TAKEN;
        for (int s = 0; s < b->stages; s++)        /* the latest stage reached */
            if (holds(&b->stage[s].when)) { out->next = b->stage[s].next; out->hint = b->stage[s].hint; }
    } else if (flag(b->from) && !flag(b->until)) out->state = JOB_OFFERED;
    else out->state = JOB_UNSEEN;
}

int journal_guild_known(void) {
    if (!story_begun()) return 0;
    if (bit(TOWN_FLAGS, TOWN_AIEDO)) return 1;
    for (int j = 0; j < N_JOBS; j++)
        if (flag(JOBS[j].taken)) return 1;
    return 0;
}

uint64_t journal_signature(void) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < 32; i++) h = (h ^ m68k_ram[EVENT_FLAGS + i]) * 1099511628211ull;
    for (unsigned i = 0; i < 4; i++) h = (h ^ m68k_ram[TOWN_FLAGS + i]) * 1099511628211ull;
    return h;
}

static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a] << 8 | m68k_ram[a + 1]); }

static int field_quiet(void) {          /* as walkto.c's field_free, vehicles or not */
    return r16(0xEF00) == 0x0C && r16(0xEC20) != 0x0C && r16(0xEC98) <= 0x6040;
}

int journal_moved_on(void) {
    static uint64_t sig;
    static int step = -2, pending;
    uint64_t s = journal_signature();
    if (s != sig) {
        sig = s;
        int now = journal_story();
        if (step >= 0 && now > step && now - step <= 3) pending = 1;
        else if (now < step) pending = 0;            /* gone back (a rewind) */
        step = now;
    }
    if (pending && field_quiet()) {
        pending = 0;
        return 1;
    }
    return 0;
}
