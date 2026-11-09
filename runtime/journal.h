/*
 * The journal (the menu's Journal): where the story has got to, what to do
 * next, and the Hunters' Guild's jobs. All of it from the game's own state,
 * read between frames, never changed (names from ps4.constants.asm):
 *
 *   Event_Flags  $FFF100   256 flags, set as the story goes on. Flag N is bit
 *                          7 - N % 8 of byte N / 8, as EventFlags_Test reads it.
 *   Town_Flags   $FFF160   the towns visited (Ryuka's list), the same way:
 *                          the guild shows once Aiedo (7) is one of them.
 *
 * The story is a list of steps in story order, each reached when its flags
 * are set; the current one is the last reached. The guild's jobs follow
 * HuntersGuildData (ps4.asm): offered from one flag until another, then
 * taken, completed and paid, each its own flag. The words are the port's own:
 * runtime/journal.txt, made into journal_data.h by tools/make_journal.py.
 */
#ifndef PSIV_JOURNAL_H
#define PSIV_JOURNAL_H

#include <stdint.h>

typedef struct {
    const char *arc;              /* the part of the game, e.g. "Motavia" */
    const char *title, *done;     /* done: what happened (the story so far) */
    const char *next, *hint;      /* what to do now, and help when asked for */
} JournalStep;

int journal_story(void);                       /* the current step; -1 before the story begins */
int journal_steps(void);
const JournalStep *journal_step(int i);
int journal_reached(int i);                    /* its flags are set */

enum { JOB_UNSEEN, JOB_OFFERED, JOB_TAKEN, JOB_REPORT, JOB_DONE };
typedef struct {
    const char *name, *about;     /* the job, as the guild puts it */
    const char *next, *hint;      /* taken: what to do now (the latest stage's) */
    const char *reward;           /* completed: how to collect the fee */
    int state;                    /* JOB_* */
} JournalJob;

int journal_guild_known(void);                 /* Aiedo has been visited */
int journal_jobs(void);                        /* the guild's jobs */
void journal_job(int j, JournalJob *out);
uint64_t journal_signature(void);              /* changes when anything the journal shows does */

/* Once a frame, for a note in the corner: 1 when the story has moved on a
 * step or a few (not a save loaded: that's a jump) and the field is the
 * player's again -- no event running, no window up (Game_Mode_Index $FFEF00
 * the field, $FFEC20 not an event, $FFEC98 no window's mappings saved) */
int journal_moved_on(void);

#endif
