/*
 * SDL3 front end: window, input, audio, 60 Hz pacing, battery saves,
 * settings, crash and hang reports.
 *
 *   psiv [--rom PATH] [--save FILE]
 *
 * Files live in the "home" folder:
 *   - the folder holding the program, when it can be written to (the Windows
 *     zip, the Linux tarball: everything stays together)
 *   - otherwise your data folder: %APPDATA%\PSIV Recomp, ~/.local/share/
 *     psiv-recomp, ~/Library/Application Support/PSIV Recomp
 *   - Android: Android/data/com.psilok.psiv/files on the shared storage
 *     (reachable over USB, for logs, music and samples)
 *   - a development build in build/native-*: the project root
 * There:
 *   psiv.ini         settings (written with defaults on first run)
 *   rom/psiv.bin     the ROM. If missing, a dump dropped into the home
 *                    folder (or rom/) installs itself; otherwise a file
 *                    dialog asks for it. Either way it's checked (.smd or
 *                    raw, or either in a .zip: unzip.h) and kept here as a
 *                    raw copy
 *   saves/psiv.srm   battery save (the cartridge's 8 KB); --save FILE plays
 *                    another, an emulator's .srm too (BizHawk, Gens, Kega
 *                    Fusion: written back in its own layout)
 *   screenshots/     F12
 *   psiv-crash.txt   written if the game hits an error or stops responding
 *
 * Default keys: arrows = D-pad, Z/X/C = A/B/C, Enter = Start, Tab (hold) =
 * fast forward, F10 = pause, F11 = fullscreen, F12 = screenshot, Esc = quit.
 * Gamepads: D-pad or left stick; x/a/b (west/south/east) = A/B/C; start.
 * Mouse: click on the field to walk there (a person or a chest: walk up to
 * it and press C); right click stops. With text up, click = C, right = B.
 * Touch (touch.h): on-screen pad and buttons; a tap elsewhere is a click.
 * Android's Back button opens the system menu.
 * Rumble (rumble.h): on the gamepad, or a phone's vibration motor.
 */
#include "bus.h"
#include "depth.h"
#include "hdtext.h"
#include "panels.h"
#include "battleui.h"
#include "algol.h"
#include "music.h"
#include "fpview.h"
#include "walkto.h"
#include "rumble.h"
#ifndef _WIN32
#include "icon.h"
#endif
#include "sfx.h"
#include "diag.h"
#include "frame.h"
#include "m68k.h"
#include "sha256.h"
#include "sound.h"
#include "touch.h"
#include "unzip.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTSC_FRAME_NS 16688154ull     /* 1 / 59.922751 Hz */
#define HANG_SECONDS 8

#ifndef PSIV_VERSION                  /* set by CMake: version + commit */
#define PSIV_VERSION "dev"
#endif
#define APP_NAME "PSIV Recomp"
#if defined(SDL_PLATFORM_LINUX) && !defined(SDL_PLATFORM_ANDROID)
#define PREF_NAME "psiv-recomp"       /* ~/.local/share/psiv-recomp */
#else
#define PREF_NAME APP_NAME
#endif
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
#define MOBILE 1
#define INTEGER_SCALING_DEFAULT "0"   /* a phone or handheld: fill the screen */
#else
#define MOBILE 0
#define INTEGER_SCALING_DEFAULT "1"
#endif

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
static char home[1024];               /* written to: settings, ROM, saves, logs */
static char base_dir[1024];           /* shipped with the program, read-only: fonts (Android: the APK's assets) */

