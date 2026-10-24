// PokeDaisy for BananaOS (the Anbernic RG DS and RG DS Plus): the game
// (libmgba) on the top panel, the companion (../../3ds/core, the same portable
// core as the 3DS port) on the bottom touchscreen. See ../README.md.
//
// BananaOS's menu is the game list: banana-ui starts this through
// /usr/libexec/banana/run-pokedaisy with the ROM, after letting go of the
// panels, the buttons and the sound device; this program keeps to the
// contract in BananaLinux's docs/EMULATORS.md (KMS on both panels, evdev read
// and never grabbed, ALSA "default", SIGSTOP / SIGCONT around a sleep, the
// save written on SIGTERM).
//
//   pokedaisy <rom.gba>                        play
//   pokedaisy --probe <rom.gba>                exit 0 if the companion knows the game, 2 if not
//   pokedaisy --headless <dir> [--size WxH] [--script "..."] <rom.gba>
//                                              no panels: run a script, write PNGs (the tests)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mgba-util/vfs.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/gba.h>

#include "../../3ds/core/pd_canvas.h"
#include "../../3ds/core/pd_game.h"
#include "../../3ds/core/pd_map.h"
#include "../../3ds/core/pd_menu.h"
#include "../../3ds/core/pd_romart.h"
#include "../../3ds/core/pd_settings.h"
#include "../../3ds/core/pd_snapshot.h"
#include "../../3ds/core/pd_ui.h"
#include "../../3ds/desktop/pd_png.h"
#include "audio.h"
#include "display.h"
#include "files.h"
#include "input.h"
#include "scale.h"

#ifndef POKEDAISY_VERSION
#define POKEDAISY_VERSION "dev"
#endif

#define GBA_PIC_W 240
#define GBA_PIC_H 160

// GBA key bits (GBA_KEY_*), as in the app's pokedaisy_jni.c.
enum {
    GBA_A = 1 << 0, GBA_B = 1 << 1, GBA_SELECT = 1 << 2, GBA_START = 1 << 3,
    GBA_RIGHT = 1 << 4, GBA_LEFT = 1 << 5, GBA_UP = 1 << 6, GBA_DOWN = 1 << 7,
    GBA_R = 1 << 8, GBA_L = 1 << 9,
};

// How often the companion reads the game (frames).
#define SNAPSHOT_EVERY 10
// What a save state keeps: the RTC, never the save data - loading one must
// not write an old copy over the save file (CLAUDE.md: save files are sacred).
#define STATE_FLAGS SAVESTATE_RTC
#define AUDIO_MAX_FRAMES 8192

static volatile sig_atomic_t quitSignal;

static void on_signal(int sig) {
    quitSignal = sig;
}

static void log_msg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("pokedaisy: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char* env_or(const char* name, const char* fallback) {
    const char* v = getenv(name);
    return v && *v ? v : fallback;
}

// mGBA's default logger prints every unmapped I/O write: only its errors here.
static void quiet_log(struct mLogger* log, int category, enum mLogLevel level, const char* format, va_list args) {
    (void) log;
    if (!(level & (mLOG_ERROR | mLOG_FATAL))) return;
    fprintf(stderr, "pokedaisy: mgba %s: ", mLogCategoryName(category));
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
}
static struct mLogger quietLogger = { .log = quiet_log, .filter = NULL };

// --- where things go (the launcher sets them; these are its values) ---

static struct {
    char data[512], saves[512], states[512], backups[512], settings[600], soundDevice[128];
} P;

static void paths_init(void) {
    snprintf(P.data, sizeof(P.data), "%s", env_or("POKEDAISY_DATA", "/data/pokedaisy"));
    snprintf(P.saves, sizeof(P.saves), "%s", env_or("POKEDAISY_SAVES", "/data/saves"));
    snprintf(P.states, sizeof(P.states), "%s", env_or("POKEDAISY_STATES", "/data/states/gba"));
    snprintf(P.backups, sizeof(P.backups), "%s", env_or("POKEDAISY_BACKUPS", "/data/pokedaisy/backups"));
    snprintf(P.settings, sizeof(P.settings), "%s/settings.ini", P.data);
    snprintf(P.soundDevice, sizeof(P.soundDevice), "%s", env_or("POKEDAISY_SOUND", "default"));
    make_dirs(P.data);
    make_dirs(P.saves);
    make_dirs(P.states);
}

