/*
 * Sprites from a mod: see spritemod.h.
 */
#include "spritemod.h"
#include "m68k.h"
#include "vdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE    0xEF00
#define MODE_FIELD   0x0C
#define MODE_ENTER   0x08              /* a place being entered: its map loaded, its picture fading in */
#define OBJECTS      0xC000            /* the field's objects, $40 each; the first is the party's leader */
#define OBJ_SIZE     0x40
#define OBJ_COUNT    64
#define O_ID         0x00
#define O_FACING     0x06              /* 0 down, 4 up, 8 right, $C left */
#define O_SCRIPTS    0x08              /* a table of four animations, one a facing */
#define O_FRAME      0x10              /* the animation's frame showing */
#define O_HOLD_LEFT  0x11              /* ticks of it still to come */
#define O_ART_TILE   0x16
#define O_ART_ROM    0x18              /* where its art is streamed from in the ROM; 0: it is all in video memory */
#define O_X          0x30
#define O_Y          0x34
#define MAX_FILES    64

static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }
static uint32_t r32(unsigned a) { return (uint32_t)r16(a) << 16 | r16(a + 2); }
static unsigned rom8(uint32_t a) { return a < 0x400000 ? m68k_rom[a] : 0; }
static uint32_t rom32(uint32_t a) {
    return (uint32_t)rom8(a) << 24 | (uint32_t)rom8(a + 1) << 16 | (uint32_t)rom8(a + 2) << 8 | rom8(a + 3);
}

/* A file: "PSIVSPR2\n" "W H FRAMES MULT TARGET\n", then for each facing (down, up, right, left), each of
 * the game's FRAMES and each of its MULT in-between pictures, W x H bytes: 0 nothing, 1-15 a colour of the
 * sprite's own palette line, 255 the game's own pixel there (the sprite editor writes that for every pixel
 * left as the game has it, so a file holds only what was changed; it wants a picture the size of the
 * game's piece). TARGET is whose it is, a key as PSIV_SPRITEMOD_LOG lists them for whoever is
 * on screen: R and the address its art is streamed from in the ROM (the party: one address a member),
 * or A and a hash of the first tiles of its art in video memory (the townspeople: their table of
 * animations is one for all of them, so their art is what tells them apart). */
typedef struct { int w, h, frames, mult, res; char kind; uint32_t target; uint8_t *px, *fine; } Art;
/* (px: the pictures at the game's size, for its sprite drawing; fine: as the file has them, res pixels to
 * each of the game's, for spritemod_draw; NULL where res is 1) */
static Art arts[MAX_FILES];
static int narts;
static VdpSpriteArt slots[80];
static struct { const uint8_t *px; int res; } fine_slots[80];       /* the finer picture of a piece's, if it has one */
static int fine_any;
static struct { int x, y, still; } was[OBJ_COUNT];
static int logging = -1;

static int load(Art *a, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "spritemod: cannot open %s\n", path); return 0; }
    char magic[16] = {0};
    char key[16] = {0};
    int res = 1;
    memset(a, 0, sizeof *a);
    /* "PSIVSPR3" has one number more, RES: the pictures have RES pixels to each of the game's, along a side
     * (1, 2 or 4), so each is (W RES) x (H RES). "PSIVSPR2" is RES 1. */
    int ok = fgets(magic, sizeof magic, f) != NULL;
    if (ok && !strncmp(magic, "PSIVSPR3", 8))
        ok = fscanf(f, "%d %d %d %d %d %15s", &a->w, &a->h, &a->frames, &a->mult, &res, key) == 6;
    else if (ok && !strncmp(magic, "PSIVSPR2", 8))
        ok = fscanf(f, "%d %d %d %d %15s", &a->w, &a->h, &a->frames, &a->mult, key) == 5;
    else ok = 0;
    ok = ok && fgetc(f) == '\n' && (key[0] == 'R' || key[0] == 'A')
          && a->w > 0 && a->w <= 64 && a->h > 0 && a->h <= 64 && a->frames > 0 && a->frames <= 8
          && a->mult > 0 && a->mult <= 8 && (res == 1 || res == 2 || res == 4);
    if (ok) {
        size_t pics = 4u * (size_t)a->frames * (size_t)a->mult, small = (size_t)a->w * (size_t)a->h;
        size_t big = small * (size_t)res * (size_t)res;
        uint8_t *fine = malloc(pics * big);
        ok = fine && fread(fine, 1, pics * big, f) == pics * big;
        if (ok && res > 1) {
            /* Finer pictures: the game's sprite drawing carries them at its own size, a block of RES x RES
             * as one pixel -- the colour most of what is drawn in it has; with nothing drawn, the game's own
             * if any of it is the game's own; nothing only if all of it is nothing. So the game's pixel is
             * the sprite's wherever the fine picture has anything, and spritemod_draw puts the detail back. */
            a->px = malloc(pics * small);
            ok = a->px != NULL;
            for (size_t p = 0; ok && p < pics; p++)
                for (int y = 0; y < a->h; y++)
                    for (int x = 0; x < a->w; x++) {
                        int count[16] = {0}, best = 0, most = 0, own = 0;
                        for (int j = 0; j < res; j++)
                            for (int i = 0; i < res; i++) {
                                int v = fine[p * big + (size_t)((y * res + j) * a->w * res + x * res + i)];
                                if (v == 0xFF) own = 1;
                                else if ((v & 15) && ++count[v & 15] > most) { most = count[v & 15]; best = v & 15; }
                            }
                        a->px[p * small + (size_t)(y * a->w + x)] = (uint8_t)(best ? best : own ? 0xFF : 0);
                    }
            a->fine = fine;
            a->res = res;
        } else { a->px = fine; a->res = 1; }
    }
    fclose(f);
    if (!ok) {
        fprintf(stderr, "spritemod: %s is not a sprite file of this port\n", path);
        free(a->px);
        free(a->fine);
        a->px = a->fine = NULL;
        return 0;
    }
    a->kind = key[0];
    a->target = (uint32_t)strtoul(key + 1, NULL, 16);
    fprintf(stderr, "spritemod: %s: for %c%X, %d x %d, %d frames a facing, each drawn %d times over\n", path,
            a->kind, (unsigned)a->target, a->w, a->h, a->frames, a->mult);
    return 1;
}

