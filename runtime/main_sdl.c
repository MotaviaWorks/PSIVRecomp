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
#include "hdtext.h"
#include "sfx.h"
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
    {"video.widescreen", "1", "16:9 widescreen in the field and in battles (other screens keep their shape): 0 off, 1 on"},
    {"video.aspect", "square", "square = square pixels; 4:3 = the console's TV shape (pixels a bit narrower)"},
    {"video.filter", "nearest", "nearest = sharp pixels; linear = smoothed"},
    {"audio.volume", "100", "0-200 percent"},
    {"enhance.mode", "enhanced", "enhanced = the port's additions below are on; classic = the original game, unchanged (Esc menu > Settings)"},
    {"enhance.battle_depth", "1", "battles drawn as layers at different depths under a gently moving camera: 0 off, 1 subtle, 2 strong (F9 cycles)"},
    {"enhance.battle_fog", "1", "drifting mist between the enemies and the party in battles: 0 off, 1 light, 2 heavy (F8 cycles)"},
    {"enhance.text", "modern", "text: modern = redrawn with the font below at full resolution; original = the game's pixel font"},
    {"enhance.text_font", "C:/Windows/Fonts/segoeui.ttf", "TrueType/OpenType font for modern text"},
    {"enhance.sfx", "modern", "sound effects: modern = your samples, mapped in sfx/sfx.ini; original = the game's own"},
    {"enhance.battle_fx", "1", "attack and technique effects light the scene; sparks, flashes, embers, lens flares: 0 off, 1 subtle, 2 full"},
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

/* testing hooks (see main) */
static uint64_t test_menu_frame, test_capture_frame;
static int test_menu_settings;
static const char *test_capture_path;
static void quit(int code);

/* the system menu (below) draws over the frame while it's open */
static int menu_is_open(void);
static void menu_draw(void);
static int classic_mode(void);

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
        if (hdtext_enabled() && hdtext_has_text(frame_lines, w, h)) {        /* modern text */
            if (!depth_shown) hdtext_upscale(pixels, VDP_MAX_W, w, h, depth_pixels, w * ds, ds);
            hdtext_draw(frame_lines, w, h, depth_pixels, w * ds, ds);
            depth_shown = 1;
        }
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
    if (menu_is_open()) menu_draw();
    if (test_capture_path && frame_count >= test_capture_frame) {
        SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
        if (shot) { SDL_SaveBMP(shot, test_capture_path); SDL_DestroySurface(shot); }
        SDL_Log("captured %s", test_capture_path);
        quit(0);
    }
    SDL_RenderPresent(renderer);
}

/* store a changed setting and save psiv.ini */
static void save_setting_int(const char *key, int v) {
    for (int i = 0; i < NSETTINGS; i++)
        if (!strcmp(settings[i].key, key)) snprintf(settings[i].value, sizeof settings[i].value, "%d", v);
    char path[1100];
    path_in_home(path, sizeof path, "psiv.ini");
    write_settings(path);
}

static void show_enhancements(void) {
    char title[128];
    snprintf(title, sizeof title, "Phantasy Star IV - battle depth: %s, fog: %s",
             depth_level_name(depth_get_level()), depth_fog_name(depth_get_fog()));
    SDL_SetWindowTitle(window, title);
    SDL_Log("%s", title);
}

static int classic_hotkey(void) {
    if (!classic_mode()) return 0;
    SDL_SetWindowTitle(window, "Phantasy Star IV - Classic mode: additions are off (Esc > Settings > Mode)");
    return 1;
}

static void cycle_battle_depth(void) {
    if (classic_hotkey()) return;
    int l = (depth_get_level() + 1) % DEPTH_LEVELS;
    depth_set_level(l);
    save_setting_int("enhance.battle_depth", l);
    show_enhancements();
}

static void cycle_battle_fog(void) {
    if (classic_hotkey()) return;
    int l = (depth_get_fog() + 1) % FOG_LEVELS;
    depth_set_fog(l);
    save_setting_int("enhance.battle_fog", l);
    show_enhancements();
}