// --- the headless script (the tests) ---

// One step: run `frames` frames with these buttons held, touching (x, y) in
// the companion's pixels or not, then write a shot, or send a signal.
struct step {
    int frames;
    uint32_t held;
    bool touch;
    int tx, ty;
    char shot[128];
    int signal;
};

#define MAX_STEPS 512
static struct step steps[MAX_STEPS];
static int stepCount;

static uint32_t button_named(const char* s, size_t n) {
    static const struct { const char* name; uint32_t bit; } NAMES[] = {
        { "A", IN_A }, { "B", IN_B }, { "X", IN_X }, { "Y", IN_Y }, { "L", IN_L }, { "R", IN_R },
        { "L2", IN_L2 }, { "R2", IN_R2 }, { "START", IN_START }, { "SELECT", IN_SELECT },
        { "UP", IN_UP }, { "DOWN", IN_DOWN }, { "LEFT", IN_LEFT }, { "RIGHT", IN_RIGHT },
        { "F", IN_MENU }, { "MENU", IN_MENU }, { "L3", IN_L3 }, { "R3", IN_R3 },
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(*NAMES); i++) {
        if (strlen(NAMES[i].name) == n && !strncmp(NAMES[i].name, s, n)) return NAMES[i].bit;
    }
    return 0;
}

static bool add_step(struct step s) {
    if (stepCount >= MAX_STEPS) return false;
    steps[stepCount++] = s;
    return true;
}

// "wait:N  press:A  hold:F+START:5  tap:X,Y  shot:NAME  term", space separated.
// A press holds the buttons 3 frames and lets go 3; a tap touches 3 frames
// and lets go 3 - long enough for the companion and the game to see them.
static bool parse_script(const char* script) {
    const char* p = script;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t') p++;
        if (!*p) break;
        const char* end = p;
        while (*end && *end != ' ' && *end != '\n' && *end != '\t') end++;
        char tok[128];
        size_t n = (size_t) (end - p) < sizeof(tok) - 1 ? (size_t) (end - p) : sizeof(tok) - 1;
        memcpy(tok, p, n);
        tok[n] = 0;
        p = end;
        struct step s = { 0 };
        if (!strncmp(tok, "wait:", 5)) {
            s.frames = atoi(tok + 5);
            add_step(s);
        } else if (!strncmp(tok, "press:", 6) || !strncmp(tok, "hold:", 5)) {
            char* names = strchr(tok, ':') + 1;
            int frames = 3;
            char* colon = strchr(names, ':');
            if (colon) {
                frames = atoi(colon + 1);
                *colon = 0;
            }
            for (char* b = names; *b;) {
                char* plus = strchr(b, '+');
                size_t len = plus ? (size_t) (plus - b) : strlen(b);
                uint32_t bit = button_named(b, len);
                if (!bit) {
                    log_msg("script: no button %.*s", (int) len, b);
                    return false;
                }
                s.held |= bit;
                b += len + (plus ? 1 : 0);
            }
            s.frames = frames;
            add_step(s);
            add_step((struct step) { .frames = 3 });
        } else if (!strncmp(tok, "tap:", 4)) {
            if (sscanf(tok + 4, "%d,%d", &s.tx, &s.ty) != 2) return false;
            s.touch = true;
            s.frames = 3;
            add_step(s);
            add_step((struct step) { .frames = 3 });
        } else if (!strncmp(tok, "shot:", 5)) {
            snprintf(s.shot, sizeof(s.shot), "%s", tok + 5);
            add_step(s);
        } else if (!strcmp(tok, "term")) {
            s.signal = SIGTERM;
            add_step(s);
        } else {
            log_msg("script: what is \"%s\"?", tok);
            return false;
        }
    }
    return true;
}

// --- the screens ---

