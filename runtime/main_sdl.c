/*
 * SDL3 front end: window, input, audio, 60 Hz pacing, battery saves,
 * settings, crash and hang reports.
 *
 *   psiv [--rom PATH] [--save FILE] [--play]
 *
 * It opens on the launcher -- Start game, Enhanced or Classic, Settings,
 * Quit -- unless --play: straight into the game.
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
#include "rewind.h"
#include "weather.h"
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
#include "uikit.h"
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
    {"enhance.rewind", "on", "rewind: Backspace or the left shoulder fades to white and back to about ten seconds before (a state kept every ten seconds, the last two minutes): on / off"},
    {"enhance.battle_weather", "1", "battle weather by place: rain and storms on Motavia's grass, beaches and sea, snow on Dezolis, mist clouds on Rykros, debris in space: 0 off, 1 light, 2 heavy"},
    {"enhance.battle_weather_sound", "on", "the rain and thunder heard with the battle weather: on / off"},
    {"enhance.text", "modern", "text: modern = redrawn with the font below at full resolution; original = the game's pixel font"},
    {"enhance.text_font", "", "TrueType/OpenType font file for modern text; empty = Segoe UI on Windows, else the bundled Noto Sans"},
    {"enhance.sfx", "modern", "sound effects: modern = the port's own, synthesized; custom = your samples, mapped in sfx/sfx.ini (the rest modern); original = the game's own"},
    {"enhance.music", "original", "music: algol = the port's modern sound chip plays the game's music (new instruments, drums and reverb); original = the YM2612 and PSG"},
    {"enhance.click_walk", "on", "mouse: on = a click on the field walks the party there (a person or a chest: up to it, then C to talk or open); right click stops; with text up a click is C, a right click B; touch: a tap is a click; off = clicks and taps only work the system menu"},
    {"enhance.custom_music", "on", "your music: on = audio files in the music folder play in place of the game's songs (music/8F.wav, or named in music/music.ini; see runtime/music.h); off = the game's"},
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
    {"keys.rewind", "Backspace", "go back about ten seconds (enhance.rewind)"},
    {"pad.a", "x", "gamepad buttons use SDL names: a (south), b (east), x (west), y (north), start, back, leftshoulder, rightshoulder"},
    {"pad.b", "a", ""},
    {"pad.c", "b", ""},
    {"pad.start", "start", ""},
    {"pad.fast_forward", "rightshoulder", ""},
    {"pad.rewind", "leftshoulder", ""},
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
enum { IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_A, IN_B, IN_C, IN_START, IN_FF, IN_REWIND, IN_COUNT };
static const char *in_names[IN_COUNT] = {"up", "down", "left", "right", "a", "b", "c", "start", "fast_forward",
                                         "rewind"};
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

static uint64_t test_ff_frame, test_ff_len;            /* --test-ff F:N: fast forward held N frames from F */

static int held(int in) {
    if (in == IN_FF && test_ff_len && frame_count >= test_ff_frame && frame_count < test_ff_frame + test_ff_len) return 1;
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

/* The ROM the game will run: looked for quietly at the start (the installed
 * copy, or a dump dropped in the folder), asked for when the game starts. */
static uint8_t rom_buf[0x400000];
static uint32_t rom_size;
static int rom_ready;                      /* verified and in rom_buf */
static char rom_problem[1500];             /* why the installed one (or --rom) can't be used */
static const char *rom_arg;                /* --rom PATH */

static void rom_look(void) {
    char path[1100];
    if (rom_arg) snprintf(path, sizeof path, "%s", rom_arg);
    else path_in_home(path, sizeof path, "rom/psiv.bin");
    if (read_rom(path, rom_buf, &rom_size, rom_problem, sizeof rom_problem)) { rom_ready = 1; return; }
    if (!rom_arg && !file_exists(path)) rom_problem[0] = 0;          /* just not installed yet */
    if (!rom_arg && find_rom_nearby(rom_buf, &rom_size)) {           /* dropped in the folder */
        install_rom(rom_buf, rom_size);
        rom_ready = 1;
        rom_problem[0] = 0;
    }
}

/* the game is about to start: 1 with a ROM -- what was wrong with the one
 * found, then the file dialog, if need be; 0 if none was chosen */
static int rom_ensure(void) {
    if (rom_ready) return 1;
    if (rom_problem[0]) message(SDL_MESSAGEBOX_ERROR, rom_problem, window);
    if (!pick_rom(rom_buf, &rom_size)) return 0;
    rom_ready = 1;
    rom_problem[0] = 0;
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
static int test_menu_settings, test_menu_page, test_menu_row;
static const char *test_menu_script;                                  /* --menu-script */
static const char *test_shot;                                         /* its shot:FILE, taken at the next present */
static const char *test_capture_path;
static struct { uint64_t frame, len; uint8_t keys; } test_keys[32];   /* --test-keys */
static int n_test_keys;
static uint64_t test_rewind_frame;    /* --test-rewind F: a rewind asked for at frame F */
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

/* testing: --capture's picture (then the end), or a --menu-script shot:, of
 * what's about to be shown */
static void test_shots(void) {
    if (!(test_capture_path && frame_count >= test_capture_frame) && !test_shot) return;
    const char *path = test_shot ? test_shot : test_capture_path;
    SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
    if (shot) { SDL_SaveBMP(shot, path); SDL_DestroySurface(shot); }
    SDL_Log("captured %s", path);
    if (!test_shot) quit(0);
    test_shot = NULL;
}

/* ------------------------------------------------------------------ rewind (rewind.h) */
static char rewind_msg[64];
static uint64_t rewind_msg_until;

static void rewind_now(void) {
    if (rewind_request()) return;
    SDL_snprintf(rewind_msg, sizeof rewind_msg, "%s",
                 rewind_enabled() ? "Nothing to go back to yet" : "Rewind is off (Settings, Game)");
    rewind_msg_until = SDL_GetTicks() + 1800;
}

/* the fade to white over everything, and a note in the corner: how far it
 * went back, or why it didn't */
static void rewind_draw(int ow, int oh) {
    (void)ow;
    float wh = rewind_white();
    if (wh > 0.002f) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, (Uint8)(wh * 255.0f + 0.5f));
        SDL_RenderFillRect(renderer, NULL);
    }
    char text[64] = "";
    int secs = rewind_note_seconds();
    if (secs) SDL_snprintf(text, sizeof text, "Back %d seconds", secs);
    else if (SDL_GetTicks() < rewind_msg_until) SDL_snprintf(text, sizeof text, "%s", rewind_msg);
    if (!*text) return;
    float s = SDL_max(1.0f, SDL_floorf((float)oh / 270.0f));     /* SDL's 8x8 font, a few times over */
    float tw = (float)SDL_strlen(text) * 8.0f * s, x = 14.0f * s, y = 12.0f * s;
    SDL_FRect bg = {x - 6.0f * s, y - 5.0f * s, tw + 12.0f * s, 18.0f * s};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 12, 18, 40, 210);
    SDL_RenderFillRect(renderer, &bg);
    SDL_SetRenderScale(renderer, s, s);
    SDL_SetRenderDrawColor(renderer, 255, 230, 160, 255);
    SDL_RenderDebugText(renderer, x / s, y / s, text);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
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
    rewind_draw(ow, oh);                                 /* a rewind's fade to white, and its note */
    if (menu_is_open()) menu_draw();
    test_shots();
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
    weather_set_sound(SDL_strcasecmp(setting("enhance.battle_weather_sound"), "off") != 0);
    weather_set_level(classic_mode() ? 0 : setting_int("enhance.battle_weather"));
    rewind_set_enabled(SDL_strcasecmp(setting("enhance.rewind"), "off") != 0);
    const char *sm = setting("enhance.sfx");
    sfx_set_mode(classic_mode() || !SDL_strcasecmp(sm, "original") ? SFX_ORIGINAL
                 : !SDL_strcasecmp(sm, "custom") ? SFX_CUSTOM : SFX_MODERN);
    hdtext_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.text"), "original") != 0);
    battleui_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.battle_ui"), "original") != 0);
    algol_set_enabled(!classic_mode() && !SDL_strcasecmp(setting("enhance.music"), "algol"));
    /* first-person dungeons are paused (PsiLok, 2026-11-05: not yet a good way
     * to play): no setting, no menu entry; PSIV_FIRST_PERSON=1 brings the
     * view back for development */
    fpview_set_enabled(!classic_mode() && SDL_getenv("PSIV_FIRST_PERSON") && *SDL_getenv("PSIV_FIRST_PERSON") == '1');
    walkto_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.click_walk"), "off") != 0);
    /* your own music files (music.h), from the music folder */
    {
        char dir[1100];
        path_in_home(dir, sizeof dir, "music");
        music_set_folder(!classic_mode() && SDL_strcasecmp(setting("enhance.custom_music"), "off") ? dir : NULL);
    }
    /* the instrument bank (paused 2026-11-06: no setting; PSIV_MUSIC_BANK=ROM
     * brings it back for development): decoded from that ROM when it changes */
    static char bank_now[1100];
    const char *bank = SDL_getenv("PSIV_MUSIC_BANK") ? SDL_getenv("PSIV_MUSIC_BANK") : "";
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

/* The settings, on five pages. Each says what it does under the list, so the
 * labels can stay short. */
typedef struct {
    const char *label;
    const char *key;                  /* the setting left / right changes */
    const char *values[4];            /* its values, and how they're shown */
    const char *names[4];
    int lo, hi, step;                 /* a number instead of a list */
    int enhancement;                  /* off (greyed) in Classic mode */
    const char *help;                 /* what it does */
    void (*apply)(void);
} MenuItem;

