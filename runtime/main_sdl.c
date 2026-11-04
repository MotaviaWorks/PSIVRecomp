/*
 * SDL3 front end: window, input, 60 Hz pacing, battery saves.
 *
 *   psiv [--rom PATH] [--scale N]
 *
 * Keys: arrows = D-pad, Z/X/C = A/B/C, Enter = Start, Tab (hold) = fast
 * forward, F11 = fullscreen, F12 = screenshot (screenshots/), Esc = quit.
 * Gamepads: D-pad or left stick; West/South/East = A/B/C; Start = Start.
 */
#include "bus.h"
#include "frame.h"
#include "m68k.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTSC_FRAME_NS 16688154ull     /* 1 / 59.922751 Hz */
#define SRAM_PATH "saves/psiv.srm"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h;
static SDL_Gamepad *pad;
static uint64_t next_deadline;
static uint64_t last_sram_flush;

static void sram_load(void) {
    FILE *f = fopen(SRAM_PATH, "rb");
    if (!f) return;
    size_t n = fread(bus_sram, 1, sizeof bus_sram, f);
    fclose(f);
    SDL_Log("loaded %s (%u bytes)", SRAM_PATH, (unsigned)n);
}

static void sram_flush(void) {
    if (!bus_sram_dirty) return;
    SDL_CreateDirectory("saves");
    FILE *f = fopen(SRAM_PATH, "wb");
    if (!f) { SDL_Log("cannot write %s", SRAM_PATH); return; }
    fwrite(bus_sram, 1, sizeof bus_sram, f);
    fclose(f);
    bus_sram_dirty = 0;
}

static void quit(int code) {
    sram_flush();
    SDL_Quit();
    exit(code);
}

static void screenshot(const uint32_t *pixels, int w, int h) {
    char path[64];
    SDL_CreateDirectory("screenshots");
    snprintf(path, sizeof path, "screenshots/psiv_%06llu.bmp", (unsigned long long)frame_count);
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ARGB8888, (void *)pixels, VDP_MAX_W * 4);
    if (s) {
        SDL_SaveBMP(s, path);
        SDL_DestroySurface(s);
        SDL_Log("saved %s", path);
    }
}

static uint8_t read_input(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    if (k[SDL_SCANCODE_UP]) b |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= PAD_RIGHT;
    if (k[SDL_SCANCODE_Z]) b |= PAD_A;
    if (k[SDL_SCANCODE_X]) b |= PAD_B;
    if (k[SDL_SCANCODE_C]) b |= PAD_C;
    if (k[SDL_SCANCODE_RETURN]) b |= PAD_START;
    if (pad) {
        const int dead = 12000;
        int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        int ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -dead) b |= PAD_UP;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > dead) b |= PAD_DOWN;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -dead) b |= PAD_LEFT;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > dead) b |= PAD_RIGHT;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST)) b |= PAD_A;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) b |= PAD_B;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST)) b |= PAD_C;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) b |= PAD_START;
    }
    return b;
}

void platform_frame(const uint32_t *pixels, int w, int h) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            quit(0);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) quit(0);
            if (e.key.key == SDLK_F12) screenshot(pixels, w, h);
            if (e.key.key == SDLK_F11)
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = NULL; }
            break;
        }
    }
    bus_pad[0] = read_input();

    if (w != tex_w || h != tex_h) {
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        SDL_SetRenderLogicalPresentation(renderer, w, h, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        tex_w = w;
        tex_h = h;
    }
    SDL_UpdateTexture(texture, NULL, pixels, VDP_MAX_W * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    uint64_t now = SDL_GetTicksNS();
    if (now - last_sram_flush > 1000000000ull) {
        sram_flush();
        last_sram_flush = now;
    }
    if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB]) {   /* fast forward: no pacing */
        next_deadline = now;
        return;
    }
    next_deadline += NTSC_FRAME_NS;
    if (next_deadline > now) SDL_DelayPrecise(next_deadline - now);
    else if (now - next_deadline > 4 * NTSC_FRAME_NS) next_deadline = now;   /* fell behind: resync */
}

int main(int argc, char **argv) {
    const char *rom_path = "rom/psiv.bin";
    int scale = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
    }
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s (run tools/prepare_rom.py on your dump first)\n", rom_path);
        return 1;
    }
    static uint8_t rom[0x400000];
    uint32_t n = (uint32_t)fread(rom, 1, sizeof rom, f);
    fclose(f);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_CreateWindowAndRenderer("Phantasy Star IV", 320 * scale, 224 * scale,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 0);              /* paced to 59.92 Hz ourselves */

    bus_init(rom, n);
    sram_load();
    next_deadline = SDL_GetTicksNS();

    m68k.s = 1;
    m68k.imask = 7;
    A(7) = rd32(0);
    m68k_call(rd32(4));                           /* the game never returns from reset */
    quit(1);
    return 1;
}