static struct display disp;
static struct input input;
static struct scale_rect gameRect[PD_SCREEN_MODES], canvasRect;
static uint32_t topPx[PD_UI_WIDTH * PD_UI_HEIGHT];
static uint32_t bottomPx[PD_UI_WIDTH * PD_UI_HEIGHT];
static uint32_t gamePx[GBA_PIC_W * GBA_PIC_H];
static char screenNames[PD_SCREEN_MODES][24];
static const char* headlessDir;
static int scriptAt, scriptLeft = -1;
static bool scriptDone;

static void screens_init(void) {
    gameRect[PD_SCREEN_PIXEL] = scale_fit(GBA_PIC_W, GBA_PIC_H, disp.w, disp.h, true, false);
    gameRect[PD_SCREEN_SHARP] = scale_fit(GBA_PIC_W, GBA_PIC_H, disp.w, disp.h, false, false);
    gameRect[PD_SCREEN_STRETCH] = scale_fit(GBA_PIC_W, GBA_PIC_H, disp.w, disp.h, false, true);
    canvasRect = scale_fit(PD_UI_WIDTH, PD_UI_HEIGHT, disp.w, disp.h, false, false);
    // SETTINGS names the modes by what they are on these panels (the core's are the 3DS's)
    snprintf(screenNames[PD_SCREEN_PIXEL], sizeof(screenNames[0]), "PIXEL %dx", gameRect[PD_SCREEN_PIXEL].w / GBA_PIC_W);
    snprintf(screenNames[PD_SCREEN_SHARP], sizeof(screenNames[0]), "SHARP %.1fx",
             (double) gameRect[PD_SCREEN_SHARP].w / GBA_PIC_W);
    snprintf(screenNames[PD_SCREEN_STRETCH], sizeof(screenNames[0]), "STRETCH");
    const char* names[PD_SCREEN_MODES] = { screenNames[0], screenNames[1], screenNames[2] };
    pd_screen_mode_set_names(names);
}

static void show_canvas(int panel, const uint32_t* px) {
    struct display_buf* b = display_begin(&disp, panel);
    scale_draw(b, disp.w, disp.h, px, PD_UI_WIDTH, PD_UI_HEIGHT, PD_UI_WIDTH, canvasRect, false);
    display_end(&disp, panel);
}

static void show_game(int mode) {
    if (mode < 0 || mode >= PD_SCREEN_MODES) mode = PD_SCREEN_SHARP;
    struct display_buf* b = display_begin(&disp, PANEL_TOP);
    // mGBA's 32-bit colour is 0x00BBGGRR; the panels' XRGB8888 is 0x00RRGGBB
    scale_draw(b, disp.w, disp.h, gamePx, GBA_PIC_W, GBA_PIC_H, GBA_PIC_W, gameRect[mode], true);
    display_end(&disp, PANEL_TOP);
}

// Both panels as one picture, the top over the bottom.
static void write_shot(const char* name) {
    char both[1024];
    snprintf(both, sizeof(both), "%s/%s.png", headlessDir, name);
    uint32_t* px = malloc((size_t) disp.w * disp.h * 2 * 4);
    if (!px) return;
    for (int s = 0; s < PANELS; s++) {
        const struct display_buf* b = &disp.p[s].buf[disp.p[s].front];
        for (int y = 0; y < disp.h; y++) {
            const uint32_t* row = (const uint32_t*) ((const uint8_t*) b->px + (size_t) y * b->pitch);
            for (int x = 0; x < disp.w; x++) px[((size_t) s * disp.h + y) * disp.w + x] = row[x] & 0xFFFFFF;
        }
    }
    if (!pd_png_write(both, disp.w, disp.h * 2, px)) log_msg("couldn't write %s", both);
    free(px);
    log_msg("shot %s", both);
}

