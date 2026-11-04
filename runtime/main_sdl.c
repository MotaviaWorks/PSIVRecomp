/*
 * SDL3 front end: window, input, audio, 60 Hz pacing, battery saves,
 * settings, crash and hang reports.
 *
 *   psiv [--rom PATH]
 *
 * Files live in the "home" folder: the folder holding psiv.exe, or the
 * project root when running a development build from build/native-*. There:
 *   psiv.ini         settings (written with defaults on first run)
 *   rom/psiv.bin     the ROM; if missing, a file dialog asks for your dump
 *                    (.smd or raw), checks it and writes this file
 *   saves/psiv.srm   battery save (the cartridge's 8 KB)
 *   screenshots/     F12
 *   psiv-crash.txt   written if the game hits an error or stops responding
 *
 * Default keys: arrows = D-pad, Z/X/C = A/B/C, Enter = Start, Tab (hold) =
 * fast forward, F10 = pause, F11 = fullscreen, F12 = screenshot, Esc = quit.
 * Gamepads: D-pad or left stick; x/a/b (west/south/east) = A/B/C; start.
 */
#include "bus.h"
#include "depth.h"
#include "diag.h"
#include "frame.h"
#include "m68k.h"
#include "sha256.h"
#include "sound.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTSC_FRAME_NS 16688154ull     /* 1 / 59.922751 Hz */
#define HANG_SECONDS 8

/* Phantasy Star IV (USA), raw: the only ROM the recompiled code matches */
static const char *ROM_SHA256 = "511f35cc11f88316f8b8940e28ab298bd75a4da193672a80172884d6eb913b6a";
#define ROM_SIZE (3u * 1024 * 1024)

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h;
static SDL_Gamepad *pad;
static SDL_AudioStream *audio;
static uint64_t next_deadline, last_sram_flush;
static int paused;
static char home[1024];

/* ------------------------------------------------------------------ paths */
static void path_in_home(char *out, size_t n, const char *rel) {
    snprintf(out, n, "%s%s", home, rel);
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void find_home(void) {
    const char *base = SDL_GetBasePath();
    snprintf(home, sizeof home, "%s", base ? base : "./");
    /* development build: build/native-<config>/psiv.exe -> project root */
    char probe[1100];
    snprintf(probe, sizeof probe, "%s../../CMakeLists.txt", home);
    if (file_exists(probe)) {
        size_t len = strlen(home);
        snprintf(home + len, sizeof home - len, "../../");
    }
}

/* SDL_Log output goes to psiv.log in the home folder (the window has no console) */
static FILE *log_file;

static void SDLCALL log_to_file(void *ud, int category, SDL_LogPriority priority, const char *message) {
    (void)ud; (void)category; (void)priority;
    if (!log_file) return;
    fprintf(log_file, "%s\n", message);
    fflush(log_file);
}

static void open_log(void) {
    char p[1100];
    path_in_home(p, sizeof p, "psiv.log");
    log_file = fopen(p, "w");
    SDL_SetLogOutputFunction(log_to_file, NULL);
    SDL_Log("home: %s", home);
}

/* ------------------------------------------------------------------ settings */
typedef struct { const char *key; char value[64]; const char *comment; } Setting;
static Setting settings[] = {
    {"video.scale", "3", "window size at start, in multiples of the game's resolution"},
    {"video.fullscreen", "0", "1 = start fullscreen (F11 toggles)"},
    {"video.integer_scaling", "1", "1 = whole-number scaling only (sharpest)"},
    {"video.aspect", "square", "square = square pixels; 4:3 = the console's TV shape (pixels a bit narrower)"},
    {"video.filter", "nearest", "nearest = sharp pixels; linear = smoothed"},
    {"audio.volume", "100", "0-200 percent"},
    {"enhance.battle_depth", "1", "battles drawn as layers at different depths under a gently moving camera: 0 off, 1 subtle, 2 strong (F9 cycles)"},
    {"keys.up", "Up", "keyboard keys use SDL key names (e.g. Up, Z, Return, Left Shift, Keypad 8)"},
    {"keys.down", "Down", ""},
    {"keys.left", "Left", ""},
    {"keys.right", "Right", ""},
    {"keys.a", "Z", ""},
    {"keys.b", "X", ""},
    {"keys.c", "C", ""},
    {"keys.start", "Return", ""},
    {"keys.fast_forward", "Tab", "hold to run unthrottled"},
    {"pad.a", "x", "gamepad buttons use SDL names: a (south), b (east), x (west), y (north), start, back, leftshoulder, rightshoulder"},
    {"pad.b", "a", ""},
    {"pad.c", "b", ""},
    {"pad.start", "start", ""},
    {"pad.fast_forward", "rightshoulder", ""},
};
#define NSETTINGS (int)(sizeof settings / sizeof settings[0])

static const char *setting(const char *key) {
    for (int i = 0; i < NSETTINGS; i++)
        if (!strcmp(settings[i].key, key)) return settings[i].value;
    return "";
}
static int setting_int(const char *key) { return atoi(setting(key)); }

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    memmove(s, b, strlen(b) + 1);
}

