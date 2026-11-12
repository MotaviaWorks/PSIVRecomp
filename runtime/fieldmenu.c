/*
 * The field menu, redrawn: see fieldmenu.h.
 */
#include "fieldmenu.h"
#include "battledesc.h"
#include "bus.h"
#include "hdtext.h"
#include "uikit.h"
#include "vdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAM ($FFxxxx); see the disassembly's ps4.constants.asm */
#define GAME_MODE_INDEX 0xEF00u   /* $C = the field */
#define WIN_STACK       0x6000u   /* a word per open window: where its record is (Chunk_Table, lent to the menu) */
#define PLANE_A_BUFFER  0x8000u   /* 64 x 32 name-table words: the game draws its windows here */
#define WINDOW_INDEX    0xEC90u   /* the window whose routine runs */
#define WIN_GROUP       0xEC94u   /* long: the group's table in the ROM */
#define WINDOWS_OPENED  0xEC9Au
#define OPTION_1        0xEC9Eu   /* the main menu's cursor */
#define OPTION_2        0xEC9Fu   /* equipment: the member; items: the row in the page */
#define OPTION_3        0xECA0u   /* equipment: the row in the list's page; items: use / look / discard */
#define OPTION_4        0xECA1u   /* equipment: the slot, or the hand; items: who it is used on */
/* macros: 2 = the letter, 3 = who acts next (among those left), 4 = what they do */
/* techniques, skills: 2 = whose, 3 = the row in the list (techniques: in its page), 4 = on whom */
/* status: 2 = status or order, 3 = whose */
#define CAMERA_Y        0xEF90u
#define CAMERA_X        0xEF94u
#define RED_CURSOR      0xC280u   /* the cursor's object: +0 id, +$2C / +$2E where its sprite is */
#define PARTY_SLOTS     0xF40Au   /* 5 character IDs, $FF = no one */
#define INVENTORY       0xF410u   /* 40 item IDs, no gaps */
#define MONEY           0xF438u
#define CHAR_STATS      0xF500u   /* $80 each, by character ID */
#define ITEM_PAGE       0xE206u   /* the items window: 8 to a page */
#define ART_COUNT       0xE200u   /* techniques, skills: how many the member can use in camp ... */
#define ART_LIST        0xE210u   /* ... their IDs ... */
#define ART_PAGE        0xE206u   /* ... and the page of eight showing (techniques) */
#define EQUIP_LIST      0xE380u   /* what the member could put on: item IDs */
#define EQUIP_COUNT     0xE3A8u
#define EQUIP_PAGE      0xE3E4u   /* 8 to a page */
#define EQUIP_FLAGS     0xE3FDu   /* bit 0: the cursor is on the slots */
#define MACRO_DATA      0xF444u   /* 8 macros of 5 turns: who (character ID), what (1 attack, 2 technique,
                                     3 skill, 4 item, 5 defend; 0 = no turn), which (its ID), a spare byte */
#define MACRO_WORK      0xE310u   /* the macro being set up, the same way */
#define MACRO_PARTY     0xE302u   /* how many turns it will have (the party), less one */
#define MACRO_CHOSEN    0xE306u   /* how many are set */
#define MACRO_LEFT      0xE330u   /* 5 bytes a turn: the characters still to be given one */
#define MACRO_FLAGS     0xE326u   /* a byte a macro: bit n = its turn n cannot be played as things stand */
/* a character's record */
#define C_JOB    0x06             /* word: the profession */
#define C_LEVEL  0x08
#define C_EXP    0x0A             /* long */
#define C_HP     0x0E
#define C_TP     0x12
#define C_STATUS 0x16
#define C_EQUIP  0x4C             /* right hand, left hand, head, body */
#define C_TECHS  0x52             /* 16 technique IDs */
#define C_SKILLS 0x62             /* 8 skill IDs */
#define C_USES   0x6A             /* 8 x (uses left, uses when rested) */
/* ROM */
#define WINGROUP_MENU   0x0689C0u /* 8 bytes a window: w, h, x, y, its routine */
#define WINGROUP_SHOP   0x069448u /* the same for a shop or an inn ... */
#define WINGROUP_CHEST  0x069650u /* ... and for something found */
#define SHOP_KIND       0xECD0u   /* 0 an inn; 1, 2 a shop */
#define SHOP_ID         0xECD1u   /* which: its goods are list n of SHOP_GOODS */
#define SHOP_GOODS      0x00681A4u /* ShopInventories: item IDs, each shop's list ended by $FF */
#define SELL_PAGE       0xE3E2u   /* selling: the page of the pack, 8 to a page */
#define I_PRICE 0x14              /* word: what it costs; it sells for half */
#define ITEM_DATA       0x2A8E28u /* $16 each: +8 who can wear it, +$A kind, +$B.. bonuses */
#define I_WHO   0x08
#define I_KIND  0x0A              /* 1, 2 one hand; 3, 4 both hands; 5 shield; 6 head; 7 body; above: not worn */
#define TECH_DATA       0x2A9BE8u /* 8 each: +1 TP, +2 bit 5 = can be used in camp */
#define JOB_NAMES       0x2A8DD8u /* the professions' names, each ended by $FF */
#define AGES            0x2A8E12u /* a word a character; 0 = not told */
#define START_LEVELS    0x05D72Eu /* a byte a character: the level they join at */
#define LEVEL_TABLES    0x05D73Au /* a long a character: $16 bytes a level from there, first the experience for the next */
#define SKILL_DATA      0x2A9D28u /* the same for skills */
/* the windows' routines */
#define R_MENU      0x057C12u
#define R_MESETA    0x057F8Cu
#define R_OVERVIEW  0x057FFEu
#define R_ITEMS     0x05B39Au     /* the items, a page of eight */
#define R_ITEM_DO   0x05B632u     /* use / look / discard */
#define R_ITEM_WHO  0x05B8ACu     /* on whom */
#define R_ITEM_NONE 0x05BA1Eu     /* messages of the items windows ... */
#define R_ITEM_USED 0x05BA98u
#define R_ITEM_LOOK 0x05C334u
#define R_ITEM_DROP 0x05C4CAu
#define R_ITEM_KEEP 0x05C702u
#define R_TELEPIPE  0x05C7E6u     /* ... and the Telepipe's list of places */
#define R_TELEPORT  0x05CAD2u
#define R_MEMBERS   0x05E7CAu     /* equipment: whose */
#define R_STATS     0x05E946u
#define R_SLOTS     0x05EAF2u
#define R_LIST      0x05F17Eu
#define R_NOTHING   0x05F460u     /* "No equippable item." */
#define R_FULL      0x05F53Au     /* nothing can be taken off: the pack is full */
#define R_GENERIC   0x0697E4u
#define R_TECH_WHO      0x05FDEEu /* techniques: whose, the list, on whom */
#define R_TECH_LIST     0x06002Au
#define R_TECH_TARGET   0x060552u
#define R_TECH_NOTE     0x0606CEu
#define R_RYUKA         0x06077Eu /* Ryuka's list of places */
#define R_TECH_TELEPORT 0x060A92u
#define R_TECH_USED     0x060B32u /* "Gires is used!  HP fully recovered!" */
#define R_SKILL_WHO     0x061322u /* skills: the same */
#define R_SKILL_LIST    0x061532u
#define R_SKILL_TARGET  0x0618A4u
#define R_SKILL_NOTE    0x061A1Au
#define R_SKILL_USED    0x061AD4u
#define R_STATE_OPTIONS 0x05CBB6u /* status: "status" or "order" */
#define R_STATUS_WHO    0x05CD04u /* whose; its pages: the profile (four plain windows and this one) ... */
#define R_STATUS_EXP    0x05D5A0u
#define R_STATUS_TECH_B 0x05D766u /* ... the techniques, for battle and for camp ... */
#define R_STATUS_TECH_C 0x05D8BCu
#define R_STATUS_SKILL_B 0x05D9DEu /* ... and the skills */
#define R_STATUS_SKILL_C 0x05DC20u
#define R_ORDER         0x05DFB4u /* the order they stand in */
#define R_ORDER_ALONE   0x05E188u
#define R_SHOP_MESETA   0x065D8Au /* shops: the meseta, the keeper's words, buy or sell, the goods, the pack */
#define R_SHOP_MSG      0x065E6Eu
#define R_SHOP_BUYSELL  0x064F62u
#define R_SHOP_BUY      0x065084u
#define R_SHOP_SELL     0x0653B8u
#define R_SHOP_BUY_Q    0x0652A0u /* their yes / no, and the inn's */
#define R_SHOP_SELL_Q   0x0656B8u
#define R_REST_Q        0x06603Eu
#define R_CHEST_FOUND   0x066FB2u /* something found: all drawn as they stand */
#define R_CHEST_DECIDE  0x067054u
#define R_CHEST_CONFIRM 0x067198u
#define R_CHEST_LIST    0x066E66u
#define R_CHEST_WHO     0x067276u
#define R_CHEST_MSG     0x0673DCu
#define R_MACRO_LABEL   0x062114u /* macros: the "MACRO" label */
#define R_MACRO_LETTERS 0x062192u /* A to H */
#define R_MACRO_WHO     0x06273Au /* who acts next */
#define R_MACRO_DO      0x0628E0u /* the five commands, as icons */
#define R_MACRO_ERASE   0x0625F8u /* "A-MACRO will be erased. Are you sure?" and its yes / no */
#define R_MACRO_ERASE_Q 0x062654u
#define R_MACRO_TECHS   0x062DC2u /* which technique, skill, item */
#define R_MACRO_SKILLS  0x0630D0u
#define R_MACRO_ITEMS   0x06333Eu
#define R_MACRO_NOTE    0x063668u
#define R_MACRO_SET     0x063758u /* "A-MACRO will set up. Are you sure?" and its yes / no */
#define R_MACRO_SET_Q   0x0637B4u
#define R_MACRO_ALONE   0x0638FCu

/* the window art */
#define T_BOX    0x6E7            /* a choice's checkbox */
#define T_CURSOR 0x6E8            /* the red one: the cursor's sprite */

enum { SCR_MAIN, SCR_EQUIP, SCR_ITEMS, SCR_MACROS, SCR_TECHS, SCR_SKILLS, SCR_STATUS, SCR_SHOP, SCR_CHEST, SCR_PLAIN };

/* a window the game has open, as it stands in the plane */
typedef struct {
    int x, y, w, h;               /* cells, frame included */
    int index;                    /* which of the group's windows */
    uint32_t routine;
    int plain;                    /* drawn as it stands: its own text, where the game put it */
    uint32_t ch[28][40];          /* interior, as Unicode; 0 = blank */
    uint8_t pal[28][40];          /* the palette line each letter is in */
    uint8_t tall[28][40];         /* a letter of a line of dialogue: two cells high */
    uint16_t raw[30][42];         /* every cell (frame included) as the plane has it ... */
    uint8_t pic[30][42];          /* ... and whether it is a piece of a picture (a portrait) */
    int framed;                   /* it has a frame of its own (a portrait fills its whole window) */
    int usual;                    /* the one most of them are in: any other is greyed */
    int boxes;                    /* checkboxes: it has choices */
    int picture;                  /* it holds tiles that are no letters: a line of dialogue, a portrait, icons */
} Win;
#define MAX_WINS 16

static int enabled, active, screen, sideways;
static uint32_t group = WINGROUP_MENU;   /* the table the open windows are from */
static Win wins[MAX_WINS];
static int nwins;
static char healed[5][12];        /* by party place: what the game has just restored ("88"), "" = nothing shown */
static uint16_t override[64 * 32];
static uint8_t hidden[80];
/* The field behind the menu: one picture, taken (dimmed) as the menu opens
 * and held until it closes. Between two of its windows the game leaves
 * pieces of one in the plane for a few frames that are in no record of its
 * stack (closed without putting back what was under it, the next one about
 * to cover it): drawn live, they flashed in the gaps between this menu's
 * windows at every choice. (The field does not move while the menu is up.) */
static uint32_t *still;
static int still_w, still_h, still_ok;

/* PSIV_FIELDMENU_LOG=1: a line whenever the menu is up and this module steps aside, and why */
static void aside(const char *why, unsigned a, unsigned b) {
    static int on = -1;
    if (on < 0) on = getenv("PSIV_FIELDMENU_LOG") != NULL;
    if (on) fprintf(stderr, "fieldmenu: aside: %s (%X, %X); %u windows open\n", why, a, b, ui_ram8(WINDOWS_OPENED));
}

void fieldmenu_set_enabled(int on) { enabled = on; }
int  fieldmenu_enabled(void) { return enabled; }
int  fieldmenu_active(void) { return active; }

static unsigned r8(unsigned a) { return ui_ram8(a); }
static unsigned r16(unsigned a) { return ui_ram16(a); }

static uint32_t routine_of(unsigned index) { return ui_rom32(group + index * 8u + 4u); }