/* ------------------------------------------------------------------ paths */
static void path_in_home(char *out, size_t n, const char *rel) {
    snprintf(out, n, "%s%s", home, rel);
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

#if !defined(SDL_PLATFORM_ANDROID)
static int dir_writable(const char *dir) {
    char probe[1100];
    snprintf(probe, sizeof probe, "%s.psiv-write-test", dir);
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}
#endif

static void find_home(void) {
    const char *base = SDL_GetBasePath();
#if defined(SDL_PLATFORM_ANDROID)
    base_dir[0] = 0;                                     /* relative paths reach the APK's assets */
#else
    snprintf(base_dir, sizeof base_dir, "%s", base ? base : "");
#endif
    snprintf(home, sizeof home, "%s", base ? base : "./");
    /* development build: build/native-<config>/psiv.exe -> project root */
    char probe[1100];
    snprintf(probe, sizeof probe, "%s../../CMakeLists.txt", home);
    if (file_exists(probe)) {
        size_t len = strlen(home);
        snprintf(home + len, sizeof home - len, "../../");
        return;
    }
#if defined(SDL_PLATFORM_ANDROID)
    const char *ext = SDL_GetAndroidExternalStoragePath();   /* Android/data/<package>/files */
    if (ext) { snprintf(home, sizeof home, "%s/", ext); return; }
#else
    /* portable: everything next to the program, if that folder takes writes
     * (not inside a macOS app bundle) */
    if (base && !strstr(base, ".app/Contents/") && dir_writable(home)) return;
#endif
    char *pref = SDL_GetPrefPath("", PREF_NAME);
    if (pref) {
        snprintf(home, sizeof home, "%s", pref);
        SDL_free(pref);
    }
}

/* a message box -- or, testing with --no-dialogs, a line in the log */
static int no_dialogs;
static void message(SDL_MessageBoxFlags kind, const char *text, SDL_Window *parent) {
    SDL_Log("%s: %s", kind == SDL_MESSAGEBOX_ERROR ? "error" : kind == SDL_MESSAGEBOX_WARNING ? "warning" : "note", text);
    if (!no_dialogs) SDL_ShowSimpleMessageBox(kind, "Phantasy Star IV", text, parent);
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
typedef struct { const char *key; char value[300]; const char *comment; } Setting;   /* room for a path */
static Setting settings[] = {
    {"video.scale", "3", "window size at start, in multiples of the game's resolution"},
    {"video.fullscreen", "0", "1 = start fullscreen (F11 toggles)"},
    {"video.integer_scaling", INTEGER_SCALING_DEFAULT, "1 = whole-number scaling only (sharpest); 0 = fill the screen"},
    {"video.widescreen", "1", "16:9 widescreen (other screens keep their shape): 0 off, 1 more map in exploration, 2 side panels with the party in exploration; battles are wide either way"},
    {"video.aspect", "square", "square = square pixels; 4:3 = the console's TV shape (pixels a bit narrower)"},
    {"video.filter", "nearest", "nearest = sharp pixels; linear = smoothed"},
    {"audio.volume", "100", "0-200 percent"},
    {"enhance.mode", "enhanced", "enhanced = the port's additions below are on; classic = the original game, unchanged (Esc menu > Settings)"},
    {"enhance.battle_depth", "1", "battles drawn as layers at different depths under a gently moving camera: 0 off, 1 subtle, 2 strong (F9 cycles)"},
    {"enhance.battle_fog", "1", "drifting mist between the enemies and the party in battles: 0 off, 1 light, 2 heavy (F8 cycles)"},
    {"enhance.text", "modern", "text: modern = redrawn with the font below at full resolution; original = the game's pixel font"},
    {"enhance.text_font", "", "TrueType/OpenType font file for modern text; empty = Segoe UI on Windows, else the bundled Noto Sans"},
    {"enhance.sfx", "modern", "sound effects: modern = the port's own, synthesized; custom = your samples, mapped in sfx/sfx.ini (the rest modern); original = the game's own"},
    {"enhance.music", "original", "music: algol = the port's modern sound chip plays the game's music (new instruments, drums and reverb); original = the YM2612 and PSG"},
    {"enhance.first_person", "off", "dungeons: first_person = seen from the leader's eyes, like Phantasy Star I's mazes (up walks forward, left / right turn); off = the game's own view"},
    {"enhance.click_walk", "on", "mouse: on = a click on the field walks the party there (a person or a chest: up to it, then C to talk or open); right click stops; with text up a click is C, a right click B; touch: a tap is a click; off = clicks and taps only work the system menu"},
    {"enhance.custom_music", "on", "your music: on = audio files in the music folder play in place of the game's songs (music/8F.wav, or named in music/music.ini; see runtime/music.h); off = the game's"},
    {"enhance.music_bank", "", "with Algol: a SNES game you own whose instruments play the music instead of Algol's (the path to its ROM, e.g. Lufia II); empty = Algol's own"},
    {"enhance.battle_ui", "modern", "battle menus: modern = full names, descriptions and a party bar with portraits; original = the game's windows"},
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
    {"pad.rumble", "normal", "rumble: off, light, normal, strong -- blows landing, critical hits, enemies destroyed, the field's doors and explosions; on the gamepad, or a phone's vibration motor"},
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
    char path[1100], line[600], section[32] = "";
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
static const char *save_arg;                /* --save FILE: this battery save instead of saves/psiv.srm */
static int sram_format = SRAM_PORT;         /* the file's own layout, kept when it's written back */

static void sram_path(char *out, size_t n) {
    if (save_arg) snprintf(out, n, "%s", save_arg);
    else path_in_home(out, n, "saves/psiv.srm");
}

static void sram_load(void) {
    char p[1100];
    sram_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    static uint8_t buf[2 * BUS_SRAM_SIZE];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    sram_format = bus_sram_import(buf, n);
    SDL_Log("loaded %s (%u bytes, %s layout)", p, (unsigned)n, bus_sram_format_name(sram_format));
}

static void sram_flush(void) {
    if (!bus_sram_dirty) return;
    char p[1100], dir[1100];
    if (!save_arg) {
        path_in_home(dir, sizeof dir, "saves");
        SDL_CreateDirectory(dir);
    }
    sram_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) { SDL_Log("cannot write %s", p); return; }
    static uint8_t buf[2 * BUS_SRAM_SIZE];
    fwrite(buf, 1, bus_sram_export(sram_format, buf), f);
    fclose(f);
    bus_sram_dirty = 0;
}

static void quit(int code) {
    sram_flush();
    SDL_Quit();
    exit(code);
}

/* Phones: the app can be sent to the background, and killed there, at any
 * moment. Save then (SDL calls this on the game's thread, at a frame
 * boundary), and tell the watchdog the pause is no hang. */
static volatile int backgrounded;
static void rumble_off(void);
static bool SDLCALL app_events(void *ud, SDL_Event *e) {
    (void)ud;
    switch (e->type) {
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_LOW_MEMORY:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        backgrounded = e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || backgrounded;
        sram_flush();
        if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) rumble_off();
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        backgrounded = 0;
        next_deadline = SDL_GetTicksNS();                /* no catching up on the time away */
        break;
    default:
        break;
    }
    return true;
}

/* ------------------------------------------------------------------ crash and hang reports */
static void write_report(const char *kind, const char *text, char *path_out, size_t n) {
    path_in_home(path_out, n, "psiv-crash.txt");
    FILE *f = fopen(path_out, "w");
    if (!f) return;
    fprintf(f, "Phantasy Star IV port: %s\n%s %s on %s\nframe %llu\n\n%s\n", kind, APP_NAME, PSIV_VERSION,
            SDL_GetPlatform(), (unsigned long long)frame_count, text);
    fprintf(f, "\nPlease send this file along with what you were doing in the game.\n");
    fclose(f);
}

void platform_fatal(const char *text) {
    char path[1100], msg[1400];
    write_report("fatal error", text, path, sizeof path);
    snprintf(msg, sizeof msg,
             "The game hit an error the port can't continue from.\n\nA report was saved to:\n%s\n\n%.400s",
             path, text);
    message(SDL_MESSAGEBOX_ERROR, msg, window);
    quit(1);
}

/* Watchdog: a frame normally arrives every 1/60 s. If none comes for
 * HANG_SECONDS while not paused, the game is stuck in a loop: report where.
 * The game held on purpose -- F10, or the system menu open (`holding`) -- is
 * no hang, and neither is the app in the background on a phone. */
static volatile int holding;

static int SDLCALL watchdog(void *unused) {
    (void)unused;
    uint64_t last = frame_count, since = SDL_GetTicks();
    int reported = 0;
    for (;;) {
        SDL_Delay(500);
        uint64_t now_frames = *(volatile uint64_t *)&frame_count;
        if (now_frames != last || paused || holding || backgrounded) {
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
            message(SDL_MESSAGEBOX_WARNING, msg, NULL);
            reported = 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ ROM */
/* `name` is how the file is called in a message: a path, or "That file" for
 * the picker's answer (Android's is a content:// URI) */
static int read_rom_named(const char *path, const char *name, uint8_t *rom, uint32_t *size, char *err, size_t errn) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");      /* any path SDL opens: also Android's content:// */
    if (!io) { snprintf(err, errn, "%s can't be opened: %s", name, SDL_GetError()); return 0; }
    static uint8_t file[0x800000], unpacked[0x400200];
    size_t n = 0, got;
    while (n < sizeof file && (got = SDL_ReadIO(io, file + n, sizeof file - n)) > 0) n += got;
    SDL_CloseIO(io);
    const uint8_t *raw = file;
    int zipped = unzip_is_zip(file, n);
    if (zipped) {                                        /* most ROM sets are zipped: the ROM inside */
        char why[200];
        n = unzip_rom(file, n, unpacked, sizeof unpacked, why, sizeof why);
        if (!n) {
            snprintf(err, errn, "%s can't be used: %s.", name, why);
            SDL_Log("rom: %s: %s", path, why);
            return 0;
        }
        raw = unpacked;
    }
    if (n >= 6 && !memcmp(raw, "7z\xBC\xAF\x27\x1C", 6)) {
        snprintf(err, errn, "%s is a .7z archive. Extract the ROM from it first, then choose the extracted file.", name);
        return 0;
    }
    int smd = n % 16384 == 512 && raw[8] == 0xAA && raw[9] == 0xBB;
    if ((smd ? n - 512 : n) > 0x400000) {               /* bigger than any cartridge: not this game */
        snprintf(err, errn, "%s is not the Phantasy Star IV (USA) cartridge this port was built for "
                 "(it's far too big).", name);
        return 0;
    }
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
        snprintf(err, errn, "%s is not the Phantasy Star IV (USA) cartridge this port was built for.\n\n"
                 "It needs that exact ROM: a raw dump (.bin, .md, .gen) or an .smd. Other releases and "
                 "modified (hacked or translated) ROMs won't work.\n\n(%u bytes, SHA-256 %.16s...)",
                 name, (unsigned)n, hex);
        SDL_Log("rom: %s rejected (%s, %u bytes, sha256 %s)", path, smd ? "SMD" : "raw", (unsigned)n, hex);
        return 0;
    }
    *size = (uint32_t)n;
    SDL_Log("rom: %s (%s%s, verified)", path, zipped ? "zipped, " : "", smd ? "SMD, de-interleaved" : "raw");
    return 1;
}

static int read_rom(const char *path, uint8_t *rom, uint32_t *size, char *err, size_t errn) {
    return read_rom_named(path, path, rom, size, err, errn);
}

/* keep a raw copy as rom/psiv.bin, so the next start needs nothing */
static void install_rom(const uint8_t *rom, uint32_t size) {
    char dir[1100], out[1100];
    path_in_home(dir, sizeof dir, "rom");
    SDL_CreateDirectory(dir);
    path_in_home(out, sizeof out, "rom/psiv.bin");
    FILE *f = fopen(out, "wb");
    if (f && fwrite(rom, 1, size, f) == size) SDL_Log("rom: installed as %s", out);
    else SDL_Log("rom: cannot write %s; it will be asked for again next time", out);
    if (f) fclose(f);
}

/* first run: a dump dropped into the home folder (or rom/, or beside the
 * program) installs itself -- no dialog needed (handy on a Steam Deck) */
static int find_rom_nearby(uint8_t *rom, uint32_t *size) {
    char dirs[3][1100];
    int nd = 0;
    snprintf(dirs[nd++], sizeof dirs[0], "%s", home);
    path_in_home(dirs[nd++], sizeof dirs[0], "rom/");
    if (base_dir[0] && strcmp(base_dir, home)) snprintf(dirs[nd++], sizeof dirs[0], "%s", base_dir);
    for (int d = 0; d < nd; d++) {
        int count = 0;
        char **names = SDL_GlobDirectory(dirs[d], "*", SDL_GLOB_CASEINSENSITIVE, &count);
        if (!names) continue;
        for (int i = 0; i < count; i++) {
            const char *dot = strrchr(names[i], '.');
            int zip = dot && !SDL_strcasecmp(dot, ".zip");
            if (!dot || (!zip && SDL_strcasecmp(dot, ".bin") && SDL_strcasecmp(dot, ".md") && SDL_strcasecmp(dot, ".gen")
                         && SDL_strcasecmp(dot, ".smd") && SDL_strcasecmp(dot, ".68k")))
                continue;
            char path[1400], err[1500];
            SDL_PathInfo info;
            snprintf(path, sizeof path, "%s%s", dirs[d], names[i]);
            if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE ||
                (zip ? info.size > 0x800000 : info.size != ROM_SIZE && info.size != ROM_SIZE + 512))
                continue;
            if (read_rom(path, rom, size, err, sizeof err)) {
                SDL_free(names);
                SDL_Log("rom: found %s", path);
                return 1;
            }
        }
        SDL_free(names);
    }
    return 0;
}

static volatile int dialog_done;
static char dialog_path[1024], dialog_error[300];
static const char *test_pick;          /* testing: --test-pick FILE answers the file dialog */

static void SDLCALL on_pick(void *ud, const char *const *list, int filter) {
    (void)ud; (void)filter;
    if (!list) snprintf(dialog_error, sizeof dialog_error, "%s", SDL_GetError());   /* no picker could open */
    else if (list[0]) snprintf(dialog_path, sizeof dialog_path, "%s", list[0]);
    dialog_done = 1;
}

/* ask for the dump, check it, keep a raw copy as rom/psiv.bin */
static int pick_rom(uint8_t *rom, uint32_t *size) {
    char err[1500], intro[1600];
    snprintf(intro, sizeof intro,
             "Welcome! This port plays your own copy of Phantasy Star IV (USA), the Mega Drive / Genesis "
             "cartridge, and ships none of the game.\n\n"
             "Choose your ROM file next (.bin, .md, .gen or .smd, or the .zip it came in). It's checked, then "
             "kept here with your saves:\n%srom/psiv.bin\n\n"
             "You can also put the file in that folder before starting the game.", home);
    message(SDL_MESSAGEBOX_INFORMATION, intro, window);
    for (int tries = 0;; tries++) {
        dialog_done = 0;
        dialog_path[0] = dialog_error[0] = 0;
        if (test_pick || no_dialogs) {                       /* testing: the dialog's answer, once (or none) */
            if (!tries && test_pick) snprintf(dialog_path, sizeof dialog_path, "%s", test_pick);
            dialog_done = 1;
        } else {
#if defined(SDL_PLATFORM_ANDROID)
            SDL_ShowOpenFileDialog(on_pick, NULL, window, NULL, 0, NULL, false);   /* ROMs have no MIME type */
#else
            static const SDL_DialogFileFilter filters[] = {{"Mega Drive / Genesis ROM", "zip;smd;bin;md;gen;68k"},
                                                           {"All files", "*"}};
            SDL_ShowOpenFileDialog(on_pick, NULL, window, filters, 2, NULL, false);
#endif
        }
        while (!dialog_done) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT) quit(0);
            SDL_Delay(10);
        }
        if (dialog_error[0]) {                               /* no picker on this system */
            snprintf(err, sizeof err, "A file picker couldn't open (%s).\n\nPut your ROM file in this folder and "
                     "start the game again:\n%s", dialog_error, home);
            message(SDL_MESSAGEBOX_ERROR, err, window);
            return 0;
        }
        if (!dialog_path[0]) return 0;                       /* cancelled */
        if (read_rom_named(dialog_path, "That file", rom, size, err, sizeof err)) break;
        message(SDL_MESSAGEBOX_ERROR, err, window);
    }
    install_rom(rom, *size);
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
static struct { uint64_t frame, len; uint8_t keys; } test_keys[32];   /* --test-keys */
static int n_test_keys;
static uint64_t test_click_frame;                                     /* --test-click */
static float test_click_x, test_click_y;
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
static int panels_on;                  /* widescreen with side panels in exploration */