static void write_settings(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "; Phantasy Star IV port settings. Delete this file to restore the defaults.\n");
    const char *section = "";
    for (int i = 0; i < NSETTINGS; i++) {
        const char *dot = strchr(settings[i].key, '.');
        if (strncmp(section, settings[i].key, (size_t)(dot - settings[i].key)) || !*section) {
            fprintf(f, "\n[%.*s]\n", (int)(dot - settings[i].key), settings[i].key);
            section = settings[i].key;
        }
        if (*settings[i].comment) fprintf(f, "; %s\n", settings[i].comment);
        fprintf(f, "%s = %s\n", dot + 1, settings[i].value);
    }
    fclose(f);
}

static void load_settings(void) {
    char path[1100], line[256], section[32] = "";
    path_in_home(path, sizeof path, "psiv.ini");
    FILE *f = fopen(path, "r");
    if (!f) { write_settings(path); return; }
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", line + 1); }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char key[96], *val = eq + 1;
        trim(line);
        trim(val);
        snprintf(key, sizeof key, "%s.%s", section, line);
        for (int i = 0; i < NSETTINGS; i++)
            if (!SDL_strcasecmp(settings[i].key, key)) snprintf(settings[i].value, sizeof settings[i].value, "%s", val);
    }
    fclose(f);
}

/* key bindings resolved from the settings */
enum { IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_A, IN_B, IN_C, IN_START, IN_FF, IN_COUNT };
static const char *in_names[IN_COUNT] = {"up", "down", "left", "right", "a", "b", "c", "start", "fast_forward"};
static SDL_Scancode key_of[IN_COUNT];
static SDL_GamepadButton pad_of[IN_COUNT];