/* ------------------------------------------------------------------ system menu (Esc) */
/* An overlay drawn over the paused game: resume, settings, quit. Settings
 * apply at once and are saved to psiv.ini. Text uses SDL's built-in 8x8
 * font for now. */
static int classic_mode(void) { return !strcmp(setting("enhance.mode"), "classic"); }

/* widescreen margin for the current settings: 16:9 is 400 pixels wide with
 * square pixels, 424 with the 4:3 TV pixel shape */
static int wide_margin(void) {
    if (classic_mode() || !setting_int("video.widescreen")) return 0;
    return strcmp(setting("video.aspect"), "4:3") ? 40 : 52;
}

/* keep the window's height and give it the frame's shape */
static void fit_window(void) {
    if (!window || SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) return;
    int ww, wh;
    SDL_GetWindowSize(window, &ww, &wh);
    float par = strcmp(setting("video.aspect"), "4:3") ? 1.0f : (4.0f / 3.0f) / (320.0f / 224.0f);
    float fw = (float)(320 + 2 * wide_margin()) * par;
    SDL_SetWindowSize(window, (int)lrintf((float)wh * fw / 224.0f), wh);
}

/* what the enhancement settings mean right now (Classic turns them all off) */
static void apply_enhancements(void) {
    int m = wide_margin();
    if (m != frame_wide_margin) { frame_wide_margin = m; fit_window(); }
    depth_set_level(classic_mode() ? 0 : setting_int("enhance.battle_depth"));
    depth_set_fog(classic_mode() ? 0 : setting_int("enhance.battle_fog"));
    depth_set_fx(classic_mode() ? 0 : setting_int("enhance.battle_fx"));
    sfx_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.sfx"), "original") != 0);
    hdtext_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.text"), "original") != 0);
}

static void apply_video(void) {
    if (texture) SDL_SetTextureScaleMode(texture, scale_mode());
    if (depth_texture) SDL_SetTextureScaleMode(depth_texture, scale_mode());
    SDL_SetWindowFullscreen(window, setting_int("video.fullscreen") != 0);
}

static void apply_audio(void) {
    if (audio) SDL_SetAudioStreamGain(audio, (float)setting_int("audio.volume") / 100.0f);
}

typedef struct {
    const char *label;
    const char *key;                  /* setting changed with left/right (NULL: an action) */
    const char *values[4];            /* setting values, and how they're shown */
    const char *names[4];
    int lo, hi, step;                 /* numeric setting instead of a list */
    int enhancement;                  /* greyed out in Classic mode */
    const char *coming;               /* not built yet: shown greyed with this note */
    void (*apply)(void);
} MenuItem;