enum { K_MAIN, K_EQUIP, K_ITEMS, K_MACROS, K_TECHS, K_SKILLS, K_STATUS, K_SHOP, K_NOTE, K_OTHER };
static int kind_of(uint32_t r) {
    switch (r) {
    case R_MENU: case R_MESETA: case R_OVERVIEW: return K_MAIN;
    case R_MEMBERS: case R_STATS: case R_SLOTS: case R_LIST: case R_NOTHING: case R_FULL: return K_EQUIP;
    case R_ITEMS: case R_ITEM_DO: case R_ITEM_WHO: return K_ITEMS;
    case R_MACRO_LABEL: case R_MACRO_LETTERS: case R_MACRO_WHO: case R_MACRO_DO: return K_MACROS;
    case R_TECH_WHO: case R_TECH_LIST: case R_TECH_TARGET: return K_TECHS;
    case R_SKILL_WHO: case R_SKILL_LIST: case R_SKILL_TARGET: return K_SKILLS;
    case R_STATE_OPTIONS: case R_STATUS_WHO: case R_STATUS_EXP: case R_STATUS_TECH_B: case R_STATUS_TECH_C:
    case R_STATUS_SKILL_B: case R_STATUS_SKILL_C: return K_STATUS;
    /* messages, questions and lists that come up over those screens */
    case R_ITEM_NONE: case R_ITEM_USED: case R_ITEM_LOOK: case R_ITEM_DROP: case R_ITEM_KEEP:
    case R_TELEPIPE: case R_TELEPORT: case R_GENERIC:
    case R_MACRO_ERASE: case R_MACRO_ERASE_Q: case R_MACRO_TECHS: case R_MACRO_SKILLS: case R_MACRO_ITEMS:
    case R_MACRO_NOTE: case R_MACRO_SET: case R_MACRO_SET_Q: case R_MACRO_ALONE:
    case R_TECH_NOTE: case R_RYUKA: case R_TECH_TELEPORT: case R_TECH_USED: case R_SKILL_NOTE: case R_SKILL_USED:
    case R_ORDER: case R_ORDER_ALONE: return K_NOTE;
    case R_SHOP_MESETA: case R_SHOP_MSG: case R_SHOP_BUYSELL: case R_SHOP_BUY: case R_SHOP_SELL: return K_SHOP;
    case R_SHOP_BUY_Q: case R_SHOP_SELL_Q: case R_REST_Q:
    case R_CHEST_FOUND: case R_CHEST_DECIDE: case R_CHEST_CONFIRM: case R_CHEST_LIST: case R_CHEST_WHO: case R_CHEST_MSG:
        return K_NOTE;
    default: return K_OTHER;
    }
}
static int open_now(uint32_t r) {
    for (int i = 0; i < nwins; i++)
        if (wins[i].routine == r) return 1;
    return 0;
}

/* The mouse. Each row that can be chosen leaves a hit as it is drawn: while
 * `routine` has the cursor, the game's cursor byte `var` is walked to
 * `target` -- a press of `dec` or `inc` at a time, each let go before the
 * next (the menus act on new presses), after its page word `page_var` (0:
 * it has none) has been turned to `page` with Left / Right -- and then C is
 * pressed: the game's own choosing, played as the pad. A row of a window
 * drawn as it stands has no cursor byte known here: its hit is the cell row
 * the game's cursor is to be walked to. */
typedef struct {
    float x0, y0, x1, y1;         /* the picture's game pixels */
    uint32_t routine;
    unsigned var, page_var;
    int target, page, cell_row;   /* cell_row < 0: by `var` */
    uint8_t dec, inc;
} Hit;
static Hit hits[200];
static int nhits;
static struct { int on, hold, rest, presses, waited; uint8_t press; Hit h; } drive;
static int pulse;                 /* a click on nothing: a press of C, or of B (the right button), for a frame */
static uint8_t pulse_key;
static int wheel, wheel_rest;     /* turns of the wheel still to play: + down, - up */
#define WAIT 0xFF

static void hit(float x0, float x1, float base, uint32_t routine, unsigned var, int target, uint8_t dec, uint8_t inc,
                unsigned page_var, int page) {
    if (nhits >= (int)(sizeof hits / sizeof hits[0])) return;
    hits[nhits++] = (Hit){x0, base - 8.6f, x1, base + 3.6f, routine, var, page_var, target, page, -1, dec, inc};
}
static void hit_cell(float x0, float x1, float base, int cell_row) {
    if (nhits >= (int)(sizeof hits / sizeof hits[0])) return;
    hits[nhits++] = (Hit){x0, base - 8.6f, x1, base + 3.6f, 0, 0, 0, 0, 0, cell_row, PAD_UP, PAD_DOWN};
}
/* (the party's rows and the members across the top serve several screens: whose cursor they are) */
static uint32_t party_routine, strip_routine;
static unsigned strip_var;

static int cursor_cell(int *col, int *row);

/* the next press of a pick: 0 when the cursor is there, WAIT while the game is between windows */
static uint8_t drive_step(void) {
    const Hit *h = &drive.h;
    if (h->cell_row >= 0) {
        int col, row;
        if (!cursor_cell(&col, &row)) return WAIT;
        return row == h->cell_row ? 0 : row < h->cell_row ? PAD_DOWN : PAD_UP;
    }
    if (routine_of(r16(WINDOW_INDEX)) != h->routine) return WAIT;
    if (h->page_var && (int)r16(h->page_var) != h->page) return (int)r16(h->page_var) < h->page ? PAD_RIGHT : PAD_LEFT;
    int at = (int)r8(h->var);
    return at == h->target ? 0 : at < h->target ? h->inc : h->dec;
}

int fieldmenu_click(float fx, float fy, int right) {
    if (!active) return 0;
    drive.on = 0;
    wheel = 0;
    if (right) { pulse = 1; pulse_key = PAD_B; return 1; }
    for (int i = nhits - 1; i >= 0; i--) {               /* the last drawn is on top */
        const Hit *h = &hits[i];
        if (fx < h->x0 || fx >= h->x1 || fy < h->y0 || fy >= h->y1) continue;
        if (h->cell_row < 0 && routine_of(r16(WINDOW_INDEX)) != h->routine) return 1;   /* not where the cursor is: nothing */
        drive.h = *h;
        drive.on = 1;
        drive.hold = drive.rest = drive.presses = drive.waited = 0;
        drive.press = 0;
        return 1;
    }
    pulse = 1;                                           /* on nothing: C, as a button would be */
    pulse_key = PAD_C;
    return 1;
}

void fieldmenu_wheel(int down) {
    if (!active || drive.on) return;
    wheel += down ? 1 : -1;
    if (wheel > 6) wheel = 6;
    if (wheel < -6) wheel = -6;
}

uint8_t fieldmenu_pad(uint8_t pad) {
    if (!active) { drive.on = 0; pulse = 0; wheel = 0; return pad; }
    uint8_t out = pad;
    if (sideways) {
        out = pad & (uint8_t)~(PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
        if (pad & PAD_LEFT) out |= PAD_UP;
        if (pad & PAD_RIGHT) out |= PAD_DOWN;
    }
    if (pad) { drive.on = 0; pulse = 0; wheel = 0; return out; }   /* the player's own buttons come first */
    if (pulse) { pulse = 0; return pulse_key; }
    if (drive.on) {
        if (drive.hold > 0) { drive.hold--; return drive.press; }
        if (drive.rest > 0) { drive.rest--; return 0; }
        if (drive.press == PAD_C) { drive.on = 0; return 0; }       /* chosen */
        uint8_t k = drive_step();
        if (k == WAIT) {
            if (++drive.waited > 40) drive.on = 0;
            return 0;
        }
        drive.waited = 0;
        if (!k) k = PAD_C;
        if (++drive.presses > 64) { drive.on = 0; return 0; }
        drive.press = k;
        drive.rest = drive.h.cell_row < 0 && drive.h.page_var && (k & (PAD_LEFT | PAD_RIGHT)) ? 6 : 2;   /* a page: a window to open */
        return k;
    }
    if (wheel_rest > 0) { wheel_rest--; return 0; }
    if (wheel) {                                         /* a turn of the wheel: the cursor a row on (the macro's commands: across) */
        int across = screen == SCR_MACROS && routine_of(r16(WINDOW_INDEX)) == R_MACRO_DO;
        uint8_t k = wheel > 0 ? (across ? PAD_RIGHT : PAD_DOWN) : (across ? PAD_LEFT : PAD_UP);
        wheel += wheel > 0 ? -1 : 1;
        wheel_rest = 2;
        return k;
    }
    return out;
}

void fieldmenu_frame(void) {
    active = 0;
    sideways = 0;
    int n = (int)r8(WINDOWS_OPENED);
    uint32_t g = ui_ram32(WIN_GROUP);
    if (!enabled || !ui_ready() || r16(GAME_MODE_INDEX) != 0x0C || n < 1
        || (g != WINGROUP_MENU && g != WINGROUP_SHOP && g != WINGROUP_CHEST)) {
        still_ok = 0;                                    /* the menu is not up: the next one takes its own picture */
        return;
    }
    group = g;
    if (n > MAX_WINS) { aside("too many windows", (unsigned)n, 0); return; }
    if (!(vdp.reg[1] & 0x40) || (vdp.reg[16] & 3) != 1) { aside("display", vdp.reg[1], vdp.reg[16]); return; }   /* display on, 64-cell planes */

    /* the windows the game has open, and what stands in each */
    int col0 = (int)(r16(CAMERA_X) >> 3), row0 = (int)(r16(CAMERA_Y) >> 3);
    int kinds = 0;
    for (int i = 0; i < n; i++) {
        unsigned p = r16(WIN_STACK + (unsigned)i * 2u);
        if (p < 0x6040 || p >= 0x8000) { aside("record", (unsigned)i, p); return; }
        Win *w = &wins[i];
        uint32_t rec = group + r16(p) * 8u;
        w->index = (int)r16(p);
        w->w = (int)ui_rom8(rec) + 1;
        w->h = (int)ui_rom8(rec + 1) + 1;
        w->x = (int)ui_rom8(rec + 2);
        w->y = (int)ui_rom8(rec + 3);
        w->routine = ui_rom32(rec + 4);
        if (w->w < 3 || w->h < 3 || w->w > 42 || w->h > 30) { aside("size", (unsigned)w->w, (unsigned)w->h); return; }
        kinds |= 1 << kind_of(w->routine);
        memset(w->ch, 0, sizeof w->ch);
        memset(w->pal, 0, sizeof w->pal);
        memset(w->tall, 0, sizeof w->tall);
        memset(w->pic, 0, sizeof w->pic);
        w->boxes = w->picture = 0;
        int in_pal[4] = {0};
        for (int cy = 0; cy < w->h; cy++)
            for (int cx = 0; cx < w->w; cx++) {
                unsigned e = r16(PLANE_A_BUFFER + (unsigned)(((row0 + w->y + cy) & 31) * 64 + ((col0 + w->x + cx) & 63)) * 2u);
                int t = (int)(e & 0x7FF), inside = cy > 0 && cx > 0 && cy < w->h - 1 && cx < w->w - 1;
                uint32_t cp = 0;
                int is_pic = 0, is_tall = 0;
                w->raw[cy][cx] = (uint16_t)e;
                if (t >= 0x580 && t < 0x600) {
                    /* a line of dialogue is drawn into tiles of its own, two to a letter; a
                     * portrait or the commands' icons can be loaded into the same tiles */
                    uint32_t c2;
                    if (!((t - 0x580) & 1) && hdtext_canvas_char(t, &c2)) { cp = c2; is_tall = 1; }
                    else if (((t - 0x580) & 1) && hdtext_canvas_char(t - 1, &c2)) cp = 0;     /* a letter's lower half */
                    else is_pic = 1;
                } else if (t >= 0x680 && t < 0x700) {
                    if (t != T_BOX && t != T_CURSOR) cp = hdtext_win_char(t - 0x680);
                } else if (t >= 0x7BF && t <= 0x7FF) {
                    cp = hdtext_win_char(t - 0x7BF);
                } else if (t) {
                    is_pic = 1;                               /* art of its own: a shopkeeper's portrait */
                }
                w->pic[cy][cx] = (uint8_t)is_pic;
                w->picture += is_pic;
                if (!inside) continue;
                w->ch[cy - 1][cx - 1] = cp == ' ' ? 0 : cp;
                w->pal[cy - 1][cx - 1] = (uint8_t)(e >> 13 & 3);
                w->tall[cy - 1][cx - 1] = (uint8_t)(is_tall && cp && cp != ' ');
                w->boxes += t == T_BOX;
                if (cp && cp != ' ' && !is_tall) in_pal[e >> 13 & 3]++;
            }
        w->framed = !w->pic[0][0];
        w->usual = 0;
        for (int k = 1; k < 4; k++)
            if (in_pal[k] > in_pal[w->usual]) w->usual = k;
    }
    nwins = n;
    screen = group == WINGROUP_CHEST ? SCR_CHEST : kinds >> K_OTHER & 1 ? SCR_PLAIN : group == WINGROUP_SHOP ? SCR_SHOP
           : kinds >> K_EQUIP & 1 ? SCR_EQUIP : kinds >> K_ITEMS & 1 ? SCR_ITEMS
           : kinds >> K_MACROS & 1 ? SCR_MACROS : kinds >> K_TECHS & 1 ? SCR_TECHS : kinds >> K_SKILLS & 1 ? SCR_SKILLS
           : kinds >> K_STATUS & 1 ? SCR_STATUS : SCR_MAIN;
    memset(healed, 0, sizeof healed);
    if (screen == SCR_PLAIN)                             /* (PSIV_FIELDMENU_LOG: which window is not laid out anew) */
        for (int i = 0; i < n; i++)
            if (kind_of(wins[i].routine) == K_OTHER) { aside("plain", (unsigned)wins[i].routine, r16(r16(WIN_STACK + (unsigned)i * 2u))); break; }
    for (int i = 0; i < n; i++) {
        int k = kind_of(wins[i].routine);
        /* (the frames the equipment and macro screens stand in are the game's plain ones, and the
         * status page's portrait, profile, stats and equipment: theirs to draw) */
        int own_frame = wins[i].routine == R_GENERIC
                        && (screen == SCR_EQUIP || screen == SCR_MACROS || screen == SCR_SHOP
                            || (screen == SCR_STATUS && wins[i].index >= 0xC9 && wins[i].index <= 0xCC));
        wins[i].plain = screen == SCR_PLAIN || screen == SCR_CHEST || k == K_OTHER || (k == K_NOTE && !own_frame);
        /* the amount an item, a technique or a skill restored: a little window the game puts beside
         * the member's HP (one of five places) -- here it goes into the member's row */
        if (wins[i].plain && wins[i].routine == R_GENERIC && wins[i].w == 5 && wins[i].h == 3 && wins[i].x == 22
            && wins[i].y >= 3 && (wins[i].y - 3) % 5 == 0 && wins[i].y <= 23
            && (screen == SCR_ITEMS || screen == SCR_TECHS || screen == SCR_SKILLS)) {
            char *out = healed[(wins[i].y - 3) / 5];
            int len = 0;
            for (int cx = 0; cx < 3 && len < 10; cx++)
                if (wins[i].ch[0][cx] && wins[i].ch[0][cx] < 0x80) out[len++] = (char)wins[i].ch[0][cx];
            out[len] = 0;
            wins[i].plain = 0;
        }
    }
    active = 1;
    /* a row laid out across: Left and Right steer it */
    uint32_t at = routine_of(r16(WINDOW_INDEX));
    sideways = (screen == SCR_EQUIP && at == R_MEMBERS) || (screen == SCR_MACROS && at == R_MACRO_LETTERS)
            || (screen == SCR_TECHS && at == R_TECH_WHO) || (screen == SCR_SKILLS && at == R_SKILL_WHO)
            || (screen == SCR_STATUS && at == R_STATUS_WHO);

    /* the game's windows out of the picture: under each, what it saved when
     * it opened -- the last opened first, so the first one's wins */
    memset(override, 0xFF, sizeof override);
    for (int i = n - 1; i >= 0; i--) {
        unsigned p = r16(WIN_STACK + (unsigned)i * 2u);
        const Win *w = &wins[i];
        if (p + 4u + (unsigned)(w->w * w->h) * 2u > 0x8000u) continue;
        for (int cy = 0; cy < w->h; cy++)
            for (int cx = 0; cx < w->w; cx++)
                override[((row0 + w->y + cy) & 31) * 64 + ((col0 + w->x + cx) & 63)] =
                    (uint16_t)r16(p + 4u + (unsigned)(cy * w->w + cx) * 2u);
    }
    vdp_plane_a_override = override;
    memset(hidden, 0, sizeof hidden);
    uint32_t sat = (uint32_t)(vdp.reg[5] & 0x7E) << 9;
    for (int i = 0; i < 80; i++) {
        const uint8_t *s = &vdp.vram[(sat + (uint32_t)i * 8) & 0xFFFF];
        if (((s[4] << 8 | s[5]) & 0x7FF) == T_CURSOR) hidden[i] = 1;
    }
    vdp_sprite_hidden = hidden;
}

/* the game's cursor, in the cells its windows are laid out in; 0 if it has none up */
static int cursor_cell(int *col, int *row) {
    if ((r16(RED_CURSOR) & 0x7FFC) != 0x34) return 0;
    *col = ((int)r16(RED_CURSOR + 0x2C) + (int)(r16(CAMERA_X) & 7) - 128) >> 3;
    *row = ((int)r16(RED_CURSOR + 0x2E) + (int)(r16(CAMERA_Y) & 7) - 128) >> 3;
    return 1;
}

/* ------------------------------------------------------------------ the game's data */
static int item_kind(int id) { return id > 0 ? (int)ui_rom8(ITEM_DATA + (uint32_t)(id - 1) * 0x16u + I_KIND) : 0; }
static int item_bonus(int id, int off) {
    return id > 0 ? (int)(int8_t)ui_rom8(ITEM_DATA + (uint32_t)(id - 1) * 0x16u + (uint32_t)off) : 0;
}

enum { ST_ATTACK, ST_DEFENSE, ST_MAGIC_DEF, ST_STRENGTH, ST_MENTAL, ST_AGILITY, ST_DEXTERITY, N_STATS };
static const char *const STAT_NAMES[N_STATS] = {"Attack", "Defense", "Magic def", "Strength", "Mental", "Agility", "Dexterity"};

/* the stats as the game works them out (UpdateCharModStats) from what is
 * worn: `rec` the character's record, `worn` four item IDs */
static void stats_with(unsigned rec, const uint8_t *worn, int *out) {
    int str = (int)r8(rec + 0x18), mental = (int)r8(rec + 0x1B), agi = (int)r8(rec + 0x1E), dex = (int)r8(rec + 0x21);
    int b[7] = {0};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 7; k++) b[k] += item_bonus(worn[i], 0x0B + k);
    out[ST_STRENGTH] = (str + b[0]) & 0xFF;
    out[ST_MENTAL] = (mental + b[1]) & 0xFF;
    out[ST_AGILITY] = (agi + b[2]) & 0xFF;
    out[ST_DEXTERITY] = (dex + b[3]) & 0xFF;
    out[ST_ATTACK] = (str + b[0] + b[4]) & 0xFFFF;
    out[ST_DEFENSE] = (agi + b[2] + b[5]) & 0xFFFF;
    out[ST_MAGIC_DEF] = (mental + b[1] + b[6]) & 0xFFFF;
}
/* the stats in the record: while the game tries something on, what they would be */
static void stats_shown(unsigned rec, int *out) {
    out[ST_STRENGTH] = (int)r8(rec + 0x19);
    out[ST_MENTAL] = (int)r8(rec + 0x1C);
    out[ST_AGILITY] = (int)r8(rec + 0x1F);
    out[ST_DEXTERITY] = (int)r8(rec + 0x22);
    out[ST_ATTACK] = (int)r16(rec + 0x24);
    out[ST_DEFENSE] = (int)r16(rec + 0x28);
    out[ST_MAGIC_DEF] = (int)r16(rec + 0x2C);
}