/* where the last picture went in the window (render coordinates), for clicks */
static SDL_FRect pic_dst;
static int pic_fw, pic_w, pic_h, pic_pm;

/* click to walk: a marker where the party is going */
static void draw_walk_marker(const SDL_FRect *dst, int fw, int pm, int w, int h) {
    float fx, fy;
    int wx, wy, sx, sy;
    if (!walkto_active()) return;
    if (fpview_active()) {
        if (!walkto_target(&wx, &wy) || !fpview_project((float)wx, (float)wy, w, h, &fx, &fy)) return;
    } else {
        if (!walkto_target_screen(&sx, &sy)) return;
        fx = (float)(sx - vdp_view_x0);
        fy = (float)sy;
    }
    float kx = dst->w / (float)fw, ky = dst->h / (float)h;
    float X = dst->x + (fx + (float)pm) * kx, Y = dst->y + fy * ky;
    if (X < dst->x || Y < dst->y || X > dst->x + dst->w || Y > dst->y + dst->h) return;
    float r0 = (7.0f + 1.5f * sinf((float)frame_count * 0.2f)) * ky, line = ky > 1.5f ? ky * 0.6f : 1.0f;
    for (int k = 0; k < 3; k++) {                          /* a diamond, breathing: dark rim, gold, dark rim */
        float r = r0 - (float)(k - 1) * line;
        SDL_FPoint p[5] = {{X, Y - r}, {X + r, Y}, {X, Y + r}, {X - r, Y}, {X, Y - r}};
        if (k == 1) SDL_SetRenderDrawColor(renderer, 0xFF, 0xE0, 0x40, 255);
        else SDL_SetRenderDrawColor(renderer, 0x40, 0x28, 0x00, 255);
        SDL_RenderLines(renderer, p, 5);
    }
}