static void resolve_bindings(void) {
    char k[64];
    for (int i = 0; i < IN_COUNT; i++) {
        snprintf(k, sizeof k, "keys.%s", in_names[i]);
        key_of[i] = SDL_GetScancodeFromName(setting(k));
        snprintf(k, sizeof k, "pad.%s", in_names[i]);
        pad_of[i] = *setting(k) ? SDL_GetGamepadButtonFromString(setting(k)) : SDL_GAMEPAD_BUTTON_INVALID;
    }
    pad_of[IN_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP;
    pad_of[IN_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    pad_of[IN_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    pad_of[IN_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
}

static int held(int in) {
    const bool *k = SDL_GetKeyboardState(NULL);
    if (key_of[in] != SDL_SCANCODE_UNKNOWN && k[key_of[in]]) return 1;
    if (pad && pad_of[in] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad, pad_of[in])) return 1;
    if (pad && in <= IN_RIGHT) {
        const int dead = 12000;
        int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if ((in == IN_UP && y < -dead) || (in == IN_DOWN && y > dead) ||
            (in == IN_LEFT && x < -dead) || (in == IN_RIGHT && x > dead)) return 1;
    }
    return 0;
}

static uint8_t read_input(void) {
    static const uint8_t bits[8] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_B, PAD_C, PAD_START};
    uint8_t b = 0;
    for (int i = 0; i < 8; i++)
        if (held(i)) b |= bits[i];
    return b;
}

/* ------------------------------------------------------------------ battery save */
static void sram_path(char *out, size_t n) { path_in_home(out, n, "saves/psiv.srm"); }

static void sram_load(void) {
    char p[1100];
    sram_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    size_t n = fread(bus_sram, 1, sizeof bus_sram, f);
    fclose(f);
    SDL_Log("loaded %s (%u bytes)", p, (unsigned)n);
}

static void sram_flush(void) {
    if (!bus_sram_dirty) return;
    char p[1100], dir[1100];
    path_in_home(dir, sizeof dir, "saves");
    SDL_CreateDirectory(dir);
    sram_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) { SDL_Log("cannot write %s", p); return; }
    fwrite(bus_sram, 1, sizeof bus_sram, f);
    fclose(f);
    bus_sram_dirty = 0;
}

static void quit(int code) {
    sram_flush();
    SDL_Quit();
    exit(code);
}

/* ------------------------------------------------------------------ crash and hang reports */
static void write_report(const char *kind, const char *text, char *path_out, size_t n) {
    path_in_home(path_out, n, "psiv-crash.txt");
    FILE *f = fopen(path_out, "w");
    if (!f) return;
    fprintf(f, "Phantasy Star IV port: %s\nframe %llu\n\n%s\n", kind, (unsigned long long)frame_count, text);
    fprintf(f, "\nPlease send this file along with what you were doing in the game.\n");
    fclose(f);
}

void platform_fatal(const char *text) {
    char path[1100], msg[1400];
    write_report("fatal error", text, path, sizeof path);
    snprintf(msg, sizeof msg,
             "The game hit an error the port can't continue from.\n\nA report was saved to:\n%s\n\n%.400s",
             path, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Phantasy Star IV", msg, window);
    quit(1);
}

/* Watchdog: a frame normally arrives every 1/60 s. If none comes for
 * HANG_SECONDS while not paused, the game is stuck in a loop: report where. */
static int SDLCALL watchdog(void *unused) {
    (void)unused;
    uint64_t last = frame_count, since = SDL_GetTicks();
    int reported = 0;
    for (;;) {
        SDL_Delay(500);
        uint64_t now_frames = *(volatile uint64_t *)&frame_count;
        if (now_frames != last || paused) {
            last = now_frames;
            since = SDL_GetTicks();
            reported = 0;
            continue;
        }
        if (!reported && SDL_GetTicks() - since > HANG_SECONDS * 1000) {
            static char text[8192];
            char path[1100], msg[1400];
            uint32_t calls[1];
            uint32_t where = diag_callstack(calls, 1) ? calls[0] - 2 : 0;
            diag_report(text, sizeof text, "the game stopped producing frames (hang)", where, 0);
            write_report("hang", text, path, sizeof path);
            snprintf(msg, sizeof msg, "The game has stopped responding.\n\nA report was saved to:\n%s", path);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Phantasy Star IV", msg, NULL);
            reported = 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ ROM */
static int read_rom(const char *path, uint8_t *rom, uint32_t *size, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "cannot open %s", path); return 0; }
    static uint8_t raw[0x400200];
    size_t n = fread(raw, 1, sizeof raw, f);
    fclose(f);
    int smd = n % 16384 == 512 && raw[8] == 0xAA && raw[9] == 0xBB;
    /* SMD copier dump: 512-byte header, 16 KB blocks of odd bytes then even bytes */
    if (smd) {
        size_t body = n - 512;
        for (size_t base = 0; base < body; base += 16384)
            for (size_t i = 0; i < 8192; i++) {
                rom[base + i * 2 + 1] = raw[512 + base + i];
                rom[base + i * 2] = raw[512 + base + 8192 + i];
            }
        n = body;
    } else {
        memcpy(rom, raw, n);
    }
    uint8_t digest[32];
    char hex[65];
    sha256(rom, n, digest);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (n != ROM_SIZE || strcmp(hex, ROM_SHA256)) {
        snprintf(err, errn, "%s is not the Phantasy Star IV (USA) cartridge this port was built for.", path);
        SDL_Log("rom: %s rejected (%s, %u bytes, sha256 %s)", path, smd ? "SMD" : "raw", (unsigned)n, hex);
        return 0;
    }
    *size = (uint32_t)n;
    SDL_Log("rom: %s (%s, verified)", path, smd ? "SMD, de-interleaved" : "raw");
    return 1;
}

static volatile int dialog_done;
static char dialog_path[1024];

static void SDLCALL on_pick(void *ud, const char *const *list, int filter) {
    (void)ud; (void)filter;
    if (list && list[0]) snprintf(dialog_path, sizeof dialog_path, "%s", list[0]);
    dialog_done = 1;
}

/* ask for the dump, check it, keep a raw copy as rom/psiv.bin */
static int pick_rom(uint8_t *rom, uint32_t *size) {
    static const SDL_DialogFileFilter filters[] = {{"Mega Drive / Genesis ROM", "smd;bin;md;gen"}, {"All files", "*"}};
    char err[1300], dir[1100], out[1100];
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Phantasy Star IV",
                             "Select your Phantasy Star IV (USA) cartridge dump (.smd, .bin, .md or .gen).", window);
    for (;;) {
        dialog_done = 0;
        dialog_path[0] = 0;
        SDL_ShowOpenFileDialog(on_pick, NULL, window, filters, 2, NULL, false);
        while (!dialog_done) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT) quit(0);
            SDL_Delay(10);
        }
        if (!dialog_path[0]) return 0;                    /* cancelled */
        if (read_rom(dialog_path, rom, size, err, sizeof err)) break;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Phantasy Star IV", err, window);
    }
    path_in_home(dir, sizeof dir, "rom");
    SDL_CreateDirectory(dir);
    path_in_home(out, sizeof out, "rom/psiv.bin");
    FILE *f = fopen(out, "wb");
    if (f) { fwrite(rom, 1, *size, f); fclose(f); }
    return 1;
}

