/*
 * Touch controls: see touch.h.
 */
#include "touch.h"
#include "bus.h"

#include <math.h>

#define MAX_FINGERS 10

enum { CTL_NONE, CTL_DPAD, CTL_A, CTL_B, CTL_C, CTL_START, CTL_MENU, CTL_FREE,
       CTL_TWO,                          /* one of the two fingers of the rewind gesture */
       CTL_USED };                       /* ... after it: it taps nothing when it lifts */
enum { GRP_DPAD, GRP_ABC, GRP_START, GRP_MENU, GROUPS };   /* what is moved as one when they are arranged */

typedef struct {
    SDL_FingerID id;
    int used, ctl;
    float x0, y0, x, y;                  /* where it went down; where it is (render pixels) */
    Uint64 t0;
} Finger;

typedef struct { float x, y, r; } Circle;

static Finger fingers[MAX_FINGERS];
static int visible, menu_pressed, suspended;
static uint8_t held;
static Circle dpad, button[4], menu;     /* button: A, B, C, Start */
static float unit;                       /* a sixth of the screen's short side, times the size chosen */

/* two fingers = the rewind button (PsiLok, 2026-11-22: "Can we build a two finger tap as the rewind. two fingers
 * held down brings up the rewind screen") */
#define TWO_APART_MS 250                 /* the second finger lands within this of the first */
#define TWO_HOLD_MS  500                 /* held this long: the menu (as the button's own hold, RW_HOLD_MS) */
static int two_state;                    /* 0 none, 1 both down and waiting, 2 held: said */
static Uint64 two_t0;                    /* when the second came down */
static int two_tapped;

/* Settings > Controls (PsiLok, 2026-11-22: "Lets build a Touch screen button setting to fulfill one of the requests
 * on github" -- issue 10: "the option to change their position, opacity and size") */
static float k_size = 1.0f, k_alpha = 1.0f;
static float moved[GROUPS][2];           /* each group from its usual place, in short sides of the screen */
static int editing, edit_done;
static int grab = -1;                    /* the group being dragged, */
static int grab_mouse;                   /* by the mouse or by */
static SDL_FingerID grab_finger;         /* this finger, */
static float grab_x, grab_y;             /* last seen here */
static SDL_FRect edit_btn[2];            /* Reset, Done */

static float clampf(float v, float lo, float hi) { return hi < lo ? (lo + hi) * 0.5f : v < lo ? lo : v > hi ? hi : v; }

/* the layout for this screen: the d-pad bottom left, A B C rising to the
 * right like the Mega Drive pad's, Start at the bottom middle, the menu
 * button top left -- on a wide phone mostly beside the picture */
static void layout(int ow, int oh) {
    float W = (float)ow, H = (float)oh, S = W < H ? W : H, u = S / 6.0f * k_size;
    unit = u;
    dpad = (Circle){1.35f * u, H - 1.35f * u, 0.95f * u};
    button[0] = (Circle){W - 2.55f * u, H - 0.80f * u, 0.42f * u};
    button[1] = (Circle){W - 1.60f * u, H - 1.15f * u, 0.42f * u};
    button[2] = (Circle){W - 0.70f * u, H - 1.55f * u, 0.42f * u};
    button[3] = (Circle){W * 0.5f, H - 0.42f * u, 0.30f * u};
    menu = (Circle){0.55f * u, 0.55f * u, 0.30f * u};
    /* each group where it was put, whole on the screen whatever the screen (and the size) is now */
    Circle *first[GROUPS] = {&dpad, &button[0], &button[3], &menu};
    static const int count[GROUPS] = {1, 3, 1, 1};
    for (int g = 0; g < GROUPS; g++) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < count[g]; i++) {
            const Circle *c = first[g] + i;
            x0 = fminf(x0, c->x - c->r); x1 = fmaxf(x1, c->x + c->r);
            y0 = fminf(y0, c->y - c->r); y1 = fmaxf(y1, c->y + c->r);
        }
        float dx = clampf(moved[g][0] * S, -x0, W - x1), dy = clampf(moved[g][1] * S, -y0, H - y1);
        if (editing && grab != g) { moved[g][0] = dx / S; moved[g][1] = dy / S; }   /* (kept as it is shown) */
        for (int i = 0; i < count[g]; i++) { first[g][i].x += dx; first[g][i].y += dy; }
    }
}