static const MenuItem GAME_ITEMS[] = {
    {"Mode", "enhance.mode", {"enhanced", "classic"}, {"Enhanced", "Classic"}, 0, 0, 0, 0,
     "Classic turns every addition off: the original game, unchanged.", apply_enhancements},
    {"Text", "enhance.text", {"modern", "original"}, {"Modern", "Original"}, 0, 0, 0, 1,
     "Dialogue and the game's windows in a modern font at full resolution, or the game's own pixel font.",
     apply_enhancements},
    {"Rewind", "enhance.rewind", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 0,
     "Backspace or the left shoulder goes back about ten seconds: a fade to white and back.",
     apply_enhancements},
};
static const MenuItem DISPLAY_ITEMS[] = {
    {"Fullscreen", "video.fullscreen", {"0", "1"}, {"Off", "On"}, 0, 0, 0, 0,
     "Fill the screen. F11 switches it too.", apply_video},
    {"Widescreen", "video.widescreen", {"0", "1", "2"}, {"Off", "More map", "Side panels"}, 0, 0, 0, 1,
     "16:9: more of the world on either side, or the party's cards in the margins while exploring. Battles "
     "are wide either way.", apply_enhancements},
    {"Pixel shape", "video.aspect", {"square", "4:3"}, {"Square", "4:3 TV"}, 0, 0, 0, 0,
     "4:3 TV: the slightly narrow pixels the console showed on a television.", apply_enhancements},
    {"Smoothing", "video.filter", {"nearest", "linear"}, {"Off", "On"}, 0, 0, 0, 0,
     "Off keeps every pixel sharp; on blends them where the picture is scaled.", apply_video},
    {"Integer scaling", "video.integer_scaling", {"1", "0"}, {"On", "Off"}, 0, 0, 0, 0,
     "Scale by whole numbers only: the sharpest pixels, with a border round the picture.", NULL},
};
static const MenuItem SOUND_ITEMS[] = {
    {"Volume", "audio.volume", {0}, {0}, 0, 200, 10, 0,
     "The music and the sound effects together.", apply_audio},
    {"Music", "enhance.music", {"original", "algol"}, {"Original chip", "Algol"}, 0, 0, 0, 1,
     "Algol: the port's own modern sound chip plays the game's music on new instruments, with rock drums.",
     apply_enhancements},
    {"Your music", "enhance.custom_music", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "Audio files you put in the music folder play in place of the game's songs.", apply_enhancements},
    {"Sound effects", "enhance.sfx", {"modern", "custom", "original"}, {"Modern", "Your samples", "Original"},
     0, 0, 0, 1, "Modern: the port's own. Your samples: the ones in the sfx folder. Original: the game's.",
     apply_enhancements},
};
static const MenuItem BATTLE_ITEMS[] = {
    {"Menus", "enhance.battle_ui", {"modern", "original"}, {"Modern", "Original"}, 0, 0, 0, 1,
     "Full names, descriptions and a party bar with portraits, or the game's own windows.", apply_enhancements},
    {"Depth", "enhance.battle_depth", {"0", "1", "2"}, {"Off", "Subtle", "Strong"}, 0, 0, 0, 1,
     "Battles drawn as layers at different depths under a gently moving camera. F9 switches it too.",
     apply_enhancements},
    {"Fog", "enhance.battle_fog", {"0", "1", "2"}, {"Off", "Light", "Heavy"}, 0, 0, 0, 1,
     "Drifting mist between the enemies and the party. F8 switches it too.", apply_enhancements},
    {"Weather", "enhance.battle_weather", {"0", "1", "2"}, {"Off", "Light", "Heavy"}, 0, 0, 0, 1,
     "Rain and storms on Motavia, snow on Dezolis, mist clouds on Rykros, debris in space. Heavy: more of it.",
     apply_enhancements},
    {"Effects", "enhance.battle_fx", {"0", "1", "2"}, {"Off", "Subtle", "Full"}, 0, 0, 0, 1,
     "Attacks and techniques light the scene: sparks, flashes, embers and lens flares.", apply_enhancements},
};
static const MenuItem CONTROL_ITEMS[] = {
    {"Click to walk", "enhance.click_walk", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "Click or tap the field to walk there, or a person or a chest to talk or open it. Battle menus take "
     "clicks too.", apply_enhancements},
    {"Rumble", "pad.rumble", {"off", "light", "normal", "strong"}, {"Off", "Light", "Normal", "Strong"}, 0, 0, 0, 0,
     "Blows landing, critical hits, enemies destroyed and the field's big moments, on a gamepad or a "
     "phone's motor.", apply_rumble},
};

/* a page has settings, or (items NULL) shows something else: draw fills it */
typedef struct { const char *name, *about; const MenuItem *items; int n; void (*draw)(float W, float H); } MenuPage;
#define PAGE(name, about, items) {name, about, items, (int)(sizeof items / sizeof items[0]), NULL}
static void draw_buttons_page(float W, float H);
static const MenuPage PAGES[] = {
    PAGE("Game", "Classic or Enhanced, and the text.", GAME_ITEMS),
    PAGE("Display", "Fullscreen, widescreen, and how the pixels are drawn.", DISPLAY_ITEMS),
    PAGE("Sound", "The volume, the music and the sound effects.", SOUND_ITEMS),
    PAGE("Battles", "The battle menus, and what's drawn over battles.", BATTLE_ITEMS),
    PAGE("Controls", "Clicks and taps, and rumble.", CONTROL_ITEMS),
    {"Buttons", "What each button and key does.", NULL, 0, draw_buttons_page},
};
#define N_PAGES (int)(sizeof PAGES / sizeof PAGES[0])
#define MAX_ROWS 5                    /* the longest page: the panel keeps its size */
static const char *const MAIN_LABELS[] = {"Resume", "Go back 10 seconds", "Settings", "Quit game"};
#define N_MAIN 4
static const char *const CLASSIC_NOTE = "Off in Classic mode. Set Mode to Enhanced, on the Game page, to use it.";

enum { MENU_CLOSED, MENU_MAIN, MENU_SETTINGS, MENU_QUIT, MENU_LAUNCHER };
static int menu_screen, menu_sel;     /* in Settings, -1 = the page bar */
static int in_launcher;               /* the launcher is up: the game hasn't started */
static int launcher_go;               /* its Start game was chosen */
static const char *const LAUNCHER_LABELS[] = {"Start game", "Mode", "Settings", "Quit"};
enum { L_START, L_MODE, L_SETTINGS, L_QUIT, N_LAUNCHER };
#define MODE_ITEM (&GAME_ITEMS[0])    /* Classic / Enhanced */
static int menu_page;                 /* the settings page (kept while the game runs) */
static uint64_t menu_opened;          /* when it opened (it fades in); 0 = at once */
static int menu_last_pad;             /* the last press was a gamepad's: the hints name its buttons */

static int item_values(const MenuItem *it) {
    int n = 0;
    while (n < 4 && it->values[n]) n++;
    return n;
}

static int item_disabled(const MenuItem *it) {
    return it->enhancement && classic_mode();
}