static const MenuItem MAIN_ITEMS[] = {
    {"Resume", NULL, {0}, {0}, 0, 0, 0, 0, NULL, NULL},
    {"Settings", NULL, {0}, {0}, 0, 0, 0, 0, NULL, NULL},
    {"Quit game", NULL, {0}, {0}, 0, 0, 0, 0, NULL, NULL},
};
static const MenuItem SETTINGS_ITEMS[] = {
    {"Mode", "enhance.mode", {"enhanced", "classic"}, {"Enhanced", "Classic"}, 0, 0, 0, 0, NULL, apply_enhancements},
    {"Battle depth", "enhance.battle_depth", {"0", "1", "2"}, {"Off", "Subtle", "Strong"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Battle fog", "enhance.battle_fog", {"0", "1", "2"}, {"Off", "Light", "Heavy"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Battle effects", "enhance.battle_fx", {"0", "1", "2"}, {"Off", "Subtle", "Full"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Sound effects", "enhance.sfx", {"modern", "original"}, {"Modern", "Original"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Text", "enhance.text", {"modern", "original"}, {"Modern", "Original"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Widescreen", "video.widescreen", {"0", "1"}, {"Off", "16:9"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Aspect", "video.aspect", {"square", "4:3"}, {"Square pixels", "4:3 TV"}, 0, 0, 0, 0, NULL, apply_enhancements},
    {"Pixels", "video.filter", {"nearest", "linear"}, {"Sharp", "Smooth"}, 0, 0, 0, 0, NULL, apply_video},
    {"Integer scaling", "video.integer_scaling", {"1", "0"}, {"On", "Off"}, 0, 0, 0, 0, NULL, NULL},
    {"Fullscreen", "video.fullscreen", {"0", "1"}, {"Off", "On"}, 0, 0, 0, 0, NULL, apply_video},
    {"Volume", "audio.volume", {0}, {0}, 0, 200, 10, 0, NULL, apply_audio},
    {"Back", NULL, {0}, {0}, 0, 0, 0, 0, NULL, NULL},
};
#define N_MAIN     (int)(sizeof MAIN_ITEMS / sizeof MAIN_ITEMS[0])
#define N_SETTINGS (int)(sizeof SETTINGS_ITEMS / sizeof SETTINGS_ITEMS[0])

enum { MENU_CLOSED, MENU_MAIN, MENU_SETTINGS, MENU_QUIT };
static int menu_screen, menu_sel;

static int item_values(const MenuItem *it) {
    int n = 0;
    while (n < 4 && it->values[n]) n++;
    return n;
}

static int item_disabled(const MenuItem *it) {
    return it->coming || (it->enhancement && classic_mode());
}

static void item_value_text(const MenuItem *it, char *buf, size_t n) {
    buf[0] = 0;
    if (it->coming) { snprintf(buf, n, "%s", it->coming); return; }
    if (!it->key) return;
    if (it->step) { snprintf(buf, n, "< %d%% >", setting_int(it->key)); return; }
    const char *v = setting(it->key);
    for (int i = 0; i < item_values(it); i++)
        if (!SDL_strcasecmp(v, it->values[i])) { snprintf(buf, n, "< %s >", it->names[i]); return; }
    snprintf(buf, n, "< %s >", v);
}

static void item_change(const MenuItem *it, int dir) {
    if (!it->key || item_disabled(it)) return;
    char path[1100];
    for (int i = 0; i < NSETTINGS; i++) {
        if (strcmp(settings[i].key, it->key)) continue;
        if (it->step) {
            int v = atoi(settings[i].value) + dir * it->step;
            v = v < it->lo ? it->lo : v > it->hi ? it->hi : v;
            snprintf(settings[i].value, sizeof settings[i].value, "%d", v);
        } else {
            int n = item_values(it), cur = 0;
            for (int j = 0; j < n; j++)
                if (!SDL_strcasecmp(settings[i].value, it->values[j])) cur = j;
            cur = (cur + dir + n) % n;
            snprintf(settings[i].value, sizeof settings[i].value, "%s", it->values[cur]);
        }
    }
    path_in_home(path, sizeof path, "psiv.ini");
    write_settings(path);
    if (it->apply) it->apply();
}

/* after the menu closes, the game sees no buttons until all are released, so
 * the key that closed it (Enter = Start) doesn't also reach the game */
static int input_hold;
static void menu_open(void) { menu_screen = MENU_MAIN; menu_sel = 0; }
static void menu_close(void) { menu_screen = MENU_CLOSED; input_hold = 1; }
static int menu_is_open(void) { return menu_screen != MENU_CLOSED; }

/* one key or button press while the menu is open */
static void menu_input(int up, int down, int left, int right, int ok, int back) {
    int n = menu_screen == MENU_MAIN ? N_MAIN : menu_screen == MENU_SETTINGS ? N_SETTINGS : 2;
    const MenuItem *items = menu_screen == MENU_MAIN ? MAIN_ITEMS : SETTINGS_ITEMS;
    if (up) menu_sel = (menu_sel + n - 1) % n;
    if (down) menu_sel = (menu_sel + 1) % n;
    if (menu_screen == MENU_SETTINGS && (left || right)) item_change(&items[menu_sel], right ? 1 : -1);
    if (menu_screen == MENU_QUIT && (left || right)) menu_sel ^= 1;
    if (back) {
        if (menu_screen == MENU_MAIN) menu_close();
        else { menu_sel = menu_screen == MENU_SETTINGS ? 1 : 2; menu_screen = MENU_MAIN; }
        return;
    }
    if (!ok) return;
    if (menu_screen == MENU_MAIN) {
        if (menu_sel == 0) menu_close();
        else if (menu_sel == 1) { menu_screen = MENU_SETTINGS; menu_sel = 0; }
        else { menu_screen = MENU_QUIT; menu_sel = 1; }               /* default: No */
    } else if (menu_screen == MENU_SETTINGS) {
        if (menu_sel == N_SETTINGS - 1) { menu_screen = MENU_MAIN; menu_sel = 1; }
        else item_change(&items[menu_sel], 1);
    } else {
        if (menu_sel == 0) quit(0);
        menu_screen = MENU_MAIN;
        menu_sel = 2;
    }
}

static void menu_text(float x, float y, const char *s, Uint8 r, Uint8 g, Uint8 b) {
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderDebugText(renderer, x, y, s);
}

static void menu_draw(void) {
    int ow, oh;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    float k = floorf((float)oh / 300.0f);                  /* text scale: whole pixels */
    if (k < 1) k = 1;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);        /* dim the game */
    SDL_FRect all = {0, 0, (float)ow, (float)oh};
    SDL_RenderFillRect(renderer, &all);
    SDL_SetRenderScale(renderer, k, k);
    float vw = (float)ow / k, vh = (float)oh / k;

    const char *title = menu_screen == MENU_SETTINGS ? "SETTINGS" : menu_screen == MENU_QUIT ? "QUIT GAME?" : "SYSTEM";
    int n = menu_screen == MENU_MAIN ? N_MAIN : menu_screen == MENU_SETTINGS ? N_SETTINGS : 2;
    float pw = menu_screen == MENU_SETTINGS ? 340.0f : 220.0f, row = 14.0f;
    float ph = 34.0f + row * (float)n + 22.0f;
    float px = floorf((vw - pw) / 2), py = floorf((vh - ph) / 2);
    SDL_FRect panel = {px, py, pw, ph};
    SDL_SetRenderDrawColor(renderer, 0x00, 0x24, 0x6D, 235);   /* the game's window blue */
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 0xB0, 0xB8, 0xE8, 255);
    SDL_RenderRect(renderer, &panel);
    SDL_FRect inner = {px + 2, py + 2, pw - 4, ph - 4};
    SDL_SetRenderDrawColor(renderer, 0x50, 0x60, 0xA8, 255);
    SDL_RenderRect(renderer, &inner);
    menu_text(px + (pw - 8.0f * (float)strlen(title)) / 2, py + 10, title, 0xFF, 0xFF, 0xFF);

    for (int i = 0; i < n; i++) {
        float y = py + 30 + row * (float)i;
        int sel = i == menu_sel;
        char line[96], value[64];
        if (menu_screen == MENU_QUIT) {
            snprintf(line, sizeof line, "%s", i == 0 ? "Yes, quit" : "No, keep playing");
            value[0] = 0;
        } else {
            const MenuItem *it = (menu_screen == MENU_MAIN ? MAIN_ITEMS : SETTINGS_ITEMS) + i;
            snprintf(line, sizeof line, "%s", it->label);
            item_value_text(it, value, sizeof value);
            if (item_disabled(it)) {
                menu_text(px + 18, y, line, 0x70, 0x78, 0x98);
                menu_text(px + 150, y, value, 0x70, 0x78, 0x98);
                if (sel) menu_text(px + 8, y, ">", 0xFF, 0xD0, 0x40);
                continue;
            }
        }
        if (sel) menu_text(px + 8, y, ">", 0xFF, 0xD0, 0x40);
        menu_text(px + 18, y, line, sel ? 0xFF : 0xE0, sel ? 0xD0 : 0xE0, sel ? 0x40 : 0xF0);
        if (value[0]) menu_text(px + 150, y, value, sel ? 0xFF : 0xC0, sel ? 0xE8 : 0xC8, sel ? 0x90 : 0xE8);
    }
    const char *hint = menu_screen == MENU_SETTINGS ? "Left/Right change   Esc back" : "Enter select   Esc back";
    menu_text(px + (pw - 8.0f * (float)strlen(hint)) / 2, py + ph - 14, hint, 0x90, 0x98, 0xC0);
    SDL_SetRenderScale(renderer, 1, 1);
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
        case SDL_EVENT_KEY_DOWN: {
            SDL_Scancode sc = e.key.scancode;
            if (menu_is_open()) {                        /* the menu takes the keyboard */
                int ok = sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_SPACE
                         || sc == key_of[IN_C] || sc == key_of[IN_START];
                int back = sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_BACKSPACE || sc == key_of[IN_B];
                menu_input(sc == SDL_SCANCODE_UP || sc == key_of[IN_UP], sc == SDL_SCANCODE_DOWN || sc == key_of[IN_DOWN],
                           sc == SDL_SCANCODE_LEFT || sc == key_of[IN_LEFT], sc == SDL_SCANCODE_RIGHT || sc == key_of[IN_RIGHT],
                           ok && !e.key.repeat, back && !e.key.repeat);
                break;
            }
            if (e.key.repeat) break;
            if (e.key.key == SDLK_ESCAPE) { menu_open(); break; }
            if (e.key.key == SDLK_F12) {
                if (depth_shown) screenshot_any(depth_pixels, depth_tex_w, depth_tex_h, depth_tex_w);
                else screenshot_any(pixels, w, h, VDP_MAX_W);
            }
            if (e.key.key == SDLK_F10) paused = !paused;
            if (e.key.key == SDLK_F9) cycle_battle_depth();
            if (e.key.key == SDLK_F8) cycle_battle_fog();
            if (e.key.key == SDLK_F11) {
                int fs = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(window, fs);
                save_setting_int("video.fullscreen", fs);
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            SDL_GamepadButton b = (SDL_GamepadButton)e.gbutton.button;
            if (b == SDL_GAMEPAD_BUTTON_BACK || b == SDL_GAMEPAD_BUTTON_GUIDE) {   /* Select/Back opens it */
                if (menu_is_open()) menu_close(); else menu_open();
                break;
            }
            if (!menu_is_open()) break;
            menu_input(b == SDL_GAMEPAD_BUTTON_DPAD_UP, b == SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                       b == SDL_GAMEPAD_BUTTON_DPAD_LEFT, b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
                       b == pad_of[IN_C] || b == pad_of[IN_START], b == pad_of[IN_B]);
            break;
        }
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
    if (test_menu_frame && frame_count == test_menu_frame) {
        menu_open();
        if (test_menu_settings) { menu_screen = MENU_SETTINGS; menu_sel = 1; }
    }
    while (paused || menu_is_open()) {                   /* F10 or the menu: hold here, keep the window alive */
        present(pixels, w, h, 0);
        SDL_Delay(16);
        handle_events(pixels, w, h);
        next_deadline = SDL_GetTicksNS();
    }
    bus_pad[0] = read_input();
    if (input_hold) {
        if (bus_pad[0]) bus_pad[0] = 0;
        else input_hold = 0;
    }
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_arg = argv[++i];
        /* testing: --menu F[:settings] opens the system menu at frame F;
         * --capture F:FILE.bmp saves the rendered window at frame F and exits
         * (run with SDL_VIDEO_DRIVER=offscreen to keep it off the screen) */
        else if (!strcmp(argv[i], "--menu") && i + 1 < argc) {
            char *e;
            test_menu_frame = strtoull(argv[++i], &e, 10);
            test_menu_settings = *e == ':';
        } else if (!strcmp(argv[i], "--capture") && i + 1 < argc) {
            char *e;
            test_capture_frame = strtoull(argv[++i], &e, 10);
            test_capture_path = *e == ':' ? e + 1 : "capture.bmp";
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Phantasy Star IV", SDL_GetError(), NULL);
        return 1;
    }
    find_home();
    open_log();
    load_settings();
    {
        char dir[1100];
        path_in_home(dir, sizeof dir, "sfx");
        SDL_Log("sound effects: %d replaced (%s/sfx.ini)", sfx_load(dir), dir);
    }
    if (!hdtext_init(setting("enhance.text_font")))
        SDL_Log("modern text: font %s not loaded; text stays original", setting("enhance.text_font"));
    apply_enhancements();
    resolve_bindings();

    int scale = setting_int("video.scale");
    if (scale < 1) scale = 1;
    if (!SDL_CreateWindowAndRenderer("Phantasy Star IV", (320 + 2 * wide_margin()) * scale, 224 * scale,
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