/* ------------------------------------------------------------------ video */
static void screenshot_any(const uint32_t *pixels, int w, int h, int stride) {
    char dir[1100], path[1200];
    path_in_home(dir, sizeof dir, "screenshots");
    SDL_CreateDirectory(dir);
    snprintf(path, sizeof path, "%s/psiv_%06llu.bmp", dir, (unsigned long long)frame_count);
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ARGB8888, (void *)pixels, stride * 4);
    if (s) {
        SDL_SaveBMP(s, path);
        SDL_DestroySurface(s);
        SDL_Log("saved %s", path);
    }
}

/* battle depth: the frame recomposed at a multiple of the game's resolution */
static SDL_Texture *depth_texture;
static int depth_tex_w, depth_tex_h, depth_shown;
static uint32_t depth_pixels[VDP_MAX_W * 4 * VDP_MAX_H * 4];

static SDL_ScaleMode scale_mode(void) {
    return strcmp(setting("video.filter"), "linear") ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR;
}

/* compose = 0 when re-showing the same frame (paused): the depth camera
 * doesn't advance and the last image is shown again */
static void present(const uint32_t *pixels, int w, int h, int compose) {
    int ow, oh;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    /* pixel aspect: square, or the 4:3 TV shape (320x224 pixels are ~0.93 wide) */
    float par = strcmp(setting("video.aspect"), "4:3") ? 1.0f : (4.0f / 3.0f) / ((float)w / (float)h);
    float sw = (float)w * par, sh = (float)h;
    float s = fminf((float)ow / sw, (float)oh / sh);
    if (setting_int("video.integer_scaling") && s >= 1.0f) s = floorf(s);
    SDL_FRect dst = {((float)ow - sw * s) / 2, ((float)oh - sh * s) / 2, sw * s, sh * s};

    if (compose) {
        int ds = (int)ceilf(s);                          /* compose at about the display scale */
        if (ds < 1) ds = 1;
        if (ds > 4) ds = 4;
        depth_shown = depth_compose(frame_lines, w, h, depth_pixels, w * ds, ds);
        if (depth_shown) {
            if (w * ds != depth_tex_w || h * ds != depth_tex_h) {
                if (depth_texture) SDL_DestroyTexture(depth_texture);
                depth_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                                  w * ds, h * ds);
                SDL_SetTextureScaleMode(depth_texture, scale_mode());
                depth_tex_w = w * ds;
                depth_tex_h = h * ds;
            }
            SDL_UpdateTexture(depth_texture, NULL, depth_pixels, w * ds * 4);
        }
    }
    if (!depth_shown) {
        if (w != tex_w || h != tex_h) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            SDL_SetTextureScaleMode(texture, scale_mode());
            tex_w = w;
            tex_h = h;
        }
        SDL_UpdateTexture(texture, NULL, pixels, VDP_MAX_W * 4);
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, depth_shown ? depth_texture : texture, NULL, &dst);
    SDL_RenderPresent(renderer);
}