static void present(const uint32_t *pixels, int w, int h, int compose) {
    int ow, oh;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    /* side panels: exploration keeps its 320 view, the margins show the party */
    int pm = !fpview_active() && panels_on && frame_wide_margin > 0 && w == 320 && panels_field() ? frame_wide_margin : 0;
    int fw = w + 2 * pm;                                 /* the picture's width in game pixels */
    /* pixel aspect: square, or the 4:3 TV shape (the mode's 320x224 pixels
     * are ~0.93 wide), whatever the picture's width */
    float par = strcmp(setting("video.aspect"), "4:3") ? 1.0f : (4.0f / 3.0f) / ((float)vdp_width() / (float)h);
    float sw = (float)fw * par, sh = (float)h;
    float s = fminf((float)ow / sw, (float)oh / sh);
    if (setting_int("video.integer_scaling") && s >= 1.0f) s = floorf(s);
    SDL_FRect dst = {((float)ow - sw * s) / 2, ((float)oh - sh * s) / 2, sw * s, sh * s};
    pic_dst = dst;
    pic_fw = fw;
    pic_w = w;
    pic_h = h;
    pic_pm = pm;

    if (compose) {
        int ds = (int)ceilf(s);                          /* compose at about the display scale */
        if (ds < 1) ds = 1;
        if (ds > 4) ds = 4;
        int stride = fw * ds;
        if (pm) {                                        /* the game in the middle, panels around it */
            uint32_t *mid = depth_pixels + (size_t)pm * (size_t)ds;
            hdtext_upscale(pixels, VDP_MAX_W, w, h, mid, stride, ds);
            if (hdtext_enabled() && hdtext_has_text(frame_lines, w, h)) hdtext_draw(frame_lines, w, h, mid, stride, ds);
            panels_draw(depth_pixels, stride, ds, pm, w, h);
            depth_shown = 1;
        } else {
            depth_shown = fpview_compose(frame_lines, pixels, w, h, depth_pixels, stride, ds)   /* first-person dungeons */
                       || depth_compose(frame_lines, w, h, depth_pixels, stride, ds);
            if (hdtext_enabled() && hdtext_has_text(frame_lines, w, h)) {    /* modern text */
                if (!depth_shown) hdtext_upscale(pixels, VDP_MAX_W, w, h, depth_pixels, stride, ds);
                hdtext_draw(frame_lines, w, h, depth_pixels, stride, ds);
                depth_shown = 1;
            }
            if (battleui_active()) {                     /* modern battle menus */
                if (!depth_shown) hdtext_upscale(pixels, VDP_MAX_W, w, h, depth_pixels, stride, ds);
                battleui_draw(depth_pixels, stride, ds, w, h);
                depth_shown = 1;
            }
        }
        if (depth_shown) {
            if (fw * ds != depth_tex_w || h * ds != depth_tex_h) {
                if (depth_texture) SDL_DestroyTexture(depth_texture);
                depth_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                                  fw * ds, h * ds);
                SDL_SetTextureScaleMode(depth_texture, scale_mode());
                depth_tex_w = fw * ds;
                depth_tex_h = h * ds;
            }
            SDL_UpdateTexture(depth_texture, NULL, depth_pixels, stride * 4);
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
    /* round a picture that doesn't fill the window: the backdrop colour, as a
     * TV's border shows it -- the SEGA screen's blue reaches the window's edges
     * and fades with it */
    uint32_t bd = vdp_color(vdp.reg[7] & 63);
    SDL_SetRenderDrawColor(renderer, (Uint8)(bd >> 16), (Uint8)(bd >> 8), (Uint8)bd, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, depth_shown ? depth_texture : texture, NULL, &dst);
    draw_walk_marker(&dst, fw, pm, w, h);
    touch_draw(renderer, ow, oh);                        /* the on-screen pad, when touch is in use */
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
    panels_on = m > 0 && setting_int("video.widescreen") == 2;
    frame_wide_field = !panels_on;
    depth_set_level(classic_mode() ? 0 : setting_int("enhance.battle_depth"));
    depth_set_fog(classic_mode() ? 0 : setting_int("enhance.battle_fog"));
    depth_set_fx(classic_mode() ? 0 : setting_int("enhance.battle_fx"));
    const char *sm = setting("enhance.sfx");
    sfx_set_mode(classic_mode() || !SDL_strcasecmp(sm, "original") ? SFX_ORIGINAL
                 : !SDL_strcasecmp(sm, "custom") ? SFX_CUSTOM : SFX_MODERN);
    hdtext_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.text"), "original") != 0);
    battleui_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.battle_ui"), "original") != 0);
    algol_set_enabled(!classic_mode() && !SDL_strcasecmp(setting("enhance.music"), "algol"));
    fpview_set_enabled(!classic_mode() && !SDL_strcasecmp(setting("enhance.first_person"), "first_person"));
    walkto_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.click_walk"), "off") != 0);
    /* your own music files (music.h), from the music folder */
    {
        char dir[1100];
        path_in_home(dir, sizeof dir, "music");
        music_set_folder(!classic_mode() && SDL_strcasecmp(setting("enhance.custom_music"), "off") ? dir : NULL);
    }
    /* the instrument bank: decoded from that ROM when the setting changes */
    static char bank_now[1100];
    const char *bank = setting("enhance.music_bank");
    if (strcmp(bank, bank_now)) {
        char path[1100];
        if (!*bank || bank[0] == '/' || bank[0] == '\\' || bank[1] == ':') snprintf(path, sizeof path, "%s", bank);
        else path_in_home(path, sizeof path, bank);
        if (algol_set_bank(path)) SDL_Log("instruments: %s, from %s", algol_bank_name(), path);
        else if (*bank) SDL_Log("instruments: %s is not a game the port knows; Algol's own", path);
        snprintf(bank_now, sizeof bank_now, "%s", bank);
    }
}

static void apply_video(void) {
    if (texture) SDL_SetTextureScaleMode(texture, scale_mode());
    if (depth_texture) SDL_SetTextureScaleMode(depth_texture, scale_mode());
    SDL_SetWindowFullscreen(window, setting_int("video.fullscreen") != 0);
}

static void apply_audio(void) {
    if (audio) SDL_SetAudioStreamGain(audio, (float)setting_int("audio.volume") / 100.0f);
}

static int rumble_setting(void) {
    const char *v = setting("pad.rumble");
    for (int i = 0; i < RUMBLE_LEVELS; i++)
        if (!SDL_strcasecmp(v, rumble_level_name(i))) return i;
    return *v >= '0' && *v <= '3' ? *v - '0' : 2;
}

static void apply_rumble(void) {                  /* changed in the menu: a taste of the new level */
    rumble_set_level(rumble_setting());
    rumble_test();
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
    {"Music", "enhance.music", {"original", "algol"}, {"Original chip", "Algol (modern)"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Dungeons", "enhance.first_person", {"off", "first_person"}, {"Top-down", "First person"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Mouse", "enhance.click_walk", {"on", "off"}, {"Click to walk", "Off"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Your music", "enhance.custom_music", {"on", "off"}, {"On (music folder)", "Off"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Sound effects", "enhance.sfx", {"modern", "custom", "original"}, {"Modern", "Your samples", "Original"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Text", "enhance.text", {"modern", "original"}, {"Modern", "Original"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Battle menus", "enhance.battle_ui", {"modern", "original"}, {"Modern", "Original"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Widescreen", "video.widescreen", {"0", "1", "2"}, {"Off", "More map", "Side panels"}, 0, 0, 0, 1, NULL, apply_enhancements},
    {"Aspect", "video.aspect", {"square", "4:3"}, {"Square pixels", "4:3 TV"}, 0, 0, 0, 0, NULL, apply_enhancements},
    {"Pixels", "video.filter", {"nearest", "linear"}, {"Sharp", "Smooth"}, 0, 0, 0, 0, NULL, apply_video},
    {"Integer scaling", "video.integer_scaling", {"1", "0"}, {"On", "Off"}, 0, 0, 0, 0, NULL, NULL},
    {"Fullscreen", "video.fullscreen", {"0", "1"}, {"Off", "On"}, 0, 0, 0, 0, NULL, apply_video},
    {"Volume", "audio.volume", {0}, {0}, 0, 200, 10, 0, NULL, apply_audio},
    {"Rumble", "pad.rumble", {"off", "light", "normal", "strong"}, {"Off", "Light", "Normal", "Strong"}, 0, 0, 0, 0, NULL, apply_rumble},
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

/* where the menu was last drawn (its own units, k render pixels each), for taps */
static float menu_k = 1, menu_px, menu_py, menu_pw, menu_ph, menu_row = 14;

/* a tap or click on the menu: a line chooses it (a setting steps to its
 * next value, or back on its "<"); outside the panel closes a level */
static void menu_tap(float rx, float ry) {
    float x = rx / menu_k, y = ry / menu_k;
    if (x < menu_px || x > menu_px + menu_pw || y < menu_py || y > menu_py + menu_ph) {
        menu_input(0, 0, 0, 0, 0, 1);
        return;
    }
    int n = menu_screen == MENU_MAIN ? N_MAIN : menu_screen == MENU_SETTINGS ? N_SETTINGS : 2;
    int i = (int)floorf((y - (menu_py + 30 - 3)) / menu_row);
    if (i < 0 || i >= n) return;
    menu_sel = i;
    if (menu_screen == MENU_SETTINGS && i < N_SETTINGS - 1) {
        item_change(&SETTINGS_ITEMS[i], x >= menu_px + 150 && x < menu_px + 150 + 20 ? -1 : 1);
        return;
    }
    menu_input(0, 0, 0, 0, 1, 0);
}

static void menu_draw(void) {
    int ow, oh;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    float k = floorf((float)(ow < oh * 4 / 3 ? ow * 3 / 4 : oh) / 300.0f);   /* text scale: whole pixels */
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
    menu_k = k; menu_px = px; menu_py = py; menu_pw = pw; menu_ph = ph; menu_row = row;
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
    const char *hint = touch_visible() ? (menu_screen == MENU_SETTINGS ? "Tap a line to change it" : "Tap to choose")
                     : menu_screen == MENU_SETTINGS ? "Left/Right change   Esc back" : "Enter select   Esc back";
    menu_text(px + (pw - 8.0f * (float)strlen(hint)) / 2, py + ph - 14, hint, 0x90, 0x98, 0xC0);
    if (menu_screen == MENU_MAIN)                          /* for bug reports */
        menu_text(6, vh - 14, APP_NAME " " PSIV_VERSION, 0x90, 0x98, 0xC0);
    SDL_SetRenderScale(renderer, 1, 1);
}

/* ------------------------------------------------------------------ rumble */
/* rumble.h says what to play; here it goes to the gamepad's two motors, or --
 * a phone, or a handheld whose controls can't rumble -- to the device's own
 * vibration motor, a pulse per effect. */
static SDL_Haptic *vibrator;
static Uint16 sent_lo, sent_hi;
static uint64_t sent_at;

static int pad_rumbles(void) {
    return pad && SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

static void rumble_output(void) {
    uint64_t now = SDL_GetTicks();
    float lo, hi;
    rumble_motors(now, &lo, &hi);
    if (pad && (pad_rumbles() || !vibrator)) {
#if MOBILE
        /* Android restarts the controller's vibrator on every change: fewer
         * steps, and a fading effect updated every 50 ms at most */
        const Uint16 mask = 0xF000;
        int soon = now - sent_at < 50;
#else
        const Uint16 mask = 0xFC00;                  /* 64 steps */
        int soon = 0;
#endif
        Uint16 l = (Uint16)(lo * 65535.0f) & mask, h = (Uint16)(hi * 65535.0f) & mask;
        if (l || h) {
            /* each call runs for its duration (a stall can't leave it on):
             * renewed while the effect lasts; a new blow goes out at once */
            int stronger = l > sent_lo || h > sent_hi;
            if ((l != sent_lo || h != sent_hi || now - sent_at >= 100) && (stronger || !soon)) {
                SDL_RumbleGamepad(pad, l, h, 200);
                sent_lo = l;
                sent_hi = h;
                sent_at = now;
            }
        } else if (sent_lo || sent_hi) {
            SDL_RumbleGamepad(pad, 0, 0, 0);
            sent_lo = sent_hi = 0;
        }
    } else if (vibrator) {
        float s;
        int ms;
        if (rumble_take_pulse(now, &s, &ms)) SDL_PlayHapticRumble(vibrator, s, (Uint32)ms);
    }
}

static void rumble_off(void) {                    /* paused, the menu, the background */
    rumble_stop();
    if (pad && (sent_lo || sent_hi)) SDL_RumbleGamepad(pad, 0, 0, 0);
    sent_lo = sent_hi = 0;
    if (vibrator) SDL_StopHapticRumble(vibrator);
}

/* testing: --test-rumble F attaches a virtual gamepad that logs what it's
 * asked to do, and plays the menu's sample at frame F */
static uint64_t test_rumble_frame;
static bool SDLCALL test_pad_rumble(void *ud, Uint16 lo, Uint16 hi) {
    (void)ud;
    SDL_Log("rumble: test pad %5u %5u at frame %llu", lo, hi, (unsigned long long)frame_count);
    return true;
}

/* ------------------------------------------------------------------ frame and audio */
void platform_audio(const int16_t *samples, int count) {
    if (!audio || count <= 0 || paused) return;
    /* keep latency bounded: if more than ~1/6 s is queued (fast-forward, a
     * stall), drop this frame's audio instead of letting the delay grow */
    if (SDL_GetAudioStreamQueued(audio) > SOUND_RATE * 4 / 6) return;
    SDL_PutAudioStreamData(audio, samples, count * 4);
}

/* a click or a tap at (rx, ry) in render pixels: the system menu, the battle
 * menus, or a walk (walkto.h) */
static void click_at(float rx, float ry, int right) {
    if (menu_is_open()) {
        if (right) menu_input(0, 0, 0, 0, 0, 1);
        else menu_tap(rx, ry);
        return;
    }
    if (paused || !walkto_enabled() || pic_dst.w <= 0) return;
    float gx = (rx - pic_dst.x) / pic_dst.w * (float)pic_fw - (float)pic_pm;   /* the game's frame pixels */
    float gy = (ry - pic_dst.y) / pic_dst.h * (float)pic_h;
    if (battleui_active()) {                       /* the battle menus: pick, or back */
        battleui_click(gx, gy, right);
        return;
    }
    if (right) {                                   /* stop walking; with text up, B */
        if (!walkto_active()) walkto_click_button(1);
        walkto_cancel();
        return;
    }
    if (walkto_click_button(0)) return;            /* text up: C, next */
    if (gx < 0 || gy < 0 || gx >= (float)pic_w || gy >= (float)pic_h) return;   /* off the picture, or a panel */
    int cx, cy, slot;
    if (fpview_active()) {
        if (fpview_pick(gx, gy, pic_w, pic_h, &cx, &cy, &slot)) walkto_go(cx, cy, slot);
    } else walkto_click_screen((int)gx + vdp_view_x0, (int)gy);
}

static uint8_t touch_prev;                         /* the on-screen buttons last time, for the menu's presses */

static void handle_events(const uint32_t *pixels, int w, int h) {
    SDL_Event e;
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            quit(0);
            break;
        case SDL_EVENT_KEY_DOWN: {
            SDL_Scancode sc = e.key.scancode;
            touch_hide();                                /* keys in use: the on-screen pad goes */
            if (menu_is_open()) {                        /* the menu takes the keyboard */
                int ok = sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_SPACE
                         || sc == key_of[IN_C] || sc == key_of[IN_START];
                int back = sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_AC_BACK
                           || sc == key_of[IN_B];
                menu_input(sc == SDL_SCANCODE_UP || sc == key_of[IN_UP], sc == SDL_SCANCODE_DOWN || sc == key_of[IN_DOWN],
                           sc == SDL_SCANCODE_LEFT || sc == key_of[IN_LEFT], sc == SDL_SCANCODE_RIGHT || sc == key_of[IN_RIGHT],
                           ok && !e.key.repeat, back && !e.key.repeat);
                break;
            }
            if (e.key.repeat) break;
            if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK) { menu_open(); break; }   /* Android: Back */
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
            touch_hide();                                /* a gamepad in use: the on-screen pad goes */
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
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {                /* click to walk (walkto.h), the menus */
            if (e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT) break;
            float rx, ry;
            SDL_RenderCoordinatesFromWindow(renderer, e.button.x, e.button.y, &rx, &ry);
            click_at(rx, ry, e.button.button == SDL_BUTTON_RIGHT);
            break;
        }
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {                  /* the on-screen pad; a tap elsewhere clicks */
            float tx, ty;
            if (touch_event(&e, ow, oh, &tx, &ty)) click_at(tx, ty, 0);
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!pad) {
                pad = SDL_OpenGamepad(e.gdevice.which);
                if (pad) SDL_Log("gamepad: %s (rumble: %s)", SDL_GetGamepadName(pad), pad_rumbles() ? "yes" : "no");
            }
            touch_hide();
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = NULL; }
            break;
        }
    }
    /* the on-screen buttons: the menu key, and presses that steer the menu */
    if (touch_take_menu()) {
        if (menu_is_open()) menu_input(0, 0, 0, 0, 0, 1);
        else menu_open();
    }
    uint8_t tp = touch_pad(), pressed = tp & (uint8_t)~touch_prev;
    touch_prev = tp;
    if (menu_is_open() && pressed)
        menu_input(pressed & PAD_UP, pressed & PAD_DOWN, pressed & PAD_LEFT, pressed & PAD_RIGHT,
                   pressed & (PAD_C | PAD_START), pressed & PAD_B);
}

void platform_frame(const uint32_t *pixels, int w, int h) {
    handle_events(pixels, w, h);
    if (test_menu_frame && frame_count == test_menu_frame) {
        menu_open();
        if (test_menu_settings) { menu_screen = MENU_SETTINGS; menu_sel = 1; }
    }
    if (paused || menu_is_open()) rumble_off();          /* the game holds: so does the rumble */
    while (paused || menu_is_open()) {                   /* F10 or the menu: hold here, keep the window alive */
        holding = 1;                                     /* (the watchdog knows it's on purpose) */
        present(pixels, w, h, 0);
        rumble_output();                                 /* the Rumble setting's sample */
        SDL_Delay(16);
        handle_events(pixels, w, h);
        next_deadline = SDL_GetTicksNS();
    }
    holding = 0;
    if (test_rumble_frame && frame_count == test_rumble_frame) rumble_test();
    rumble_output();
    uint8_t user = read_input() | touch_pad();
    for (int i = 0; i < n_test_keys; i++)                /* testing: scripted keys */
        if (frame_count >= test_keys[i].frame && frame_count < test_keys[i].frame + test_keys[i].len) user |= test_keys[i].keys;
    if (test_click_frame && frame_count == test_click_frame && pic_dst.w > 0) {   /* testing: a click on frame pixel X,Y */
        SDL_Event c;
        SDL_zero(c);
        c.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        c.button.button = SDL_BUTTON_LEFT;
        c.button.down = true;
        c.button.windowID = SDL_GetWindowID(window);
        SDL_RenderCoordinatesToWindow(renderer, pic_dst.x + (test_click_x + (float)pic_pm) * pic_dst.w / (float)pic_fw,
                                      pic_dst.y + test_click_y * pic_dst.h / (float)pic_h, &c.button.x, &c.button.y);
        SDL_PushEvent(&c);
    }
    /* first person: the d-pad turns with the view; a click's walk, or its
     * presses in a battle menu, hold it while they run */
    bus_pad[0] = battleui_pad(user, walkto_pad(user, fpview_remap_pad(user)));
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

/* ------------------------------------------------------------------ modern text's font */
static int try_font(const char *path) {
    size_t n = 0;
    void *d = SDL_LoadFile(path, &n);                    /* Android: relative paths read the APK's assets */
    if (!d) return 0;
    void *m = malloc(n);
    if (m) memcpy(m, d, n);
    SDL_free(d);
    if (m && hdtext_init_mem(m, n)) {
        SDL_Log("modern text: %s", path);
        return 1;
    }
    free(m);
    return 0;
}

/* the setting's file; else Segoe UI on Windows; else the Noto Sans shipped
 * beside the program (fonts/, OFL); else a common system font */
static void load_font(void) {
    const char *want = setting("enhance.text_font");
    if (*want) {
        if (try_font(want)) return;
        SDL_Log("modern text: font %s not loaded; trying the defaults", want);
    }
    char bundled[1100];
    snprintf(bundled, sizeof bundled, "%sfonts/NotoSans-Regular.ttf", base_dir);
#ifdef _WIN32
    char winfont[300];
    snprintf(winfont, sizeof winfont, "%s/Fonts/segoeui.ttf", SDL_getenv("WINDIR") ? SDL_getenv("WINDIR") : "C:/Windows");
#endif
    const char *candidates[] = {
#ifdef _WIN32
        winfont,
#endif
        bundled,
#if defined(SDL_PLATFORM_ANDROID)
        "/system/fonts/Roboto-Regular.ttf",
#elif defined(SDL_PLATFORM_APPLE)
        "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
#endif
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++)
        if (try_font(candidates[i])) return;
    SDL_Log("modern text: no font found; text stays original");
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    const char *rom_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_arg = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_arg = argv[++i];
        /* testing: --menu F[:settings] opens the system menu at frame F;
         * --capture F:FILE.bmp saves the rendered window at frame F and exits
         * (run with SDL_VIDEO_DRIVER=offscreen to keep it off the screen);
         * --test-keys F:KEYS:N,... holds keys by frame; --test-click F:X,Y
         * clicks the frame's pixel (X, Y) through the window, as a mouse would */
        else if (!strcmp(argv[i], "--menu") && i + 1 < argc) {
            char *e;
            test_menu_frame = strtoull(argv[++i], &e, 10);
            test_menu_settings = *e == ':';
        } else if (!strcmp(argv[i], "--capture") && i + 1 < argc) {
            char *e;
            test_capture_frame = strtoull(argv[++i], &e, 10);
            test_capture_path = *e == ':' ? e + 1 : "capture.bmp";
        } else if (!strcmp(argv[i], "--test-keys") && i + 1 < argc) {
            /* F:KEYS:N,... -- keys held from frame F for N frames (UDLRABCS) */
            char *s = argv[++i];
            while (*s && n_test_keys < 32) {
                char *e;
                test_keys[n_test_keys].frame = strtoull(s, &e, 10);
                uint8_t k = 0;
                if (*e == ':') e++;
                for (; *e && *e != ':' && *e != ','; e++)
                    k |= *e == 'U' ? PAD_UP : *e == 'D' ? PAD_DOWN : *e == 'L' ? PAD_LEFT : *e == 'R' ? PAD_RIGHT
                       : *e == 'A' ? PAD_A : *e == 'B' ? PAD_B : *e == 'C' ? PAD_C : *e == 'S' ? PAD_START : 0;
                test_keys[n_test_keys].keys = k;
                test_keys[n_test_keys].len = *e == ':' ? strtoull(e + 1, &e, 10) : 1;
                n_test_keys++;
                s = *e == ',' ? e + 1 : e;
            }
        } else if (!strcmp(argv[i], "--test-click") && i + 1 < argc) {
            /* F:X,Y -- a left click at frame F on the game's frame pixel (X, Y) */
            char *e;
            test_click_frame = strtoull(argv[++i], &e, 10);
            if (*e == ':') test_click_x = strtof(e + 1, &e);
            if (*e == ',') test_click_y = strtof(e + 1, &e);
        }
        /* testing the first run: --test-pick FILE answers the file dialog;
         * --no-dialogs writes message boxes to the log instead */
        else if (!strcmp(argv[i], "--test-pick") && i + 1 < argc) test_pick = argv[++i];
        else if (!strcmp(argv[i], "--no-dialogs")) no_dialogs = 1;
        /* testing the rumble: --test-rumble F (see test_pad_rumble) */
        else if (!strcmp(argv[i], "--test-rumble") && i + 1 < argc) test_rumble_frame = strtoull(argv[++i], NULL, 10);
    }

    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");        /* touches are handled as touches (touch.h) */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  /* Back opens the system menu */
    SDL_SetAppMetadata(APP_NAME, PSIV_VERSION, "com.psilok.psiv");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        message(SDL_MESSAGEBOX_ERROR, SDL_GetError(), NULL);
        return 1;
    }
    SDL_AddEventWatch(app_events, NULL);
    find_home();
    open_log();
    SDL_Log("%s %s on %s", APP_NAME, PSIV_VERSION, SDL_GetPlatform());
    load_settings();
    {
        char dir[1100];
        path_in_home(dir, sizeof dir, "sfx");
        SDL_Log("sound effects: %d synthesized; %d of yours (%s/sfx.ini)", sfx_synth_load(), sfx_load(dir), dir);
    }
    load_font();
    apply_enhancements();
    resolve_bindings();
    rumble_set_level(rumble_setting());
#if MOBILE
    /* the device's own vibration motor, for when the controls can't rumble */
    if (SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        int n = 0;
        SDL_HapticID *ids = SDL_GetHaptics(&n);
        if (ids && n > 0 && (vibrator = SDL_OpenHaptic(ids[0])) && !SDL_InitHapticRumble(vibrator)) {
            SDL_CloseHaptic(vibrator);
            vibrator = NULL;
        }
        SDL_free(ids);
    }
    SDL_Log("rumble: %s; vibration motor: %s", rumble_level_name(rumble_get_level()), vibrator ? "yes" : "no");
#else
    SDL_Log("rumble: %s", rumble_level_name(rumble_get_level()));
#endif
    if (test_rumble_frame) {
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.name = "PSIV test pad";
        d.Rumble = test_pad_rumble;
        if (!SDL_AttachVirtualJoystick(&d)) SDL_Log("rumble: no test pad: %s", SDL_GetError());
    }

    int scale = setting_int("video.scale");
    if (scale < 1) scale = 1;
    SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE;
#if MOBILE
    wflags |= SDL_WINDOW_FULLSCREEN;                      /* the whole screen, system bars hidden */
#endif
    if (!SDL_CreateWindowAndRenderer("Phantasy Star IV", (320 + 2 * wide_margin()) * scale, 224 * scale,
                                     wflags, &window, &renderer)) {
        message(SDL_MESSAGEBOX_ERROR, SDL_GetError(), NULL);
        return 1;
    }
    touch_init(MOBILE && !SDL_HasGamepad());             /* a phone starts with the on-screen pad, a handheld without */
    SDL_SetRenderVSync(renderer, 0);                    /* paced to 59.92 Hz ourselves */
#ifndef _WIN32
    {                                                   /* the icon (Windows: the exe's own, res/psiv.rc) */
        SDL_Surface *icon = SDL_CreateSurfaceFrom(PSIV_ICON_SIZE, PSIV_ICON_SIZE, SDL_PIXELFORMAT_ARGB8888,
                                                  (void *)psiv_icon, PSIV_ICON_SIZE * 4);
        if (icon) { SDL_SetWindowIcon(window, icon); SDL_DestroySurface(icon); }
    }
#endif
    if (setting_int("video.fullscreen")) SDL_SetWindowFullscreen(window, true);

    static uint8_t rom[0x400000];
    uint32_t rom_size = 0;
    char path[1100], err[1500];
    if (rom_arg) snprintf(path, sizeof path, "%s", rom_arg);
    else path_in_home(path, sizeof path, "rom/psiv.bin");
    if (!read_rom(path, rom, &rom_size, err, sizeof err)) {
        if (rom_arg || file_exists(path)) message(SDL_MESSAGEBOX_ERROR, err, window);
        if (!rom_arg && find_rom_nearby(rom, &rom_size)) install_rom(rom, rom_size);   /* dropped in the folder */
        else if (!pick_rom(rom, &rom_size)) quit(0);
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
    SDL_Log("side panels: %d portraits from the ROM", panels_init());
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