/* "Weapon, one hand. Attack +12, Agility +2." -- or, of something used, what it does */
static void item_line(int id, char *out, size_t n) {
    static const char *const KIND[8] = {"", "Weapon, one hand", "Weapon, one hand, wide", "Weapon, both hands",
                                        "Weapon, both hands, wide", "Shield", "Headgear", "Armor"};
    static const char *const BONUS[7] = {"Strength", "Mental", "Agility", "Dexterity", "Attack", "Defense", "Magic def"};
    static const int ORDER[7] = {4, 5, 6, 0, 1, 2, 3};
    int kind = item_kind(id), len;
    const char *does = battledesc_item(id);
    if (kind < 1 || kind > 7) { snprintf(out, n, "%s", does ? does : ""); return; }
    len = snprintf(out, n, "%s.", KIND[kind]);
    for (int k = 0, first = 1; k < 7; k++) {
        int v = item_bonus(id, 0x0B + ORDER[k]);
        if (!v || len >= (int)n) continue;
        len += snprintf(out + len, n - (size_t)len, "%s %s %+d", first ? "" : ",", BONUS[ORDER[k]], v);
        first = 0;
    }
    if (does && len < (int)n) snprintf(out + len, n - (size_t)len, ". In battle: %s", does);   /* used as an item there */
}

static void item_wearers(int id, char *out, size_t n) {
    unsigned who = id > 0 ? ui_rom16(ITEM_DATA + (uint32_t)(id - 1) * 0x16u + I_WHO) : 0;
    int len = snprintf(out, n, "For ");
    for (int c = 0, first = 1; c < 11; c++)
        if (who >> c & 1 && len < (int)n) {
            len += snprintf(out + len, n - (size_t)len, "%s%s", first ? "" : ", ", UI_CHAR_NAMES[c]);
            first = 0;
        }
}

static const char *status_word(unsigned st, int brief) {
    return st & 0x44 ? "Down" : st & 0x02 ? (brief ? "Par" : "Paralyzed") : st & 0x01 ? (brief ? "Psn" : "Poisoned")
         : st & 0x28 ? (brief ? "Slp" : "Asleep") : st & 0x10 ? (brief ? "Seal" : "Sealed") : "";
}

/* ------------------------------------------------------------------ drawing */
#define WHITE   0xFFFFFFu
#define SOFT    0xAEBEF0u          /* labels */
#define FAINT   0x7C8CCCu
#define NAVY    0x16287Au
#define BAR     0xE8ECF8u          /* the cursor's row */
#define BAR_OFF 0x4868C8u          /* a choice made, the cursor elsewhere */
#define UP      0x8CF0A0u
#define DOWN    0xFF9E9Eu
#define CAP     5.6f               /* capitals' height, game pixels */

static UiCanvas cv;
static float S;                    /* output pixels per game pixel */

static void win(float x0, float y0, float x1, float y1) { ui_frame_window(&cv, x0 * S, y0 * S, x1 * S, y1 * S, S); }
static float text(float x, float base, const char *s, uint32_t c) { return ui_text(&cv, x * S, base * S, CAP * S, s, c, 255) / S; }
static void text_right(float x, float base, const char *s, uint32_t c) { ui_text_right(&cv, x * S, base * S, CAP * S, s, c, 255); }
static void text_mid(float x, float base, const char *s, uint32_t c) { ui_text_center(&cv, x * S, base * S, CAP * S, s, c, 255); }
/* the bar behind a row whose text sits on `base` */
static void bar(float x0, float x1, float base, uint32_t c) {
    ui_round_rect(&cv, x0 * S, (base - 8.6f) * S, x1 * S, (base + 3.2f) * S, 1.5f * S, c, 255);
}
/* two lines at the foot of a screen: the first wrapped, the second if there is room */
static void foot(float x0, float x1, float y1, const char *a, const char *b) {
    win(x0, y1 - 27.0f, x1, y1);
    int used = ui_text_wrap(&cv, (x0 + 8.0f) * S, (y1 - 16.6f) * S, (x1 - x0 - 16.0f) * S, 10.2f * S, 2, CAP * S, a, WHITE, 255);
    if (used < 2 && b && b[0]) text(x0 + 8.0f, y1 - 6.4f, b, SOFT);
}