static void cycle_battle_depth(void) {
    int l = (depth_get_level() + 1) % DEPTH_LEVELS;
    depth_set_level(l);
    for (int i = 0; i < NSETTINGS; i++)
        if (!strcmp(settings[i].key, "enhance.battle_depth")) snprintf(settings[i].value, sizeof settings[i].value, "%d", l);
    char path[1100], title[96];
    path_in_home(path, sizeof path, "psiv.ini");
    write_settings(path);
    snprintf(title, sizeof title, "Phantasy Star IV - battle depth: %s", depth_level_name(l));
    SDL_SetWindowTitle(window, title);
    SDL_Log("battle depth: %s", depth_level_name(l));
}

/* ------------------------------------------------------------------ frame and audio */
void platform_audio(const int16_t *samples, int count) {
    if (!audio || count <= 0 || paused) return;
    /* keep latency bounded: if more than ~1/6 s is queued (fast-forward, a
     * stall), drop this frame's audio instead of letting the delay grow */
    if (SDL_GetAudioStreamQueued(audio) > SOUND_RATE * 4 / 6) return;
    SDL_PutAudioStreamData(audio, samples, count * 4);
}

static void handle_events(const uint32_t *pixels, int w, int h) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            quit(0);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_ESCAPE) quit(0);
            if (e.key.key == SDLK_F12) {
                if (depth_shown) screenshot_any(depth_pixels, depth_tex_w, depth_tex_h, depth_tex_w);
                else screenshot_any(pixels, w, h, VDP_MAX_W);
            }
            if (e.key.key == SDLK_F10) paused = !paused;
            if (e.key.key == SDLK_F9) cycle_battle_depth();
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
}

void platform_frame(const uint32_t *pixels, int w, int h) {
    handle_events(pixels, w, h);
    while (paused) {                                     /* F10: hold here, keep the window alive */
        present(pixels, w, h, 0);
        SDL_Delay(16);
        handle_events(pixels, w, h);
        next_deadline = SDL_GetTicksNS();
    }
    bus_pad[0] = read_input();
    present(pixels, w, h, 1);

    uint64_t now = SDL_GetTicksNS();
    if (now - last_sram_flush > 1000000000ull) {
        sram_flush();
        last_sram_flush = now;
    }
    if (held(IN_FF)) {                                   /* fast forward: no pacing */
        next_deadline = now;
        return;
    }
    next_deadline += NTSC_FRAME_NS;
    if (next_deadline > now) SDL_DelayPrecise(next_deadline - now);
    else if (now - next_deadline > 4 * NTSC_FRAME_NS) next_deadline = now;   /* fell behind: resync */
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    const char *rom_arg = NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_arg = argv[++i];

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Phantasy Star IV", SDL_GetError(), NULL);
        return 1;
    }
    find_home();
    open_log();
    load_settings();
    depth_set_level(setting_int("enhance.battle_depth"));
    resolve_bindings();

    int scale = setting_int("video.scale");
    if (scale < 1) scale = 1;
    if (!SDL_CreateWindowAndRenderer("Phantasy Star IV", 320 * scale, 224 * scale,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Phantasy Star IV", SDL_GetError(), NULL);
        return 1;
    }
    SDL_SetRenderVSync(renderer, 0);                    /* paced to 59.92 Hz ourselves */
    if (setting_int("video.fullscreen")) SDL_SetWindowFullscreen(window, true);

    static uint8_t rom[0x400000];
    uint32_t rom_size = 0;
    char path[1100], err[1300];
    if (rom_arg) snprintf(path, sizeof path, "%s", rom_arg);
    else path_in_home(path, sizeof path, "rom/psiv.bin");
    if (!read_rom(path, rom, &rom_size, err, sizeof err)) {
        if (rom_arg || file_exists(path))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Phantasy Star IV", err, window);
        if (!pick_rom(rom, &rom_size)) quit(0);
    }

    /* the game's native rate; SDL resamples to the output device */
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, SOUND_RATE};
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) {
        SDL_SetAudioStreamGain(audio, (float)setting_int("audio.volume") / 100.0f);
        SDL_ResumeAudioStreamDevice(audio);
    } else {
        SDL_Log("no audio: %s", SDL_GetError());
    }

    bus_init(rom, rom_size);
    sram_load();
    next_deadline = SDL_GetTicksNS();
    SDL_DetachThread(SDL_CreateThread(watchdog, "psiv-watchdog", NULL));

    m68k.s = 1;
    m68k.imask = 7;
    A(7) = rd32(0);
    diag_stack_top = A(7);
    m68k_call(rd32(4));                                /* the game never returns from reset */
    quit(1);
    return 1;
}