static void item_value(const MenuItem *it, char *buf, size_t n) {
    if (it->step) { snprintf(buf, n, "%d%%", setting_int(it->key)); return; }
    const char *v = setting(it->key);
    for (int i = 0; i < item_values(it); i++)
        if (!SDL_strcasecmp(v, it->values[i])) { snprintf(buf, n, "%s", it->names[i]); return; }
    snprintf(buf, n, "%s", v);
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
static void menu_open(void) { menu_screen = MENU_MAIN; menu_sel = 0; menu_opened = SDL_GetTicks(); }
static void menu_close(void) {
    if (in_launcher) { menu_screen = MENU_LAUNCHER; return; }   /* it stays until Start game */
    menu_screen = MENU_CLOSED;
    input_hold = 1;
}
static int menu_is_open(void) { return menu_screen != MENU_CLOSED; }

static const MenuPage *page_now(void) { return &PAGES[menu_page]; }

/* into Settings: the page's first setting, or the page bar on a page without any */
static void settings_enter(void) {
    menu_screen = MENU_SETTINGS;
    menu_sel = page_now()->n ? 0 : -1;
}

/* the next or previous settings page (shoulder buttons, Tab, the page bar) */
static void menu_turn_page(int dir) {
    if (menu_screen != MENU_SETTINGS) return;
    menu_page = (menu_page + dir + N_PAGES) % N_PAGES;
    if (menu_sel >= page_now()->n) menu_sel = page_now()->n - 1;
}

/* one key or button press while the menu is open */
static void menu_input(int up, int down, int left, int right, int ok, int back) {
    if (back) {
        if (menu_screen == MENU_LAUNCHER) menu_sel = L_QUIT;
        else if (menu_screen == MENU_MAIN) menu_close();
        else if (menu_screen == MENU_SETTINGS && in_launcher) { menu_screen = MENU_LAUNCHER; menu_sel = L_SETTINGS; }
        else { menu_sel = menu_screen == MENU_SETTINGS ? 1 : 2; menu_screen = MENU_MAIN; }
        return;
    }
    if (menu_screen == MENU_LAUNCHER) {
        if (up) menu_sel = (menu_sel + N_LAUNCHER - 1) % N_LAUNCHER;
        if (down) menu_sel = (menu_sel + 1) % N_LAUNCHER;
        if ((left || right) && menu_sel == L_MODE) item_change(MODE_ITEM, right ? 1 : -1);
        if (ok) {
            if (menu_sel == L_START) launcher_go = 1;
            else if (menu_sel == L_MODE) item_change(MODE_ITEM, 1);
            else if (menu_sel == L_SETTINGS) settings_enter();
            else quit(0);
        }
        return;
    }
    if (menu_screen == MENU_SETTINGS) {               /* the page bar sits above the first setting */
        int n = page_now()->n;
        if (up) menu_sel = menu_sel < 0 ? n - 1 : menu_sel - 1;
        if (down) menu_sel = menu_sel >= n - 1 ? -1 : menu_sel + 1;
        if (left || right) {
            if (menu_sel < 0) menu_turn_page(right ? 1 : -1);
            else item_change(&page_now()->items[menu_sel], right ? 1 : -1);
        }
        if (ok) {
            if (menu_sel >= 0) item_change(&page_now()->items[menu_sel], 1);
            else if (n) menu_sel = 0;
        }
        return;
    }
    int n = menu_screen == MENU_MAIN ? N_MAIN : 2;
    if (up) menu_sel = (menu_sel + n - 1) % n;
    if (down) menu_sel = (menu_sel + 1) % n;
    if (menu_screen == MENU_QUIT && (left || right)) menu_sel ^= 1;
    if (!ok) return;
    if (menu_screen == MENU_MAIN) {
        if (menu_sel == 0) menu_close();
        else if (menu_sel == 1) { menu_close(); rewind_now(); }
        else if (menu_sel == 2) settings_enter();
        else { menu_screen = MENU_QUIT; menu_sel = 1; }               /* default: No */
    } else {
        if (menu_sel == 0) quit(0);
        menu_screen = MENU_MAIN;
        menu_sel = 3;
    }
}

/* ------------------------------------------------------------------ the menus' look */
/* Drawn like the modern battle menus (uikit.h: rounded navy panels, a gold
 * highlight, the modern text's font) into a transparent texture that is only
 * redrawn when something on it changes; without a font, with SDL's own 8x8
 * one straight to the window. Sizes are in units of the 320x224 screen. */
#define C_PANEL_TOP 0x1B2756
#define C_PANEL_LOW 0x0C1230
#define C_RULE      0x6F8FE0
#define C_GOLD      0xFFE08A
#define C_GOLD_LOW  0xF0A83C
#define C_INK       0x1A1406                     /* text on the gold */
#define C_INK_SOFT  0x3A2A08
#define C_TEXT      0xFFFFFF
#define C_VALUE     0xD8E2FF
#define C_SOFT      0x9FB4E8
#define C_DIM       0x6F7FA8
#define C_HINT      0x8C9CC8

typedef struct { float x0, y0, x1, y1; int kind, index; } MenuHit;   /* render pixels */
enum { HIT_ROW, HIT_STEP_BACK, HIT_PAGE, HIT_BACK, HIT_MODE };
/* the launcher fills the window: units of 1/224 of its height (narrow windows:
 * 1/300 of the width), its content in a 224-tall column in the middle; the
 * emblem (drawn under it, launcher_backdrop) is centred this far down */
#define LAUNCH_EMBLEM_Y 47.0f
static float launch_scale(int ow, int oh) { return fminf((float)oh / 224.0f, (float)ow / 300.0f); }
static MenuHit menu_hits[24];
static int n_menu_hits;
static SDL_FRect menu_panel;                     /* where the panel is, render pixels */
static SDL_Texture *emblem_tex, *halo_tex;       /* the launcher's emblem (emblem_make) */
static int emblem_n;
static SDL_Texture *menu_tex;
static uint32_t *menu_buf;
static int menu_tex_w, menu_tex_h;
static uint64_t menu_drawn;                      /* what the texture shows (menu_signature) */

static int m_ttf;                                /* drawing into menu_buf with the TrueType font */
static UiCanvas m_cv;
static float m_S, m_ox, m_oy;                    /* render pixels per unit; the panel's top left */

static float m_debug_scale(float cap) {          /* SDL's font: capitals 7 of its 8 pixels */
    float k = floorf(cap * m_S / 7.0f + 0.3f);
    return k < 1.0f ? 1.0f : k;
}
static void m_box(float x0, float y0, float x1, float y1, float r, uint32_t top, uint32_t bottom, int alpha) {
    if (m_ttf) {
        ui_round_rect_v(&m_cv, x0 * m_S, y0 * m_S, x1 * m_S, y1 * m_S, r * m_S, top, bottom, alpha);
        return;
    }
    SDL_SetRenderDrawColor(renderer, (Uint8)(top >> 16), (Uint8)(top >> 8), (Uint8)top, (Uint8)alpha);
    SDL_FRect f = {m_ox + x0 * m_S, m_oy + y0 * m_S, (x1 - x0) * m_S, (y1 - y0) * m_S};
    SDL_RenderFillRect(renderer, &f);
}
static void m_frame(float x0, float y0, float x1, float y1, float r, uint32_t c, int alpha) {
    if (m_ttf) {
        ui_round_frame(&m_cv, x0 * m_S, y0 * m_S, x1 * m_S, y1 * m_S, r * m_S, fmaxf(1.0f, 0.35f * m_S), c, alpha);
        return;
    }
    SDL_SetRenderDrawColor(renderer, (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c, (Uint8)alpha);
    SDL_FRect f = {m_ox + x0 * m_S, m_oy + y0 * m_S, (x1 - x0) * m_S, (y1 - y0) * m_S};
    SDL_RenderRect(renderer, &f);
}
static float m_width(const char *s, float cap) {
    if (m_ttf) return ui_text_width(s, cap * m_S) / m_S;
    return 8.0f * m_debug_scale(cap) * (float)strlen(s) / m_S;
}
/* at (x, baseline); align 0 left, 1 right of x, 2 centred on it */
static void m_text(float x, float base, float cap, const char *s, uint32_t c, int align) {
    if (align) x -= m_width(s, cap) / (align == 1 ? 1.0f : 2.0f);
    if (m_ttf) {
        ui_text(&m_cv, x * m_S, base * m_S, cap * m_S, s, c, 255);
        return;
    }
    float k = m_debug_scale(cap);
    SDL_SetRenderScale(renderer, k, k);
    SDL_SetRenderDrawColor(renderer, (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c, 255);
    SDL_RenderDebugText(renderer, (m_ox + x * m_S) / k, (m_oy + base * m_S) / k - 7.0f, s);
    SDL_SetRenderScale(renderer, 1, 1);
}
/* word-wrapped to `width` from x, at most `lines` lines; centre: each line centred in the width */
static void m_wrap(float x, float base, float width, float line_h, int lines, float cap, const char *s, uint32_t c,
                   int centre) {
    char line[256];
    while (*s && lines-- > 0) {
        while (*s == ' ') s++;
        int fit = 0, i = 0;
        for (;;) {                                   /* as many whole words as fit */
            while (s[i] && s[i] != ' ') i++;
            if (i >= (int)sizeof line) break;
            memcpy(line, s, (size_t)i);
            line[i] = 0;
            if (fit && m_width(line, cap) > width) break;
            fit = i;
            if (!s[i]) break;
            i++;
        }
        if (!fit) break;
        memcpy(line, s, (size_t)fit);
        line[fit] = 0;
        m_text(centre ? x + width / 2 : x, base, cap, line, c, centre ? 2 : 0);
        s += fit;
        base += line_h;
    }
}
static void m_arrow(float cx, float cy, float size, int dir, uint32_t c) {   /* dir 2 right, 3 left */
    if (m_ttf) ui_triangle(&m_cv, cx * m_S, cy * m_S, size * m_S, dir, c, 255);
    else m_text(cx, cy + size * 0.8f, size * 1.2f, dir == 3 ? "<" : ">", c, 2);
}
static void m_hit(float x0, float y0, float x1, float y1, int kind, int index) {
    if (n_menu_hits >= (int)(sizeof menu_hits / sizeof menu_hits[0])) return;
    MenuHit *h = &menu_hits[n_menu_hits++];
    h->x0 = m_ox + x0 * m_S;
    h->y0 = m_oy + y0 * m_S;
    h->x1 = m_ox + x1 * m_S;
    h->y1 = m_oy + y1 * m_S;
    h->kind = kind;
    h->index = index;
}

/* the gamepad's own names for its buttons, for the hints */
static const char *pad_button_name(SDL_GamepadButton b) {
    switch (pad ? SDL_GetGamepadButtonLabel(pad, b) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
    default: return b == pad_of[IN_B] ? "B" : "C";              /* the Mega Drive's names */
    }
}
static const char *pad_shoulder_names(void) {
    switch (pad ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return MOBILE ? "L1 / R1" : "LB / RB";   /* handhelds pass for an Xbox pad */
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "L / R";
    default: return "L1 / R1";
    }
}

/* how to drive the menu with what's in use: touch, a gamepad, the keyboard */
static void menu_hint(char *out, size_t n) {
    const char *sep = m_ttf ? "   \xC2\xB7   " : "   ";
    int settings_screen = menu_screen == MENU_SETTINGS;
    if (settings_screen && !page_now()->n) {             /* a page to read: only the pages to turn */
        if (touch_visible()) snprintf(out, n, "Tap a page's name%sTap outside to go back", sep);
        else if (menu_last_pad && pad)
            snprintf(out, n, "Left / Right or %s page%s%s back", pad_shoulder_names(), sep, pad_button_name(pad_of[IN_B]));
        else snprintf(out, n, "Left / Right or Tab page%sEsc back", sep);
        return;
    }
    if (menu_screen == MENU_LAUNCHER) {
        if (touch_visible()) snprintf(out, n, "Tap to choose");
        else snprintf(out, n, "%s select%sLeft / Right switch the mode",
                      menu_last_pad && pad ? pad_button_name(pad_of[IN_C]) : "Enter", sep);
    } else if (touch_visible())
        snprintf(out, n, "%s%sTap outside to go back", settings_screen ? "Tap a setting to change it" : "Tap to choose", sep);
    else if (menu_last_pad && pad) {
        if (settings_screen)
            snprintf(out, n, "Left / Right change%s%s page%s%s back", sep, pad_shoulder_names(), sep,
                     pad_button_name(pad_of[IN_B]));
        else
            snprintf(out, n, "%s choose%s%s back", pad_button_name(pad_of[IN_C]), sep, pad_button_name(pad_of[IN_B]));
    } else if (settings_screen)
        snprintf(out, n, "Left / Right change%sTab page%sEsc back", sep, sep);
    else
        snprintf(out, n, "Enter choose%sEsc back", sep);
}

/* Resume / Settings / Quit, and the quit question's answers */
static void draw_rows(float W, const char *const *labels, int n, float y0) {
    for (int i = 0; i < n; i++) {
        float a = y0 + 16.0f * (float)i, b = a + 14.0f;
        if (i == menu_sel) m_box(8.0f, a, W - 8.0f, b, 7.0f, C_GOLD, C_GOLD_LOW, 255);
        m_text(15.0f, a + 9.8f, 5.0f, labels[i], i == menu_sel ? C_INK : C_TEXT, 0);
        m_hit(8.0f, a, W - 8.0f, b, HIT_ROW, i);
    }
}

static void draw_settings(float W, float H, const char *hint) {
    const float pad = 10.0f;
    const MenuPage *pg = page_now();
    m_text(pad + 2.0f, 17.0f, 6.4f, "Settings", C_GOLD, 0);
    /* Back, for the mouse and for touch */
    float bx1 = W - pad, bx0 = bx1 - m_width("Back", 4.2f) - 17.0f, by0 = 7.0f, by1 = 20.0f;
    m_box(bx0, by0, bx1, by1, 6.5f, 0x26346A, 0x18214A, 240);
    m_frame(bx0, by0, bx1, by1, 6.5f, C_RULE, 120);
    m_arrow(bx0 + 7.5f, (by0 + by1) / 2, 4.0f, 3, C_SOFT);
    m_text(bx0 + 12.0f, by0 + 9.1f, 4.2f, "Back", C_VALUE, 0);
    m_hit(bx0, by0, bx1, by1, HIT_BACK, 0);
    /* the pages: their names smaller if they wouldn't fit across (SDL's 8x8 font) */
    float x = pad, t0 = 26.0f, t1 = 40.0f, cap = 4.6f, room = 14.0f;
    for (;;) {
        float all = 3.0f * (N_PAGES - 1);
        for (int i = 0; i < N_PAGES; i++) all += m_width(PAGES[i].name, cap) + room;
        if (all <= W - 2 * pad || cap <= 3.0f) break;
        cap -= 0.2f;
        room = 10.0f;
    }
    for (int i = 0; i < N_PAGES; i++) {
        float w = m_width(PAGES[i].name, cap) + room;
        int on = i == menu_page, focus = on && menu_sel < 0;
        if (focus) m_box(x, t0, x + w, t1, 7.0f, C_GOLD, C_GOLD_LOW, 255);
        else if (on) m_box(x, t0, x + w, t1, 7.0f, 0x3A4C8E, 0x2A3870, 255);
        m_text(x + w / 2, t0 + 9.8f, cap, PAGES[i].name, focus ? C_INK : on ? C_TEXT : C_SOFT, 2);
        m_hit(x, t0, x + w, t1, HIT_PAGE, i);
        x += w + 3.0f;
    }
    m_box(pad, 44.5f, W - pad, 45.0f, 0, C_RULE, C_RULE, 80);
    if (pg->draw) {                                  /* a page without settings draws its own */
        pg->draw(W, H);
        m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
        return;
    }
    /* its settings: the label, the value; the highlighted one with arrows */
    for (int i = 0; i < pg->n; i++) {
        const MenuItem *it = &pg->items[i];
        float a = 48.5f + 15.0f * (float)i, b = a + 13.5f, base = a + 9.5f, mid = (a + b) / 2;
        int sel = i == menu_sel, off = item_disabled(it);
        char v[64];
        item_value(it, v, sizeof v);
        if (sel) m_box(pad, a, W - pad, b, 6.75f, off ? 0x56607E : C_GOLD, off ? 0x3E4766 : C_GOLD_LOW, 255);
        uint32_t lc = off ? (sel ? 0xC8D0E8 : C_DIM) : sel ? C_INK : C_TEXT;
        uint32_t vc = off ? (sel ? 0xC8D0E8 : C_DIM) : sel ? C_INK : C_VALUE;
        m_text(pad + 8.0f, base, 4.6f, it->label, lc, 0);
        float vr = W - pad - 13.0f, vw = m_width(v, 4.6f);
        m_text(vr, base, 4.6f, v, vc, 1);
        if (sel && !off) {
            m_arrow(vr - vw - 6.5f, mid, 4.0f, 3, C_INK_SOFT);
            m_arrow(vr + 6.5f, mid, 4.0f, 2, C_INK_SOFT);
        }
        m_hit(vr - vw - 14.0f, a, vr - vw * 0.5f, b, HIT_STEP_BACK, i);   /* the value's left: a step back */
        m_hit(pad, a, W - pad, b, HIT_ROW, i);
    }
    /* what the highlighted one does */
    m_box(pad, 127.5f, W - pad, 128.0f, 0, C_RULE, C_RULE, 80);
    const char *about = menu_sel < 0 ? pg->about
                      : item_disabled(&pg->items[menu_sel]) ? CLASSIC_NOTE : pg->items[menu_sel].help;
    m_wrap(pad + 2.0f, 138.0f, W - 2 * pad - 4.0f, 8.6f, 2, 4.0f, about, C_VALUE, 0);
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* ------------------------------------------------------------------ the Buttons page */
/* A button by the name printed on this gamepad (with none connected: an Xbox
 * pad's). Handhelds pass for an Xbox pad but print L1 / R1 on their shoulders. */
static const char *pad_label(SDL_GamepadButton b) {
    SDL_GamepadType t = pad ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_STANDARD;
    int ps = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
    int nin = t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
              t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    int xbox = !MOBILE && (t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE);
    switch (b) {
    case SDL_GAMEPAD_BUTTON_INVALID: return m_ttf ? "\xE2\x80\x94" : "-";   /* none: a dash */
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH:
        switch (pad ? SDL_GetGamepadButtonLabel(pad, b) : SDL_GetGamepadButtonLabelForType(t, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: break;
        }
        return b == SDL_GAMEPAD_BUTTON_SOUTH ? "South" : b == SDL_GAMEPAD_BUTTON_EAST ? "East"
             : b == SDL_GAMEPAD_BUTTON_WEST ? "West" : "North";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return nin ? "L" : xbox ? "LB" : "L1";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return nin ? "R" : xbox ? "RB" : "R1";
    case SDL_GAMEPAD_BUTTON_START: return ps ? "Options" : nin ? "+" : "Start";
    case SDL_GAMEPAD_BUTTON_BACK:
        return t == SDL_GAMEPAD_TYPE_PS5 ? "Create" : ps ? "Share" : nin ? "-"
             : xbox ? (t == SDL_GAMEPAD_TYPE_XBOXONE ? "View" : "Back") : "Select";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
    default: {
        const char *s = SDL_GetGamepadStringForButton(b);              /* SDL's own name for the rest */
        return s ? s : "?";
    }
    }
}

/* a bound key by its name on the keyboard (Return is Enter on most) */
static void key_label(int in, char *out, size_t n) {
    const char *s = key_of[in] != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(key_of[in]) : "";
    if (!*s) s = m_ttf ? "\xE2\x80\x94" : "-";
    else if (!strcmp(s, "Return")) s = "Enter";
    snprintf(out, n, "%s", s);
}

/* what each button does: the game's buttons (the Mega Drive's A, B and C),
 * then the port's own; this gamepad's names, and the keyboard (on a phone or a
 * handheld, the on-screen pad) */
static void draw_buttons_page(float W, float H) {
    const float pad_x = 10.0f, x_what = pad_x + 7.0f, x_pad = 150.0f, x_other = 220.0f;
    const char *dash = m_ttf ? "\xE2\x80\x94" : "-";
    (void)H;
    typedef struct { const char *what, *md, *gp; char other[48]; } Row;
    Row r[10];
    int n = 0;
    memset(r, 0, sizeof r);
    r[n] = (Row){"Move", NULL, "D-pad, left stick", ""};
    if (MOBILE) snprintf(r[n].other, sizeof r[n].other, "On-screen pad");
    else if (key_of[IN_UP] == SDL_SCANCODE_UP && key_of[IN_DOWN] == SDL_SCANCODE_DOWN &&
             key_of[IN_LEFT] == SDL_SCANCODE_LEFT && key_of[IN_RIGHT] == SDL_SCANCODE_RIGHT)
        snprintf(r[n].other, sizeof r[n].other, "Arrow keys");
    else {
        char k[4][16];
        const int order[4] = {IN_UP, IN_LEFT, IN_DOWN, IN_RIGHT};             /* as W A S D */
        for (int i = 0; i < 4; i++) key_label(order[i], k[i], sizeof k[i]);
        snprintf(r[n].other, sizeof r[n].other, "%s %s %s %s", k[0], k[1], k[2], k[3]);
    }
    n++;
    static const struct { const char *what, *md; int in; } GAME[] = {
        {"Talk, check, OK", "C", IN_C}, {"Cancel, back", "B", IN_B}, {"The game's menu", "A", IN_A}, {"Start", NULL, IN_START},
    };
    for (int i = 0; i < 4; i++, n++) {
        r[n] = (Row){GAME[i].what, GAME[i].md, pad_label(pad_of[GAME[i].in]), ""};
        if (MOBILE) snprintf(r[n].other, sizeof r[n].other, "%s", GAME[i].md ? GAME[i].md : "Start");
        else key_label(GAME[i].in, r[n].other, sizeof r[n].other);
    }
    r[n] = (Row){"This menu", NULL, pad_label(SDL_GAMEPAD_BUTTON_BACK), ""};
    snprintf(r[n++].other, sizeof r[0].other, "%s", MOBILE ? "Top-left button, Back" : "Esc");
    r[n] = (Row){"Fast forward (hold)", NULL, pad_label(pad_of[IN_FF]), ""};
    if (MOBILE) snprintf(r[n].other, sizeof r[n].other, "%s", dash);
    else key_label(IN_FF, r[n].other, sizeof r[n].other);
    n++;
    r[n] = (Row){"Go back 10 seconds", NULL, pad_label(pad_of[IN_REWIND]), ""};
    if (MOBILE) snprintf(r[n].other, sizeof r[n].other, "In the menu");
    else key_label(IN_REWIND, r[n].other, sizeof r[n].other);
    n++;
    if (!MOBILE) {
        r[n] = (Row){"Pause, fullscreen, screenshot", NULL, dash, ""};
        snprintf(r[n++].other, sizeof r[0].other, "F10, F11, F12");
    }

    m_text(x_pad, 54.0f, 3.6f, "Gamepad", C_SOFT, 0);
    m_text(x_other, 54.0f, 3.6f, MOBILE ? "Touch" : "Keyboard", C_SOFT, 0);
    float step = n > 8 ? 7.6f : 8.6f;                     /* nine rows on a desktop: closer */
    for (int i = 0; i < n; i++) {
        float a = 57.5f + step * (float)i, base = a + step * 0.73f;
        if (i & 1) m_box(pad_x, a, W - pad_x, a + step, 3.0f, 0x26346A, 0x22305F, 110);
        m_text(x_what, base, 4.2f, r[i].what, C_TEXT, 0);
        if (r[i].md) {                               /* the game's own name for it: the Mega Drive's button */
            char md[16];
            snprintf(md, sizeof md, "(%s)", r[i].md);
            m_text(x_what + m_width(r[i].what, 4.2f) + 2.5f, base, 3.6f, md, C_DIM, 0);
        }
        m_text(x_pad, base, 4.2f, r[i].gp, C_GOLD, 0);
        m_text(x_other, base, 4.2f, r[i].other, C_VALUE, 0);
    }
    m_box(pad_x, 127.5f, W - pad_x, 128.0f, 0, C_RULE, C_RULE, 80);
    m_wrap(pad_x + 2.0f, 138.0f, W - 2 * pad_x - 4.0f, 8.6f, 2, 4.0f,
           MOBILE ? "Tap the field to walk there; touch the screen to bring the on-screen pad back. "
                    "Change the buttons in psiv.ini, under [pad]."
                  : "Mouse: click the field to walk there, or a person to talk; a right click is Cancel. "
                    "Change the keys and buttons in psiv.ini, under [keys] and [pad].",
           C_VALUE, 0);
}

/* the launcher: its title under the emblem, the choices, what the highlighted
 * one means; the version and the hints in the corners */
static void draw_launcher(float W, float H, const char *hint) {
    float cx = W / 2.0f, oy = (H - 224.0f) / 2.0f;
    m_text(cx, oy + 98.0f, 12.5f, "PSIV Recomp", C_GOLD, 2);
    m_text(cx, oy + 108.5f, 4.2f, "A native port of Phantasy Star IV", C_SOFT, 2);

    float pw = 158.0f, x0 = cx - pw / 2.0f, x1 = cx + pw / 2.0f, y0 = oy + 116.0f, row = 16.0f;
    m_box(x0, y0, x1, y0 + 6.0f + row * N_LAUNCHER, 7.0f, C_PANEL_TOP, C_PANEL_LOW, 236);
    m_frame(x0, y0, x1, y0 + 6.0f + row * N_LAUNCHER, 7.0f, C_RULE, 150);
    int classic = classic_mode();
    for (int i = 0; i < N_LAUNCHER; i++) {
        float a = y0 + 3.0f + row * (float)i, b = a + row - 2.0f;
        int sel = i == menu_sel;
        if (sel) m_box(x0 + 3.0f, a, x1 - 3.0f, b, 7.0f, C_GOLD, C_GOLD_LOW, 255);
        m_text(x0 + 11.0f, a + 9.6f, 5.2f, LAUNCHER_LABELS[i], sel ? C_INK : C_TEXT, 0);
        if (i == L_MODE) {                           /* a two-way switch, the mode in force lit */
            static const char *const MODES[2] = {"Enhanced", "Classic"};
            float sw[2], sx1 = x1 - 8.0f, sh0 = a + 1.8f, sh1 = b - 1.8f;
            for (int k = 0; k < 2; k++) sw[k] = m_width(MODES[k], 4.1f) + 10.0f;
            float sx0 = sx1 - sw[0] - sw[1];
            m_box(sx0, sh0, sx1, sh1, (sh1 - sh0) / 2, sel ? 0x6A4A12 : 0x101830, sel ? 0x5A3C0C : 0x0A1024, 200);
            for (int k = 0; k < 2; k++) {
                float s0 = k ? sx0 + sw[0] : sx0, s1 = s0 + sw[k];
                int on = k == classic;
                if (on) m_box(s0 + 0.6f, sh0 + 0.6f, s1 - 0.6f, sh1 - 0.6f, (sh1 - sh0) / 2 - 0.6f,
                              sel ? 0xFFFFFF : C_GOLD, sel ? 0xF4F0E6 : C_GOLD_LOW, 255);
                m_text((s0 + s1) / 2, a + 9.1f, 4.1f, MODES[k], on ? C_INK : sel ? 0xF2D8A0 : C_SOFT, 2);
                m_hit(s0, a, s1, b, HIT_MODE, k);    /* before the row's: a tap here picks that mode */
            }
        }
        m_hit(x0 + 3.0f, a, x1 - 3.0f, b, HIT_ROW, i);
    }

    const char *about =
        menu_sel == L_START ? (rom_ready ? "Your Phantasy Star IV (USA), checked and ready."
                               : rom_problem[0] ? "The ROM that's installed can't be used: Start game says why, then asks for another."
                               : "Start game asks for your copy of Phantasy Star IV (USA): a .bin, .md, .gen or .smd file, or the .zip it came in.")
        : menu_sel == L_MODE ? (classic ? "Classic: the original game, unchanged. Every addition is off."
                                : "Enhanced: modern text and battle menus, widescreen, battle depth and effects, and the port's other additions.")
        : menu_sel == L_SETTINGS ? "The display, sound, battles and controls, and what each button does."
        : "Leave the game.";
    float ab = y0 + 6.0f + row * N_LAUNCHER + 9.5f;
    m_wrap(cx - 95.0f, ab, 190.0f, 7.6f, 2, 3.8f, about, C_VALUE, 1);

    m_text(6.0f, H - 5.0f, 3.2f, APP_NAME " " PSIV_VERSION, C_DIM, 0);
    m_text(W - 6.0f, H - 5.0f, 3.4f, hint, C_HINT, 1);
}

static void menu_layout(float W, float H) {
    char hint[200];
    menu_hint(hint, sizeof hint);
    if (menu_screen == MENU_LAUNCHER) {
        draw_launcher(W, H, hint);
        return;
    }
    m_box(0, 0, W, H, 7.0f, C_PANEL_TOP, C_PANEL_LOW, 244);
    m_frame(0, 0, W, H, 7.0f, C_RULE, 150);
    if (menu_screen == MENU_SETTINGS) {
        draw_settings(W, H, hint);
    } else if (menu_screen == MENU_MAIN) {
        m_text(12.0f, 17.0f, 6.4f, "Paused", C_GOLD, 0);
        draw_rows(W, MAIN_LABELS, N_MAIN, 26.0f);
        m_text(12.0f, H - 15.0f, 3.4f, hint, C_HINT, 0);
        m_text(12.0f, H - 6.5f, 3.1f, APP_NAME " " PSIV_VERSION, C_DIM, 0);   /* for bug reports */
    } else {
        static const char *const ANSWERS[2] = {"Yes, quit", "No, keep playing"};
        m_text(12.0f, 17.0f, 6.4f, "Quit the game?", C_GOLD, 0);
        draw_rows(W, ANSWERS, 2, 26.0f);
        m_wrap(12.0f, 67.5f, W - 24.0f, 7.5f, 2, 3.8f, "Anything since you last saved in the game is lost.", C_VALUE, 0);
        m_text(12.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
    }
}

/* what's on the panel: when this changes, it's drawn again */
static uint64_t menu_signature(int ow, int oh) {
    uint64_t parts[] = {(uint64_t)menu_screen, (uint64_t)(menu_sel + 1), (uint64_t)menu_page, (uint64_t)ow,
                        (uint64_t)oh, (uint64_t)touch_visible(), (uint64_t)(menu_last_pad && pad),
                        pad ? (uint64_t)SDL_GetGamepadID(pad) : 0, (uint64_t)rom_ready, (uint64_t)(rom_problem[0] != 0)};
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) h = (h ^ parts[i]) * 1099511628211ull;
    for (int i = 0; i < NSETTINGS; i++)
        for (const char *p = settings[i].value; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    return h;
}

static void menu_draw(void) {
    int ow, oh;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    float fade = menu_opened ? fminf(1.0f, (float)(SDL_GetTicks() - menu_opened) / 140.0f) : 1.0f;
    int launcher_screen = menu_screen == MENU_LAUNCHER;
    if (!launcher_screen) {                          /* dim the game (or the launcher) behind */
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(165.0f * fade));
        SDL_FRect all = {0, 0, (float)ow, (float)oh};
        SDL_RenderFillRect(renderer, &all);
    }

    float W, H;
    int tw, th;
    if (launcher_screen) {                           /* the whole window */
        m_S = launch_scale(ow, oh);
        W = (float)ow / m_S;
        H = (float)oh / m_S;
        tw = ow;
        th = oh;
        m_ox = m_oy = 0;
    } else {
        W = menu_screen == MENU_SETTINGS ? 288.0f : menu_screen == MENU_QUIT ? 176.0f : 168.0f;
        H = menu_screen == MENU_SETTINGS ? 168.0f : menu_screen == MENU_QUIT ? 86.0f : 116.0f;
        /* about the game's own scale, small enough that Settings always fits */
        m_S = fminf(fminf((float)ow / 320.0f, (float)oh / 224.0f),
                    fminf((float)ow * 0.95f / 288.0f, (float)oh * 0.92f / 168.0f));
        tw = (int)ceilf(W * m_S);
        th = (int)ceilf(H * m_S);
        m_ox = floorf(((float)ow - (float)tw) / 2);
        m_oy = floorf(((float)oh - (float)th) / 2);
    }
    menu_panel.x = m_ox;
    menu_panel.y = m_oy;
    menu_panel.w = (float)tw;
    menu_panel.h = (float)th;

    static int force_8x8 = -1;                       /* testing: PSIV_MENU_8X8=1 draws it as with no font */
    if (force_8x8 < 0) force_8x8 = SDL_getenv("PSIV_MENU_8X8") != NULL;
    m_ttf = ui_ready() && !force_8x8;
    if (m_ttf && (!menu_tex || tw != menu_tex_w || th != menu_tex_h)) {
        if (menu_tex) SDL_DestroyTexture(menu_tex);
        free(menu_buf);
        menu_buf = malloc((size_t)tw * (size_t)th * 4);
        menu_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, tw, th);
        menu_tex_w = tw;
        menu_tex_h = th;
        menu_drawn = 0;
        if (menu_tex) {
            SDL_SetTextureBlendMode(menu_tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(menu_tex, SDL_SCALEMODE_NEAREST);
        }
    }
    if (!m_ttf || !menu_tex || !menu_buf) {          /* no font: straight to the window, each frame */
        m_ttf = 0;
        n_menu_hits = 0;
        menu_layout(W, H);
        return;
    }
    uint64_t sig = menu_signature(ow, oh);
    if (sig != menu_drawn) {
        memset(menu_buf, 0, (size_t)tw * (size_t)th * 4);
        m_cv.px = menu_buf;
        m_cv.stride = tw;
        m_cv.x0 = m_cv.y0 = 0;
        m_cv.x1 = tw;
        m_cv.y1 = th;
        n_menu_hits = 0;
        ui_set_straight_alpha(1);
        menu_layout(W, H);
        ui_set_straight_alpha(0);
        SDL_UpdateTexture(menu_tex, NULL, menu_buf, tw * 4);
        menu_drawn = sig;
    }
    SDL_SetTextureAlphaModFloat(menu_tex, fade);
    SDL_FRect dst = {m_ox, m_oy, (float)tw, (float)th};
    SDL_RenderTexture(renderer, menu_tex, NULL, &dst);
}

/* a tap or a click on the menu: what's under it; outside the panel, back */
static void menu_tap(float rx, float ry) {
    if (rx < menu_panel.x || ry < menu_panel.y || rx >= menu_panel.x + menu_panel.w || ry >= menu_panel.y + menu_panel.h) {
        menu_input(0, 0, 0, 0, 0, 1);
        return;
    }
    for (int i = 0; i < n_menu_hits; i++) {
        const MenuHit *h = &menu_hits[i];
        if (rx < h->x0 || ry < h->y0 || rx >= h->x1 || ry >= h->y1) continue;
        if (h->kind == HIT_BACK) {
            menu_input(0, 0, 0, 0, 0, 1);
        } else if (h->kind == HIT_MODE) {           /* the launcher's switch: that mode */
            menu_sel = L_MODE;
            if ((h->index == 1) != classic_mode()) item_change(MODE_ITEM, 1);
        } else if (h->kind == HIT_PAGE) {
            menu_page = h->index;
            menu_sel = page_now()->n ? 0 : -1;
        } else if (menu_screen == MENU_SETTINGS) {   /* a setting: a step on (or back, left of its value) */
            menu_sel = h->index;
            item_change(&page_now()->items[h->index], h->kind == HIT_STEP_BACK ? -1 : 1);
        } else {
            menu_sel = h->index;
            menu_input(0, 0, 0, 0, 1, 0);
        }
        return;
    }
}

/* the mouse over a line highlights it */
static void menu_hover(float rx, float ry) {
    for (int i = 0; i < n_menu_hits; i++) {
        const MenuHit *h = &menu_hits[i];
        if (h->kind == HIT_ROW && rx >= h->x0 && ry >= h->y0 && rx < h->x1 && ry < h->y1) {
            menu_sel = h->index;
            return;
        }
    }
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
/* Diagnostics for psiv.log. The game makes a frame of sound at a time and
 * SDL's stream holds it until the output device takes it. An underrun is the
 * device asking for sound the stream doesn't have yet: a gap, heard as a
 * click. Counted on SDL's audio thread while the game is playing (it pushed
 * sound within the last 250 ms: not paused, in a menu or loading). */
static SDL_AtomicInt audio_last_push_ms, audio_underruns, audio_missing, audio_starved;
static void SDLCALL audio_requested(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)s; (void)total;
    if (additional <= 0) return;
    if ((int)SDL_GetTicks() - SDL_GetAtomicInt(&audio_last_push_ms) > 250) return;
    SDL_AddAtomicInt(&audio_underruns, 1);
    SDL_AddAtomicInt(&audio_missing, additional);
    SDL_SetAtomicInt(&audio_starved, 1);
}

/* The queue: the game keeps audio_target bytes ahead of the device, a
 * cushion for a frame that runs late (set from the device's buffer when the
 * stream opens). The game's clock (the PC's timer) and the device's own
 * crystal never run at quite the same speed -- Desk's Focusrite drifts about
 * 0.02% -- so the queue would creep until it ran dry or grew too long. The
 * stream's playback speed is nudged to hold it at the target instead: at
 * most 0.5% (8 cents) when far off, in practice a few hundredths of a
 * percent, which no one can hear. */
static int audio_target = SOUND_RATE * 4 / 20;          /* bytes; 50 ms until the device says */
static double audio_level = -1.0;                       /* the queue before each push, smoothed */
static float audio_ratio = 1.0f;

static struct {
    int pushes, drops, primes, qmin, qmax;
    double qsum;
    float rmin, rmax;
    uint64_t since;
} astat;

/* a line in psiv.log every 30 s that had a gap, a drop or a refill
 * (PSIV_AUDIO_LOG=1: every 5 s, whatever happened) */
static void audio_report(void) {
    static int every_ms = -1;
    if (every_ms < 0) every_ms = SDL_getenv("PSIV_AUDIO_LOG") ? 5000 : 30000;
    uint64_t now = SDL_GetTicks();
    if (!astat.since) astat.since = now;
    if (now - astat.since < (uint64_t)every_ms) return;
    int under = SDL_SetAtomicInt(&audio_underruns, 0), missing = SDL_SetAtomicInt(&audio_missing, 0);
    if (under || astat.drops || astat.primes > 1 || every_ms == 5000) {
        double ms = 1000.0 / (SOUND_RATE * 4.0);
        SDL_Log("audio: %d underruns (%.0f ms of sound missing), %d frames dropped, %d refills; queue %.0f / %.0f / "
                "%.0f ms (least / average / most; target %.0f); speed %+.3f%% to %+.3f%%; over %.0f s",
                under, missing * ms, astat.drops, astat.primes, astat.qmin * ms,
                astat.pushes ? astat.qsum / astat.pushes * ms : 0.0, astat.qmax * ms, audio_target * ms,
                (astat.rmin - 1.0f) * 100.0f, (astat.rmax - 1.0f) * 100.0f, (double)(now - astat.since) / 1000.0);
    }
    memset(&astat, 0, sizeof astat);
    astat.since = now;
}

/* fill the queue up to the target with silence: when sound starts, after a
 * pause, a menu or a long stall, and after fast forward */
static void audio_prime(int queued) {
    static const int16_t silence[2 * 1024];
    int want = (audio_target - queued) & ~3;
    while (want > 0) {
        int b = want > (int)sizeof silence ? (int)sizeof silence : want;
        SDL_PutAudioStreamData(audio, silence, b);
        want -= b;
    }
    audio_level = audio_target;
    audio_ratio = 1.0f;
    SDL_SetAudioStreamFrequencyRatio(audio, 1.0f);
    astat.primes++;
}

void platform_audio(const int16_t *samples, int count) {
    if (!audio || count <= 0 || paused) return;
    static int16_t faded[2 * 8192];
    float vol = rewind_volume();
    if (vol < 0.999f && count <= 8192) {                  /* a rewind fades the sound too */
        for (int i = 0; i < 2 * count; i++) faded[i] = (int16_t)((float)samples[i] * vol);
        samples = faded;
    }
    int queued = SDL_GetAudioStreamQueued(audio), bytes = count * 4;
    int now = (int)SDL_GetTicks();
    int idle = now - SDL_GetAtomicInt(&audio_last_push_ms) > 250;   /* the first sound, or after a pause or a menu */
    if (!astat.pushes || queued < astat.qmin) astat.qmin = queued;
    if (queued > astat.qmax) astat.qmax = queued;
    astat.qsum += queued;
    if (!astat.pushes || audio_ratio < astat.rmin) astat.rmin = audio_ratio;
    if (!astat.pushes || audio_ratio > astat.rmax) astat.rmax = audio_ratio;
    astat.pushes++;
    SDL_SetAtomicInt(&audio_last_push_ms, now);
    static int ff_until;                                 /* ms: fast forward was held until then, and a little */
    if (held(IN_FF)) {                                   /* fast forward: some of it, the delay bounded */
        ff_until = now + 500;
        if (queued > 2 * audio_target) astat.drops++;
        else SDL_PutAudioStreamData(audio, samples, bytes);
        audio_report();
        return;
    }
    static int verbose = -1;                             /* PSIV_AUDIO_LOG=2: each refill and clear */
    if (verbose < 0) verbose = SDL_getenv("PSIV_AUDIO_LOG") && SDL_atoi(SDL_getenv("PSIV_AUDIO_LOG")) >= 2;
    if (now < ff_until && queued > audio_target * 3 / 2) {   /* fast forward just ended: back to the cushion */
        if (verbose) SDL_Log("audio: cleared at %d ms queued (frame %llu)", queued * 1000 / (SOUND_RATE * 4),
                             (unsigned long long)frame_count);
        SDL_ClearAudioStream(audio);
        queued = 0;
        idle = 1;
        ff_until = 0;
    } else if (queued > 4 * audio_target) {             /* the device stopped taking sound: keep the delay bounded */
        astat.drops++;
        audio_report();
        return;
    }
    /* starting, or the device ran dry (a long stall): the gap happened
     * anyway; refill the cushion so it doesn't happen again and again */
    int starved = SDL_SetAtomicInt(&audio_starved, 0);
    if (idle || (starved && queued < bytes)) {
        if (verbose) SDL_Log("audio: refill at %d ms queued (frame %llu, %s)", queued * 1000 / (SOUND_RATE * 4),
                             (unsigned long long)frame_count, idle ? "idle" : "ran dry");
        audio_prime(queued);
        queued = audio_target;
    }
    SDL_PutAudioStreamData(audio, samples, bytes);
    /* hold the queue at the target: faster when it's long, slower when it's short */
    audio_level = audio_level < 0 ? queued : audio_level + (queued - audio_level) * 0.02;
    double e = (audio_level - audio_target) / audio_target;
    e = e > 1.0 ? 1.0 : e < -1.0 ? -1.0 : e;
    float ratio = (float)(1.0 + 0.005 * e);
    if (ratio != audio_ratio) {
        audio_ratio = ratio;
        SDL_SetAudioStreamFrequencyRatio(audio, ratio);
    }
    audio_report();
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
            menu_last_pad = 0;
            if (e.key.key == SDLK_F11 && !e.key.repeat) {   /* fullscreen, menus or not */
                int fs = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(window, fs);
                save_setting_int("video.fullscreen", fs);
                break;
            }
            if (menu_is_open()) {                        /* the menu takes the keyboard */
                if (sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_PAGEDOWN || sc == SDL_SCANCODE_PAGEUP) {
                    if (!e.key.repeat || sc != SDL_SCANCODE_TAB)    /* settings pages */
                        menu_turn_page(sc == SDL_SCANCODE_PAGEUP || (sc == SDL_SCANCODE_TAB && (e.key.mod & SDL_KMOD_SHIFT)) ? -1 : 1);
                    break;
                }
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
            if (sc == key_of[IN_REWIND]) rewind_now();
            if (e.key.key == SDLK_F9) cycle_battle_depth();
            if (e.key.key == SDLK_F8) cycle_battle_fog();
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            SDL_GamepadButton b = (SDL_GamepadButton)e.gbutton.button;
            touch_hide();                                /* a gamepad in use: the on-screen pad goes */
            menu_last_pad = 1;
            if (b == SDL_GAMEPAD_BUTTON_BACK || b == SDL_GAMEPAD_BUTTON_GUIDE) {   /* Select/Back opens it */
                if (in_launcher) menu_input(0, 0, 0, 0, 0, 1);
                else if (menu_is_open()) menu_close();
                else menu_open();
                break;
            }
            if (!menu_is_open()) {
                if (b == pad_of[IN_REWIND] && !in_launcher) rewind_now();
                break;
            }
            if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {   /* settings pages */
                menu_turn_page(b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER ? 1 : -1);
                break;
            }
            menu_input(b == SDL_GAMEPAD_BUTTON_DPAD_UP, b == SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                       b == SDL_GAMEPAD_BUTTON_DPAD_LEFT, b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
                       b == pad_of[IN_C] || b == pad_of[IN_START], b == pad_of[IN_B]);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {                /* click to walk (walkto.h), the menus */
            if (e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT) break;
            float rx, ry;
            menu_last_pad = 0;
            SDL_RenderCoordinatesFromWindow(renderer, e.button.x, e.button.y, &rx, &ry);
            click_at(rx, ry, e.button.button == SDL_BUTTON_RIGHT);
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {              /* the left stick steers the menus too */
            static int stick[2];                           /* each axis: -1, 0 or 1, with some hysteresis */
            int axis = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? 0 : e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? 1 : -1;
            if (axis < 0) break;
            int v = e.gaxis.value, now = v > 20000 ? 1 : v < -20000 ? -1 : abs(v) < 12000 ? 0 : stick[axis];
            if (now == stick[axis]) break;
            stick[axis] = now;
            if (!now || !menu_is_open()) break;
            menu_last_pad = 1;
            if (axis == 0) menu_input(0, 0, now < 0, now > 0, 0, 0);
            else menu_input(now < 0, now > 0, 0, 0, 0, 0);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:                       /* the menu: the line under the mouse */
            if (menu_is_open() && e.motion.which != SDL_TOUCH_MOUSEID) {
                float rx, ry;
                SDL_RenderCoordinatesFromWindow(renderer, e.motion.x, e.motion.y, &rx, &ry);
                menu_hover(rx, ry);
            }
            break;
        case SDL_EVENT_RENDER_DEVICE_RESET:                /* every texture lost (phones): made again */
            if (menu_tex) SDL_DestroyTexture(menu_tex);
            if (texture) SDL_DestroyTexture(texture);
            if (depth_texture) SDL_DestroyTexture(depth_texture);
            if (emblem_tex) SDL_DestroyTexture(emblem_tex);
            if (halo_tex) SDL_DestroyTexture(halo_tex);
            menu_tex = texture = depth_texture = emblem_tex = halo_tex = NULL;
            menu_tex_w = tex_w = depth_tex_w = emblem_n = 0;
            break;
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

/* testing: --menu-script STEPS, one step per pass of the menu's loop: a key
 * by SDL name ("Down", "Tab", "Shift+Tab", "Return", "Escape"), click:X:Y and
 * move:X:Y (render pixels), pad:BUTTON (SDL's name, e.g. rightshoulder),
 * shot:FILE.bmp; then the program ends */
static void menu_script_step(void) {
    static const char *p;
    static char shot_path[300];
    if (!p) p = test_menu_script;
    if (!*p) quit(0);
    char tok[300];
    size_t n = strcspn(p, ",");
    snprintf(tok, sizeof tok, "%.*s", (int)n, p);
    p += n + (p[n] == ',');
    SDL_Event e;
    SDL_zero(e);
    float x, y;
    if (!strncmp(tok, "shot:", 5)) {
        snprintf(shot_path, sizeof shot_path, "%s", tok + 5);
        test_shot = shot_path;
    } else if (sscanf(tok, "click:%f:%f", &x, &y) == 2) {
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = true;
        e.button.windowID = SDL_GetWindowID(window);
        SDL_RenderCoordinatesToWindow(renderer, x, y, &e.button.x, &e.button.y);
        SDL_PushEvent(&e);
    } else if (sscanf(tok, "move:%f:%f", &x, &y) == 2) {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = SDL_GetWindowID(window);
        SDL_RenderCoordinatesToWindow(renderer, x, y, &e.motion.x, &e.motion.y);
        SDL_PushEvent(&e);
    } else if (!strncmp(tok, "pad:", 4)) {             /* a gamepad button by SDL name (with --test-rumble's pad) */
        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        e.gbutton.which = pad ? SDL_GetGamepadID(pad) : 0;
        e.gbutton.button = (Uint8)SDL_GetGamepadButtonFromString(tok + 4);
        e.gbutton.down = true;
        SDL_PushEvent(&e);
    } else {
        int shift = !strncmp(tok, "Shift+", 6);
        SDL_Scancode sc = SDL_GetScancodeFromName(shift ? tok + 6 : tok);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.scancode = sc;
        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.mod = shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
        e.key.down = true;
        e.key.windowID = SDL_GetWindowID(window);
        if (sc == SDL_SCANCODE_UNKNOWN) SDL_Log("menu script: no key %s", tok);
        else SDL_PushEvent(&e);
    }
}

void platform_frame(const uint32_t *pixels, int w, int h) {
    handle_events(pixels, w, h);
    if (test_menu_frame && frame_count == test_menu_frame) {
        menu_open();
        menu_opened = 0;                                 /* no fade: a capture shows it at once */
        if (test_menu_settings) {
            menu_screen = MENU_SETTINGS;
            menu_page = test_menu_page >= 0 && test_menu_page < N_PAGES ? test_menu_page : 0;
            menu_sel = test_menu_row >= -1 && test_menu_row < page_now()->n ? test_menu_row : page_now()->n ? 0 : -1;
        }
    }
    if (paused || menu_is_open()) rumble_off();          /* the game holds: so does the rumble */
    while (paused || menu_is_open()) {                   /* F10 or the menu: hold here, keep the window alive */
        holding = 1;                                     /* (the watchdog knows it's on purpose) */
        present(pixels, w, h, 0);
        rumble_output();                                 /* the Rumble setting's sample */
        SDL_Delay(16);
        handle_events(pixels, w, h);
        if (test_menu_script && menu_is_open()) menu_script_step();
        next_deadline = SDL_GetTicksNS();
    }
    holding = 0;
    if (test_rumble_frame && frame_count == test_rumble_frame) rumble_test();
    if (test_rewind_frame && frame_count == test_rewind_frame) rewind_now();   /* testing: Backspace then */
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

/* ------------------------------------------------------------------ the launcher */
/* What the app opens on: Start game, the mode (Enhanced or Classic),
 * Settings, Quit -- over a slowly drifting field of stars, under the port's
 * emblem: the four-point star of its icon (tools/make_icon.py), drawn here at
 * full size from the same shape. The game starts only with Start game, and
 * only with a ROM: if none is installed yet, that's when it's asked for. */

/* the icon's star: |x/a|^p + |y/b|^p < 1, white at the heart to gold at the tips */
static void star_ramp(float t, float *r, float *g, float *b) {
    static const float T[5] = {0.00f, 0.12f, 0.32f, 0.60f, 1.00f};
    static const float C[5][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 182}, {255, 218, 72}, {255, 182, 0}};
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    for (int i = 0; i < 4; i++)
        if (t <= T[i + 1]) {
            float k = (t - T[i]) / (T[i + 1] - T[i]);
            *r = C[i][0] + (C[i + 1][0] - C[i][0]) * k;
            *g = C[i][1] + (C[i + 1][1] - C[i][1]) * k;
            *b = C[i][2] + (C[i + 1][2] - C[i][2]) * k;
            return;
        }
}

#define EMBLEM_K    0.75f            /* the icon's star at this size in its square: room for the halo */
#define EMBLEM_SIDE 88.9f            /* the square, units: the star 56 tall */

/* the star and its two sparkles (supersampled), and a soft gold halo on its own so it can breathe */
static void emblem_make(int n) {
    if (n < 16) n = 16;
    if (emblem_tex && n == emblem_n) return;
    if (emblem_tex) SDL_DestroyTexture(emblem_tex);
    if (halo_tex) SDL_DestroyTexture(halo_tex);
    emblem_tex = halo_tex = NULL;
    emblem_n = n;
    uint32_t *px = malloc((size_t)n * (size_t)n * 4);
    if (!px) return;
    static const struct { float cx, cy, a, b, alpha; int lit; } STARS[3] = {
        {0.0f, 0.0f, 0.70f * EMBLEM_K, 0.84f * EMBLEM_K, 1.0f, 1},
        {0.50f * EMBLEM_K, -0.52f * EMBLEM_K, 0.16f * EMBLEM_K, 0.20f * EMBLEM_K, 0.95f, 0},
        {-0.52f * EMBLEM_K, 0.55f * EMBLEM_K, 0.11f * EMBLEM_K, 0.14f * EMBLEM_K, 0.80f, 0},
    };
    const int ss = 3;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float r = 0, g = 0, b = 0, w = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    float u = ((float)x + ((float)sx + 0.5f) / (float)ss) / (float)n * 2.0f - 1.0f;
                    float v = ((float)y + ((float)sy + 0.5f) / (float)ss) / (float)n * 2.0f - 1.0f;
                    for (int k = 0; k < 3; k++) {
                        float du = (u - STARS[k].cx) / STARS[k].a, dv = (v - STARS[k].cy) / STARS[k].b;
                        if (fabsf(du) >= 1.0f || fabsf(dv) >= 1.0f) continue;
                        if (powf(fabsf(du), 0.46f) + powf(fabsf(dv), 0.46f) >= 1.0f) continue;
                        float cr = 255.0f, cg = 255.0f, cb = 218.0f;            /* the sparkles: pale gold */
                        if (STARS[k].lit) star_ramp(powf(hypotf(du, dv), 0.8f), &cr, &cg, &cb);
                        r += cr * STARS[k].alpha;
                        g += cg * STARS[k].alpha;
                        b += cb * STARS[k].alpha;
                        w += STARS[k].alpha;
                        break;
                    }
                }
            uint32_t out = 0;
            if (w > 0.0f)
                out = (uint32_t)(w / (float)(ss * ss) * 255.0f + 0.5f) << 24 | (uint32_t)(r / w) << 16 |
                      (uint32_t)(g / w) << 8 | (uint32_t)(b / w);
            px[(size_t)y * (size_t)n + (size_t)x] = out;
        }
    emblem_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, n, n);
    if (emblem_tex) {
        SDL_UpdateTexture(emblem_tex, NULL, px, n * 4);
        SDL_SetTextureBlendMode(emblem_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(emblem_tex, SDL_SCALEMODE_LINEAR);
    }
    int hn = n / 4 < 32 ? 32 : n / 4;                /* the halo: a gaussian, small and smoothed up */
    const float sg = 0.36f, edge = expf(-1.0f / (2.0f * sg * sg));
    for (int y = 0; y < hn; y++)
        for (int x = 0; x < hn; x++) {
            float u = ((float)x + 0.5f) / (float)hn * 2.0f - 1.0f, v = ((float)y + 0.5f) / (float)hn * 2.0f - 1.0f;
            float d2 = u * u + v * v, h = d2 >= 1.0f ? 0.0f : (expf(-d2 / (2.0f * sg * sg)) - edge) / (1.0f - edge);
            px[(size_t)y * (size_t)hn + (size_t)x] = (uint32_t)(h * 0.42f * 255.0f + 0.5f) << 24 | 0xFFD45Au;
        }
    halo_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, hn, hn);
    if (halo_tex) {
        SDL_UpdateTexture(halo_tex, NULL, px, hn * 4);
        SDL_SetTextureBlendMode(halo_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(halo_tex, SDL_SCALEMODE_LINEAR);
    }
    free(px);
}

/* the stars behind: twinkling, drifting slowly (the near ones faster); the
 * brightest with four small points, like the emblem's */
typedef struct { float x, y, size, drift, phase, rate; float r, g, b; } BgStar;
#define N_BG_STARS 240
static BgStar bg_stars[N_BG_STARS];

static void stars_init(void) {
    uint32_t s = 0x2545F491u;
    for (int i = 0; i < N_BG_STARS; i++) {
        BgStar *st = &bg_stars[i];
        float f[6];
        for (int k = 0; k < 6; k++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            f[k] = (float)(s & 0xFFFFFF) / 16777216.0f;
        }
        st->x = f[0];
        st->y = f[1];
        st->size = 0.25f + 1.0f * f[2] * f[2] * f[2];            /* mostly small */
        st->drift = 0.0012f + 0.004f * f[2];                       /* of the width, a second */
        st->phase = f[3] * 6.2831853f;
        st->rate = 0.5f + 1.8f * f[4];
        st->r = 1.0f; st->g = 1.0f; st->b = 1.0f;
        if (f[5] < 0.12f) { st->g = 0.89f; st->b = 0.63f; }       /* warm */
        else if (f[5] > 0.55f) { st->r = 0.75f; st->g = 0.82f; }   /* cool */
    }
}

static void quad(SDL_Vertex *v, int *ix, int *nv, int *ni, float x0, float y0, float x1, float y1, SDL_FColor c) {
    static const int Q[6] = {0, 1, 2, 0, 2, 3};
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    for (int k = 0; k < 4; k++) {
        v[*nv + k].position.x = xs[k];
        v[*nv + k].position.y = ys[k];
        v[*nv + k].color = c;
        v[*nv + k].tex_coord.x = v[*nv + k].tex_coord.y = 0.0f;
    }
    for (int k = 0; k < 6; k++) ix[*ni + k] = *nv + Q[k];
    *nv += 4;
    *ni += 6;
}

/* deep space, the stars, the emblem at (ex, ey) render pixels; S render pixels per unit, t seconds */
static void launcher_backdrop(int ow, int oh, float S, float t, float ex, float ey) {
    static SDL_Vertex v[N_BG_STARS * 12];
    static int ix[N_BG_STARS * 18];
    int nv = 0, ni = 0;
    SDL_FColor top = {0.047f, 0.086f, 0.25f, 1.0f}, low = {0.008f, 0.012f, 0.047f, 1.0f};
    quad(v, ix, &nv, &ni, 0, 0, (float)ow, (float)oh, top);
    v[2].color = v[3].color = low;                    /* a navy sky, darker below */
    SDL_RenderGeometry(renderer, NULL, v, nv, ix, ni);
    nv = ni = 0;
    for (int i = 0; i < N_BG_STARS; i++) {
        const BgStar *st = &bg_stars[i];
        float x = st->x - t * st->drift;
        x = (x - floorf(x)) * (float)ow;
        float y = st->y * (float)oh, tw = 0.5f + 0.5f * sinf(t * st->rate + st->phase);
        float h = fmaxf(0.6f, st->size * S * 0.32f);
        SDL_FColor c = {st->r, st->g, st->b, (0.25f + 0.75f * tw) * (0.35f + 0.65f * fminf(1.0f, st->size))};
        quad(v, ix, &nv, &ni, x - h, y - h, x + h, y + h, c);
        if (st->size > 0.9f) {
            float L = h * 4.0f * (0.7f + 0.3f * tw), w = fmaxf(0.5f, h * 0.3f);
            c.a *= 0.55f;
            quad(v, ix, &nv, &ni, x - L, y - w, x + L, y + w, c);
            quad(v, ix, &nv, &ni, x - w, y - L, x + w, y + L, c);
        }
    }
    SDL_RenderGeometry(renderer, NULL, v, nv, ix, ni);
    float side = EMBLEM_SIDE * S;
    emblem_make((int)ceilf(side));
    if (halo_tex) {
        float hs = side * 1.9f;
        SDL_FRect d = {ex - hs / 2, ey - hs / 2, hs, hs};
        SDL_SetTextureAlphaModFloat(halo_tex, 0.75f + 0.25f * sinf(t * 1.3f + 0.8f));
        SDL_RenderTexture(renderer, halo_tex, NULL, &d);
    }
    if (emblem_tex) {
        float es = side * (1.0f + 0.015f * sinf(t * 1.3f));
        SDL_FRect d = {ex - es / 2, ey - es / 2, es, es};
        SDL_RenderTexture(renderer, emblem_tex, NULL, &d);
    }
}

/* the launcher's picture; shade: black over it, 0..1 (the fades in and out) */
static void launcher_present(uint64_t t0, float shade) {
    int ow, oh;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    float S = launch_scale(ow, oh), oy = ((float)oh / S - 224.0f) / 2.0f;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    launcher_backdrop(ow, oh, S, (float)(SDL_GetTicks() - t0) / 1000.0f, (float)ow / 2.0f, (oy + LAUNCH_EMBLEM_Y) * S);
    menu_draw();
    if (shade > 0.0f) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(fminf(shade, 1.0f) * 255.0f));
        SDL_FRect all = {0, 0, (float)ow, (float)oh};
        SDL_RenderFillRect(renderer, &all);
    }
    test_shots();
    SDL_RenderPresent(renderer);
}

/* until Start game is chosen with a ROM in hand (Quit ends the program) */
static void launcher(void) {
    in_launcher = 1;
    touch_suspend(1);                                /* no on-screen pad here: every touch a tap */
    menu_screen = MENU_LAUNCHER;
    menu_sel = L_START;
    menu_opened = 0;
    stars_init();
    uint64_t t0 = SDL_GetTicks();
    for (;;) {
        handle_events(NULL, 0, 0);
        if (launcher_go) {
            launcher_go = 0;
            if (rom_ensure()) break;                 /* the ROM's there (or just chosen): off we go */
        }
        if (test_menu_script) menu_script_step();
        rumble_output();                             /* the Rumble setting's sample */
        float in = test_menu_script ? 1.0f : fminf(1.0f, (float)(SDL_GetTicks() - t0) / 450.0f);
        launcher_present(t0, 1.0f - in);             /* out of black (a test's shots: at once) */
        SDL_Delay(16);
    }
    for (uint64_t f0 = SDL_GetTicks(); !test_menu_script;) {   /* into black, then the game */
        float k = (float)(SDL_GetTicks() - f0) / 260.0f;
        if (k >= 1.0f) break;
        launcher_present(t0, k);
        SDL_Delay(16);
    }
    touch_suspend(0);
    in_launcher = 0;
    menu_screen = MENU_CLOSED;
    input_hold = 1;                                  /* the button that started it isn't the game's */
}

/* ------------------------------------------------------------------ main */
/* the console's reset, on the game's own stack (game_run) */
static void game_reset(void) {
    m68k_call(rd32(4));
}

int main(int argc, char **argv) {
    int play_now = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_arg = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_arg = argv[++i];
        else if (!strcmp(argv[i], "--play")) play_now = 1;              /* no launcher: straight into the game */
        /* testing: --menu F[:settings[:PAGE[:ROW]]] opens the system menu at
         * frame F (Settings on that page, that row highlighted; -1 the page
         * bar); --capture F:FILE.bmp saves the rendered window at frame F and
         * exits (run with SDL_VIDEO_DRIVER=offscreen to keep it off the screen);
         * --test-keys F:KEYS:N,... holds keys by frame; --test-click F:X,Y
         * clicks the frame's pixel (X, Y) through the window, as a mouse would */
        else if (!strcmp(argv[i], "--menu") && i + 1 < argc) {
            char *e;
            test_menu_frame = strtoull(argv[++i], &e, 10);
            test_menu_settings = *e == ':';
            const char *p = test_menu_settings ? strchr(e + 1, ':') : NULL;
            if (p) {
                test_menu_page = atoi(p + 1);
                p = strchr(p + 1, ':');
                if (p) test_menu_row = atoi(p + 1);
            }
        } else if (!strcmp(argv[i], "--menu-script") && i + 1 < argc) {   /* with --menu: see menu_script_step */
            test_menu_script = argv[++i];
        } else if (!strcmp(argv[i], "--capture") && i + 1 < argc) {
            char *e;
            test_capture_frame = strtoull(argv[++i], &e, 10);
            test_capture_path = *e == ':' ? e + 1 : "capture.bmp";
        } else if (!strcmp(argv[i], "--test-rewind") && i + 1 < argc) {
            test_rewind_frame = strtoull(argv[++i], NULL, 10);
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
        else if (!strcmp(argv[i], "--test-ff") && i + 1 < argc) {   /* F:N: fast forward held N frames from F */
            char *e;
            test_ff_frame = strtoull(argv[++i], &e, 10);
            test_ff_len = *e == ':' ? strtoull(e + 1, NULL, 10) : 60;
        }
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

    /* the launcher first -- unless told to play at once (--play), or a test
     * drives the game from its first frame (--menu, --test-keys, --test-click,
     * --test-rumble, or --capture without a --menu-script for the launcher) */
    rom_look();                                          /* the installed ROM, or one dropped in the folder */
    if (!play_now && !test_menu_frame && !n_test_keys && !test_click_frame && !test_rumble_frame &&
        (!test_capture_path || test_menu_script))
        launcher();                                      /* until Start game, with a ROM */
    else if (!rom_ensure())
        quit(0);

    /* the game's native rate; SDL resamples to the output device */
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, SOUND_RATE};
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) {
        SDL_SetAudioStreamGain(audio, (float)setting_int("audio.volume") / 100.0f);
        SDL_SetAudioStreamGetCallback(audio, audio_requested, NULL);
        SDL_AudioSpec dev;
        int dev_frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(audio), &dev, &dev_frames)) {
            /* the cushion: twice the device's buffer and a frame, 50 ms at least */
            double buf_ms = dev.freq > 0 ? 1000.0 * dev_frames / dev.freq : 10.0, want_ms = 2.0 * buf_ms + 17.0;
            if (want_ms < 50.0) want_ms = 50.0;
            audio_target = (int)(want_ms / 1000.0 * SOUND_RATE) * 4;
            SDL_Log("audio: %s, %d Hz, %d channels, %d-frame buffer (%.1f ms); %.0f ms kept queued",
                    SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(audio)), dev.freq, dev.channels, dev_frames,
                    buf_ms, want_ms);
        }
        SDL_ResumeAudioStreamDevice(audio);
    } else {
        SDL_Log("no audio: %s", SDL_GetError());
    }

    bus_init(rom_buf, rom_size);
    sram_load();
    SDL_Log("side panels: %d portraits from the ROM", panels_init());
    next_deadline = SDL_GetTicksNS();
    SDL_DetachThread(SDL_CreateThread(watchdog, "psiv-watchdog", NULL));

    m68k.s = 1;
    m68k.imask = 7;
    A(7) = rd32(0);
    diag_stack_top = A(7);
    game_run(game_reset);                              /* on its own stack (rewind.h); never returns */
    quit(1);
    return 1;
}