/* ---- a window as it stands: the game's own words, in this menu's frame */
static int put_utf8(char *out, int n, int max, uint32_t c) {
    if (c < 0x80) { if (n < max) out[n++] = (char)c; }
    else if (c < 0x800) { if (n + 1 < max) { out[n++] = (char)(0xC0 | c >> 6); out[n++] = (char)(0x80 | (c & 0x3F)); } }
    else if (n + 2 < max) { out[n++] = (char)(0xE0 | c >> 12); out[n++] = (char)(0x80 | (c >> 6 & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    return n;
}
static int numeric(uint32_t c) { return (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' || c == '.'; }
/* the word from cell `k` of a row is a number (a figure the game set from the right) */
static int number_at(const Win *w, int cy, int k, int iw) {
    int digits = 0;
    for (; k < iw && k < 40 && w->ch[cy][k]; k++) {
        if (!numeric(w->ch[cy][k])) return 0;
        digits |= w->ch[cy][k] >= '0' && w->ch[cy][k] <= '9';
    }
    return digits;
}

/* "MONOMATE" -> "Monomate": the game writes its names in capitals; the short labels stay */
static void soften(char *s) {
    static const char *const KEEP[] = {"HP", "TP", "LV", "MST", "EXP", "ATK", "DFS", "POW", "MTL", "OK", NULL};
    for (char *p = s; *p;) {
        char *e = p;
        int caps = 1;
        while (*e && *e != ' ') { if (!(*e >= 'A' && *e <= 'Z') && *e != '-') caps = 0; e++; }
        int len = (int)(e - p), keep = !caps || len < 2;
        for (int k = 0; KEEP[k] && !keep; k++) keep = (int)strlen(KEEP[k]) == len && !strncmp(KEEP[k], p, (size_t)len);
        if (!keep)
            for (char *q = p + 1; q < e; q++)
                if (*q >= 'A' && *q <= 'Z' && q[-1] != '-') *q = (char)(*q - 'A' + 'a');
        p = *e ? e + 1 : e;
    }
}

/* a window's picture cells (a portrait) from the game's own tiles, its top left at (x, y): each
 * of the game's pixels a whole number of output pixels */
static void draw_picture(const Win *w, float x, float y) {
    int k = (int)S, px0 = (int)(x * S + 0.5f), py0 = (int)(y * S + 0.5f);
    if (k < 1) k = 1;
    for (int cy = 0; cy < w->h && cy < 30; cy++)
        for (int cx = 0; cx < w->w && cx < 42; cx++) {
            if (!w->pic[cy][cx]) continue;
            unsigned e = w->raw[cy][cx];
            const uint8_t *t = &vdp.vram[(e & 0x7FF) * 32];
            int pal = (int)(e >> 13 & 3), hf = (int)(e >> 11 & 1), vf = (int)(e >> 12 & 1);
            for (int ty = 0; ty < 8; ty++)
                for (int tx = 0; tx < 8; tx++) {
                    int sx = hf ? 7 - tx : tx, sy = vf ? 7 - ty : ty;
                    int idx = t[sy * 4 + sx / 2] >> (sx & 1 ? 0 : 4) & 15;
                    if (!idx) continue;
                    uint32_t c = 0xFF000000u | vdp_color(pal * 16 + idx);
                    int bx = px0 + (cx * 8 + tx) * k, by = py0 + (cy * 8 + ty) * k;
                    for (int yy = by; yy < by + k; yy++) {
                        if (yy < cv.y0 || yy >= cv.y1) continue;
                        for (int xx = bx; xx < bx + k; xx++)
                            if (xx >= cv.x0 && xx < cv.x1) cv.px[(size_t)yy * (size_t)cv.stride + (size_t)xx] = c;
                    }
                }
        }
}

/* a row of a window that holds a line of dialogue: as text, the game's own capitals; 0 if it holds none */
static int tall_text(const Win *w, int cy, char *out, int max, int *first) {
    int n = 0, gap = 0, any = 0;
    for (int cx = 0; cx < w->w - 2 && cx < 40; cx++) {
        if (!w->ch[cy][cx]) { gap = n > 0; continue; }
        if (!w->tall[cy][cx]) return 0;
        if (!any) *first = cx;
        any = 1;
        if (gap) n = put_utf8(out, n, max - 1, ' ');
        gap = 0;
        n = put_utf8(out, n, max - 1, w->ch[cy][cx]);
    }
    out[n] = 0;
    return any;
}

static void draw_plain(const Win *w, float ox, float oy, int has_cursor, int ccol, int crow) {
    float fx0 = ox + (float)w->x * 8.0f + 1.5f, fx1 = ox + (float)(w->x + w->w) * 8.0f - 1.5f;
    if (w->framed) win(fx0, oy + (float)w->y * 8.0f + 1.5f, fx1, oy + (float)(w->y + w->h) * 8.0f - 1.5f);
    if (w->picture) draw_picture(w, ox + (float)w->x * 8.0f, oy + (float)w->y * 8.0f);
    int iw = w->w - 2, ih = w->h - 2;
    for (int cy = 0; cy < ih && cy < 28; cy++) {
        float base = oy + (float)(w->y + 1 + cy) * 8.0f + 6.9f;
        int lit = has_cursor && crow == w->y + 1 + cy, lit_done = 0;
        char said[128];
        int first = 0;
        if (tall_text(w, cy, said, (int)sizeof said, &first)) {     /* a line of dialogue: larger, over two rows */
            ui_text(&cv, (ox + (float)(w->x + 1 + first) * 8.0f + 0.5f) * S, (base + 4.6f) * S, 6.3f * S, said, WHITE, 255);
            continue;
        }
        for (int cx = 0; cx < iw && cx < 40;) {
            if (!w->ch[cy][cx]) { cx++; continue; }
            /* a run: words a single blank apart; a number stands alone, set from its right end */
            int start = cx, end = cx, num = number_at(w, cy, cx, iw);
            char s[128];
            int n = 0;
            for (;;) {
                while (end < iw && end < 40 && w->ch[cy][end]) n = put_utf8(s, n, (int)sizeof s - 1, w->ch[cy][end++]);
                if (num || end + 1 >= iw || end + 1 >= 40 || !w->ch[cy][end + 1] || number_at(w, cy, end + 1, iw)) break;
                n = put_utf8(s, n, (int)sizeof s - 1, ' ');
                end++;
            }
            s[n] = 0;
            soften(s);
            if (!strcmp(s, "Dsc")) snprintf(s, sizeof s, "Drop");      /* (the game's short for "discard") */
            float x0 = ox + (float)(w->x + 1 + start) * 8.0f + 0.5f, x1 = ox + (float)(w->x + 1 + end) * 8.0f - 0.5f;
            uint32_t c = w->pal[cy][start] != w->usual ? FAINT : WHITE;
            if (has_cursor && !lit && start > 0
                && ((w->raw[cy + 1][start] & 0x7FF) == T_BOX || (start > 1 && (w->raw[cy + 1][start - 1] & 0x7FF) == T_BOX)))
                hit_cell(fx0 + 3.0f, fx1 - 3.0f, base, w->y + 1 + cy);      /* another choice of the window the cursor is in */
            if (lit && !lit_done && w->x + 1 + start > ccol) {      /* the choice the cursor is at */
                float to = (num ? x1 : x0 + ui_text_width(s, CAP * S) / S) + 4.0f;
                bar(ox + (float)ccol * 8.0f + 3.0f, to < fx1 - 3.0f ? to : fx1 - 3.0f, base, BAR);
                c = NAVY;
                lit_done = 1;
            }
            if (num) text_right(x1, base, s, c);
            else text(x0, base, s, c);
            cx = end;
        }
    }
}

/* a row of a window as a line of text; returns its length */
static int row_text(const Win *w, int cy, char *out, int max) {
    int n = 0, gap = 0, tall = 0;
    for (int cx = 0; cx < w->w - 2 && cx < 40; cx++) {
        if (!w->ch[cy][cx]) { gap = n > 0; continue; }
        tall |= w->tall[cy][cx];
        if (gap) n = put_utf8(out, n, max - 1, ' ');
        gap = 0;
        n = put_utf8(out, n, max - 1, w->ch[cy][cx]);
    }
    out[n] = 0;
    if (!tall) soften(out);                              /* (a line of dialogue has its own capitals) */
    return n;
}

/* ---- the screens laid out anew */
static void draw_commands(float x0, float y0, float x1, int cursor, int focus) {
    static const char *const NAMES[7] = {"Items", "Techniques", "Skills", "Equipment", "Status", "Talk", "Macros"};
    win(x0, y0, x1, y0 + 10.0f + 7 * 13.0f);
    for (int i = 0; i < 7; i++) {
        float base = y0 + 15.0f + (float)i * 13.0f;
        uint32_t c = WHITE;
        if (focus) hit(x0 + 4.0f, x1 - 4.0f, base, R_MENU, OPTION_1, i, PAD_UP, PAD_DOWN, 0, 0);
        if (i == cursor) {
            bar(x0 + 4.0f, x1 - 4.0f, base, focus ? BAR : BAR_OFF);
            if (focus) c = NAVY;
        }
        text(x0 + 9.0f, base, i == 5 && r8(PARTY_SLOTS + 1) >= 0x80 ? "Mumble" : NAMES[i], c);
    }
}

static void draw_meseta(float x0, float y1, float x1) {
    char t[32];
    win(x0, y1 - 19.0f, x1, y1);
    snprintf(t, sizeof t, "%lu", (unsigned long)ui_ram32(MONEY));
    text_right(x1 - 30.0f, y1 - 6.6f, t, WHITE);
    text(x1 - 27.0f, y1 - 6.6f, "mst", SOFT);
}

static int party(int *members) {
    int n = 0;
    for (int i = 0; i < 5; i++)
        if (r8(PARTY_SLOTS + (unsigned)i) < 11) members[n++] = (int)r8(PARTY_SLOTS + (unsigned)i);
    return n;
}

/* everyone with their level (or how they are), HP and TP; `cursor` a row, or -1 */
static void draw_party(float x0, float y0, float x1, float row_h, int levels, int cursor, int focus) {
    int members[5], n = party(members);
    if (!n) return;
    float w = x1 - x0;
    win(x0, y0, x1, y0 + 9.0f + (float)n * row_h);
    for (int i = 0; i < n; i++) {
        unsigned rec = CHAR_STATS + (unsigned)members[i] * 0x80u;
        float base = y0 + 7.5f + row_h * 0.5f + (float)i * row_h;
        unsigned st = r8(rec + C_STATUS);
        int down = (st & 0x44) != 0, on = i == cursor && focus;
        uint32_t c = on ? NAVY : down ? FAINT : WHITE, lc = on ? NAVY : SOFT;
        char t[40];
        if (party_routine) hit(x0 + 4.0f, x1 - 4.0f, base, party_routine, OPTION_4, i, PAD_UP, PAD_DOWN, 0, 0);
        if (i == cursor) bar(x0 + 4.0f, x1 - 4.0f, base, focus ? BAR : BAR_OFF);
        /* across: spread over a wide window; in a narrow one (the 4:3 view) packed from the right */
        int narrow = w < 200.0f;
        float second = narrow ? x0 + (levels ? 33.0f : 38.0f) : x0 + w * (levels ? 0.27f : 0.24f);
        float hp_label = narrow ? x1 - 99.0f : x0 + w * 0.44f, hp_right = narrow ? x1 - 54.0f : x0 + w * 0.44f + 54.0f;
        float tp_label = narrow ? x1 - 51.0f : x0 + w * 0.72f;
        text(x0 + 9.0f, base, UI_CHAR_NAMES[members[i]], c);
        if (levels) {
            text(second, base, "LV", lc);
            snprintf(t, sizeof t, "%u", r16(rec + C_LEVEL));
            text_right(second + (narrow ? 22.0f : 26.0f), base, t, c);
        } else if (healed[i][0]) {                   /* just restored: how much */
            snprintf(t, sizeof t, "+%s", healed[i]);
            text(second, base, t, on ? NAVY : UP);
        } else if (st) {
            text(second, base, status_word(st, narrow), on ? NAVY : DOWN);
        }
        text(hp_label, base, "HP", lc);
        snprintf(t, sizeof t, "%u/%u", r16(rec + C_HP), r16(rec + C_HP + 2));
        text_right(hp_right, base, t, down && !on ? DOWN : c);
        text(tp_label, base, "TP", lc);
        snprintf(t, sizeof t, "%u/%u", r16(rec + C_TP), r16(rec + C_TP + 2));
        text_right(x1 - 9.0f, base, t, c);
    }
}

/* ---- items: the pack a page to a column, what to do with one, on whom */
static void draw_items(float x0, float y0, float x1, float y1) {
    uint32_t now = routine_of(r16(WINDOW_INDEX));
    int doing = open_now(R_ITEM_DO), whom = open_now(R_ITEM_WHO);
    int count = 0;
    while (count < 40 && r8(INVENTORY + (unsigned)count)) count++;
    int page = (int)r16(ITEM_PAGE), at = page * 8 + ((int)r8(OPTION_2) & 7);
    if (at >= count) at = count - 1;
    int item = at >= 0 ? (int)r8(INVENTORY + (unsigned)at) : 0;
    float w = x1 - x0;

    /* the pack: the game's pages of eight, side by side -- Left and Right turn them */
    int pages = (count + 7) / 8, cols = (int)((w - 8.0f) / 70.0f), first = 0;
    if (cols > 5) cols = 5;
    if (cols < 1) cols = 1;
    if (page >= cols) first = page - cols + 1;
    float ly1 = y0 + 10.0f + 8 * 12.2f, cw = (w - 8.0f) / (float)cols;
    win(x0, y0, x1, ly1);
    for (int i = first * 8; i < count && i < (first + cols) * 8; i++) {
        float cx0 = x0 + 4.0f + cw * (float)(i / 8 - first), base = y0 + 14.2f + (float)(i % 8) * 12.2f;
        int id = (int)r8(INVENTORY + (unsigned)i), k = item_kind(id);
        uint32_t c = k >= 1 && k <= 7 ? SOFT : WHITE;          /* things to wear: quieter than things to use */
        hit(cx0, cx0 + cw, base, R_ITEMS, OPTION_2, i % 8, PAD_UP, PAD_DOWN, ITEM_PAGE, i / 8);
        if (i == at) {
            bar(cx0, cx0 + cw, base, now == R_ITEMS ? BAR : BAR_OFF);
            c = now == R_ITEMS ? NAVY : WHITE;
        }
        text(cx0 + 5.0f, base, ui_item_name(id), c);
    }
    if (first > 0) ui_triangle(&cv, (x0 + 4.4f) * S, (y0 + 6.0f) * S, 2.8f * S, 3, WHITE, 255);
    if (first + cols < pages) ui_triangle(&cv, (x1 - 4.4f) * S, (y0 + 6.0f) * S, 2.8f * S, 2, WHITE, 255);

    /* what to do with it, and the party: who it would be used on */
    static const char *const DO[3] = {"Use", "Look", "Discard"};
    float my0 = ly1 + 3.0f, my1 = y1 - 30.0f, ax1 = x0 + 62.0f;
    int choice = (int)r8(OPTION_3);
    win(x0, my0, ax1, my0 + 10.0f + 3 * 12.5f);
    for (int i = 0; i < 3; i++) {
        float base = my0 + 14.5f + (float)i * 12.5f;
        uint32_t c = doing ? WHITE : FAINT;
        if (doing) hit(x0 + 4.0f, ax1 - 4.0f, base, R_ITEM_DO, OPTION_3, i, PAD_UP, PAD_DOWN, 0, 0);
        if (doing && i == choice) {
            bar(x0 + 4.0f, ax1 - 4.0f, base, now == R_ITEM_DO ? BAR : BAR_OFF);
            if (now == R_ITEM_DO) c = NAVY;
        }
        text(x0 + 9.0f, base, DO[i], c);
    }
    int members[5], n = party(members);
    float row_h = n ? (my1 - my0 - 9.0f) / (float)n : 12.0f;
    if (row_h > 13.5f) row_h = 13.5f;
    party_routine = whom ? R_ITEM_WHO : 0;
    draw_party(ax1 + 3.0f, my0, x1, row_h, 0, whom ? (int)r8(OPTION_4) : -1, now == R_ITEM_WHO);
    party_routine = 0;

    char a[200] = "", b[120] = "";
    if (now == R_ITEM_WHO) snprintf(a, sizeof a, "Use the %s on whom?", ui_item_name(item));
    else if (now == R_ITEM_DO && choice == 1) snprintf(a, sizeof a, "Look the %s over.", ui_item_name(item));
    else if (now == R_ITEM_DO && choice == 2) snprintf(a, sizeof a, "Throw the %s away.", ui_item_name(item));
    else if (item) {
        int k = item_kind(item);
        item_line(item, a, sizeof a);
        if (k >= 1 && k <= 7) item_wearers(item, b, sizeof b);
    }
    foot(x0, x1, y1, a, b);
}

/* ---- techniques and skills: the party across the top, what the member can use in camp, on whom */
static void member_strip(float x0, float y0, float x1, const int *members, int n, int sel, int focus) {
    win(x0, y0, x1, y0 + 19.0f);
    for (int i = 0; i < n; i++) {
        float cx0 = x0 + 4.0f + (x1 - x0 - 8.0f) * (float)i / (float)n, cx1 = x0 + 4.0f + (x1 - x0 - 8.0f) * (float)(i + 1) / (float)n;
        uint32_t c = WHITE;
        if (strip_routine) hit(cx0, cx1, y0 + 12.8f, strip_routine, strip_var, i, PAD_UP, PAD_DOWN, 0, 0);
        if (i == sel) {
            bar(cx0, cx1, y0 + 12.8f, focus ? BAR : BAR_OFF);
            if (focus) c = NAVY;
        }
        text_mid((cx0 + cx1) / 2.0f, y0 + 12.8f, UI_CHAR_NAMES[members[i]], c);
    }
}

static void draw_arts(float x0, float y0, float x1, float y1, int skills) {
    uint32_t r_who = skills ? R_SKILL_WHO : R_TECH_WHO, r_list = skills ? R_SKILL_LIST : R_TECH_LIST;
    uint32_t r_target = skills ? R_SKILL_TARGET : R_TECH_TARGET, data = skills ? SKILL_DATA : TECH_DATA;
    uint32_t now = routine_of(r16(WINDOW_INDEX));
    const char *what = skills ? "skill" : "technique";
    int members[5], n = party(members), sel = (int)r8(OPTION_2);
    if (!n) return;
    if (sel >= n) sel = n - 1;
    int id = members[sel];
    unsigned rec = CHAR_STATS + (unsigned)id * 0x80u;
    float w = x1 - x0;
    strip_routine = r_who;
    strip_var = OPTION_2;
    member_strip(x0, y0, x1, members, n, sel, now == r_who);

    /* what they can use here: the game's list once it has made it, before that the same from the record */
    uint8_t ids[16];
    int count = 0, cursor = -1, held = skills ? 8 : 16;
    if (open_now(r_list)) {
        count = (int)r16(ART_COUNT);
        if (count > 16) count = 16;
        for (int i = 0; i < count; i++) ids[i] = (uint8_t)r8(ART_LIST + (unsigned)i);
        cursor = (skills ? 0 : (int)r16(ART_PAGE) * 8) + (int)r8(OPTION_3);
        if (cursor >= count) cursor = count - 1;
    } else {
        for (int i = 0; i < held; i++) {
            int a = (int)r8(rec + (unsigned)(skills ? C_SKILLS : C_TECHS) + (unsigned)i);
            if (a && (ui_rom8(data + (uint32_t)(a - 1) * 8u + 2) & 0x20)) ids[count++] = (uint8_t)a;
        }
    }
    int tp = (int)r16(rec + C_TP);

    /* side by side in a wide window; one over the other in a narrow one (the 4:3 view) */
    float by0 = y0 + 22.0f, by1 = y1 - 30.0f, ly1 = by1, lx1 = x1, px0, py0, row_h;
    if (w >= 280.0f) {
        lx1 = x0 + w * 0.46f;
        px0 = lx1 + 3.0f;
        py0 = by0;
        row_h = (by1 - by0 - 9.0f) / (float)n;
        if (row_h > 13.5f) row_h = 13.5f;
    } else {
        row_h = 11.6f;
        ly1 = by1 - (9.0f + (float)n * row_h) - 3.0f;
        px0 = x0;
        py0 = ly1 + 3.0f;
    }
    char t[64];
    win(x0, by0, lx1, ly1);
    /* the game's pages of eight, side by side (Left and Right turn them, as in the game); in a
     * wide window the member's name and TP head the list */
    int head = w >= 280.0f, cols = count > 8 ? 2 : 1;
    float top = by0 + (head ? 29.0f : 14.0f), pitch = (ly1 - top - 2.0f) / 8.0f, cw = (lx1 - x0 - 8.0f) / (float)cols;
    if (pitch > 12.2f) pitch = 12.2f;
    if (head) {
        text(x0 + 9.0f, by0 + 13.5f, UI_CHAR_NAMES[id], now == r_who ? SOFT : WHITE);
        if (!skills) {
            snprintf(t, sizeof t, "TP %d/%u", tp, r16(rec + C_TP + 2));
            text_right(lx1 - 9.0f, by0 + 13.5f, t, SOFT);
        }
    }
    if (!count) text(x0 + 9.0f, top, skills ? "No skill to use here" : "No technique to use here", FAINT);
    for (int i = 0; i < count && i < cols * 8; i++) {
        float cx0 = x0 + 4.0f + cw * (float)(i / 8), base = top + (float)(i % 8) * pitch;
        int a = ids[i], cannot;
        if (skills) {
            int slot = 0;
            while (slot < 7 && (int)r8(rec + C_SKILLS + (unsigned)slot) != a) slot++;
            int left = (int)r8(rec + C_USES + (unsigned)slot * 2u), full = (int)r8(rec + C_USES + (unsigned)slot * 2u + 1);
            snprintf(t, sizeof t, "%d/%d", left, full);
            cannot = left == 0;
        } else {
            int cost = (int)ui_rom8(TECH_DATA + (uint32_t)(a - 1) * 8u + 1);
            snprintf(t, sizeof t, "%d TP", cost);
            cannot = cost > tp;
        }
        uint32_t c = cannot ? FAINT : now == r_who ? SOFT : WHITE, sc = cannot ? FAINT : SOFT;
        if (open_now(r_list)) hit(cx0, cx0 + cw, base, r_list, OPTION_3, skills ? i : i % 8, PAD_UP, PAD_DOWN, skills ? 0 : ART_PAGE, i / 8);
        if (i == cursor) {
            bar(cx0, cx0 + cw, base, now == r_list ? BAR : BAR_OFF);
            if (now == r_list) c = sc = NAVY;
            else c = WHITE;
        }
        text(cx0 + 5.0f, base, skills ? ui_skill_name(a) : ui_tech_name(a), c);
        text_right(cx0 + cw - 5.0f, base, t, sc);
    }

    /* the party: who it would be used on */
    party_routine = open_now(r_target) ? r_target : 0;
    draw_party(px0, py0, x1, row_h, 0, open_now(r_target) ? (int)r8(OPTION_4) : -1, now == r_target);
    party_routine = 0;

    char a[200] = "", b[120] = "";
    int art = cursor >= 0 ? ids[cursor] : 0;
    const char *name = art ? (skills ? ui_skill_name(art) : ui_tech_name(art)) : "";
    if (now == r_who) {
        snprintf(a, sizeof a, "Whose %ss?", what);
        snprintf(b, sizeof b, "Those that can be used outside battle.");
    } else if (now == r_target) {
        snprintf(a, sizeof a, "Use %s on whom?", name);
    } else if (art) {
        const char *does = skills ? battledesc_skill(art) : battledesc_tech(art);
        snprintf(a, sizeof a, "%s", does ? does : name);
    }
    foot(x0, x1, y1, a, b);
}

/* ---- status: status or order; then whose, and their pages -- the profile, the techniques, the skills */
static uint32_t next_level_exp(int id, unsigned rec) {
    int level = (int)r16(rec + C_LEVEL), start = (int)ui_rom8(START_LEVELS + (uint32_t)id);
    if (level >= 99 || level < start) return 0;
    uint32_t need = ui_rom32(ui_rom32(LEVEL_TABLES + (uint32_t)id * 4u) + (uint32_t)(level - start) * 0x16u), have = ui_ram32(rec + C_EXP);
    return need > have ? need - have : 0;
}

static void job_name(unsigned job, char *out, int n) {
    uint32_t a = JOB_NAMES;
    for (unsigned i = 0; i < job && i < 32; i++) {
        while (ui_rom8(a) != 0xFF && a < JOB_NAMES + 0x80) a++;
        a++;
    }
    uint8_t raw[20];
    int len = 0;
    while (len < 18 && ui_rom8(a + (uint32_t)len) != 0xFF) { raw[len] = (uint8_t)ui_rom8(a + (uint32_t)len); len++; }
    raw[len] = 0xFF;
    ui_decode_name(raw, len + 1, out, n);
}

/* a label, a bar and "49/49" on one line */
static void gauge(float x0, float x1, float base, const char *label, unsigned have, unsigned full, uint32_t c) {
    char t[24];
    text(x0, base, label, SOFT);
    snprintf(t, sizeof t, "%u/%u", have, full);
    text_right(x1, base, t, WHITE);
    float bx0 = x0 + 16.0f, bx1 = x1 - 4.0f - ui_text_width("999/999", CAP * S) / S;
    if (bx1 > bx0 + 8.0f)
        ui_bar(&cv, bx0 * S, (base - 4.6f) * S, bx1 * S, 3.4f * S, full ? (float)have / (float)full : 0.0f, c, 255);
}

static void draw_status(float x0, float y0, float x1, float y1) {
    uint32_t now = routine_of(r16(WINDOW_INDEX));
    float w = x1 - x0;
    int members[5], n = party(members);
    if (!n) return;
    if (!open_now(R_STATUS_WHO)) {                       /* status, or the order they stand in */
        static const char *const WHICH[2] = {"Status", "Order"};
        float ox1 = x0 + 62.0f;
        win(x0, y0, ox1, y0 + 10.0f + 2 * 12.5f);
        for (int i = 0; i < 2; i++) {
            float base = y0 + 14.5f + (float)i * 12.5f;
            uint32_t c = WHITE;
            hit(x0 + 4.0f, ox1 - 4.0f, base, R_STATE_OPTIONS, OPTION_2, i, PAD_UP, PAD_DOWN, 0, 0);
            if (i == (int)r8(OPTION_2)) {
                bar(x0 + 4.0f, ox1 - 4.0f, base, now == R_STATE_OPTIONS ? BAR : BAR_OFF);
                if (now == R_STATE_OPTIONS) c = NAVY;
            }
            text(x0 + 9.0f, base, WHICH[i], c);
        }
        draw_party(ox1 + 3.0f, y0, x1, 16.0f, 1, -1, 0);
        foot(x0, x1, y1, r8(OPTION_2) ? "The order the party stands in." : "Each member's profile, techniques and skills.", "");
        return;
    }
    int sel = (int)r8(OPTION_3);
    if (sel >= n) sel = n - 1;
    int id = members[sel];
    unsigned rec = CHAR_STATS + (unsigned)id * 0x80u;
    int page = open_now(R_STATUS_SKILL_B) || open_now(R_STATUS_SKILL_C) ? 3
             : open_now(R_STATUS_TECH_B) || open_now(R_STATUS_TECH_C) ? 2 : open_now(R_STATUS_EXP) ? 1 : 0;
    strip_routine = R_STATUS_WHO;
    strip_var = OPTION_3;
    member_strip(x0, y0, x1, members, n, sel, now == R_STATUS_WHO);
    float by0 = y0 + 22.0f, by1 = y1 - 30.0f;
    char t[64], a[160] = "", b[120] = "";

    if (page <= 1) {                                     /* the profile (before a member is chosen: the one under the cursor) */
        float lx1 = x0 + w * 0.5f - 1.5f, rx0 = lx1 + 3.0f, P = w >= 260.0f ? 48.0f : 40.0f;
        uint32_t ink = page ? WHITE : SOFT;
        win(x0, by0, lx1, by1);
        ui_portraits_init();
        const UiImage *face = ui_portrait(id);
        if (face && face->w > 0) {                       /* pixel art: a whole number of output pixels to each of its own */
            int k = (int)(P * S / (float)face->w);
            if (k < 1) k = 1;
            P = (float)(k * face->w) / S;
        }
        if (face) {
            ui_round_rect(&cv, (x0 + 7.0f) * S, (by0 + 7.0f) * S, (x0 + 9.0f + P) * S, (by0 + 9.0f + P) * S, 1.5f * S, 0xDDE1EE, 255);
            ui_image(&cv, face, (x0 + 8.0f) * S, (by0 + 8.0f) * S, P * S, 0, 255);
        }
        float tx = x0 + 15.0f + P;
        text(tx, by0 + 17.0f, UI_CHAR_NAMES[id], WHITE);
        job_name(r16(rec + C_JOB), t, (int)sizeof t);
        text(tx, by0 + 29.0f, t, SOFT);
        snprintf(t, sizeof t, "Level %u", r16(rec + C_LEVEL));
        text(tx, by0 + 41.0f, t, ink);
        unsigned age = ui_rom16(AGES + (uint32_t)id * 2u);
        if (age) snprintf(t, sizeof t, "Age %u", age);
        else snprintf(t, sizeof t, "Age ?");
        text(tx, by0 + 53.0f, t, ink);
        float gy = by0 + (P + 24.0f > 68.0f ? P + 24.0f : 68.0f);
        unsigned st = r8(rec + C_STATUS);
        gauge(x0 + 8.0f, lx1 - 8.0f, gy, "HP", r16(rec + C_HP), r16(rec + C_HP + 2), 0x6FD080);
        gauge(x0 + 8.0f, lx1 - 8.0f, gy + 12.5f, "TP", r16(rec + C_TP), r16(rec + C_TP + 2), 0x6AA8F0);
        text(x0 + 8.0f, gy + 29.0f, "Experience", SOFT);
        snprintf(t, sizeof t, "%lu", (unsigned long)ui_ram32(rec + C_EXP));
        text_right(lx1 - 8.0f, gy + 29.0f, t, ink);
        text(x0 + 8.0f, gy + 40.6f, "To next level", SOFT);
        snprintf(t, sizeof t, "%lu", (unsigned long)next_level_exp(id, rec));
        text_right(lx1 - 8.0f, gy + 40.6f, t, ink);
        if (st) text(x0 + 8.0f, gy + 54.0f, status_word(st, 0), DOWN);

        /* every stat, and what they wear */
        static const char *const SLOT[4] = {"Head", "R hand", "L hand", "Body"};
        static const int SLOT_AT[4] = {2, 0, 1, 3};
        int have[N_STATS];
        stats_shown(rec, have);
        win(rx0, by0, x1, by1);
        for (int k = 0; k < N_STATS; k++) {
            float base = by0 + 14.0f + (float)k * 11.6f;
            text(rx0 + 8.0f, base, STAT_NAMES[k], SOFT);
            snprintf(t, sizeof t, "%d", have[k]);
            text_right(x1 - 8.0f, base, t, ink);
        }
        float line = by0 + 14.0f + 7 * 11.6f - 5.0f;
        ui_round_rect(&cv, (rx0 + 6.0f) * S, line * S, (x1 - 6.0f) * S, (line + 0.8f) * S, 0, SOFT, 150);
        int both = item_kind((int)r8(rec + C_EQUIP)) == 3 || item_kind((int)r8(rec + C_EQUIP)) == 4;
        for (int i = 0; i < 4; i++) {
            float base = line + 12.5f + (float)i * 12.0f;
            int item = (int)r8(rec + C_EQUIP + (unsigned)SLOT_AT[i]);
            text(rx0 + 8.0f, base, SLOT[i], SOFT);
            if (i == 2 && both) text_right(x1 - 8.0f, base, "(both hands)", FAINT);
            else text_right(x1 - 8.0f, base, item ? ui_item_name(item) : "-", item ? ink : FAINT);
        }
        if (!page) snprintf(a, sizeof a, "Whose status?");
        else {
            snprintf(a, sizeof a, "%s: the profile, page 1 of 3.", UI_CHAR_NAMES[id]);
            snprintf(b, sizeof b, "Choose again for the techniques.");
        }
    } else {                                             /* the techniques, or the skills: all of them, for battle and camp */
        int skills = page == 3, held = skills ? 8 : 16, count = 0, tp = (int)r16(rec + C_TP);
        uint8_t ids[16], slots[16];
        for (int i = 0; i < held; i++) {
            int art = (int)r8(rec + (unsigned)(skills ? C_SKILLS : C_TECHS) + (unsigned)i);
            if (art) { ids[count] = (uint8_t)art; slots[count++] = (uint8_t)i; }
        }
        win(x0, by0, x1, by1);
        text(x0 + 9.0f, by0 + 13.5f, skills ? "Skills" : "Techniques", SOFT);
        if (!skills) {
            snprintf(t, sizeof t, "TP %d/%u", tp, r16(rec + C_TP + 2));
            text_right(x1 - 9.0f, by0 + 13.5f, t, SOFT);
        }
        int per = skills ? 4 : 8;
        float cw = (w - 8.0f) / 2.0f, pitch = (by1 - by0 - 34.0f) / (float)per;
        if (pitch > 13.0f) pitch = 13.0f;
        if (!count) text(x0 + 9.0f, by0 + 30.0f, skills ? "No skills yet" : "No techniques yet", FAINT);
        for (int i = 0; i < count; i++) {
            float cx0 = x0 + 4.0f + cw * (float)(i / per), base = by0 + 30.0f + (float)(i % per) * pitch;
            int art = ids[i], cannot;
            if (skills) {
                int left = (int)r8(rec + C_USES + (unsigned)slots[i] * 2u), full = (int)r8(rec + C_USES + (unsigned)slots[i] * 2u + 1);
                snprintf(t, sizeof t, "%d/%d", left, full);
                cannot = left == 0;
            } else {
                int cost = (int)ui_rom8(TECH_DATA + (uint32_t)(art - 1) * 8u + 1);
                snprintf(t, sizeof t, "%d TP", cost);
                cannot = cost > tp;
            }
            text(cx0 + 5.0f, base, skills ? ui_skill_name(art) : ui_tech_name(art), cannot ? FAINT : WHITE);
            text_right(cx0 + cw - 9.0f, base, t, cannot ? FAINT : SOFT);
        }
        if (skills) {
            snprintf(a, sizeof a, "%s: the skills and the uses left, page 3 of 3.", UI_CHAR_NAMES[id]);
            snprintf(b, sizeof b, "Choose again to pick another member.");
        } else {
            snprintf(a, sizeof a, "%s: the techniques and what each costs, page 2 of 3.", UI_CHAR_NAMES[id]);
            snprintf(b, sizeof b, "Choose again for the skills.");
        }
    }
    foot(x0, x1, y1, a, b);
}

/* ---- a shop: the meseta, the keeper, buy or sell, the goods (or the pack), who could use the one
 * the cursor is on and what it would do for them, the keeper's words at the foot */
static int item_price(int id) { return id > 0 ? (int)ui_rom16(ITEM_DATA + (uint32_t)(id - 1) * 0x16u + I_PRICE) : 0; }

/* what putting `item` on would do for a member: 0 if they cannot wear it */
static int would_change(int id, unsigned rec, int item, int *attack, int *defense) {
    int kind = item_kind(item);
    if (kind < 1 || kind > 7 || !(ui_rom16(ITEM_DATA + (uint32_t)(item - 1) * 0x16u + I_WHO) >> id & 1)) return 0;
    uint8_t worn[4];
    int have[N_STATS], will[N_STATS];
    for (int i = 0; i < 4; i++) worn[i] = (uint8_t)r8(rec + C_EQUIP + (unsigned)i);
    stats_with(rec, worn, have);
    int both = item_kind(worn[0]) == 3 || item_kind(worn[0]) == 4;
    if (kind <= 2) worn[0] = (uint8_t)item;
    else if (kind <= 4) { worn[0] = (uint8_t)item; worn[1] = 0; }
    else if (kind == 5) { worn[1] = (uint8_t)item; if (both) worn[0] = 0; }
    else worn[kind == 6 ? 2 : 3] = (uint8_t)item;
    stats_with(rec, worn, will);
    *attack = will[ST_ATTACK] - have[ST_ATTACK];
    *defense = will[ST_DEFENSE] - have[ST_DEFENSE];
    return 1;
}

static void draw_shop(float x0, float y0, float x1, float y1) {
    /* (the game takes the keeper's portrait down while its "who can use it" list is up in the
     * same place; here the keeper stays: their window as it last stood, the art still loaded) */
    static Win kept;
    static uint32_t kept_shop = 0xFFFFFFFFu;
    uint32_t now = routine_of(r16(WINDOW_INDEX)), shop = r16(SHOP_KIND);
    const Win *face = NULL, *words = NULL;
    for (int i = 0; i < nwins; i++) {
        if (wins[i].routine == R_GENERIC && wins[i].index == 1 && wins[i].picture == wins[i].w * wins[i].h)
            face = &wins[i];                             /* (window 1, whole: the keeper; the list covers it in the plane) */
        if (wins[i].routine == R_SHOP_MSG) words = &wins[i];
    }
    if (face) { kept = *face; kept_shop = shop; }
    else if (kept_shop == shop && nwins > 1) face = &kept;
    else kept_shop = 0xFFFFFFFFu;
    char t[64];
    float lx1 = x0 + 84.0f, rx0 = lx1 + 3.0f, w = x1 - rx0;

    /* the left: the meseta, the keeper, buy or sell */
    win(x0, y0, lx1, y0 + 19.0f);
    snprintf(t, sizeof t, "%lu", (unsigned long)ui_ram32(MONEY));
    text_right(lx1 - 30.0f, y0 + 12.6f, t, WHITE);
    text(lx1 - 27.0f, y0 + 12.6f, "mst", SOFT);
    if (face) draw_picture(face, x0 + (84.0f - (float)face->w * 8.0f) / 2.0f, y0 + 23.0f);
    int trade = open_now(R_SHOP_BUYSELL), buying = open_now(R_SHOP_BUY), selling = open_now(R_SHOP_SELL);
    float ty0 = y0 + 23.0f + 48.0f + 4.0f;
    if (trade) {
        static const char *const WHICH[2] = {"Buy", "Sell"};
        win(x0, ty0, lx1, ty0 + 10.0f + 2 * 12.5f);
        for (int i = 0; i < 2; i++) {
            float base = ty0 + 14.5f + (float)i * 12.5f;
            uint32_t c = WHITE;
            hit(x0 + 4.0f, lx1 - 4.0f, base, R_SHOP_BUYSELL, OPTION_2, i, PAD_UP, PAD_DOWN, 0, 0);
            if (i == (int)r8(OPTION_2)) {
                bar(x0 + 4.0f, lx1 - 4.0f, base, now == R_SHOP_BUYSELL ? BAR : BAR_OFF);
                if (now == R_SHOP_BUYSELL) c = NAVY;
            }
            text(x0 + 9.0f, base, WHICH[i], c);
        }
    }

    /* the keeper's words */
    win(rx0, y1 - 27.0f, x1, y1);
    if (words) {
        char line[2][128];
        int lines = 0, first;
        for (int cy = 0; cy < words->h - 2 && cy < 28 && lines < 2; cy++)
            if (tall_text(words, cy, line[lines], (int)sizeof line[0], &first) || row_text(words, cy, line[lines], (int)sizeof line[0])) lines++;
        if (lines > 0) text(rx0 + 8.0f, y1 - 16.6f, line[0], WHITE);
        if (lines > 1) text(rx0 + 8.0f, y1 - 6.4f, line[1], WHITE);
    }
    if (!trade && !buying && !selling) return;           /* an inn: the keeper, their words, yes or no */

    /* the goods, or the pack */
    int sell = selling || (!buying && r8(OPTION_2) == 1);
    uint8_t list[40];
    int count = 0, cursor = -1;
    uint32_t money = ui_ram32(MONEY);
    if (sell) {
        while (count < 40 && r8(INVENTORY + (unsigned)count)) { list[count] = (uint8_t)r8(INVENTORY + (unsigned)count); count++; }
        if (selling) cursor = (int)r16(SELL_PAGE) * 8 + (int)r8(OPTION_3);
    } else {
        uint32_t a = SHOP_GOODS;
        for (unsigned i = 0; i < r8(SHOP_ID) && i < 64; i++) {
            while (ui_rom8(a) != 0xFF && a < SHOP_GOODS + 0x400) a++;
            a++;
        }
        while (count < 16 && ui_rom8(a + (uint32_t)count) != 0xFF) { list[count] = (uint8_t)ui_rom8(a + (uint32_t)count); count++; }
        if (buying) cursor = (int)r8(OPTION_3);
    }
    if (cursor >= count) cursor = count - 1;
    int members[5], n = party(members);
    float by1 = y1 - 30.0f, ph = 9.0f + 15.0f + (float)n * 12.6f, gx1, gy1, px0, py0;
    if (w >= 280.0f) { gx1 = rx0 + w * 0.5f - 1.5f; gy1 = by1; px0 = gx1 + 3.0f; py0 = y0; }
    else { gx1 = x1; gy1 = by1 - ph - 3.0f; px0 = rx0; py0 = gy1 + 3.0f; }
    win(rx0, y0, gx1, gy1);
    int focus = now == R_SHOP_BUY || now == R_SHOP_SELL;
    float pitch = (gy1 - y0 - 10.0f) / 8.0f;
    if (pitch > 12.2f) pitch = 12.2f;
    if (!count) text(rx0 + 9.0f, y0 + 14.2f, sell ? "The pack is empty" : "Nothing for sale", FAINT);
    if (sell) {                                          /* the game's pages of eight, side by side */
        int pages = (count + 7) / 8, cols = (int)((gx1 - rx0 - 8.0f) / 70.0f), first = 0, page = cursor >= 0 ? cursor / 8 : 0;
        if (cols < 1) cols = 1;
        if (cols > pages) cols = pages > 0 ? pages : 1;
        if (page >= cols) first = page - cols + 1;
        float cw = (gx1 - rx0 - 8.0f) / (float)cols;
        for (int i = first * 8; i < count && i < (first + cols) * 8; i++) {
            float cx0 = rx0 + 4.0f + cw * (float)(i / 8 - first), base = y0 + 14.2f + (float)(i % 8) * pitch;
            uint32_t c = item_price(list[i]) ? (selling ? WHITE : SOFT) : FAINT;
            if (selling) hit(cx0, cx0 + cw, base, R_SHOP_SELL, OPTION_3, i % 8, PAD_UP, PAD_DOWN, SELL_PAGE, i / 8);
            if (i == cursor) {
                bar(cx0, cx0 + cw, base, focus ? BAR : BAR_OFF);
                c = focus ? NAVY : WHITE;
            }
            text(cx0 + 5.0f, base, ui_item_name(list[i]), c);
        }
        if (first > 0) ui_triangle(&cv, (rx0 + 4.4f) * S, (y0 + 6.0f) * S, 2.8f * S, 3, WHITE, 255);
        if (first + cols < pages) ui_triangle(&cv, (gx1 - 4.4f) * S, (y0 + 6.0f) * S, 2.8f * S, 2, WHITE, 255);
    } else {
        int rows = (int)((gy1 - y0 - 10.0f) / pitch), first = 0;
        if (cursor >= rows) first = cursor - rows + 1;
        for (int i = first; i < count && i < first + rows; i++) {
            float base = y0 + 14.2f + (float)(i - first) * pitch;
            int price = item_price(list[i]), dear = (uint32_t)price > money;
            uint32_t c = dear ? FAINT : buying ? WHITE : SOFT, pc = dear ? FAINT : SOFT;
            if (buying) hit(rx0 + 4.0f, gx1 - 4.0f, base, R_SHOP_BUY, OPTION_3, i, PAD_UP, PAD_DOWN, 0, 0);
            if (i == cursor) {
                bar(rx0 + 4.0f, gx1 - 4.0f, base, focus ? BAR : BAR_OFF);
                if (focus) c = pc = NAVY;
                else c = WHITE;
            }
            text(rx0 + 9.0f, base, ui_item_name(list[i]), c);
            snprintf(t, sizeof t, "%d", price);
            text_right(gx1 - 9.0f, base, t, pc);
        }
    }

    /* the party: who could use it, and what it would do for them */
    int item = cursor >= 0 ? list[cursor] : 0, kind = item_kind(item), worn = kind >= 1 && kind <= 7;
    win(px0, py0, x1, py0 + ph);
    if (item) {
        int owned = 0;
        for (int i = 0; i < 40; i++) owned += (int)r8(INVENTORY + (unsigned)i) == item;
        if (sell) snprintf(t, sizeof t, item_price(item) ? "Sells for %d mst" : "Cannot be sold", item_price(item) / 2);
        else snprintf(t, sizeof t, "In the pack: %d", owned);
        text(px0 + 9.0f, py0 + 13.5f, t, SOFT);
    } else {
        text(px0 + 9.0f, py0 + 13.5f, "Party", FAINT);
    }
    for (int i = 0; i < n; i++) {
        unsigned rec = CHAR_STATS + (unsigned)members[i] * 0x80u;
        float base = py0 + 28.5f + (float)i * 12.6f;
        int da = 0, dd = 0, can = worn && would_change(members[i], rec, item, &da, &dd);
        text(px0 + 9.0f, base, UI_CHAR_NAMES[members[i]], !item ? FAINT : worn && !can ? FAINT : WHITE);
        if (!item) continue;
        if (!worn) {
            snprintf(t, sizeof t, "HP %u/%u", r16(rec + C_HP), r16(rec + C_HP + 2));
            text_right(x1 - 9.0f, base, t, SOFT);
        } else if (!can) {
            text_right(x1 - 9.0f, base, "cannot use", FAINT);
        } else {
            int weapon = kind <= 4, d = weapon ? da : dd;
            int wearing = 0;
            for (int k = 0; k < 4; k++) wearing |= (int)r8(rec + C_EQUIP + (unsigned)k) == item;
            if (wearing && !d) snprintf(t, sizeof t, "wearing it");
            else snprintf(t, sizeof t, "%s %+d", weapon ? "Attack" : "Defense", d);
            text_right(x1 - 9.0f, base, t, d > 0 ? UP : d < 0 ? DOWN : SOFT);
        }
    }
}

/* ---- macros: the eight across the top, the turns of the one chosen, who acts next, what they do */
static void macro_turn(unsigned at, int brief, char *out, size_t n) {
    int what = (int)r8(at + 1), id = (int)r8(at + 2);
    if (what == 2) snprintf(out, n, "%s: %s", brief ? "Tech" : "Technique", ui_tech_name(id));
    else if (what == 3) snprintf(out, n, "Skill: %s", ui_skill_name(id));
    else if (what == 4) snprintf(out, n, "Item: %s", ui_item_name(id));
    else snprintf(out, n, "%s", what == 1 ? "Attack" : what == 5 ? "Defend" : "");
}

static void draw_macros(float x0, float y0, float x1, float y1) {
    static const char *const NTH[5] = {"first", "second", "third", "fourth", "fifth"};
    uint32_t now = routine_of(r16(WINDOW_INDEX));
    int letter = (int)r8(OPTION_2) & 7;
    int who_open = open_now(R_MACRO_WHO), do_open = open_now(R_MACRO_DO);
    int setting = who_open || do_open || open_now(R_MACRO_SET) || open_now(R_MACRO_SET_Q);
    int chosen = (int)r16(MACRO_CHOSEN), turns = (int)r16(MACRO_PARTY) + 1;
    if (chosen > 5) chosen = 5;
    if (turns > 5) turns = 5;
    float w = x1 - x0;
    int narrow = w < 260.0f;

    /* the eight macros across the top: one that is set stands out */
    win(x0, y0, x1, y0 + 19.0f);
    for (int i = 0; i < 8; i++) {
        float cx0 = x0 + 4.0f + (w - 8.0f) * (float)i / 8.0f, cx1 = x0 + 4.0f + (w - 8.0f) * (float)(i + 1) / 8.0f;
        char name[2] = {(char)('A' + i), 0};
        uint32_t c = r8(MACRO_DATA + (unsigned)i * 20u + 1) ? WHITE : FAINT;
        hit(cx0, cx1, y0 + 12.8f, R_MACRO_LETTERS, OPTION_2, i, PAD_UP, PAD_DOWN, 0, 0);
        if (i == letter) {
            bar(cx0, cx1, y0 + 12.8f, now == R_MACRO_LETTERS ? BAR : BAR_OFF);
            c = now == R_MACRO_LETTERS ? NAVY : WHITE;
        }
        text_mid((cx0 + cx1) / 2.0f, y0 + 12.8f, name, c);
    }

    /* its turns, in the order they are played; while it is set up, the ones set so far and the one in hand */
    float by0 = y0 + 22.0f, by1 = y1 - 52.0f, px1 = x0 + w * 0.62f;
    unsigned src = setting ? MACRO_WORK : MACRO_DATA + (unsigned)letter * 20u;
    unsigned cannot = setting ? 0 : r8(MACRO_FLAGS + (unsigned)letter);
    int n = 0;
    while (n < 5 && r8(src + (unsigned)n * 4u + 1)) n++;
    if (who_open && n > chosen) n = chosen;
    char t[64];
    win(x0, by0, px1, by1);
    snprintf(t, sizeof t, "Macro %c", 'A' + letter);
    text(x0 + 9.0f, by0 + 13.5f, t, SOFT);
    if (!n && !setting) text(x0 + 9.0f, by0 + 30.0f, "Not set", FAINT);
    for (int i = 0; i < n || (i == n && who_open && i < 5); i++) {
        float base = by0 + 30.0f + (float)i * 14.5f;
        unsigned at = src + (unsigned)i * 4u;
        int in_hand = i == n;
        uint32_t c = cannot >> i & 1 ? FAINT : WHITE;
        snprintf(t, sizeof t, "%d", i + 1);
        text(x0 + 9.0f, base, t, SOFT);
        if (in_hand) {                               /* the turn being given: whoever the cursor is on, what it is on */
            int id = (int)r8(MACRO_LEFT + (unsigned)chosen * 5u + (r8(OPTION_3) % 5u));
            static const char *const DO[5] = {"Attack", "Technique", "Skill", "Item", "Defend"};
            if (id < 11) text(x0 + 22.0f, base, UI_CHAR_NAMES[id], SOFT);
            if (do_open) text(x0 + 56.0f, base, DO[r8(OPTION_4) % 5u], SOFT);
        } else {
            int id = (int)r8(at);
            if (id < 11) text(x0 + 22.0f, base, UI_CHAR_NAMES[id], c);
            macro_turn(at, narrow, t, sizeof t);
            text(x0 + 56.0f, base, t, c);
        }
    }

    /* who acts next: those not yet given a turn */
    win(px1 + 3.0f, by0, x1, by1);
    if (who_open && chosen < 5) {
        int left = turns - chosen, cursor = (int)r8(OPTION_3);
        snprintf(t, sizeof t, "Acts %s", NTH[chosen]);
        text(px1 + 12.0f, by0 + 13.5f, t, SOFT);
        for (int i = 0; i < left && i < 5; i++) {
            float base = by0 + 30.0f + (float)i * 14.5f;
            int id = (int)r8(MACRO_LEFT + (unsigned)chosen * 5u + (unsigned)i);
            uint32_t c = WHITE;
            hit(px1 + 7.0f, x1 - 4.0f, base, R_MACRO_WHO, OPTION_3, i, PAD_UP, PAD_DOWN, 0, 0);
            if (i == cursor) {
                bar(px1 + 7.0f, x1 - 4.0f, base, now == R_MACRO_WHO ? BAR : BAR_OFF);
                if (now == R_MACRO_WHO) c = NAVY;
            }
            if (id < 11) text(px1 + 12.0f, base, UI_CHAR_NAMES[id], c);
        }
    } else {
        text(px1 + 12.0f, by0 + 13.5f, "Party", FAINT);
        int members[5], m = party(members);
        for (int i = 0; i < m; i++) text(px1 + 12.0f, by0 + 30.0f + (float)i * 14.5f, UI_CHAR_NAMES[members[i]], FAINT);
    }

    /* what they do: the game's five icons, as words, across */
    static const char *const WORDS[2][5] = {{"Attack", "Technique", "Skill", "Item", "Defend"},
                                            {"Attack", "Tech", "Skill", "Item", "Defend"}};
    win(x0, y1 - 49.0f, x1, y1 - 30.0f);
    for (int i = 0; i < 5; i++) {
        float cx0 = x0 + 4.0f + (w - 8.0f) * (float)i / 5.0f, cx1 = x0 + 4.0f + (w - 8.0f) * (float)(i + 1) / 5.0f;
        uint32_t c = do_open ? WHITE : FAINT;
        if (do_open) hit(cx0, cx1, y1 - 36.2f, R_MACRO_DO, OPTION_4, i, PAD_LEFT, PAD_RIGHT, 0, 0);
        if (do_open && i == (int)r8(OPTION_4)) {
            bar(cx0, cx1, y1 - 36.2f, now == R_MACRO_DO ? BAR : BAR_OFF);
            if (now == R_MACRO_DO) c = NAVY;
        }
        text_mid((cx0 + cx1) / 2.0f, y1 - 36.2f, WORDS[narrow][i], c);
    }

    char a[160] = "", b[120] = "";
    if (now == R_MACRO_LETTERS) {
        if (n) snprintf(a, sizeof a, "Macro %c: %d turn%s. Choose it to set it up anew.", 'A' + letter, n, n == 1 ? "" : "s");
        else snprintf(a, sizeof a, "Macro %c is not set. Choose it to set it up.", 'A' + letter);
        snprintf(b, sizeof b, "In battle a macro plays its turns in this order.");
    } else if (now == R_MACRO_WHO && chosen < 5) {
        snprintf(a, sizeof a, "Who acts %s?", NTH[chosen]);
    } else if (now == R_MACRO_DO) {
        int id = (int)r8(MACRO_LEFT + (unsigned)chosen * 5u + (r8(OPTION_3) % 5u));
        snprintf(a, sizeof a, "What does %s do?", id < 11 ? UI_CHAR_NAMES[id] : "this one");
    } else if (now == R_MACRO_TECHS) snprintf(a, sizeof a, "Which technique?");
    else if (now == R_MACRO_SKILLS) snprintf(a, sizeof a, "Which skill?");
    else if (now == R_MACRO_ITEMS) snprintf(a, sizeof a, "Which item?");
    foot(x0, x1, y1, a, b);
}

/* ---- equipment */
enum { F_MEMBER, F_LIST, F_SLOTS, F_HAND, F_NOTHING, F_FULL };

static int equip_focus(void) {
    uint32_t now = routine_of(r16(WINDOW_INDEX));
    if (now == R_MEMBERS) return F_MEMBER;
    if (now == R_NOTHING) return F_NOTHING;
    if (now == R_FULL) return F_FULL;
    if (now == R_SLOTS && (r8(EQUIP_FLAGS) & 1)) return F_SLOTS;
    if (now == R_LIST && (r16(RED_CURSOR) & 0x7FFC) == 0x34) {
        /* asked which hand, the game puts its cursor back on the slots ($A0 across) */
        int x = (int)r16(RED_CURSOR + 0x2C) + (int)(r16(CAMERA_X) & 7);
        if (x >= 0x9C && x <= 0xA4) return F_HAND;
    }
    return F_LIST;
}

static void draw_equip(float x0, float y0, float x1, float y1) {
    int focus = equip_focus();
    int members[5], n = party(members), sel = (int)r8(OPTION_2);
    if (!n) return;
    if (sel >= n) sel = n - 1;
    int id = members[sel];
    unsigned rec = CHAR_STATS + (unsigned)id * 0x80u;
    float w = x1 - x0;

    /* the party across the top */
    win(x0, y0, x1, y0 + 19.0f);
    for (int i = 0; i < n; i++) {
        float cx0 = x0 + 4.0f + (w - 8.0f) * (float)i / (float)n, cx1 = x0 + 4.0f + (w - 8.0f) * (float)(i + 1) / (float)n;
        uint32_t c = WHITE;
        hit(cx0, cx1, y0 + 12.8f, R_MEMBERS, OPTION_2, i, PAD_UP, PAD_DOWN, 0, 0);
        if (i == sel) {
            bar(cx0, cx1, y0 + 12.8f, focus == F_MEMBER ? BAR : BAR_OFF);
            if (focus == F_MEMBER) c = NAVY;
        }
        text_mid((cx0 + cx1) / 2.0f, y0 + 12.8f, UI_CHAR_NAMES[members[i]], c);
    }

    float by0 = y0 + 22.0f, by1 = y1 - 30.0f, lw = w * 0.5f - 1.5f;
    float lx1 = x0 + lw, rx0 = lx1 + 3.0f;

    /* what they wear -- the game's order: head, right hand, left hand, body -- and every stat */
    static const char *const SLOT[4] = {"Head", "R hand", "L hand", "Body"};
    static const int SLOT_AT[4] = {2, 0, 1, 3};          /* into the record's equipment */
    uint8_t worn[4];
    for (int i = 0; i < 4; i++) worn[i] = (uint8_t)r8(rec + C_EQUIP + (unsigned)i);
    int both = item_kind(worn[0]) == 3 || item_kind(worn[0]) == 4;
    int slot_cursor = focus == F_SLOTS ? (int)r8(OPTION_4) & 3 : focus == F_HAND ? 1 + ((int)r8(OPTION_4) & 1) : -1;
    win(x0, by0, lx1, by1);
    for (int i = 0; i < 4; i++) {
        float base = by0 + 14.0f + (float)i * 12.5f;
        int item = worn[SLOT_AT[i]];
        uint32_t c = WHITE, lc = SOFT;
        if (focus == F_SLOTS) hit(x0 + 4.0f, lx1 - 4.0f, base, R_SLOTS, OPTION_4, i, PAD_UP, PAD_DOWN, 0, 0);
        else if (focus == F_HAND && (i == 1 || i == 2)) hit(x0 + 4.0f, lx1 - 4.0f, base, R_LIST, OPTION_4, i - 1, PAD_UP, PAD_DOWN, 0, 0);
        if (i == slot_cursor) {
            bar(x0 + 4.0f, lx1 - 4.0f, base, BAR);
            c = lc = NAVY;
        }
        text(x0 + 8.0f, base, SLOT[i], lc);
        if (i == 2 && both) text_right(lx1 - 8.0f, base, "(both hands)", i == slot_cursor ? NAVY : FAINT);
        else if (item) text_right(lx1 - 8.0f, base, ui_item_name(item), c);
        else text_right(lx1 - 8.0f, base, "-", i == slot_cursor ? NAVY : FAINT);
    }
    float line = by0 + 14.0f + 4 * 12.5f - 5.0f;
    ui_round_rect(&cv, (x0 + 6.0f) * S, line * S, (lx1 - 6.0f) * S, (line + 0.8f) * S, 0, SOFT, 150);
    int have[N_STATS], will[N_STATS];
    stats_with(rec, worn, have);
    stats_shown(rec, will);
    int trying = focus == F_LIST || focus == F_SLOTS || focus == F_HAND;
    for (int k = 0; k < N_STATS; k++) {
        float base = line + 12.0f + (float)k * 11.6f;
        char t[16];
        text(x0 + 8.0f, base, STAT_NAMES[k], SOFT);
        snprintf(t, sizeof t, "%d", have[k]);
        text_right(x0 + lw * 0.68f, base, t, WHITE);
        if (trying && will[k] != have[k]) {
            uint32_t c = will[k] > have[k] ? UP : DOWN;
            ui_triangle(&cv, (x0 + lw * 0.68f + 7.0f) * S, (base - 2.8f) * S, 3.6f * S, 2, c, 255);
            snprintf(t, sizeof t, "%d", will[k]);
            text_right(lx1 - 8.0f, base, t, c);
        }
    }

    /* what they could put on: the game's list once it has made it, before that the same from the pack */
    uint8_t list[40];
    int count = 0, cursor = -1;
    if (focus == F_MEMBER) {
        for (int i = 0; i < 40; i++) {
            int item = (int)r8(INVENTORY + (unsigned)i), kind = item_kind(item);
            if (item && kind >= 1 && kind <= 7 && (ui_rom16(ITEM_DATA + (uint32_t)(item - 1) * 0x16u + I_WHO) >> id & 1))
                list[count++] = (uint8_t)item;
        }
    } else {
        count = (int)r16(EQUIP_COUNT);
        if (count > 40) count = 40;
        for (int i = 0; i < count; i++) list[i] = (uint8_t)r8(EQUIP_LIST + (unsigned)i);
        if (focus == F_LIST || focus == F_HAND) cursor = (int)r16(EQUIP_PAGE) * 8 + (int)r8(OPTION_3);
        if (cursor >= count) cursor = count - 1;
    }
    win(rx0, by0, x1, by1);
    int rows = (int)((by1 - by0 - 10.0f) / 12.0f), first = 0;
    if (cursor >= rows) first = cursor - rows + 1;
    if (!count) text_mid((rx0 + x1) / 2.0f, by0 + 16.0f, "Nothing to put on", FAINT);
    for (int i = first; i < count && i < first + rows; i++) {
        float base = by0 + 14.0f + (float)(i - first) * 12.0f;
        int item = list[i], kind = item_kind(item), weapon = kind >= 1 && kind <= 4;
        uint32_t c = focus == F_MEMBER ? SOFT : WHITE, sc = SOFT;
        char t[24];
        if (focus == F_LIST) hit(rx0 + 4.0f, x1 - 4.0f, base, R_LIST, OPTION_3, i % 8, PAD_UP, PAD_DOWN, EQUIP_PAGE, i / 8);
        if (i == cursor) {
            bar(rx0 + 4.0f, x1 - 4.0f, base, focus == F_LIST ? BAR : BAR_OFF);
            if (focus == F_LIST) c = sc = NAVY;
        }
        text(rx0 + 8.0f, base, ui_item_name(item), c);
        snprintf(t, sizeof t, "%s %d", weapon ? "Atk" : "Def", item_bonus(item, weapon ? 0x0F : 0x10));
        text_right(x1 - 8.0f, base, t, sc);
    }
    if (first > 0) ui_triangle(&cv, (x1 - 7.0f) * S, (by0 + 4.5f) * S, 2.6f * S, 1, WHITE, 255);
    if (first + rows < count) ui_triangle(&cv, (x1 - 7.0f) * S, (by1 - 5.5f) * S, 2.6f * S, 0, WHITE, 255);

    /* a line or two on where the cursor is */
    char a[200] = "", b[120] = "";
    int about = 0;
    if (focus == F_MEMBER) snprintf(a, sizeof a, "Whose equipment?");
    else if (focus == F_NOTHING) {
        snprintf(a, sizeof a, "%s has nothing else to put on.", UI_CHAR_NAMES[id]);
        snprintf(b, sizeof b, "Left: take something off.");
    } else if (focus == F_FULL) snprintf(a, sizeof a, "The pack is full: nothing can be taken off.");
    else if (focus == F_HAND) {
        snprintf(a, sizeof a, "Which hand?");
        about = cursor >= 0 ? list[cursor] : 0;
        if (about) item_line(about, b, sizeof b);
    } else if (focus == F_SLOTS) {
        about = worn[SLOT_AT[slot_cursor & 3]];
        if (slot_cursor == 2 && both) about = worn[0];
        if (about) item_line(about, a, sizeof a);
        else snprintf(a, sizeof a, "Nothing here.");
        snprintf(b, sizeof b, about ? "Choose it to take it off. Right: the list." : "Right: the list.");
    } else if (cursor >= 0) {
        about = list[cursor];
        item_line(about, a, sizeof a);
        item_wearers(about, b, sizeof b);
    }
    foot(x0, x1, y1, a, b);
}

void fieldmenu_draw(uint32_t *out, int stride, int scale, int w, int h) {
    if (!active || !out || scale < 1) return;
    int ow = w * scale, oh = h * scale;
    /* the field behind: there, but out of the way -- and still (see `still`) */
    if (still_ok && still && (still_w != ow || still_h != oh)) {     /* the window's size changed: the same picture, resized */
        uint32_t *resized = malloc((size_t)ow * (size_t)oh * 4);
        if (resized) {
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow; x++)
                    resized[(size_t)y * (size_t)ow + (size_t)x] =
                        still[(size_t)(y * still_h / oh) * (size_t)still_w + (size_t)(x * still_w / ow)];
            free(still);
            still = resized;
            still_w = ow;
            still_h = oh;
        } else {
            still_ok = 0;
        }
    }
    if (still_ok && still) {
        for (int y = 0; y < oh; y++)
            memcpy(out + (size_t)y * (size_t)stride, still + (size_t)y * (size_t)ow, (size_t)ow * 4);
    } else {
        for (int y = 0; y < oh; y++) {
            uint32_t *row = out + (size_t)y * (size_t)stride;
            for (int x = 0; x < ow; x++) {
                uint32_t p = row[x];
                if (screen == SCR_CHEST) break;              /* something found: the field as it is, held */
                uint32_t r = (p >> 16 & 0xFF) * 70u >> 8, g = (p >> 8 & 0xFF) * 76u >> 8, b = ((p & 0xFF) * 100u >> 8) + 14u;
                row[x] = 0xFF000000u | r << 16 | g << 8 | b;
            }
        }
        if (still_w != ow || still_h != oh || !still) {
            free(still);
            still = malloc((size_t)ow * (size_t)oh * 4);
            still_w = ow;
            still_h = oh;
        }
        if (still) {
            for (int y = 0; y < oh; y++)
                memcpy(still + (size_t)y * (size_t)ow, out + (size_t)y * (size_t)stride, (size_t)ow * 4);
            still_ok = 1;
        }
    }
    cv = (UiCanvas){out, stride, 0, 0, ow, oh};
    S = (float)scale;
    nhits = 0;
    party_routine = strip_routine = 0;
    float m = 5.0f, cmd = 84.0f, W = (float)w, H = (float)h, rx0 = m + cmd + 3.0f;
    if (screen == SCR_SHOP) {
        draw_shop(m, m, W - m, H - m);
    } else if (screen != SCR_PLAIN && screen != SCR_CHEST) {
        draw_commands(m, m, m + cmd, (int)r8(OPTION_1), screen == SCR_MAIN && routine_of(r16(WINDOW_INDEX)) == R_MENU);
        draw_meseta(m, H - m, m + cmd);
        if (screen == SCR_EQUIP) draw_equip(rx0, m, W - m, H - m);
        else if (screen == SCR_ITEMS) draw_items(rx0, m, W - m, H - m);
        else if (screen == SCR_MACROS) draw_macros(rx0, m, W - m, H - m);
        else if (screen == SCR_TECHS) draw_arts(rx0, m, W - m, H - m, 0);
        else if (screen == SCR_SKILLS) draw_arts(rx0, m, W - m, H - m, 1);
        else if (screen == SCR_STATUS) draw_status(rx0, m, W - m, H - m);
        else draw_party(rx0, m, W - m, 16.0f, 1, -1, 0);
    }
    /* The windows not laid out anew, with the game's own words. Among only
     * such windows: where the game has them (its 320 across, in the middle).
     * Over a screen laid out anew: a message of a line or two takes the foot's
     * place; a question or a list comes up in the middle of the screen's part. */
    int ccol = 0, crow = 0, has = cursor_cell(&ccol, &crow), top = -1;
    for (int i = 0; i < nwins && has; i++)
        if (ccol >= wins[i].x && ccol < wins[i].x + wins[i].w && crow > wins[i].y && crow < wins[i].y + wins[i].h - 1) top = i;
    /* (the windows that come up in the middle keep their places to one another: Order's two) */
    int bx0 = 99, by0c = 99, bx1 = 0, by1c = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nwins; i++) {
            const Win *p = &wins[i];
            if (!p->plain) continue;
            if (screen == SCR_PLAIN || screen == SCR_CHEST) {
                if (pass) draw_plain(p, (W - 320.0f) / 2.0f, 0.0f, i == top, ccol, crow);
                continue;
            }
            char line[3][128];
            int lines = 0;
            for (int cy = 0; cy < p->h - 2 && cy < 28 && lines < 3; cy++)
                if (row_text(p, cy, line[lines], (int)sizeof line[0])) lines++;
            if (!p->boxes && lines <= 2 && p->w >= 20) {     /* a message */
                if (!pass) continue;
                win(rx0, H - m - 27.0f, W - m, H - m);
                if (lines > 0) text(rx0 + 8.0f, H - m - 16.6f, line[0], WHITE);
                if (lines > 1) text(rx0 + 8.0f, H - m - 6.4f, line[1], WHITE);
            } else if (!pass) {
                if (p->x < bx0) bx0 = p->x;
                if (p->y < by0c) by0c = p->y;
                if (p->x + p->w > bx1) bx1 = p->x + p->w;
                if (p->y + p->h > by1c) by1c = p->y + p->h;
            } else {
                /* (Order's list as it closes -- no one left to choose, the cursor gone -- holds what
                 * was under it for a moment: it keeps its place, empty) */
                if (p->routine == R_ORDER && i != top) continue;
                float py = (H - (float)(by1c - by0c) * 8.0f) / 2.0f;
                if (screen == SCR_STATUS && !open_now(R_STATUS_WHO)) {   /* (Order: under the party's rows) */
                    int members[5];
                    py = m + 9.0f + (float)party(members) * 16.0f + 1.5f;
                }
                draw_plain(p, rx0 + (W - m - rx0 - (float)(bx1 - bx0) * 8.0f) / 2.0f - (float)bx0 * 8.0f,
                           py - (float)by0c * 8.0f, i == top, ccol, crow);
            }
        }
}
