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
#include "smooth.h"
#include "scalex.h"
#include "panels.h"
#include "battleui.h"
#include "fieldmenu.h"
#include "algol.h"
#include "music.h"
#include "fpview.h"
#include "walkto.h"
#include "worldmap.h"
#include "titleart.h"
#include "perf.h"
#include "par.h"
#include "savepages.h"
#include "autosave.h"
#include "arena.h"
#include "arenapic.h"
#include "wave.h"
#include "rewind.h"
#include "weather.h"
#include "fieldfx.h"
#include "battlevox.h"
#include "scenevox.h"
#include "cheats.h"
#include "journal.h"
#include "mixbus.h"
#include "rumble.h"
#ifndef _WIN32
#include "icon.h"
#endif
#include "launcher_logo.h"
#include "sfx.h"
#include "diag.h"
#include "frame.h"
#include "m68k.h"
#include "sha256.h"
#include "sound.h"
#include "touch.h"
#include "discord_mark.h"
#include "uikit.h"
#include "unzip.h"
#include "clip.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static char font_path[1100];          /* the modern text's font, as loaded (the settings' previews draw with it too) */

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

/* a voiced scene's clip, by name, from the home folder's voices/scenes */
static void *scene_clip(const char *name, size_t *len) {
    char path[1200], rel[400];
    snprintf(rel, sizeof rel, "voices/scenes/%s", name);
    path_in_home(path, sizeof path, rel);
    size_t n = 0;
    void *d = SDL_LoadFile(path, &n);
    if (!d) return NULL;
    void *copy = malloc(n);                              /* the caller frees it with free() */
    if (copy) memcpy(copy, d, n);
    SDL_free(d);
    *len = n;
    return copy;
}

/* ------------------------------------------------------------------ the voice pack
 * The voiced scenes' pack (scenevox.h) is a .zip of voices/scenes -- its index,
 * scenes.txt, and the clips it names -- unpacked into the home folder: chosen
 * in Settings > Sound > Voice pack, or dropped into the home folder (installed
 * at the start; voices/scenes/pack.txt remembers the .zip, so it is unpacked
 * once). Only .wav and .txt files under the folder holding scenes.txt are
 * written, each name checked to stay inside voices/scenes. */
#define PACK_FILE_MAX (16u << 20)            /* the biggest file a pack may hold */

typedef struct { char prefix[300]; size_t plen; int files; char err[400]; } PackJob;

static int pack_file(const char *name, size_t nlen, const uint8_t *data, size_t size, void *ud) {
    PackJob *j = ud;
    if (nlen <= j->plen || SDL_strncasecmp(name, j->prefix, j->plen)) return 1;      /* not in the pack's folder */
    char rel[300];
    size_t rlen = nlen - j->plen;
    if (rlen >= sizeof rel) return 1;
    memcpy(rel, name + j->plen, rlen);
    rel[rlen] = 0;
    for (char *c = rel; *c; c++)
        if (*c == '\\') *c = '/';
    const char *dot = strrchr(rel, '.');
    if (rel[0] == '/' || strstr(rel, "..") || strchr(rel, ':') || !dot ||
        (SDL_strcasecmp(dot, ".wav") && SDL_strcasecmp(dot, ".txt")))
        return 1;                                                                    /* never outside voices/scenes */
    char sub[400], path[1400];
    snprintf(sub, sizeof sub, "voices/scenes/%s", rel);
    path_in_home(path, sizeof path, sub);
    char *slash = strrchr(path, '/');
    if (slash) { *slash = 0; SDL_CreateDirectory(path); *slash = '/'; }
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(data, 1, size, f) == size;
    if (f && fclose(f)) ok = 0;
    if (!ok) { snprintf(j->err, sizeof j->err, "couldn't write %s", path); return 0; }
    j->files++;
    return 1;
}

/* the installed pack, loaded (again): the pages it voices */
static int load_scene_pack(void) {
    char idx[1200];
    path_in_home(idx, sizeof idx, "voices/scenes/scenes.txt");
    size_t n = 0;
    void *d = SDL_LoadFile(idx, &n);
    if (!d) return 0;
    scenevox_unload();
    int pages = scenevox_load(d, n, scene_clip);
    SDL_free(d);
    SDL_Log("voiced scenes: %d pages", pages);
    return pages;
}

/* a voice pack's .zip (a path, or the Android picker's content URI) into
 * voices/scenes, then loaded: the pages it voices; 0 if it failed, -1 if it
 * isn't a voice pack (the reason in err either way) */
static int voicepack_install(const char *path, char *err, size_t errn) {
    size_t n = 0;
    uint8_t *zip = SDL_LoadFile(path, &n);
    if (!zip) { snprintf(err, errn, "That file couldn't be read (%s).", SDL_GetError()); return 0; }
    char index[300];
    if (!unzip_is_zip(zip, n) || (!unzip_find(zip, n, "scenes/scenes.txt", index, sizeof index) &&
                                  !unzip_find(zip, n, "scenes.txt", index, sizeof index))) {
        SDL_free(zip);
        snprintf(err, errn, "That isn't a voice pack: a voice pack is a .zip with voices/scenes/scenes.txt in it.");
        return -1;
    }
    PackJob *j = calloc(1, sizeof *j);
    uint8_t *buf = malloc(PACK_FILE_MAX);
    char zerr[300] = "";
    int r = -1;
    if (j && buf) {
        j->plen = strlen(index) - strlen("scenes.txt");
        memcpy(j->prefix, index, j->plen);
        r = unzip_each(zip, n, buf, PACK_FILE_MAX, pack_file, j, zerr, sizeof zerr);
    } else {
        snprintf(zerr, sizeof zerr, "out of memory");
    }
    SDL_free(zip);
    free(buf);
    if (r < 0 || !j || j->err[0]) {
        snprintf(err, errn, "The voice pack couldn't be installed: %s.", j && j->err[0] ? j->err : zerr);
        free(j);
        return 0;
    }
    SDL_Log("voice pack: %d files from %s", j->files, path);
    free(j);
    int pages = load_scene_pack();
    if (pages <= 0) { snprintf(err, errn, "The voice pack was unpacked, but its index names no pages."); return 0; }
    return pages;
}

/* a voice pack's .zip dropped into the home folder (or beside the program)
 * installs itself at the start, once: 1 if one did (and is loaded) */
static int quip_pending;                       /* a pack unpacked itself at the start: a quip in the launcher */
static int voicepack_nearby(void) {
    char dirs[2][1100], mark[1200], last[700] = "";
    int nd = 0;
    snprintf(dirs[nd++], sizeof dirs[0], "%s", home);
    if (base_dir[0] && strcmp(base_dir, home)) snprintf(dirs[nd++], sizeof dirs[0], "%s", base_dir);
    path_in_home(mark, sizeof mark, "voices/scenes/pack.txt");
    FILE *f = fopen(mark, "rb");
    if (f) { size_t k = fread(last, 1, sizeof last - 1, f); last[k] = 0; fclose(f); }
    for (int d = 0; d < nd; d++) {
        int count = 0;
        char **names = SDL_GlobDirectory(dirs[d], "*.zip", SDL_GLOB_CASEINSENSITIVE, &count);
        if (!names) continue;
        for (int i = 0; i < count; i++) {
            char path[1400], id[700], err[700];
            SDL_PathInfo info;
            snprintf(path, sizeof path, "%s%s", dirs[d], names[i]);
            if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE || info.size < 1024 ||
                info.size > ((Sint64)1 << 30))
                continue;
            snprintf(id, sizeof id, "%s %lld\n", names[i], (long long)info.size);
            if (!strcmp(id, last)) continue;                                 /* unpacked already */
            int pages = voicepack_install(path, err, sizeof err);
            if (pages > 0) {
                f = fopen(mark, "wb");
                if (f) { fputs(id, f); fclose(f); }
                SDL_Log("voice pack: installed %s (%d pages)", path, pages);
                quip_pending = 1;
                SDL_free(names);
                return 1;
            }
            if (pages == 0) SDL_Log("voice pack: %s: %s", path, err);           /* (not a pack: quietly) */
        }
        SDL_free(names);
    }
    return 0;
}

/* a file of the sound library, for sfx_library_load (Android: a relative path reads the APK's assets) */
static void *sound_file_read(const char *path, long *size) {
    size_t n = 0;
    void *d = SDL_LoadFile(path, &n);
    *size = (long)n;
    return d;
}
static void sound_file_done(void *d) { SDL_free(d); }

/* the party's battle voices (battlevox.h): yours in the home folder's
 * voices folder first, else the port's beside the program (or the APK's
 * assets), else a development build's res/voices */
static void load_voices(void) {
    int yours = 0;
    for (int c = 0; c < VOX_CHARS; c++)
        for (int m = 0; m < VOX_MOMENTS; m++)
            for (int k = 0; k < VOX_TAKES; k++) {
                char name[64], path[1200];
                snprintf(name, sizeof name, "voices/%s_%s_%d.wav", battlevox_char_name(c), battlevox_moment_name(m), k + 1);
                const char *where[3] = {home, base_dir, home};
                const char *sub_[3] = {"", "", "res/"};
                for (int w = 0; w < 3; w++) {
                    snprintf(path, sizeof path, "%s%s%s", where[w], sub_[w], name);
                    size_t n = 0;
                    void *d = SDL_LoadFile(path, &n);
                    if (!d) continue;
                    int ok = battlevox_add(c, m, k, d, n);
                    SDL_free(d);
                    if (ok) { yours += w == 0; break; }
                }
            }
    SDL_Log("voices: %d takes (%d of yours)", battlevox_count(), yours);
    /* a voiced-scenes pack, if there is one: a .zip just dropped in, or the
     * one installed (voices/scenes/scenes.txt) */
    if (!voicepack_nearby()) load_scene_pack();
}

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
    {"saves.last", "0", "the save made last, 1 to 12 (0: none yet): the launcher's Continue goes to it"},
    {"arena.best", "", "the Boss Arena: each boss's quickest win in seconds, in story order (0: not beaten yet)"},
    {"saves.page", "1", "the page of saves in use, 1 to 4, three slots each: Left / Right in the game's Save window, and past the ends of its Continue and Erase windows, turn the page (Enhanced; Classic keeps to page 1)"},
    {"video.detail", "auto", "the port's own picture (battles, modern text and menus) is drawn at up to 4 times the game's size: auto = at 3 or 2 times on a machine that cannot draw it in a frame's time, so the game never slows down for it; full = always at the display's size"},
    {"video.fullscreen", "0", "1 = start fullscreen (F11 toggles)"},
    {"video.integer_scaling", INTEGER_SCALING_DEFAULT, "1 = whole-number scaling only (sharpest); 0 = fill the screen"},
    {"video.widescreen", "1", "16:9 widescreen (other screens keep their shape): 0 off, 1 more map in exploration, 2 side panels with the party in exploration; battles are wide either way"},
    {"video.aspect", "square", "square = square pixels; 4:3 = the console's TV shape (pixels a bit narrower)"},
    {"video.filter", "nearest", "nearest = sharp pixels; linear = smoothed; scale2x = the Scale2x / Scale3x filter: stair-steps rounded off, nothing blurred"},
    {"video.vsync", "off", "on = each picture waits for the display's refresh, so it never tears (on a display that refreshes 60 times a second or more); off = the game keeps its own time"},
    {"audio.volume", "100", "0-200 percent: everything"},
    {"audio.launcher_song", "A4", "the song the launcher plays, by its ID ($81-$B4; A4 = Her Last Breath): chosen in Bonus Modes > Jukebox"},
    {"audio.launcher_music", "on", "the launcher plays its song (Her Last Breath, or the one chosen in the Jukebox) from your ROM (the game's own sound, before the game starts; or your own file for song A4 in the music folder): on / off -- the note at the launcher's top right, M or the pad's north button switch it"},
    {"audio.music", "100", "the music, 0-100 percent: the game's songs (either chip) or your own files"},
    {"audio.effects", "100", "sound effects, 0-100 percent: the game's or the port's or yours, and the battle weather"},
    {"audio.voices", "100", "voices, 0-100 percent: the party's in battle and the voiced scenes"},
    {"enhance.mode", "enhanced", "enhanced = the port's additions below are on; classic = the original game, unchanged (Esc menu > Settings)"},
    {"enhance.battle_depth", "1", "battles drawn as layers at different depths under a gently moving camera: 0 off, 1 subtle, 2 strong (F9 cycles)"},
    {"enhance.battle_fog", "1", "drifting mist between the enemies and the party in battles: 0 off, 1 light, 2 heavy (F8 cycles)"},
    {"enhance.autosave", "on", "auto save: the game saved for you as you go -- each time the party comes to another place, and every few minutes on the way -- in a save of its own (saves/psiv-auto.srm) that the launcher's Continue and Load game take; your own saves are never touched: on / off"},
    {"enhance.rewind", "on", "rewind: Backspace or the left shoulder fades to white and back to about ten seconds before (a state kept every ten seconds, the last two minutes): on / off"},
    {"enhance.trees", "on", "trees on the field sway in the breeze, light flickering through their leaves: on / off"},
    {"enhance.water", "on", "the field's living water: ripples, light, glints and foam drawn over the sea, rivers and pools: on / off"},
    {"cheat.hp", "off", "cheat: everyone's HP kept full: on / off"},
    {"cheat.tp", "off", "cheat: everyone's TP kept full: on / off"},
    {"cheat.no_battles", "off", "cheat: no random battles (set battles still happen): on / off"},
    {"cheat.money", "off", "cheat: meseta kept at 999,999: on / off"},
    {"cheat.exp", "1", "cheat: experience from battles: 1 normal, 2 double, 4 quadruple"},
    {"cheat.weak", "off", "cheat: enemies have 1 HP: on / off"},
    {"enhance.voices", "on", "the party's voices in battle: shouts as they attack, grunts when hit, a breath as they cast: on / off"},
    {"enhance.scene_voices", "on", "the voiced story scenes, from the optional voice pack once it is installed: on / off"},
    {"enhance.battle_camera", "on", "when an enemy attacks someone, the view turns to face them: the party slides along instead of overlapping (needs battle depth): on / off"},
    {"enhance.battle_weather", "1", "battle weather by place: rain and storms on Motavia's grass, beaches and sea, snow on Dezolis, mist clouds on Rykros, debris in space: 0 off, 1 light, 2 heavy"},
    {"enhance.battle_weather_sound", "on", "the rain and thunder heard with the battle weather: on / off"},
    {"enhance.text", "modern", "text: modern = redrawn with the font below at full resolution; original = the game's pixel font"},
    {"enhance.text_font", "", "TrueType/OpenType font file for modern text; empty = Segoe UI on Windows, else the bundled Noto Sans"},
    {"enhance.sfx", "original", "sound effects: what plays the game's own: original = the chip; algol = the chip remastered: clean, in stereo, with room around them"},
    {"enhance.sfx_modern", "on", "modern sounds: on = use the modern sounds that are available, in place of the game's (your own in sfx/sfx.ini first, then the library that comes with the game); the rest play as enhance.sfx says; off = the game's own for all"},
    {"enhance.music", "original", "music: original = the YM2612 and PSG; algol = the chip remastered: the game's own instruments and drums, clean, in stereo, with reverb; proto = Proto Algol, the first Algol: the songs on new instruments of its own, with its own drums"},
    {"enhance.click_walk", "on", "mouse: on = a click on the field walks the party there (a person or a chest: up to it, then C to talk or open); right click stops; with text up a click is C, a right click B; touch: a tap is a click; off = clicks and taps only work the system menu"},
    {"enhance.diagonal", "on", "diagonal walking: on = two directions held walk the party diagonally, a cell at a time (not through a wall's corner; doors, exits and people are still met straight on); off = the game's four directions"},
    {"enhance.custom_music", "on", "music pack: on = audio files in the music folder play in place of the game's songs (music/8F.wav, or named in music/music.ini; see runtime/music.h); off = the game's"},
    {"enhance.battle_ui", "modern", "battle menus: modern = full names, descriptions and a party bar with portraits; original = the game's windows"},
    {"enhance.group_items", "on", "the modern menu's items: on = one row a kind of item, with how many (the pack is the game's as ever: 40 items, one a slot); off = one row each, as the game lists them"},
    {"enhance.field_menu", "modern", "the field menu: modern = the port's own, on the whole screen (the main menu and the equipment screen so far; the rest are still the game's windows); original = the game's windows"},
    {"enhance.battle_fx", "1", "attack and technique effects light the scene; sparks, flashes, embers, lens flares: 0 off, 1 subtle, 2 full"},
    {"keys.up", "Up", "keyboard keys use SDL key names (e.g. Up, Z, Return, Left Shift, Keypad 8); W, A, S and D move too, unless given another job here"},
    {"keys.down", "Down", ""},
    {"keys.left", "Left", ""},
    {"keys.right", "Right", ""},
    {"keys.a", "Z", ""},
    {"keys.b", "X", ""},
    {"keys.c", "C", ""},
    {"keys.start", "Return", ""},
    {"keys.fast_forward", "Tab", "hold to run unthrottled"},
    {"keys.rewind", "Backspace", "go back about ten seconds (enhance.rewind)"},
    {"keys.auto_text", "V", "turn auto-advance on or off: voiced scenes move on by themselves once each line is said"},
    {"keys.encounters", "E", "random battles off, or on again (the No random battles cheat; never in the Bonus Modes)"},
    {"keys.map", "M", "the world map, and away again (enhance.world_map)"},
    {"pad.a", "x", "gamepad buttons use SDL names: a (south), b (east), x (west), y (north), start, back, leftshoulder, rightshoulder"},
    {"pad.b", "a", ""},
    {"pad.c", "b", ""},
    {"pad.start", "start", ""},
    {"pad.fast_forward", "rightshoulder", ""},
    {"pad.rewind", "leftshoulder", ""},
    {"pad.auto_text", "y", ""},
    {"pad.encounters", "leftstick", "(leftstick = L3, the left stick pressed in)"},
    {"enhance.auto_text", "off", "auto-advance in voiced scenes (the north button or V turns it on and off): on / off"},
    {"enhance.world_map", "off", "a map of the planet you walk (Motavia, Dezolis, Rykros): off / explored (only where you have been) / all -- a small one in the corner, the whole map on its key (keys.map, pad.map) or a tap on the small one; click a place on it and the party walks there"},
    {"enhance.journal_notes", "on", "a note in the corner when the story moves on (the menu's Journal says what to do next): on / off"},
    {"pad.map", "rightstick", "(rightstick = R3, the right stick pressed in)"},
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
    int has_sfx_modern = 0;
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
        if (!SDL_strcasecmp(key, "enhance.sfx_modern")) has_sfx_modern = 1;
        for (int i = 0; i < NSETTINGS; i++)
            if (!SDL_strcasecmp(settings[i].key, key)) snprintf(settings[i].value, sizeof settings[i].value, "%s", val);
    }
    fclose(f);
    /* Sound effects was one setting (modern / custom / original) before it was two: a file from then
     * keeps what it had -- modern or custom is the modern sounds on, original is them off */
    for (int i = 0; i < NSETTINGS; i++) {
        if (strcmp(settings[i].key, "enhance.sfx")) continue;
        int was_modern = !SDL_strcasecmp(settings[i].value, "modern") || !SDL_strcasecmp(settings[i].value, "custom");
        if (was_modern) snprintf(settings[i].value, sizeof settings[i].value, "original");
        if (!has_sfx_modern)
            for (int k = 0; k < NSETTINGS; k++)
                if (!strcmp(settings[k].key, "enhance.sfx_modern"))
                    snprintf(settings[k].value, sizeof settings[k].value, "%s", was_modern ? "on" : "off");
    }
}

/* key bindings resolved from the settings */
enum { IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_A, IN_B, IN_C, IN_START, IN_FF, IN_REWIND, IN_AUTO, IN_ENCOUNTERS, IN_MAP, IN_COUNT };
static const char *in_names[IN_COUNT] = {"up", "down", "left", "right", "a", "b", "c", "start", "fast_forward",
                                         "rewind", "auto_text", "encounters", "map"};
static SDL_Scancode key_of[IN_COUNT];
static SDL_GamepadButton pad_of[IN_COUNT];
/* WASD, beside the four direction keys, wherever those work -- each unless psiv.ini gives that
 * letter another job */
static SDL_Scancode wasd_of[4];
static int dir_key(SDL_Scancode sc, int in) { return sc == key_of[in] || (sc != SDL_SCANCODE_UNKNOWN && sc == wasd_of[in]); }

static void resolve_bindings(void) {
    char k[64];
    for (int i = 0; i < IN_COUNT; i++) {
        snprintf(k, sizeof k, "keys.%s", in_names[i]);
        key_of[i] = SDL_GetScancodeFromName(setting(k));
        snprintf(k, sizeof k, "pad.%s", in_names[i]);
        pad_of[i] = *setting(k) ? SDL_GetGamepadButtonFromString(setting(k)) : SDL_GAMEPAD_BUTTON_INVALID;
    }
    static const SDL_Scancode WASD[4] = {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D};
    for (int d = 0; d < 4; d++) {
        wasd_of[d] = WASD[d];
        for (int i = 0; i < IN_COUNT; i++) if (i != d && key_of[i] == WASD[d]) wasd_of[d] = SDL_SCANCODE_UNKNOWN;
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
    if (in <= IN_RIGHT && wasd_of[in] != SDL_SCANCODE_UNKNOWN && k[wasd_of[in]]) return 1;
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

static int save_page;                       /* the page of saves in memory (savepages.h): 0 = the file itself */

/* the save's file; a further page's: the same name with -2, -3 ... before its ending */
static void sram_path(char *out, size_t n) {
    char base[1100];
    if (save_arg) snprintf(base, sizeof base, "%s", save_arg);
    else path_in_home(base, sizeof base, "saves/psiv.srm");
    if (!save_page) { snprintf(out, n, "%s", base); return; }
    char *dot = strrchr(base, '.');
    if (dot && (strchr(dot, '/') || strchr(dot, '\\'))) dot = NULL;    /* a dot in a folder's name */
    if (dot) snprintf(out, n, "%.*s-%d%s", (int)(dot - base), base, save_page + 1, dot);
    else snprintf(out, n, "%s-%d", base, save_page + 1);
}

/* The auto save (autosave.h): a file of its own beside the saves -- the same name with -auto before
 * its ending -- in the cartridge's layout, its one save in the first slot. The game's windows never
 * see it: to play on from it the launcher puts it in the cartridge's place for the moment the game
 * takes to load it (auto_loading), and the page of saves comes back after. */
#define AUTO_PAGE SAVEPAGES_MAX              /* as a page: one past the real ones */
static uint8_t auto_page[BUS_SRAM_SIZE];     /* as the launcher read it */
static int auto_ok;                          /* ... and it holds a save */
static int auto_loading;                     /* it is in the cartridge's place: nothing is written back */
static uint8_t auto_keep[BUS_SRAM_SIZE];     /* the page of saves meanwhile */
static uint64_t auto_note_until;             /* its note is shown until then (SDL_GetTicks) */
static void auto_path(char *out, size_t n) {
    char base[1100];
    if (save_arg) snprintf(base, sizeof base, "%s", save_arg);
    else path_in_home(base, sizeof base, "saves/psiv.srm");
    char *dot = strrchr(base, '.');
    if (dot && (strchr(dot, '/') || strchr(dot, '\\'))) dot = NULL;
    if (dot) snprintf(out, n, "%.*s-auto%s", (int)(dot - base), base, dot);
    else snprintf(out, n, "%s-auto", base);
}

/* the save's file into memory: its size, 0 if there is none */
static size_t sram_read(void) {
    char p[1100];
    sram_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    static uint8_t buf[2 * BUS_SRAM_SIZE];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    sram_format = bus_sram_import(buf, n);
    return n;
}

static void sram_load(void) {
    char p[1100];
    size_t n = sram_read();
    sram_path(p, sizeof p);
    if (n) SDL_Log("loaded %s (%u bytes, %s layout)", p, (unsigned)n, bus_sram_format_name(sram_format));
}

static int arena_mode;                     /* the Boss Arena: the cartridge holds a fight's save, never written */
static int wave_mode;                      /* ... and Wave Mode's runs (wave.h): such a session too */
static int arena_scanning, arena_scan_done, arena_scan_total;   /* its bosses' pictures being gathered, unseen */
static void sram_flush(void) {
    if (!bus_sram_dirty || auto_loading || arena_mode) return;
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

/* Turn to another page of saves (savepages.h): the one in memory to its
 * file, that one's file into memory (a page not saved to yet: empty). */
static void save_setting_int(const char *key, int v);
static int page_go(int page) {
    sram_flush();
    if (bus_sram_dirty) return 0;                    /* not written: stay on this page */
    save_page = page;
    memset(bus_sram, 0, sizeof bus_sram);
    sram_format = SRAM_PORT;
    sram_load();
    bus_sram_dirty = 0;
    save_setting_int("saves.page", page + 1);
    return 1;
}

/* The launcher's Continue: the last save made, wherever it is in the twelve
 * slots (savepages.h tells the port of each save: saves.last). With no such
 * record -- saves from before this, or brought in from an emulator -- the
 * page written last, and on it the slot the game used last. Classic keeps
 * to the first page. The pages are read here, before the game has the
 * cartridge: nothing of this stays in memory. */
static int classic_mode(void);
static struct { int ok, page, slot, level; uint32_t meseta; } cont;
static int launch_continue;                /* Continue was chosen: the game goes straight to that save */
static int continuing_now;                 /* ... and is on its way there, unseen (savepages_continuing) */

/* (the pages as continue_find read them: the launcher's Load game lists what each slot holds) */
static uint8_t save_pages[SAVEPAGES_MAX][BUS_SRAM_SIZE];
#define pg save_pages

static void continue_find(void) {
    int64_t when[SAVEPAGES_MAX] = {0};
    int n = classic_mode() ? 1 : SAVEPAGES_MAX, keep = save_page;
    memset(&cont, 0, sizeof cont);
    memset(save_pages, 0, sizeof save_pages);
    for (int k = 0; k < n; k++) {
        save_page = k;
        memset(bus_sram, 0, sizeof bus_sram);
        if (sram_read()) {
            char path[1100];
            SDL_PathInfo info;
            sram_path(path, sizeof path);
            if (SDL_GetPathInfo(path, &info)) when[k] = (int64_t)info.modify_time;
        }
        memcpy(pg[k], bus_sram, sizeof bus_sram);
    }
    auto_ok = 0;                                     /* the auto save, if there is one (Enhanced) */
    memset(auto_page, 0, sizeof auto_page);
    if (!classic_mode()) {
        char path[1100];
        auto_path(path, sizeof path);
        FILE *f = fopen(path, "rb");
        if (f) {
            static uint8_t buf[2 * BUS_SRAM_SIZE];
            size_t got = fread(buf, 1, sizeof buf, f);
            fclose(f);
            memset(bus_sram, 0, sizeof bus_sram);
            bus_sram_import(buf, got);
            memcpy(auto_page, bus_sram, sizeof bus_sram);
            auto_ok = savepages_saved(auto_page, 0);
        }
    }
    save_page = keep;
    memset(bus_sram, 0, sizeof bus_sram);
    bus_sram_dirty = 0;
    sram_format = SRAM_PORT;
    int page = -1, slot = -1, last = setting_int("saves.last") - 1;
    if (last == AUTO_PAGE * 3 && auto_ok) {          /* the auto save was the last one made */
        page = AUTO_PAGE;
        slot = 0;
    } else if (last >= 0 && last / 3 < n && savepages_saved(pg[last / 3], last % 3)) {
        page = last / 3;
        slot = last % 3;
    } else {
        for (int k = 0; k < n; k++) {
            int sl = pg[k][0x13] & 3;                        /* the slot the game saved to or loaded last */
            if (!savepages_saved(pg[k], sl))
                for (sl = 0; sl < 3 && !savepages_saved(pg[k], sl); sl++) {}
            if (sl < 3 && (page < 0 || when[k] > when[page])) { page = k; slot = sl; }
        }
    }
    if (page < 0 && auto_ok) { page = AUTO_PAGE; slot = 0; }   /* no save but the auto save */
    if (page < 0) return;
    const uint8_t *d = page == AUTO_PAGE ? &auto_page[0x100]  /* the slot: Chaz's level and the party's meseta */
                     : &pg[page][0x100 + slot * 0xA00];
    cont.ok = 1;
    cont.page = page;
    cont.slot = slot;
    cont.level = d[0x409];
    cont.meseta = (uint32_t)d[0x338] << 24 | (uint32_t)d[0x339] << 16 | (uint32_t)d[0x33A] << 8 | d[0x33B];
}
#undef pg

/* told of each save the game makes (savepages.h) */
static void page_saved(int page, int slot) { save_setting_int("saves.last", page * 3 + slot + 1); }

/* an auto save, made (autosave.h): to its file, and the launcher's Continue now goes to it (saves.last
 * 13: one past the twelve) */
static void auto_write(const uint8_t *made) {
    char p[1100], dir[1100];
    if (!save_arg) {
        path_in_home(dir, sizeof dir, "saves");
        SDL_CreateDirectory(dir);
    }
    auto_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) { SDL_Log("cannot write %s", p); return; }
    size_t n = fwrite(made, 1, AUTOSAVE_SIZE, f);
    fclose(f);
    if (n != AUTOSAVE_SIZE) { SDL_Log("cannot write %s", p); return; }
    if (setting_int("saves.last") != AUTO_PAGE * 3 + 1) save_setting_int("saves.last", AUTO_PAGE * 3 + 1);
    auto_note_until = SDL_GetTicks() + 2200;
}
/* the launcher's Continue or Load game took it: in the cartridge's place until the game has loaded it */
static void auto_swap_in(void) {
    memcpy(auto_keep, bus_sram, sizeof bus_sram);
    memcpy(bus_sram, auto_page, sizeof bus_sram);
    bus_sram_dirty = 0;
    auto_loading = 1;
}
static void auto_swap_out(void) {
    if (!auto_loading) return;
    memcpy(bus_sram, auto_keep, sizeof bus_sram);
    bus_sram_dirty = 0;
    auto_loading = 0;
    SDL_Log("auto save: loaded; page %d of the saves is back in the cartridge (its slots: %d %d %d)", save_page + 1,
            savepages_saved(bus_sram, 0), savepages_saved(bus_sram, 1), savepages_saved(bus_sram, 2));
}

/* the world map: what the party has explored (worldmap.h), kept beside the saves */
static void explored_load(void) {
    static uint8_t buf[WORLDMAP_FILE_SIZE];
    char path[1100];
    path_in_home(path, sizeof path, "saves/explored.bin");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    worldmap_explored_unpack(buf, n);
}
static void explored_store(void) {
    static uint8_t buf[WORLDMAP_FILE_SIZE];
    if (!worldmap_explored_dirty()) return;
    char path[1100];
    path_in_home(path, sizeof path, "saves");
    SDL_CreateDirectory(path);
    path_in_home(path, sizeof path, "saves/explored.bin");
    worldmap_explored_pack(buf);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
}

static void quit(int code) {
    sram_flush();
    explored_store();
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
        explored_store();
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

/* psiv --headless: the headless front end (headless_app.c) runs in place of the window */
#ifdef PSIV_NO_HEADLESS                              /* (Android: not built in) */
static int headless_main(int argc, char **argv) { (void)argc; (void)argv; return 2; }
static void headless_platform_frame(const uint32_t *pixels, int width, int height) { (void)pixels; (void)width; (void)height; }
static void headless_platform_audio(const int16_t *samples, int count) { (void)samples; (void)count; }
static void headless_platform_fatal(const char *text) { (void)text; }
#else
int headless_main(int argc, char **argv);
void headless_platform_frame(const uint32_t *pixels, int width, int height);
void headless_platform_audio(const int16_t *samples, int count);
void headless_platform_fatal(const char *text);
#endif
static int headless_mode;

void platform_fatal(const char *text) {
    if (headless_mode) { headless_platform_fatal(text); return; }
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

/* Settings > Sound > Voice pack: choose the pack's .zip, install it */
static const char *test_voicepack;       /* testing: --test-voice-pack FILE answers its file dialog */

static void voice_quip(void);                  /* (with the launcher's sound, below) */
static char quip_line[200];                    /* the last quip: who and what */
static uint64_t quip_until;                    /* SDL_GetTicks: its words are shown until then */
static uint64_t quip_direct_until;             /* ... and its sound, put straight in the stream, lasts until then */

static void voicepack_choose(void) {
    dialog_done = 0;
    dialog_path[0] = dialog_error[0] = 0;
    if (test_voicepack || no_dialogs) {
        if (test_voicepack) snprintf(dialog_path, sizeof dialog_path, "%s", test_voicepack);
        dialog_done = 1;
    } else {
#if defined(SDL_PLATFORM_ANDROID)
        SDL_ShowOpenFileDialog(on_pick, NULL, window, NULL, 0, NULL, false);
#else
        static const SDL_DialogFileFilter filters[] = {{"Voice pack (.zip)", "zip"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(on_pick, NULL, window, filters, 2, NULL, false);
#endif
    }
    while (!dialog_done) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) quit(0);
        SDL_Delay(10);
    }
    char msg[1600];
    if (dialog_error[0]) {                                   /* no picker on this system */
        snprintf(msg, sizeof msg, "A file picker couldn't open (%s).\n\nPut the voice pack's .zip in this folder and "
                 "start the game again: it installs itself.\n%s", dialog_error, home);
        message(SDL_MESSAGEBOX_ERROR, msg, window);
        return;
    }
    if (!dialog_path[0]) return;                             /* cancelled */
    int pages = voicepack_install(dialog_path, msg, sizeof msg);
    if (pages > 0) {
        snprintf(msg, sizeof msg, "The voice pack is installed: %d page%s of the story voiced.\n\nScene voices, on "
                 "this page, turns them on and off (they're off in Classic mode). The .zip you chose isn't needed "
                 "any more.", pages, pages == 1 ? "" : "s");
        message(SDL_MESSAGEBOX_INFORMATION, msg, window);
        voice_quip();
    } else {
        message(SDL_MESSAGEBOX_ERROR, msg, window);
    }
}

static const char *voicepack_label(void) { return scenevox_pages() > 0 ? "Installed" : "Install..."; }

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
static uint64_t test_map_frame;        /* --map F[:ZOOM:X:Y[:go]]: the world map opened at frame F (at that zoom step, that map pixel in
                                        * the middle; go: and the party sent there, as Enter does) */
static int test_map_zoom = -1, test_map_go;
static float test_map_x, test_map_y;
static int test_menu_settings, test_menu_journal, test_menu_page, test_menu_row;
static const char *test_menu_script;                                  /* --menu-script */
static uint64_t menu_script_passes;   /* its steps so far: the launcher's clock under a script (1/60 s a step), so
                                      * captures animate at the same pace however fast the shots are written */
static const char *test_shot;                                         /* its shot:FILE, taken at the next present */
/* For videos of the app. --record FILE: every picture it shows, raw (four bytes a pixel: blue, green, red, one
 * unused), one after another into FILE, their size in FILE.txt; written quickly enough for the app to keep its
 * real pace, so what it animates by the clock plays as it does for a player. --events LIST: steps as
 * --menu-script's (a key by SDL's name, click:X:Y, wheel:N, move:X:Y, pad:NAME; and quit), each with when:
 * pN:STEP at the Nth picture shown, fN:STEP at game frame N; taken in the order given. */
static const char *test_record_path, *test_events;
static FILE *test_record_file;
static uint64_t test_presents;
static void script_do(const char *tok);
static const char *test_capture_path;
static struct { uint64_t frame, len; uint8_t keys; } test_keys[4096]; /* --test-keys */
static int n_test_keys;
static uint64_t test_rewind_frame;    /* --test-rewind F: a rewind asked for at frame F */
static uint64_t test_click_frame;                                     /* --test-click */
static float test_click_x, test_click_y;
static uint64_t test_hover_frame;                                     /* --test-hover */
static uint64_t test_key_frame;                                       /* --test-key F:NAME */
static char test_key_name[32];
static float test_hover_x, test_hover_y;
static void quit(int code);

/* the system menu (below) draws over the frame while it's open */
static int menu_is_open(void);
static void menu_draw(void);
static int map_open;                   /* the whole world map is up, over the game (which runs on) */
static void map_draw_mini(void);
static void map_draw_full(int ow, int oh);
static void map_textures_lost(void);
static float arena_card_alpha(void);
/* the Boss Arena's game unseen behind its card -- the party waiting on the field before a fight,
 * the field or the title after one: unheard, and the player's buttons not the game's */
static int arena_behind_card(void) {
    if (!arena_mode || arena_scanning) return 0;
    if (wave_mode) {                                 /* Wave Mode: loading, or a run over (its walks are seen) */
        int w = wave_state();
        return w == WAVE_LOADING || w == WAVE_RELOAD || w == WAVE_CLEARED || w == WAVE_LOST;
    }
    int s = arena_state();
    return arena_staging() || s == ARENA_WON || s == ARENA_LOST;
}
static void arena_card(int ow, int oh, float alpha);
static void wave_hud(int ow, int oh, float card);
static int classic_mode(void);

/* battle depth: the frame recomposed at a multiple of the game's resolution */
static SDL_Texture *depth_texture;
static int depth_tex_w, depth_tex_h, depth_shown;
static uint32_t depth_pixels[VDP_MAX_W * 4 * VDP_MAX_H * 4];

/* Smoothing (video.filter): how the game's picture is enlarged */
enum { LOOK_SHARP, LOOK_SOFT, LOOK_SCALEX };
static int picture_look(void) {
    const char *f = setting("video.filter");
    return !strcmp(f, "linear") ? LOOK_SOFT : !SDL_strcasecmp(f, "scale2x") ? LOOK_SCALEX : LOOK_SHARP;
}

/* (Scale2x's picture is made at a whole multiple of the game's size, up to four times: the graphics
 * card blends what is left to the window's size, which at a whole multiple is nothing) */
static SDL_ScaleMode scale_mode(void) {
    return picture_look() == LOOK_SHARP ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR;
}

/* the game's own frame at `ds` times its size at out, for the port to draw into and over: as
 * plain blocks, or through Scale2x / Scale3x (scalex.h) */
static void game_base(const uint32_t *pixels, int w, int h, uint32_t *out, int stride, int ds, int look) {
    if (look == LOOK_SCALEX) scalex_upscale(pixels, VDP_MAX_W, w, h, out, stride, ds);
    else hdtext_upscale(pixels, VDP_MAX_W, w, h, out, stride, ds);
}

/* ... and finished, in depth_pixels: Soft, as soft as the graphics card makes a frame it scales
 * itself (smooth.h) */
static void game_picture(const uint32_t *pixels, int w, int h, int stride, int ds, int look) {
    game_base(pixels, w, h, depth_pixels, stride, ds, look);
    if (look == LOOK_SOFT) smooth_soften(depth_pixels, stride, w * ds, h * ds, ds);
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
    if (!walkto_active() || worldmap_going(&wx, &wy)) return;   /* (a walk across the world: the map shows where to) */
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
static void test_record(void) {
    test_presents++;
    while (test_events && *test_events) {                /* the steps whose time has come */
        char *e;
        uint64_t at = strtoull(test_events + 1, &e, 10);
        if (*e != ':' || (*test_events == 'f' ? frame_count : test_presents) < at) break;
        char tok[300];
        size_t n = strcspn(e + 1, ",");
        snprintf(tok, sizeof tok, "%.*s", (int)n, e + 1);
        test_events = e + 1 + n + (e[1 + n] == ',');
        script_do(tok);
    }
    if (!test_record_path) return;
    SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
    if (!shot) return;
    if (shot->format != SDL_PIXELFORMAT_XRGB8888 && shot->format != SDL_PIXELFORMAT_ARGB8888) {
        SDL_Surface *c = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_XRGB8888);
        SDL_DestroySurface(shot);
        if (!(shot = c)) return;
    }
    if (!test_record_file) {
        test_record_file = fopen(test_record_path, "wb");
        char note[1100];
        snprintf(note, sizeof note, "%s.txt", test_record_path);
        FILE *f = fopen(note, "w");
        if (f) { fprintf(f, "%d %d bgra 60\n", shot->w, shot->h); fclose(f); }
    }
    if (test_record_file)
        for (int y = 0; y < shot->h; y++)
            fwrite((const uint8_t *)shot->pixels + (size_t)y * (size_t)shot->pitch, 4, (size_t)shot->w, test_record_file);
    SDL_DestroySurface(shot);
}

static void test_shots(void) {
    test_record();
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
static int journal_notes;             /* Settings > Game > Journal notes */

static void rewind_now(void) {
    if (arena_scanning) return;                      /* (the Boss Arena's gathering: nothing to go back to) */
    if (arena_mode) {                                /* the Boss Arena: no going back, every fight fought fair */
        SDL_snprintf(rewind_msg, sizeof rewind_msg, "No going back in the Boss Arena");
        rewind_msg_until = SDL_GetTicks() + 1800;
        return;
    }
    if (rewind_request()) return;
    SDL_snprintf(rewind_msg, sizeof rewind_msg, "%s",
                 rewind_enabled() ? "Nothing to go back to yet" : "Rewind is off (Settings, Game)");
    rewind_msg_until = SDL_GetTicks() + 1800;
}

/* auto-advance in voiced scenes (scenevox.h): on or off, remembered, and said */
static void save_setting_str(const char *key, const char *v);
static void toggle_auto_text(void) {
    int on = !scenevox_auto();
    scenevox_set_auto(on);
    save_setting_str("enhance.auto_text", on ? "on" : "off");
    SDL_snprintf(rewind_msg, sizeof rewind_msg, "Auto-advance: %s", on ? "on" : "off");
    rewind_msg_until = SDL_GetTicks() + 1800;
}

/* random battles off, or on again, at a press (L3, or its key): the No random battles cheat
 * (cheats.h), remembered, and said. Not in the Bonus Modes: their fights are fought fair. */
static void toggle_encounters(void) {
    if (arena_mode) {
        SDL_snprintf(rewind_msg, sizeof rewind_msg, "Not in the Bonus Modes");
        rewind_msg_until = SDL_GetTicks() + 1800;
        return;
    }
    int off = !cheats_get(CHEAT_NO_BATTLES);
    cheats_set(CHEAT_NO_BATTLES, off);
    save_setting_str("cheat.no_battles", off ? "on" : "off");
    SDL_snprintf(rewind_msg, sizeof rewind_msg, "Random battles: %s", off ? "off" : "on");
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
    if (!*text && SDL_GetTicks() < auto_note_until) SDL_snprintf(text, sizeof text, "Auto-saved");   /* (autosave.h) */
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

/* Picture detail. The port's own picture is composed at about the display's
 * scale, up to 4 times the game's size: 1.6 million pixels a frame, more
 * than a small machine draws in a frame's time, and then the game itself
 * slows down. So the frames' work is watched: where it leaves no room in
 * the 16.7 ms a frame has, the picture is composed a step smaller (the
 * graphics card scales it up), down to twice the game's size; and a step
 * larger again only after ten seconds in which that would have fitted
 * easily. (video.detail = full: always at the display's scale.) */
#define DETAIL_MAX 4
#define DETAIL_MIN 2
static int detail_cap = DETAIL_MAX;   /* the largest scale to compose at */
static int detail_used;               /* the scale this frame was composed at; 0: not composed */

static void detail_watch(uint64_t work_ns) {
    static uint64_t sum;
    static int n, calm;
    int used = detail_used;
    detail_used = 0;
    if (!used || !strcmp(setting("video.detail"), "full")) {
        if (detail_cap != DETAIL_MAX && !strcmp(setting("video.detail"), "full")) detail_cap = DETAIL_MAX;
        sum = 0;
        n = calm = 0;
        return;
    }
    if (work_ns > 2 * NTSC_FRAME_NS) work_ns = 2 * NTSC_FRAME_NS;   /* one long stall is not a slow machine */
    sum += work_ns;
    if (++n < 30) return;                                /* half a second of composed frames */
    double ms = (double)sum / (double)n / 1e6;
    sum = 0;
    n = 0;
    if (ms > 15.0 && used > DETAIL_MIN) {                /* no room left: a step smaller */
        detail_cap = used - 1;
        calm = 0;
        SDL_Log("picture detail: %dx (frames were taking %.1f ms)", detail_cap, ms);
    } else if (detail_cap < DETAIL_MAX && used == detail_cap
               && ms * (double)((used + 1) * (used + 1)) / (double)(used * used) < 10.0) {
        if (++calm >= 20) {                              /* ten seconds of room to spare: a step larger */
            detail_cap++;
            calm = 0;
            SDL_Log("picture detail: %dx (frames are taking %.1f ms)", detail_cap, ms);
        }
    } else {
        calm = 0;
    }
}

/* which saves the game's window is showing, and how to see the others
 * (savepages.h): a line at the top, in the rewind note's style */
static void savepages_draw(int ow, int oh) {
    char text[80];
    if (!savepages_note(text, sizeof text)) return;
    if (fieldmenu_system_up()) return;                   /* the new System screen says which saves these are itself */
    float s = SDL_max(1.0f, SDL_floorf((float)oh / 270.0f));
    float tw = (float)SDL_strlen(text) * 8.0f * s, x = ((float)ow - tw) / 2.0f, y = 12.0f * s;
    SDL_FRect bg = {x - 6.0f * s, y - 5.0f * s, tw + 12.0f * s, 18.0f * s};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 12, 18, 40, 210);
    SDL_RenderFillRect(renderer, &bg);
    SDL_SetRenderScale(renderer, s, s);
    SDL_SetRenderDrawColor(renderer, 255, 230, 160, 255);
    SDL_RenderDebugText(renderer, x / s, y / s, text);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

/* the Boss Arena's pictures being gathered (arenapic.h): a bar, and what's going on */
static void arena_scan_screen(void) {
    int ow, oh;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    SDL_SetRenderDrawColor(renderer, 6, 9, 26, 255);
    SDL_RenderClear(renderer);
    float s = fmaxf(1.0f, floorf((float)oh / 270.0f)), bw = (float)ow * 0.5f, bh = 6.0f * s;
    float bx = ((float)ow - bw) / 2.0f, by = (float)oh / 2.0f;
    float k = arena_scan_total ? (float)arena_scan_done / (float)arena_scan_total : 0.0f;
    SDL_FRect track = {bx, by, bw, bh}, fill = {bx, by, bw * k, bh};
    SDL_SetRenderDrawColor(renderer, 30, 42, 92, 255);
    SDL_RenderFillRect(renderer, &track);
    SDL_SetRenderDrawColor(renderer, 255, 214, 120, 255);
    SDL_RenderFillRect(renderer, &fill);
    char t[96];
    SDL_SetRenderScale(renderer, s, s);
    const char *what = "Boss Arena: gathering the bosses from your ROM";
    SDL_SetRenderDrawColor(renderer, 255, 224, 138, 255);
    SDL_RenderDebugText(renderer, ((float)ow / s - 8.0f * (float)strlen(what)) / 2.0f, (by - 22.0f * s) / s, what);
    SDL_snprintf(t, sizeof t, "%d of %d  -  once only, then kept", arena_scan_done, arena_scan_total);
    SDL_SetRenderDrawColor(renderer, 190, 204, 240, 255);
    SDL_RenderDebugText(renderer, ((float)ow / s - 8.0f * (float)strlen(t)) / 2.0f, (by + bh + 12.0f * s) / s, t);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    SDL_RenderPresent(renderer);
}

/* V-Sync (video.vsync, GitHub issue 5). The game keeps its own time, 59.92 pictures a second, and
 * shows each when it is ready: on a display with no compositor in between, part of one picture and
 * part of the next can then show at once (tearing). With V-Sync on, each waits for the display's
 * refresh. The game's own pace stays: on a 60 Hz display it runs with the display, and about every
 * twenty seconds one picture stays for two refreshes. Not while fast-forwarding or passing the
 * title unseen (they would be held to the display's rate), and not on a display slower than the
 * game (it would slow the game down, and its sound with it). The launcher is not synced. */
static int vsync_now;                 /* what the renderer was last told */
static uint64_t present_ns;           /* how long the last picture took to show: with V-Sync, mostly the wait */

static void vsync_update(void) {
    static uint64_t checked;
    static int slow = -1;
    int want = !SDL_strcasecmp(setting("video.vsync"), "on") && !held(IN_FF) && !continuing_now && !arena_scanning;
    if (want) {
        uint64_t now = SDL_GetTicksNS();
        if (slow < 0 || now - checked > 2000000000ull) {     /* (the window may have moved to another display) */
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
            int was = slow;
            slow = dm && dm->refresh_rate > 1.0f && dm->refresh_rate < 59.0f;
            if (slow && was != 1)
                SDL_Log("v-sync: this display refreshes %.1f times a second, fewer than the game's 59.92: left off",
                        (double)dm->refresh_rate);
            checked = now;
        }
        if (slow) want = 0;
    }
    static int refused;                                      /* the renderer cannot: not asked again each frame */
    static int said;                                         /* the log has it (once each time the setting goes on) */
    if (SDL_strcasecmp(setting("video.vsync"), "on")) refused = said = 0;
    else if (refused) want = 0;
    if (want == vsync_now) return;
    if (!SDL_SetRenderVSync(renderer, want ? 1 : 0) && want) {
        SDL_Log("v-sync: not available here (%s)", SDL_GetError());
        refused = 1;
        return;
    }
    vsync_now = want;
    if (want && !said) { SDL_Log("v-sync: on"); said = 1; }
}

static void present(const uint32_t *pixels, int w, int h, int compose) {
    vsync_update();
    if (arena_scanning) {                                /* (unseen: a bar now and then) */
        static unsigned skipped;
        if ((skipped++ & 15) == 0) arena_scan_screen();
        return;
    }
    if (continuing_now) {                                /* Continue: the title goes by unseen, in black */
        static unsigned skipped;
        if ((skipped++ & 7) == 0) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            if (arena_mode) {                            /* (the Boss Arena: its card already) */
                int cw, ch;
                SDL_GetRenderOutputSize(renderer, &cw, &ch);
                arena_card(cw, ch, 1.0f);
            }
            SDL_RenderPresent(renderer);
        }
        return;
    }
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

    perf_begin(PERF_COMPOSE);
    if (compose) {
        int ds = (int)ceilf(s);                          /* compose at about the display scale */
        if (ds < 1) ds = 1;
        if (ds > detail_cap) ds = detail_cap;            /* (at most 4: see detail_watch) */
        int stride = fw * ds;
        /* Smoothing. Soft: the game's picture (and the water or battle depth drawn into it) is softened
         * here as the graphics card softens a frame that is not composed (smooth.h). Scale2x: the game's
         * picture is enlarged through the filter (scalex.h), every frame, so none goes to the card at
         * the game's own size; a battle with depth is composed from its layers, each put through the
         * filter first (depth_set_filter).
         * The port's text, menus and panels go over either afterwards, crisp */
        int look = picture_look();
        int soft = look == LOOK_SOFT;
        if (pm) {                                        /* the game in the middle, panels around it */
            uint32_t *mid = depth_pixels + (size_t)pm * (size_t)ds;
            game_base(pixels, w, h, mid, stride, ds, look);
            if (fieldfx_prepare(frame_lines, w, h)) fieldfx_draw(pixels, VDP_MAX_W, w, h, mid, stride, ds);
            if (soft) smooth_soften(mid, stride, w * ds, h * ds, ds);
            if (hdtext_enabled() && hdtext_has_text(frame_lines, w, h)) hdtext_draw(frame_lines, w, h, mid, stride, ds);
            panels_draw(depth_pixels, stride, ds, pm, w, h);
            depth_shown = 1;
        } else {
            depth_set_filter(look == LOOK_SCALEX);       /* (a battle's layers through Scale2x, each on its own) */
            depth_shown = fpview_compose(frame_lines, pixels, w, h, depth_pixels, stride, ds)   /* first-person dungeons */
                       || depth_compose(frame_lines, w, h, depth_pixels, stride, ds);
            if (!depth_shown && fieldfx_prepare(frame_lines, w, h)) {   /* the field's living water */
                game_base(pixels, w, h, depth_pixels, stride, ds, look);
                fieldfx_draw(pixels, VDP_MAX_W, w, h, depth_pixels, stride, ds);
                depth_shown = 1;
            }
            if (depth_shown && soft) smooth_soften(depth_pixels, stride, w * ds, h * ds, ds);
            if (!depth_shown && look == LOOK_SCALEX && ds > 1) {        /* Scale2x: a plain frame too */
                game_picture(pixels, w, h, stride, ds, look);
                depth_shown = 1;
            }
            if (hdtext_enabled() && hdtext_has_text(frame_lines, w, h)) {    /* modern text */
                if (!depth_shown) game_picture(pixels, w, h, stride, ds, look);
                hdtext_draw(frame_lines, w, h, depth_pixels, stride, ds);
                depth_shown = 1;
            }
            if (battleui_active()) {                     /* modern battle menus */
                if (!depth_shown) game_picture(pixels, w, h, stride, ds, look);
                battleui_draw(depth_pixels, stride, ds, w, h);
                depth_shown = 1;
            }
        }
        if (fieldmenu_active()) {                        /* the field menu, redrawn: over the whole picture */
            if (!depth_shown) game_picture(pixels, w, h, stride, ds, look);
            fieldmenu_draw(depth_pixels, stride, ds, fw, h);
            depth_shown = 1;
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
            detail_used = ds;
            perf_end(PERF_COMPOSE);
            perf_begin(PERF_UPLOAD);
            SDL_UpdateTexture(depth_texture, NULL, depth_pixels, stride * 4);
            perf_end(PERF_UPLOAD);
        }
    }
    if (!depth_shown) perf_end(PERF_COMPOSE);
    if (!depth_shown) {
        if (w != tex_w || h != tex_h) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            SDL_SetTextureScaleMode(texture, scale_mode());
            tex_w = w;
            tex_h = h;
        }
        perf_begin(PERF_UPLOAD);
        SDL_UpdateTexture(texture, NULL, pixels, VDP_MAX_W * 4);
        perf_end(PERF_UPLOAD);
    }
    perf_begin(PERF_PRESENT);
    /* round a picture that doesn't fill the window: the backdrop colour, as a
     * TV's border shows it -- the SEGA screen's blue reaches the window's edges
     * and fades with it */
    uint32_t bd = vdp_color(vdp.reg[7] & 63);
    SDL_SetRenderDrawColor(renderer, (Uint8)(bd >> 16), (Uint8)(bd >> 8), (Uint8)bd, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, depth_shown ? depth_texture : texture, NULL, &dst);
    draw_walk_marker(&dst, fw, pm, w, h);
    map_draw_mini();                                     /* the world map, small, in the corner */
    touch_draw(renderer, ow, oh);                        /* the on-screen pad, when touch is in use */
    float card = arena_card_alpha();                     /* the Boss Arena: no map, its card */
    if (card > 0.0f) arena_card(ow, oh, card);
    if (wave_mode) wave_hud(ow, oh, card);               /* Wave Mode: the battle and the clock */
    rewind_draw(ow, oh);                                 /* a rewind's fade to white, and its note */
    savepages_draw(ow, oh);                             /* which page of saves the game's window shows */
    if (map_open) map_draw_full(ow, oh);                 /* the whole world map */
    if (menu_is_open()) menu_draw();
    test_shots();
    {
        uint64_t t0 = SDL_GetTicksNS();
        SDL_RenderPresent(renderer);
        present_ns = SDL_GetTicksNS() - t0;
    }
    perf_end(PERF_PRESENT);
}

/* store a changed setting and save psiv.ini */
static void write_settings(const char *path);
static void save_setting_str(const char *key, const char *v) {
    for (int i = 0; i < NSETTINGS; i++)
        if (!strcmp(settings[i].key, key)) snprintf(settings[i].value, sizeof settings[i].value, "%s", v);
    char path[1100];
    path_in_home(path, sizeof path, "psiv.ini");
    write_settings(path);
}
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
static void apply_cheats(void) {
    cheats_set(CHEAT_HP, !SDL_strcasecmp(setting("cheat.hp"), "on"));
    cheats_set(CHEAT_TP, !SDL_strcasecmp(setting("cheat.tp"), "on"));
    cheats_set(CHEAT_NO_BATTLES, !SDL_strcasecmp(setting("cheat.no_battles"), "on"));
    cheats_set(CHEAT_MONEY, !SDL_strcasecmp(setting("cheat.money"), "on"));
    cheats_set(CHEAT_EXP, setting_int("cheat.exp"));
    cheats_set(CHEAT_WEAK, !SDL_strcasecmp(setting("cheat.weak"), "on"));
}

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
    fieldfx_set_water(!classic_mode() && SDL_strcasecmp(setting("enhance.water"), "off") != 0);
    fieldfx_set_trees(!classic_mode() && SDL_strcasecmp(setting("enhance.trees"), "off") != 0);
    rewind_set_enabled(!arena_mode && SDL_strcasecmp(setting("enhance.rewind"), "off") != 0);   /* (off in the Boss Arena) */
    autosave_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.autosave"), "off") != 0);
    /* the modern sounds where there is one (yours in sfx/sfx.ini first, then the port's); the rest are the
     * game's own, on the chip or on Algol */
    sfx_set_mode(!classic_mode() && SDL_strcasecmp(setting("enhance.sfx_modern"), "off") != 0 ? SFX_CUSTOM : SFX_ORIGINAL);
    sfx_set_algol(!classic_mode() && !SDL_strcasecmp(setting("enhance.sfx"), "algol"));
    hdtext_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.text"), "original") != 0);
    battleui_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.battle_ui"), "original") != 0);
    fieldmenu_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.field_menu"), "original") != 0);
    fieldmenu_set_group_items(SDL_strcasecmp(setting("enhance.group_items"), "off") != 0);
    battleui_set_group_items(SDL_strcasecmp(setting("enhance.group_items"), "off") != 0);
    battleui_set_pan(SDL_strcasecmp(setting("enhance.battle_camera"), "off") != 0);
    battlevox_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.voices"), "off") != 0);
    scenevox_set_auto(!SDL_strcasecmp(setting("enhance.auto_text"), "on"));
    journal_notes = !classic_mode() && SDL_strcasecmp(setting("enhance.journal_notes"), "off") != 0;
    scenevox_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.scene_voices"), "off") != 0);
    {   /* the music's chip: the original, Algol (the chip remastered), or Proto Algol (the first) */
        const char *mus = setting("enhance.music");
        int algol = !SDL_strcasecmp(mus, "algol") || !SDL_strcasecmp(mus, "algol2"), proto = !SDL_strcasecmp(mus, "proto");
        algol_set_version(algol ? 2 : 1);
        algol_set_enabled(!classic_mode() && (algol || proto));
    }
    /* first-person dungeons are paused (PsiLok, 2026-11-05: not yet a good way
     * to play): no setting, no menu entry; PSIV_FIRST_PERSON=1 brings the
     * view back for development */
    fpview_set_enabled(!classic_mode() && SDL_getenv("PSIV_FIRST_PERSON") && *SDL_getenv("PSIV_FIRST_PERSON") == '1');
    walkto_set_enabled(!classic_mode() && SDL_strcasecmp(setting("enhance.click_walk"), "off") != 0);
    walkto_set_diagonal(!classic_mode() && SDL_strcasecmp(setting("enhance.diagonal"), "off") != 0);
    {   /* the world map: off (where the party goes is noted all the same), what was explored, or all of it */
        const char *wm = setting("enhance.world_map");
        worldmap_set_mode(classic_mode() || arena_mode ? -1 : !SDL_strcasecmp(wm, "explored") ? 1 : !SDL_strcasecmp(wm, "all") ? 2 : 0);
    }
    titleart_set(!classic_mode());                 /* the port's IV and RECOMP beside the title's logo */
    savepages_set(classic_mode() ? 1 : SAVEPAGES_MAX, page_go);   /* twelve save slots, in pages of three */
    savepages_on_save(page_saved);
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
    ui_set_image_look(picture_look());                   /* the portraits the port draws itself, the same way */
    if (texture) SDL_SetTextureScaleMode(texture, scale_mode());
    if (depth_texture) SDL_SetTextureScaleMode(depth_texture, scale_mode());
    SDL_SetWindowFullscreen(window, setting_int("video.fullscreen") != 0);
}

static void apply_audio(void) {
    if (audio) SDL_SetAudioStreamGain(audio, (float)setting_int("audio.volume") / 100.0f);
    mixbus_set_levels((float)setting_int("audio.music") / 100.0f, (float)setting_int("audio.effects") / 100.0f,
                      (float)setting_int("audio.voices") / 100.0f);
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
    void (*action)(void);             /* a row that does something when chosen (no setting) */
    const char *(*shown)(void);       /* ... and what it shows as its value */
} MenuItem;

static const MenuItem GAME_ITEMS[] = {
    {"Mode", "enhance.mode", {"enhanced", "classic"}, {"Enhanced", "Classic"}, 0, 0, 0, 0,
     "Classic turns every addition off: the original game, unchanged.", apply_enhancements},
    {"Text", "enhance.text", {"modern", "original"}, {"Modern", "Original"}, 0, 0, 0, 1,
     "Dialogue and the game's windows in a modern font at full resolution, or the game's own pixel font.",
     apply_enhancements},
    {"Menu", "enhance.field_menu", {"modern", "original"}, {"Modern", "Original"}, 0, 0, 0, 1,
     "The field menu on the whole screen (the main menu and equipment so far), or the game's own windows.",
     apply_enhancements},
    {"Group items", "enhance.group_items", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "In the modern menus (camp and battle), one row for each kind of item, with how many you carry. The pack "
     "itself is the game's: 40 items, one a slot.", apply_enhancements},
    {"Auto save", "enhance.autosave", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "Saves for you as you go, each time you come to a new place: a save of its own, for the launcher's Continue "
     "and Load game. Your own saves are never touched.", apply_enhancements},
    {"Rewind", "enhance.rewind", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 0,
     "Backspace or the left shoulder goes back about ten seconds: a fade to white and back.",
     apply_enhancements},
    {"Journal notes", "enhance.journal_notes", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "A note in the corner when the story moves on. The Journal, in this menu, says what to do next.",
     apply_enhancements},
    {"World map", "enhance.world_map", {"off", "explored", "all"}, {"Off", "Explored", "Everything"}, 0, 0, 0, 1,
     "A map of Motavia, Dezolis and Rykros as you walk them: a small one in the corner, the whole map on M, "
     "R3 or a tap on the small one. Explored shows only where you have been. Click a place and the party walks there.",
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
    {"Smoothing", "video.filter", {"nearest", "linear", "scale2x"}, {"Off", "Soft", "Scale2x"}, 0, 0, 0, 0,
     "Off keeps every pixel sharp. Soft blends them where the picture is scaled. Scale2x rounds off the "
     "stair-steps and blurs nothing.", apply_video},
    {"V-Sync", "video.vsync", {"off", "on"}, {"Off", "On"}, 0, 0, 0, 0,
     "On: each picture waits for the display, so it never tears. Off: the game keeps its own time.", NULL},
    {"Water", "enhance.water", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "The sea, rivers and pools come alive as you walk: ripples, moving light, glints and foam on the shore.",
     apply_enhancements},
    {"Trees", "enhance.trees", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "Trees sway in the breeze, more at their tops, with light flickering through the leaves.",
     apply_enhancements},
    {"Integer scaling", "video.integer_scaling", {"1", "0"}, {"On", "Off"}, 0, 0, 0, 0,
     "Scale by whole numbers only: the sharpest pixels, with a border round the picture.", NULL},
};
/* Scene voices switched: on, with a pack installed, someone has a word to say */
static void apply_scene_voices(void) {
    apply_enhancements();
    if (SDL_strcasecmp(setting("enhance.scene_voices"), "off") && scenevox_pages() > 0) voice_quip();
}

static const MenuItem SOUND_ITEMS[] = {
    {"Volume", "audio.volume", {0}, {0}, 0, 200, 10, 0,
     "Everything together: the music, the sound effects and the voices.", apply_audio},
    {"Music volume", "audio.music", {0}, {0}, 0, 100, 5, 0,
     "The music on its own: the game's songs, on either chip, or your own files.", apply_audio},
    {"Effects volume", "audio.effects", {0}, {0}, 0, 100, 5, 0,
     "The sound effects on their own: the game's, the port's or yours, and the rain and thunder in battles.",
     apply_audio},
    {"Voice volume", "audio.voices", {0}, {0}, 0, 100, 5, 1,
     "The voices on their own: the party in battle, and the voiced scenes.", apply_audio},
    {"Music", "enhance.music", {"original", "algol", "proto"}, {"Original chip", "Algol", "Proto Algol"}, 0, 0, 0, 1,
     "Algol: the chip remastered, the game's own instruments and drums, clean and in stereo. Proto Algol: "
     "the first Algol, the same songs on new instruments of its own.",
     apply_enhancements},
    {"Battle voices", "enhance.voices", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "The party shout as they attack, grunt when hit, draw breath as they cast. Your own in the voices folder.",
     apply_enhancements},
    {"Scene voices", "enhance.scene_voices", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "The story scenes spoken aloud, once the voice pack below is installed. Off: the scenes are read, as ever.",
     apply_scene_voices},
    {"Voice pack", NULL, {0}, {0}, 0, 0, 0, 0,
     "The optional voice pack for the story scenes: choose its .zip to install it, or a newer one. A pack's .zip "
     "left in the game's folder installs itself at the start.", NULL, voicepack_choose, voicepack_label},
    {"Music pack", "enhance.custom_music", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "A music pack, or audio files of your own, in the music folder: they play in place of the game's songs.",
     apply_enhancements},
    {"Sound effects", "enhance.sfx", {"original", "algol"}, {"Original", "Algol"},
     0, 0, 0, 1, "What plays the game's own sound effects. Original: the chip. Algol: the chip remastered: clean, in "
     "stereo, with room around them.",
     apply_enhancements},
    {"Modern sounds", "enhance.sfx_modern", {"on", "off"}, {"On", "Off"},
     0, 0, 0, 1, "Use modern sounds that are available: a new sound in place of the game's, for the effects that "
     "have one (and your own, from the sfx folder). The rest play as chosen above.",
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
    {"Camera", "enhance.battle_camera", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "When an enemy attacks someone, the view turns to face them: the party slides along instead of overlapping.",
     apply_enhancements},
    {"Weather", "enhance.battle_weather", {"0", "1", "2"}, {"Off", "Light", "Heavy"}, 0, 0, 0, 1,
     "Rain and storms on Motavia, snow on Dezolis, mist clouds on Rykros, debris in space. Heavy: more of it.",
     apply_enhancements},
    {"Effects", "enhance.battle_fx", {"0", "1", "2"}, {"Off", "Subtle", "Full"}, 0, 0, 0, 1,
     "Attacks and techniques light the scene: sparks, flashes, embers and lens flares.", apply_enhancements},
};
static const MenuItem CONTROL_ITEMS[] = {
    {"Diagonal walking", "enhance.diagonal", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "Hold two directions to walk diagonally, a step at a time. Doors, exits and people are still met "
     "straight on.", apply_enhancements},
    {"Click to walk", "enhance.click_walk", {"on", "off"}, {"On", "Off"}, 0, 0, 0, 1,
     "Click or tap the field to walk there, or a person or a chest to talk or open it. Battle menus take "
     "clicks too.", apply_enhancements},
    {"Rumble", "pad.rumble", {"off", "light", "normal", "strong"}, {"Off", "Light", "Normal", "Strong"}, 0, 0, 0, 0,
     "Blows landing, critical hits, enemies destroyed and the field's big moments, on a gamepad or a "
     "phone's motor.", apply_rumble},
};

static void apply_cheats(void);
static const MenuItem CHEAT_ITEMS[] = {
    {"Infinite HP", "cheat.hp", {"off", "on"}, {"Off", "On"}, 0, 0, 0, 0,
     "Everyone's HP stays full. (One blow bigger than all of someone's HP still fells them.)", apply_cheats},
    {"Infinite TP", "cheat.tp", {"off", "on"}, {"Off", "On"}, 0, 0, 0, 0,
     "Everyone's TP stays full: techniques as often as you like.", apply_cheats},
    {"No random battles", "cheat.no_battles", {"off", "on"}, {"Off", "On"}, 0, 0, 0, 0,
     "Walk anywhere without random battles. The story's own battles and bosses still happen.", apply_cheats},
    {"Max meseta", "cheat.money", {"off", "on"}, {"Off", "On"}, 0, 0, 0, 0,
     "Your meseta stays at 999,999: buy anything.", apply_cheats},
    {"Experience", "cheat.exp", {"1", "2", "4"}, {"Normal", "Double", "Quadruple"}, 0, 0, 0, 0,
     "More experience from every battle won.", apply_cheats},
    {"Enemies at 1 HP", "cheat.weak", {"off", "on"}, {"Off", "On"}, 0, 0, 0, 0,
     "Every enemy falls to a single blow, bosses too.", apply_cheats},
};

/* a page has settings, or (items NULL) shows something else: draw fills it */
typedef struct { const char *name, *about; const MenuItem *items; int n; void (*draw)(float W, float H); } MenuPage;
#define PAGE(name, about, items) {name, about, items, (int)(sizeof items / sizeof items[0]), NULL}
static void draw_buttons_page(float W, float H);
static const MenuPage PAGES[] = {
    PAGE("Game", "Classic or Enhanced, and the text.", GAME_ITEMS),
    PAGE("Display", "Fullscreen, widescreen, and how the pixels are drawn.", DISPLAY_ITEMS),
    PAGE("Sound", "How loud the music, the sound effects and the voices are, and what plays them.", SOUND_ITEMS),
    PAGE("Battles", "The battle menus, and what's drawn over battles.", BATTLE_ITEMS),
    PAGE("Controls", "Walking, clicks and taps, and rumble.", CONTROL_ITEMS),
    PAGE("Cheats", "Make the game easier. All off unless you turn them on.", CHEAT_ITEMS),
    {"Buttons", "What each button and key does.", NULL, 0, draw_buttons_page},
};
#define N_PAGES (int)(sizeof PAGES / sizeof PAGES[0])
#define MAX_ROWS 8                    /* rows a page shows at the usual spacing: the panel keeps its size */
#define TIGHT_ROWS 9                  /* a page of this many fits too, its rows a little closer (Sound) */
#define SETTINGS_H 213.0f             /* the panel: the top, eight rows of 15, the help, the hints */
#define SETTINGS_W_MAX 392.0f         /* ... 288 wide, or up to this where the window is wide */
static const char *const MAIN_LABELS[] = {"Resume", "Go back 10 seconds", "Journal", "Settings", "Return to menu",
                                          "Quit game"};
enum { M_RESUME, M_REWIND, M_JOURNAL, M_SETTINGS, M_MENU, M_QUIT, N_MAIN };
static int quit_to_menu;                  /* the question is Return to menu's, not Quit's */
static int to_launcher;                   /* ... answered Yes: the launcher, between this frame and the next */
/* (in the Boss Arena, the Journal's place has the list of bosses: there's no story to follow) */
static const char *const *main_labels(void) {
    static const char *labels[N_MAIN];
    for (int i = 0; i < N_MAIN; i++) labels[i] = MAIN_LABELS[i];
    if (arena_mode) labels[M_JOURNAL] = "Bonus Modes", labels[M_REWIND] = "Go back: off in the Arena";
    return labels;
}
static const char *const CLASSIC_NOTE = "Off in Classic mode. Set Mode to Enhanced, on the Game page, to use it.";

enum { MENU_CLOSED, MENU_MAIN, MENU_SETTINGS, MENU_QUIT, MENU_LAUNCHER, MENU_JOURNAL, MENU_LOAD, MENU_ARENA,
       MENU_ARENA_PREP, MENU_ARENA_TPL, MENU_WAVE, MENU_WAVE_SETUP, MENU_WAVE_RESULT, MENU_BONUS, MENU_JUKEBOX };
static int menu_screen, menu_sel;     /* in Settings, -1 = the page bar */
static int in_launcher;               /* the launcher is up: the game hasn't started */
static int launcher_go;               /* its Start game was chosen */
static const char *const LAUNCHER_LABELS[] = {"Continue", "Load game", "Start game", "Bonus Modes", "Mode", "Settings", "Quit"};
enum { L_CONTINUE, L_LOAD, L_START, L_ARENA, L_MODE, L_SETTINGS, L_QUIT, N_LAUNCHER };
static int launch_has_continue(void);
static int launch_first(void) { return launch_has_continue() ? L_CONTINUE : L_START; }   /* Continue, Load game: only with a save */
static int launch_rows(void) { return N_LAUNCHER - launch_first(); }
/* Load game: the saves, a page of three at a time (the cartridge's three slots; Enhanced has four
 * pages), each with what it holds -- any of them can be played on, as Continue plays on the last.
 * menu_sel is the slot in the page; Left / Right (the shoulders, Tab, the tabs) turn the page, and
 * Up / Down go on past a page's ends. */
static int load_page;
static int load_pages(void) { return classic_mode() ? 1 : SAVEPAGES_MAX; }
/* (the auto save, if there is one, has a tab of its own after the pages: one card) */
static int load_tabs(void) { return load_pages() + (auto_ok && !classic_mode() ? 1 : 0); }
static int load_on_auto(void) { return load_page == load_pages(); }
static const uint8_t *load_cartridge(void) { return load_on_auto() ? auto_page : save_pages[load_page]; }
/* its two sounds, the game's own from the ROM (launcher_sfx, below): $F3 as the panel opens and
 * $CC, the healing techniques' sound, as a save is chosen (PsiLok's choice of them) */
#define LOAD_OPEN_SFX 0xF3
#define LOAD_PICK_SFX 0xCC
static void launcher_sfx(int id, int ms);
static int launcher_music;                 /* (defined with the launcher's music: its driver is running) */
/* The Jukebox (Bonus Modes): the game's 52 songs, $81-$B4, by the names the disassembly gives them.
 * One chosen plays in the launcher in place of its own song (launcher_music_run), by the game's own
 * driver -- or your own file for it from the music folder, as in the game (music.h). */
#define JUKEBOX_FIRST 0x81
static const char *const JUKEBOX_TITLES[] = {
    "Tonoe De Pon", "Inn", "Motavia Village", "Motavia Town", "Organic Beat", "Dezolis Town I", "Now on Sale",
    "Behind the Circuit", "Machine Center", "In the Cave", "Winners", "Field Motavia", "Land Master",
    "Requiem for Lutz", "Meet Them Head-On", "Ryucross Field", "Dungeon Arrange I", "Fal", "Temple Ngangbius",
    "Thray", "Defeat at a Blow", "Cybernetic Carnival", "Terrible Sight", "Edge of Darkness", "Dezolis Field I",
    "Tower", "Take Off! Landale", "Dezolis Town II", "Dezolis Field II", "A Happy Settlement", "Suspicion",
    "The King of Terrors", "The Age of Fables", "Abyss", "Enemy Appearance", "Her Last Breath", "Pain",
    "Jijy no Rag", "Dungeon Arrange II (2)", "The Black Blood", "Red Alert", "Laughter", "Mystery",
    "End of the Millennium", "Explosion", "Staff Roll", "The Promising Future I", "Pao Pao",
    "Dungeon Arrange II", "The Promising Future II", "Dezolis de Don", "Ooze"};
#define JUKEBOX_N (int)(sizeof JUKEBOX_TITLES / sizeof JUKEBOX_TITLES[0])
#define JUKEBOX_ROWS 8
static int jukebox_song;                   /* the song ID playing in the launcher by the Jukebox; 0: its own */
static const char *setting(const char *key);
/* the launcher's own song: Her Last Breath unless the Jukebox chose another (audio.launcher_song) */
static int launcher_song(void) {
    int id = (int)strtol(setting("audio.launcher_song"), NULL, 16);
    return id >= JUKEBOX_FIRST && id < JUKEBOX_FIRST + JUKEBOX_N ? id : 0xA4;
}
static int jukebox_top;                    /* the list's first row shown */
/* what plays it, for the Jukebox only (Settings are put back when it stops): the YM2612 and PSG as
 * the Mega Drive had them, Algol (the chip remastered), Proto Algol (the first), or your music pack -- the music folder's
 * files, the chip under any song without one */
enum { JB_CHIP, JB_ALGOL, JB_PROTO, JB_PACK, JB_N };
static int jukebox_src = -1;               /* -1: not chosen yet (from Settings, the first time) */
static uint8_t jukebox_file[64];           /* the song has a file in the music folder */
static int jukebox_files;                  /* how many */
static int jukebox_applied = -1;           /* the source the sound is set to now (-1: Settings) */
static void load_enter(void) {
    if (rom_ready) {                                 /* the party's faces, from the ROM: it has to be in the
                                                      * machine (the music's driver put it there, if it runs) */
        if (!launcher_music) bus_init(rom_buf, rom_size);
        ui_portraits_init();
    }
    launcher_sfx(LOAD_OPEN_SFX, 350);
    load_page = cont.ok && cont.page == AUTO_PAGE && load_tabs() > load_pages() ? load_pages()
              : cont.ok && cont.page < load_pages() ? cont.page : 0;
    menu_sel = cont.ok ? cont.slot : 0;              /* on the save Continue would take */
    menu_screen = MENU_LOAD;
}
static void load_turn(int dir) {
    load_page = (load_page + dir + load_tabs()) % load_tabs();
    if (load_on_auto()) menu_sel = 0;                /* its one card */
}
/* the slot under the cursor is played on, if it holds a save: as Continue, with this one */
static void load_pick(void) {
    if (menu_sel < 0 || menu_sel > 2 || !savepages_saved(load_cartridge(), menu_sel)) return;
    const uint8_t *d = &load_cartridge()[0x100 + menu_sel * 0xA00];
    cont.ok = 1;
    cont.page = load_on_auto() ? AUTO_PAGE : load_page;
    cont.slot = menu_sel;
    cont.level = d[0x409];
    cont.meseta = (uint32_t)d[0x338] << 24 | (uint32_t)d[0x339] << 16 | (uint32_t)d[0x33A] << 8 | d[0x33B];
    menu_screen = MENU_LAUNCHER;                     /* (the launcher fades out as it does for Continue) */
    menu_sel = L_LOAD;
    launcher_go = 2;
    launcher_sfx(LOAD_PICK_SFX, 1200);               /* (the fade to black waits for it: it sounds whole) */
}
/* Boss Arena (arena.h): the game's boss fights in story order (all 27 but the first Zio, scripted to lose), each with the party the story has
 * there. A fight is played from a save made for it in memory -- the player's own saves are never
 * touched, nor written over while it lasts -- and when it's won or lost the list comes back, the
 * fight's time beside it. A fight chosen in the game starts the console over (rewind_restart). */
#define ARENA_ROWS 9
static int arena_scroll;
static int arena_result = -1, arena_result_boss = -1, arena_result_secs;   /* the last fight: ARENA_WON / LOST */
static int arena_go = -1;                        /* in the game: the fight to start, the menu closed */
static int launch_arena = -1;                    /* the launcher: the fight chosen */
static int arena_faces;                          /* the portraits are loaded */
static uint64_t menu_opened;                     /* (the menu's: when it opened, it fades in; 0 = at once) */
static void menu_close(void);
#define ARENA_MAX 64
static ArenaPic arena_pics[ARENA_MAX];
static int arena_pic_state[ARENA_MAX];           /* 0 not looked for yet, 1 loaded, -1 not gathered */
/* what the player keeps for boss i goes by its key (ArenaBoss.key), not its place in the list */
static int boss_key(int i) { const ArenaBoss *b = arena_boss(i); return b ? b->key : -1; }
static void arena_pic_path(int i, char *out, size_t n) {   /* in the player's folder: arena/NN.bin (NN: its key) */
    char dir[1100];
    path_in_home(dir, sizeof dir, "arena");
    snprintf(out, n, "%s/%02d.bin", dir, boss_key(i));
}
static void arena_pics_load(void) {
    for (int i = 0; i < arena_count() && i < ARENA_MAX; i++) {
        if (arena_pic_state[i]) continue;
        char path[1200];
        arena_pic_path(i, path, sizeof path);
        size_t n = 0;
        void *d = SDL_LoadFile(path, &n);
        arena_pic_state[i] = d && arenapic_unpack(d, n, &arena_pics[i]) ? 1 : -1;
        SDL_free(d);
    }
}
static int arena_pics_missing(void) {
    int m = 0;
    arena_pics_load();
    for (int i = 0; i < arena_count() && i < ARENA_MAX; i++) m += arena_pic_state[i] < 0;
    return m;
}
static int arena_next_missing(int from) {
    for (int i = from; i < arena_count() && i < ARENA_MAX; i++)
        if (arena_pic_state[i] < 0) return i;
    return -1;
}
/* the recording just made: to its file, and in the list at once */
static void arena_pic_store(int i) {
    uint8_t *pk;
    size_t pn;
    if (i < 0 || i >= ARENA_MAX || !arenapic_capture_pack(&pk, &pn)) return;
    char dir[1100], path[1200];
    path_in_home(dir, sizeof dir, "arena");
    SDL_CreateDirectory(dir);
    arena_pic_path(i, path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(pk, 1, pn, f); fclose(f); }
    else SDL_Log("arena: cannot write %s", path);
    if (arena_pic_state[i] > 0) arenapic_free(&arena_pics[i]);
    arena_pic_state[i] = arenapic_unpack(pk, pn, &arena_pics[i]) ? 1 : -2;   /* (-2: tried; not again this time) */
    free(pk);
}

static int best_at(int key) {                   /* arena.best: a number a key */
    const char *s = setting("arena.best");
    if (key < 0) return 0;
    for (int k = 0; k < key && *s; k++) {
        s = strchr(s, ',');
        if (!s) return 0;
        s++;
    }
    return atoi(s);
}
static int arena_best(int i) { return best_at(boss_key(i)); }   /* seconds; 0: not beaten yet */
static void arena_set_best(int i, int secs) {
    char out[300];
    int o = 0, n = 0, key = boss_key(i);
    for (int b = 0; b < arena_count(); b++)
        if (boss_key(b) + 1 > n) n = boss_key(b) + 1;
    for (int k = 0; k < n && o < (int)sizeof out - 8; k++)
        o += snprintf(out + o, sizeof out - (size_t)o, "%s%d", k ? "," : "", k == key ? secs : best_at(k));
    save_setting_str("arena.best", out);
}
static void arena_keep_visible(void) {
    if (menu_sel < arena_scroll) arena_scroll = menu_sel;
    if (menu_sel >= arena_scroll + ARENA_ROWS) arena_scroll = menu_sel - ARENA_ROWS + 1;
}
static void arena_enter(void) {
    if (!arena_faces && rom_ready) {                 /* the party's faces, from the ROM (as Load game) */
        if (in_launcher && !launcher_music) bus_init(rom_buf, rom_size);
        arena_faces = ui_portraits_init() > 0;
    }
    if (in_launcher) launcher_sfx(LOAD_OPEN_SFX, 350);
    arena_pics_load();
    menu_sel = arena_result_boss >= 0 ? arena_result_boss : arena_current() >= 0 ? arena_current() : 0;
    arena_keep_visible();
    if (menu_screen == MENU_CLOSED) menu_opened = SDL_GetTicks();
    menu_screen = MENU_ARENA;
}
/* Macros for the arena's fights (arena.h: Macro_Data's 160 bytes): each boss keeps its own for a
 * rematch -- as last fought, so macros set in battle are kept too -- and eight templates hold sets
 * for any fight. In the player's folder: arena/macros.txt, a line each ("boss N HEX", "template N HEX"). */
#define TEMPLATES 8
static uint8_t boss_macros[ARENA_MAX][ARENA_MACROS], tpl_macros[TEMPLATES][ARENA_MACROS];
static int boss_macros_set[ARENA_MAX], tpl_set[TEMPLATES], macros_read;
static uint8_t prep_m[ARENA_MACROS];             /* the Prepare screen's: what the fight will have */
static int prep_boss = -1, prep_slot, prep_row = -1, prep_btn, prep_grab = -1, tpl_btn;
#define PREP_WAVE 1000                           /* prep_boss: Wave Mode's party, not a boss's */
static ArenaBoss wave_prep_src;                  /* (its party and items, as the run will have them) */
static const ArenaBoss *prep_src(void) { return prep_boss == PREP_WAVE ? &wave_prep_src : arena_boss(prep_boss); }
static uint64_t prep_note_until;
static char prep_note[80];
#define PICK_MOVE 0x7FFF                         /* the pick list's last entry: move this turn */
#define PICK_ROWS 9
static int prep_pick = -1, prep_pick_sel, prep_pick_top, prep_pick_n, prep_copy = -1;   /* the list: for that turn */
static int prep_pick_opt[100];
static void macros_path(char *out, size_t n) {
    char dir[1100];
    path_in_home(dir, sizeof dir, "arena");
    snprintf(out, n, "%s/macros.txt", dir);
}
static void macros_read_file(void) {
    if (macros_read) return;
    macros_read = 1;
    char path[1200];
    macros_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[600], kind[16], hex[400];
    int k;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%15s %d %399s", kind, &k, hex) != 3 || strlen(hex) != 2 * ARENA_MACROS) continue;
        uint8_t m[ARENA_MACROS];
        for (int i = 0; i < ARENA_MACROS; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); m[i] = (uint8_t)v; }
        if (!strcmp(kind, "boss") && k >= 0 && k < ARENA_MAX) { memcpy(boss_macros[k], m, ARENA_MACROS); boss_macros_set[k] = 1; }
        if (!strcmp(kind, "template") && k >= 1 && k <= TEMPLATES) { memcpy(tpl_macros[k - 1], m, ARENA_MACROS); tpl_set[k - 1] = 1; }
    }
    fclose(f);
}
static void macros_write_file(void) {
    char dir[1100], path[1200];
    path_in_home(dir, sizeof dir, "arena");
    SDL_CreateDirectory(dir);
    macros_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) { SDL_Log("arena: cannot write %s", path); return; }
    fprintf(f, "; Boss Arena macros: each boss's as last fought, and the templates (8 macros x 5 turns x 4 bytes)\n");
    for (int k = 0; k < ARENA_MAX; k++)
        if (boss_macros_set[k]) {
            fprintf(f, "boss %d ", k);
            for (int i = 0; i < ARENA_MACROS; i++) fprintf(f, "%02X", boss_macros[k][i]);
            fprintf(f, "\n");
        }
    for (int k = 0; k < TEMPLATES; k++)
        if (tpl_set[k]) {
            fprintf(f, "template %d ", k + 1);
            for (int i = 0; i < ARENA_MACROS; i++) fprintf(f, "%02X", tpl_macros[k][i]);
            fprintf(f, "\n");
        }
    fclose(f);
}
static void boss_macros_keep(int i, const uint8_t *m) {   /* (kept by the boss's key) */
    int k = boss_key(i);
    if (k < 0 || k >= ARENA_MAX) return;
    macros_read_file();
    memcpy(boss_macros[k], m, ARENA_MACROS);
    boss_macros_set[k] = 1;
    macros_write_file();
}

/* what a party member can be told to do in a macro: attack, defend, their techniques and
 * skills, the items carried (each once); (command << 8 | ID) */
/* the ROM's tables of techniques, skills and items (see prep_describe) */
#define TECH_DATA  0x2A9BE8u
#define SKILL_DATA 0x2A9D28u
#define ITEM_DATA  0x2A8E12u
static int prep_options(const ArenaBoss *ab, int k, int *opt, int cap) {
    int n = 0;
    if (!ab || k < 0 || k >= ab->n) return 0;
    opt[n++] = 1 << 8;
    opt[n++] = 5 << 8;
    uint8_t t[16], s[8];
    int nt, ns;
    arena_member_arts_of(&ab->party[k], t, &nt, s, &ns);
    for (int j = 0; j < nt && n < cap; j++) opt[n++] = 2 << 8 | t[j];
    for (int j = 0; j < ns && n < cap; j++) opt[n++] = 3 << 8 | s[j];
    for (int j = 0; j < 40 && n < cap; j++) {
        int it = ab->items[j], dup = 0;
        if (!it) continue;
        if (m68k_rom && !m68k_rom[ITEM_DATA + (uint32_t)it * 22u + 7]) continue;   /* (no use in battle: a vehicle, a key) */
        for (int q = 0; q < j; q++) dup |= ab->items[q] == it;
        if (!dup) opt[n++] = 4 << 8 | it;
    }
    return n;
}
/* What a choice is, from the ROM's own tables (TechniqueData, SkillData: 8 bytes each from ID 1,
 * effect, TP or the stat used, targets, power, -, element; the items: 22 bytes each from ID 0,
 * effect, power, targets): "Fire damage to one enemy, power 24" */
static const char *prep_target(int t, int item) {
    (void)item;                                      /* (the items' targets read as the techniques' do) */
    switch (t & 15) {
    case 1: return "one enemy";
    case 2: return "all enemies";
    case 3: return "the user";
    case 4: return "one human ally";
    case 5: return "all human allies";
    case 6: return "one android";
    case 7: return "all androids";
    case 8: return "one ally";
    case 9: return "the whole party";
    default: return "";
    }
}
static const char *prep_element(int e) {
    static const char *const E[15] = {"", "", "Energy", "Fire", "Gravity", "Ice", "Holy", "Lightning", "Holy-word",
                                      "Brose", "Bio", "Psychic", "Mechanical", "Efess", "Death"};
    return e > 0 && e < 15 ? E[e] : "";
}
static void prep_effect(int eff, int target, int power, int elem, int item, char *out, size_t n) {
    const char *t = prep_target(target, item), *el = prep_element(elem);
    char pw[24] = "";
    if (power && (eff == 1 || eff == 0x12)) snprintf(pw, sizeof pw, ", power %d", power);
    switch (eff) {
    case 0x01: snprintf(out, n, "%s%sdamage to %s%s", el, *el ? " " : "", t, pw); if (!*el) out[0] = 'D'; break;
    case 0x02: snprintf(out, n, "%s%sinstant death to %s", el, *el ? " " : "", t); if (!*el) out[0] = 'I'; break;
    case 0x03: snprintf(out, n, "Lowers the attack of %s", t); break;
    case 0x04: snprintf(out, n, "Lowers the defense of %s", t); break;
    case 0x06: snprintf(out, n, "Lowers the agility of %s", t); break;
    case 0x07: snprintf(out, n, "Puts %s to sleep", t); break;
    case 0x08: snprintf(out, n, "Seals the techniques of %s", t); break;
    case 0x09: snprintf(out, n, "Raises the attack of %s", t); break;
    case 0x0A: snprintf(out, n, "Raises the defense of %s", t); break;
    case 0x0B: snprintf(out, n, "Raises the technique defense of %s", t); break;
    case 0x0C: snprintf(out, n, "Raises the agility of %s", t); break;
    case 0x0D: snprintf(out, n, "Raises the elemental resistance of %s", t); break;
    case 0x0E: snprintf(out, n, "Wakes %s", t); break;
    case 0x12: snprintf(out, n, "Heals %s%s", t, pw); break;
    case 0x13: snprintf(out, n, "Cures poison: %s", t); break;
    case 0x14: snprintf(out, n, "Cures paralysis: %s", t); break;
    case 0x15: snprintf(out, n, "Revives %s", t); break;
    case 0x16: snprintf(out, n, "Fully heals %s", t); break;
    case 0x1B: snprintf(out, n, "Poisons %s", t); break;
    case 0x1C: snprintf(out, n, "Paralyzes %s", t); break;
    case 0x1D: snprintf(out, n, "Destroys an android: %s", t); break;
    case 0x21: snprintf(out, n, "Lowers the dexterity of %s", t); break;
    case 0x26: snprintf(out, n, "Raises the dexterity of %s", t); break;
    case 0x27: snprintf(out, n, "Undoes stat boosts and drops on %s", t); break;
    case 0x2B: snprintf(out, n, "Raises every stat of %s", t); break;
    default: snprintf(out, n, "Used on %s", t); break;
    }
}
/* what a macro turn's command does (cmd 1 attack, 2 tech, 3 skill, 4 item, 5 defend) */
static void prep_describe(int cmd, int id, char *out, size_t n) {
    const uint8_t *r = m68k_rom;
    out[0] = 0;
    if (cmd == 1) { snprintf(out, n, "A normal attack on one enemy"); return; }
    if (cmd == 5) { snprintf(out, n, "Guards for the round: less damage taken"); return; }
    if (!r || id <= 0) return;
    if (cmd == 2 || cmd == 3) {
        const uint8_t *d = r + (cmd == 2 ? TECH_DATA : SKILL_DATA) + (uint32_t)(id - 1) * 8u;
        prep_effect(d[0], d[2], d[3], d[5], 0, out, n);
        if (cmd == 3 && (d[1] & 0x80)) snprintf(out + strlen(out), n - strlen(out), "; needs a weapon");
    } else if (cmd == 4) {
        const uint8_t *d = r + ITEM_DATA + (uint32_t)id * 22u;
        prep_effect(d[0], d[2], d[1], 0, 1, out, n);
    }
}
/* what it costs member k: TP, a skill's uses, how many of the item the party carries */
static void prep_cost(const ArenaBoss *ab, int k, int cmd, int id, char *out, size_t n) {
    out[0] = 0;
    if (!ab || k < 0 || k >= ab->n || !m68k_rom) return;
    if (cmd == 2) snprintf(out, n, "%d TP", m68k_rom[TECH_DATA + (uint32_t)(id - 1) * 8u + 1]);
    else if (cmd == 3) {
        uint8_t c[0x80];
        arena_member_char(&ab->party[k], c);
        for (int j = 0; j < 8; j++)
            if (c[0x62 + j] == id) { snprintf(out, n, "%d use%s", c[0x6A + 2 * j], c[0x6A + 2 * j] == 1 ? "" : "s"); break; }
    } else if (cmd == 4) {
        int c = 0;
        for (int j = 0; j < 40; j++) c += ab->items[j] == id;
        snprintf(out, n, "x%d", c);
    }
}
static void prep_cmd_text(const uint8_t *e, char *out, size_t n);

static int party_index(const ArenaBoss *ab, int id) {
    for (int k = 0; ab && k < ab->n; k++) if (ab->party[k].id == id) return k;
    return -1;
}
static int macro_empty(const uint8_t *m, int slot) {
    for (int i = 0; i < 20; i++) if (m[slot * 20 + i]) return 0;
    return 1;
}
/* a set of macros made fit for this fight's party: each macro's turns for those in the party, in
 * its order, then any of the party it leaves out; what one can't do here becomes an attack */
static void macros_fit(const ArenaBoss *ab, const uint8_t *src, uint8_t *dst) {
    memset(dst, 0, ARENA_MACROS);
    if (!ab) return;
    for (int s = 0; s < 8; s++) {
        if (macro_empty(src, s)) continue;
        int used[5] = {0}, t = 0;
        uint8_t *d = dst + s * 20;
        for (int j = 0; j < 5; j++) {
            const uint8_t *e = src + s * 20 + 4 * j;
            int k = party_index(ab, e[0]);
            if (!e[1] || k < 0 || used[k]) continue;
            int opt[96], n = prep_options(ab, k, opt, 96), ok = 0, want = e[1] << 8 | e[2];
            for (int q = 0; q < n; q++) ok |= opt[q] == want || (e[1] == 1 && opt[q] == 1 << 8) || (e[1] == 5 && opt[q] == 5 << 8);
            used[k] = 1;
            d[4 * t] = e[0];
            d[4 * t + 1] = ok ? e[1] : 1;
            d[4 * t + 2] = ok && e[1] != 1 && e[1] != 5 ? e[2] : 0;
            t++;
        }
        for (int k = 0; k < ab->n && t < 5; k++)
            if (!used[k]) { d[4 * t] = ab->party[k].id; d[4 * t + 1] = 1; t++; }
    }
}
static void prep_enter(int boss) {
    macros_read_file();
    prep_boss = boss;
    int key = boss_key(boss);                       /* (Wave Mode's party: none) */
    if (key >= 0 && key < ARENA_MAX && boss_macros_set[key]) macros_fit(arena_boss(boss), boss_macros[key], prep_m);
    else arena_default_macros(boss, prep_m);
    prep_slot = 0;
    prep_row = arena_boss(boss) ? arena_boss(boss)->n : 0;   /* on Fight: one press to fight as before */
    prep_btn = 0;
    prep_grab = -1;
    prep_pick = prep_copy = -1;
    prep_note[0] = 0;
    menu_screen = MENU_ARENA_PREP;
}
static void arena_fight_chosen(void);
static void arena_pick(void) {
    if (menu_sel < 0 || menu_sel >= arena_count()) return;
    prep_enter(menu_sel);
}
/* the Prepare screen's Fight: the macros go with the fight, and are kept for a rematch */
static void arena_fight_chosen(void) {
    menu_sel = prep_boss;
    arena_set_macros(prep_m);
    boss_macros_keep(prep_boss, prep_m);
    if (in_launcher) {                               /* (the launcher fades out as it does for Continue) */
        launch_arena = prep_boss;
        menu_screen = MENU_LAUNCHER;
        menu_sel = L_ARENA;
        launcher_go = 3;
        launcher_sfx(LOAD_PICK_SFX, 1200);
    } else {
        arena_go = menu_sel;
        menu_close();
    }
}
static void prep_say(const char *s) {
    snprintf(prep_note, sizeof prep_note, "%s", s);
    prep_note_until = SDL_GetTicks() + 2200;
}
/* Left / Right on a turn: the next (or last) thing that member can do */
static void prep_cycle(int row, int dir) {
    const ArenaBoss *ab = prep_src();
    if (!ab || row < 0 || row >= ab->n) return;
    uint8_t *m = prep_m + prep_slot * 20;
    if (macro_empty(prep_m, prep_slot))              /* an empty macro: everyone attacks, to start from */
        for (int k = 0; k < ab->n; k++) { m[4 * k] = ab->party[k].id; m[4 * k + 1] = 1; }
    uint8_t *e = m + 4 * row;
    int k = party_index(ab, e[0]), opt[96], n = prep_options(ab, k, opt, 96), at = 0;
    if (n <= 0) return;
    int cur = e[1] << 8 | (e[1] == 1 || e[1] == 5 ? 0 : e[2]);
    for (int q = 0; q < n; q++) if (opt[q] == cur) at = q;
    at = (at + dir + n) % n;
    e[1] = (uint8_t)(opt[at] >> 8);
    e[2] = (uint8_t)(opt[at] & 0xFF);
}
static void prep_move(int up, int down) {
    const ArenaBoss *ab = prep_src();
    int n = ab ? ab->n : 0;
    if (prep_grab >= 0) {                            /* a turn picked up: it moves through the order */
        int to = prep_grab + (down ? 1 : up ? -1 : 0);
        if (to >= 0 && to < n && to != prep_grab) {
            uint8_t *m = prep_m + prep_slot * 20, t[4];
            memcpy(t, m + 4 * prep_grab, 4);
            memcpy(m + 4 * prep_grab, m + 4 * to, 4);
            memcpy(m + 4 * to, t, 4);
            prep_grab = prep_row = to;
        }
        return;
    }
    if (up) prep_row = prep_row <= -1 ? n : prep_row - 1;
    if (down) prep_row = prep_row >= n ? -1 : prep_row + 1;
}
static void wave_prep_done(void);
static void prep_ok(void) {
    const ArenaBoss *ab = prep_src();
    int n = ab ? ab->n : 0;
    if (prep_row == -1) {
        if (prep_copy >= 0) {                        /* Copy: here */
            char t[64];
            if (prep_copy == prep_slot) snprintf(t, sizeof t, "Pick another letter to copy %c to", 'A' + prep_copy);
            else {
                memcpy(prep_m + prep_slot * 20, prep_m + prep_copy * 20, 20);
                snprintf(t, sizeof t, "%c-Macro copied to %c", 'A' + prep_copy, 'A' + prep_slot);
                prep_copy = -1;
            }
            prep_say(t);
            return;
        }
        prep_row = 0;
        return;
    }
    if (prep_row < n) {
        if (prep_grab >= 0) { prep_grab = -1; return; }  /* a turn moved: put it down */
        if (macro_empty(prep_m, prep_slot)) prep_cycle(prep_row, 0);   /* (an empty macro: everyone attacks first) */
        const uint8_t *e = prep_m + prep_slot * 20 + 4 * prep_row;
        int k = party_index(ab, e[0]), cur = e[1] << 8 | (e[1] == 1 || e[1] == 5 ? 0 : e[2]);
        prep_pick_n = prep_options(ab, k, prep_pick_opt, 99);
        prep_pick_opt[prep_pick_n++] = PICK_MOVE;
        prep_pick_sel = 0;
        for (int q = 0; q < prep_pick_n; q++) if (prep_pick_opt[q] == cur) prep_pick_sel = q;
        prep_pick_top = prep_pick_sel >= PICK_ROWS ? prep_pick_sel - PICK_ROWS + 1 : 0;
        prep_pick = prep_row;
        return;
    }
    if (prep_btn == 0) { if (prep_boss == PREP_WAVE) wave_prep_done(); else arena_fight_chosen(); }
    else if (prep_btn == 1) { menu_screen = MENU_ARENA_TPL; menu_sel = 0; tpl_btn = 0; }
    else if (prep_btn == 2) {                        /* Copy: this macro, then a letter for it */
        if (macro_empty(prep_m, prep_slot)) { prep_say("Nothing to copy: this macro is empty"); return; }
        prep_copy = prep_slot;
        prep_row = -1;
        prep_slot = (prep_slot + 1) % 8;
    }
    else { memset(prep_m + prep_slot * 20, 0, 20); prep_say("Macro cleared"); }
}
/* the pick list: a choice made (or the turn picked up to move) */
static void prep_pick_set(void) {
    if (prep_pick < 0 || prep_pick_sel < 0 || prep_pick_sel >= prep_pick_n) return;
    int v = prep_pick_opt[prep_pick_sel], row = prep_pick;
    prep_pick = -1;
    if (v == PICK_MOVE) { prep_grab = row; return; }
    uint8_t *e = prep_m + prep_slot * 20 + 4 * row;
    e[1] = (uint8_t)(v >> 8);
    e[2] = (uint8_t)(v & 0xFF);
}
static void prep_pick_move(int d) {
    if (prep_pick_n <= 0) return;
    if (d == 1 || d == -1) prep_pick_sel = (prep_pick_sel + d + prep_pick_n) % prep_pick_n;
    else prep_pick_sel = prep_pick_sel + d < 0 ? 0 : prep_pick_sel + d >= prep_pick_n ? prep_pick_n - 1 : prep_pick_sel + d;
    if (prep_pick_sel < prep_pick_top) prep_pick_top = prep_pick_sel;
    if (prep_pick_sel >= prep_pick_top + PICK_ROWS) prep_pick_top = prep_pick_sel - PICK_ROWS + 1;
}
static void tpl_ok(void) {
    int k = menu_sel;
    if (k < 0 || k >= TEMPLATES) return;
    if (tpl_btn == 0) {                              /* Load: into this fight's macros, made to fit */
        if (!tpl_set[k]) return;
        macros_fit(prep_src(), tpl_macros[k], prep_m);
        menu_screen = MENU_ARENA_PREP;
        snprintf(prep_note, sizeof prep_note, "Template %d loaded", k + 1);
        prep_note_until = SDL_GetTicks() + 2200;
    } else {                                         /* Save: this fight's macros as the template */
        memcpy(tpl_macros[k], prep_m, ARENA_MACROS);
        tpl_set[k] = 1;
        macros_write_file();
        menu_screen = MENU_ARENA_PREP;
        snprintf(prep_note, sizeof prep_note, "Saved as template %d", k + 1);
        prep_note_until = SDL_GetTicks() + 2200;
    }
}
static void arena_move(int d) {
    int n = arena_count();
    if (d == 1 || d == -1) menu_sel = (menu_sel + d + n) % n;
    else menu_sel = menu_sel + d < 0 ? 0 : menu_sel + d >= n ? n - 1 : menu_sel + d;
    arena_keep_visible();
}

/* ------------------------------------------------------------------ Wave Mode (wave.h): the menus' side */
/* The runs' list (Tab from the Boss Arena's), a run's party and macros, and how a run went.
 * Records: arena/waves.txt in the player's folder, a line a cleared run ("clear RUN FRAMES
 * IDS FELL WHEN"): the quickest of each run is shown. */
static int wave_sel_run, wave_picks[5], wave_npicks, wave_setup_btn = -1, wave_go, launch_wave = -1;
static uint8_t wave_m[ARENA_MACROS];
static int wave_m_for = -1;                      /* the run wave_m was set for (another run: everyone attacks) */
static int wave_result = -1, wave_result_run, wave_result_node, wave_result_best, wave_result_btn;
static uint32_t wave_result_frames;
static uint8_t wave_result_ids[5], wave_result_fell;
static int wave_result_n;
#define WAVE_RUNS_MAX 16
static uint32_t wave_best[WAVE_RUNS_MAX];        /* frames; 0 not cleared */
static uint8_t wave_best_ids[WAVE_RUNS_MAX][5], wave_best_n[WAVE_RUNS_MAX], wave_best_fell[WAVE_RUNS_MAX];
static int wave_records_read;

static void wave_time_text(uint32_t f, char *out, size_t n) {
    uint32_t s = f / 60, cs = (f % 60) * 100 / 60;
    snprintf(out, n, "%u:%02u.%02u", (unsigned)(s / 60), (unsigned)(s % 60), (unsigned)cs);
}
static void wave_records_path(char *out, size_t n) {
    char dir[1100];
    path_in_home(dir, sizeof dir, "arena");
    snprintf(out, n, "%s/waves.txt", dir);
}
static void wave_records_load(void) {
    if (wave_records_read) return;
    wave_records_read = 1;
    char path[1200], line[300], ids[32];
    wave_records_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int r;
    unsigned fr, fell;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "clear %d %u %31s %u", &r, &fr, ids, &fell) != 4 || r < 0 || r >= WAVE_RUNS_MAX || !fr) continue;
        if (wave_best[r] && fr >= wave_best[r]) continue;
        wave_best[r] = fr;
        wave_best_fell[r] = (uint8_t)fell;
        int n = 0;
        for (int k = 0; ids[2 * k] && ids[2 * k + 1] && n < 5; k++) {
            unsigned v;
            if (sscanf(ids + 2 * k, "%2x", &v) == 1) wave_best_ids[r][n++] = (uint8_t)v;
        }
        wave_best_n[r] = (uint8_t)n;
    }
    fclose(f);
}
/* a cleared run: kept; 1 if it's the run's quickest */
static int wave_record_add(int r, uint32_t fr, const uint8_t *ids, int n, uint8_t fell) {
    wave_records_load();
    char dir[1100], path[1200];
    path_in_home(dir, sizeof dir, "arena");
    SDL_CreateDirectory(dir);
    wave_records_path(path, sizeof path);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "clear %d %u ", r, (unsigned)fr);
        for (int k = 0; k < n; k++) fprintf(f, "%02X", ids[k]);
        fprintf(f, " %u %lld\n", (unsigned)fell, (long long)time(NULL));
        fclose(f);
    } else SDL_Log("wave: cannot write %s", path);
    if (r < 0 || r >= WAVE_RUNS_MAX || (wave_best[r] && fr >= wave_best[r])) return 0;
    wave_best[r] = fr;
    memcpy(wave_best_ids[r], ids, (size_t)n);
    wave_best_n[r] = (uint8_t)n;
    wave_best_fell[r] = fell;
    return 1;
}

static void wave_enter(void) {
    if (!arena_faces && rom_ready) {                 /* the faces, from the ROM (as the Boss Arena's) */
        if (in_launcher && !launcher_music) bus_init(rom_buf, rom_size);
        arena_faces = ui_portraits_init() > 0;
    }
    wave_records_load();
    if (menu_screen == MENU_CLOSED) menu_opened = SDL_GetTicks();
    menu_screen = MENU_WAVE;
    menu_sel = wave_sel_run >= 0 && wave_sel_run < wave_count() ? wave_sel_run : 0;
}
/* Bonus Modes: the Boss Arena and Wave Mode, one above the other */
static void bonus_enter(void) {
    if (!arena_faces && rom_ready) {                 /* (the faces: both lists show them) */
        if (in_launcher && !launcher_music) bus_init(rom_buf, rom_size);
        arena_faces = ui_portraits_init() > 0;
    }
    wave_records_load();
    if (menu_screen == MENU_CLOSED) menu_opened = SDL_GetTicks();
    if (in_launcher) launcher_sfx(LOAD_OPEN_SFX, 350);
    menu_sel = menu_screen == MENU_WAVE ? 1 : menu_screen == MENU_ARENA ? 0 : menu_screen == MENU_JUKEBOX ? 2
             : wave_mode ? 1 : 0;
    menu_screen = MENU_BONUS;
}

/* the Jukebox: the list scrolled so the song under the cursor shows */
static void jukebox_scroll(int d) {
    jukebox_top += d;
    if (jukebox_top > JUKEBOX_N - JUKEBOX_ROWS) jukebox_top = JUKEBOX_N - JUKEBOX_ROWS;
    if (jukebox_top < 0) jukebox_top = 0;
}
static void jukebox_move(int d) {
    if (menu_sel < 0) { if (d > 0) menu_sel = 0; return; }   /* the switch, above the list */
    if (d < 0 && menu_sel == 0) { menu_sel = -1; return; }
    if (d == 1 || d == -1) menu_sel = (menu_sel + d + JUKEBOX_N) % JUKEBOX_N;
    else menu_sel = menu_sel + d < 0 ? 0 : menu_sel + d >= JUKEBOX_N ? JUKEBOX_N - 1 : menu_sel + d;
    if (menu_sel < jukebox_top) jukebox_top = menu_sel;
    if (menu_sel >= jukebox_top + JUKEBOX_ROWS) jukebox_top = menu_sel - JUKEBOX_ROWS + 1;
}
static void save_setting_str(const char *key, const char *v);
static void prep_say(const char *s);
/* the highlighted song as the launcher's own (L, the pad's Y, its button) */
static void jukebox_set_launcher(int i) {
    if (i < 0 || i >= JUKEBOX_N) return;
    char v[8], t[96];
    snprintf(v, sizeof v, "%02X", JUKEBOX_FIRST + i);
    save_setting_str("audio.launcher_song", v);
    snprintf(t, sizeof t, "The launcher now opens with %s", JUKEBOX_TITLES[i]);
    prep_say(t);
}
static void jukebox_pick(int i) {                    /* a song: it plays; the one playing: it stops */
    if (i < 0 || i >= JUKEBOX_N) return;
    jukebox_song = jukebox_song == JUKEBOX_FIRST + i ? 0 : JUKEBOX_FIRST + i;
}
/* the songs your music folder has a file for: music.ini's "8F = file" lines, or 8F.wav (as music.c finds them) */
static void jukebox_find_files(void) {
    char dir[1100], path[1400], line[700];
    path_in_home(dir, sizeof dir, "music");
    char named[64][300];
    memset(named, 0, sizeof named);
    snprintf(path, sizeof path, "%s/music.ini", dir);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *p = line;
        unsigned h;
        int used;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || sscanf(p, "%x%n", &h, &used) != 1 || h < JUKEBOX_FIRST || h >= JUKEBOX_FIRST + JUKEBOX_N) continue;
        p += used;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = 0;
        snprintf(named[h - JUKEBOX_FIRST], sizeof named[0], "%s", p);
    }
    if (f) fclose(f);
    jukebox_files = 0;
    for (int i = 0; i < JUKEBOX_N; i++) {
        const char *n = named[i];
        char own[16];
        if (!n[0]) { snprintf(own, sizeof own, "%02X.wav", JUKEBOX_FIRST + i); n = own; }
        if (n[0] == '/' || n[0] == '\\' || (n[0] && n[1] == ':')) snprintf(path, sizeof path, "%s", n);
        else snprintf(path, sizeof path, "%s/%s", dir, n);
        FILE *g = fopen(path, "rb");
        jukebox_file[i] = g != NULL;
        if (g) fclose(g);
        jukebox_files += jukebox_file[i];
    }
}
static void jukebox_source(int dir) {                /* the switch: the next source (the pack: if there are files) */
    for (int k = 0; k < JB_N; k++) {
        jukebox_src = (jukebox_src + dir + JB_N) % JB_N;
        if (jukebox_src != JB_PACK || jukebox_files) break;
    }
}
static void jukebox_enter(void) {
    jukebox_find_files();
    if (jukebox_src < 0)                             /* the first time: as Settings have it */
        jukebox_src = jukebox_files && SDL_strcasecmp(setting("enhance.custom_music"), "off") ? JB_PACK
                    : !SDL_strcasecmp(setting("enhance.music"), "proto") ? JB_PROTO
                    : !SDL_strcasecmp(setting("enhance.music"), "algol") || !SDL_strcasecmp(setting("enhance.music"), "algol2")
                    ? JB_ALGOL : JB_CHIP;
    if (jukebox_src == JB_PACK && !jukebox_files) jukebox_src = JB_CHIP;
    menu_screen = MENU_JUKEBOX;
    menu_sel = jukebox_song ? jukebox_song - JUKEBOX_FIRST : 0;
    jukebox_top = menu_sel - JUKEBOX_ROWS / 2;
    jukebox_scroll(0);
}
/* Bonus Modes' cards: the Boss Arena, Wave Mode, the Jukebox (the launcher's: in the game, the game
 * holds its own sound driver, still while a menu is up) */
static void bonus_open(int k) {
    if (k == 0) arena_enter();
    else if (k == 1) wave_enter();
    else if (in_launcher) jukebox_enter();
}

/* the run's party as the Prepare screen and the run will have it */
static void wave_fill_src(void) {
    const WaveRun *w = wave_run(wave_sel_run);
    memset(&wave_prep_src, 0, sizeof wave_prep_src);
    if (!w) return;
    wave_prep_src.name = w->name;
    for (int k = 0; k < wave_npicks; k++) wave_prep_src.party[k] = w->roster[wave_picks[k]];
    wave_prep_src.n = (uint8_t)wave_npicks;
    memcpy(wave_prep_src.items, w->items, 40);
}
static void wave_default_macros(uint8_t *m) {    /* A: everyone attacks, in the party's order */
    memset(m, 0, ARENA_MACROS);
    for (int k = 0; k < wave_prep_src.n; k++) { m[4 * k] = wave_prep_src.party[k].id; m[4 * k + 1] = 1; }
}
static void wave_setup_enter(int r) {
    const WaveRun *w = wave_run(r);
    if (!w) return;
    if (r != wave_sel_run || wave_npicks == 0) {     /* another run: its first five, to start from */
        wave_npicks = 0;
        for (int k = 0; k < w->nroster && wave_npicks < 5; k++) wave_picks[wave_npicks++] = k;
    }
    wave_sel_run = r;
    wave_fill_src();
    if (wave_m_for != r) { wave_default_macros(wave_m); wave_m_for = r; }
    else { uint8_t t[ARENA_MACROS]; memcpy(t, wave_m, ARENA_MACROS); macros_fit(&wave_prep_src, t, wave_m); }
    menu_screen = MENU_WAVE_SETUP;
    menu_sel = w->nroster;                           /* on Start */
    wave_setup_btn = 1;
}
static void wave_prep_enter(void) {
    wave_fill_src();
    if (!wave_prep_src.n) return;
    macros_fit(&wave_prep_src, wave_m, prep_m);
    if (macro_empty(prep_m, 0) && macro_empty(prep_m, 1)) wave_default_macros(prep_m);
    prep_boss = PREP_WAVE;
    prep_pick = prep_copy = -1;
    prep_slot = 0;
    prep_row = wave_prep_src.n;
    prep_btn = 0;
    prep_grab = -1;
    prep_note[0] = 0;
    menu_screen = MENU_ARENA_PREP;
}
static void wave_prep_done(void) {                   /* the Prepare screen's Done (or back): the run's macros */
    memcpy(wave_m, prep_m, ARENA_MACROS);
    wave_m_for = wave_sel_run;
    prep_boss = -1;
    menu_screen = MENU_WAVE_SETUP;
    menu_sel = wave_run(wave_sel_run) ? wave_run(wave_sel_run)->nroster : 0;
    wave_setup_btn = 0;
}
static void wave_toggle(int k) {                     /* a member: picked (last in the order), or dropped */
    for (int j = 0; j < wave_npicks; j++)
        if (wave_picks[j] == k) {
            memmove(&wave_picks[j], &wave_picks[j + 1], (size_t)(wave_npicks - j - 1) * sizeof wave_picks[0]);
            wave_npicks--;
            wave_fill_src();
            return;
        }
    if (wave_npicks < 5) wave_picks[wave_npicks++] = k;
    else prep_say("Five is the most a party can have");
    wave_fill_src();
}
/* Start: the party and macros go with the run; from the launcher, as a Boss Arena fight goes */
static void wave_start_chosen(void) {
    if (!wave_npicks) { prep_say("Pick at least one"); return; }
    wave_fill_src();
    uint8_t m[ARENA_MACROS], picks[5];
    macros_fit(&wave_prep_src, wave_m, m);
    if (macro_empty(m, 0) && macro_empty(m, 1)) wave_default_macros(m);
    for (int k = 0; k < wave_npicks; k++) picks[k] = (uint8_t)wave_picks[k];
    wave_setup(wave_sel_run, picks, wave_npicks, m);
    if (in_launcher) {
        launch_wave = wave_sel_run;
        menu_screen = MENU_LAUNCHER;
        menu_sel = L_ARENA;
        launcher_go = 4;
        launcher_sfx(LOAD_PICK_SFX, 1200);
    } else {
        wave_go = 1;
        menu_close();
    }
}
static void wave_result_enter(void) {
    if (menu_screen == MENU_CLOSED) menu_opened = SDL_GetTicks();
    menu_screen = MENU_WAVE_RESULT;
    menu_sel = 0;
    wave_result_btn = 0;
}
static void wave_menu_input(int up, int down, int left, int right, int ok) {
    if (menu_screen == MENU_WAVE) {
        int n = wave_count();
        if (up) menu_sel = (menu_sel + n - 1) % n;
        if (down) menu_sel = (menu_sel + 1) % n;
        wave_sel_run = wave_sel_run < 0 ? 0 : wave_sel_run;
        if (ok) wave_setup_enter(menu_sel);
        return;
    }
    if (menu_screen == MENU_WAVE_RESULT) {
        if (left || right) wave_result_btn ^= 1;
        if (ok) {
            if (wave_result_btn == 0) wave_start_chosen();   /* the same run again: the same party and macros */
            else wave_enter();
        }
        return;
    }
    const WaveRun *w = wave_run(wave_sel_run);       /* MENU_WAVE_SETUP: the roster, two rows of five; the buttons */
    if (!w) return;
    int n = w->nroster;
    if (menu_sel < n) {
        int row = menu_sel / 5, col = menu_sel % 5;
        if (left && col > 0) menu_sel--;
        if (right && col < 4 && menu_sel + 1 < n) menu_sel++;
        if (up && row > 0) menu_sel -= 5;
        if (down) {
            if (menu_sel + 5 < n) menu_sel += 5;
            else { menu_sel = n; wave_setup_btn = wave_setup_btn < 0 ? 1 : wave_setup_btn; }
        }
        if (ok) wave_toggle(menu_sel);
    } else {
        if (left || right) wave_setup_btn ^= 1;
        if (up) menu_sel = n > 5 ? 5 : 0;
        if (ok) { if (wave_setup_btn == 0) wave_prep_enter(); else wave_start_chosen(); }
    }
}
static uint64_t wave_menu_sig(void) {
    uint64_t h = (uint64_t)(wave_sel_run + 1) * 31u + (uint64_t)(wave_npicks + 1) * 7u + (uint64_t)(wave_setup_btn + 2);
    for (int k = 0; k < wave_npicks; k++) h = h * 13u + (uint64_t)wave_picks[k] + 1u;
    h = h * 17u + (uint64_t)(wave_result + 2) * 5u + (uint64_t)wave_result_btn;
    h = h * 19u + (prep_note[0] && SDL_GetTicks() < prep_note_until ? 1u : 0u);
    for (int r = 0; r < WAVE_RUNS_MAX; r++) h = h * 23u + wave_best[r];
    return h;
}
/* the project's Discord server -- the launcher's Discord button, D: the invite
 * opens in the player's browser (or the Discord app, where it takes such links) */
#define DISCORD_URL "https://discord.gg/YmgWfQGJAr"
static void open_discord(void) {
    if (!SDL_OpenURL(DISCORD_URL)) SDL_Log("Discord: %s could not be opened: %s", DISCORD_URL, SDL_GetError());
}
/* the launcher's music on or off -- its note, M, the pad's north button: the
 * launcher's loop starts it, or fades it out (launcher_music_run) */
static void launcher_music_toggle(void) {
    save_setting_str("audio.launcher_music", SDL_strcasecmp(setting("audio.launcher_music"), "off") ? "off" : "on");
    jukebox_song = 0;                                /* (the note, M: the Jukebox's song stops too) */
}
#define MODE_ITEM (&GAME_ITEMS[0])    /* Classic / Enhanced */
static int menu_page;                 /* the settings page (kept while the game runs) */
static int menu_last_pad;             /* the last press was a gamepad's: the hints name its buttons */

static int item_values(const MenuItem *it) {
    int n = 0;
    while (n < 4 && it->values[n]) n++;
    return n;
}

static int arena_cheat_item(const MenuItem *it) {   /* the cheats and Rewind: off in the Boss Arena */
    return arena_mode && it->key && (!strncmp(it->key, "cheat.", 6) || !strcmp(it->key, "enhance.rewind"));
}
static int item_disabled(const MenuItem *it) {
    if (arena_cheat_item(it)) return 1;              /* the Boss Arena: no cheats (cheats_suspend) */
    return it->enhancement && classic_mode();
}

static void item_value(const MenuItem *it, char *buf, size_t n) {
    if (it->shown) { snprintf(buf, n, "%s", it->shown()); return; }
    if (it->step) { snprintf(buf, n, "%d%%", setting_int(it->key)); return; }
    const char *v = setting(it->key);
    for (int i = 0; i < item_values(it); i++)
        if (!SDL_strcasecmp(v, it->values[i])) { snprintf(buf, n, "%s", it->names[i]); return; }
    snprintf(buf, n, "%s", v);
}

/* a number setting to v, kept in its range: saved and applied (when it changed) */
static void item_set(const MenuItem *it, int v) {
    if (!it->key || item_disabled(it)) return;
    v = v < it->lo ? it->lo : v > it->hi ? it->hi : v;
    for (int i = 0; i < NSETTINGS; i++) {
        if (strcmp(settings[i].key, it->key)) continue;
        if (atoi(settings[i].value) == v && settings[i].value[0]) return;
        snprintf(settings[i].value, sizeof settings[i].value, "%d", v);
    }
    char path[1100];
    path_in_home(path, sizeof path, "psiv.ini");
    write_settings(path);
    if (it->apply) it->apply();
}

static void item_change(const MenuItem *it, int dir) {
    if (it->action) {                                 /* chosen (OK, right, a click): it does its thing */
        if (dir > 0 && !item_disabled(it)) it->action();
        return;
    }
    if (!it->key || item_disabled(it)) return;
    if (it->step) {                                   /* a number: a step up or down */
        item_set(it, setting_int(it->key) + dir * it->step);
        return;
    }
    char path[1100];
    for (int i = 0; i < NSETTINGS; i++) {
        if (strcmp(settings[i].key, it->key)) continue;
        int n = item_values(it), cur = 0;
        for (int j = 0; j < n; j++)
            if (!SDL_strcasecmp(settings[i].value, it->values[j])) cur = j;
        cur = (cur + dir + n) % n;
        snprintf(settings[i].value, sizeof settings[i].value, "%s", it->values[cur]);
    }
    path_in_home(path, sizeof path, "psiv.ini");
    write_settings(path);
    if (it->apply) it->apply();
}

/* after the menu closes, the game sees no buttons until all are released, so
 * the key that closed it (Enter = Start) doesn't also reach the game */
static int input_hold;
static void menu_open(void) {
    if (arena_scanning) return;                      /* (the Boss Arena's gathering runs on, unseen) */
    menu_screen = MENU_MAIN;
    menu_sel = 0;
    menu_opened = SDL_GetTicks();
}
static void menu_close(void) {
    if (in_launcher) { menu_screen = MENU_LAUNCHER; return; }   /* it stays until Start game */
    menu_screen = MENU_CLOSED;
    input_hold = 1;
}
static int menu_is_open(void) { return menu_screen != MENU_CLOSED; }

static const MenuPage *page_now(void) { return &PAGES[menu_page]; }
/* A page with more settings than the panel shows scrolls: by the wheel (or its arrows, tapped),
 * and as the keys or the pad move the highlight past an edge. The mouse resting on a row only
 * highlights it -- the list never moves under the pointer. */
static int settings_top, settings_top_page = -1;
static int settings_rows(void) { return page_now()->n <= TIGHT_ROWS ? page_now()->n : MAX_ROWS; }
static void settings_keep_top(void) {             /* the highlighted row in view (the keys, the pad) */
    int n = page_now()->n, rows = settings_rows();
    if (settings_top_page != menu_page) { settings_top = 0; settings_top_page = menu_page; }
    if (menu_sel >= 0 && menu_sel < settings_top) settings_top = menu_sel;
    if (menu_sel >= settings_top + rows) settings_top = menu_sel - rows + 1;
    if (settings_top > n - rows) settings_top = n - rows;
    if (settings_top < 0) settings_top = 0;
}
static void settings_scroll(int d) {              /* the wheel, the arrows: the list; the highlight stays on it */
    int n = page_now()->n, rows = settings_rows();
    if (settings_top_page != menu_page) { settings_top = 0; settings_top_page = menu_page; }
    settings_top += d;
    if (settings_top > n - rows) settings_top = n - rows;
    if (settings_top < 0) settings_top = 0;
    if (menu_sel >= 0 && menu_sel < settings_top) menu_sel = settings_top;
    if (menu_sel >= settings_top + rows) menu_sel = settings_top + rows - 1;
}

/* into Settings: the page's first setting, or the page bar on a page without any */
static void settings_enter(void) {
    menu_screen = MENU_SETTINGS;
    menu_sel = page_now()->n ? 0 : -1;
}

/* ------------------------------------------------------------------ the Journal */
/* journal.h: where the story is and what to do next (Now), what has happened
 * (Story so far), and the Hunters' Guild's jobs, once the guild is known.
 * A hint shows only when asked for, and stays shown for that step. */
enum { JT_NOW, JT_STORY, JT_GUILD };
static int j_tab;                     /* the page */
static int j_scroll;                  /* Story so far: the first entry shown (the newest is 0) */
static int j_more_below;              /* ... and there are more under the last one shown */
static int j_hint_step = -2;          /* the step whose hint is showing */
static int j_hint_job = -1;           /* the job whose hint is showing */

static int j_tabs(void) { return journal_guild_known() ? 3 : 2; }

/* the guild's jobs worth showing: offered, taken or done */
static int j_jobs(int *v) {
    int m = 0;
    JournalJob jb;
    for (int j = 0; j < journal_jobs() && m < 16; j++) {
        journal_job(j, &jb);
        if (jb.state != JOB_UNSEEN) v[m++] = j;
    }
    return m;
}

/* Story so far: the steps reached that say what happened, the newest first */
static int j_entries(int *v, int cap) {
    int m = 0;
    for (int i = journal_story(); i >= 0 && m < cap; i--) {
        const JournalStep *s = journal_step(i);
        if (s->done[0] && journal_reached(i)) v[m++] = i;
    }
    return m;
}

static void journal_enter(void) {
    menu_screen = MENU_JOURNAL;
    j_tab = JT_NOW;
    j_scroll = 0;
    menu_sel = 0;
}

static void journal_turn(int dir) {
    int n = j_tabs();
    j_tab = (j_tab + dir + n) % n;
    j_scroll = 0;
    menu_sel = 0;
}

static void journal_input(int up, int down, int left, int right, int ok) {
    if (j_tab >= j_tabs()) j_tab = JT_NOW;
    if (left || right) { journal_turn(right ? 1 : -1); return; }
    if (j_tab == JT_NOW) {
        int s = journal_story();
        if (ok && s >= 0) j_hint_step = j_hint_step == s ? -2 : s;
    } else if (j_tab == JT_STORY) {
        if (up && j_scroll > 0) j_scroll--;
        if (down && j_more_below) j_scroll++;
    } else {
        int v[16], m = j_jobs(v);
        if (!m) return;
        if (menu_sel >= m) menu_sel = m - 1;
        if (up) menu_sel = (menu_sel + m - 1) % m;
        if (down) menu_sel = (menu_sel + 1) % m;
        if (ok) j_hint_job = j_hint_job == v[menu_sel] ? -1 : v[menu_sel];
    }
}

/* the next or previous settings page or journal page (shoulder buttons, Tab, the page bar) */
static void menu_turn_page(int dir) {
    if (menu_screen == MENU_JOURNAL) { journal_turn(dir); return; }
    if (menu_screen == MENU_LOAD) { load_turn(dir); return; }
    if (menu_screen == MENU_ARENA) { (void)dir; wave_enter(); return; }
    if (menu_screen == MENU_JUKEBOX) { jukebox_source(dir); return; }   /* (Tab, the shoulders: what plays) */
    if (menu_screen == MENU_WAVE) { (void)dir; arena_enter(); return; }
    if (menu_screen == MENU_ARENA_PREP) { prep_slot = (prep_slot + dir + 8) % 8; prep_grab = -1; return; }
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
        else if (menu_screen == MENU_LOAD) { menu_screen = MENU_LAUNCHER; menu_sel = L_LOAD; }
        else if (menu_screen == MENU_ARENA_TPL) menu_screen = MENU_ARENA_PREP;
        else if (menu_screen == MENU_ARENA_PREP && prep_pick >= 0) prep_pick = -1;
        else if (menu_screen == MENU_ARENA_PREP && prep_copy >= 0) { prep_slot = prep_copy; prep_copy = -1; prep_say("Copy cancelled"); }
        else if (menu_screen == MENU_ARENA_PREP && prep_grab >= 0) prep_grab = -1;
        else if (menu_screen == MENU_ARENA_PREP && prep_boss == PREP_WAVE) wave_prep_done();
        else if (menu_screen == MENU_ARENA_PREP) { menu_screen = MENU_ARENA; menu_sel = prep_boss; arena_keep_visible(); }
        else if (menu_screen == MENU_WAVE_SETUP) { menu_screen = MENU_WAVE; menu_sel = wave_sel_run; }
        else if (menu_screen == MENU_WAVE_RESULT) wave_enter();
        else if (menu_screen == MENU_WAVE) { menu_screen = MENU_BONUS; menu_sel = 1; }
        else if (menu_screen == MENU_ARENA) { menu_screen = MENU_BONUS; menu_sel = 0; }
        else if (menu_screen == MENU_JUKEBOX) { menu_screen = MENU_BONUS; menu_sel = 2; }
        else if (menu_screen == MENU_BONUS && in_launcher) { menu_screen = MENU_LAUNCHER; menu_sel = L_ARENA; }
        else {                                       /* back to the main menu, on the line it came from */
            menu_sel = menu_screen == MENU_SETTINGS ? M_SETTINGS
                     : menu_screen == MENU_JOURNAL || menu_screen == MENU_BONUS ? M_JOURNAL
                     : menu_screen == MENU_QUIT && quit_to_menu ? M_MENU : M_QUIT;
            menu_screen = MENU_MAIN;
        }
        return;
    }
    if (menu_screen == MENU_JOURNAL) {
        journal_input(up, down, left, right, ok);
        return;
    }
    if (menu_screen == MENU_ARENA_PREP) {             /* the macro bar, the turns, the buttons */
        const ArenaBoss *ab = prep_src();
        int n = ab ? ab->n : 0;
        if (prep_pick >= 0) {                        /* the pick list */
            if (up) prep_pick_move(-1);
            if (down) prep_pick_move(1);
            if (left) prep_pick_move(-PICK_ROWS);
            if (right) prep_pick_move(PICK_ROWS);
            if (ok) prep_pick_set();
            return;
        }
        if (prep_copy >= 0) {                        /* Copy: Left / Right a letter, Enter puts it there */
            if (left || right) prep_slot = (prep_slot + (right ? 1 : 7)) % 8;
            if (ok) prep_ok();
            return;
        }
        if (up || down) prep_move(up, down);
        if ((left || right) && prep_grab < 0) {
            if (prep_row == -1) prep_slot = (prep_slot + (right ? 1 : 7)) % 8;
            else if (prep_row < n) prep_cycle(prep_row, right ? 1 : -1);
            else prep_btn = (prep_btn + (right ? 1 : 3)) % 4;
        }
        if (ok) prep_ok();
        return;
    }
    if (menu_screen == MENU_ARENA_TPL) {              /* eight templates, each with Load and Save */
        if (up) menu_sel = (menu_sel + TEMPLATES - 1) % TEMPLATES;
        if (down) menu_sel = (menu_sel + 1) % TEMPLATES;
        if (left || right) tpl_btn ^= 1;
        if (ok) tpl_ok();
        return;
    }
    if (menu_screen == MENU_WAVE || menu_screen == MENU_WAVE_SETUP || menu_screen == MENU_WAVE_RESULT) {
        wave_menu_input(up, down, left, right, ok);
        return;
    }
    if (menu_screen == MENU_BONUS) {                  /* the modes, and the Jukebox */
        if (up) menu_sel = (menu_sel + 2) % 3;
        if (down) menu_sel = (menu_sel + 1) % 3;
        if (ok) bonus_open(menu_sel);
        return;
    }
    if (menu_screen == MENU_JUKEBOX) {                /* a song: play it, or stop it; the switch: what plays */
        if (up) jukebox_move(-1);
        if (down) jukebox_move(1);
        if (menu_sel < 0) { if (left || right) jukebox_source(right ? 1 : -1); if (ok) jukebox_source(1); return; }
        if (left) jukebox_move(-JUKEBOX_ROWS);
        if (right) jukebox_move(JUKEBOX_ROWS);
        if (ok) jukebox_pick(menu_sel);
        return;
    }
    if (menu_screen == MENU_ARENA) {                  /* the bosses: Left / Right a page at a time */
        if (up) arena_move(-1);
        if (down) arena_move(1);
        if (left) arena_move(-ARENA_ROWS);
        if (right) arena_move(ARENA_ROWS);
        if (ok) arena_pick();
        return;
    }
    if (menu_screen == MENU_LOAD) {                   /* three slots a page: past its ends, the next page */
        if (up) { if (menu_sel > 0) menu_sel--; else { load_turn(-1); menu_sel = load_on_auto() ? 0 : 2; } }
        if (down) { if (menu_sel < 2 && !load_on_auto()) menu_sel++; else { load_turn(1); menu_sel = 0; } }
        if (left) load_turn(-1);
        if (right) load_turn(1);
        if (ok) load_pick();
        return;
    }
    if (menu_screen == MENU_LAUNCHER) {
        if (up) menu_sel = menu_sel <= launch_first() ? L_QUIT : menu_sel - 1;
        if (down) menu_sel = menu_sel >= L_QUIT ? launch_first() : menu_sel + 1;
        if ((left || right) && menu_sel == L_MODE) item_change(MODE_ITEM, right ? 1 : -1);
        if (ok) {
            if (menu_sel == L_START) launcher_go = 1;
            else if (menu_sel == L_CONTINUE) launcher_go = 2;
            else if (menu_sel == L_LOAD) load_enter();
            else if (menu_sel == L_ARENA) bonus_enter();
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
        if (menu_sel == M_RESUME) menu_close();
        else if (menu_sel == M_REWIND) { menu_close(); rewind_now(); }
        else if (menu_sel == M_JOURNAL) { if (arena_mode) bonus_enter(); else journal_enter(); }
        else if (menu_sel == M_SETTINGS) settings_enter();
        else { quit_to_menu = menu_sel == M_MENU; menu_screen = MENU_QUIT; menu_sel = 1; }   /* default: No */
    } else {
        if (menu_sel == 0 && !quit_to_menu) quit(0);
        if (menu_sel == 0) { menu_close(); to_launcher = 1; return; }   /* (platform_frame: back_to_launcher) */
        menu_screen = MENU_MAIN;
        menu_sel = quit_to_menu ? M_MENU : M_QUIT;
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
enum { HIT_ROW, HIT_STEP_BACK, HIT_PAGE, HIT_BACK, HIT_MODE, HIT_JTAB, HIT_JHINT, HIT_JUP, HIT_JDOWN, HIT_SLIDER,
       HIT_MUSIC, HIT_DISCORD };
/* the launcher fills the window: units of 1/224 of its height (narrow windows:
 * 1/300 of the width), its content in a 224-tall column in the middle; the
 * emblem (drawn under it, launcher_backdrop) is centred this far down */
#define LAUNCH_EMBLEM_Y 47.0f
static float launch_scale(int ow, int oh) { return fminf((float)oh / 224.0f, (float)ow / 300.0f); }
static MenuHit menu_hits[40];
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
/* word-wrapped to `width` from x, at most `lines` lines; centre: each line
 * centred in the width. The lines it took; c 0: only counted, not drawn */
static int m_wrap(float x, float base, float width, float line_h, int lines, float cap, const char *s, uint32_t c,
                  int centre) {
    char line[256];
    int drawn = 0;
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
        if (c) m_text(centre ? x + width / 2 : x, base, cap, line, c, centre ? 2 : 0);
        s += fit;
        base += line_h;
        drawn++;
    }
    return drawn;
}
static void m_arrow(float cx, float cy, float size, int dir, uint32_t c) {   /* dir 0 down, 1 up, 2 right, 3 left */
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
    if (menu_screen == MENU_JOURNAL) {                   /* the pages; scroll, choose, a hint */
        int pad_in = menu_last_pad && pad;
        char what[64];
        const char *ok_name = pad_in ? pad_button_name(pad_of[IN_C]) : "Enter";
        if (touch_visible()) snprintf(what, sizeof what, "%s", j_tab == JT_STORY ? "Tap the arrows to scroll" : "Tap the box for a hint");
        else if (j_tab == JT_STORY) snprintf(what, sizeof what, "Up / Down scroll");
        else if (j_tab == JT_GUILD) snprintf(what, sizeof what, "Up / Down choose, %s hint", ok_name);
        else snprintf(what, sizeof what, "%s hint", ok_name);
        if (touch_visible()) snprintf(out, n, "Tap a page's name%s%s%sTap outside to go back", sep, what, sep);
        else if (pad_in)
            snprintf(out, n, "%s page%s%s%s%s back", pad_shoulder_names(), sep, what, sep, pad_button_name(pad_of[IN_B]));
        else snprintf(out, n, "Left / Right page%s%s%sEsc back", sep, what, sep);
        return;
    }
    if (settings_screen && !page_now()->n) {             /* a page to read: only the pages to turn */
        if (touch_visible()) snprintf(out, n, "Tap a page's name%sTap outside to go back", sep);
        else if (menu_last_pad && pad)
            snprintf(out, n, "Left / Right or %s page%s%s back", pad_shoulder_names(), sep, pad_button_name(pad_of[IN_B]));
        else snprintf(out, n, "Left / Right or Tab page%sEsc back", sep);
        return;
    }
    if (menu_screen == MENU_ARENA_PREP || menu_screen == MENU_ARENA_TPL) {
        int pad_in = menu_last_pad && pad;
        const char *okn = pad_in ? pad_button_name(pad_of[IN_C]) : "Enter", *bk = pad_in ? pad_button_name(pad_of[IN_B]) : "Esc";
        if (touch_visible()) snprintf(out, n, "Tap to choose and change%sTap outside to go back", sep);
        else if (menu_screen == MENU_ARENA_TPL)
            snprintf(out, n, "Up / Down a template%sLeft / Right Load or Save%s%s do it%s%s back", sep, sep, okn, sep, bk);
        else if (prep_pick >= 0) snprintf(out, n, "Up / Down choose%sLeft / Right a page%s%s set it%s%s back", sep, sep, okn, sep, bk);
        else if (prep_copy >= 0) snprintf(out, n, "Left / Right a letter%s%s copy it there%s%s cancel", sep, okn, sep, bk);
        else if (prep_grab >= 0) snprintf(out, n, "Up / Down move the turn%s%s put it down", sep, okn);
        else snprintf(out, n, "Left / Right change%s%s list all%s%s macro%s%s back", sep, okn, sep,
                      pad_in ? pad_shoulder_names() : "Tab", sep, bk);
        return;
    }
    if (menu_screen == MENU_ARENA) {
        if (touch_visible()) snprintf(out, n, "Tap a boss to fight it%sTap the arrows to scroll%sTap outside to go back", sep, sep);
        else if (menu_last_pad && pad)
            snprintf(out, n, "%s fight%sUp / Down choose%s%s Wave Mode%s%s back", pad_button_name(pad_of[IN_C]), sep, sep,
                     pad_shoulder_names(), sep, pad_button_name(pad_of[IN_B]));
        else snprintf(out, n, "Enter fight%sUp / Down choose%sLeft / Right page%sTab Wave Mode%sEsc back", sep, sep, sep, sep);
        return;
    }
    if (menu_screen == MENU_JUKEBOX) {
        if (touch_visible()) snprintf(out, n, "Tap a song to play or stop it%sTap outside to go back", sep);
        else if (menu_last_pad && pad)
            snprintf(out, n, "%s play / stop%s%s launcher song%sUp / Down choose%s%s back", pad_button_name(pad_of[IN_C]),
                     sep, pad_button_name(pad_of[IN_AUTO]), sep, sep, pad_button_name(pad_of[IN_B]));
        else if (menu_sel < 0) snprintf(out, n, "Left / Right what plays the songs%sDown the songs%sEsc back", sep, sep);
        else snprintf(out, n, "Enter play / stop%sL launcher song%sUp / Down choose%sTab what plays it%sEsc back",
                      sep, sep, sep, sep);
        return;
    }
    if (menu_screen == MENU_BONUS) {
        if (touch_visible()) snprintf(out, n, "Tap a mode%sTap outside to go back", sep);
        else if (menu_last_pad && pad)
            snprintf(out, n, "%s open%sUp / Down choose%s%s back", pad_button_name(pad_of[IN_C]), sep, sep, pad_button_name(pad_of[IN_B]));
        else snprintf(out, n, "Enter open%sUp / Down choose%sEsc back", sep, sep);
        return;
    }
    if (menu_screen == MENU_WAVE || menu_screen == MENU_WAVE_SETUP || menu_screen == MENU_WAVE_RESULT) {
        int pad_in = menu_last_pad && pad;
        const char *okn = pad_in ? pad_button_name(pad_of[IN_C]) : "Enter", *bk = pad_in ? pad_button_name(pad_of[IN_B]) : "Esc";
        if (touch_visible()) snprintf(out, n, "Tap to choose%sTap outside to go back", sep);
        else if (menu_screen == MENU_WAVE)
            snprintf(out, n, "%s choose a run%sUp / Down%s%s Boss Arena%s%s back", okn, sep, sep,
                     pad_in ? pad_shoulder_names() : "Tab", sep, bk);
        else if (menu_screen == MENU_WAVE_SETUP)
            snprintf(out, n, "Arrows move%s%s pick or drop%s%s back", sep, okn, sep, bk);
        else snprintf(out, n, "Left / Right choose%s%s do it%s%s back", sep, okn, sep, bk);
        return;
    }
    if (menu_screen == MENU_LOAD) {
        const char *more = load_tabs() > 1 ? "more saves" : NULL;
        if (touch_visible()) snprintf(out, n, "Tap a save to play on from it%sTap outside to go back", sep);
        else if (menu_last_pad && pad)
            snprintf(out, n, "%s load%s%s%s%s%s back", pad_button_name(pad_of[IN_C]), sep, more ? "Left / Right " : "",
                     more ? more : "", more ? sep : "", pad_button_name(pad_of[IN_B]));
        else snprintf(out, n, "Enter load%s%s%s%sEsc back", sep, more ? "Left / Right " : "", more ? more : "", more ? sep : "");
        return;
    }
    if (menu_screen == MENU_LAUNCHER) {
        if (touch_visible()) snprintf(out, n, "Tap to choose");
        else snprintf(out, n, "%s select%sLeft / Right switch the mode%s%s music%s",
                      menu_last_pad && pad ? pad_button_name(pad_of[IN_C]) : "Enter", sep, sep,
                      menu_last_pad && pad ? pad_button_name(pad_of[IN_AUTO]) : "M",
                      menu_last_pad && pad ? "" : m_ttf ? "   \xC2\xB7   D Discord" : "   D Discord");
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
static unsigned rows_off;                        /* rows drawn dim (bit i): the pause menu's Go back, in the Boss Arena */
static void draw_rows(float W, const char *const *labels, int n, float y0) {
    for (int i = 0; i < n; i++) {
        float a = y0 + 16.0f * (float)i, b = a + 14.0f;
        if (i == menu_sel) m_box(8.0f, a, W - 8.0f, b, 7.0f, C_GOLD, C_GOLD_LOW, 255);
        m_text(15.0f, a + 9.8f, 5.0f, labels[i], i == menu_sel ? C_INK : (rows_off >> i & 1) ? C_DIM : C_TEXT, 0);
        m_hit(8.0f, a, W - 8.0f, b, HIT_ROW, i);
    }
}

/* Back, top right, for the mouse and for touch */
static void draw_back_button(float W, float pad) {
    float bx1 = W - pad, bx0 = bx1 - m_width("Back", 4.2f) - 17.0f, by0 = 7.0f, by1 = 20.0f;
    m_box(bx0, by0, bx1, by1, 6.5f, 0x26346A, 0x18214A, 240);
    m_frame(bx0, by0, bx1, by1, 6.5f, C_RULE, 120);
    m_arrow(bx0 + 7.5f, (by0 + by1) / 2, 4.0f, 3, C_SOFT);
    m_text(bx0 + 12.0f, by0 + 9.1f, 4.2f, "Back", C_VALUE, 0);
    m_hit(bx0, by0, bx1, by1, HIT_BACK, 0);
}

/* A number setting (the volumes) is a slider: a bar filled to its value with a
 * knob there, then the value. Left / Right step it (held, they go on), a click
 * or a tap on the bar puts it there, and the mouse can drag it. */
#define SLIDER_W    110.0f
#define SLIDER_GRIP 4.0f                         /* its hit area reaches this far past the ends */
static int slider_drag = -1;                     /* the row whose knob the mouse is dragging */

static void draw_slider(const MenuItem *it, float x0, float x1, float mid, int sel, int off) {
    float f = (float)(setting_int(it->key) - it->lo) / (float)(it->hi - it->lo);
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    float x = x0 + (x1 - x0) * f, h = 1.3f, k = 2.9f;
    uint32_t track = off ? 0x3E4766 : sel ? 0xC9933A : 0x2C3A78;
    uint32_t fill_top = off ? C_DIM : sel ? C_INK_SOFT : 0xC4D3FF, fill_low = off ? C_DIM : sel ? C_INK : C_SOFT;
    uint32_t knob = off ? C_DIM : sel ? C_INK : C_TEXT;
    m_box(x0, mid - h, x1, mid + h, h, track, track, 255);
    if (x > x0 + 0.5f) m_box(x0, mid - h, x, mid + h, h, fill_top, fill_low, 255);
    if (it->lo < 100 && it->hi > 100) {          /* the overall volume: a mark at 100%, the game's own */
        float t = x0 + (x1 - x0) * (float)(100 - it->lo) / (float)(it->hi - it->lo);
        m_box(t - 0.35f, mid - 2.5f, t + 0.35f, mid + 2.5f, 0.0f, knob, knob, 150);
    }
    m_box(x - k, mid - k, x + k, mid + k, k, knob, knob, 255);
}

/* the slider under hit h, set from where the pointer is along it (render pixels) */
static void slider_to(const MenuHit *h, float rx) {
    const MenuItem *it = &page_now()->items[h->index];
    float grip = SLIDER_GRIP * m_S, x0 = h->x0 + grip, x1 = h->x1 - grip;
    float f = x1 > x0 ? (rx - x0) / (x1 - x0) : 0.0f;
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    item_set(it, it->lo + (int)lrintf(f * (float)(it->hi - it->lo) / (float)it->step) * it->step);
}

/* the mouse moved with its button held: the knob being dragged follows */
static void slider_drag_to(float rx) {
    if (menu_screen != MENU_SETTINGS) { slider_drag = -1; return; }
    for (int i = 0; i < n_menu_hits; i++)
        if (menu_hits[i].kind == HIT_SLIDER && menu_hits[i].index == slider_drag) {
            slider_to(&menu_hits[i], rx);
            return;
        }
}

/* a gamepad's Left / Right held on a slider goes on stepping, as a held key
 * does (the gamepad sends one press) */
static void menu_pad_repeat(void) {
    static uint64_t since, last;
    static int held;
    int dir = 0;
    if (pad && menu_screen == MENU_SETTINGS && menu_sel >= 0 && page_now()->n && page_now()->items[menu_sel].step) {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dir = -1;
        else if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dir = 1;
    }
    uint64_t now = SDL_GetTicks();
    if (dir != held) { held = dir; since = last = now; return; }
    if (!dir || now - since < 400 || now - last < 60) return;
    last = now;
    item_change(&page_now()->items[menu_sel], dir);
}

/* ------------------------------------------------------------------ the settings' previews */
/* Settings > Display and Battles show what a setting does: a short clip with it off beside one
 * with it as chosen (clip.h). Made once from the player's ROM -- the port ships nothing of the
 * game -- by the game itself, windowless (psiv --headless --preview-scene), one clip at a time
 * in the background at a low priority, into the player's folder (previews/). Then kept. */
typedef struct { const char *name, *scene, *args; } PreviewSpec;
static const PreviewSpec PREVIEWS[] = {
    {"battle", "battle", "--depth 1:1 --fog 1 --fx 1 --weather 0"},   /* (the defaults; no rain but Weather's own) */
    {"depth0", "battle", "--depth 0:1 --fog 1 --fx 1 --weather 0"},
    {"depth2", "battle", "--depth 2:1 --fog 1 --fx 1 --weather 0"},
    {"fog0", "battle", "--depth 1:1 --fog 0 --fx 1 --weather 0"},
    {"fog2", "battle", "--depth 1:1 --fog 2 --fx 1 --weather 0"},
    {"fx0", "battle", "--depth 1:1 --fog 1 --fx 0 --weather 0"},
    {"fx2", "battle", "--depth 1:1 --fog 1 --fx 2 --weather 0"},
    {"cam0", "battle", "--depth 1:1 --fog 1 --fx 1 --weather 0 --no-pan"},
    {"rain1", "battle", "--depth 1:1 --fog 1 --fx 1 --weather 1"},
    {"rain2", "battle", "--depth 1:1 --fog 1 --fx 1 --weather 2"},
    {"field", "field", "--water 1"},
    {"nowater", "field", "--water 1 --no-water"},
    {"notrees", "field", "--water 1 --no-trees"},
};
#define N_PREVIEWS (int)(sizeof PREVIEWS / sizeof PREVIEWS[0])
#define PREVIEWS_VERSION "1"                         /* in the files' names: a new one makes them again */
/* each setting with a preview: the clip for each of its values, which value is off, and the one
 * shown beside Off while Off is chosen */
typedef struct { const char *key; const char *clip[3]; int off, on, smooth; } PreviewOf;
static const PreviewOf PREVIEW_OF[] = {
    {"video.filter", {"field", "field", "field"}, 0, 1, 1},
    {"enhance.water", {"field", "nowater"}, 1, 0, 0},
    {"enhance.trees", {"field", "notrees"}, 1, 0, 0},
    {"enhance.battle_depth", {"depth0", "battle", "depth2"}, 0, 1, 0},
    {"enhance.battle_fog", {"fog0", "battle", "fog2"}, 0, 1, 0},
    {"enhance.battle_camera", {"battle", "cam0"}, 1, 0, 0},
    {"enhance.battle_weather", {"battle", "rain1", "rain2"}, 0, 1, 0},
    {"enhance.battle_fx", {"fx0", "battle", "fx2"}, 0, 1, 0},
};
static SDL_AtomicInt previews_made;                  /* bumped as the maker finishes each */
static SDL_AtomicInt previews_busy;                  /* the maker at work */
static int previews_failed;                          /* (it gave up: the clip it couldn't make) */
static Clip prev_clip[N_PREVIEWS];
static int prev_state[N_PREVIEWS];                   /* 0 not looked for, 1 loaded, -1 not made yet */
static uint64_t prev_used[N_PREVIEWS];
static int prev_seen_made = -1;
static SDL_Texture *prev_tex[2];
static int prev_tex_w[2], prev_tex_h[2];

static void preview_path(int i, char *out, size_t n, int tmp) {
    char dir[1100];
    path_in_home(dir, sizeof dir, "previews");
    snprintf(out, n, "%s/v%s-%s.clip%s", dir, PREVIEWS_VERSION, PREVIEWS[i].name, tmp ? ".part" : "");
}
static int preview_index(const char *name) {
    for (int i = 0; i < N_PREVIEWS; i++)
        if (!strcmp(PREVIEWS[i].name, name)) return i;
    return -1;
}
static int previews_count(void) {                    /* made so far */
    int n = 0;
    char p[1200];
    for (int i = 0; i < N_PREVIEWS; i++) {
        preview_path(i, p, sizeof p, 0);
        n += file_exists(p);
    }
    return n;
}

/* the program itself, to run windowless */
static void self_exe(char *out, size_t n) {
#if defined(_WIN32)
    snprintf(out, n, "%spsiv.exe", base_dir);
#elif defined(SDL_PLATFORM_MACOS)
    if (strstr(base_dir, ".app/Contents/Resources")) snprintf(out, n, "%s../MacOS/psiv", base_dir);
    else snprintf(out, n, "%spsiv", base_dir);
#else
    snprintf(out, n, "%spsiv", base_dir);
#endif
}

static char maker_rom[1100], maker_font[1100], maker_exe[1100];
static int SDLCALL previews_maker(void *ud) {
    (void)ud;
    char dir[1100];
    path_in_home(dir, sizeof dir, "previews");
    SDL_CreateDirectory(dir);
    uint64_t t0 = SDL_GetTicks();
    int made = 0;
    for (int i = 0; i < N_PREVIEWS; i++) {
        char path[1200], part[1200], font[1200], argbuf[256];
        preview_path(i, path, sizeof path, 0);
        if (file_exists(path)) continue;
        preview_path(i, part, sizeof part, 1);
        const char *args[40];
        int n = 0;
        args[n++] = maker_exe;
        args[n++] = "--headless";
        args[n++] = "--nice";
        args[n++] = "--rom";
        args[n++] = maker_rom;
        args[n++] = "--preview-scene";
        args[n++] = PREVIEWS[i].scene;
        snprintf(argbuf, sizeof argbuf, "%s", PREVIEWS[i].args);
        for (char *t = argbuf; *t && n < 30;) {      /* its words */
            args[n++] = t;
            while (*t && *t != ' ') t++;
            if (*t) *t++ = 0;
        }
        if (maker_font[0] && !strcmp(PREVIEWS[i].scene, "battle")) {   /* the modern battle menus, as the game shows them */
            snprintf(font, sizeof font, "%s:1", maker_font);
            args[n++] = "--text";
            args[n++] = font;
            args[n++] = "--battle-ui";
        }
        args[n++] = "--clip";
        args[n++] = part;
        args[n] = NULL;
        SDL_PropertiesID pr = SDL_CreateProperties();
        SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_Process *p = SDL_CreateProcessWithProperties(pr);
        SDL_DestroyProperties(pr);
        int code = -1;
        if (p) {
            SDL_WaitProcess(p, true, &code);
            SDL_DestroyProcess(p);
        }
        if (!p || code != 0 || !SDL_RenamePath(part, path)) {
            SDL_Log("previews: %s not made (%s)", PREVIEWS[i].name, p ? "it stopped" : SDL_GetError());
            SDL_RemovePath(part);
            previews_failed = 1;
            break;
        }
        made++;
        SDL_AddAtomicInt(&previews_made, 1);
    }
    if (made) SDL_Log("previews: %d made from the ROM in %.1f s", made, (double)(SDL_GetTicks() - t0) / 1000.0);
    SDL_SetAtomicInt(&previews_busy, 0);
    return 0;
}

/* the clips not made yet: made now, in the background (not on phones: one program at a time) */
static void previews_start(void) {
    if (MOBILE || !rom_ready || previews_failed || SDL_GetAtomicInt(&previews_busy)) return;
    if (previews_count() == N_PREVIEWS) return;
    if (rom_arg) snprintf(maker_rom, sizeof maker_rom, "%s", rom_arg);
    else path_in_home(maker_rom, sizeof maker_rom, "rom/psiv.bin");
    if (!file_exists(maker_rom)) return;
    snprintf(maker_font, sizeof maker_font, "%s", font_path);
    self_exe(maker_exe, sizeof maker_exe);
    if (!file_exists(maker_exe)) { SDL_Log("previews: %s not found", maker_exe); previews_failed = 1; return; }
    SDL_SetAtomicInt(&previews_busy, 1);
    SDL_Thread *t = SDL_CreateThread(previews_maker, "psiv-previews", NULL);
    if (t) SDL_DetachThread(t);
    else SDL_SetAtomicInt(&previews_busy, 0);
}

/* a clip, read when first shown; three kept at most */
static const Clip *preview_clip(int i) {
    int made = SDL_GetAtomicInt(&previews_made);
    if (made != prev_seen_made) {                    /* (new ones made: look again for those missing) */
        prev_seen_made = made;
        for (int k = 0; k < N_PREVIEWS; k++)
            if (prev_state[k] < 0) prev_state[k] = 0;
    }
    if (i < 0) return NULL;
    if (!prev_state[i]) {
        char p[1200];
        preview_path(i, p, sizeof p, 0);
        int loaded = 0;
        for (int k = 0; k < N_PREVIEWS; k++) loaded += prev_state[k] > 0;
        while (loaded >= 3) {                        /* the one shown longest ago goes */
            int old = -1;
            for (int k = 0; k < N_PREVIEWS; k++)
                if (prev_state[k] > 0 && (old < 0 || prev_used[k] < prev_used[old])) old = k;
            clip_free(&prev_clip[old]);
            prev_state[old] = 0;
            loaded--;
        }
        uint64_t t = SDL_GetTicksNS();
        prev_state[i] = clip_load(p, &prev_clip[i]) ? 1 : -1;
        if (prev_state[i] > 0)
            SDL_Log("previews: %s read in %.0f ms", PREVIEWS[i].name, (double)(SDL_GetTicksNS() - t) / 1e6);
    }
    prev_used[i] = SDL_GetTicks();
    return prev_state[i] > 0 ? &prev_clip[i] : NULL;
}

/* what the panel shows for the highlighted setting (draw_settings), drawn over it each frame
 * (previews_render): two clips, units */
static struct {
    int on;
    int clip[2];                                     /* PREVIEWS indices */
    float x[2], y, w, h;                             /* the boxes */
    int smooth;                                      /* Smoothing: both close up, the second blended (1) or
                                                      * through Scale3x (2) */
} prev_show;

static const PreviewOf *preview_of(const MenuItem *it) {
    for (size_t i = 0; it && it->key && i < sizeof PREVIEW_OF / sizeof PREVIEW_OF[0]; i++)
        if (!strcmp(PREVIEW_OF[i].key, it->key)) return &PREVIEW_OF[i];
    return NULL;
}
static int page_has_previews(const MenuPage *pg) {
    for (int i = 0; pg->items && i < pg->n; i++)
        if (preview_of(&pg->items[i])) return 1;
    return 0;
}
static uint64_t previews_sig(void) {                 /* for menu_signature: the clips made so far */
    return (uint64_t)SDL_GetAtomicInt(&previews_made) * 2 + (uint64_t)SDL_GetAtomicInt(&previews_busy);
}

/* the preview area, x0..x1 from y0 (units): the highlighted setting off, and as chosen */
static void draw_previews(const MenuPage *pg, float x0, float x1, float y0, float y1) {
    prev_show.on = 0;
    const MenuItem *it = menu_sel >= 0 && menu_sel < pg->n ? &pg->items[menu_sel] : NULL;
    const PreviewOf *po = preview_of(it);
    m_text((x0 + x1) / 2, y0 + 5.0f, 3.6f, "Preview", C_SOFT, 2);
    float gap = 4.0f, w = (x1 - x0 - gap) / 2, top = y0 + 9.0f;
    if (!po) {                                       /* a setting with none: which have one */
        m_box(x0, top, x1, top + 44.0f, 4.0f, 0x141C42, 0x101634, 255);
        m_wrap(x0 + 6.0f, top + 17.0f, x1 - x0 - 12.0f, 7.0f, 4, 3.8f,
               pg->items == BATTLE_ITEMS ? "Highlight Depth, Fog, Camera, Weather or Effects to see it here."
                                         : "Highlight Smoothing, Water or Trees to see it here.", C_DIM, 1);
        return;
    }
    int nv = item_values(it), cur = 0;
    for (int k = 0; k < nv; k++)
        if (!SDL_strcasecmp(setting(it->key), it->values[k])) cur = k;
    int right = cur == po->off ? po->on : cur;       /* beside Off: what's chosen (or, Off chosen, the usual) */
    int a = preview_index(po->clip[po->off]), b = preview_index(po->clip[right]);
    const Clip *ca = preview_clip(a), *cb = preview_clip(b);
    float h = w * (ca ? (float)ca->h / (float)ca->w : cb ? (float)cb->h / (float)cb->w : 164.0f / 320.0f);
    if (po->smooth) h = w * 0.75f;                   /* (Smoothing: close up, 4:3) */
    if (top + h + 10.0f > y1) h = y1 - top - 10.0f;
    for (int k = 0; k < 2; k++) {
        float bx = x0 + (float)k * (w + gap);
        m_box(bx - 0.6f, top - 0.6f, bx + w + 0.6f, top + h + 0.6f, 1.2f, 0x2A3870, 0x2A3870, 255);
        m_box(bx, top, bx + w, top + h, 0.8f, 0x080B1C, 0x080B1C, 255);
        int v = k ? right : po->off;
        int mine = v == cur;
        m_text(bx + w / 2, top + h + 6.5f, 3.9f, it->names[v], mine ? C_GOLD : C_SOFT, 2);
    }
    if (ca && cb) {
        prev_show.on = 1;
        prev_show.clip[0] = a;
        prev_show.clip[1] = b;
        prev_show.x[0] = x0;
        prev_show.x[1] = x0 + w + gap;
        prev_show.y = top;
        prev_show.w = w;
        prev_show.h = h;
        prev_show.smooth = !po->smooth ? 0 : !SDL_strcasecmp(it->values[right], "scale2x") ? 2 : 1;
    } else {                                         /* not made yet */
        int made = previews_count();
        char t[120];
        if (MOBILE) snprintf(t, sizeof t, "Previews aren't made on phones yet.");
        else if (previews_failed) snprintf(t, sizeof t, "The previews couldn't be made (psiv.log says why).");
        else if (!rom_ready) snprintf(t, sizeof t, "Made from your ROM once it's installed.");
        else snprintf(t, sizeof t, "Making the previews from your ROM: %d of %d", made, N_PREVIEWS);
        m_wrap(x0 + 4.0f, top + h / 2, x1 - x0 - 8.0f, 6.5f, 2, 3.7f, t, C_DIM, 1);
    }
}

/* Smoothing's preview with Scale2x chosen: the clip's frame through Scale3x, as a texture three
 * times its size (NULL: no room for it) */
static SDL_Texture *preview_scalex(const uint16_t *frame, int w, int h) {
    static SDL_Texture *tex;
    static int tw, th;
    static uint32_t *in, *out;
    if (!tex || tw != w || th != h) {
        if (tex) SDL_DestroyTexture(tex);
        free(in);
        free(out);
        in = malloc((size_t)w * (size_t)h * sizeof *in);
        out = malloc((size_t)w * (size_t)h * 9u * sizeof *out);
        tex = in && out ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w * 3, h * 3) : NULL;
        tw = w;
        th = h;
        if (!tex) return NULL;
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    for (int i = 0; i < w * h; i++) {                    /* (the clips are kept in 16 bits) */
        uint32_t p = frame[i], r = p >> 11 & 31, g = p >> 5 & 63, b = p & 31;
        in[i] = 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    scalex_upscale(in, w, w, h, out, w * 3, 3);
    SDL_UpdateTexture(tex, NULL, out, w * 3 * 4);
    return tex;
}

/* over the menu, each frame: the clips at their frame now */
static void previews_render(void) {
    if (!prev_show.on || menu_screen != MENU_SETTINGS) return;
    const Clip *c[2] = {preview_clip(prev_show.clip[0]), preview_clip(prev_show.clip[1])};
    if (!c[0] || !c[1]) return;
    int fps = c[0]->fps > 0 ? c[0]->fps : 15, n = c[0]->n < c[1]->n ? c[0]->n : c[1]->n;
    int k = (int)((SDL_GetTicks() * (uint64_t)fps / 1000) % (uint64_t)n);
    for (int j = 0; j < 2; j++) {
        if (!prev_tex[j] || prev_tex_w[j] != c[j]->w || prev_tex_h[j] != c[j]->h) {
            if (prev_tex[j]) SDL_DestroyTexture(prev_tex[j]);
            prev_tex[j] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, c[j]->w, c[j]->h);
            prev_tex_w[j] = c[j]->w;
            prev_tex_h[j] = c[j]->h;
            if (!prev_tex[j]) return;
        }
        const uint16_t *frame = c[j]->px + (size_t)k * (size_t)c[j]->w * (size_t)c[j]->h;
        SDL_Texture *tex = prev_tex[j];
        float up = 1.0f;                                 /* the texture's size, in the clip's pixels */
        if (j && prev_show.smooth == 2 && (tex = preview_scalex(frame, c[j]->w, c[j]->h)) != NULL) {
            up = 3.0f;
        } else {
            tex = prev_tex[j];
            SDL_UpdateTexture(tex, NULL, frame, c[j]->w * 2);
            SDL_SetTextureScaleMode(tex, prev_show.smooth && j ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        }
        SDL_FRect dst = {m_ox + prev_show.x[j] * m_S, m_oy + prev_show.y * m_S, prev_show.w * m_S, prev_show.h * m_S};
        SDL_FRect src = {0, 0, (float)c[j]->w, (float)c[j]->h};
        if (prev_show.smooth) {                      /* close up on the party and the shore */
            src.w = (float)c[j]->w * 0.3f;
            src.h = src.w * 0.75f;
            src.x = (float)c[j]->w * 0.5f - src.w * 0.55f;
            src.y = (float)c[j]->h * 0.46f - src.h * 0.5f;
        } else if (dst.w / dst.h > src.w / src.h + 0.01f) {   /* (squeezed: the middle, whole height) */
            float sw = src.h * dst.w / dst.h;
            src.x = (src.w - sw) / 2;
            src.w = sw;
        } else if (dst.w / dst.h < src.w / src.h - 0.01f) {
            float sh = src.w * dst.h / dst.w;
            src.y = (src.h - sh) / 2;
            src.h = sh;
        }
        src.x *= up; src.y *= up; src.w *= up; src.h *= up;
        SDL_RenderTexture(renderer, tex, &src, &dst);
    }
}

static void draw_settings(float W, float H, const char *hint) {
    const float pad = 10.0f;
    const MenuPage *pg = page_now();
    m_text(pad + 2.0f, 17.0f, 6.4f, "Settings", C_GOLD, 0);
    draw_back_button(W, pad);
    /* the pages: their names smaller if they wouldn't fit across (SDL's 8x8 font) */
    float x = pad, t0 = 26.0f, t1 = 40.0f, cap = 4.6f, room = 14.0f;
    for (;;) {
        float all = 3.0f * (N_PAGES - 1);
        for (int i = 0; i < N_PAGES; i++) all += m_width(PAGES[i].name, cap) + room;
        if (all <= W - 2 * pad || cap <= 3.0f) {
            if (all < W - 2 * pad) x += (W - 2 * pad - all) / 2;   /* (a wide panel: centred) */
            break;
        }
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
    /* its settings: the label, the value; the highlighted one with arrows.
     * Eight rows fit, or nine a little closer together (the Sound page),
     * so every page shows whole; a longer one would scroll to keep the
     * highlighted row in view, with a mark where there are more above or
     * below */
    const float rule = H - 40.5f;
    int rows = settings_rows(), first = 0;
    float step = pg->n == TIGHT_ROWS ? 15.0f * MAX_ROWS / TIGHT_ROWS : 15.0f;
    /* a page with previews: its list on the left, the preview on the right */
    int previews = page_has_previews(pg);
    float xr = previews ? fminf(150.0f, W * 0.465f) : W - pad, xm = (pad + xr) / 2;
    if (previews) draw_previews(pg, xr + 6.0f, W - pad, 47.0f, rule - 2.0f);
    else prev_show.on = 0;
    if (pg->n > rows) {
        settings_keep_top();
        first = settings_top;
        if (first > 0) {                                                          /* more above (pointing up) */
            m_arrow(xm, 46.8f, 3.0f, 1, C_SOFT);
            m_hit(xm - 12.0f, 43.0f, xm + 12.0f, 48.4f, HIT_JUP, 0);
        }
        if (first + rows < pg->n) {                                               /* more below */
            m_arrow(xm, rule - 1.9f, 3.0f, 0, C_SOFT);
            m_hit(xm - 12.0f, rule - 5.5f, xm + 12.0f, rule + 1.5f, HIT_JDOWN, 0);
        }
    }
    for (int i = first; i < pg->n && i < first + rows; i++) {
        const MenuItem *it = &pg->items[i];
        float a = 48.5f + step * (float)(i - first), b = a + step - 1.5f, mid = (a + b) / 2, base = mid + 2.75f;
        int sel = i == menu_sel, off = item_disabled(it);
        char v[64];
        item_value(it, v, sizeof v);
        if (sel) m_box(pad, a, xr, b, (b - a) / 2, off ? 0x56607E : C_GOLD, off ? 0x3E4766 : C_GOLD_LOW, 255);
        uint32_t lc = off ? (sel ? 0xC8D0E8 : C_DIM) : sel ? C_INK : C_TEXT;
        uint32_t vc = off ? (sel ? 0xC8D0E8 : C_DIM) : sel ? C_INK : C_VALUE;
        m_text(pad + 8.0f, base, 4.6f, it->label, lc, 0);
        float vr = xr - 13.0f, vw = m_width(v, 4.6f);
        m_text(vr, base, 4.6f, v, vc, 1);
        if (it->step) {                              /* a number: its slider, the arrows round both */
            float sx1 = vr - m_width("200%", 4.6f) - 12.0f, sx0 = sx1 - SLIDER_W;
            draw_slider(it, sx0, sx1, mid, sel, off);
            if (sel && !off) {
                m_arrow(sx0 - 8.0f, mid, 4.0f, 3, C_INK_SOFT);
                m_arrow(vr + 6.5f, mid, 4.0f, 2, C_INK_SOFT);
            }
            m_hit(sx0 - SLIDER_GRIP, a, sx1 + SLIDER_GRIP, b, HIT_SLIDER, i);
        } else {
            if (sel && !off) {
                if (!it->action) m_arrow(vr - vw - 6.5f, mid, 4.0f, 3, C_INK_SOFT);
                m_arrow(vr + 6.5f, mid, 4.0f, 2, C_INK_SOFT);
            }
            if (!it->action)                         /* the value's left: a step back */
                m_hit(vr - vw - 14.0f, a, vr - vw * 0.5f, b, HIT_STEP_BACK, i);
        }
        m_hit(pad, a, xr, b, HIT_ROW, i);
    }
    /* what the highlighted one does */
    m_box(pad, rule, W - pad, rule + 0.5f, 0, C_RULE, C_RULE, 80);
    const char *about = menu_sel < 0 ? pg->about
                      : arena_cheat_item(&pg->items[menu_sel]) ? "Off in the Boss Arena: every fight is fought fair."
                      : item_disabled(&pg->items[menu_sel]) ? CLASSIC_NOTE : pg->items[menu_sel].help;
    if (quip_until > SDL_GetTicks() && pg->items == SOUND_ITEMS) about = quip_line;   /* a quip, while it sounds */
    m_wrap(pad + 2.0f, rule + 10.5f, W - 2 * pad - 4.0f, 8.6f, 2, 4.0f, about, C_VALUE, 0);
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
    const float rule = H - 40.5f;
    typedef struct { const char *what, *md, *gp; char other[48]; } Row;
    Row r[14];
    int n = 0;
    memset(r, 0, sizeof r);
    r[n] = (Row){"Move", NULL, "D-pad, left stick", ""};
    if (MOBILE) snprintf(r[n].other, sizeof r[n].other, "On-screen pad");
    else if (key_of[IN_UP] == SDL_SCANCODE_UP && key_of[IN_DOWN] == SDL_SCANCODE_DOWN &&
             key_of[IN_LEFT] == SDL_SCANCODE_LEFT && key_of[IN_RIGHT] == SDL_SCANCODE_RIGHT)
        snprintf(r[n].other, sizeof r[n].other, wasd_of[0] && wasd_of[1] && wasd_of[2] && wasd_of[3] ? "Arrow keys, WASD" : "Arrow keys");
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
    r[n] = (Row){"Auto-advance voiced scenes", NULL, pad_label(pad_of[IN_AUTO]), ""};
    if (MOBILE) snprintf(r[n].other, sizeof r[n].other, "%s", dash);
    else key_label(IN_AUTO, r[n].other, sizeof r[n].other);
    n++;
    r[n] = (Row){"Random battles off / on", NULL, pad_label(pad_of[IN_ENCOUNTERS]), ""};
    if (MOBILE) snprintf(r[n].other, sizeof r[n].other, "Settings, Cheats");
    else key_label(IN_ENCOUNTERS, r[n].other, sizeof r[n].other);
    n++;
    r[n] = (Row){"World map", NULL, pad_label(pad_of[IN_MAP]), ""};
    if (MOBILE) snprintf(r[n].other, sizeof r[n].other, "Tap the small map");
    else key_label(IN_MAP, r[n].other, sizeof r[n].other);
    n++;
    if (!MOBILE) {
        r[n] = (Row){"Pause, fullscreen, screenshot", NULL, dash, ""};
        snprintf(r[n++].other, sizeof r[0].other, "F10, F11, F12");
    }

    m_text(x_pad, 54.0f, 3.6f, "Gamepad", C_SOFT, 0);
    m_text(x_other, 54.0f, 3.6f, MOBILE ? "Touch" : "Keyboard", C_SOFT, 0);
    float step = fminf(10.5f, (rule - 2.0f - 57.5f) / (float)n);     /* as roomy as the panel allows */
    for (int i = 0; i < n; i++) {
        float a = 57.5f + step * (float)i, base = a + step / 2 + 2.5f;
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
    m_box(pad_x, rule, W - pad_x, rule + 0.5f, 0, C_RULE, C_RULE, 80);
    m_wrap(pad_x + 2.0f, rule + 8.5f, W - 2 * pad_x - 4.0f, 7.2f, 3, 3.7f,
           MOBILE ? "Tap the field to walk there; touch the screen to bring the on-screen pad back. "
                    "Change the buttons in psiv.ini, under [pad]. "
                    "A, B and C can be set up in the game's own System menu: Start on the field, then Buttons."
                  : "Mouse: click the field to walk there, or a person to talk; a right click is Cancel. "
                    "Change the keys and buttons in psiv.ini, under [keys] and [pad]. "
                    "A, B and C can be set up in the game's own System menu: Start on the field, then Buttons.",
           C_VALUE, 0);
}

/* ------------------------------------------------------------------ the Journal's look */
static const char *const J_TAB_NAMES[3] = {"Now", "Story so far", "Hunters' Guild"};

static void draw_journal_tabs(float W, float pad) {
    float x = pad, t0 = 26.0f, t1 = 40.0f, cap = 4.6f, room = 16.0f;
    for (int i = 0; i < j_tabs(); i++) {
        float w = m_width(J_TAB_NAMES[i], cap) + room;
        int on = i == j_tab;
        if (on) m_box(x, t0, x + w, t1, 7.0f, 0x3A4C8E, 0x2A3870, 255);
        m_text(x + w / 2, t0 + 9.8f, cap, J_TAB_NAMES[i], on ? C_TEXT : C_SOFT, 2);
        m_hit(x, t0, x + w, t1, HIT_JTAB, i);
        x += w + 3.0f;
    }
    m_box(pad, 44.5f, W - pad, 45.0f, 0, C_RULE, C_RULE, 80);
}

/* Now: the part of the game, the step, what to do; the hint in a box below,
 * hidden until asked for */
static void draw_journal_now(float W, float pad) {
    int cur = journal_story();
    const JournalStep *s = journal_step(cur);
    float x = pad + 4.0f, width = W - 2 * pad - 8.0f;
    if (!s) {
        m_wrap(x, 62.0f, width, 8.6f, 3, 4.4f,
               "The journal fills in as the story goes on: start a new game, or continue one.", C_VALUE, 0);
        return;
    }
    float y = 55.5f;
    if (s->arc[0]) {
        char arc[40];
        size_t i = 0;
        for (; s->arc[i] && i < sizeof arc - 1; i++) arc[i] = (char)SDL_toupper((unsigned char)s->arc[i]);
        arc[i] = 0;
        m_text(x, y, 3.5f, arc, C_SOFT, 0);
        y += 11.0f;
    }
    m_text(x, y, 6.2f, s->title, C_GOLD, 0);
    y += 11.5f;
    y += 8.4f * (float)m_wrap(x, y, width, 8.4f, 3, 4.6f, s->next, C_TEXT, 0);
    float b0 = 110.0f, b1 = 152.0f;
    int room = (int)((b0 - 4.0f - y) / 7.2f) + 1;          /* what just happened, under it, as it fits */
    if (s->done[0] && room > 0) m_wrap(x, y + 2.0f, width, 7.2f, room > 2 ? 2 : room, 3.9f, s->done, C_SOFT, 0);
    if (!s->hint[0]) return;
    int shown = j_hint_step == cur;
    m_box(pad, b0, W - pad, b1, 6.0f, shown ? 0x22305F : 0x26346A, shown ? 0x1A2550 : 0x1E2A58, 235);
    m_frame(pad, b0, W - pad, b1, 6.0f, C_RULE, shown ? 90 : 140);
    if (shown) {
        m_text(x + 2.0f, b0 + 8.5f, 3.4f, "HINT", C_SOFT, 0);
        m_wrap(x + 2.0f, b0 + 17.0f, width - 4.0f, 7.4f, 4, 4.0f, s->hint, C_VALUE, 0);
    } else {
        m_text(W / 2, (b0 + b1) / 2 + 0.5f, 4.6f, "Show a hint", C_VALUE, 2);
        m_text(W / 2, (b0 + b1) / 2 + 8.5f, 3.4f, "Where to go and what to do, if you're stuck", C_DIM, 2);
    }
    m_hit(pad, b0, W - pad, b1, HIT_JHINT, 0);
}

/* Story so far: what happened, the newest first, scrolled an entry at a time */
static void draw_journal_story(float W, float pad) {
    int v[256], m = j_entries(v, 256);
    float x = pad + 4.0f, width = W - 2 * pad - 8.0f, y = 48.5f, bottom = 153.0f;
    j_more_below = 0;
    if (!m) {
        m_wrap(x, 62.0f, width, 8.6f, 3, 4.4f, "Nothing yet: the story is just beginning.", C_VALUE, 0);
        return;
    }
    if (j_scroll > m - 1) j_scroll = m - 1;
    int e = j_scroll;
    for (; e < m; e++) {
        const JournalStep *s = journal_step(v[e]);
        int lines = m_wrap(x, 0, width, 7.2f, 4, 3.9f, s->done, 0, 0);       /* counted, not drawn */
        float h = 9.0f + 7.2f * (float)lines + 3.4f;
        if (y + h > bottom && e > j_scroll) break;
        m_text(x, y + 6.2f, 4.3f, s->title, C_GOLD, 0);
        if (s->arc[0] && (e == j_scroll || strcmp(s->arc, journal_step(v[e - 1])->arc)))
            m_text(W - pad - 10.0f, y + 6.2f, 3.3f, s->arc, C_DIM, 1);      /* where the part of the game changes */
        m_wrap(x, y + 6.2f + 8.0f, width, 7.2f, 4, 3.9f, s->done, C_TEXT, 0);
        y += h;
    }
    j_more_below = e < m;
    float mid = (48.5f + bottom) / 2;
    if (j_scroll > 0) {
        m_arrow(W - pad - 4.0f, 49.5f, 2.8f, 1, C_SOFT);                    /* more above */
        m_hit(pad, 45.0f, W - pad, mid, HIT_JUP, 0);
    }
    if (j_more_below) {
        m_arrow(W - pad - 4.0f, 152.0f, 2.8f, 0, C_SOFT);                   /* more below */
        m_hit(pad, mid, W - pad, bottom + 2.0f, HIT_JDOWN, 0);
    }
}

static const char *job_state_name(int state) {
    return state == JOB_OFFERED ? "Offered" : state == JOB_TAKEN ? "Under way" : state == JOB_REPORT ? "Report back" : "Done";
}

/* the Hunters' Guild: its jobs, then the highlighted one -- what the client
 * wants, what to do now (or the hint), how to collect the fee */
static void draw_journal_guild(float W, float pad) {
    int v[16], m = j_jobs(v);
    float x = pad + 4.0f, width = W - 2 * pad - 8.0f;
    if (!m) {
        m_wrap(x, 62.0f, width, 8.6f, 3, 4.4f,
               "No jobs on the board yet. The Hunters' Guild in Aiedo posts new work as the story goes on.", C_VALUE, 0);
        return;
    }
    if (menu_sel >= m) menu_sel = m - 1;
    if (menu_sel < 0) menu_sel = 0;
    const float r0 = 48.0f, row = 8.6f;
    JournalJob jb;
    for (int i = 0; i < m; i++) {
        journal_job(v[i], &jb);
        float a = r0 + row * (float)i, b = a + row - 0.8f;
        int sel = i == menu_sel;
        if (sel) m_box(pad, a, W - pad, b, 3.9f, C_GOLD, C_GOLD_LOW, 255);
        m_text(x + 2.0f, a + 6.0f, 4.2f, jb.name, sel ? C_INK : C_TEXT, 0);
        uint32_t c = jb.state == JOB_DONE ? C_DIM : jb.state == JOB_OFFERED ? C_SOFT : C_GOLD;
        m_text(W - pad - 6.0f, a + 6.0f, 3.8f, job_state_name(jb.state), sel ? C_INK_SOFT : c, 1);
        m_hit(pad, a, W - pad, b, HIT_ROW, i);
    }
    float rule = r0 + row * 8.0f + 2.0f;                                  /* below room for all eight */
    m_box(pad, rule, W - pad, rule + 0.5f, 0, C_RULE, C_RULE, 80);
    journal_job(v[menu_sel], &jb);
    char text[400];
    int shown = j_hint_job == v[menu_sel] && jb.state == JOB_TAKEN && jb.hint && jb.hint[0];
    if (shown) snprintf(text, sizeof text, "Hint: %s", jb.hint);
    else snprintf(text, sizeof text, "%s", jb.state == JOB_REPORT ? jb.reward : jb.state == JOB_TAKEN && jb.next ? jb.next : jb.about);
    m_wrap(x, rule + 9.0f, width, 7.4f, 4, 3.9f, text, shown ? C_VALUE : C_TEXT, 0);
    m_hit(pad, rule, W - pad, 156.0f, HIT_JHINT, 0);
}

static void draw_journal(float W, float H, const char *hint) {
    const float pad = 10.0f;
    if (j_tab >= j_tabs()) j_tab = JT_NOW;
    m_text(pad + 2.0f, 17.0f, 6.4f, "Journal", C_GOLD, 0);
    draw_back_button(W, pad);
    draw_journal_tabs(W, pad);
    if (j_tab == JT_NOW) draw_journal_now(W, pad);
    else if (j_tab == JT_STORY) draw_journal_story(W, pad);
    else draw_journal_guild(W, pad);
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* The launcher's logo: PsiLok's own artwork (launcher_logo.h, a PNG's bytes;
 * tools/make_launcher_logo.py). With it the launcher is two columns in a
 * window wider than it is tall -- the logo on the left with the port's name
 * under it, the choices on the right -- and in a tall one (a phone held
 * upright) the logo stands over the choices, where the star was. Without it
 * (it did not load) the star and the old column. */
static uint32_t *logo_px;                        /* as stored: 0xAARRGGBB, logo_sw x logo_sh */
static int logo_sw, logo_sh;                     /* 0: not loaded */
static SDL_Texture *logo_tex, *logo_glow_tex;    /* at the window's size, and its backlight (logo_make) */
static int logo_tw;                              /* the width they were made for */
static float logo_glow_x, logo_glow_y;           /* how far the backlight reaches past the logo, in logo widths and heights */

#define LAUNCH_PANEL_W 158.0f
typedef struct {
    int side;                                    /* the logo beside the choices */
    float logo_cx, logo_cy, logo_w, logo_h;      /* units; logo_w 0: the star */
    float title_y, menu_cx, menu_y0;
} LaunchLayout;

static LaunchLayout launch_layout(float W, float H) {
    LaunchLayout L;
    float oy = (H - 224.0f) / 2.0f, aspect = logo_sw ? (float)logo_sw / (float)logo_sh : 1.0f;
    L.side = logo_sw && W >= H * 1.15f;
    if (L.side) {
        L.logo_w = fminf(fmaxf(W - LAUNCH_PANEL_W - 30.0f, 60.0f), 200.0f);
        L.logo_h = L.logo_w / aspect;
        float gap = (W - LAUNCH_PANEL_W - L.logo_w) / 3.0f;     /* the same at both edges and between */
        float block = L.logo_h + 13.0f;                         /* the logo and the line under it */
        L.logo_cx = gap + L.logo_w / 2.0f;
        L.logo_cy = (H - block) / 2.0f + L.logo_h / 2.0f;
        L.title_y = L.logo_cy + L.logo_h / 2.0f + 11.0f;
        L.menu_cx = W - gap - LAUNCH_PANEL_W / 2.0f;
        /* the panel level with the logo's lettering (its stars rise above it) */
        L.menu_y0 = L.logo_cy + 0.10f * L.logo_h - (6.0f + 16.0f * (float)launch_rows()) / 2.0f;
    } else {
        L.logo_w = logo_sw ? fminf(W - 40.0f, 150.0f) : 0.0f;
        L.logo_h = L.logo_w / aspect;
        L.logo_cx = L.menu_cx = W / 2.0f;
        L.logo_cy = logo_sw ? oy + 88.0f - L.logo_h / 2.0f : oy + LAUNCH_EMBLEM_Y;   /* the logo: down to just over its line */
        L.title_y = logo_sw ? oy + 100.0f : oy + 98.0f;
        L.menu_y0 = oy + 116.0f;
    }
    return L;
}

/* the launcher: a line under the logo, the choices, what the highlighted
 * one means; the version and the hints in the corners */
static void draw_launcher(float W, float H, const char *hint) {
    LaunchLayout L = launch_layout(W, H);
    float cx = L.menu_cx;
    if (L.logo_w > 0.0f) {                           /* the logo is the title: one line under it */
        m_text(L.logo_cx, L.title_y, 4.2f, "A native port of Phantasy Star IV", C_SOFT, 2);
    } else {                                         /* no logo (it did not load): the port's name under the star */
        m_text(L.logo_cx, L.title_y, 12.5f, "PSIV Recomp", C_GOLD, 2);
        m_text(L.logo_cx, L.title_y + 10.5f, 4.2f, "A native port of Phantasy Star IV", C_SOFT, 2);
    }

    float pw = LAUNCH_PANEL_W, x0 = cx - pw / 2.0f, x1 = cx + pw / 2.0f, y0 = L.menu_y0;
    float row = !L.side && launch_rows() > 6 ? 14.0f : 16.0f;   /* (seven rows under the logo: a little closer) */
    float rows = (float)launch_rows();
    m_box(x0, y0, x1, y0 + 6.0f + row * rows, 7.0f, C_PANEL_TOP, C_PANEL_LOW, 236);
    m_frame(x0, y0, x1, y0 + 6.0f + row * rows, 7.0f, C_RULE, 150);
    int classic = classic_mode();
    for (int i = launch_first(); i < N_LAUNCHER; i++) {
        float a = y0 + 3.0f + row * (float)(i - launch_first()), b = a + row - 2.0f;
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

    char going[120];
    if (cont.page == AUTO_PAGE)
        snprintf(going, sizeof going, "Your auto save: Chaz at level %d, %u meseta. Straight in, past the title.",
                 cont.level, (unsigned)cont.meseta);
    else
        snprintf(going, sizeof going, "Save %d of %d: Chaz at level %d, %u meseta. Straight in, past the title.",
                 cont.page * 3 + cont.slot + 1, classic ? 3 : SAVEPAGES_MAX * 3, cont.level, (unsigned)cont.meseta);
    const char *about =
        menu_sel == L_CONTINUE ? going
        : menu_sel == L_LOAD ? "Choose which of your saves to play on."
        : menu_sel == L_ARENA ? "The Boss Arena and Wave Mode: the game's bosses again, or five battles in a row against the clock. Your saves are never touched."
        : menu_sel == L_START ? (rom_ready ? "Your Phantasy Star IV (USA), checked and ready."
                               : rom_problem[0] ? "The ROM that's installed can't be used: Start game says why, then asks for another."
                               : "Start game asks for your copy of Phantasy Star IV (USA): a .bin, .md, .gen or .smd file, or the .zip it came in.")
        : menu_sel == L_MODE ? (classic ? "Classic: the original game, unchanged. Every addition is off."
                                : "Enhanced: modern text and battle menus, widescreen, battle depth and effects, and the port's other additions.")
        : menu_sel == L_SETTINGS ? "The display, sound, battles and controls, and what each button does."
        : "Leave the game.";
    float ab = y0 + 6.0f + row * rows + 9.5f;
    if (L.side) m_wrap(cx - pw / 2.0f, ab, pw, 7.6f, 3, 3.8f, about, C_VALUE, 1);   /* under the panel, as wide */
    else m_wrap(cx - 95.0f, ab, 190.0f, 7.6f, 2, 3.8f, about, C_VALUE, 1);

    {   /* the music's switch: a pair of notes at the top right, struck through when it is off */
        int on = SDL_strcasecmp(setting("audio.launcher_music"), "off") != 0;
        float bx = W - 15.0f, by = 15.0f, r = 9.5f;
        uint32_t ink = on ? C_GOLD : C_DIM;
        m_box(bx - r, by - r, bx + r, by + r, r, C_PANEL_TOP, C_PANEL_LOW, 236);
        m_frame(bx - r, by - r, bx + r, by + r, r, on ? C_GOLD : C_RULE, on ? 190 : 130);
        m_box(bx - 5.8f, by + 1.6f, bx - 1.6f, by + 5.0f, 1.7f, ink, ink, 255);     /* the heads */
        m_box(bx + 0.8f, by + 1.6f, bx + 5.0f, by + 5.0f, 1.7f, ink, ink, 255);
        m_box(bx - 2.5f, by - 5.2f, bx - 1.6f, by + 3.4f, 0.0f, ink, ink, 255);     /* their stems */
        m_box(bx + 4.1f, by - 5.2f, bx + 5.0f, by + 3.4f, 0.0f, ink, ink, 255);
        m_box(bx - 2.5f, by - 5.4f, bx + 5.0f, by - 3.2f, 0.0f, ink, ink, 255);     /* and the beam */
        if (!on)
            for (int k = 0; k <= 26; k++) {
                float t = (float)k / 26.0f, x = bx - 6.0f + 12.0f * t, y = by + 6.0f - 12.0f * t;
                m_box(x - 0.85f, y - 0.85f, x + 0.85f, y + 0.85f, 0.85f, 0xF07882, 0xF07882, 255);
            }
        m_hit(bx - r - 1.5f, by - r - 1.5f, bx + r + 1.5f, by + r + 1.5f, HIT_MUSIC, 0);
    }
    {   /* the Discord server: Discord's mark, white on its blue, beside the notes */
        float bx = W - 15.0f - 23.0f, by = 15.0f, r = 9.5f, k = 6.3f;
        m_box(bx - r, by - r, bx + r, by + r, r, 0x5865F2, 0x5865F2, 255);
        if (m_ttf)
            ui_mask(&m_cv, DISCORD_MARK, DISCORD_MARK_N, DISCORD_MARK_N, (bx - k) * m_S, (by - k + 0.2f) * m_S,
                    (bx + k) * m_S, (by + k + 0.2f) * m_S, 0xFFFFFF, 255);
        else m_text(bx, by + 2.5f, 5.0f, "D", 0xFFFFFF, 2);
        m_hit(bx - r - 1.5f, by - r - 1.5f, bx + r + 1.5f, by + r + 1.5f, HIT_DISCORD, 0);
    }

    m_text(6.0f, H - 5.0f, 3.2f, APP_NAME " " PSIV_VERSION, C_DIM, 0);
    m_text(W - 6.0f, H - 5.0f, 3.4f, hint, C_HINT, 1);
}

/* Load game: the page's three slots as cards -- the slot's number among them all, the party's faces
 * (from the ROM), the leader and their level, the world, the meseta -- and the pages' tabs */
#define LOAD_H 190.0f
static void draw_load(float W, float H, const char *hint) {
    static const char *const WORLDS[6] = {"Motavia", "Dezolis", "Rykros", "Zelan", "Kuran", "The Air Castle"};
    const float pad = 10.0f;
    int pages = load_tabs(), on_auto = load_on_auto();
    const uint8_t *cart = load_cartridge();
    char t[64];
    m_text(pad + 2.0f, 17.0f, 6.4f, "Load game", C_GOLD, 0);
    draw_back_button(W, pad);
    if (pages > 1) {                                 /* the pages: which slots each holds; then the auto save */
        float x = pad + 2.0f + m_width("Load game", 6.4f) + 12.0f;
        for (int i = 0; i < pages; i++) {
            if (i == load_pages()) snprintf(t, sizeof t, "Auto");
            else snprintf(t, sizeof t, "%d-%d", i * 3 + 1, i * 3 + 3);
            float w = m_width(t, 4.4f) + 12.0f;
            int on = i == load_page;
            if (on) m_box(x, 8.0f, x + w, 21.0f, 6.5f, 0x3A4C8E, 0x2A3870, 255);
            m_text(x + w / 2, 17.2f, 4.4f, t, on ? C_TEXT : C_SOFT, 2);
            m_hit(x, 8.0f, x + w, 21.0f, HIT_PAGE, i);
            x += w + 2.0f;
        }
    }
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    float top = 30.0f, gap = 3.0f, ch = (H - 16.0f - top - 2.0f * gap) / 3.0f;
    if (on_auto)                                     /* its one card, and what it is */
        m_wrap(pad + 4.0f, top + ch + gap + 14.0f, W - 2.0f * pad - 8.0f, 9.0f, 4, 4.3f,
               "The game saves here for you as you play: each time you come to a new place, and every few minutes "
               "on the way. It is one save, made over each time; your own saves are never touched. "
               "Settings > Game > Auto save turns it off.", C_SOFT, 0);
    for (int k = 0; k < (on_auto ? 1 : 3); k++) {
        float c0 = top + (float)k * (ch + gap), c1 = c0 + ch, mid = (c0 + c1) / 2.0f, x0 = pad, x1 = W - pad;
        int sel = k == menu_sel, saved = savepages_saved(cart, k);
        const uint8_t *d = &cart[0x100 + k * 0xA00];
        if (sel) m_box(x0, c0, x1, c1, 7.0f, C_GOLD, C_GOLD_LOW, 255);
        else m_box(x0, c0, x1, c1, 7.0f, 0x243470, 0x1A2658, 255);
        m_hit(x0, c0, x1, c1, HIT_ROW, k);
        snprintf(t, sizeof t, "%d", load_page * 3 + k + 1);
        if (on_auto) m_text(x0 + 14.0f, mid + 1.8f, 4.2f, "Auto", sel ? C_INK : C_SOFT, 2);
        else m_text(x0 + 13.0f, mid + 3.6f, 8.4f, t, sel ? C_INK : C_SOFT, 2);
        if (!saved) {
            m_text(x0 + 28.0f, mid + 2.2f, 5.0f, "Empty", sel ? C_INK_SOFT : C_DIM, 0);
            continue;
        }
        int ids[5], n = 0;
        for (int i = 0; i < 5; i++)
            if (d[0x30A + i] < 11) ids[n++] = d[0x30A + i];
        float ps = fminf(27.0f, ch - 12.0f), px = x0 + 28.0f;
        int faces = 0;
        for (int i = 0; i < n && m_ttf; i++) faces += ui_portrait(ids[i]) != NULL;
        if (faces) {
            for (int i = 0; i < n; i++) {
                const UiImage *im = ui_portrait(ids[i]);
                m_box(px, mid - ps / 2.0f, px + ps, mid + ps / 2.0f, 1.5f, 0x0C1440, 0x0C1440, 255);
                if (im) ui_image(&m_cv, im, px * m_S, (mid - ps / 2.0f) * m_S, ps * m_S, 0, 255);
                px += ps + 2.0f;
            }
            px = x0 + 28.0f + 5.0f * (ps + 2.0f) + 5.0f;
        }
        if (n) {
            m_text(px, mid - 1.6f, 5.2f, UI_CHAR_NAMES[ids[0]], sel ? C_INK : C_TEXT, 0);
            snprintf(t, sizeof t, "LV %u", (unsigned)(d[0x400 + ids[0] * 0x80 + 8] << 8 | d[0x400 + ids[0] * 0x80 + 9]));
            m_text(x1 - 10.0f, mid - 1.6f, 5.2f, t, sel ? C_INK : C_TEXT, 1);
        }
        m_text(px, mid + 9.4f, 4.4f, d[0x300] < 6 ? WORLDS[d[0x300]] : "", sel ? C_INK_SOFT : C_SOFT, 0);
        {   /* the meseta, its thousands apart */
            unsigned long v = (unsigned long)d[0x338] << 24 | (unsigned long)d[0x339] << 16 | (unsigned long)d[0x33A] << 8 | d[0x33B];
            char raw[16];
            int len = snprintf(raw, sizeof raw, "%lu", v), o = 0;
            for (int i = 0; i < len; i++) {
                if (i && (len - i) % 3 == 0) t[o++] = ',';
                t[o++] = raw[i];
            }
            snprintf(t + o, sizeof t - (size_t)o, " meseta");
        }
        m_text(x1 - 10.0f, mid + 9.4f, 4.4f, t, sel ? C_INK_SOFT : C_SOFT, 1);
    }
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* a boss's picture (arenapic.h) at its frame now, nearest-pixel, into units x0..x0+w, y0..y0+h */
static uint64_t arena_anim_t(void) { return SDL_GetTicks() * 60 / 1000; }
static void draw_arena_pic(const ArenaPic *pic, float x0, float y0, float w, float h) {
    int k = arenapic_frame_at(pic, arena_anim_t());
    const uint8_t *src = pic->px + (size_t)k * (size_t)pic->w * (size_t)pic->h;
    const uint16_t *pal = pic->pal + k * 64;
    uint32_t rgb[64];
    for (int c = 0; c < 64; c++) {
        unsigned v = pal[c];
        unsigned r = (v >> 1 & 7) * 36, g = (v >> 5 & 7) * 36, bl = (v >> 9 & 7) * 36;
        rgb[c] = r << 16 | g << 8 | bl;
    }
    int X0 = (int)(x0 * m_S), Y0 = (int)(y0 * m_S), X1 = (int)((x0 + w) * m_S), Y1 = (int)((y0 + h) * m_S);
    for (int Y = Y0; Y < Y1; Y++) {
        if (Y < m_cv.y0 || Y >= m_cv.y1) continue;
        int v = (int)((float)(Y - Y0) * (float)pic->h / (float)(Y1 - Y0));
        if (v >= pic->h) v = pic->h - 1;
        for (int X = X0; X < X1; X++) {
            if (X < m_cv.x0 || X >= m_cv.x1) continue;
            int u = (int)((float)(X - X0) * (float)pic->w / (float)(X1 - X0));
            if (u >= pic->w) u = pic->w - 1;
            ui_blend(&m_cv.px[(size_t)Y * (size_t)m_cv.stride + (size_t)X], rgb[src[v * pic->w + u]], 255);
        }
    }
}
/* The Boss Arena's card: over the game while a fight's save loads and the party waits on the field
 * (the map never shows), fading as the battle comes in; after a fight, under the list. The boss
 * (its picture moving), its number and music, the quickest win. */
#define CARD_HOLD 4                                  /* fight frames before the card starts to go */
#define CARD_FADE 24                                 /* ... and over how many it goes */
static int wave_card_t;                              /* Wave Mode: frames since its card started to go */
static float arena_card_alpha(void) {
    if (!arena_mode || arena_scanning) return 0.0f;
    if (wave_mode) {                                 /* over the loading and a run's end; going as the walk starts */
        int w = wave_state();
        if (w == WAVE_OFF) return 0.0f;
        if (w == WAVE_LOADING || w == WAVE_RELOAD || w == WAVE_CLEARED || w == WAVE_LOST) return 1.0f;
        return wave_card_t >= CARD_FADE ? 0.0f : 1.0f - (float)wave_card_t / (float)CARD_FADE;
    }
    int s = arena_state();
    if (s == ARENA_FIGHTING) {
        int f = arena_fight_frames() - CARD_HOLD;
        return f <= 0 ? 1.0f : f >= CARD_FADE ? 0.0f : 1.0f - (float)f / (float)CARD_FADE;
    }
    return s == ARENA_LOADING || s == ARENA_WON || s == ARENA_LOST ? 1.0f : 0.0f;
}
static void arena_card(int ow, int oh, float alpha) {
    static SDL_Texture *tex;
    static uint32_t *buf;
    static int tw, th, last_i = -2, last_k = -2, last_hint = -1;
    int i = wave_mode ? 1000 + wave_current() * 10 + wave_node() : arena_current();
    const ArenaBoss *b = wave_mode ? NULL : arena_boss(i);
    const WaveRun *wr = wave_mode ? wave_run(wave_current()) : NULL;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (!ui_ready() || (!b && !wr)) {                /* no font (or no fight): a plain cover */
        SDL_SetRenderDrawColor(renderer, 6, 9, 26, (Uint8)(alpha * 255.0f));
        SDL_RenderFillRect(renderer, NULL);
        return;
    }
    if (!tex || ow != tw || oh != th) {
        if (tex) SDL_DestroyTexture(tex);
        free(buf);
        buf = malloc((size_t)ow * (size_t)oh * 4);
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
        tw = ow;
        th = oh;
        last_i = -2;
        if (!tex || !buf) return;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    }
    const ArenaPic *pic = !wr && i < ARENA_MAX && arena_pic_state[i] > 0 ? &arena_pics[i] : NULL;
    int k = pic ? arenapic_frame_at(pic, arena_anim_t()) : wr ? (int)(wave_time() / 6) : -1;
    int hint = wr ? wave_state() == WAVE_CLEARED || wave_state() == WAVE_LOST
                  : arena_state() == ARENA_WON || arena_state() == ARENA_LOST;
    if (i != last_i || k != last_k || hint != last_hint) {   /* drawn again only when it changes */
        last_i = i;
        last_k = k;
        last_hint = hint;
        float sS = m_S, sx = m_ox, sy = m_oy;        /* (the menu's own, put back after) */
        UiCanvas scv = m_cv;
        int sttf = m_ttf;
        m_S = fminf((float)ow / 320.0f, (float)oh / 224.0f);
        m_ox = m_oy = 0;
        m_ttf = 1;
        m_cv = (UiCanvas){buf, ow, 0, 0, ow, oh};
        ui_set_straight_alpha(1);
        ui_round_rect_v(&m_cv, 0, 0, (float)ow, (float)oh, 0, 0x101A44, 0x02040E, 255);
        float W = (float)ow / m_S, H = (float)oh / m_S, cx = W / 2.0f;
        if (wr) {                                    /* Wave Mode: the battle coming, where, and the time so far */
            int nn = wave_node() < WAVE_NODES ? wave_node() : WAVE_NODES - 1;
            const WaveNode *nd = &wr->node[nn];
            char t[160], tm[32];
            float y = H / 2.0f - 34.0f;
            snprintf(t, sizeof t, "WAVE MODE   \xC2\xB7   %s", wr->name);
            m_text(cx, y, 3.8f, t, C_SOFT, 2);
            if (hint) {                              /* the run over: how it ended, not a battle to come */
                int won = wave_state() == WAVE_CLEARED;
                m_text(cx, y + 18.0f, 10.0f, won ? "Run cleared" : "Run over", won ? 0xA8F0B4 : 0xF4A0A0, 2);
                if (won) snprintf(t, sizeof t, "All %d battles won", WAVE_NODES);
                else snprintf(t, sizeof t, "Defeated in battle %d, at %s", nn + 1, nd->place);
                m_text(cx, y + 31.0f, 5.0f, t, C_TEXT, 2);
            } else {
                snprintf(t, sizeof t, "Battle %d of %d", nn + 1, WAVE_NODES);
                m_text(cx, y + 18.0f, 10.0f, t, C_GOLD, 2);
                m_text(cx, y + 31.0f, 5.6f, nd->place, C_TEXT, 2);
                m_text(cx, y + 40.0f, 4.2f, nd->enemies, C_VALUE, 2);
            }
            uint8_t ids[5];
            int np = wave_party(ids);
            float fw = 16.0f, gap = 6.0f, px = cx - ((float)np * fw + (float)(np - 1) * gap) / 2.0f, fy = y + 48.0f;
            for (int q = 0; q < np; q++) {
                const UiImage *im = arena_faces && ids[q] < 11 ? ui_portrait(ids[q]) : NULL;
                m_box(px, fy, px + fw, fy + fw, 1.5f, 0x0C1440, 0x0C1440, 255);
                if (im) ui_image(&m_cv, im, px * m_S, fy * m_S, fw * m_S, 0, wave_fell() >> q & 1 ? 150 : 255);
                px += fw + gap;
            }
            if (wave_time()) {
                wave_time_text(wave_time(), tm, sizeof tm);
                snprintf(t, sizeof t, "Time in battle so far  %s", tm);
                m_text(cx, fy + fw + 10.0f, 4.0f, t, 0xA8F0B4, 2);
            }
            if (hint) m_text(cx, H - 8.0f, 3.6f, "Press any button for Wave Mode", C_HINT, 2);
            ui_set_straight_alpha(0);
            SDL_UpdateTexture(tex, NULL, buf, ow * 4);
            m_S = sS, m_ox = sx, m_oy = sy, m_cv = scv, m_ttf = sttf;
            SDL_SetTextureAlphaModFloat(tex, alpha);
            SDL_RenderTexture(renderer, tex, NULL, NULL);
            return;
        }
        float pw = 216.0f, ph = pic ? pw * (float)pic->h / (float)pic->w : 0.0f;
        float y = (H - (ph + 62.0f)) / 2.0f;     /* the whole stack, centred */
        char t[128];
        snprintf(t, sizeof t, "BOSS ARENA   \xC2\xB7   %d of %d", i + 1, arena_count());
        m_text(cx, y + 5.0f, 3.6f, t, C_SOFT, 2);
        m_text(cx, y + 19.0f, 8.4f, b->name, C_GOLD, 2);
        y += 27.0f;
        if (pic) {
            float x0 = cx - pw / 2.0f;
            m_box(x0 - 3.0f, y - 3.0f, x0 + pw + 3.0f, y + ph + 3.0f, 4.0f, C_PANEL_TOP, C_PANEL_LOW, 255);
            draw_arena_pic(pic, x0, y, pw, ph);
            m_frame(x0 - 3.0f, y - 3.0f, x0 + pw + 3.0f, y + ph + 3.0f, 4.0f, C_RULE, 140);
            y += ph + 3.0f;
        }
        m_text(cx, y + 13.0f, 4.4f, arena_music(i), C_VALUE, 2);
        int best = arena_best(i);
        if (best) snprintf(t, sizeof t, "Best %d:%02d", best / 60, best % 60);
        else snprintf(t, sizeof t, "Not beaten yet");
        m_text(cx, y + 23.0f, 3.8f, t, best ? 0xA8F0B4 : C_HINT, 2);
        if (hint) m_text(cx, H - 8.0f, 3.6f, "Press any button for the Boss Arena", C_HINT, 2);
        ui_set_straight_alpha(0);
        SDL_UpdateTexture(tex, NULL, buf, ow * 4);
        m_S = sS, m_ox = sx, m_oy = sy, m_cv = scv, m_ttf = sttf;
    }
    SDL_SetTextureAlphaModFloat(tex, alpha);
    SDL_RenderTexture(renderer, tex, NULL, NULL);
}

/* Wave Mode under way: which battle, and the time in battle so far, top centre */
static void wave_hud(int ow, int oh, float card) {
    int w = wave_state();
    if (card >= 0.99f || (w != WAVE_WALKING && w != WAVE_FIGHTING)) return;
    char t[64], tm[32];
    wave_time_text(wave_time(), tm, sizeof tm);
    snprintf(t, sizeof t, "BATTLE %d/%d  %s", wave_node() + 1, WAVE_NODES, tm);
    float s = fmaxf(1.0f, floorf((float)oh / 270.0f)), tw = 8.0f * s * (float)strlen(t), th = 8.0f * s;
    float x = ((float)ow - tw) / 2.0f, y = 4.0f * s;
    Uint8 a = (Uint8)((1.0f - card) * 255.0f);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_FRect bg = {x - 5.0f * s, y - 3.0f * s, tw + 10.0f * s, th + 6.0f * s};
    SDL_SetRenderDrawColor(renderer, 12, 18, 40, (Uint8)(a * 200 / 255));
    SDL_RenderFillRect(renderer, &bg);
    SDL_SetRenderScale(renderer, s, s);
    SDL_SetRenderDrawColor(renderer, 255, 224, 138, a);
    SDL_RenderDebugText(renderer, x / s, y / s, t);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

/* the picture shown now, for the menu's signature (it is drawn again when this changes) */
static uint64_t arena_anim_sig(void) {
    if (menu_screen != MENU_ARENA || menu_sel < 0 || menu_sel >= ARENA_MAX || arena_pic_state[menu_sel] <= 0) return 0;
    return (uint64_t)arenapic_frame_at(&arena_pics[menu_sel], arena_anim_t()) + 1;
}

/* Bonus Modes: each mode a card -- what it is, and how far you've got */
static void draw_bonus(float W, float H, const char *hint) {
    const float pad = 10.0f;
    char t[160];
    m_text(pad + 2.0f, 17.0f, 6.4f, "Bonus Modes", C_GOLD, 0);
    draw_back_button(W, pad);
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    static const char *const NAME[3] = {"Boss Arena", "Wave Mode", "Jukebox"};
    static const char *const ABOUT[3] = {
        "The game's boss fights again, with the party the story has there: set their macros, "
        "beat your quickest time.",
        "Five battles in a row against the clock, on the game's own maps. Pick a party, set its macros "
        "and keep them for the run; between fights the party gets some HP and TP back.",
        "Every song in the game, played by its own sound chips from your ROM, or your own file for it "
        "from the music folder."};
    int beaten = 0, cleared = 0, runs = wave_count();
    for (int i = 0; i < arena_count(); i++) beaten += arena_best(i) > 0;
    for (int r = 0; r < runs && r < WAVE_RUNS_MAX; r++) cleared += wave_best[r] != 0;
    float y = 31.0f, h = 42.0f;
    for (int k = 0; k < 3; k++) {
        int sel = menu_sel == k, off = k == 2 && !in_launcher;   /* (the Jukebox: the launcher's) */
        float a = y + (h + 5.0f) * (float)k, b = a + h;
        if (sel) m_box(pad, a, W - pad, b, 8.0f, C_GOLD, C_GOLD_LOW, 255);
        else m_box(pad, a, W - pad, b, 8.0f, 0x243470, 0x1A2658, 255);
        m_text(pad + 10.0f, a + 14.0f, 6.4f, NAME[k], sel ? C_INK : off ? C_DIM : C_GOLD, 0);
        if (k == 0) snprintf(t, sizeof t, "%d of %d beaten", beaten, arena_count());
        else if (k == 1) snprintf(t, sizeof t, "%d of %d runs cleared", cleared, runs);
        else if (off) snprintf(t, sizeof t, "From the launcher");
        else if (jukebox_song) snprintf(t, sizeof t, "Playing: %s", JUKEBOX_TITLES[jukebox_song - JUKEBOX_FIRST]);
        else snprintf(t, sizeof t, "%d songs", JUKEBOX_N);
        int lit = k == 0 ? beaten : k == 1 ? cleared : jukebox_song && !off;
        m_text(W - pad - 10.0f, a + 13.5f, 4.0f, t, sel ? C_INK_SOFT : off ? C_DIM : lit ? 0xA8F0B4 : C_SOFT, 1);
        m_wrap(pad + 10.0f, a + 24.0f, W - 2.0f * pad - 20.0f, 6.6f, 3, 3.8f, ABOUT[k],
               sel ? C_INK_SOFT : off ? C_DIM : C_VALUE, 0);
        m_hit(pad, a, W - pad, b, HIT_ROW, k);
    }
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* The Jukebox's three sources, as small pictures centred on (cx, cy), `s` units a step: the
 * Mega Drive's sound chip (a chip, its legs and its notch), Algol (a wave), Proto Algol (a saw), a music pack (a 1/4"
 * jack plug: the cable, the grip, the sleeve, the ring between, the tip). `bg` cuts the gaps. */
static void jb_icon(int kind, float cx, float cy, float s, uint32_t c, uint32_t bg) {
    if (kind == JB_CHIP) {
        float w = 7.0f * s, h = 4.6f * s;
        for (int k = 0; k < 4; k++) {
            float x = cx - w / 2.0f + ((float)k + 0.5f) * w / 4.0f;
            m_box(x - 0.45f * s, cy - h / 2.0f - 1.3f * s, x + 0.45f * s, cy + h / 2.0f + 1.3f * s, 0, c, c, 255);
        }
        m_box(cx - w / 2.0f, cy - h / 2.0f, cx + w / 2.0f, cy + h / 2.0f, 0.7f * s, c, c, 255);
        m_box(cx - w / 2.0f + 0.8f * s, cy - h / 2.0f + 0.8f * s, cx - w / 2.0f + 1.9f * s, cy - h / 2.0f + 1.9f * s,
              0.55f * s, bg, bg, 255);
    } else if (kind == JB_ALGOL) {                   /* a clean wave */
        for (int k = 0; k <= 36; k++) {
            float t = (float)k / 36.0f, x = cx - 4.6f * s + 9.2f * s * t, y = cy - 2.4f * s * sinf(t * 9.4247780f);
            m_box(x - 0.5f * s, y - 0.5f * s, x + 0.5f * s, y + 0.5f * s, 0.5f * s, c, c, 255);
        }
    } else if (kind == JB_PROTO) {                   /* a saw: the synth's */
        for (int k = 0; k <= 36; k++) {
            float t = (float)k / 36.0f, x = cx - 4.6f * s + 9.2f * s * t, ph = t * 2.0f - floorf(t * 2.0f);
            float y = cy + 2.4f * s * (1.0f - 2.0f * ph);
            m_box(x - 0.45f * s, y - 0.45f * s, x + 0.45f * s, y + 0.45f * s, 0.45f * s, c, c, 255);
        }
        m_box(cx - 0.45f * s, cy - 2.4f * s, cx + 0.45f * s, cy + 2.4f * s, 0.3f * s, c, c, 255);   /* (its edge) */
    } else {
        m_box(cx - 6.0f * s, cy - 0.45f * s, cx - 2.6f * s, cy + 0.45f * s, 0.3f * s, c, c, 255);   /* the cable */
        m_box(cx - 3.0f * s, cy - 1.5f * s, cx + 0.4f * s, cy + 1.5f * s, 0.9f * s, c, c, 255);    /* the grip */
        m_box(cx + 0.4f * s, cy - 0.85f * s, cx + 3.6f * s, cy + 0.85f * s, 0, c, c, 255);         /* the sleeve */
        m_box(cx + 3.6f * s, cy - 0.85f * s, cx + 4.3f * s, cy + 0.85f * s, 0, bg, bg, 255);       /* the ring */
        m_box(cx + 4.3f * s, cy - 0.75f * s, cx + 6.0f * s, cy + 0.75f * s, 0.75f * s, c, c, 255); /* the tip */
    }
}

/* the Jukebox: what plays (a switch of three), the songs down the list -- the one playing, and a plug
 * by each your music pack has -- and what plays at the foot */
static void draw_jukebox(float W, float H, const char *hint) {
    const float pad = 10.0f;
    char t[160];
    static const char *const SRC[JB_N] = {"Chip", "Algol", "Proto Algol", "Music pack"};
    m_text(pad + 2.0f, 17.0f, 6.4f, "Jukebox", C_GOLD, 0);
    m_text(pad + 6.0f + m_width("Jukebox", 6.4f), 16.6f, 4.3f, "The game's songs", C_SOFT, 0);
    draw_back_button(W, pad);
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    {   /* the switch */
        float y0 = 29.5f, y1 = 43.5f, x = pad, mid = (y0 + y1) / 2.0f;
        float sx0 = x;
        for (int k = 0; k < JB_N; k++) {
            float w = 18.0f + m_width(SRC[k], 4.2f) + 8.0f;
            int on = jukebox_src == k, off = k == JB_PACK && !jukebox_files;
            uint32_t top = on ? C_GOLD : 0x26346A, low = on ? C_GOLD_LOW : 0x18214A;
            m_box(x, y0, x + w, y1, 6.0f, top, low, 255);
            uint32_t ink = on ? C_INK : off ? C_DIM : C_TEXT;
            jb_icon(k, x + 10.0f, mid, 0.95f, ink, on ? C_GOLD_LOW : 0x1E2A5C);
            m_text(x + 18.0f, mid + 1.6f, 4.2f, SRC[k], ink, 0);
            m_hit(x, y0, x + w, y1, HIT_PAGE, k);
            x += w + 4.0f;
        }
        if (menu_sel < 0) m_frame(sx0 - 2.0f, y0 - 2.0f, x - 2.0f, y1 + 2.0f, 7.5f, C_GOLD, 230);
        if (jukebox_files) {                         /* how much of the game your pack has */
            snprintf(t, sizeof t, "%d", jukebox_files);
            m_text(W - pad - 2.0f, mid + 1.5f, 3.6f, t, C_SOFT, 1);
            jb_icon(JB_PACK, W - pad - 8.0f - m_width(t, 3.6f), mid, 0.62f, C_SOFT, 0x1A2658);
        }
    }
    float top = 48.0f, row = 14.0f, x0 = pad, x1 = W - pad - 14.0f, bottom = top + row * JUKEBOX_ROWS - 2.0f;
    for (int r = 0; r < JUKEBOX_ROWS && jukebox_top + r < JUKEBOX_N; r++) {
        int i = jukebox_top + r, sel = i == menu_sel, on = jukebox_song == JUKEBOX_FIRST + i;
        float a = top + row * (float)r, b = a + row - 2.0f, mid = (a + b) / 2.0f;
        if (sel) m_box(x0, a, x1, b, 6.5f, C_GOLD, C_GOLD_LOW, 255);
        else if (on) m_box(x0, a, x1, b, 6.5f, 0x3A4C8E, 0x2A3870, 255);
        snprintf(t, sizeof t, "%d", i + 1);
        m_text(x0 + 15.0f, mid + 1.5f, 3.8f, t, sel ? C_INK_SOFT : C_DIM, 1);
        m_text(x0 + 21.0f, mid + 1.7f, 4.6f, JUKEBOX_TITLES[i], sel ? C_INK : on ? C_GOLD : C_TEXT, 0);
        if (on) m_text(x1 - 40.0f, mid + 1.5f, 3.8f, "Playing", sel ? C_INK_SOFT : 0xA8F0B4, 1);
        if (JUKEBOX_FIRST + i == launcher_song()) {      /* the launcher's song: a tag */
            float tw = m_width("Launcher", 3.2f) + 7.0f, tx1 = on ? x1 - 44.0f - m_width("Playing", 3.8f) : x1 - 40.0f;
            m_box(tx1 - tw, mid - 4.0f, tx1, mid + 4.0f, 4.0f, sel ? 0x8A5A10 : 0x6A4A12, sel ? 0x6A4008 : 0x4A3008, 255);
            m_text(tx1 - tw / 2.0f, mid + 1.4f, 3.2f, "Launcher", 0xFFE08A, 2);
        }
        if (jukebox_file[i])                         /* your music pack has it */
            jb_icon(JB_PACK, x1 - 29.0f, mid, 0.62f, sel ? C_INK_SOFT : jukebox_src == JB_PACK ? 0xA8F0B4 : C_SOFT,
                    sel ? C_GOLD_LOW : on ? 0x2A3870 : 0x1A2658);
        snprintf(t, sizeof t, "$%02X", JUKEBOX_FIRST + i);
        m_text(x1 - 8.0f, mid + 1.5f, 3.6f, t, sel ? C_INK_SOFT : C_DIM, 1);
        m_hit(x0, a, x1, b, HIT_ROW, i);
    }
    if (jukebox_top > 0) {                           /* more above, more below: arrows (for touch, too) */
        m_arrow(x1 + 7.0f, top + 5.0f, 3.2f, 1, C_SOFT);
        m_hit(x1 + 1.0f, top, x1 + 13.0f, top + 14.0f, HIT_JUP, 0);
    }
    if (jukebox_top + JUKEBOX_ROWS < JUKEBOX_N) {
        m_arrow(x1 + 7.0f, bottom - 5.0f, 3.2f, 0, C_SOFT);
        m_hit(x1 + 1.0f, bottom - 14.0f, x1 + 13.0f, bottom, HIT_JDOWN, 0);
    }
    if (jukebox_song) {
        int i = jukebox_song - JUKEBOX_FIRST;
        snprintf(t, sizeof t, "Playing: %s, %s", JUKEBOX_TITLES[i],
                 jukebox_src == JB_ALGOL ? "on Algol"
                 : jukebox_src == JB_PROTO ? "on Proto Algol"
                 : jukebox_src == JB_PACK && jukebox_file[i] ? "from your music pack"
                 : jukebox_src == JB_PACK ? "on the chip (no file for it in your pack)" : "on the chip");
    } else snprintf(t, sizeof t, "%s", SDL_strcasecmp(setting("audio.launcher_music"), "off")
                                     ? "Playing the launcher's own song: choose one to hear it instead"
                                     : "Choose a song to hear it");
    int noted = prep_note[0] && SDL_GetTicks() < prep_note_until;
    if (noted) snprintf(t, sizeof t, "%s", prep_note);
    m_text(pad + 2.0f, bottom + 9.0f, 3.9f, t, noted ? C_GOLD : jukebox_song ? 0xA8F0B4 : C_DIM, 0);
    if (menu_sel >= 0 && menu_sel < JUKEBOX_N) {     /* the highlighted song: the launcher's, at a press */
        int is = JUKEBOX_FIRST + menu_sel == launcher_song();
        const char *b = is ? "The launcher's song" : "Set as launcher song";
        float bw = m_width(b, 3.8f) + 12.0f, bx1 = W - pad - 2.0f, bx0 = bx1 - bw;
        m_box(bx0, bottom + 3.0f, bx1, bottom + 12.5f, 4.5f, is ? 0x4A3008 : 0x26346A, is ? 0x3A2406 : 0x18214A, 255);
        m_text((bx0 + bx1) / 2.0f, bottom + 9.4f, 3.8f, b, is ? 0xFFE08A : C_TEXT, 2);
        if (!is) m_hit(bx0, bottom + 2.0f, bx1, bottom + 13.5f, HIT_MODE, 0);
    }
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* the Boss Arena's two lists, as tabs in the title: the bosses, and Wave Mode's runs */
static float draw_mode_tabs(float pad, int which) {
    static const char *const T[2] = {"Boss Arena", "Wave Mode"};
    float x = pad + 2.0f;
    for (int k = 0; k < 2; k++) {
        float sz = k == which ? 6.4f : 5.0f, w = m_width(T[k], sz);
        m_text(x, 17.0f, sz, T[k], k == which ? C_GOLD : C_SOFT, 0);
        m_hit(x - 2.0f, 6.0f, x + w + 2.0f, 23.0f, HIT_PAGE, k);
        x += w + 9.0f;
    }
    return x - 9.0f;
}

/* Boss Arena: the bosses down the left (a mark by those beaten), the one under the cursor on the
 * right -- its music, the story's party with their faces and levels, the quickest win */
static void draw_arena(float W, float H, const char *hint) {
    const float pad = 10.0f;
    const char *dot = m_ttf ? "   \xC2\xB7   " : "  -  ";
    char t[128];
    int n = arena_count();
    float tabs_end = draw_mode_tabs(pad, 0);
    draw_back_button(W, pad);
    if (arena_result >= 0 && arena_boss(arena_result_boss)) {   /* how the last fight went */
        const char *who = arena_boss(arena_result_boss)->name;
        if (arena_result == ARENA_WON)
            snprintf(t, sizeof t, "Won: %s in %d:%02d", who, arena_result_secs / 60, arena_result_secs % 60);
        else snprintf(t, sizeof t, "Defeated by %s", who);
        m_text(tabs_end + 8.0f, 16.6f, 4.3f, t,
               arena_result == ARENA_WON ? 0xA8F0B4 : 0xF4A0A0, 0);
    }
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    float top = 30.0f, row = 15.0f, lx0 = pad, lx1 = 126.0f, bottom = top + row * ARENA_ROWS - 2.0f;
    if (arena_scroll > n - ARENA_ROWS) arena_scroll = n > ARENA_ROWS ? n - ARENA_ROWS : 0;
    if (arena_scroll < 0) arena_scroll = 0;
    for (int r = 0; r < ARENA_ROWS && arena_scroll + r < n; r++) {
        int i = arena_scroll + r, sel = i == menu_sel;
        float a = top + row * (float)r, b = a + row - 2.0f, mid = (a + b) / 2.0f;
        if (sel) m_box(lx0, a, lx1, b, 6.5f, C_GOLD, C_GOLD_LOW, 255);
        snprintf(t, sizeof t, "%d", i + 1);
        m_text(lx0 + 13.0f, mid + 1.5f, 3.8f, t, sel ? C_INK_SOFT : C_DIM, 1);
        const char *name = arena_boss(i)->name;
        float sz = 4.6f;
        while (sz > 3.4f && m_width(name, sz) > lx1 - lx0 - 30.0f) sz -= 0.2f;
        m_text(lx0 + 17.0f, mid + 1.7f, sz, name, sel ? C_INK : C_TEXT, 0);
        if (arena_best(i) > 0) {                     /* beaten: a mark */
            uint32_t c = sel ? 0x2E7A3C : 0x7CDC8E;
            m_box(lx1 - 9.5f, mid - 2.0f, lx1 - 5.5f, mid + 2.0f, 2.0f, c, c, 255);
        }
        m_hit(lx0, a, lx1, b, HIT_ROW, i);
    }
    if (arena_scroll > 0) {                          /* more above, more below: arrows (for touch, too) */
        m_arrow(lx1 + 6.0f, top + 5.0f, 3.2f, 1, C_SOFT);
        m_hit(lx1 + 1.0f, top, lx1 + 11.0f, top + 14.0f, HIT_JUP, 0);
    }
    if (arena_scroll + ARENA_ROWS < n) {
        m_arrow(lx1 + 6.0f, bottom - 5.0f, 3.2f, 0, C_SOFT);
        m_hit(lx1 + 1.0f, bottom - 14.0f, lx1 + 11.0f, bottom, HIT_JDOWN, 0);
    }

    const ArenaBoss *b = arena_boss(menu_sel);
    float x0 = lx1 + 14.0f, x1 = W - pad;
    m_box(x0, top, x1, bottom, 7.0f, 0x243470, 0x1A2658, 255);
    if (!b) { m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0); return; }
    float sz = 6.0f;
    while (sz > 4.2f && m_width(b->name, sz) > x1 - x0 - 16.0f) sz -= 0.2f;
    m_text(x0 + 8.0f, top + 12.0f, sz, b->name, C_GOLD, 0);
    snprintf(t, sizeof t, "Boss %d of %d%s%s", menu_sel + 1, n, dot, arena_music(menu_sel));
    m_text(x0 + 8.0f, top + 20.5f, 3.6f, t, C_SOFT, 0);
    int best = arena_best(menu_sel);
    if (best > 0) {
        snprintf(t, sizeof t, "Best %d:%02d", best / 60, best % 60);
        m_text(x1 - 8.0f, top + 20.5f, 3.6f, t, 0xA8F0B4, 1);
    }
    {   /* the boss as its battle shows it, idling (arenapic.h) */
        float bx0 = x0 + 8.0f, bx1 = x1 - 8.0f, by0 = top + 24.5f, bh = 64.0f;
        const ArenaPic *pic = menu_sel < ARENA_MAX && arena_pic_state[menu_sel] > 0 ? &arena_pics[menu_sel] : NULL;
        if (pic && m_ttf) {
            float pw = fminf(bx1 - bx0, bh * (float)pic->w / (float)pic->h), ph = pw * (float)pic->h / (float)pic->w;
            float pxo = (bx0 + bx1 - pw) / 2.0f;
            draw_arena_pic(pic, pxo, by0, pw, ph);
            m_frame(pxo, by0, pxo + pw, by0 + ph, 1.0f, C_RULE, 110);
        } else {
            m_box(bx0, by0, bx1, by0 + bh, 3.0f, 0x0C1440, 0x0C1440, 255);
            m_wrap(bx0 + 6.0f, by0 + bh / 2.0f - 2.0f, bx1 - bx0 - 12.0f, 6.0f, 2, 3.5f,
                   in_launcher ? "Its picture is gathered from your ROM when you start your first fight here."
                               : "No picture yet.", C_DIM, 2);
        }
    }
    float fw = 18.0f, px = x0 + 8.0f, fy = top + 93.0f;
    for (int k = 0; k < b->n; k++) {
        int id = b->party[k].id;
        const UiImage *im = arena_faces && m_ttf ? ui_portrait(id) : NULL;
        m_box(px, fy, px + fw, fy + fw, 1.5f, 0x0C1440, 0x0C1440, 255);
        if (im) ui_image(&m_cv, im, px * m_S, fy * m_S, fw * m_S, 0, 255);
        m_text(px + fw / 2.0f, fy + fw + 5.5f, 3.4f, UI_CHAR_NAMES[id], C_TEXT, 2);
        snprintf(t, sizeof t, "LV %d", b->party[k].level);
        m_text(px + fw / 2.0f, fy + fw + 10.5f, 3.2f, t, C_VALUE, 2);
        px += fw + 7.0f;
    }
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* a macro turn's command, in words */
static void prep_cmd_text(const uint8_t *e, char *out, size_t n) {
    switch (e[1]) {
    case 1: snprintf(out, n, "Attack"); break;
    case 2: snprintf(out, n, "Tech: %s", ui_tech_name(e[2])); break;
    case 3: snprintf(out, n, "Skill: %s", ui_skill_name(e[2])); break;
    case 4: snprintf(out, n, "Item: %s", ui_item_name(e[2])); break;
    case 5: snprintf(out, n, "Defend"); break;
    default: snprintf(out, n, "-"); break;
    }
}

/* Prepare: the boss's macros A-H for this fight -- each turn who acts and what they do, in order */
static void draw_arena_prep(float W, float H, const char *hint) {
    const float pad = 10.0f;
    const ArenaBoss *ab = prep_src();
    char t[128];
    if (!ab) return;
    float sz = 6.0f;
    snprintf(t, sizeof t, "%s", ab->name);
    while (sz > 4.4f && m_width(t, sz) > W - 120.0f) sz -= 0.2f;
    m_text(pad + 2.0f, 17.0f, sz, t, C_GOLD, 0);
    m_text(pad + 4.0f + m_width(t, sz) + 4.0f, 16.6f, 4.3f, "Macros", C_SOFT, 0);
    draw_back_button(W, pad);
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    /* the macro bar */
    float bx = pad, by0 = 30.0f, by1 = 42.0f, tw = 15.0f;
    for (int s = 0; s < 8; s++) {
        float x0 = bx + (tw + 3.0f) * (float)s, x1 = x0 + tw;
        int on = s == prep_slot, cur = on && prep_row == -1, empty = macro_empty(prep_m, s);
        if (cur) m_box(x0, by0, x1, by1, 4.5f, C_GOLD, C_GOLD_LOW, 255);
        else if (on) m_box(x0, by0, x1, by1, 4.5f, 0x3A4C8E, 0x2A3870, 255);
        else m_box(x0, by0, x1, by1, 4.5f, 0x1E2A5C, 0x18214A, 255);
        char L2[2] = {(char)('A' + s), 0};
        m_text((x0 + x1) / 2.0f, by0 + 8.6f, 4.8f, L2, cur ? C_INK : empty ? C_DIM : C_TEXT, 2);
        if (!empty) m_box((x0 + x1) / 2.0f - 1.2f, by1 + 1.2f, (x0 + x1) / 2.0f + 1.2f, by1 + 3.6f, 1.2f, 0x7CDC8E, 0x7CDC8E, 255);
        m_hit(x0, by0, x1, by1, HIT_ROW, 100 + s);
    }
    snprintf(t, sizeof t, "%c-Macro%s", 'A' + prep_slot, macro_empty(prep_m, prep_slot) ? ": empty" : "");
    m_text(bx + (tw + 3.0f) * 8.0f + 6.0f, by0 + 8.4f, 4.2f, t, C_SOFT, 0);
    /* the turns */
    const uint8_t *m = prep_m + prep_slot * 20;
    int empty = macro_empty(prep_m, prep_slot);
    float ry = 48.0f, rh = 15.0f, cx0 = 120.0f, cx1 = W - pad;
    for (int r = 0; r < ab->n; r++) {
        float a = ry + rh * (float)r, b2 = a + rh - 2.0f, mid = (a + b2) / 2.0f;
        int sel = r == prep_row, grab = r == prep_grab;
        const uint8_t *e = m + 4 * r;
        int id = empty ? ab->party[r].id : e[0];
        if (grab) m_box(pad, a, W - pad, b2, 6.0f, 0x3A4C8E, 0x2A3870, 255);
        else if (sel) m_box(pad, a, W - pad, b2, 6.0f, C_GOLD, C_GOLD_LOW, 255);
        if (grab) m_frame(pad, a, W - pad, b2, 6.0f, C_GOLD, 220);
        snprintf(t, sizeof t, "%d", r + 1);
        m_text(pad + 9.0f, mid + 1.6f, 4.0f, t, sel && !grab ? C_INK_SOFT : C_DIM, 1);
        const UiImage *im = arena_faces && m_ttf && id < 11 ? ui_portrait(id) : NULL;
        float fs = 11.0f;
        m_box(pad + 13.0f, mid - fs / 2.0f, pad + 13.0f + fs, mid + fs / 2.0f, 1.2f, 0x0C1440, 0x0C1440, 255);
        if (im) ui_image(&m_cv, im, (pad + 13.0f) * m_S, (mid - fs / 2.0f) * m_S, fs * m_S, 0, 255);
        m_text(pad + 28.0f, mid + 1.8f, 4.6f, id < 11 ? UI_CHAR_NAMES[id] : "?", sel && !grab ? C_INK : C_TEXT, 0);
        char cost[24] = "";
        if (empty) snprintf(t, sizeof t, "-");
        else { prep_cmd_text(e, t, sizeof t); prep_cost(ab, party_index(ab, e[0]), e[1], e[2], cost, sizeof cost); }
        uint32_t ink = sel && !grab ? C_INK : C_VALUE;
        m_arrow(cx0 + 4.0f, mid, 3.0f, 3, sel && !grab ? C_INK_SOFT : C_DIM);
        m_arrow(cx1 - 6.0f, mid, 3.0f, 2, sel && !grab ? C_INK_SOFT : C_DIM);
        float csz = 4.4f;
        while (csz > 3.4f && m_width(t, csz) > cx1 - cx0 - 60.0f) csz -= 0.2f;
        m_text((cx0 + cx1) / 2.0f - 8.0f, mid + 1.6f, csz, t, ink, 2);
        if (cost[0]) m_text(cx1 - 12.0f, mid + 1.5f, 3.6f, cost, sel && !grab ? C_INK_SOFT : C_SOFT, 1);
        m_hit(pad, a, cx1, b2, HIT_ROW, 200 + r);    /* a click: the list of what they can do */
    }
    /* the buttons */
    const char *BTN[4] = {prep_boss == PREP_WAVE ? "Done" : "Fight", "Templates", "Copy", "Clear macro"};
    float y0 = ry + rh * 5.0f + 4.0f, y1 = y0 + 14.0f, x = pad;
    for (int k = 0; k < 4; k++) {
        float w = m_width(BTN[k], 4.8f) + 22.0f;
        int on = prep_row == ab->n && prep_btn == k;
        if (on) m_box(x, y0, x + w, y1, 7.0f, C_GOLD, C_GOLD_LOW, 255);
        else m_box(x, y0, x + w, y1, 7.0f, k == 0 ? 0x3A5CA8 : 0x26346A, k == 0 ? 0x2A4486 : 0x18214A, 255);
        m_text(x + w / 2.0f, y0 + 9.4f, 4.8f, BTN[k], on ? C_INK : C_TEXT, 2);
        m_hit(x, y0, x + w, y1, HIT_ROW, 400 + k);
        x += w + 5.0f;
    }
    char what[160] = "";
    if (prep_row >= 0 && prep_row < ab->n && !empty) {   /* the turn under the cursor: what it does */
        const uint8_t *e = m + 4 * prep_row;
        char nm[64], d[110];
        prep_cmd_text(e, nm, sizeof nm);
        prep_describe(e[1], e[2], d, sizeof d);
        snprintf(what, sizeof what, "%s: %s", nm, d);
    }
    int noted = prep_note[0] && SDL_GetTicks() < prep_note_until;
    const char *note = noted ? prep_note
        : prep_copy >= 0 ? "Copy: choose the letter to copy to"
        : what[0] ? what
        : prep_boss == PREP_WAVE ? "These macros are locked for the whole run once it starts."
        : "Kept for a rematch when you fight; macros set in battle are kept too.";
    float nsz = 3.8f;
    while (nsz > 3.0f && m_width(note, nsz) > W - 2.0f * pad - 4.0f) nsz -= 0.1f;
    m_text(pad + 2.0f, y1 + 11.0f, nsz, note, noted ? 0xA8F0B4 : prep_copy >= 0 ? C_GOLD : what[0] ? C_VALUE : C_DIM, 0);
    if (prep_pick >= 0 && prep_pick < ab->n) {       /* the pick list, over the turns */
        int k = party_index(ab, m[4 * prep_pick]);
        float lx0 = 112.0f, lx1 = W - pad, ly0 = 28.0f, rowh = 11.0f, ly1 = ly0 + 12.0f + rowh * PICK_ROWS + 16.0f;
        m_box(lx0 - 2.0f, ly0 - 2.0f, lx1 + 2.0f, ly1 + 2.0f, 7.0f, 0x05081C, 0x05081C, 150);
        m_box(lx0, ly0, lx1, ly1, 6.0f, 0x2A3A7C, 0x1C285C, 255);
        m_frame(lx0, ly0, lx1, ly1, 6.0f, C_GOLD, 200);
        snprintf(t, sizeof t, "%s's turn %d", k >= 0 ? UI_CHAR_NAMES[ab->party[k].id] : "?", prep_pick + 1);
        m_text(lx0 + 7.0f, ly0 + 8.0f, 4.2f, t, C_GOLD, 0);
        snprintf(t, sizeof t, "%d of %d", prep_pick_sel + 1, prep_pick_n);
        m_text(lx1 - 7.0f, ly0 + 8.0f, 3.4f, t, C_DIM, 1);
        for (int r = 0; r < PICK_ROWS && prep_pick_top + r < prep_pick_n; r++) {
            int q = prep_pick_top + r, v = prep_pick_opt[q], on = q == prep_pick_sel;
            float a = ly0 + 12.0f + rowh * (float)r, b2 = a + rowh - 1.0f, mid = (a + b2) / 2.0f;
            if (on) m_box(lx0 + 3.0f, a, lx1 - 3.0f, b2, 4.5f, C_GOLD, C_GOLD_LOW, 255);
            char nm[64] = "", cost[24] = "";
            if (v == PICK_MOVE) snprintf(nm, sizeof nm, "Move this turn up or down");
            else {
                uint8_t e2[4] = {0, (uint8_t)(v >> 8), (uint8_t)(v & 0xFF), 0};
                prep_cmd_text(e2, nm, sizeof nm);
                prep_cost(ab, k, v >> 8, v & 0xFF, cost, sizeof cost);
            }
            m_text(lx0 + 9.0f, mid + 1.5f, 4.0f, nm, on ? C_INK : v == PICK_MOVE ? C_SOFT : C_TEXT, 0);
            if (cost[0]) m_text(lx1 - 9.0f, mid + 1.5f, 3.6f, cost, on ? C_INK_SOFT : C_SOFT, 1);
            m_hit(lx0 + 3.0f, a, lx1 - 3.0f, b2, HIT_ROW, 600 + q);
        }
        float mx = (lx0 + lx1) / 2.0f;                 /* more above, more below: arrows on the centre line */
        if (prep_pick_top > 0) { m_arrow(mx, ly0 + 7.0f, 2.6f, 1, C_SOFT); m_hit(mx - 8.0f, ly0, mx + 8.0f, ly0 + 12.0f, HIT_ROW, 700); }
        if (prep_pick_top + PICK_ROWS < prep_pick_n) {
            m_arrow(mx, ly1 - 12.5f, 2.6f, 0, C_SOFT);
            m_hit(mx - 8.0f, ly1 - 16.0f, mx + 8.0f, ly1 - 9.0f, HIT_ROW, 701);
        }
        int v = prep_pick_opt[prep_pick_sel];
        if (v == PICK_MOVE) snprintf(t, sizeof t, "Then Up / Down moves it through the order");
        else prep_describe(v >> 8, v & 0xFF, t, sizeof t);
        float dsz = 3.6f;
        while (dsz > 2.8f && m_width(t, dsz) > lx1 - lx0 - 14.0f) dsz -= 0.1f;
        m_text(lx0 + 7.0f, ly1 - 5.0f, dsz, t, C_VALUE, 0);
    }
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* the templates: eight sets of macros kept for any fight, each with Load and Save */
static void draw_arena_tpl(float W, float H, const char *hint) {
    const float pad = 10.0f;
    char t[160];
    m_text(pad + 2.0f, 17.0f, 6.0f, "Macro templates", C_GOLD, 0);
    draw_back_button(W, pad);
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    float ry = 30.0f, rh = 16.0f;
    for (int k = 0; k < TEMPLATES; k++) {
        float a = ry + rh * (float)k, b2 = a + rh - 2.0f, mid = (a + b2) / 2.0f;
        int sel = k == menu_sel;
        if (sel) m_box(pad, a, W - pad, b2, 6.0f, 0x3A4C8E, 0x2A3870, 255);
        snprintf(t, sizeof t, "%d", k + 1);
        m_text(pad + 9.0f, mid + 1.6f, 4.2f, t, C_DIM, 1);
        if (!tpl_set[k]) snprintf(t, sizeof t, "Empty");
        else {                                       /* which macros it has, and whose */
            int o = 0, seen[11] = {0};
            for (int s = 0; s < 8; s++) if (!macro_empty(tpl_macros[k], s)) o += snprintf(t + o, sizeof t - (size_t)o, "%c", 'A' + s);
            o += snprintf(t + o, sizeof t - (size_t)o, "   ");
            for (int s = 0; s < 8; s++)
                for (int j = 0; j < 5; j++) {
                    int id = tpl_macros[k][s * 20 + 4 * j];
                    if (tpl_macros[k][s * 20 + 4 * j + 1] && id < 11 && !seen[id] && o < (int)sizeof t - 12) {
                        seen[id] = 1;
                        o += snprintf(t + o, sizeof t - (size_t)o, "%s ", UI_CHAR_NAMES[id]);
                    }
                }
        }
        float tsz = 4.4f;
        while (tsz > 3.4f && m_width(t, tsz) > W - 2 * pad - 90.0f) tsz -= 0.2f;
        m_text(pad + 16.0f, mid + 1.6f, tsz, t, tpl_set[k] ? C_TEXT : C_DIM, 0);
        static const char *const B2[2] = {"Load", "Save"};
        float x1 = W - pad - 3.0f;
        for (int q = 1; q >= 0; q--) {
            float w = m_width(B2[q], 4.2f) + 14.0f, x0 = x1 - w;
            int on = sel && tpl_btn == q, can = q == 1 || tpl_set[k];
            if (on) m_box(x0, a + 1.5f, x1, b2 - 1.5f, 5.0f, C_GOLD, C_GOLD_LOW, 255);
            else m_box(x0, a + 1.5f, x1, b2 - 1.5f, 5.0f, 0x26346A, 0x18214A, 255);
            m_text((x0 + x1) / 2.0f, mid + 1.6f, 4.2f, B2[q], on ? C_INK : can ? C_TEXT : C_DIM, 2);
            m_hit(x0, a, x1, b2, HIT_ROW, 500 + k * 2 + q);
            x1 = x0 - 4.0f;
        }
    }
    m_text(pad + 2.0f, H - 15.0f, 3.5f, "A template fits any fight: each turn is matched by character, and what one can't do there becomes an attack.", C_SOFT, 0);
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}


/* ------------------------------------------------------------------ Wave Mode's screens */
/* the run's kit, in words: "Monomate x6, Dimate x2, ..." */
static void wave_kit_text(const WaveRun *w, char *out, size_t n) {
    int o = 0, done[256] = {0};
    out[0] = 0;
    for (int j = 0; j < 40 && o < (int)n - 24; j++) {
        int it = w->items[j], c = 0;
        if (!it || done[it]) continue;
        done[it] = 1;
        for (int q = 0; q < 40; q++) c += w->items[q] == it;
        o += snprintf(out + o, n - (size_t)o, "%s%s x%d", o ? ", " : "", ui_item_name(it), c);
    }
}
static const char *wave_dot(void) { return m_ttf ? "   \xC2\xB7   " : "  -  "; }

static void draw_wave(float W, float H, const char *hint) {
    const float pad = 10.0f;
    char t[200];
    int n = wave_count();
    draw_mode_tabs(pad, 1);
    draw_back_button(W, pad);
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    float top = 30.0f, row = 15.0f, lx0 = pad, lx1 = 126.0f, bottom = top + row * ARENA_ROWS - 2.0f;
    for (int i = 0; i < n; i++) {
        const WaveRun *w = wave_run(i);
        int sel = i == menu_sel;
        float a = top + row * (float)i, b = a + row - 2.0f, mid = (a + b) / 2.0f;
        if (sel) m_box(lx0, a, lx1, b, 6.5f, C_GOLD, C_GOLD_LOW, 255);
        m_text(lx0 + 6.0f, mid + 1.7f, 4.6f, w->name, sel ? C_INK : C_TEXT, 0);
        snprintf(t, sizeof t, "LV %d", w->level);
        m_text(lx1 - 13.0f, mid + 1.5f, 3.6f, t, sel ? C_INK_SOFT : C_DIM, 1);
        if (i < WAVE_RUNS_MAX && wave_best[i]) {     /* cleared: a mark */
            uint32_t c = sel ? 0x2E7A3C : 0x7CDC8E;
            m_box(lx1 - 9.5f, mid - 2.0f, lx1 - 5.5f, mid + 2.0f, 2.0f, c, c, 255);
        }
        m_hit(lx0, a, lx1, b, HIT_ROW, i);
    }
    m_wrap(lx0 + 4.0f, top + row * (float)n + 9.0f, lx1 - lx0 - 8.0f, 5.6f, 6, 3.4f,
           "Five battles in a row. The party and its macros stay the same for all five; after each win the "
           "party gets some HP and TP back, and anyone who fell gets up. The clock runs only in battle.", C_DIM, 0);
    const WaveRun *w = wave_run(menu_sel);
    float x0 = lx1 + 14.0f, x1 = W - pad;
    m_box(x0, top, x1, bottom, 7.0f, 0x243470, 0x1A2658, 255);
    if (!w) { m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0); return; }
    m_text(x0 + 8.0f, top + 12.0f, 6.0f, w->name, C_GOLD, 0);
    snprintf(t, sizeof t, "Level %d%s%d battles%s%d to pick from", w->level, wave_dot(), WAVE_NODES, wave_dot(), w->nroster);
    m_text(x0 + 8.0f, top + 20.5f, 3.6f, t, C_SOFT, 0);
    float y = top + 31.0f;
    for (int k = 0; k < WAVE_NODES; k++) {
        const WaveNode *nd = &w->node[k];
        snprintf(t, sizeof t, "%d", k + 1);
        m_text(x0 + 13.0f, y, 4.0f, t, C_GOLD, 1);
        m_text(x0 + 17.0f, y, 4.0f, nd->place, C_TEXT, 0);
        m_text(x0 + 17.0f, y + 5.6f, 3.4f, nd->enemies, C_VALUE, 0);
        y += 13.4f;
    }
    wave_kit_text(w, t, sizeof t);
    float ksz = 3.4f;
    char kit[220];
    snprintf(kit, sizeof kit, "Items: %s", t);
    while (ksz > 2.8f && m_width(kit, ksz) > x1 - x0 - 16.0f) ksz -= 0.1f;
    m_text(x0 + 8.0f, y + 1.5f, ksz, kit, C_SOFT, 0);
    if (menu_sel < WAVE_RUNS_MAX && wave_best[menu_sel]) {
        char tm[32];
        wave_time_text(wave_best[menu_sel], tm, sizeof tm);
        int o = snprintf(t, sizeof t, "Best %s%s", tm, wave_dot());
        for (int k = 0; k < wave_best_n[menu_sel]; k++)
            o += snprintf(t + o, sizeof t - (size_t)o, "%s%s", k ? " " : "", wave_best_ids[menu_sel][k] < 11 ? UI_CHAR_NAMES[wave_best_ids[menu_sel][k]] : "?");
        float bsz = 3.6f;
        while (bsz > 2.8f && m_width(t, bsz) > x1 - x0 - 16.0f) bsz -= 0.1f;
        m_text(x0 + 8.0f, bottom - 6.0f, bsz, t, 0xA8F0B4, 0);
    } else m_text(x0 + 8.0f, bottom - 6.0f, 3.6f, "Not cleared yet", C_DIM, 0);
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* a run's party: the roster in two rows of five (a number on each one picked: their place in the
 * order), then Macros and Start */
static void draw_wave_setup(float W, float H, const char *hint) {
    const float pad = 10.0f;
    const WaveRun *w = wave_run(wave_sel_run);
    char t[220];
    if (!w) return;
    m_text(pad + 2.0f, 17.0f, 6.0f, w->name, C_GOLD, 0);
    m_text(pad + 6.0f + m_width(w->name, 6.0f), 16.6f, 4.3f, "Party", C_SOFT, 0);
    draw_back_button(W, pad);
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    float cw = (W - 2.0f * pad) / 5.0f, ch = 40.0f, gy = 30.0f;
    for (int k = 0; k < w->nroster; k++) {
        const ArenaMember *m = &w->roster[k];
        float a = pad + cw * (float)(k % 5), b = gy + (ch + 3.0f) * (float)(k / 5);
        int sel = menu_sel == k, order = -1;
        for (int j = 0; j < wave_npicks; j++) if (wave_picks[j] == k) order = j;
        if (sel) m_box(a + 1.0f, b, a + cw - 1.0f, b + ch, 6.0f, C_GOLD, C_GOLD_LOW, 255);
        else if (order >= 0) m_box(a + 1.0f, b, a + cw - 1.0f, b + ch, 6.0f, 0x3A4C8E, 0x2A3870, 255);
        else m_box(a + 1.0f, b, a + cw - 1.0f, b + ch, 6.0f, 0x1E2A5C, 0x18214A, 255);
        float fs = 18.0f, fx = a + (cw - fs) / 2.0f, fy = b + 3.0f;
        const UiImage *im = arena_faces && m_ttf && m->id < 11 ? ui_portrait(m->id) : NULL;
        m_box(fx, fy, fx + fs, fy + fs, 1.5f, 0x0C1440, 0x0C1440, 255);
        if (im) ui_image(&m_cv, im, fx * m_S, fy * m_S, fs * m_S, 0, order >= 0 || sel ? 255 : 150);
        m_text(a + cw / 2.0f, fy + fs + 6.5f, 4.0f, m->id < 11 ? UI_CHAR_NAMES[m->id] : "?", sel ? C_INK : C_TEXT, 2);
        snprintf(t, sizeof t, "LV %d", m->level);
        m_text(a + cw / 2.0f, fy + fs + 12.0f, 3.2f, t, sel ? C_INK_SOFT : C_VALUE, 2);
        if (order >= 0) {                            /* its place in the order */
            float bx = a + cw - 10.0f, by = b + 3.0f;
            m_box(bx, by, bx + 7.0f, by + 7.0f, 3.5f, 0x7CDC8E, 0x4CAC5E, 255);
            snprintf(t, sizeof t, "%d", order + 1);
            m_text(bx + 3.5f, by + 5.4f, 3.6f, t, 0x0C2A14, 2);
        }
        m_hit(a, b, a + cw, b + ch, HIT_ROW, k);
    }
    float y = gy + (ch + 3.0f) * 2.0f + 6.0f;
    int o = snprintf(t, sizeof t, "Party: ");
    if (!wave_npicks) snprintf(t + o, sizeof t - (size_t)o, "none yet (up to five)");
    for (int j = 0; j < wave_npicks; j++)
        o += snprintf(t + o, sizeof t - (size_t)o, "%s%s", j ? ", " : "", UI_CHAR_NAMES[w->roster[wave_picks[j]].id]);
    m_text(pad + 2.0f, y, 4.2f, t, wave_npicks ? C_TEXT : C_DIM, 0);
    {
        char kit[200];
        wave_kit_text(w, kit, sizeof kit);
        snprintf(t, sizeof t, "Items: %s", kit);
        float ksz = 3.5f;
        while (ksz > 2.8f && m_width(t, ksz) > W - 2.0f * pad) ksz -= 0.1f;
        m_text(pad + 2.0f, y + 7.5f, ksz, t, C_SOFT, 0);
    }
    static const char *const BTN[2] = {"Macros", "Start run"};
    float by0 = y + 12.5f, by1 = by0 + 14.0f, x = pad;
    for (int k = 0; k < 2; k++) {
        float bw = m_width(BTN[k], 4.8f) + 22.0f;
        int on = menu_sel >= w->nroster && wave_setup_btn == k;
        if (on) m_box(x, by0, x + bw, by1, 7.0f, C_GOLD, C_GOLD_LOW, 255);
        else m_box(x, by0, x + bw, by1, 7.0f, k == 1 ? 0x3A5CA8 : 0x26346A, k == 1 ? 0x2A4486 : 0x18214A, 255);
        m_text(x + bw / 2.0f, by0 + 9.4f, 4.8f, BTN[k], on ? C_INK : C_TEXT, 2);
        m_hit(x, by0, x + bw, by1, HIT_ROW, 100 + k);
        x += bw + 5.0f;
    }
    {   /* the macros set: which, in a word */
        uint8_t m[ARENA_MACROS];
        macros_fit(&wave_prep_src, wave_m, m);
        int o2 = snprintf(t, sizeof t, "Macros ");
        int any = 0;
        for (int s2 = 0; s2 < 8; s2++) if (!macro_empty(m, s2)) { o2 += snprintf(t + o2, sizeof t - (size_t)o2, "%c", 'A' + s2); any = 1; }
        if (!any) snprintf(t, sizeof t, "No macros: A will be everyone attacking");
        else snprintf(t + o2, sizeof t - (size_t)o2, " set");
        m_text(x + 4.0f, by0 + 9.0f, 3.6f, t, C_SOFT, 0);
    }
    const char *note = prep_note[0] && SDL_GetTicks() < prep_note_until ? prep_note
        : "Once the run starts, the party and its macros can't be changed.";
    m_text(pad + 2.0f, by1 + 8.5f, 3.6f, note, prep_note[0] && SDL_GetTicks() < prep_note_until ? 0xF4D08A : C_DIM, 0);
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

/* how the run went: cleared (the time; the quickest yet?) or lost (where), the party with who never fell */
static void draw_wave_result(float W, float H, const char *hint) {
    const float pad = 10.0f;
    const WaveRun *w = wave_run(wave_result_run);
    char t[200], tm[32];
    if (!w) return;
    int won = wave_result == WAVE_CLEARED;
    m_text(pad + 2.0f, 17.0f, 6.4f, won ? "Run cleared" : "Run over", won ? 0xA8F0B4 : 0xF4A0A0, 0);
    m_text(pad + 6.0f + m_width(won ? "Run cleared" : "Run over", 6.4f), 16.6f, 4.3f, w->name, C_SOFT, 0);
    draw_back_button(W, pad);
    m_box(pad, 25.5f, W - pad, 26.0f, 0, C_RULE, C_RULE, 80);
    wave_time_text(wave_result_frames, tm, sizeof tm);
    m_text(W / 2.0f, 50.0f, 11.0f, tm, C_GOLD, 2);
    if (won) {
        if (wave_result_best) snprintf(t, sizeof t, "Time in battle%sa new best", wave_dot());
        else {
            char bt[32];
            wave_time_text(wave_best[wave_result_run], bt, sizeof bt);
            snprintf(t, sizeof t, "Time in battle%sbest %s", wave_dot(), bt);
        }
    } else {
        const WaveNode *nd = &w->node[wave_result_node < WAVE_NODES ? wave_result_node : WAVE_NODES - 1];
        snprintf(t, sizeof t, "Defeated in battle %d of %d, at %s", wave_result_node + 1, WAVE_NODES, nd->place);
    }
    m_text(W / 2.0f, 61.0f, 4.2f, t, won && wave_result_best ? 0xA8F0B4 : C_SOFT, 2);
    float fw = 24.0f, gap = 14.0f, tot = (float)wave_result_n * fw + (float)(wave_result_n - 1) * gap, px = (W - tot) / 2.0f, fy = 74.0f;
    int flawless = 0;
    for (int k = 0; k < wave_result_n; k++) {
        int id = wave_result_ids[k], fell = wave_result_fell >> k & 1;
        const UiImage *im = arena_faces && m_ttf && id < 11 ? ui_portrait(id) : NULL;
        m_box(px, fy, px + fw, fy + fw, 2.0f, 0x0C1440, 0x0C1440, 255);
        if (im) ui_image(&m_cv, im, px * m_S, fy * m_S, fw * m_S, 0, fell ? 140 : 255);
        if (!fell && won) m_frame(px - 1.0f, fy - 1.0f, px + fw + 1.0f, fy + fw + 1.0f, 2.5f, 0x7CDC8E, 255);
        m_text(px + fw / 2.0f, fy + fw + 6.5f, 4.0f, id < 11 ? UI_CHAR_NAMES[id] : "?", C_TEXT, 2);
        m_text(px + fw / 2.0f, fy + fw + 12.5f, 3.4f, fell ? "Fell" : won ? "Flawless" : "Standing",
               fell ? C_DIM : won ? 0xA8F0B4 : C_VALUE, 2);
        flawless += !fell;
        px += fw + gap;
    }
    if (won) {
        snprintf(t, sizeof t, flawless == wave_result_n ? "Nobody fell: a flawless run" :
                 flawless ? "Flawless: those who never fell in the run" : "Everyone fell at least once");
        m_text(W / 2.0f, fy + fw + 24.0f, 3.6f, t, C_DIM, 2);
    }
    static const char *const BTN[2] = {"Run again", "Wave Mode"};
    float by0 = 146.0f, by1 = by0 + 14.0f, bw[2], x;
    for (int k = 0; k < 2; k++) bw[k] = m_width(BTN[k], 4.8f) + 22.0f;
    x = (W - bw[0] - bw[1] - 6.0f) / 2.0f;
    for (int k = 0; k < 2; k++) {
        int on = wave_result_btn == k;
        if (on) m_box(x, by0, x + bw[k], by1, 7.0f, C_GOLD, C_GOLD_LOW, 255);
        else m_box(x, by0, x + bw[k], by1, 7.0f, 0x26346A, 0x18214A, 255);
        m_text(x + bw[k] / 2.0f, by0 + 9.4f, 4.8f, BTN[k], on ? C_INK : C_TEXT, 2);
        m_hit(x, by0, x + bw[k], by1, HIT_ROW, 100 + k);
        x += bw[k] + 6.0f;
    }
    m_text(pad + 2.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
}

static void wave_menu_tap(int kind, int index) {
    if (kind == HIT_PAGE) { if (index == 0) arena_enter(); else wave_enter(); return; }
    if (menu_screen == MENU_WAVE) { menu_sel = index; wave_setup_enter(index); return; }
    if (menu_screen == MENU_WAVE_RESULT) { wave_result_btn = index - 100; wave_menu_input(0, 0, 0, 0, 1); return; }
    const WaveRun *w = wave_run(wave_sel_run);
    if (!w) return;
    if (index < 100) { menu_sel = index; wave_toggle(index); }
    else { menu_sel = w->nroster; wave_setup_btn = index - 100; wave_menu_input(0, 0, 0, 0, 1); }
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
    if (menu_screen == MENU_ARENA_PREP) {
        draw_arena_prep(W, H, hint);
    } else if (menu_screen == MENU_ARENA_TPL) {
        draw_arena_tpl(W, H, hint);
    } else if (menu_screen == MENU_ARENA) {
        draw_arena(W, H, hint);
    } else if (menu_screen == MENU_BONUS) {
        draw_bonus(W, H, hint);
    } else if (menu_screen == MENU_JUKEBOX) {
        draw_jukebox(W, H, hint);
    } else if (menu_screen == MENU_WAVE) {
        draw_wave(W, H, hint);
    } else if (menu_screen == MENU_WAVE_SETUP) {
        draw_wave_setup(W, H, hint);
    } else if (menu_screen == MENU_WAVE_RESULT) {
        draw_wave_result(W, H, hint);
    } else if (menu_screen == MENU_LOAD) {
        draw_load(W, H, hint);
    } else if (menu_screen == MENU_SETTINGS) {
        draw_settings(W, H, hint);
    } else if (menu_screen == MENU_JOURNAL) {
        draw_journal(W, H, hint);
    } else if (menu_screen == MENU_MAIN) {
        m_text(12.0f, 17.0f, 6.4f, "Paused", C_GOLD, 0);
        rows_off = arena_mode ? 1u << M_REWIND : 0;
        draw_rows(W, main_labels(), N_MAIN, 26.0f);
        rows_off = 0;
        m_text(12.0f, H - 15.0f, 3.4f, hint, C_HINT, 0);
        m_text(12.0f, H - 6.5f, 3.1f, APP_NAME " " PSIV_VERSION, C_DIM, 0);   /* for bug reports */
    } else {
        static const char *const ANSWERS[2] = {"Yes, quit", "No, keep playing"};
        static const char *const TO_MENU[2] = {"Yes, to the menu", "No, keep playing"};
        m_text(12.0f, 17.0f, 6.4f, quit_to_menu ? "Return to the menu?" : "Quit the game?", C_GOLD, 0);
        draw_rows(W, quit_to_menu ? TO_MENU : ANSWERS, 2, 26.0f);
        m_wrap(12.0f, 67.5f, W - 24.0f, 7.5f, 2, 3.8f, "Anything since you last saved in the game is lost.", C_VALUE, 0);
        m_text(12.0f, H - 6.5f, 3.4f, hint, C_HINT, 0);
    }
}

/* the Prepare screen's macros and its note, for the signature */
static uint64_t prep_sig(void) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < ARENA_MACROS; i++) h = (h ^ prep_m[i]) * 1099511628211ull;
    for (int k = 0; k < TEMPLATES; k++) h = (h ^ (uint64_t)tpl_set[k]) * 1099511628211ull;
    h ^= (uint64_t)(prep_note[0] && SDL_GetTicks() < prep_note_until);
    h = (h ^ (uint64_t)(prep_pick + 2)) * 1099511628211ull;
    h = (h ^ (uint64_t)prep_pick_sel) * 1099511628211ull;
    h = (h ^ (uint64_t)prep_pick_top) * 1099511628211ull;
    h = (h ^ (uint64_t)(prep_copy + 2)) * 1099511628211ull;
    return h;
}

/* what's on the panel: when this changes, it's drawn again */
static uint64_t menu_signature(int ow, int oh) {
    uint64_t parts[] = {(uint64_t)menu_screen, (uint64_t)(menu_sel + 1), (uint64_t)menu_page, (uint64_t)ow,
                        (uint64_t)oh, (uint64_t)touch_visible(), (uint64_t)(menu_last_pad && pad),
                        pad ? (uint64_t)SDL_GetGamepadID(pad) : 0, (uint64_t)rom_ready, (uint64_t)(rom_problem[0] != 0),
                        (uint64_t)j_tab, (uint64_t)j_scroll, (uint64_t)(j_hint_step + 2), (uint64_t)(j_hint_job + 1),
                        journal_signature(), (uint64_t)scenevox_pages(), (uint64_t)load_page,
                        (uint64_t)arena_scroll, (uint64_t)(arena_result + 2), (uint64_t)(arena_result_boss + 2),
                        (uint64_t)arena_faces, (uint64_t)arena_mode, arena_anim_sig(),
                        (uint64_t)(menu_sel >= 0 && menu_sel < ARENA_MAX ? arena_pic_state[menu_sel] + 3 : 0),
                        (uint64_t)(prep_boss + 2), (uint64_t)prep_slot, (uint64_t)(prep_row + 2), (uint64_t)prep_btn,
                        (uint64_t)(prep_grab + 2), (uint64_t)tpl_btn, prep_sig(), wave_menu_sig(),
                        (uint64_t)jukebox_song, (uint64_t)jukebox_top, (uint64_t)in_launcher, (uint64_t)(jukebox_src + 2),
                        (uint64_t)jukebox_files, (uint64_t)settings_top, (uint64_t)launcher_song(), previews_sig()};
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
        int settings = menu_screen == MENU_SETTINGS, load = menu_screen == MENU_LOAD || menu_screen == MENU_ARENA
                   || menu_screen == MENU_ARENA_PREP || menu_screen == MENU_ARENA_TPL || menu_screen == MENU_WAVE
                   || menu_screen == MENU_WAVE_SETUP || menu_screen == MENU_WAVE_RESULT || menu_screen == MENU_BONUS
                   || menu_screen == MENU_JUKEBOX;
        int big = settings || load || menu_screen == MENU_JOURNAL;
        W = big ? 288.0f : menu_screen == MENU_QUIT ? 176.0f : 168.0f;
        H = settings ? SETTINGS_H : load ? LOAD_H : big ? 168.0f : menu_screen == MENU_QUIT ? 86.0f
          : 26.0f + 16.0f * N_MAIN + 26.0f;
        /* about the game's own scale, small enough that Settings (the
         * tallest) always fits; the other menus keep the same scale */
        m_S = fminf(fminf((float)ow / 320.0f, (float)oh / 224.0f),
                    fminf((float)ow * 0.95f / 288.0f, (float)oh * 0.94f / SETTINGS_H));
        if (settings) {                              /* Settings: wider where the window is (its previews bigger) */
            float room = (float)ow * 0.95f / m_S;
            W = room > SETTINGS_W_MAX ? SETTINGS_W_MAX : room < 288.0f ? 288.0f : floorf(room);
        }
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
        previews_render();
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
    previews_render();                               /* the settings' previews, moving */
}

/* the mouse wheel over the menu: the lists that scroll (the Prepare screen's pick list, the
 * bosses), a row a notch; a trackpad's small steps add up */
static void menu_wheel(float y) {
    static float acc;
    acc += y;
    int steps = (int)acc;
    if (!steps) return;
    acc -= (float)steps;
    if (menu_screen == MENU_ARENA_PREP && prep_pick >= 0) {
        int most = prep_pick_n > PICK_ROWS ? prep_pick_n - PICK_ROWS : 0;
        prep_pick_top -= steps;                      /* (up: the list's top comes back) */
        if (prep_pick_top < 0) prep_pick_top = 0;
        if (prep_pick_top > most) prep_pick_top = most;
        if (prep_pick_sel < prep_pick_top) prep_pick_sel = prep_pick_top;   /* the choice stays in view */
        if (prep_pick_sel >= prep_pick_top + PICK_ROWS) prep_pick_sel = prep_pick_top + PICK_ROWS - 1;
    } else if (menu_screen == MENU_JUKEBOX) {
        jukebox_scroll(-steps);
    } else if (menu_screen == MENU_SETTINGS && page_now()->n > settings_rows()) {
        settings_scroll(-steps);
    } else if (menu_screen == MENU_ARENA) {
        int most = arena_count() > ARENA_ROWS ? arena_count() - ARENA_ROWS : 0;
        arena_scroll -= steps;
        if (arena_scroll < 0) arena_scroll = 0;
        if (arena_scroll > most) arena_scroll = most;
    }
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
        } else if (h->kind == HIT_MUSIC) {          /* the launcher's note */
            launcher_music_toggle();
        } else if (h->kind == HIT_DISCORD) {        /* and its Discord button */
            open_discord();
        } else if (menu_screen == MENU_JOURNAL) {   /* a page, the hint, the arrows, a job */
            if (h->kind == HIT_JTAB) { j_tab = h->index; j_scroll = 0; menu_sel = 0; }
            else if (h->kind == HIT_JHINT) journal_input(0, 0, 0, 0, 1);
            else if (h->kind == HIT_JUP) journal_input(1, 0, 0, 0, 0);
            else if (h->kind == HIT_JDOWN) journal_input(0, 1, 0, 0, 0);
            else if (h->kind == HIT_ROW) menu_sel = h->index;
        } else if (menu_screen == MENU_ARENA_PREP) {   /* a macro, a turn (its left half back), a button */
            const ArenaBoss *ab = prep_src();
            int ix = h->index;
            if (prep_pick >= 0 && (ix < 600 || ix > 701)) prep_pick = -1;   /* (a tap outside the list: it closes) */
            else if (ix >= 100 && ix < 108) { prep_slot = ix - 100; prep_row = -1; prep_grab = -1; if (prep_copy >= 0) prep_ok(); }
            else if (ix >= 200 && ix < 205) { prep_row = ix - 200; prep_ok(); }
            else if (ix >= 400 && ix < 404 && ab) { prep_row = ab->n; prep_btn = ix - 400; prep_ok(); }
            else if (ix >= 600 && ix < 700 && prep_pick >= 0) { prep_pick_sel = ix - 600; prep_pick_set(); }
            else if (ix == 700 && prep_pick >= 0) prep_pick_move(-PICK_ROWS);
            else if (ix == 701 && prep_pick >= 0) prep_pick_move(PICK_ROWS);
        } else if (menu_screen == MENU_ARENA_TPL) {    /* a template's Load or Save */
            if (h->index >= 500) { menu_sel = (h->index - 500) / 2; tpl_btn = (h->index - 500) % 2; tpl_ok(); }
        } else if (menu_screen == MENU_WAVE || menu_screen == MENU_WAVE_SETUP || menu_screen == MENU_WAVE_RESULT
                   || (menu_screen == MENU_ARENA && h->kind == HIT_PAGE)) {
            wave_menu_tap(h->kind, h->index);
        } else if (menu_screen == MENU_BONUS) {     /* a mode: open it */
            menu_sel = h->index;
            bonus_open(menu_sel);
        } else if (menu_screen == MENU_JUKEBOX && h->kind == HIT_MODE) {   /* the button: the highlighted song, the launcher's */
            jukebox_set_launcher(menu_sel);
        } else if (menu_screen == MENU_JUKEBOX) {   /* a song: play it, or stop it; the arrows scroll */
            if (h->kind == HIT_PAGE) { menu_sel = -1; if (h->index != JB_PACK || jukebox_files) jukebox_src = h->index; }
            else if (h->kind == HIT_ROW) { menu_sel = h->index; jukebox_pick(h->index); }
            else if (h->kind == HIT_JUP) jukebox_scroll(-1);
            else if (h->kind == HIT_JDOWN) jukebox_scroll(1);
        } else if (menu_screen == MENU_ARENA) {     /* a boss: fight it; the arrows scroll */
            if (h->kind == HIT_ROW) { menu_sel = h->index; arena_pick(); }
            else if (h->kind == HIT_JUP) { arena_scroll = arena_scroll > 0 ? arena_scroll - 1 : 0; }
            else if (h->kind == HIT_JDOWN) arena_scroll++;
        } else if (menu_screen == MENU_LOAD) {      /* a page's tab, or a save: play on from it */
            if (h->kind == HIT_PAGE) load_page = h->index;
            else if (h->kind == HIT_ROW) { menu_sel = h->index; load_pick(); }
        } else if (h->kind == HIT_MODE) {           /* the launcher's switch: that mode */
            menu_sel = L_MODE;
            if ((h->index == 1) != classic_mode()) item_change(MODE_ITEM, 1);
        } else if (h->kind == HIT_PAGE) {
            menu_page = h->index;
            menu_sel = page_now()->n ? 0 : -1;
        } else if (h->kind == HIT_SLIDER) {          /* a slider: its value where it was clicked; the mouse can drag on */
            menu_sel = h->index;
            slider_drag = h->index;
            slider_to(h, rx);
        } else if (menu_screen == MENU_SETTINGS && (h->kind == HIT_JUP || h->kind == HIT_JDOWN)) {
            settings_scroll(h->kind == HIT_JUP ? -1 : 1);   /* (a long page's arrows) */
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
/* (PSIV_AUDIO_LOG) the device's side: the sound it asked for, in the game's
 * bytes; the longest wait between two of its calls and how many came over
 * two and a half of its periods apart -- a device that asks late has played
 * a gap of its own */
static SDL_AtomicInt audio_taken, audio_call_gap_us, audio_calls_late;
static int audio_late_us = 25000;                       /* (set when the device opens) */
static void SDLCALL audio_requested(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)s;
    static uint64_t last;                                /* (the audio thread's own) */
    uint64_t t = SDL_GetTicksNS();
    if (last) {
        int us = (int)((t - last) / 1000);
        if (us > SDL_GetAtomicInt(&audio_call_gap_us)) SDL_SetAtomicInt(&audio_call_gap_us, us);
        if (us > audio_late_us) SDL_AddAtomicInt(&audio_calls_late, 1);
    }
    last = t;
    if (total > 0) SDL_AddAtomicInt(&audio_taken, total);
    if (additional <= 0) return;
    if ((int)SDL_GetTicks() - SDL_GetAtomicInt(&audio_last_push_ms) > 250) return;
    SDL_AddAtomicInt(&audio_underruns, 1);
    SDL_AddAtomicInt(&audio_missing, additional);
    SDL_SetAtomicInt(&audio_starved, 1);
}

/* The sound stops on purpose (a menu, the pause, the launcher's end): what
 * the device asks for meanwhile is not a gap, and the next push starts the
 * cushion afresh. */
static void audio_rest(void) { SDL_SetAtomicInt(&audio_last_push_ms, (int)SDL_GetTicks() - 1000); }

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
    int made;                                            /* bytes the game handed over */
    int push_gap_us, pushes_late, pushes_close;          /* between two pushes: the longest; over 25 ms; under 8 ms */
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
    int said = under || astat.drops || astat.primes > 1 || every_ms == 5000;
    if (said) {
        double ms = 1000.0 / (SOUND_RATE * 4.0);
        SDL_Log("audio: %d underruns (%.0f ms of sound missing), %d frames dropped, %d refills; queue %.0f / %.0f / "
                "%.0f ms (least / average / most; target %.0f); speed %+.3f%% to %+.3f%%; over %.0f s",
                under, missing * ms, astat.drops, astat.primes, astat.qmin * ms,
                astat.pushes ? astat.qsum / astat.pushes * ms : 0.0, astat.qmax * ms, audio_target * ms,
                (astat.rmin - 1.0f) * 100.0f, (astat.rmax - 1.0f) * 100.0f, (double)(now - astat.since) / 1000.0);
    }
    {
        int taken = SDL_SetAtomicInt(&audio_taken, 0), gap = SDL_SetAtomicInt(&audio_call_gap_us, 0);
        int late = SDL_SetAtomicInt(&audio_calls_late, 0);
        if (said) {                                      /* the two sides, with every line above */
            double ms = 1000.0 / (SOUND_RATE * 4.0);
            SDL_Log("audio: the game made %.0f ms of sound, the device took %.0f ms; pushes up to %.1f ms apart "
                    "(%d over 25 ms, %d under 8 ms); the device's calls up to %.1f ms apart (%d over %.0f ms)",
                    astat.made * ms, taken * ms, astat.push_gap_us / 1000.0, astat.pushes_late, astat.pushes_close,
                    gap / 1000.0, late, audio_late_us / 1000.0);
        }
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
    if (headless_mode) { headless_platform_audio(samples, count); return; }
    if (!audio || count <= 0 || paused || continuing_now || arena_scanning) return;   /* (Continue's way in: unheard) */
    if (arena_behind_card()) return;                      /* (nor the field's music under the arena's card) */
    static int16_t faded[2 * 8192];
    float vol = rewind_volume();
    if (vol < 0.999f && count <= 8192) {                  /* a rewind fades the sound too */
        for (int i = 0; i < 2 * count; i++) faded[i] = (int16_t)((float)samples[i] * vol);
        samples = faded;
    }
    int queued = SDL_GetAudioStreamQueued(audio), bytes = count * 4;
    int now = (int)SDL_GetTicks();
    {   /* (PSIV_AUDIO_LOG) the game's side: how far apart its pushes come; at 2, each late one and each jump */
        static uint64_t last_ns;
        static int last_queued = -1, loud = -1;
        if (loud < 0) loud = SDL_getenv("PSIV_AUDIO_LOG") && SDL_atoi(SDL_getenv("PSIV_AUDIO_LOG")) >= 2;
        uint64_t t = SDL_GetTicksNS();
        int us = last_ns ? (int)((t - last_ns) / 1000) : 0;
        if (us < 250000) {
            if (us > astat.push_gap_us) astat.push_gap_us = us;
            if (us > 25000) astat.pushes_late++;
            if (last_ns && us < 8000) astat.pushes_close++;
            if (loud && (us > 25000 || (last_queued >= 0 && queued > last_queued + bytes)))
                SDL_Log("audio: frame %llu: push after %.1f ms; queue %d ms, was %d ms after the last push",
                        (unsigned long long)frame_count, us / 1000.0, queued * 1000 / (SOUND_RATE * 4),
                        last_queued * 1000 / (SOUND_RATE * 4));
        }
        last_ns = t;
        last_queued = queued + bytes;
        astat.made += bytes;
    }
    int idle = now - SDL_GetAtomicInt(&audio_last_push_ms) > 250;   /* the first sound, or after a pause or a menu */
    if (quip_direct_until) {                             /* a quip put straight in the stream: the game's sound */
        if ((uint64_t)now < quip_direct_until) {         /* does not wait behind what is left of it */
            SDL_ClearAudioStream(audio);
            queued = 0;
            idle = 1;
        }
        quip_direct_until = 0;
    }
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

/* ------------------------------------------------------------------ the world map (worldmap.h) */
/* The small map sits in the picture's top right corner while the party walks a world. The whole
 * map takes the window; the game runs on under it (its music plays, a walk across the world goes
 * on), the player's keys and buttons going to the map: drag it or steer it, zoom by whole steps
 * (so its pixels stay sharp), click a place and the party walks there. Anything that takes the
 * field -- a battle, a window, an event -- puts it away. Both maps are drawn into a buffer at the
 * window's size (the whole map at half that or less on very large windows) and shown as a
 * texture, made again only when what they show changes; the party's ring on the whole map is a
 * small texture of its own, so it can breathe and move without the map being drawn again. */
static const struct { int lvl, mag; } MAP_ZOOMS[] = {{4, 1}, {3, 1}, {2, 1}, {1, 1}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 6}, {0, 8}};
#define N_MAP_ZOOMS (int)(sizeof MAP_ZOOMS / sizeof MAP_ZOOMS[0])
static SDL_Texture *map_tex, *mini_tex, *ring_tex;
static uint32_t *map_buf, *mini_buf;
static int map_bw, map_bh, map_tw, map_th, mini_n, mini_tn;   /* the buffers; the textures' sizes */
static float map_cx, map_cy;                /* the view's middle, a map pixel */
static int map_zi;                          /* its zoom: a step of MAP_ZOOMS */
static int map_cursor;                      /* keys or a gamepad steer it: a cross in the middle */
static uint64_t map_sig, mini_sig;
static SDL_FRect mini_rect;                 /* where the small map is (render pixels); w 0: not shown */
static char map_note[64];
static uint64_t map_note_until;
static SDL_FRect map_btn[4];                /* zoom out, zoom in, close, random battles: buffer pixels */
static int map_drag, map_dragged;
static float map_drag_x, map_drag_y;
static struct { SDL_FingerID id; float x, y; int used; } map_finger[2];
static float map_pinch;

static void map_textures_lost(void) {
    if (map_tex) SDL_DestroyTexture(map_tex);
    if (mini_tex) SDL_DestroyTexture(mini_tex);
    if (ring_tex) SDL_DestroyTexture(ring_tex);
    map_tex = mini_tex = ring_tex = NULL;
    map_tw = map_th = mini_tn = 0;
    map_sig = mini_sig = 0;
}

static float map_scale_of(int zi) { return MAP_ZOOMS[zi].lvl ? 1.0f / (float)(1 << MAP_ZOOMS[zi].lvl) : (float)MAP_ZOOMS[zi].mag; }

/* the steps that make sense: from the whole world in view to about the game's own size */
static void map_zoom_limits(int *lo, int *hi) {
    int W, H;
    worldmap_size(&W, &H);
    int big = W > H ? W : H;
    *lo = 0;
    while (*lo < N_MAP_ZOOMS - 1 && (float)big * map_scale_of(*lo) < 0.55f * (float)map_bh) (*lo)++;
    *hi = N_MAP_ZOOMS - 1;
    while (*hi > *lo && map_scale_of(*hi) > (float)map_bh / 200.0f) (*hi)--;
}

/* a planet goes round: where the view is smaller than the world it shows on past the edges
 * (1: east and west, 2: north and south, as worldmap_render takes it) */
static int map_round(void) {
    int W, H;
    worldmap_size(&W, &H);
    float sc = map_scale_of(map_zi);
    return (worldmap_wraps() && (float)W * sc >= (float)map_bw ? 1 : 0)
         | (worldmap_wraps_y() && (float)H * sc >= (float)map_bh ? 2 : 0);
}

/* the zoom within its steps; the middle on the world (round a planet), or so that the view stays on the
 * map -- in the middle of the window where the whole of it shows */
static void map_clamp(void) {
    int W, H, lo, hi;
    worldmap_size(&W, &H);
    map_zoom_limits(&lo, &hi);
    if (map_zi < lo) map_zi = lo;
    if (map_zi > hi) map_zi = hi;
    if (W <= 0 || H <= 0) return;
    int rnd = map_round();
    float sc = map_scale_of(map_zi), hw = (float)map_bw / sc / 2, hh = (float)map_bh / sc / 2;
    if (rnd & 1) {
        map_cx = fmodf(map_cx, (float)W);
        if (map_cx < 0) map_cx += (float)W;
    } else if (hw * 2 >= (float)W) map_cx = (float)W / 2;
    else map_cx = map_cx < hw ? hw : map_cx > (float)W - hw ? (float)W - hw : map_cx;
    if (rnd & 2) {
        map_cy = fmodf(map_cy, (float)H);
        if (map_cy < 0) map_cy += (float)H;
    } else if (hh * 2 >= (float)H) map_cy = (float)H / 2;
    else map_cy = map_cy < hh ? hh : map_cy > (float)H - hh ? (float)H - hh : map_cy;
}

/* the view: worldmap_render's origin for it */
static void map_view(int *ox, int *oy, int *lvl, int *mag) {
    *lvl = MAP_ZOOMS[map_zi].lvl;
    *mag = MAP_ZOOMS[map_zi].mag;
    if (*lvl) {
        *ox = ((int)floorf(map_cx) >> *lvl) - map_bw / 2;
        *oy = ((int)floorf(map_cy) >> *lvl) - map_bh / 2;
    } else {
        *ox = (int)floorf(map_cx) - map_bw / (2 * *mag);
        *oy = (int)floorf(map_cy) - map_bh / (2 * *mag);
    }
}
static void map_to_buf(float mx, float my, float *bx, float *by) {
    int ox, oy, lvl, mag;
    map_view(&ox, &oy, &lvl, &mag);
    int rnd = map_round(), W, H;                   /* (the copy of it nearest the view's middle) */
    worldmap_size(&W, &H);
    if (rnd & 1) mx += (float)W * roundf((map_cx - mx) / (float)W);
    if (rnd & 2) my += (float)H * roundf((map_cy - my) / (float)H);
    if (lvl) { *bx = mx / (float)(1 << lvl) - (float)ox; *by = my / (float)(1 << lvl) - (float)oy; }
    else { *bx = (mx - (float)ox) * (float)mag; *by = (my - (float)oy) * (float)mag; }
}
static void map_from_buf(float bx, float by, float *mx, float *my) {
    int ox, oy, lvl, mag;
    map_view(&ox, &oy, &lvl, &mag);
    if (lvl) { *mx = (bx + (float)ox) * (float)(1 << lvl); *my = (by + (float)oy) * (float)(1 << lvl); }
    else { *mx = (float)ox + bx / (float)mag; *my = (float)oy + by / (float)mag; }
}

/* the buffer for a window that size: the window's pixels, or 1/2, 1/3 ... of them where it is very large */
static int map_fit(int ow, int oh) {
    int k = 1;
    while ((ow / k) * (oh / k) > 2400000) k++;
    int bw = ow / k, bh = oh / k;
    if (bw < 64 || bh < 64) return 0;
    if (bw != map_bw || bh != map_bh || !map_buf) {
        free(map_buf);
        map_buf = malloc((size_t)bw * (size_t)bh * 4);
        map_bw = bw;
        map_bh = bh;
        map_sig = 0;
    }
    return map_buf != NULL;
}

static void map_say(const char *text) {
    SDL_snprintf(map_note, sizeof map_note, "%s", text);
    map_note_until = SDL_GetTicks() + 2200;
}

/* the whole map up, round the party; cursor: by a key or a button, so a cross to steer */
static void map_show(int cursor) {
    if (!worldmap_shown()) {
        SDL_snprintf(rewind_msg, sizeof rewind_msg, "%s", worldmap_mode() < 1 ? "World map: off (Settings, Game)" : "No map here");
        rewind_msg_until = SDL_GetTicks() + 1800;
        return;
    }
    int ow, oh, x = 0, y = 0, lo, hi;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    if (!map_fit(ow, oh)) return;
    worldmap_party(&x, &y);
    map_cx = (float)x;
    map_cy = (float)y;
    map_zoom_limits(&lo, &hi);
    map_zi = hi;
    while (map_zi > lo && map_scale_of(map_zi) > (float)map_bh / 1000.0f) map_zi--;
    map_clamp();
    map_cursor = cursor;
    map_drag = 0;
    map_finger[0].used = map_finger[1].used = 0;
    map_note[0] = 0;
    map_sig = 0;
    map_open = 1;
}
static void map_toggle(void) {
    if (map_open) map_open = 0;
    else map_show(1);
}

/* a step in or out, the map pixel under buffer pixel (bx, by) staying where it is */
static void map_zoom(int dir, float bx, float by) {
    int lo, hi;
    map_zoom_limits(&lo, &hi);
    int z = map_zi + dir;
    if (z < lo) z = lo;
    if (z > hi) z = hi;
    if (z == map_zi) return;
    float mx, my;
    map_from_buf(bx, by, &mx, &my);
    map_zi = z;
    map_cx = mx - (bx - (float)(map_bw / 2)) / map_scale_of(z);
    map_cy = my - (by - (float)(map_bh / 2)) / map_scale_of(z);
    map_clamp();
}

/* the party sets out for that map pixel, and the map goes */
static void map_walk(float mx, float my) {
    static const uint8_t VEH_ITEM[4] = {0, 0x96, 0x97, 0x98};          /* (the vehicles' item IDs, for their names) */
    int v = worldmap_riding();
    if (!worldmap_can_go()) { map_say("The party can't walk from here now"); return; }
    if (!worldmap_go((int)floorf(mx), (int)floorf(my))) {
        char t[64];
        if (v) SDL_snprintf(t, sizeof t, "No way there by %s", ui_item_name(VEH_ITEM[v]));
        map_say(v ? t : "No way there on foot");
        return;
    }
    map_open = 0;
    SDL_snprintf(rewind_msg, sizeof rewind_msg, v ? "Driving there" : "Walking there");
    rewind_msg_until = SDL_GetTicks() + 1600;
}

/* a click or a tap on the whole map, at render pixel (rx, ry) */
static void map_click(float rx, float ry, int ow, int oh) {
    if (ow <= 0 || oh <= 0) return;
    float bx = rx * (float)map_bw / (float)ow, by = ry * (float)map_bh / (float)oh, mx, my;
    for (int i = 0; i < 4; i++)
        if (map_btn[i].w > 0 && bx >= map_btn[i].x && by >= map_btn[i].y && bx < map_btn[i].x + map_btn[i].w && by < map_btn[i].y + map_btn[i].h) {
            if (i == 3) toggle_encounters();
            else if (i == 2) map_open = 0;
            else map_zoom(i ? 1 : -1, (float)(map_bw / 2), (float)(map_bh / 2));
            return;
        }
    map_from_buf(bx, by, &mx, &my);
    map_walk(mx, my);
}

static void map_dot(const UiCanvas *cv, float x, float y, float r, uint32_t rgb, int alpha) {
    ui_round_rect(cv, x - r, y - r, x + r, y + r, r, rgb, alpha);
}

/* the whole map into its buffer: the view, the way being walked, the places' names, the party */
static void map_paint(int hint_kind) {
    int ox, oy, lvl, mag, x, y;
    map_view(&ox, &oy, &lvl, &mag);
    worldmap_render(map_buf, map_bw, map_bw, map_bh, ox, oy, lvl, mag, map_round());
    UiCanvas cv = {map_buf, map_bw, 0, 0, map_bw, map_bh};
    float u = (float)map_bh / 224.0f, sc = map_scale_of(map_zi), bx, by;
    int ttf = ui_ready();

    for (int i = 0; worldmap_route(i, &x, &y); i += 2) {              /* the way there: a dotted line */
        map_to_buf((float)x, (float)y, &bx, &by);
        if (bx < 0 || by < 0 || bx >= (float)map_bw || by >= (float)map_bh) continue;
        map_dot(&cv, bx, by, 0.9f * u, 0x101830, 200);
        map_dot(&cv, bx, by, 0.55f * u, 0xFFF0C0, 255);
    }

    WorldmapPlace pl[48];
    int np = worldmap_places(pl, 48), nr = 0;
    SDL_FRect used[48];
    float cap = (sc < 0.2f ? 2.9f : 3.4f) * u;
    for (int i = 0; i < np; i++) {                                    /* the places: a dot at the way in ... */
        map_to_buf((float)pl[i].x, (float)pl[i].y, &bx, &by);
        map_dot(&cv, bx, by, 1.5f * u, 0x101830, 255);
        map_dot(&cv, bx, by, 0.95f * u, 0xFFE08A, 255);
    }
    for (int i = 0; i < np && ttf; i++) {                             /* ... and the name under it, where there's room */
        map_to_buf((float)pl[i].x, (float)pl[i].y, &bx, &by);
        if (bx < -40.0f * u || by < -20.0f * u || bx >= (float)map_bw + 40.0f * u || by >= (float)map_bh + 20.0f * u) continue;
        float w = ui_text_width(pl[i].name, cap), base = by + 4.8f * u + cap + (sc > 1.0f ? 6.0f * sc : 0.0f);
        SDL_FRect r = {bx - w / 2 - 1.6f * u, base - cap - 1.3f * u, w + 3.2f * u, cap + 2.8f * u};
        int clash = 0;
        for (int k = 0; k < nr && !clash; k++)
            clash = r.x < used[k].x + used[k].w && used[k].x < r.x + r.w && r.y < used[k].y + used[k].h && used[k].y < r.y + r.h;
        if (clash) continue;
        used[nr++] = r;
        ui_round_rect(&cv, r.x, r.y, r.x + r.w, r.y + r.h, 1.6f * u, 0x0C1430, 185);
        ui_text(&cv, bx - w / 2, base, cap, pl[i].name, 0xFFF4D0, 255);
    }

    if (worldmap_going(&x, &y)) {                                     /* where the party is walking to */
        map_to_buf((float)x, (float)y, &bx, &by);
        ui_triangle(&cv, bx, by - 3.6f * u, 5.4f * u, 0, 0x101830, 255);
        ui_triangle(&cv, bx, by - 3.8f * u, 3.8f * u, 0, 0xFF6048, 255);
    }
    if (map_cursor) {                                                 /* steered: a cross in the middle */
        float mx = (float)(map_bw / 2), my = (float)(map_bh / 2), a = 2.0f * u, b = 6.5f * u, t = 0.45f * u, o = 0.5f * u;
        for (int k = 0; k < 2; k++) {
            uint32_t c = k ? 0xFFFFFF : 0x101830;
            float e = k ? 0 : o;
            ui_round_rect(&cv, mx - b - e, my - t - e, mx - a + e, my + t + e, 0, c, 255);
            ui_round_rect(&cv, mx + a - e, my - t - e, mx + b + e, my + t + e, 0, c, 255);
            ui_round_rect(&cv, mx - t - e, my - b - e, mx + t + e, my - a + e, 0, c, 255);
            ui_round_rect(&cv, mx - t - e, my + a - e, mx + t + e, my + b + e, 0, c, 255);
        }
    }

    for (int i = 0; i < 4; i++) map_btn[i].w = 0;
    if (!ttf) return;
    {   /* the world's name */
        const char *name = worldmap_name();
        float w = ui_text_width(name, 5.2f * u);
        ui_round_rect(&cv, 4.0f * u, 4.0f * u, 12.0f * u + w, 16.0f * u, 2.2f * u, 0x0C1430, 205);
        ui_round_frame(&cv, 4.0f * u, 4.0f * u, 12.0f * u + w, 16.0f * u, 2.2f * u, 0.6f * u, 0x8C9CD8, 255);
        ui_text(&cv, 8.0f * u, 12.7f * u, 5.2f * u, name, 0xFFE08A, 255);
        /* beside it: random battles on or off (the No random battles cheat), and what switches them */
        char t[96], how[32];
        int off = cheats_get(CHEAT_NO_BATTLES);
        if (hint_kind == 1) SDL_snprintf(how, sizeof how, "%s", pad_button_name(pad_of[IN_ENCOUNTERS]));
        else if (hint_kind == 2) SDL_snprintf(how, sizeof how, "tap");
        else key_label(IN_ENCOUNTERS, how, sizeof how);
        SDL_snprintf(t, sizeof t, "Random battles: %s", off ? "Off" : "On");
        float cap = 3.4f * u, x0 = 15.0f * u + w, tw = ui_text_width(t, cap), hw = how[0] ? ui_text_width(how, 2.9f * u) + 3.0f * u : 0;
        float x1 = x0 + tw + hw + 7.0f * u;
        ui_round_rect(&cv, x0, 5.5f * u, x1, 14.5f * u, 2.0f * u, 0x0C1430, 205);
        ui_round_frame(&cv, x0, 5.5f * u, x1, 14.5f * u, 2.0f * u, 0.5f * u, off ? 0x58B878 : 0x8C9CD8, 255);
        ui_text(&cv, x0 + 3.5f * u, 11.7f * u, cap, t, off ? 0xA8F0B4 : 0xFFF4D0, 255);
        if (how[0]) ui_text(&cv, x0 + 3.5f * u + tw + 3.0f * u, 11.6f * u, 2.9f * u, how, 0x9CA8D8, 255);
        map_btn[3] = (SDL_FRect){x0, 5.5f * u, x1 - x0, 9.0f * u};
    }
    if (hint_kind != 1) {                                             /* a mouse or a finger: buttons to zoom, to close */
        static const char *const SIGN[3] = {"-", "+", "x"};
        float bs = 12.0f * u, gap = 2.5f * u, x0 = (float)map_bw - 4.0f * u - 3.0f * bs - 2.0f * gap;
        for (int i = 0; i < 3; i++) {
            float a = x0 + (bs + gap) * (float)i;
            map_btn[i] = (SDL_FRect){a, 4.0f * u, bs, bs};
            ui_round_rect(&cv, a, 4.0f * u, a + bs, 4.0f * u + bs, 2.2f * u, 0x0C1430, 205);
            ui_round_frame(&cv, a, 4.0f * u, a + bs, 4.0f * u + bs, 2.2f * u, 0.6f * u, 0x8C9CD8, 255);
            ui_text_center(&cv, a + bs / 2, 4.0f * u + bs / 2 + 2.4f * u, 5.6f * u, SIGN[i], 0xFFF4D0, 255);
        }
    }
    {   /* how to use it */
        char hint[200];
        if (hint_kind == 1)
            SDL_snprintf(hint, sizeof hint, "D-pad or stick: move      LB / RB: zoom      %s: walk there      %s: close",
                         pad_button_name(pad_of[IN_C]), pad_button_name(pad_of[IN_B]));
        else if (hint_kind == 2) SDL_snprintf(hint, sizeof hint, "Drag: move      Pinch: zoom      Tap a place: walk there");
        else SDL_snprintf(hint, sizeof hint, "Drag or arrows: move      Wheel or + / -: zoom      Click or Enter: walk there      M or Esc: close");
        float hc = 3.3f * u;
        if (ui_text_width(hint, hc) > (float)map_bw - 12.0f * u) hc *= ((float)map_bw - 12.0f * u) / ui_text_width(hint, hc);
        ui_round_rect(&cv, 0, (float)map_bh - 11.0f * u, (float)map_bw, (float)map_bh, 0, 0x0C1430, 200);
        ui_text_center(&cv, (float)(map_bw / 2), (float)map_bh - 4.0f * u, hc, hint, 0xD8E0FF, 255);
    }
    if (map_note[0] && SDL_GetTicks() < map_note_until) {
        float w = ui_text_width(map_note, 4.2f * u), mx = (float)(map_bw / 2), y0 = (float)map_bh - 30.0f * u;
        ui_round_rect(&cv, mx - w / 2 - 5.0f * u, y0, mx + w / 2 + 5.0f * u, y0 + 13.0f * u, 2.5f * u, 0x301418, 235);
        ui_text_center(&cv, mx, y0 + 8.7f * u, 4.2f * u, map_note, 0xFFE0C0, 255);
    }
}

static uint64_t map_hash(const int *v, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) h = (h ^ (uint64_t)(uint32_t)v[i]) * 1099511628211ull;
    return h | 1;
}

/* the party's ring: a dark rim, gold, dark, a white middle */
static void ring_make(void) {
    enum { N = 96 };
    static uint32_t buf[N * N];
    if (ring_tex) return;
    ring_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, N, N);
    if (!ring_tex) return;
    memset(buf, 0, sizeof buf);
    UiCanvas cv = {buf, N, 0, 0, N, N};
    float c = N / 2.0f, r = N / 2.0f - 1.0f;
    ui_set_straight_alpha(1);
    map_dot(&cv, c, c, r, 0x101830, 255);
    map_dot(&cv, c, c, r * 0.81f, 0xFFE040, 255);
    map_dot(&cv, c, c, r * 0.55f, 0x101830, 255);
    map_dot(&cv, c, c, r * 0.24f, 0xFFFFFF, 255);
    ui_set_straight_alpha(0);
    SDL_UpdateTexture(ring_tex, NULL, buf, N * 4);
    SDL_SetTextureBlendMode(ring_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(ring_tex, SDL_SCALEMODE_LINEAR);
}

/* the whole map, over everything */
static void map_draw_full(int ow, int oh) {
    if (!worldmap_shown() || !map_fit(ow, oh)) { map_open = 0; return; }   /* (a battle, a window, an event: away) */
    map_clamp();
    int gx = 0, gy = 0, ox, oy, lvl, mag;
    int going = worldmap_going(&gx, &gy);
    int hint_kind = touch_visible() ? 2 : menu_last_pad && pad ? 1 : 0;
    map_view(&ox, &oy, &lvl, &mag);
    int v[] = {(int)worldmap_rev(), map_bw, map_bh, map_zi, ox, oy, map_cursor, hint_kind, going, gx, gy,
               map_note[0] && SDL_GetTicks() < map_note_until, ui_ready(), cheats_get(CHEAT_NO_BATTLES)};
    uint64_t sig = map_hash(v, (int)(sizeof v / sizeof v[0]));
    if (!map_tex || map_tw != map_bw || map_th != map_bh) {
        if (map_tex) SDL_DestroyTexture(map_tex);
        map_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, map_bw, map_bh);
        if (!map_tex) { map_open = 0; return; }
        SDL_SetTextureScaleMode(map_tex, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(map_tex, SDL_BLENDMODE_NONE);
        map_tw = map_bw;
        map_th = map_bh;
        map_sig = 0;
    }
    if (sig != map_sig) {
        map_paint(hint_kind);
        SDL_UpdateTexture(map_tex, NULL, map_buf, map_bw * 4);
        map_sig = sig;
    }
    SDL_FRect all = {0, 0, (float)ow, (float)oh};
    SDL_RenderTexture(renderer, map_tex, NULL, &all);
    int px, py;
    ring_make();
    if (ring_tex && worldmap_party(&px, &py)) {          /* the party: a ring, breathing */
        float bx, by, u = (float)map_bh / 224.0f, k = (float)oh / (float)map_bh;
        map_to_buf((float)px, (float)py, &bx, &by);
        float r = (3.9f + 0.55f * sinf((float)SDL_GetTicks() * 0.006f)) * u * k;
        SDL_FRect d = {bx * (float)ow / (float)map_bw - r, by * k - r, 2 * r, 2 * r};
        SDL_RenderTexture(renderer, ring_tex, NULL, &d);
    }
}

/* the small map, in the top right corner of the game's view */
static void map_draw_mini(void) {
    mini_rect.w = 0;
    if (map_open || !worldmap_shown() || fieldmenu_active() || pic_dst.w <= 0 || pic_fw <= 0 || pic_h <= 0) return;
    float ky = pic_dst.h / (float)pic_h, kx = pic_dst.w / (float)pic_fw;
    int n = (int)(56.0f * ky + 0.5f);
    if (n < 48) n = 48;
    if (n > 448) n = 448;
    int lvl = 1, px = 0, py = 0, gx = 0, gy = 0, W, H;
    for (int l = 2; l <= 4; l++)
        if (abs((n << l) - 1000) < abs((n << lvl) - 1000)) lvl = l;
    if (!worldmap_party(&px, &py)) return;
    worldmap_size(&W, &H);
    int going = worldmap_going(&gx, &gy), phase = (int)(SDL_GetTicks() / 260 % 4);
    int v[] = {(int)worldmap_rev(), n, lvl, px >> lvl, py >> lvl, phase, going, gx, gy};
    uint64_t sig = map_hash(v, (int)(sizeof v / sizeof v[0]));
    if (n != mini_n || !mini_buf) {
        free(mini_buf);
        mini_buf = malloc((size_t)n * (size_t)n * 4);
        mini_n = n;
        mini_sig = 0;
        if (!mini_buf) return;
    }
    if (!mini_tex || mini_tn != n) {
        if (mini_tex) SDL_DestroyTexture(mini_tex);
        mini_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, n, n);
        if (!mini_tex) return;
        SDL_SetTextureScaleMode(mini_tex, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(mini_tex, SDL_BLENDMODE_BLEND);
        mini_tn = n;
        mini_sig = 0;
    }
    if (sig != mini_sig) {
        int ox = (px >> lvl) - n / 2, oy = (py >> lvl) - n / 2;
        worldmap_render(mini_buf, n, n, n, ox, oy, lvl, 1, 3);
        UiCanvas cv = {mini_buf, n, 0, 0, n, n};
        float u = (float)n / 56.0f, mid = (float)(n / 2);
        WorldmapPlace pl[48];
        int np = worldmap_places(pl, 48);
        for (int i = 0; i < np; i++) {                                /* the places: dots */
            int dx = pl[i].x - px, dy = pl[i].y - py;
            if (worldmap_wraps()) { if (dx > W / 2) dx -= W; else if (dx < -W / 2) dx += W; }
            if (worldmap_wraps_y()) { if (dy > H / 2) dy -= H; else if (dy < -H / 2) dy += H; }
            float bx = mid + (float)dx / (float)(1 << lvl), by = mid + (float)dy / (float)(1 << lvl);
            if (bx < 2 * u || by < 2 * u || bx > (float)n - 2 * u || by > (float)n - 2 * u) continue;
            map_dot(&cv, bx, by, 1.5f * u, 0x101830, 255);
            map_dot(&cv, bx, by, 0.9f * u, 0xFFE08A, 255);
        }
        if (going) {                                                  /* where the party is walking to: there, or at the edge that way */
            int dx = gx - px, dy = gy - py;
            if (worldmap_wraps()) { if (dx > W / 2) dx -= W; else if (dx < -W / 2) dx += W; }
            if (worldmap_wraps_y()) { if (dy > H / 2) dy -= H; else if (dy < -H / 2) dy += H; }
            float bx = (float)dx / (float)(1 << lvl), by = (float)dy / (float)(1 << lvl), lim = mid - 3.5f * u;
            float m = fmaxf(fabsf(bx), fabsf(by));
            if (m > lim) { bx *= lim / m; by *= lim / m; }
            map_dot(&cv, mid + bx, mid + by, 2.0f * u, 0x101830, 255);
            map_dot(&cv, mid + bx, mid + by, 1.3f * u, 0xFF6048, 255);
        }
        float r = (phase == 3 ? 1.9f : 1.5f + 0.2f * (float)phase) * u;   /* the party */
        map_dot(&cv, mid, mid, r + 0.8f * u, 0x101830, 255);
        map_dot(&cv, mid, mid, r, 0xFFE040, 255);
        ui_round_frame(&cv, 0, 0, (float)n, (float)n, 3.0f * u, 1.3f * u, 0x101830, 255);
        ui_round_frame(&cv, 0.8f * u, 0.8f * u, (float)n - 0.8f * u, (float)n - 0.8f * u, 2.4f * u, 0.7f * u, 0xC8D0F0, 255);
        int rr = (int)(3.0f * u);                                     /* the corners outside the frame: clear */
        for (int j = 0; j < rr; j++)
            for (int i = 0; i < rr; i++) {
                float dx = (float)(rr - i) - 0.5f, dy = (float)(rr - j) - 0.5f;
                if (dx * dx + dy * dy <= (float)(rr * rr)) continue;
                mini_buf[j * n + i] = mini_buf[j * n + (n - 1 - i)] = 0;
                mini_buf[(n - 1 - j) * n + i] = mini_buf[(n - 1 - j) * n + (n - 1 - i)] = 0;
            }
        SDL_UpdateTexture(mini_tex, NULL, mini_buf, n * 4);
        mini_sig = sig;
    }
    float x1 = pic_dst.x + (float)(pic_pm + pic_w) * kx - 5.0f * ky, y0 = pic_dst.y + 5.0f * ky;
    mini_rect = (SDL_FRect){floorf(x1 - (float)n), floorf(y0), (float)n, (float)n};
    SDL_SetTextureAlphaMod(mini_tex, 236);
    SDL_RenderTexture(renderer, mini_tex, NULL, &mini_rect);
}

/* held keys and the pad steer the whole map: about a screen a second */
static void map_tick(void) {
    int dx = held(IN_RIGHT) - held(IN_LEFT), dy = held(IN_DOWN) - held(IN_UP);
    if (!dx && !dy) return;
    map_cursor = 1;
    float step = (float)map_bh * 0.016f / map_scale_of(map_zi);
    map_cx += (float)dx * step;
    map_cy += (float)dy * step;
    map_clamp();
}

/* An event while the whole map is up. 1: the map took it. */
static int map_event(const SDL_Event *e, int ow, int oh) {
    float rx, ry;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        SDL_Scancode sc = e->key.scancode;
        if (e->key.key == SDLK_F11 || e->key.key == SDLK_F12) return 0;
        touch_hide();
        menu_last_pad = 0;
        if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_AC_BACK || sc == key_of[IN_B]
            || sc == key_of[IN_MAP]) {
            if (!e->key.repeat) map_open = 0;
        } else if (sc == key_of[IN_ENCOUNTERS]) {        /* random battles off, or on again: here too */
            if (!e->key.repeat) toggle_encounters();
        } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_SPACE || sc == key_of[IN_C]
                   || sc == key_of[IN_START]) {
            if (!e->key.repeat) map_walk(map_cx, map_cy);
        } else if (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS || sc == SDL_SCANCODE_PAGEUP) {
            map_zoom(1, (float)(map_bw / 2), (float)(map_bh / 2));
        } else if (sc == SDL_SCANCODE_MINUS || sc == SDL_SCANCODE_KP_MINUS || sc == SDL_SCANCODE_PAGEDOWN) {
            map_zoom(-1, (float)(map_bw / 2), (float)(map_bh / 2));
        }
        return 1;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        float y = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        if (y == 0.0f || ow <= 0 || oh <= 0) return 1;
        SDL_RenderCoordinatesFromWindow(renderer, e->wheel.mouse_x, e->wheel.mouse_y, &rx, &ry);
        map_cursor = 0;
        map_zoom(y > 0 ? 1 : -1, rx * (float)map_bw / (float)ow, ry * (float)map_bh / (float)oh);
        return 1;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        menu_last_pad = 0;
        if (e->button.button == SDL_BUTTON_RIGHT) { map_open = 0; return 1; }
        if (e->button.button != SDL_BUTTON_LEFT) return 1;
        SDL_RenderCoordinatesFromWindow(renderer, e->button.x, e->button.y, &rx, &ry);
        map_drag = 1;
        map_dragged = 0;
        map_drag_x = rx;
        map_drag_y = ry;
        map_cursor = 0;
        return 1;
    case SDL_EVENT_MOUSE_MOTION:
        if (!map_drag || !(e->motion.state & SDL_BUTTON_LMASK) || ow <= 0) return 1;
        SDL_RenderCoordinatesFromWindow(renderer, e->motion.x, e->motion.y, &rx, &ry);
        if (!map_dragged && fabsf(rx - map_drag_x) + fabsf(ry - map_drag_y) < 5.0f) return 1;
        map_dragged = 1;
        map_cx -= (rx - map_drag_x) * (float)map_bw / (float)ow / map_scale_of(map_zi);
        map_cy -= (ry - map_drag_y) * (float)map_bh / (float)oh / map_scale_of(map_zi);
        map_drag_x = rx;
        map_drag_y = ry;
        map_clamp();
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button != SDL_BUTTON_LEFT || !map_drag) return 1;
        map_drag = 0;
        SDL_RenderCoordinatesFromWindow(renderer, e->button.x, e->button.y, &rx, &ry);
        if (!map_dragged) map_click(rx, ry, ow, oh);
        return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        SDL_GamepadButton b = (SDL_GamepadButton)e->gbutton.button;
        touch_hide();
        menu_last_pad = 1;
        map_cursor = 1;
        if (b == pad_of[IN_B] || b == pad_of[IN_MAP] || b == SDL_GAMEPAD_BUTTON_BACK || b == SDL_GAMEPAD_BUTTON_GUIDE) map_open = 0;
        else if (b == pad_of[IN_C] || b == pad_of[IN_START]) map_walk(map_cx, map_cy);
        else if (b == pad_of[IN_ENCOUNTERS]) toggle_encounters();
        else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) map_zoom(-1, (float)(map_bw / 2), (float)(map_bh / 2));
        else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) map_zoom(1, (float)(map_bw / 2), (float)(map_bh / 2));
        return 1;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return 1;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {                    /* a finger drags it or taps a place; two pinch */
        float x = e->tfinger.x * (float)ow, y = e->tfinger.y * (float)oh;
        int k = -1, other;
        for (int i = 0; i < 2; i++)
            if (map_finger[i].used && map_finger[i].id == e->tfinger.fingerID) k = i;
        if (e->type == SDL_EVENT_FINGER_DOWN) {
            if (k < 0) k = !map_finger[0].used ? 0 : !map_finger[1].used ? 1 : -1;
            if (k < 0) return 1;
            other = map_finger[1 - k].used;
            map_finger[k].used = 1;
            map_finger[k].id = e->tfinger.fingerID;
            map_finger[k].x = x;
            map_finger[k].y = y;
            map_cursor = 0;
            map_dragged = other;                          /* (a second finger: no tap) */
            map_drag_x = x;
            map_drag_y = y;
            if (other) map_pinch = hypotf(map_finger[0].x - map_finger[1].x, map_finger[0].y - map_finger[1].y);
            return 1;
        }
        if (k < 0) {                                      /* not the map's finger: one that was down before it opened */
            float tx, ty;
            if (e->type != SDL_EVENT_FINGER_MOTION) touch_event(e, ow, oh, &tx, &ty);
            return 1;
        }
        other = map_finger[1 - k].used;
        if (e->type == SDL_EVENT_FINGER_MOTION) {
            if (other) {                                  /* a pinch: a step each time the fingers' distance grows or shrinks by a third */
                map_finger[k].x = x;
                map_finger[k].y = y;
                float d = hypotf(map_finger[0].x - map_finger[1].x, map_finger[0].y - map_finger[1].y);
                float mx = (map_finger[0].x + map_finger[1].x) / 2 * (float)map_bw / (float)ow;
                float my = (map_finger[0].y + map_finger[1].y) / 2 * (float)map_bh / (float)oh;
                if (map_pinch > 1.0f && d > map_pinch * 1.33f) { map_zoom(1, mx, my); map_pinch = d; }
                else if (d > 1.0f && d < map_pinch / 1.33f) { map_zoom(-1, mx, my); map_pinch = d; }
                map_dragged = 1;
                return 1;
            }
            if (!map_dragged && fabsf(x - map_drag_x) + fabsf(y - map_drag_y) < 12.0f) return 1;
            map_dragged = 1;
            map_cx -= (x - map_finger[k].x) * (float)map_bw / (float)ow / map_scale_of(map_zi);
            map_cy -= (y - map_finger[k].y) * (float)map_bh / (float)oh / map_scale_of(map_zi);
            map_finger[k].x = x;
            map_finger[k].y = y;
            map_clamp();
            return 1;
        }
        map_finger[k].used = 0;                           /* up */
        if (e->type == SDL_EVENT_FINGER_UP && !other && !map_dragged) map_click(x, y, ow, oh);
        return 1;
    }
    default:
        return 0;
    }
}

/* a click or a tap at (rx, ry) in render pixels: the system menu, the battle
 * menus, or a walk (walkto.h) */
static void click_at(float rx, float ry, int right) {
    if (menu_is_open()) {
        if (right) menu_input(0, 0, 0, 0, 0, 1);
        else menu_tap(rx, ry);
        return;
    }
    if (!right && !paused && mini_rect.w > 0 && rx >= mini_rect.x && ry >= mini_rect.y && rx < mini_rect.x + mini_rect.w
        && ry < mini_rect.y + mini_rect.h) {       /* the small world map: the whole one */
        map_show(0);
        return;
    }
    if (paused || !walkto_enabled() || pic_dst.w <= 0) return;
    float gx = (rx - pic_dst.x) / pic_dst.w * (float)pic_fw - (float)pic_pm;   /* the game's frame pixels */
    float gy = (ry - pic_dst.y) / pic_dst.h * (float)pic_h;
    if (fieldmenu_active()) {                      /* the field menu, a shop, a chest: pick, C, or back */
        fieldmenu_click(gx + (float)pic_pm, gy, right);
        return;
    }
    if (battleui_active()) {                       /* the battle menus: pick, or back */
        battleui_click(gx, gy, right);
        return;
    }
    if (right) {                                   /* stop walking; with text up, B */
        if (!walkto_active()) walkto_click_button(1);
        worldmap_stop();
        walkto_cancel();
        return;
    }
    if (walkto_click_button(0)) return;            /* text up: C, next */
    worldmap_stop();                               /* (a walk across the world: this click's instead) */
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
        if (map_open && !menu_is_open() && map_event(&e, ow, oh)) continue;   /* the world map takes them */
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
                if (in_launcher && menu_screen == MENU_LAUNCHER && sc == SDL_SCANCODE_M) {
                    if (!e.key.repeat) launcher_music_toggle();
                    break;
                }
                if (menu_screen == MENU_JUKEBOX && sc == SDL_SCANCODE_L) {   /* the highlighted song: the launcher's */
                    if (!e.key.repeat) jukebox_set_launcher(menu_sel);
                    break;
                }
                if (in_launcher && menu_screen == MENU_LAUNCHER && sc == SDL_SCANCODE_D && menu_sel != L_MODE) {   /* (on Mode: Right) */
                    if (!e.key.repeat) open_discord();
                    break;
                }
                if (sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_PAGEDOWN || sc == SDL_SCANCODE_PAGEUP) {
                    if (!e.key.repeat || sc != SDL_SCANCODE_TAB)    /* settings pages */
                        menu_turn_page(sc == SDL_SCANCODE_PAGEUP || (sc == SDL_SCANCODE_TAB && (e.key.mod & SDL_KMOD_SHIFT)) ? -1 : 1);
                    break;
                }
                int ok = sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_SPACE
                         || sc == key_of[IN_C] || sc == key_of[IN_START];
                int back = sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_AC_BACK
                           || sc == key_of[IN_B];
                menu_input(sc == SDL_SCANCODE_UP || dir_key(sc, IN_UP), sc == SDL_SCANCODE_DOWN || dir_key(sc, IN_DOWN),
                           sc == SDL_SCANCODE_LEFT || dir_key(sc, IN_LEFT), sc == SDL_SCANCODE_RIGHT || dir_key(sc, IN_RIGHT),
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
            if (sc == key_of[IN_AUTO] && !in_launcher) toggle_auto_text();
            if (sc == key_of[IN_ENCOUNTERS] && !in_launcher) toggle_encounters();
            if (sc == key_of[IN_MAP] && !in_launcher) map_toggle();
            if (e.key.key == SDLK_F9) cycle_battle_depth();
            if (e.key.key == SDLK_F8) cycle_battle_fog();
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:                      /* the field menu: the cursor a row on, or back */
            if (!menu_is_open() && !paused && walkto_enabled() && fieldmenu_active() && e.wheel.y != 0.0f)
                fieldmenu_wheel(e.wheel.y < 0.0f);
            if (menu_is_open()) menu_wheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
            break;
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
                if (b == pad_of[IN_AUTO] && !in_launcher) toggle_auto_text();
                if (b == pad_of[IN_ENCOUNTERS] && !in_launcher) toggle_encounters();
                if (b == pad_of[IN_MAP] && !in_launcher) map_toggle();
                break;
            }
            if (menu_screen == MENU_JUKEBOX && b == pad_of[IN_AUTO]) {   /* the highlighted song: the launcher's */
                jukebox_set_launcher(menu_sel);
                break;
            }
            if (in_launcher && menu_screen == MENU_LAUNCHER && b == pad_of[IN_AUTO]) {   /* the launcher's music */
                launcher_music_toggle();
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
            slider_drag = -1;                              /* a press on a slider starts a drag again */
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
        case SDL_EVENT_MOUSE_MOTION:                       /* the menu: the line under the mouse; a knob dragged */
            if (menu_is_open() && e.motion.which != SDL_TOUCH_MOUSEID) {
                float rx, ry;
                SDL_RenderCoordinatesFromWindow(renderer, e.motion.x, e.motion.y, &rx, &ry);
                if (slider_drag >= 0 && (e.motion.state & SDL_BUTTON_LMASK)) slider_drag_to(rx);
                else menu_hover(rx, ry);
            } else if (!paused && walkto_enabled() && battleui_active() && pic_dst.w > 0
                       && e.motion.which != SDL_TOUCH_MOUSEID) {   /* the battle menus: the entry under the mouse */
                float rx, ry;
                SDL_RenderCoordinatesFromWindow(renderer, e.motion.x, e.motion.y, &rx, &ry);
                battleui_hover((rx - pic_dst.x) / pic_dst.w * (float)pic_fw - (float)pic_pm,
                               (ry - pic_dst.y) / pic_dst.h * (float)pic_h);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) slider_drag = -1;
            break;
        case SDL_EVENT_RENDER_DEVICE_RESET:                /* every texture lost (phones): made again */
            if (menu_tex) SDL_DestroyTexture(menu_tex);
            if (texture) SDL_DestroyTexture(texture);
            if (depth_texture) SDL_DestroyTexture(depth_texture);
            if (emblem_tex) SDL_DestroyTexture(emblem_tex);
            if (halo_tex) SDL_DestroyTexture(halo_tex);
            if (logo_tex) SDL_DestroyTexture(logo_tex);
            if (logo_glow_tex) SDL_DestroyTexture(logo_glow_tex);
            menu_tex = texture = depth_texture = emblem_tex = halo_tex = logo_tex = logo_glow_tex = NULL;
            menu_tex_w = tex_w = depth_tex_w = emblem_n = logo_tw = 0;
            map_textures_lost();
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
    if (menu_is_open()) menu_pad_repeat();
}

/* testing: --menu-script STEPS, one step per pass of the menu's loop: a key
 * by SDL name ("Down", "Tab", "Shift+Tab", "Return", "Escape"), click:X:Y and
 * move:X:Y (render pixels), wheel:N (notches, + up), pad:BUTTON (SDL's name, e.g. rightshoulder),
 * shot:FILE.bmp; then the program ends */
static void menu_script_step(void) {
    static const char *p;
    if (!p) p = test_menu_script;
    if (!*p) quit(0);
    menu_script_passes++;
    char tok[300];
    size_t n = strcspn(p, ",");
    snprintf(tok, sizeof tok, "%.*s", (int)n, p);
    p += n + (p[n] == ',');
    script_do(tok);
}

/* one step of a test script (--menu-script, --events) */
static void script_do(const char *tok) {
    static char shot_path[300];
    SDL_Event e;
    SDL_zero(e);
    float x, y;
    if (!strcmp(tok, "quit")) {
        quit(0);
    } else if (!strncmp(tok, "shot:", 5)) {
        snprintf(shot_path, sizeof shot_path, "%s", tok + 5);
        test_shot = shot_path;
    } else if (sscanf(tok, "click:%f:%f", &x, &y) == 2) {
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = true;
        e.button.windowID = SDL_GetWindowID(window);
        SDL_RenderCoordinatesToWindow(renderer, x, y, &e.button.x, &e.button.y);
        SDL_PushEvent(&e);
        if (map_open) {                              /* (the world map takes a click when the button comes up) */
            e.type = SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.down = false;
            SDL_PushEvent(&e);
        }
    } else if (sscanf(tok, "wheel:%f", &y) == 1) {      /* the mouse wheel, notches (+ up) */
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = SDL_GetWindowID(window);
        e.wheel.y = y;
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
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

static uint64_t work_start;           /* when this frame's work began: the end of the wait before it */

/* the Boss Arena's fight: its save in the cartridge's place (in memory only), through the title
 * unseen, then the fight (arena.c) */
static void arena_load(int i) {
    static uint8_t cart[ARENA_CART_SIZE];
    arena_make_cart(i, cart);
    memset(bus_sram, 0, sizeof bus_sram);
    bus_sram_import(cart, sizeof cart);
    bus_sram_dirty = 0;
    savepages_continue(0);
    arena_begin(i);
    autosave_reset();
    SDL_Log("arena: %s", arena_boss(i) ? arena_boss(i)->name : "?");
}
/* each frame of an arena session: a fight won or lost brings the list back (its time noted); one
 * chosen there starts the console over from its first frame, with that fight's save */
static void arena_watch(void) {
    static int was = ARENA_OFF;
    int s = arena_state();
    if (s != was && s == ARENA_FIGHTING) SDL_Log("arena: the fight on at frame %llu", (unsigned long long)frame_count);
    if (s != was && (s == ARENA_WON || s == ARENA_LOST)) {
        int i = arena_current();
        arena_result = s;
        arena_result_boss = i;
        arena_result_secs = (arena_fight_frames() + 30) / 60;
        if (arena_result_secs < 1) arena_result_secs = 1;
        if (s == ARENA_WON && (arena_best(i) == 0 || arena_result_secs < arena_best(i))) arena_set_best(i, arena_result_secs);
        boss_macros_keep(i, arena_fight_macros());   /* (as last fought: set in battle too) */
        SDL_Log("arena: %s %s in %d s (frame %llu)", arena_boss(i) ? arena_boss(i)->name : "?",
                s == ARENA_WON ? "won" : "lost", arena_result_secs, (unsigned long long)frame_count);
        rumble_off();
        audio_rest();
        arena_enter();
    }
    was = s;
    if (arena_scanning) {
        if (s == ARENA_CAPTURED || s == ARENA_WON || s == ARENA_LOST) {
            int i = arena_current();
            if (s == ARENA_CAPTURED) arena_pic_store(i);
            else if (i >= 0 && i < ARENA_MAX) arena_pic_state[i] = -2;   /* (it didn't wait at its menu) */
            arena_scan_done++;
            int next = arena_next_missing(i + 1);
            rewind_restart();
            if (next >= 0) arena_load(next);
            else {                                   /* all gathered: the fight that was chosen */
                SDL_Log("arena: %d pictures gathered", arena_scan_done);
                arena_scanning = 0;
                arena_set_scan(0);
                arena_load(launch_arena);
            }
            was = ARENA_LOADING;
        }
        return;
    }
    if (arena_go >= 0 && !menu_is_open()) {
        int i = arena_go;
        arena_go = -1;
        wave_stop();                                 /* (from Wave Mode: the Boss Arena's again) */
        wave_mode = 0;
        arena_result = -1;
        rewind_restart();                            /* the console as it started, after this frame */
        arena_load(i);
        was = ARENA_LOADING;
    }
}

/* Wave Mode's run: its first node's save (made by wave_make_cart), Continued unseen */
static void wave_load_first(void) {
    static uint8_t cart[ARENA_CART_SIZE];
    if (!arena_faces && rom_ready) arena_faces = ui_portraits_init() > 0;   /* (for its card) */
    arena_begin(-1);                                 /* (no boss fight) */
    wave_mode = 1;
    wave_make_cart(cart);
    memset(bus_sram, 0, sizeof bus_sram);
    bus_sram_import(cart, sizeof cart);
    bus_sram_dirty = 0;
    savepages_continue(0);
    wave_begin();
    autosave_reset();
    wave_card_t = 0;
    wave_result = -1;
    SDL_Log("wave: %s", wave_run(wave_current()) ? wave_run(wave_current())->name : "?");
}
/* each frame of a Wave Mode session: the next map (the game as it is, saved at its door, Continued
 * after the console starts over), a run over (its record; how it went), a run chosen */
static void wave_watch(void) {
    static int was = WAVE_OFF;
    int s = wave_state();
    if (s != was && s == WAVE_FIGHTING)
        SDL_Log("wave: battle %d at frame %llu", wave_node() + 1, (unsigned long long)frame_count);
    if (s == WAVE_RELOAD) {
        static uint8_t cart[ARENA_CART_SIZE];
        wave_make_reload_cart(cart);                 /* (from RAM: before the console starts over) */
        rewind_restart();
        memset(bus_sram, 0, sizeof bus_sram);
        bus_sram_import(cart, sizeof cart);
        bus_sram_dirty = 0;
        savepages_continue(0);
        wave_begin();
        wave_card_t = 0;
        s = wave_state();
        SDL_Log("wave: on to %s", wave_run(wave_current())->node[wave_node()].place);
    }
    if (s != was && (s == WAVE_CLEARED || s == WAVE_LOST)) {
        wave_result = s;
        wave_result_run = wave_current();
        wave_result_node = wave_node();
        wave_result_frames = wave_time();
        wave_result_n = wave_party(wave_result_ids);
        wave_result_fell = wave_fell();
        wave_result_best = s == WAVE_CLEARED && wave_record_add(wave_result_run, wave_result_frames, wave_result_ids,
                                                                wave_result_n, wave_result_fell);
        SDL_Log("wave: %s %s, %u frames in battle, fell %02X", wave_run(wave_result_run)->name,
                s == WAVE_CLEARED ? "cleared" : "lost", (unsigned)wave_result_frames, wave_result_fell);
        rumble_off();
        audio_rest();
        wave_result_enter();
    }
    if (wave_staging()) wave_card_t = 0;
    else if (wave_card_t < 1000) wave_card_t++;
    was = s;
    if (wave_go && !menu_is_open()) {                /* a run chosen in the game: the console starts over */
        wave_go = 0;
        arena_result = -1;
        rewind_restart();
        wave_load_first();
        was = wave_state();
    }
}

static void back_to_launcher(void);
void platform_frame(const uint32_t *pixels, int w, int h) {
    if (headless_mode) { headless_platform_frame(pixels, w, h); return; }
    handle_events(pixels, w, h);
    if (test_menu_frame && frame_count == test_menu_frame) {
        menu_open();
        menu_opened = 0;                                 /* no fade: a capture shows it at once */
        if (test_menu_journal) {
            journal_enter();
            j_tab = test_menu_page > 0 ? test_menu_page : 0;
        } else if (test_menu_settings) {
            menu_screen = MENU_SETTINGS;
            menu_page = test_menu_page >= 0 && test_menu_page < N_PAGES ? test_menu_page : 0;
            menu_sel = test_menu_row >= -1 && test_menu_row < page_now()->n ? test_menu_row : page_now()->n ? 0 : -1;
        }
    }
    if (test_map_frame && frame_count == test_map_frame && !map_open) {   /* testing: --map */
        map_show(1);
        if (map_open && test_map_zoom >= 0) { map_zi = test_map_zoom; map_cx = test_map_x; map_cy = test_map_y; map_clamp(); }
        if (map_open && test_map_go) { map_walk(map_cx, map_cy); SDL_Log("map: %s", map_open ? map_note : "walking"); }
        test_map_frame = 0;
    }
    if (paused || menu_is_open()) {                      /* the game holds: so do the rumble and the sound */
        rumble_off();
        audio_rest();
    }
    while (paused || menu_is_open()) {                   /* F10 or the menu: hold here, keep the window alive */
        holding = 1;                                     /* (the watchdog knows it's on purpose) */
        present(pixels, w, h, 0);
        rumble_output();                                 /* the Rumble setting's sample */
        SDL_Delay(16);
        handle_events(pixels, w, h);
        if (test_menu_script && menu_is_open()) menu_script_step();
        next_deadline = SDL_GetTicksNS();
        work_start = 0;                                  /* a held frame is not a slow one (detail_watch) */
    }
    holding = 0;
    if (to_launcher) {                                   /* Return to menu: the launcher, then the console over */
        back_to_launcher();
        return;                                          /* (this frame's picture is the old game's: not shown) */
    }
    if (map_open) {                                      /* the world map: the game runs on under it */
        map_tick();
        if (test_menu_script) menu_script_step();        /* (testing: the map's script, a step a frame) */
    }
    if (journal_notes && !arena_mode && journal_moved_on()) {           /* the story moved on: the journal has more */
        SDL_snprintf(rewind_msg, sizeof rewind_msg, "Journal updated");
        rewind_msg_until = SDL_GetTicks() + 2600;
    }
    if (test_rumble_frame && frame_count == test_rumble_frame) rumble_test();
    if (test_rewind_frame && frame_count == test_rewind_frame) rewind_now();   /* testing: Backspace then */
    rumble_output();
    uint8_t user = read_input() | touch_pad();
    for (int i = 0; i < n_test_keys; i++)                /* testing: scripted keys */
        if (frame_count >= test_keys[i].frame && frame_count < test_keys[i].frame + test_keys[i].len) user |= test_keys[i].keys;
    {   /* the world map is up: what the player holds is the map's, and stays so until it is let go */
        static int map_hold;
        if (map_open) { map_hold = 1; user = 0; }
        else if (map_hold) { if (user) user = 0; else map_hold = 0; }
    }
    uint8_t user_raw = user;
    if (wave_mode && wave_state() != WAVE_OFF && !wave_player_input()) user = 0;   /* a run walks by itself */
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
    if (test_key_frame && frame_count == test_key_frame) {   /* testing: a key pressed (by SDL's name) */
        SDL_Event c;
        SDL_zero(c);
        c.type = SDL_EVENT_KEY_DOWN;
        c.key.scancode = SDL_GetScancodeFromName(test_key_name);
        c.key.key = SDL_GetKeyFromScancode(c.key.scancode, SDL_KMOD_NONE, false);
        c.key.down = true;
        c.key.windowID = SDL_GetWindowID(window);
        SDL_PushEvent(&c);
    }
    if (test_hover_frame && frame_count == test_hover_frame && pic_dst.w > 0) {   /* testing: the mouse moved there */
        SDL_Event c;
        SDL_zero(c);
        c.type = SDL_EVENT_MOUSE_MOTION;
        c.motion.windowID = SDL_GetWindowID(window);
        SDL_RenderCoordinatesToWindow(renderer, pic_dst.x + (test_hover_x + (float)pic_pm) * pic_dst.w / (float)pic_fw,
                                      pic_dst.y + test_hover_y * pic_dst.h / (float)pic_h, &c.motion.x, &c.motion.y);
        SDL_PushEvent(&c);
    }
    /* first person: the d-pad turns with the view; a click's walk, or its
     * presses in a battle menu, hold it while they run */
    {   /* the world map: kept as the game runs; a walk across it steps on */
        static int loaded;
        if (!loaded) { loaded = 1; explored_load(); }
        worldmap_frame();
        worldmap_journey(user);
    }
    bus_pad[0] = fieldmenu_pad(scenevox_pad(user, battleui_pad(user, walkto_pad(user, fpview_remap_pad(user)))));
    if (wave_mode && wave_state() == WAVE_WALKING) input_hold = 0;   /* (the walk's presses are no held button) */
    if (input_hold) {
        if (bus_pad[0]) bus_pad[0] = 0;
        else input_hold = 0;
    }
    {   /* the launcher's Continue, on its way to the save */
        static uint64_t since, frames;
        int was = continuing_now;
        continuing_now = savepages_continuing();
        if (continuing_now && !was) { since = SDL_GetTicksNS(); frames = frame_count; }
        if (was && !continuing_now) {
            SDL_Log("continue: at the save after %llu frames, %.2f s", (unsigned long long)(frame_count - frames),
                    (double)(SDL_GetTicksNS() - since) / 1e9);
            auto_swap_out();                             /* (the auto save: the page of saves back in the cartridge) */
            autosave_reset();
        }
    }
    if (arena_mode) arena_watch();                       /* the Boss Arena: a fight over, another chosen */
    if (arena_mode) wave_watch();                        /* Wave Mode: a map to load, a run over, one chosen */
    if (!continuing_now && !auto_loading && !arena_mode) {   /* the auto save, when one is due and can be made */
        static uint8_t made[AUTOSAVE_SIZE];
        if (autosave_frame(made)) auto_write(made);
    }
    if (continuing_now) {
        bus_pad[0] = savepages_pad();                    /* Start, to pass the title; nothing of the player's */
        input_hold = 1;                                  /* (a button still held when it ends isn't the game's) */
    } else if (arena_scanning || arena_behind_card()) {  /* the bosses wait at their menus, untouched;
                                                         * the party waits on the field, unseen */
        bus_pad[0] = 0;
        input_hold = 1;
        static int armed;                                /* a fight over and the list closed: a new press opens it */
        int over = wave_mode ? wave_state() == WAVE_CLEARED || wave_state() == WAVE_LOST
                             : arena_mode && (arena_state() == ARENA_WON || arena_state() == ARENA_LOST);
        if (!over || menu_is_open()) armed = 0;
        else if (!user_raw) armed = 1;                   /* (the press that closed it let go first) */
        else if (armed) { armed = 0; if (wave_mode) wave_result_enter(); else arena_enter(); }
    }
    present(pixels, w, h, 1);
    {   /* PSIV_PERF=1: where the frame's time went, about once a second */
        const char *line = perf_frame_done((m68k_ram[0xEF00] << 8 | m68k_ram[0xEF01]) / 4);
        if (line) SDL_Log("%s", line);
    }

    uint64_t now = SDL_GetTicksNS();
    if (work_start && !held(IN_FF)) {                    /* too much to draw in a frame's time? */
        uint64_t work = now - work_start;
        if (vsync_now) work = work > present_ns ? work - present_ns : 0;   /* (the wait for the display is not work) */
        detail_watch(work);
    }
    else detail_used = 0;
    if (now - last_sram_flush > 1000000000ull) {
        sram_flush();
        last_sram_flush = now;
        {   /* what the world map has explored: now and then */
            static uint64_t last;
            if (now - last > 8000000000ull) { explored_store(); last = now; }
        }
    }
    if (held(IN_FF) || continuing_now || arena_scanning) {                 /* fast forward (or Continue's way in): no pacing */
        next_deadline = now;
        work_start = SDL_GetTicksNS();
        perf_frame_start();
        return;
    }
    next_deadline += NTSC_FRAME_NS;
    if (next_deadline > now) SDL_DelayPrecise(next_deadline - now);
    else if (now - next_deadline > 4 * NTSC_FRAME_NS) next_deadline = now;   /* fell behind: resync */
    work_start = SDL_GetTicksNS();
    perf_frame_start();
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
        snprintf(font_path, sizeof font_path, "%s", path);
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

/* the logo's picture, once */
static void logo_load(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    SDL_Surface *png = SDL_LoadPNG_IO(SDL_IOFromConstMem(LAUNCHER_LOGO_PNG, sizeof LAUNCHER_LOGO_PNG), true);
    SDL_Surface *c = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (png) SDL_DestroySurface(png);
    if (c && c->w > 0 && c->h > 0) logo_px = malloc((size_t)c->w * (size_t)c->h * 4);
    if (!logo_px) {
        SDL_Log("launcher: the logo did not load (%s): the star instead", SDL_GetError());
        if (c) SDL_DestroySurface(c);
        return;
    }
    for (int y = 0; y < c->h; y++)
        memcpy(logo_px + (size_t)y * (size_t)c->w, (const uint8_t *)c->pixels + (size_t)y * (size_t)c->pitch, (size_t)c->w * 4);
    logo_sw = c->w;
    logo_sh = c->h;
    SDL_DestroySurface(c);
}

static void blur_rows(float *f, float *tmp, int w, int h, int r) {      /* a box blur, across then down */
    float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float a = 0;
            for (int k = -r; k <= r; k++) a += x + k >= 0 && x + k < w ? f[y * w + x + k] : 0.0f;
            tmp[y * w + x] = a * inv;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float a = 0;
            for (int k = -r; k <= r; k++) a += y + k >= 0 && y + k < h ? tmp[(y + k) * w + x] : 0.0f;
            f[y * w + x] = a * inv;
        }
}

/* The logo at w render pixels wide: each pixel the average of the stored ones
 * it covers, colours weighted by their alpha (so its edges keep no dark
 * fringe, and it is drawn premultiplied). And its backlight: the logo's own
 * shape, spread and softened, in warm gold -- its letters are black, and the
 * launcher's sky is nearly so. */
static void logo_make(int w) {
    if (!logo_px) return;
    if (w > logo_sw) w = logo_sw;                    /* a larger window: the graphics card scales it up */
    if (w < 16) w = 16;
    if (logo_tex && w == logo_tw) return;
    if (logo_tex) SDL_DestroyTexture(logo_tex);
    if (logo_glow_tex) SDL_DestroyTexture(logo_glow_tex);
    logo_tex = logo_glow_tex = NULL;
    logo_tw = w;
    int h = (int)((int64_t)w * logo_sh / logo_sw);
    if (h < 1) h = 1;
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return;
    for (int y = 0; y < h; y++) {
        int y0 = (int)((int64_t)y * logo_sh / h), y1 = (int)((int64_t)(y + 1) * logo_sh / h);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            int x0 = (int)((int64_t)x * logo_sw / w), x1 = (int)((int64_t)(x + 1) * logo_sw / w);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t a = 0, r = 0, g = 0, b = 0;
            for (int v = y0; v < y1; v++)
                for (int u = x0; u < x1; u++) {
                    uint32_t c = logo_px[(size_t)v * (size_t)logo_sw + (size_t)u];
                    uint32_t ca = c >> 24;
                    a += ca;
                    r += (c >> 16 & 255) * ca;
                    g += (c >> 8 & 255) * ca;
                    b += (c & 255) * ca;
                }
            uint64_t n = (uint64_t)(y1 - y0) * (uint64_t)(x1 - x0);
            px[(size_t)y * (size_t)w + (size_t)x] = (uint32_t)(a / n) << 24 | (uint32_t)(r / (n * 255)) << 16
                                                   | (uint32_t)(g / (n * 255)) << 8 | (uint32_t)(b / (n * 255));
        }
    }
    logo_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (logo_tex) {
        SDL_UpdateTexture(logo_tex, NULL, px, w * 4);
        SDL_SetTextureBlendMode(logo_tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(logo_tex, SDL_SCALEMODE_LINEAR);
    }
    /* the backlight, at a quarter of the size with room round it */
    int qw = w / 4 < 8 ? 8 : w / 4, qh = (int)((int64_t)qw * h / w), rad = qw / 44 < 1 ? 1 : qw / 44;
    if (qh < 1) qh = 1;
    int pad = rad * 4 + 2, gw = qw + 2 * pad, gh = qh + 2 * pad;
    float *f = calloc((size_t)gw * (size_t)gh * 2, sizeof *f);
    uint32_t *gp = malloc((size_t)gw * (size_t)gh * 4);
    if (f && gp) {
        for (int y = 0; y < qh; y++)
            for (int x = 0; x < qw; x++) {
                int sx = (int)((int64_t)x * w / qw), sy = (int)((int64_t)y * h / qh);
                f[(y + pad) * gw + x + pad] = (float)(px[(size_t)sy * (size_t)w + (size_t)sx] >> 24) / 255.0f;
            }
        for (int i = 0; i < 3; i++) blur_rows(f, f + (size_t)gw * (size_t)gh, gw, gh, rad);
        for (int i = 0; i < gw * gh; i++) {
            float a = fminf(1.0f, f[i] * 2.6f) * 0.8f;           /* spread a little past the shape, then soft */
            gp[i] = (uint32_t)(a * 255.0f) << 24 | (uint32_t)(a * 255.0f) << 16 | (uint32_t)(a * 226.0f) << 8
                  | (uint32_t)(a * 150.0f);
        }
        logo_glow_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, gw, gh);
        if (logo_glow_tex) {
            SDL_UpdateTexture(logo_glow_tex, NULL, gp, gw * 4);
            SDL_SetTextureBlendMode(logo_glow_tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            SDL_SetTextureScaleMode(logo_glow_tex, SDL_SCALEMODE_LINEAR);
        }
        logo_glow_x = (float)pad / (float)qw;
        logo_glow_y = (float)pad / (float)qh;
    }
    free(f);
    free(gp);
    free(px);
}

/* a premultiplied texture at a strength 0..1 (its colours scaled with its alpha) */
static void draw_lit(SDL_Texture *t, const SDL_FRect *d, float k) {
    SDL_SetTextureColorModFloat(t, k, k, k);
    SDL_SetTextureAlphaModFloat(t, k);
    SDL_RenderTexture(renderer, t, NULL, d);
}

/* deep space, the stars, the logo where the layout has it (or the star emblem);
 * S render pixels per unit, t seconds; hide: another screen is over it (the
 * stars alone behind Settings: the logo would only show round its edges) */
static void launcher_backdrop(int ow, int oh, float S, float t, const LaunchLayout *L, int hide) {
    float ex = L->logo_cx * S, ey = L->logo_cy * S;
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
    if (hide) return;
    if (L->logo_w > 0.0f) {
        float lw = L->logo_w * S, lh = L->logo_h * S, k = 1.0f;
        logo_make((int)ceilf(lw));
        if (logo_glow_tex) {                              /* the backlight, breathing a little */
            SDL_FRect d = {ex - lw / 2 - logo_glow_x * lw, ey - lh / 2 - logo_glow_y * lh,
                           lw * (1.0f + 2.0f * logo_glow_x), lh * (1.0f + 2.0f * logo_glow_y)};
            draw_lit(logo_glow_tex, &d, k * (0.86f + 0.14f * sinf(t * 1.3f + 0.8f)));
        }
        if (logo_tex) {
            SDL_FRect d = {ex - lw / 2, ey - lh / 2, lw, lh};
            draw_lit(logo_tex, &d, k);
            return;
        }
    }
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
    float S = launch_scale(ow, oh);
    LaunchLayout L = launch_layout((float)ow / S, (float)oh / S);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    launcher_backdrop(ow, oh, S, test_menu_script ? (float)menu_script_passes / 60.0f : (float)(SDL_GetTicks() - t0) / 1000.0f,
                      &L, menu_screen != MENU_LAUNCHER);
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

/* The launcher's music: "Her Last Breath" (the game's song $A4), played from
 * the ROM by the game's own sound driver while the game itself has not been
 * started (frame.h: frame_song_start, frame_song_tick). It needs the ROM, so
 * a first run with none installed is silent; your own file for that song, or
 * the Algol chip, plays it if Settings says so, as in the game. The driver
 * ticks by the clock, 59.92 times a second, whatever the launcher's own
 * pace. Before the game starts the machine is reset (bus_init) and the
 * processor's registers cleared, so it boots exactly as it always did. */
static int launcher_music;                 /* the driver is running */
static uint64_t launcher_tick_at;          /* when its next tick is due */

static float launcher_music_level = 1.0f;  /* down to 0 when it is switched off */
/* While it plays, twice the usual sound is kept queued: the launcher's own
 * picture can take most of the usual 50 ms to draw (pushes up to 47 ms
 * apart were logged on a fast PC), and here the delay is heard by no one. */
static int launcher_cushion;               /* the usual cushion, kept while it is doubled (0: it is not) */
static void launcher_music_fade(float k);
static void launcher_music_stop(void);
/* A sound effect of the launcher's own (Load game's two), from the ROM by the same driver: asked
 * for at the next run, over the song; with the music switched off the driver is started for the
 * effect alone and stopped again when it has sounded (`ms`: how long it lasts). */
static int launcher_sfx_pending;           /* the game's sound ID, 0: none */
static uint64_t launcher_sfx_until;        /* one is sounding until then (SDL_GetTicksNS) */
static int launcher_song_on;               /* the song has been asked of the running driver */
static void launcher_sfx(int id, int ms) {
    launcher_sfx_pending = id;
    launcher_sfx_until = SDL_GetTicksNS() + (uint64_t)ms * 1000000ull;
}

/* A quip from the party as the voiced scenes come on -- a pack just installed, or Scene voices switched on
 * (PsiLok's idea): one of the pack's own (scenevox_quip), its words shown under the settings while it
 * sounds. The game is held while a menu is up, and the launcher is silent without its music, so the sound
 * goes straight to the device; with the launcher's music playing, through the voices' mixer. */
static void voice_quip(void) {
    const char *who, *text;
    int frames = 0;
    if (classic_mode() || !SDL_strcasecmp(setting("enhance.scene_voices"), "off")) return;
    const int16_t *pcm = scenevox_quip(&who, &text, &frames);
    if (!pcm || frames <= 0) return;
    snprintf(quip_line, sizeof quip_line, "%s: \"%s\"", who, text);
    quip_until = SDL_GetTicks() + (uint64_t)frames * 1000 / SOUND_RATE + 1800;
    SDL_Log("quip: %s", quip_line);
    if (!audio) return;
    if (launcher_music) { scenevox_say(pcm, frames); return; }
    float g = scenevox_gain() * (float)setting_int("audio.voices") / 100.0f;
    int16_t *buf = malloc((size_t)frames * 4);
    if (!buf) return;
    for (int i = 0; i < frames * 2; i++) {
        float v = (float)pcm[i] * g;
        buf[i] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
    }
    SDL_PutAudioStreamData(audio, buf, frames * 4);
    free(buf);
    quip_direct_until = SDL_GetTicks() + (uint64_t)frames * 1000 / SOUND_RATE;
}

static int launcher_song_id;               /* the song asked of the driver */
/* the Jukebox's source to the sound (src -1: Settings' again); a change starts the song over, so it
 * starts clean on the other (the file starts with its song) */
static void jukebox_apply(int src) {
    if (src == jukebox_applied) return;
    jukebox_applied = src;
    if (src < 0) apply_enhancements();
    else {
        char dir[1100];
        path_in_home(dir, sizeof dir, "music");
        algol_set_version(src == JB_ALGOL ? 2 : 1);
        algol_set_enabled(src == JB_ALGOL || src == JB_PROTO);
        music_set_folder(src == JB_PACK ? dir : NULL);
    }
    if (launcher_music && launcher_song_id) frame_song_request(launcher_song_id);
}
static void launcher_music_run(void) {
    uint64_t now = SDL_GetTicksNS();
    int wanted = SDL_strcasecmp(setting("audio.launcher_music"), "off") != 0 || jukebox_song;
    int song = jukebox_song ? jukebox_song : launcher_song();   /* (the Jukebox's, or the launcher's own) */
    jukebox_apply(jukebox_song ? jukebox_src : -1);
    int pending = launcher_sfx_pending;
    launcher_sfx_pending = 0;
    if (!launcher_music) {
        if (!rom_ready || !audio || (!wanted && !pending)) return;
        bus_init(rom_buf, rom_size);
        frame_song_start(wanted ? song : pending);   /* (the effect alone: the driver's first request) */
        if (!wanted) pending = 0;
        launcher_song_on = wanted;
        launcher_song_id = wanted ? song : 0;
        launcher_music = 1;
        launcher_tick_at = now;
        launcher_music_level = 1.0f;
        launcher_cushion = audio_target;
        audio_target *= 2;
    }
    if (pending) frame_song_request(pending);
    if (wanted && (!launcher_song_on || launcher_song_id != song)) {   /* switched on while the driver ran for an
                                                      * effect, or another song chosen in the Jukebox */
        frame_song_request(song);
        launcher_song_on = 1;
        launcher_song_id = song;
    }
    if (!wanted && !launcher_song_on && now < launcher_sfx_until) {
        /* the music is off and an effect is sounding: the driver runs on for it */
    } else if (!wanted) {                            /* switched off (its note, M): out over a quarter of a second */
        launcher_music_level -= 0.05f;
        if (launcher_music_level <= 0.0f || !launcher_song_on) { launcher_music_stop(); return; }
        launcher_music_fade(launcher_music_level);
    } else if (launcher_music_level < 1.0f) {        /* and on again before it had gone */
        launcher_music_level = 1.0f;
        launcher_music_fade(1.0f);
    }
    if (backgrounded || now > launcher_tick_at + 8 * NTSC_FRAME_NS) launcher_tick_at = now;   /* away, or a long stall: no catching up */
    for (int n = 0; now >= launcher_tick_at && n < 4; n++) {
        frame_song_tick();
        launcher_tick_at += NTSC_FRAME_NS;
    }
}

/* the sound's level as the launcher fades to black: 1 down to 0 */
static void launcher_music_fade(float k) {
    if (launcher_music && audio)
        SDL_SetAudioStreamGain(audio, (float)setting_int("audio.volume") / 100.0f * (k < 0.0f ? 0.0f : k));
}

static void launcher_music_stop(void) {
    jukebox_song = 0;
    jukebox_apply(-1);                               /* (Settings' sound for the game) */
    if (!launcher_music) return;
    launcher_music = 0;
    launcher_song_id = 0;
    if (launcher_cushion) { audio_target = launcher_cushion; launcher_cushion = 0; }
    if (audio) SDL_ClearAudioStream(audio);
    audio_rest();
    apply_audio();                                   /* the volume back */
    memset(&m68k, 0, sizeof m68k);                   /* the processor as it was: nothing has run */
}

static int launch_has_continue(void) { return cont.ok; }

/* until Start game or Continue is chosen with a ROM in hand (Quit ends the program) */
static void launcher(void) {
    in_launcher = 1;
    touch_suspend(1);                                /* no on-screen pad here: every touch a tap */
    menu_screen = MENU_LAUNCHER;
    int for_classic = classic_mode();
    continue_find();
    menu_sel = launch_first();                       /* Continue, when there is a save: one press to play on */
    menu_opened = 0;
    if (quip_pending) {                              /* a voice pack unpacked itself at the start: a word on it */
        quip_pending = 0;
        voice_quip();
    }
    stars_init();
    logo_load();
    uint64_t t0 = SDL_GetTicks();
    for (;;) {
        handle_events(NULL, 0, 0);
        if (classic_mode() != for_classic) {         /* the mode switched: Classic has the first page only */
            for_classic = classic_mode();
            continue_find();
            if ((menu_sel == L_CONTINUE || menu_sel == L_LOAD) && !cont.ok) menu_sel = L_START;
        }
        if (launcher_go) {
            int go = launcher_go;
            launcher_go = 0;
            if (rom_ensure()) {                      /* the ROM's there (or just chosen): off we go */
                launch_continue = go == 2 && cont.ok;
                if (go != 3) launch_arena = -1;      /* (3: a fight in the Boss Arena) */
                if (go != 4) launch_wave = -1;       /* (4: a run of Wave Mode) */
                break;
            }
        }
        if (test_menu_script) menu_script_step();
        rumble_output();                             /* the Rumble setting's sample */
        float in = test_menu_script ? 1.0f : fminf(1.0f, (float)(SDL_GetTicks() - t0) / 450.0f);
        launcher_present(t0, 1.0f - in);             /* out of black (a test's shots: at once) */
        launcher_music_run();                        /* (after the picture: the first one takes a while to make) */
        SDL_Delay(launcher_music ? 12 : 16);         /* (with music: a little under a tick, so none comes late) */
    }
    /* into black, then the game: a quarter of a second -- or, a save chosen in Load game, as long
     * as its sound lasts (the sound whole, then down with the music over the last quarter second) */
    float out_ms = 260.0f;
    if (launcher_sfx_until > SDL_GetTicksNS())
        out_ms = fmaxf(out_ms, (float)(launcher_sfx_until - SDL_GetTicksNS()) / 1e6f + 60.0f);
    for (uint64_t f0 = SDL_GetTicks(); !test_menu_script || out_ms > 260.0f;) {   /* (a test's script: at once, but for that sound) */
        float ms = (float)(SDL_GetTicks() - f0), k = ms / out_ms;
        if (k >= 1.0f) break;
        launcher_music_fade(fminf(1.0f, (out_ms - ms) / 260.0f));   /* the music goes with the picture */
        launcher_music_run();
        launcher_present(t0, k);
        SDL_Delay(launcher_music ? 12 : 16);
    }
    launcher_music_stop();
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

static void game_setup(void);
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--headless")) {   /* (the settings' previews are made this way) */
        headless_mode = 1;
        return headless_main(argc - 1, argv + 1);
    }
    int play_now = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_arg = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_arg = argv[++i];
        else if (!strcmp(argv[i], "--play")) play_now = 1;              /* no launcher: straight into the game */
        else if (!strcmp(argv[i], "--arena") && i + 1 < argc) {        /* no launcher: that Boss Arena fight (0-26) */
            launch_arena = atoi(argv[++i]);
            play_now = 1;
        } else if (!strcmp(argv[i], "--wave") && i + 1 < argc) {       /* no launcher: a Wave Mode run, R:PICK,PICK,..
                                                                         * (roster indices; macro A: everyone attacks) */
            char *e;
            int r = (int)strtol(argv[++i], &e, 10);
            uint8_t picks[5];
            int np = 0;
            while ((*e == ':' || *e == ',') && np < 5) picks[np++] = (uint8_t)strtol(e + 1, &e, 10);
            const WaveRun *wr = wave_run(r);
            if (wr) {
                uint8_t m[ARENA_MACROS] = {0};
                for (int k = 0; k < np; k++) { m[4 * k] = picks[k] < wr->nroster ? wr->roster[picks[k]].id : 0; m[4 * k + 1] = 1; }
                wave_setup(r, picks, np, m);
                wave_sel_run = r;
                wave_npicks = np;                      /* (the setup screen: this party, these macros) */
                for (int k = 0; k < np; k++) wave_picks[k] = picks[k];
                memcpy(wave_m, m, ARENA_MACROS);
                wave_m_for = r;
                launch_wave = r;
                play_now = 1;
            }
        }
        /* testing: --menu F[:settings[:PAGE[:ROW]]] opens the system menu at
         * frame F (Settings on that page, that row highlighted; -1 the page
         * bar; F:journal:PAGE the Journal on that page); --capture F:FILE.bmp saves the rendered window at frame F and
         * exits (run with SDL_VIDEO_DRIVER=offscreen to keep it off the screen);
         * --test-keys F:KEYS:N,... holds keys by frame; --test-click F:X,Y
         * clicks the frame's pixel (X, Y) through the window, as a mouse would */
        else if (!strcmp(argv[i], "--menu") && i + 1 < argc) {
            char *e;
            test_menu_frame = strtoull(argv[++i], &e, 10);
            test_menu_journal = *e == ':' && e[1] == 'j';            /* F:journal[:PAGE]: the Journal */
            test_menu_settings = *e == ':';
            const char *p = test_menu_settings ? strchr(e + 1, ':') : NULL;
            if (p) {
                test_menu_page = atoi(p + 1);
                p = strchr(p + 1, ':');
                if (p) test_menu_row = atoi(p + 1);
            }
        } else if (!strcmp(argv[i], "--map") && i + 1 < argc) {       /* testing: the world map, as --menu */
            char *e;
            test_map_frame = strtoull(argv[++i], &e, 10);
            if (*e == ':') {
                test_map_zoom = (int)strtol(e + 1, &e, 10);
                if (*e == ':') test_map_x = strtof(e + 1, &e);
                if (*e == ':') test_map_y = strtof(e + 1, &e);
                test_map_go = *e == ':';
            }
        } else if (!strcmp(argv[i], "--menu-script") && i + 1 < argc) {   /* with --menu: see menu_script_step */
            test_menu_script = argv[++i];
        } else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            test_record_path = argv[++i];
        } else if (!strcmp(argv[i], "--events") && i + 1 < argc) {
            test_events = argv[++i];
        } else if (!strcmp(argv[i], "--capture") && i + 1 < argc) {
            char *e;
            test_capture_frame = strtoull(argv[++i], &e, 10);
            test_capture_path = *e == ':' ? e + 1 : "capture.bmp";
        } else if (!strcmp(argv[i], "--test-rewind") && i + 1 < argc) {
            test_rewind_frame = strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--test-keys") && i + 1 < argc) {
            /* F:KEYS:N,... -- keys held from frame F for N frames (UDLRABCS) */
            char *s = argv[++i];
            while (*s && n_test_keys < 4096) {
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
        else if (!strcmp(argv[i], "--test-key") && i + 1 < argc) {
            /* F:NAME -- the key (SDL's name: E, Backspace ...) pressed at frame F */
            char *e;
            test_key_frame = strtoull(argv[++i], &e, 10);
            if (*e == ':') snprintf(test_key_name, sizeof test_key_name, "%s", e + 1);
        }
        else if (!strcmp(argv[i], "--test-hover") && i + 1 < argc) {
            /* F:X,Y -- the mouse moved at frame F to the game's frame pixel (X, Y) */
            char *e;
            test_hover_frame = strtoull(argv[++i], &e, 10);
            if (*e == ':') test_hover_x = strtof(e + 1, &e);
            if (*e == ',') test_hover_y = strtof(e + 1, &e);
        }
        /* testing the first run: --test-pick FILE answers the file dialog;
         * --no-dialogs writes message boxes to the log instead */
        else if (!strcmp(argv[i], "--test-pick") && i + 1 < argc) test_pick = argv[++i];
        else if (!strcmp(argv[i], "--test-voice-pack") && i + 1 < argc) test_voicepack = argv[++i];   /* its dialog */
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
    /* Linux: SDL looks for controllers through libusb as well as hidraw, and
     * each time a USB device comes or goes it reads every device's
     * descriptors there, on this thread: up to 0.7 s of standstill measured.
     * hidraw finds the controllers; libusb is only for a few adapters. (An
     * SDL_HIDAPI_LIBUSB in the environment still decides.) */
    SDL_SetHint(SDL_HINT_HIDAPI_LIBUSB, "0");
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
        /* the modern sounds: the library beside the program (or in the APK's assets), else a development
         * build's res/sounds; and yours (sfx/sfx.ini in the home folder), which come first */
        char dir[1100];
        snprintf(dir, sizeof dir, "%ssounds", base_dir);
        sfx_library_load(dir, sound_file_read, sound_file_done);
        snprintf(dir, sizeof dir, "%sres/sounds", home);
        sfx_library_load(dir, sound_file_read, sound_file_done);
        path_in_home(dir, sizeof dir, "sfx");
        SDL_Log("sound effects: %d in the library; %d of yours (%s/sfx.ini)", sfx_library_count(), sfx_load(dir), dir);
    }
    load_voices();
    load_font();
    {   /* PSIV_WEATHER=rain (storm, snow...): this weather in every battle, for testing (as headless --weather) */
        const char *wx = getenv("PSIV_WEATHER");
        if (wx && *wx && weather_kind_by_name(wx) >= 0) weather_force(weather_kind_by_name(wx));
    }
    apply_enhancements();
    ui_set_image_look(picture_look());                   /* (Smoothing, for the portraits the port draws) */
    apply_audio();                                       /* the levels (the volume once the sound is open) */
    apply_cheats();
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

    /* The game's native rate; SDL's stream resamples to the output device.
     * On Windows the device is opened in its own format. Asked for 53267 Hz,
     * Windows takes the device at that rate and resamples itself (its 10 ms
     * period is then 532.67 frames, which SDL rounds to 533), and every few
     * minutes the device falls to taking less than real time, up to a tenth
     * less, for most of a minute: the queue fills and frames of sound have
     * to be thrown away, two or three a second -- the hitches heard on
     * Windows and nowhere else. Side by side on the same device for five
     * minutes: 142 frames thrown away that way, none this way (the device's
     * side is in psiv.log with PSIV_AUDIO_LOG=1).
     * PSIV_AUDIO_DEVICE=game opens it at the game's rate (as before, and as
     * the other systems still do); =own in its own format anywhere. */
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, SOUND_RATE};
    const char *how = SDL_getenv("PSIV_AUDIO_DEVICE");
#ifdef _WIN32
    int own_format = !(how && !SDL_strcmp(how, "game"));
#else
    int own_format = how && !SDL_strcmp(how, "own");
#endif
    SDL_AudioDeviceID out = own_format ? SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL) : 0;
    if (out) {
        SDL_AudioSpec own;
        audio = SDL_GetAudioDeviceFormat(out, &own, NULL) ? SDL_CreateAudioStream(&spec, &own) : NULL;
        if (!audio || !SDL_BindAudioStream(out, audio)) {
            if (audio) SDL_DestroyAudioStream(audio);
            audio = NULL;
            SDL_CloseAudioDevice(out);
        }
    }
    if (!audio) audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
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
            audio_late_us = (int)(buf_ms * 2500.0);
            SDL_Log("audio: %s, %d Hz, %d channels, %d-frame buffer (%.1f ms); %.0f ms kept queued",
                    SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(audio)), dev.freq, dev.channels, dev_frames,
                    buf_ms, want_ms);
        }
        SDL_ResumeAudioStreamDevice(audio);
    } else {
        SDL_Log("no audio: %s", SDL_GetError());
    }

    /* the launcher first -- unless told to play at once (--play), or a test
     * drives the game from its first frame (--menu, --test-keys, --test-click,
     * --test-rumble, or --capture without a --menu-script for the launcher; --events drives the launcher
     * itself, its --test-keys being for the game after it) */
    rom_look();                                          /* the installed ROM, or one dropped in the folder */
    previews_start();                                    /* the settings' previews, if not made yet (in the background) */
    if (!play_now && !test_menu_frame && (!n_test_keys || test_events) && !test_click_frame && !test_rumble_frame &&
        (!test_capture_path || test_menu_script))
        launcher();                                      /* until Start game, with a ROM */
    else if (!rom_ensure())
        quit(0);

    bus_init(rom_buf, rom_size);
    previews_start();                                  /* (a ROM chosen just now: its previews) */
    game_setup();
    SDL_Log("side panels: %d portraits from the ROM", panels_init());
    next_deadline = SDL_GetTicksNS();
    par_sdl_start();                                   /* the picture's helper threads (par.h) */
    SDL_Log("drawing on %d thread%s", par_sdl_threads(), par_sdl_threads() == 1 ? "" : "s");
    {   /* PSIV_PERF=1: time each frame's parts into the log (perf.h) */
        const char *v = SDL_getenv("PSIV_PERF");
        perf_enable(v && *v == '1');
    }
    SDL_DetachThread(SDL_CreateThread(watchdog, "psiv-watchdog", NULL));

    m68k.s = 1;
    m68k.imask = 7;
    A(7) = rd32(0);
    diag_stack_top = A(7);
    game_run(game_reset);                              /* on its own stack (rewind.h); never returns */
    quit(1);
    return 1;
}

/* what the launcher chose, set up for the console's first frame: the Boss Arena's fight or Wave Mode's run
 * (their own saves), or the player's saves -- Continue's through the title to its save, unseen */
static void game_setup(void) {
    if (launch_arena >= 0 || launch_wave >= 0) {       /* the Boss Arena: the fight's own save, never the player's */
        arena_mode = 1;
        cheats_suspend(1);                             /* every fight fair, whatever Settings > Cheats says */
        rewind_set_enabled(0);                         /* and no going back in one (apply_enhancements keeps it so) */
        launch_continue = 0;
    }
    if (arena_mode && launch_wave >= 0) {
        wave_load_first();                             /* Wave Mode: its run (wave_setup by the menu or --wave) */
    } else if (arena_mode) {
        int first = arena_pics_missing() ? arena_next_missing(0) : -1;
        if (first >= 0) {                              /* the bosses' pictures first, once (arenapic.h) */
            arena_scanning = 1;
            arena_scan_done = 0;
            arena_scan_total = arena_pics_missing();
            arena_set_scan(1);
            SDL_Log("arena: gathering %d pictures", arena_scan_total);
            arena_load(first);
        } else arena_load(launch_arena);
    } else {
    if (launch_continue && !classic_mode() && cont.page != AUTO_PAGE)
        save_setting_int("saves.page", cont.page + 1);     /* Continue: its page (the auto save: the page last in use) */
    if (!classic_mode()) {                             /* the page of saves last in use */
        save_page = setting_int("saves.page") - 1;
        if (save_page < 0 || save_page >= SAVEPAGES_MAX) save_page = 0;
    }
    savepages_set_page(save_page);
    sram_load();
    if (launch_continue && cont.page == AUTO_PAGE) auto_swap_in();   /* the auto save: in the cartridge's place till loaded */
    if (launch_continue) savepages_continue(cont.slot);   /* through the title to that save, unseen */
    autosave_reset();
    }
}

/* Return to menu (the pause menu), between two frames: the game's saves written as Quit writes them, the
 * session's own modes ended, the launcher; then what's chosen there from the console's first frame, as the
 * Boss Arena starts each fight (rewind_restart: the machine as it was, the player's saves read again) */
static void back_to_launcher(void) {
    to_launcher = 0;
    quit_to_menu = 0;
    rumble_off();
    audio_rest();
    if (audio) SDL_ClearAudioStream(audio);
    auto_swap_out();                                   /* (the auto save in the cartridge's place: the saves back) */
    sram_flush();
    explored_store();
    worldmap_stop();
    map_open = 0;
    paused = 0;
    savepages_continue(-1);                            /* (a Continue on its way: no more) */
    if (arena_mode) {                                  /* out of the Boss Arena or Wave Mode */
        wave_stop();
        wave_mode = 0;
        arena_begin(-1);
        arena_scanning = 0;
        arena_set_scan(0);
        arena_result = -1;
        arena_go = -1;
        arena_mode = 0;
        cheats_suspend(0);
    }
    SDL_Log("return to menu at frame %llu", (unsigned long long)frame_count);
    holding = 1;                                       /* (the watchdog: the launcher is no hang) */
    launcher();
    holding = 0;
    rewind_restart();                                  /* the console as it started, after this frame */
    game_setup();
    apply_enhancements();                              /* (Go back on again, out of the Arena; the launcher's Settings) */
    apply_cheats();
    next_deadline = SDL_GetTicksNS();
    work_start = 0;
}