static float dist(const Circle *c, float x, float y) { return hypotf(x - c->x, y - c->y); }

void touch_set_look(int size_pct, int opacity_pct) {
    k_size = clampf((float)size_pct, 50.0f, 200.0f) / 100.0f;
    k_alpha = clampf((float)opacity_pct, 0.0f, 300.0f) / 100.0f;
}

/* eight whole numbers, thousandths of the screen's short side: x and y of the d-pad, A B C, Start, the menu button */
void touch_set_layout(const char *s) {
    SDL_zeroa(moved);
    for (int i = 0; i < GROUPS * 2 && s && *s; i++) {
        char *e;
        long v = SDL_strtol(s, &e, 10);
        if (e == s) break;
        moved[i / 2][i % 2] = clampf((float)v, -3000.0f, 3000.0f) / 1000.0f;
        s = *e == ',' ? e + 1 : e;
    }
}

void touch_layout_get(char *out, size_t n) {
    int any = 0, v[GROUPS * 2];
    for (int i = 0; i < GROUPS * 2; i++) any |= v[i] = (int)lrintf(moved[i / 2][i % 2] * 1000.0f);
    if (!any) { if (n) out[0] = 0; return; }
    SDL_snprintf(out, n, "%d,%d,%d,%d,%d,%d,%d,%d", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

/* ---------------------------------------------------------------- arranging */
static int group_at(float x, float y) {
    if (dist(&dpad, x, y) <= dpad.r * 1.15f) return GRP_DPAD;
    for (int i = 0; i < 3; i++) if (dist(&button[i], x, y) <= button[i].r * 1.6f) return GRP_ABC;
    if (dist(&button[3], x, y) <= button[3].r * 2.0f) return GRP_START;
    if (dist(&menu, x, y) <= menu.r * 2.0f) return GRP_MENU;
    return -1;
}

static void edit_buttons(int ow, int oh) {
    float S = (float)(ow < oh ? ow : oh), w = S * 0.30f, h = S * 0.11f, gap = S * 0.04f, y = S * 0.20f;
    edit_btn[0] = (SDL_FRect){(float)ow * 0.5f - gap * 0.5f - w, y, w, h};
    edit_btn[1] = (SDL_FRect){(float)ow * 0.5f + gap * 0.5f, y, w, h};
}

static int in_rect(const SDL_FRect *q, float x, float y) { return x >= q->x && y >= q->y && x < q->x + q->w && y < q->y + q->h; }

/* a finger or the mouse, arranging: phase 0 down, 1 moved, 2 up */
static void edit_point(int phase, int mouse, SDL_FingerID id, float x, float y, int ow, int oh) {
    float S = (float)(ow < oh ? ow : oh);
    layout(ow, oh);
    edit_buttons(ow, oh);
    if (phase == 0) {
        if (grab >= 0 && mouse && grab_mouse) grab = -1;     /* (the mouse's button came up unseen) */
        if (grab >= 0) return;                               /* one at a time */
        if (in_rect(&edit_btn[0], x, y)) { SDL_zeroa(moved); return; }
        if (in_rect(&edit_btn[1], x, y)) { edit_done = 1; return; }
        grab = group_at(x, y);
        grab_mouse = mouse;
        grab_finger = id;
        grab_x = x;
        grab_y = y;
        return;
    }
    if (grab < 0 || grab_mouse != mouse || (!mouse && grab_finger != id)) return;
    if (phase == 1) {
        moved[grab][0] += (x - grab_x) / S;
        moved[grab][1] += (y - grab_y) / S;
        grab_x = x;
        grab_y = y;
    } else {
        grab = -1;
        layout(ow, oh);                                      /* (let go past an edge: it stays where it is shown) */
    }
}

void touch_edit(int on) {
    editing = on;
    two_state = two_tapped = 0;
    edit_done = 0;
    grab = -1;
    SDL_zeroa(fingers);
    held = 0;
}
int touch_editing(void) { return editing; }
void touch_edit_pointer(int phase, float x, float y, int ow, int oh) {
    if (editing && ow > 0 && oh > 0) edit_point(phase, 1, 0, x, y, ow, oh);
}
int touch_edit_take_done(void) {
    int d = edit_done;
    edit_done = 0;
    return d;
}

/* the button under (x, y), with some slack; the nearest when two are close */
static int button_at(float x, float y) {
    int best = CTL_NONE;
    float bd = 1e9f;
    for (int i = 0; i < 4; i++) {
        float d = dist(&button[i], x, y);
        if (d <= button[i].r * 1.25f && d < bd) { bd = d; best = CTL_A + i; }
    }
    return best;
}

static int control_at(float x, float y) {
    if (dist(&dpad, x, y) <= dpad.r * 1.3f) return CTL_DPAD;
    int b = button_at(x, y);
    if (b) return b;
    if (dist(&menu, x, y) <= menu.r * 1.6f) return CTL_MENU;
    return CTL_FREE;
}

/* eight equal sectors (|sin| > sin 22.5 deg), a small dead centre */
static uint8_t dpad_bits(float x, float y) {
    float dx = x - dpad.x, dy = y - dpad.y, len = hypotf(dx, dy);
    if (len < dpad.r * 0.18f) return 0;
    uint8_t b = 0;
    if (dx > 0.38f * len) b |= PAD_RIGHT;
    if (dx < -0.38f * len) b |= PAD_LEFT;
    if (dy > 0.38f * len) b |= PAD_DOWN;
    if (dy < -0.38f * len) b |= PAD_UP;
    return b;
}

static void recompute(void) {
    static const uint8_t bits[4] = {PAD_A, PAD_B, PAD_C, PAD_START};
    held = 0;
    for (int i = 0; i < MAX_FINGERS; i++) {
        const Finger *f = &fingers[i];
        if (!f->used) continue;
        if (f->ctl == CTL_DPAD) held |= dpad_bits(f->x, f->y);
        else if (f->ctl >= CTL_A && f->ctl <= CTL_START) held |= bits[f->ctl - CTL_A];
    }
}

void touch_init(int on) { visible = on; }
int touch_visible(void) { return visible; }
uint8_t touch_pad(void) { return visible ? held : 0; }

int touch_take_menu(void) {
    int m = menu_pressed;
    menu_pressed = 0;
    return m;
}

static void two_end(void) {              /* over: its fingers tap nothing */
    two_state = 0;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (fingers[i].used && fingers[i].ctl == CTL_TWO) fingers[i].ctl = CTL_USED;
}

int touch_take_rewind(void) {
    if (two_tapped) { two_tapped = 0; return 1; }
    if (two_state == 1 && SDL_GetTicks() - two_t0 >= TWO_HOLD_MS) { two_state = 2; return 2; }
    return 0;
}

void touch_hide(void) {
    visible = 0;
    SDL_zeroa(fingers);
    held = 0;
    two_state = two_tapped = 0;
}

void touch_suspend(int on) {
    suspended = on;
    SDL_zeroa(fingers);
    held = 0;
    two_state = two_tapped = 0;
}

int touch_event(const SDL_Event *e, int ow, int oh, float *tap_x, float *tap_y) {
    if (ow <= 0 || oh <= 0) return 0;
    if (editing) {                                           /* arranging them: nothing is pressed, nothing tapped */
        edit_point(e->type == SDL_EVENT_FINGER_DOWN ? 0 : e->type == SDL_EVENT_FINGER_MOTION ? 1 : 2, 0, e->tfinger.fingerID,
                   e->tfinger.x * (float)ow, e->tfinger.y * (float)oh, ow, oh);
        return 0;
    }
    layout(ow, oh);
    float x = e->tfinger.x * (float)ow, y = e->tfinger.y * (float)oh;
    Finger *f = NULL;
    for (int i = 0; i < MAX_FINGERS && !f; i++)
        if (fingers[i].used && fingers[i].id == e->tfinger.fingerID) f = &fingers[i];
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN: {
        int was = visible;
        visible = 1;
        for (int i = 0; i < MAX_FINGERS && !f; i++)
            if (!fingers[i].used) f = &fingers[i];
        if (!f) return 0;
        f->used = 1;
        f->id = e->tfinger.fingerID;
        /* the first touch only shows the pad: it presses nothing (and while
         * suspended, no touch does) */
        f->ctl = was && !suspended ? control_at(x, y) : CTL_FREE;
        f->x0 = f->x = x;
        f->y0 = f->y = y;
        f->t0 = SDL_GetTicks();
        if (f->ctl == CTL_MENU) menu_pressed = 1;
        if (f->ctl == CTL_FREE && !suspended && !two_state) {      /* a second finger beside a first: the rewind button */
            Finger *g = NULL;
            int nfree = 0;
            for (int i = 0; i < MAX_FINGERS; i++)
                if (fingers[i].used && fingers[i].ctl == CTL_FREE) { nfree++; if (&fingers[i] != f) g = &fingers[i]; }
            if (nfree == 2 && f->t0 - g->t0 <= TWO_APART_MS) {
                f->ctl = g->ctl = CTL_TWO;
                two_state = 1;
                two_t0 = f->t0;
            }
        }
        break;
    }
    case SDL_EVENT_FINGER_MOTION:
        if (!f) return 0;
        f->x = x;
        f->y = y;
        if (f->ctl == CTL_TWO && two_state == 1 && hypotf(x - f->x0, y - f->y0) > 0.06f * (float)(ow < oh ? ow : oh))
            two_end();                                     /* slid: something else was meant */
        if (f->ctl >= CTL_A && f->ctl <= CTL_C) {          /* roll between A, B and C */
            int b = button_at(x, y);
            if (b >= CTL_A && b <= CTL_C) f->ctl = b;
        }
        break;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        if (!f) return 0;
        float sh = (float)(ow < oh ? ow : oh);
        int tap = e->type == SDL_EVENT_FINGER_UP && f->ctl == CTL_FREE &&
                  hypotf(f->x - f->x0, f->y - f->y0) < 0.05f * sh && SDL_GetTicks() - f->t0 < 500;
        float tx = f->x0, ty = f->y0;
        if (f->ctl == CTL_TWO) {                           /* the first of the two lifts: a tap, if it was quick */
            if (two_state == 1 && e->type == SDL_EVENT_FINGER_UP) two_tapped = 1;
            two_end();
        }
        f->used = 0;
        recompute();
        if (tap) {
            *tap_x = tx;
            *tap_y = ty;
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
    recompute();
    return 0;
}

/* ------------------------------------------------------------------ drawing */
static float k_draw = 1.0f;              /* the strength chosen (arranging: never too faint to find) */
static float alpha(float a) { a *= k_draw; return a > 1.0f ? 1.0f : a; }

static void disc(SDL_Renderer *r, float cx, float cy, float rad, float a) {
    enum { N = 48 };
    SDL_Vertex v[N + 1];
    int idx[N * 3];
    SDL_FColor c = {1.0f, 1.0f, 1.0f, alpha(a)};
    v[0] = (SDL_Vertex){{cx, cy}, c, {0, 0}};
    for (int i = 0; i < N; i++) {
        float t = (float)i / N * 6.2831853f;
        v[i + 1] = (SDL_Vertex){{cx + rad * cosf(t), cy + rad * sinf(t)}, c, {0, 0}};
        idx[i * 3] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = (i + 1) % N + 1;
    }
    SDL_RenderGeometry(r, NULL, v, N + 1, idx, N * 3);
}

static void ring(SDL_Renderer *r, float cx, float cy, float rad, float width, float a) {
    enum { N = 64 };
    SDL_Vertex v[N * 2];
    int idx[N * 6];
    SDL_FColor c = {1.0f, 1.0f, 1.0f, alpha(a)};
    for (int i = 0; i < N; i++) {
        float t = (float)i / N * 6.2831853f, ct = cosf(t), st = sinf(t);
        v[i * 2] = (SDL_Vertex){{cx + rad * ct, cy + rad * st}, c, {0, 0}};
        v[i * 2 + 1] = (SDL_Vertex){{cx + (rad - width) * ct, cy + (rad - width) * st}, c, {0, 0}};
        int j = (i + 1) % N;
        int q[6] = {i * 2, i * 2 + 1, j * 2, j * 2, i * 2 + 1, j * 2 + 1};
        for (int k = 0; k < 6; k++) idx[i * 6 + k] = q[k];
    }
    SDL_RenderGeometry(r, NULL, v, N * 2, idx, N * 6);
}

static void rect(SDL_Renderer *r, float x, float y, float w, float h, float a) {
    SDL_SetRenderDrawColorFloat(r, 1.0f, 1.0f, 1.0f, alpha(a));
    SDL_FRect q = {x, y, w, h};
    SDL_RenderFillRect(r, &q);
}

/* SDL's 8x8 debug font, scaled to whole pixels, centred on (cx, cy) */
static void label(SDL_Renderer *r, float cx, float cy, const char *s, float a) {
    float k = floorf(unit / 45.0f);
    if (k < 1) k = 1;
    SDL_SetRenderScale(r, k, k);
    SDL_SetRenderDrawColorFloat(r, 1.0f, 1.0f, 1.0f, alpha(a));
    SDL_RenderDebugText(r, cx / k - 4.0f * (float)SDL_strlen(s), cy / k - 4.0f, s);
    SDL_SetRenderScale(r, 1, 1);
}

/* arranging: the screen dimmed, what to do, the two buttons */
static void edit_draw(SDL_Renderer *r, int ow, int oh) {
    float S = (float)(ow < oh ? ow : oh);
    edit_buttons(ow, oh);
    SDL_SetRenderDrawColorFloat(r, 0.0f, 0.0f, 0.0f, 0.55f);
    SDL_FRect all = {0, 0, (float)ow, (float)oh};
    SDL_RenderFillRect(r, &all);
    float k = floorf(S / 300.0f);
    if (k < 1) k = 1;
    static const char *const say[2] = {"Drag the controls to where you want them", "Size and opacity: Settings > Controls"};
    SDL_SetRenderScale(r, k, k);
    for (int i = 0; i < 2; i++) {
        SDL_SetRenderDrawColorFloat(r, 1.0f, 1.0f, 1.0f, i ? 0.6f : 0.95f);
        SDL_RenderDebugText(r, (float)ow * 0.5f / k - 4.0f * (float)SDL_strlen(say[i]), (S * 0.07f + (float)i * S * 0.055f) / k, say[i]);
    }
    SDL_SetRenderScale(r, 1, 1);
    static const char *const name[2] = {"Reset", "Done"};
    for (int i = 0; i < 2; i++) {
        const SDL_FRect *q = &edit_btn[i];
        SDL_SetRenderDrawColorFloat(r, i ? 1.0f : 0.25f, i ? 0.80f : 0.30f, i ? 0.38f : 0.55f, 0.92f);
        SDL_RenderFillRect(r, q);
        SDL_SetRenderDrawColorFloat(r, 1.0f, 1.0f, 1.0f, 0.9f);
        SDL_RenderRect(r, q);
        SDL_SetRenderScale(r, k, k);
        SDL_SetRenderDrawColorFloat(r, i ? 0.05f : 1.0f, i ? 0.05f : 1.0f, i ? 0.15f : 1.0f, 1.0f);
        SDL_RenderDebugText(r, (q->x + q->w * 0.5f) / k - 4.0f * (float)SDL_strlen(name[i]), (q->y + q->h * 0.5f) / k - 4.0f, name[i]);
        SDL_SetRenderScale(r, 1, 1);
    }
}

void touch_draw(SDL_Renderer *r, int ow, int oh) {
    if ((!visible && !editing) || ow <= 0 || oh <= 0) return;
    layout(ow, oh);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    float line = unit * 0.035f > 2.0f ? unit * 0.035f : 2.0f;
    k_draw = k_alpha;
    if (editing) {
        edit_draw(r, ow, oh);
        if (k_draw < 1.6f) k_draw = 1.6f;                    /* the controls as chosen, never too faint to find */
        if (grab >= 0) {                                     /* the one held: a ring about it */
            const Circle *c = grab == GRP_DPAD ? &dpad : grab == GRP_ABC ? &button[1] : grab == GRP_START ? &button[3] : &menu;
            float rr = grab == GRP_ABC ? hypotf(button[2].x - button[0].x, button[2].y - button[0].y) * 0.5f + button[0].r * 1.25f : c->r * 1.35f;
            ring(r, c->x, c->y, rr, line * 1.5f, 0.55f);
        }
    }

    /* the d-pad: a disc, a cross, the held arms lit */
    disc(r, dpad.x, dpad.y, dpad.r, 0.10f);
    ring(r, dpad.x, dpad.y, dpad.r, line, 0.30f);
    float arm = dpad.r * 0.30f, len = dpad.r * 0.78f;
    rect(r, dpad.x - arm, dpad.y - len, 2 * arm, 2 * len, 0.16f);
    rect(r, dpad.x - len, dpad.y - arm, 2 * len, 2 * arm, 0.16f);
    uint8_t d = held & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    if (d & PAD_UP) rect(r, dpad.x - arm, dpad.y - len, 2 * arm, len - arm, 0.35f);
    if (d & PAD_DOWN) rect(r, dpad.x - arm, dpad.y + arm, 2 * arm, len - arm, 0.35f);
    if (d & PAD_LEFT) rect(r, dpad.x - len, dpad.y - arm, len - arm, 2 * arm, 0.35f);
    if (d & PAD_RIGHT) rect(r, dpad.x + arm, dpad.y - arm, len - arm, 2 * arm, 0.35f);

    static const char *names[4] = {"A", "B", "C", "START"};
    static const uint8_t bits[4] = {PAD_A, PAD_B, PAD_C, PAD_START};
    for (int i = 0; i < 4; i++) {
        const Circle *c = &button[i];
        int on = (held & bits[i]) != 0;
        disc(r, c->x, c->y, c->r, on ? 0.40f : 0.14f);
        ring(r, c->x, c->y, c->r, line, 0.35f);
        if (i == 3) label(r, c->x, c->y - c->r - unit * 0.18f, names[i], 0.55f);   /* Start: a small round key */
        else label(r, c->x, c->y, names[i], on ? 0.95f : 0.60f);
    }

    /* the menu button: three bars */
    disc(r, menu.x, menu.y, menu.r, 0.12f);
    ring(r, menu.x, menu.y, menu.r, line, 0.30f);
    for (int i = -1; i <= 1; i++)
        rect(r, menu.x - menu.r * 0.5f, menu.y + (float)i * menu.r * 0.36f - line * 0.5f, menu.r, line, 0.60f);
}
