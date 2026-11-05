/*
 * Touch controls: see touch.h.
 */
#include "touch.h"
#include "bus.h"

#include <math.h>

#define MAX_FINGERS 10

enum { CTL_NONE, CTL_DPAD, CTL_A, CTL_B, CTL_C, CTL_START, CTL_MENU, CTL_FREE };

typedef struct {
    SDL_FingerID id;
    int used, ctl;
    float x0, y0, x, y;                  /* where it went down; where it is (render pixels) */
    Uint64 t0;
} Finger;

typedef struct { float x, y, r; } Circle;

static Finger fingers[MAX_FINGERS];
static int visible, menu_pressed;
static uint8_t held;
static Circle dpad, button[4], menu;     /* button: A, B, C, Start */
static float unit;                       /* a sixth of the screen's short side */

/* the layout for this screen: the d-pad bottom left, A B C rising to the
 * right like the Mega Drive pad's, Start at the bottom middle, the menu
 * button top left -- on a wide phone mostly beside the picture */
static void layout(int ow, int oh) {
    float W = (float)ow, H = (float)oh, u = (W < H ? W : H) / 6.0f;
    unit = u;
    dpad = (Circle){1.35f * u, H - 1.35f * u, 0.95f * u};
    button[0] = (Circle){W - 2.55f * u, H - 0.80f * u, 0.42f * u};
    button[1] = (Circle){W - 1.60f * u, H - 1.15f * u, 0.42f * u};
    button[2] = (Circle){W - 0.70f * u, H - 1.55f * u, 0.42f * u};
    button[3] = (Circle){W * 0.5f, H - 0.42f * u, 0.30f * u};
    menu = (Circle){0.55f * u, 0.55f * u, 0.30f * u};
}

static float dist(const Circle *c, float x, float y) { return hypotf(x - c->x, y - c->y); }

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

void touch_hide(void) {
    visible = 0;
    SDL_zeroa(fingers);
    held = 0;
}

int touch_event(const SDL_Event *e, int ow, int oh, float *tap_x, float *tap_y) {
    if (ow <= 0 || oh <= 0) return 0;
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
        /* the first touch only shows the pad: it presses nothing */
        f->ctl = was ? control_at(x, y) : CTL_FREE;
        f->x0 = f->x = x;
        f->y0 = f->y = y;
        f->t0 = SDL_GetTicks();
        if (f->ctl == CTL_MENU) menu_pressed = 1;
        break;
    }
    case SDL_EVENT_FINGER_MOTION:
        if (!f) return 0;
        f->x = x;
        f->y = y;
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
static void disc(SDL_Renderer *r, float cx, float cy, float rad, float a) {
    enum { N = 48 };
    SDL_Vertex v[N + 1];
    int idx[N * 3];
    SDL_FColor c = {1.0f, 1.0f, 1.0f, a};
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
    SDL_FColor c = {1.0f, 1.0f, 1.0f, a};
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
    SDL_SetRenderDrawColorFloat(r, 1.0f, 1.0f, 1.0f, a);
    SDL_FRect q = {x, y, w, h};
    SDL_RenderFillRect(r, &q);
}

/* SDL's 8x8 debug font, scaled to whole pixels, centred on (cx, cy) */
static void label(SDL_Renderer *r, float cx, float cy, const char *s, float a) {
    float k = floorf(unit / 45.0f);
    if (k < 1) k = 1;
    SDL_SetRenderScale(r, k, k);
    SDL_SetRenderDrawColorFloat(r, 1.0f, 1.0f, 1.0f, a);
    SDL_RenderDebugText(r, cx / k - 4.0f * (float)SDL_strlen(s), cy / k - 4.0f, s);
    SDL_SetRenderScale(r, 1, 1);
}

void touch_draw(SDL_Renderer *r, int ow, int oh) {
    if (!visible || ow <= 0 || oh <= 0) return;
    layout(ow, oh);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    float line = unit * 0.035f > 2.0f ? unit * 0.035f : 2.0f;

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