int spritemod_set_file(const char *paths) {
    for (int i = 0; i < narts; i++) { free(arts[i].px); free(arts[i].fine); }
    narts = 0;
    vdp_sprite_art = NULL;
    memset(was, 0, sizeof was);
    if (!paths || !*paths) return 0;
    static char buf[8192];
    snprintf(buf, sizeof buf, "%s", paths);
    for (char *p = buf; p && *p && narts < MAX_FILES;) {       /* several files: a ; between them */
        char *end = strchr(p, ';');
        if (end) *end = 0;
        if (*p && load(&arts[narts], p)) narts++;
        p = end ? end + 1 : NULL;
    }
    return narts;
}

void spritemod_frame(void) {
    vdp_sprite_art = NULL;
    fine_any = 0;
    if (logging < 0) logging = getenv("PSIV_SPRITEMOD_LOG") != NULL;
    static int said = -1;
    static long tick;
    tick++;
    /* On the field, and as a place fades in: the game already shows its people then, and for those frames
     * they were the game's own (PsiLok: "the sprites start off with the og for a few frames then switch") */
    int mode = (int)r16(GAME_MODE);
    if (!narts || (mode != MODE_FIELD && mode != MODE_ENTER)) {
        if (logging && narts && said != mode) {
            said = mode;
            fprintf(stderr, "spritemod: tick %ld: not the field (game mode %X, routine %X): the game's own\n", tick, (unsigned)mode, r16(0xEC20));
        }
        return;
    }
    if (logging && said != mode) {
        if (mode == MODE_FIELD) {                        /* (the sprites' colours here, faded in: the party's line and the townspeople's) */
            fprintf(stderr, "spritemod: palette lines 2 and 3:");
            for (int i = 32; i < 64; i++) fprintf(stderr, "%s%04X", i == 48 ? " | " : " ", vdp.cram[i]);
            fputc('\n', stderr);
        }
        said = mode;
        fprintf(stderr, "spritemod: tick %ld: %s (routine %X)\n", tick, mode == MODE_FIELD ? "the field" : "a place fading in", r16(0xEC20));
    }

    /* the sprite table as the game left it */
    struct { int x, y, w, h, tile, pal, owner, far; } sp[80];
    int nsp = 0, entry[80];
    uint32_t sat = (uint32_t)(vdp.reg[5] & 0x7E) << 9;
    for (int link = 0, count = 0; count < 80; count++) {
        const uint8_t *s = &vdp.vram[(sat + (uint32_t)link * 8u) & 0xFFFF];
        entry[nsp] = link;
        sp[nsp].y = ((s[0] << 8 | s[1]) & 0x3FF) - 128;
        sp[nsp].x = ((s[6] << 8 | s[7]) & 0x1FF) - 128;
        sp[nsp].h = ((s[2] & 3) + 1) * 8;
        sp[nsp].w = ((s[2] >> 2 & 3) + 1) * 8;
        sp[nsp].tile = (s[4] << 8 | s[5]) & 0x7FF;
        sp[nsp].pal = s[4] >> 5 & 3;
        sp[nsp].owner = -1;
        sp[nsp].far = 1 << 30;
        nsp++;
        link = s[3] & 0x7F;
        if (!link) break;
    }

    /* the objects with art: their tiles (from each one's art tile up to the next one's) */
    int tile0[OBJ_COUNT], tile1[OBJ_COUNT];
    for (int i = 0; i < OBJ_COUNT; i++) {
        unsigned o = OBJECTS + (unsigned)i * OBJ_SIZE;
        tile0[i] = r16(o + O_ID) ? (int)(r16(o + O_ART_TILE) & 0x7FF) : -1;
    }
    for (int i = 0; i < OBJ_COUNT; i++) {
        tile1[i] = tile0[i] + 0x40;
        for (int k = 0; k < OBJ_COUNT; k++)
            if (tile0[k] > tile0[i] && tile0[k] < tile1[i]) tile1[i] = tile0[k];
    }
    /* Where an object's pieces stand on the screen: the leader's own piece gives the step from the map's
     * coordinates to the screen's (it has tiles of its own). Without it (a scene with no leader) nothing
     * is replaced. */
    int lx = 9999, ly = 9999;
    for (int k = 0; k < nsp; k++)
        if (tile0[0] >= 0 && sp[k].tile >= tile0[0] && sp[k].tile < tile1[0] && sp[k].y > -64 && sp[k].y < 240) {
            if (sp[k].x < lx) lx = sp[k].x;
            if (sp[k].y < ly) ly = sp[k].y;
        }
    static int lost = -1;
    if (logging && lost != (lx == 9999)) {
        lost = lx == 9999;
        fprintf(stderr, "spritemod: tick %ld: the leader's piece %s (id %X, art tile %X, %d pieces in the table)\n", tick,
                lost ? "not found: the game's own" : "found", r16(OBJECTS + O_ID), (unsigned)(tile0[0] < 0 ? 0xFFF : tile0[0]), nsp);
    }
    if (lx == 9999) return;
    int dx = lx - (int)(int16_t)r16(OBJECTS + O_X), dy = ly - (int)(int16_t)r16(OBJECTS + O_Y);
    /* each piece to the nearest object that has its tiles (two townspeople of a kind share theirs) */
    for (int i = 0; i < OBJ_COUNT; i++) {
        if (tile0[i] < 0) continue;
        unsigned o = OBJECTS + (unsigned)i * OBJ_SIZE;
        int ex = (int)(int16_t)r16(o + O_X) + dx, ey = (int)(int16_t)r16(o + O_Y) + dy;
        for (int k = 0; k < nsp; k++) {
            if (sp[k].tile < tile0[i] || sp[k].tile >= tile1[i]) continue;
            int ax = sp[k].x - ex, ay = sp[k].y - ey;
            if (ax < -16 || ax > 16 || ay < -16 || ay > 48) continue;
            int far = ax * ax + ay * ay;
            if (far < sp[k].far) { sp[k].far = far; sp[k].owner = i; }
        }
    }

    memset(slots, 0, sizeof slots);
    memset(fine_slots, 0, sizeof fine_slots);
    int any = 0;
    static uint32_t logged[OBJ_COUNT];
    for (int i = 0; i < OBJ_COUNT; i++) {
        if (tile0[i] < 0) continue;
        unsigned o = OBJECTS + (unsigned)i * OBJ_SIZE;
        uint32_t table = r32(o + O_SCRIPTS);
        int bx0 = 9999, by0 = 9999, bx1 = -9999, by1 = -9999, n = 0, pal = 0;
        for (int k = 0; k < nsp; k++)
            if (sp[k].owner == i) {
                n++;
                pal = sp[k].pal;
                if (sp[k].x < bx0) bx0 = sp[k].x;
                if (sp[k].y < by0) by0 = sp[k].y;
                if (sp[k].x + sp[k].w > bx1) bx1 = sp[k].x + sp[k].w;
                if (sp[k].y + sp[k].h > by1) by1 = sp[k].y + sp[k].h;
            }
        /* whose art it is */
        char kind = 'R';
        uint32_t key = r32(o + O_ART_ROM);
        if (!key && n) {
            kind = 'A';
            key = 2166136261u;
            const uint8_t *t = &vdp.vram[((uint32_t)tile0[i] * 32u) & 0xFFFF];
            for (int b = 0; b < 16 * 32 && (uint32_t)tile0[i] * 32u + (uint32_t)b < 0x10000; b++) key = (key ^ t[b]) * 16777619u;
        }
        const Art *a = NULL;
        for (int k = 0; k < narts && !a; k++)
            if (arts[k].kind == kind && arts[k].target == key) a = &arts[k];
        if (logging && n && logged[i] != key + 1u) {            /* who is on the screen, once each */
            logged[i] = key + 1u;
            fprintf(stderr, "spritemod: object %d: key %c%X, %d pieces in %d x %d, palette line %d, tiles from %03X, animations %06X%s\n", i,
                    kind, (unsigned)key, n, bx1 - bx0, by1 - by0, pal, (unsigned)tile0[i], (unsigned)table, a ? ": replaced" : "");
        }
        /* standing, the game's count stands too: seen by the object not moving */
        int x = (int)r16(o + O_X), y = (int)r16(o + O_Y);
        was[i].still = (x == was[i].x && y == was[i].y) ? was[i].still + 1 : 0;
        was[i].x = x;
        was[i].y = y;
        if (!a || !n || bx1 - bx0 < 16 || by1 - by0 < 24) continue;   /* (a person's size: not a small thing on the same tiles) */
        int facing = (int)(r16(o + O_FACING) >> 2 & 3);
        int frame = (int)r8(o + O_FRAME), left = (int)(int8_t)r8(o + O_HOLD_LEFT);
        uint32_t script = rom32(table + (uint32_t)facing * 4u);
        int hold = (rom8(script) & 0x80) ? 7 : (int)rom8(script + 1);    /* (a list of its own holds: taken as 8 ticks) */
        int g = frame % a->frames, sub = 0;
        if (was[i].still < 2) {
            int gone = hold - left;
            if (gone < 0) gone = 0;
            if (gone > hold) gone = hold;
            sub = gone * a->mult / (hold + 1);
        }
        const uint8_t *px = a->px + ((size_t)(facing * a->frames + g) * (size_t)a->mult + (size_t)sub) * (size_t)(a->w * a->h);
        int16_t x0 = (int16_t)((bx0 + bx1) / 2 - a->w / 2), y0 = (int16_t)(by1 - a->h);   /* feet on the box's foot, centred */
        for (int k = 0; k < nsp; k++)
            if (sp[k].owner == i) {
                VdpSpriteArt *v = &slots[entry[k]];
                v->px = px;
                v->x0 = x0;
                v->y0 = y0;
                v->w = (int16_t)a->w;
                v->h = (int16_t)a->h;
                any = 1;
                if (a->fine) {
                    fine_slots[entry[k]].px = a->fine + (size_t)(px - a->px) * (size_t)(a->res * a->res);
                    fine_slots[entry[k]].res = a->res;
                    fine_any = 1;
                }
            }
    }
    if (any) vdp_sprite_art = slots;
}