// One frame's input: the panels' devices, or the script's next step.
static void read_input(void) {
    if (!headlessDir) {
        input_poll(&input);
        return;
    }
    input.held = 0;
    input.touching = false;
    while (!scriptDone) {
        if (scriptAt >= stepCount) {
            scriptDone = true;
            break;
        }
        struct step* s = &steps[scriptAt];
        if (scriptLeft < 0) {
            scriptLeft = s->frames;
            if (s->shot[0]) write_shot(s->shot);
            if (s->signal) raise(s->signal);
        }
        if (scriptLeft == 0) {
            scriptAt++;
            scriptLeft = -1;
            continue;
        }
        scriptLeft--;
        input.held = s->held;
        if (s->touch) {
            input.touching = true;
            input.touchX = canvasRect.x + (s->tx * canvasRect.w + canvasRect.w / 2) / PD_UI_WIDTH;
            input.touchY = canvasRect.y + (s->ty * canvasRect.h + canvasRect.h / 2) / PD_UI_HEIGHT;
        }
        break;
    }
}

// The touch in the companion's pixels.
static void touch_point(int* x, int* y) {
    int cx = canvasRect.w ? (input.touchX - canvasRect.x) * PD_UI_WIDTH / canvasRect.w : 0;
    int cy = canvasRect.h ? (input.touchY - canvasRect.y) * PD_UI_HEIGHT / canvasRect.h : 0;
    *x = cx < 0 ? 0 : (cx >= PD_UI_WIDTH ? PD_UI_WIDTH - 1 : cx);
    *y = cy < 0 ? 0 : (cy >= PD_UI_HEIGHT ? PD_UI_HEIGHT - 1 : cy);
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

// Without page flips (none on this display) the frames are paced by the clock.
static void pace(double hz) {
    static struct timespec next;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long step = (long) (1e9 / hz);
    if (next.tv_sec == 0 || now.tv_sec > next.tv_sec + 1) next = now; // the first frame, or after a sleep
    next.tv_nsec += step;
    while (next.tv_nsec >= 1000000000L) {
        next.tv_nsec -= 1000000000L;
        next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
}

// A message on the top panel until B or A (headless: shown once).
static void show_message(const char* line1, const char* line2) {
    struct pd_canvas top;
    pd_canvas_init(&top, PD_UI_WIDTH, PD_UI_HEIGHT, topPx);
    pd_splash_draw(&top, line1, line2, "Press B.");
    struct pd_canvas bottom;
    pd_canvas_init(&bottom, PD_UI_WIDTH, PD_UI_HEIGHT, bottomPx);
    pd_splash_draw(&bottom, "BananaOS", NULL, NULL);
    show_canvas(PANEL_TOP, topPx);
    show_canvas(PANEL_BOTTOM, bottomPx);
    if (headlessDir) {
        write_shot("message");
        return;
    }
    uint32_t last = ~0u;
    long until = now_ms() + 60000;
    while (!quitSignal && now_ms() < until) {
        input_poll(&input);
        uint32_t down = input.held & ~last;
        last = input.held;
        if (down & (IN_A | IN_B)) break;
        pace(30);
    }
}

// --- the game ---

struct game_mem {
    uint8_t* ewram;
    size_t ewramSize;
    uint8_t* iwram;
    size_t iwramSize;
    const uint8_t* rom;
    size_t romSize;
};

static bool bus_read(void* ctx, uint32_t addr, void* out, size_t len) {
    const struct game_mem* m = ctx;
    if (addr >= 0x02000000 && addr - 0x02000000 + len <= m->ewramSize) {
        memcpy(out, m->ewram + (addr - 0x02000000), len);
        return true;
    }
    if (addr >= 0x03000000 && addr - 0x03000000 + len <= m->iwramSize) {
        memcpy(out, m->iwram + (addr - 0x03000000), len);
        return true;
    }
    // ROM: pointers in RAM often lead into it (a map's layout).
    if (addr >= 0x08000000 && addr - 0x08000000 + len <= m->romSize) {
        memcpy(out, m->rom + (addr - 0x08000000), len);
        return true;
    }
    return false;
}

static uint32_t gba_keys(uint32_t held) {
    uint32_t k = 0;
    if (held & IN_A) k |= GBA_A;
    if (held & IN_B) k |= GBA_B;
    if (held & IN_SELECT) k |= GBA_SELECT;
    if (held & IN_START) k |= GBA_START;
    if (held & IN_RIGHT) k |= GBA_RIGHT;
    if (held & IN_LEFT) k |= GBA_LEFT;
    if (held & IN_UP) k |= GBA_UP;
    if (held & IN_DOWN) k |= GBA_DOWN;
    if (held & IN_R) k |= GBA_R;
    if (held & IN_L) k |= GBA_L;
    return k;
}

// The state file's time for LOAD STATE, or false if there's none.
static bool state_info(const char* path, char* when, size_t len) {
    struct stat st;
    if (stat(path, &st) || st.st_size == 0) return false;
    struct tm tm;
    localtime_r(&st.st_mtime, &tm);
    strftime(when, len, "%m-%d %H:%M", &tm);
    return true;
}

// Written beside it and renamed over it: a state is never half a file.
static bool save_state(struct mCore* core, const char* path) {
    char tmp[900];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    struct VFile* vf = VFileOpen(tmp, O_CREAT | O_TRUNC | O_RDWR);
    if (!vf) return false;
    bool ok = mCoreSaveStateNamed(core, vf, STATE_FLAGS);
    vf->close(vf);
    sync_file(tmp);
    if (ok && rename(tmp, path) == 0) return true;
    unlink(tmp);
    return false;
}

static bool load_state(struct mCore* core, const char* path) {
    struct VFile* vf = VFileOpen(path, O_RDONLY);
    if (!vf) return false;
    bool ok = mCoreLoadStateNamed(core, vf, STATE_FLAGS);
    vf->close(vf);
    return ok;
}

// Reads the ROM's header: false when it isn't one.
static bool read_header(const char* romPath, uint8_t header[0xC0], size_t* size) {
    FILE* f = fopen(romPath, "rb");
    if (!f) return false;
    memset(header, 0, 0xC0);
    size_t n = fread(header, 1, 0xC0, f);
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fclose(f);
    *size = s > 0 ? (size_t) s : 0;
    return n == 0xC0;
}

static int probe(const char* romPath) {
    uint8_t header[0xC0];
    size_t size;
    if (!read_header(romPath, header, &size)) {
        printf("not a GBA game: %s\n", romPath);
        return 1;
    }
    struct pd_game game;
    pd_game_detect(&game, header, size);
    if (game.cfg) {
        printf("%s (%s rev %d)\n", game.title ? game.title : "?", game.code, game.revision);
        return 0;
    }
    printf("%s\n", game.unsupported ? game.unsupported : "not a game the companion knows");
    return 2;
}

// The save is the card's GBA save, the one RetroArch keeps for the same game
// (/data/saves/<game>.srm: raw save data, as mGBA writes it), so the player
// can switch emulators and keep playing. A save next to the ROM (another
// firmware's) is copied in the first time, never moved.
static void save_path_for(const char* romPath, const char* base, char* out, size_t len) {
    snprintf(out, len, "%s/%s.srm", P.saves, base);
    if (file_exists(out)) return;
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", romPath);
    char* slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else snprintf(dir, sizeof(dir), ".");
    static const char* const EXTS[] = { "sav", "srm" };
    for (int i = 0; i < 2; i++) {
        char other[1200];
        snprintf(other, sizeof(other), "%s/%s.%s", dir, base, EXTS[i]);
        if (strcmp(other, out) && file_exists(other)) {
            if (copy_file(other, out)) log_msg("using %s as the save (copied to %s)", other, out);
            else log_msg("couldn't copy %s to %s (%s)", other, out, strerror(errno));
            return;
        }
    }
}

static int run_game(const char* romPath) {
    char base[256], savePath[800], statePath[800];
    rom_base(romPath, base, sizeof(base));
    save_path_for(romPath, base, savePath, sizeof(savePath));
    snprintf(statePath, sizeof(statePath), "%s/%s.pd1", P.states, base);

    uint8_t header[0xC0];
    size_t romFileSize;
    if (!read_header(romPath, header, &romFileSize)) {
        log_msg("can't read %s", romPath);
        show_message("Couldn't open the game:", base);
        return 1;
    }
    struct pd_game game;
    pd_game_detect(&game, header, romFileSize);

    struct VFile* rom = VFileOpen(romPath, O_RDONLY);
    if (!rom) {
        show_message("Couldn't open the game:", base);
        return 1;
    }
    struct mCore* core = mCoreFindVF(rom);
    if (!core || !core->init(core)) {
        rom->close(rom);
        show_message("Not a GBA game:", base);
        return 1;
    }
    mCoreInitConfig(core, NULL);
    core->setVideoBuffer(core, (color_t*) gamePx, GBA_PIC_W);
    core->setAudioBufferSize(core, 2048);
    if (!core->loadROM(core, rom)) {
        core->deinit(core);
        show_message("The game didn't load:", base);
        return 1;
    }
    // the save as it was, before the game touches it
    backup_save(P.backups, base, savePath);
    struct VFile* save = VFileOpen(savePath, O_CREAT | O_RDWR);
    if (save) core->loadSave(core, save);
    else log_msg("can't open the save %s (%s): playing without one", savePath, strerror(errno));
    log_msg("%s: %s, save %s", base, game.cfg ? game.title : (game.unsupported ? game.unsupported : "no companion"),
            savePath);

    // the sound resampled so a game frame per panel refresh gives 48 kHz (the GBA's 59.73 Hz to the
    // panels' ~60: the game runs that much faster, as on the 3DS)
    double ratio = GBAAudioCalculateRatio(1, disp.refresh, 1);
    blip_t* left = core->getAudioChannel(core, 0);
    blip_t* right = core->getAudioChannel(core, 1);
    blip_set_rates(left, core->frequency(core), AUDIO_RATE * ratio);
    blip_set_rates(right, core->frequency(core), AUDIO_RATE * ratio);
    core->reset(core);

    struct game_mem mem = { 0 };
    mem.ewram = core->getMemoryBlock(core, 0x02, &mem.ewramSize);
    mem.iwram = core->getMemoryBlock(core, 0x03, &mem.iwramSize);
    size_t romSize = 0;
    game.rom = core->getMemoryBlock(core, 0x08, &romSize);
    game.romSize = game.rom ? romSize : 0;
    mem.rom = game.rom;
    mem.romSize = game.romSize;
    // the ROM's art (the region map, the TRAINER CARD): found now, in one pass, not mid-game
    if (game.cfg) {
        pd_romart_scan(&game);
        pd_map_art(&game);
    }

    struct pd_settings settings;
    pd_settings_load(&settings, P.settings);
    struct pd_ui ui;
    pd_ui_init(&ui, &settings);
    struct pd_snapshot snap, shown;
    pd_snapshot_read(&shown, &game, bus_read, &mem);
    shown.frame = 0;
    struct pd_host_info host = { .count = 2 };
    host.labels[0] = "FILE";
    snprintf(host.values[0], sizeof(host.values[0]), "%.39s", base);
    host.labels[1] = "SPEED";
    snprintf(host.values[1], sizeof(host.values[1]), "-");
    host.hasState = state_info(statePath, host.stateWhen, sizeof(host.stateWhen));
    struct pd_canvas bottom;
    pd_canvas_init(&bottom, PD_UI_WIDTH, PD_UI_HEIGHT, bottomPx);

    // the game's save, as mGBA keeps it: when its dirty flags clear, mGBA has just written a finished save
    // into the mapped file, which is then flushed to the card (else it sat in the page cache up to ~30 s)
    struct GBA* gba = core->board;
    int saveDirty = gba->memory.savedata.dirty;
    static int16_t audioBuf[AUDIO_MAX_FRAMES * 2];
    bool redraw = true, touching = false, ffToggled = false;
    uint32_t last = 0;
    bool menuArmed = false, menuChord = false;
    enum pd_tab tabBeforeMenu = PD_TAB_PARTY;
    unsigned frame = 0, fpsFrames = 0;
    long fpsStart = now_ms(), lastTick = fpsStart;
    int exitCode = 0;

    while (!quitSignal) {
        read_input();
        if (headlessDir && scriptDone) break;
        uint32_t held = input.held, down = held & ~last, up = last & ~held;
        last = held;

        // F: a modifier with Start (quit), R (save state) and L (load state); alone, it opens SETTINGS
        bool leave = false;
        bool saveNow = false, loadNow = false;
        if (down & IN_MENU) {
            menuArmed = true;
            menuChord = false;
        }
        if (held & IN_MENU) {
            if (down & ~IN_MENU) menuChord = true;
            if (down & IN_START) leave = true;
            if (down & IN_R) saveNow = true;
            if (down & IN_L) loadNow = true;
        } else {
            if (down & IN_X) {
                pd_ui_next_tab(&ui, 1);
                redraw = true;
            }
            if (down & IN_Y) redraw |= pd_ui_back(&ui);
            if (down & IN_L2) ffToggled = !ffToggled;
        }
        if ((up & IN_MENU) && menuArmed) {
            menuArmed = false;
            if (!menuChord) {
                // (a pick-list or confirm open over the tab goes with it)
                ui.overlay = PD_OVERLAY_NONE;
                ui.summarySlot = -1;
                if (ui.tab == PD_TAB_SETTINGS) {
                    ui.tab = tabBeforeMenu;
                } else {
                    tabBeforeMenu = ui.tab;
                    ui.tab = PD_TAB_SETTINGS;
                }
                redraw = true;
            }
        }

        if (input.touching) {
            int x, y;
            touch_point(&x, &y);
            redraw |= pd_ui_touch(&ui, x, y, true);
            touching = true;
        } else if (touching) {
            redraw |= pd_ui_touch(&ui, 0, 0, false);
            touching = false;
        }

        switch (pd_ui_take_action(&ui)) {
        case PD_ACTION_TOGGLE_FF:
            ffToggled = !ffToggled;
            break;
        case PD_ACTION_SAVE_STATE:
            saveNow = true;
            break;
        case PD_ACTION_LOAD_STATE:
            loadNow = true;
            break;
        case PD_ACTION_LEAVE_GAME:
            leave = true;
            break;
        case PD_ACTION_SETTINGS_CHANGED:
            if (!pd_settings_save(&settings, P.settings)) log_msg("couldn't write %s", P.settings);
            break;
        case PD_ACTION_NONE:
            break;
        }
        if (saveNow) {
            if (save_state(core, statePath)) log_msg("state saved: %s", statePath);
            else log_msg("couldn't save the state %s (%s)", statePath, strerror(errno));
            host.hasState = state_info(statePath, host.stateWhen, sizeof(host.stateWhen));
            redraw = true;
        }
        if (loadNow && host.hasState) {
            backup_save(P.backups, base, savePath);
            if (load_state(core, statePath)) log_msg("state loaded: %s", statePath);
            else log_msg("couldn't load the state %s", statePath);
        }
        if (leave) break;

        // fast-forward: toggled (L2, the companion's chip) or held (R2), muted meanwhile
        bool ff = ffToggled || (held & IN_R2);
        if (ff != host.ffOn) {
            host.ffOn = ff;
            redraw = true;
        }
        // F's chords never reach the game
        uint32_t keys = (held & IN_MENU) ? 0 : gba_keys(held);
        int frames = ff ? settings.ffSpeed : 1;
        for (int i = 0; i < frames; i++) {
            core->setKeys(core, keys);
            core->runFrame(core);
            frame++;
        }
        if (saveDirty && !gba->memory.savedata.dirty) sync_file(savePath);
        saveDirty = gba->memory.savedata.dirty;
        int avail = blip_samples_avail(left);
        if (avail > AUDIO_MAX_FRAMES) avail = AUDIO_MAX_FRAMES;
        if (ff || !audio_on()) {
            blip_clear(left);
            blip_clear(right);
        } else if (avail > 0) {
            blip_read_samples(left, audioBuf, avail, true);
            blip_read_samples(right, audioBuf + 1, avail, true);
            audio_write(audioBuf, avail);
        }

        if (frame / SNAPSHOT_EVERY != (frame - (unsigned) frames) / SNAPSHOT_EVERY && mem.ewram && mem.iwram) {
            pd_snapshot_read(&snap, &game, bus_read, &mem);
            pd_ui_update(&ui, &snap);
            // only a change the companion shows is worth a redraw (not the frame counter)
            snap.frame = 0;
            if (memcmp(&snap, &shown, sizeof(snap))) {
                shown = snap;
                redraw = true;
            }
        }

        fpsFrames += (unsigned) frames;
        long now = now_ms();
        long dt = now - lastTick;
        // (a sleep is a gap of minutes: the animations take it as a moment)
        redraw |= pd_ui_tick(&ui, (unsigned) (dt < 0 ? 0 : (dt > 250 ? 250 : dt)));
        lastTick = now;
        if (now - fpsStart >= 1000) {
            // against the GBA's own 268111856 / 4481136 = 59.73 frames a second
            unsigned long pct =
                (unsigned long) ((unsigned long long) fpsFrames * 1000 * 100 * 4481136 / ((now - fpsStart) * 268111856ULL));
            snprintf(host.values[1], sizeof(host.values[1]), "%lu%% (%u FPS)", pct, fpsFrames);
            fpsFrames = 0;
            fpsStart = now;
            if (ui.tab == PD_TAB_SETTINGS) redraw = true;
        }

        if (redraw) {
            pd_ui_draw(&ui, &bottom, &game, &shown, &host);
            show_canvas(PANEL_BOTTOM, bottomPx);
            redraw = false;
        }
        show_game(settings.screenMode);
        if (!headlessDir && !disp.p[PANEL_TOP].flips) pace(disp.refresh);
    }
    if (quitSignal) log_msg("signal %d: writing the save and leaving", (int) quitSignal);

    // deinit writes the save back (mGBA keeps it mapped and flushes it as the game writes it); then it's
    // flushed to the card, so a shutdown right after can't lose it
    core->deinit(core);
    sync_file(savePath);
    return exitCode;
}

int main(int argc, char** argv) {
    const char* romPath = NULL;
    const char* script = "";
    int hw = 640, hh = 480;
    bool probeOnly = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probeOnly = true;
        else if (!strcmp(argv[i], "--headless") && i + 1 < argc) headlessDir = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &hw, &hh) != 2 || hw < 320 || hh < 240 || hw > 4096 || hh > 4096) {
                fprintf(stderr, "--size WxH, at least 320x240\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--version")) {
            printf("pokedaisy-banana %s\n", POKEDAISY_VERSION);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: pokedaisy [--probe | --headless DIR [--size WxH] [--script S]] ROM\n");
            return 1;
        } else romPath = argv[i];
    }
    if (!romPath) {
        fprintf(stderr, "usage: pokedaisy [--probe | --headless DIR [--size WxH] [--script S]] ROM\n");
        return 1;
    }
    if (probeOnly) return probe(romPath);

    // SIGTERM (a shutdown while playing), SIGINT, SIGHUP: the save, then out. No SA_RESTART: a poll or
    // a sleep waiting on the panels returns at once.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    log_msg("pokedaisy-banana %s: %s", POKEDAISY_VERSION, romPath);
    mLogSetDefaultLogger(&quietLogger);
    paths_init();

    if (headlessDir) {
        make_dirs(headlessDir);
        if (!parse_script(script)) return 1;
        if (!display_open_headless(&disp, hw, hh)) return 1;
    } else {
        char err[256];
        const char* top = env_or("POKEDAISY_TOP", env_or("BANANA_TOP", "DSI-2"));
        if (!display_open_kms(&disp, top, err, sizeof(err))) {
            log_msg("can't open the panels: %s", err);
            return 1;
        }
        log_msg("panels %dx%d at %.4f Hz, top %s", disp.w, disp.h, disp.refresh, top);
        input_open(&input, env_or("POKEDAISY_TOUCH", env_or("BANANA_TOUCH", "fe5e0000.i2c")), disp.w, disp.h);
        if (env_or("POKEDAISY_SOUND_OFF", NULL) == NULL) audio_open(P.soundDevice);
    }
    screens_init();

    int code = run_game(romPath);

    audio_close();
    if (!headlessDir) input_close(&input);
    display_close(&disp);
    log_msg("bye (%d)", code);
    return code;
}