int spritemod_fine(void) { return fine_any && vdp_sprite_art != NULL; }

/* The finer sprites' detail, over the enlarged picture. A game pixel at a time, where one of their pieces is
 * the layer the line shows (not where the game puts a roof or a tree in front): each of its ds x ds output
 * pixels takes the fine picture's pixel there -- a colour of the piece's palette line; "the game's own",
 * which is the game's pixel for the whole block; or nothing, which is what the line shows with no sprite.
 * The colours are looked up as the frame ends, so the game's fades carry over. */
void spritemod_draw(const void *lines_, int w, int h, uint32_t *out, int stride, int ds) {
    const VdpLine *lines = lines_;
    if (!spritemod_fine() || ds < 1) return;
    for (int y = 0; y < h; y++) {
        const VdpLine *l = &lines[y];
        for (int x = 0; x < w; x++) {
            uint8_t s = l->sprite[x], shown, under;
            if (!s) continue;
            int me = l->sprite_no[x] & 0x7F;
            if (!fine_slots[me].px) continue;
            vdp_line_codes(l, x, &shown, &under);
            if (shown != s) continue;                               /* behind something: the game's picture stands */
            const VdpSpriteArt *v = &slots[me];
            int res = fine_slots[me].res, fw = v->w * res;
            int ax = (x + vdp_view_x0 - v->x0) * res, ay = (y - v->y0) * res;
            if (ax < 0 || ay < 0 || ax >= fw || ay >= v->h * res) continue;
            uint32_t below = vdp_color(under & 0x3F), own = l->sprite_own[x] ? vdp_color(l->sprite_own[x] & 0x3F) : below;
            uint32_t *row = out + (size_t)(y * ds) * (size_t)stride + (size_t)(x * ds);
            for (int j = 0; j < ds; j++, row += stride) {
                const uint8_t *src = fine_slots[me].px + (size_t)(ay + (j * 2 + 1) * res / (ds * 2)) * (size_t)fw + (size_t)ax;
                for (int i = 0; i < ds; i++) {
                    uint8_t c = src[(i * 2 + 1) * res / (ds * 2)];
                    row[i] = c == 0xFF ? own : (c & 15) ? vdp_color((s & 0x30) | (c & 15)) : below;
                }
            }
        }
    }
}
