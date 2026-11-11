//
// XPSemu: per-game settings and play time
//
// While a game runs, g_config holds its settings: the user's own values
// with the game's overrides on top. The user's values are kept aside, and
// put back in g_config while the settings file is written (see
// xemu_settings_save_hook), so a game's values never end up in xemu.toml.
//
// Files, in xemu's base path (/data/xemu/ on the PS5):
//   game-settings/<key>.txt   "option=value" lines, overrides only
//   playtime.txt              "<key>\t<seconds>\t<last started>" lines
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <ctype.h>
#include <math.h>
#include <sstream>
#include <time.h>

#include "common.hh"
#include "game-profile.hh"
#include "xog_ui.h"

static const char *const kOptionKeys[GO__COUNT] = {
    "resolution", "screen_shape", "picture_fit", "smoothing",
    "dsp",        "volume",       "overlay",      "anti_aliasing", "fps_cap",
    "anisotropic_filtering", "cas_sharpness", "texture_sharpness", "crt_filter",
    "adaptive_triggers", "gyro_aim", "turbo", "msaa",
    "msaa_cutouts", "movie_upscaler",       // patch 354, 355
    "ambient_occlusion",                    // patch 363
};

const char *GameOptionName(int option)
{
    static const char *const names[GO__COUNT] = {
        "Resolution",     "Screen shape",          "Picture fit",
        "Upscaling",      "Audio processor (DSP)", "Volume",
        "FPS counter",    "Anti-aliasing", "FPS cap",
        "Anisotropic filtering", "AMD CAS sharpness", "Texture sharpness", "CRT filter",
        "Adaptive triggers", "Gyro aim", "Turbo buttons", "MSAA",
        "MSAA smooth cut-outs", "Movie upscaler",       // patch 354, 355
        "Ambient occlusion",                            // patch 363
    };
    return names[option];
}

const char *GameOptionHelp(int option)
{
    static const char *const help[GO__COUNT] = {
        "Higher is sharper, and uses more of the GPU. Lower it if this game runs slow.",
        "Auto follows the game, and goes 16:9 when a widescreen patch is on. "
        "16:9 here also tells the game the console is set to widescreen.",
        "Stretch fills the whole screen.",
        "How the picture is enlarged to the screen. Sharp uses SGSR; AMD CAS sharpens, best with anti-aliasing on.",
        "Keep it Off: very slow on the PS5 and games stall. Only if this "
        "game has no sound without it.",
        "How loud this game is.",
        "In the corner while this game runs: the frame rate, or Full for frame time, CPU, memory and resolution too.",
        "Smooths jagged edges. FXAA is light, SMAA is sharper, SSAA draws at x6 and averages it down; SSAA + SMAA smooths that once more (the smoothest, and the heaviest).",
        "Holds the game at this rate. Custom lets you type any number. A game whose speed is tied to its frames slows down with it.",
        "Sharper textures on floors, roads and walls seen at an angle. Higher is sharper; 16x costs a little more GPU.",
        "How much AMD CAS sharpens: 0.0 is the lightest, 1.0 the strongest. Only with Upscaling on AMD CAS.",
        "Sharper textures by picking more detailed mipmaps. Works with anisotropic filtering and CAS. High can shimmer in some games.",
        "An old TV's look over the picture. Scanlines draws dark lines between the game's own 480 lines; CRT adds a slight curve, darker corners and a light shadow mask.",
        "Triggers push back. Heavy: harder to press. Trigger: a click like a gun. Auto: picks for known games.",
        "Tilt the controller to aim or look around, on top of the right stick. Higher turns faster.",
        "Hold a turbo button and the game sees it pressed again and again, about 15 times a second: nonstop "
        "fire in shooters, button mashing done for you. With your own button mapping, it follows the Xbox button behind it.",
        "Smooths the jagged edges of the game's 3D shapes as they are drawn: 2x, 4x or 8x samples per pixel. "
        "Higher is smoother and uses more of the GPU. Works with the other anti-aliasing. Used from the next start.",
        // patch 354
        "With MSAA on, also smooths the edges of see-through cut-outs: grass, leaves, fences, hair. "
        "Does nothing while MSAA is Off.",
        // patch 355
        "Makes the game's videos sharper when Resolution is above x1. Only videos change, nothing else in the game.",
        // patch 363
        "Soft shadows in corners and where things meet, for more depth in 3D scenes. "
        "Low is subtle, High is stronger. Uses a little more of the GPU; a few games show none.",
    };
    return help[option];
}

// Each option's values, in the order they're offered.
static std::vector<int> Choices(int option)
{
    switch (option) {
    case GO_SCALE: return { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };   // patch 328: up to x10 (user: "for fun")
    case GO_ASPECT:
        return { CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO,
                 CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3,
                 CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9 };
    case GO_FIT:
        return { CONFIG_DISPLAY_UI_FIT_SCALE, CONFIG_DISPLAY_UI_FIT_STRETCH };
    case GO_FILTER:
        return { CONFIG_UPSCALE_SMOOTH, CONFIG_UPSCALE_SHARP, CONFIG_UPSCALE_CAS };
    case GO_DSP: return { 1, 0 };
    case GO_VOLUME: return { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    case GO_OVERLAY:
        return { CONFIG_FPS_ON, CONFIG_FPS_FULL, CONFIG_FPS_OFF };
    case GO_AA: return { 0, 1, 2, 3, 4 };
    case GO_FPS_CAP: return { 0, 30, 60, 90, GO_FPS_CAP_CUSTOM };
    case GO_AF: return { 0, 2, 4, 8, 16 };
    case GO_CAS: return { 0, 1, 2, 3, 4 };
    case GO_TEXSHARP: return { 0, 1, 2, 3 };
    case GO_CRT: return { 0, 1, 2 };
    case GO_TRIGGERS: return { TRIGGERS_OFF, TRIGGERS_DRIVING, TRIGGERS_SHOOTER };     // Auto is the default
    case GO_GYRO: return { 1, 2, 3 };     // Off is the default
    case GO_TURBO:                        // Off is the default
        return { TURBO_A, TURBO_B, TURBO_X, TURBO_Y, TURBO_RT, TURBO_A | TURBO_X, TURBO_A | TURBO_B | TURBO_X | TURBO_Y,
                 TURBO_A | TURBO_B | TURBO_X | TURBO_Y | TURBO_RT | TURBO_LT };
    case GO_MSAA: return { 2, 4, 8 };     // patch 325: Off is the default
    case GO_A2C: return { 0 };            // patch 354: On is the default
    case GO_MOVIEUP: return { 0 };        // patch 355: On is the default
    case GO_SSAO: return { 0, 1, 2 };     // patch 367: a Video setting too (per game: Default is it)
    }
    return {};
}

const char *GameOptionValueName(int option, int value)
{
    static char buf[16];
    if (value == GAME_DEFAULT) {
        return option == GO_TRIGGERS ? "Auto" : option == GO_GYRO || option == GO_TURBO || option == GO_MSAA ? "Off" :
               option == GO_A2C || option == GO_MOVIEUP ? "On" : "Default";     // patch 354, 355
    }
    switch (option) {
    case GO_SCALE:
        snprintf(buf, sizeof(buf), "x%d", value);
        return buf;
    case GO_ASPECT:
        switch (value) {
        case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3: return "4:3";
        case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9: return "16:9";
        default: return "Auto";
        }
    case GO_FIT:
        return value == CONFIG_DISPLAY_UI_FIT_STRETCH ? "Stretch" : "Fit";
    case GO_FILTER:
        return value == CONFIG_UPSCALE_SHARP ? "Sharp" : value == CONFIG_UPSCALE_CAS ? "AMD CAS" : "Smooth";
    case GO_DSP: return value ? "On" : "Off";
    case GO_VOLUME:
        snprintf(buf, sizeof(buf), "%d%%", value * 10);
        return buf;
    case GO_OVERLAY:
        return value == CONFIG_FPS_FULL ? "Full" : value == CONFIG_FPS_ON ? "FPS" : "Off";
    case GO_AA:
        return value == 1 ? "FXAA" : value == 2 ? "SMAA" : value == 3 ? "SSAA" : value == 4 ? "SSAA + SMAA" : "Off";
    case GO_FPS_CAP:
        if (value == 0) {
            return "Off";
        }
        if (value == GO_FPS_CAP_CUSTOM) {
            return "Custom";
        }
        snprintf(buf, sizeof(buf), "%d FPS", value);
        return buf;
    case GO_AF:
        if (value < 2) {
            return "Off";
        }
        snprintf(buf, sizeof(buf), "%dx", value);
        return buf;
    case GO_CAS: {
        static const char *const k[5] = { "0.0", "0.25", "0.5", "0.75", "1.0" };
        return k[std::clamp(value, 0, 4)];
    }
    case GO_TEXSHARP: {
        static const char *const k[4] = { "Off", "Low", "Medium", "High" };
        return k[std::clamp(value, 0, 3)];
    }
    case GO_CRT: return value == 2 ? "CRT" : value == 1 ? "Scanlines" : "Off";
    case GO_TRIGGERS: return value == TRIGGERS_DRIVING ? "Heavy" : value == TRIGGERS_SHOOTER ? "Trigger" : "Off";
    case GO_GYRO: return value >= 3 ? "High" : value == 2 ? "Medium" : "Low";
    case GO_TURBO:
        switch (value) {
        // patch 327: the DualSense's names (the Xbox's A B X Y RT under the default layout)
        case TURBO_A: return "Cross";
        case TURBO_B: return "Circle";
        case TURBO_X: return "Square";
        case TURBO_Y: return "Triangle";
        case TURBO_RT: return "R2";
        case TURBO_A | TURBO_X: return "Cross + Square";
        case TURBO_A | TURBO_B | TURBO_X | TURBO_Y: return "All buttons";
        default: return "All buttons + L2, R2";
        }
    case GO_MSAA: return value >= 8 ? "8x" : value >= 4 ? "4x" : value >= 2 ? "2x" : "Off";
    case GO_A2C:
    case GO_MOVIEUP: return value ? "On" : "Off";     // patch 354, 355
    case GO_SSAO: return value >= 2 ? "High" : value == 1 ? "Low" : "Off";     // patch 363
    }
    return "";
}

const char *GameOptionValueLabel(int option, int value)
{
    if (option != GO_SCALE || value == GAME_DEFAULT) {
        return GameOptionValueName(option, value);
    }
    return GameOptionValueName(option, value);
}

int GameOptionStep(int option, int value, int step, bool with_default)
{
    std::vector<int> choices = Choices(option);
    if (with_default) {
        choices.insert(choices.begin(), GAME_DEFAULT);
    }
    int n = choices.size();
    auto it = std::find(choices.begin(), choices.end(), value);
    int i = it == choices.end() ? 0 : it - choices.begin();
    return choices[((i + step) % n + n) % n];
}

// patch 266: the adaptive triggers' kind of game. By title ID, then by name for the series with many IDs.
int TriggersBuiltIn(const std::string &key, const std::string &name)
{
    static const char *const driving[] = {
        "4541005B",     // Burnout 3: Takedown
        "45410076",     // Burnout Revenge
        "41430019",     // Burnout 2: Point of Impact
        "4D53004B",     // Project Gotham Racing 2
        "4D530003",     // Project Gotham Racing
        "54540079",     // Midnight Club 3: DUB Edition Remix
        "45410028",     // Need for Speed: Hot Pursuit 2
        "45410047",     // Need for Speed: Underground
        "4541005A",     // Need for Speed: Underground 2
        "4541007B",     // Need for Speed: Most Wanted
        "4541009E",     // Need for Speed: Carbon
        "434D0001",     // Colin McRae Rally 3
        "434D002B",     // Colin McRae Rally 2005
        "434D0011",     // TOCA Race Driver 2
        "434D0050",     // TOCA Race Driver 3
        "4D53002A",     // Midtown Madness 3
    };
    static const char *const shooter[] = {
        "4D530004",     // Halo: Combat Evolved
        "4D530064",     // Halo 2
        "55530008",     // Far Cry Instincts
        "55530060",     // Far Cry Instincts: Evolution
        "45410083",     // Black
        "4D530051",     // Conker: Live & Reloaded
        "4553000A",     // TimeSplitters 2
        "4156002A",     // Call of Duty: Finest Hour
        "41560051",     // Call of Duty 2: Big Red One
        "4156005D",     // Call of Duty 3
        "45410091",     // Half-Life 2
        "4541001A",     // Medal of Honor: Frontline
        "4541003F",     // Medal of Honor: Rising Sun
        "4541005E",     // Medal of Honor: European Assault
        "41560020",     // DOOM 3
        "4156004E",     // DOOM 3: Resurrection of Evil
    };
    for (const char *k : driving) {
        if (key == k) {
            return TRIGGERS_DRIVING;
        }
    }
    for (const char *k : shooter) {
        if (key == k) {
            return TRIGGERS_SHOOTER;
        }
    }
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
    static const char *const driving_names[] = {
        "outrun", "out run", "need for speed", "burnout", "gotham", "midnight club", "forza", "rallisport",
    };
    for (const char *w : driving_names) {
        if (n.find(w) != std::string::npos) {
            return TRIGGERS_DRIVING;
        }
    }
    if (n.rfind("halo", 0) == 0) {
        return TRIGGERS_SHOOTER;
    }
    return TRIGGERS_OFF;
}

int TriggersChihiro(const std::string &id)
{
    if (id == "SGBZ" || id == "SBFY" || id == "SBHQ" || id == "SBKD") {    // OutRun 2, Crazy Taxi, Maximum Tune 1 / 2
        return TRIGGERS_DRIVING;
    }
    if (id == "SBFN" || id == "SBFZ" || id == "SBHU") {    // House of the Dead 3, Virtua Cop 3, Ghost Squad
        return TRIGGERS_SHOOTER;
    }
    return TRIGGERS_OFF;
}

int TriggersFor(const std::string &key, const std::string &name, int choice)
{
    return choice == GAME_DEFAULT ? TriggersBuiltIn(key, name) : std::clamp(choice, 0, 2);
}

std::string GameKey(uint32_t title_id, const std::string &fallback)
{
    if (title_id) {
        char id[16];
        snprintf(id, sizeof(id), "%08X", title_id);
        return id;
    }
    return fallback;
}

static std::string BasePath()
{
    return xemu_settings_get_base_path();
}

//
// Values in g_config
//

static bool g_scale_now;

static void ReadConfig(int values[GO__COUNT])
{
    values[GO_SCALE] = std::clamp(g_cfg.scale, 1, 10);
    values[GO_ASPECT] = g_cfg.aspect;
    values[GO_FIT] = g_cfg.fit;
    values[GO_FILTER] = g_cfg.upscale;
    values[GO_DSP] = 0;
    values[GO_VOLUME] = g_cfg.volume;
    values[GO_OVERLAY] = g_cfg.fps;
    values[GO_AA] = g_cfg.aa;
    values[GO_FPS_CAP] = g_cfg.fps_cap;
    values[GO_AF] = g_cfg.af;
    values[GO_CAS] = g_cfg.cas;
    values[GO_TEXSHARP] = g_cfg.texsharp;
    values[GO_CRT] = g_cfg.crt;
    values[GO_TRIGGERS] = GAME_DEFAULT;     // patch 266: per game only (Auto), no setting of the user's
    values[GO_GYRO] = GAME_DEFAULT;         // (Off)
    values[GO_TURBO] = GAME_DEFAULT;        // (Off), per game only
    values[GO_MSAA] = GAME_DEFAULT;         // patch 325: (Off), per game only
    values[GO_A2C] = GAME_DEFAULT;          // patch 354: (On), per game only
    values[GO_MOVIEUP] = GAME_DEFAULT;      // patch 355: (On), per game only
    values[GO_SSAO] = g_cfg.ssao;           // patch 367: the Video setting (patch 363 was per game only)
}

static void WriteConfig(const int values[GO__COUNT], bool)
{
    g_cfg.scale = values[GO_SCALE];
    g_cfg.aspect = values[GO_ASPECT];
    g_cfg.fit = values[GO_FIT];
    g_cfg.upscale = values[GO_FILTER];
    g_cfg.volume = values[GO_VOLUME];
    g_cfg.fps = values[GO_OVERLAY];
    g_cfg.aa = values[GO_AA];
    g_cfg.fps_cap = values[GO_FPS_CAP];
    g_cfg.af = values[GO_AF];
    g_cfg.cas = values[GO_CAS];
    g_cfg.texsharp = values[GO_TEXSHARP];
    g_cfg.crt = values[GO_CRT];
    g_cfg.ssao = values[GO_SSAO];           // patch 367
}

//
// The running game
//

static bool g_running;           // A game's settings are in g_config
static std::string g_key;        // Its key
static GameProfile g_profile;    // Its overrides
static int g_globals[GO__COUNT]; // The user's own values meanwhile

static void Apply()
{
    int merged[GO__COUNT];
    for (int i = 0; i < GO__COUNT; i++) {
        merged[i] = g_profile.value[i] == GAME_DEFAULT ? g_globals[i] :
                                                         g_profile.value[i];
    }
#ifdef __PROSPERO__
    merged[GO_SCALE] = g_globals[GO_SCALE]; // Never a game's own (WriteConfig)
    merged[GO_FILTER] = g_globals[GO_FILTER]; // Main settings only too
    merged[GO_DSP] = g_globals[GO_DSP];
#endif
    WriteConfig(merged, true);
}

int GameGlobal(int option)
{
    if (g_running) {
        return g_globals[option];
    }
    int values[GO__COUNT];
    ReadConfig(values);
    return values[option];
}

int GameEffective(int option)
{
    if (g_running && g_profile.value[option] != GAME_DEFAULT) {
        return g_profile.value[option];
    }
    return GameGlobal(option);
}

void GameSetGlobal(int option, int value)
{
    if (g_running) {
        g_globals[option] = value;
        Apply();
    } else {
        int values[GO__COUNT];
        ReadConfig(values);
        values[option] = value;
        WriteConfig(values, true);
    }
    xemu_settings_save();
}

static std::set<std::string> g_pal60_on, g_pal60_off;
static bool g_pal60_loaded;
static const char *const kPal60File = "/data/xpsemu/pal60.txt";

static void Pal60Load()
{
    if (g_pal60_loaded) {
        return;
    }
    g_pal60_loaded = true;
    std::ifstream f(kPal60File);
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() > 1 && line[0] == '-') {
            g_pal60_off.insert(line.substr(1));
        } else if (!line.empty()) {
            g_pal60_on.insert(line);
        }
    }
}

bool Pal60On(const std::string &key)
{
    Pal60Load();
    // On for every PAL game by default (user, patch 313), like a real Xbox with PAL 60 set in its dashboard:
    // a game that supports 60 Hz picks it, one that doesn't stays at 50 Hz. "-key" in pal60.txt turns one game off.
    return !g_pal60_off.count(key);
}

void Pal60Toggle(const std::string &key)
{
    if (Pal60On(key)) {
        g_pal60_on.erase(key);
        g_pal60_off.insert(key);
    } else {
        g_pal60_off.erase(key);
        g_pal60_on.insert(key);
    }
    std::ofstream f(kPal60File);
    for (const std::string &k : g_pal60_on) {
        f << k << "\n";
    }
    for (const std::string &k : g_pal60_off) {
        f << "-" << k << "\n";
    }
}

static std::map<std::string, int> g_fg_modes;
static bool g_fg_loaded;
static const char *const kFgFile = "/data/xpsemu/framegen.txt";

// 0 off, 1 Auto, 2 to 4 x2 to x4. A line is the game's key, then its amount (none: Auto).
int FrameGenMode(const std::string &key)
{
    if (!g_fg_loaded) {
        g_fg_loaded = true;
        std::ifstream f(kFgFile);
        std::string line;
        while (std::getline(f, line)) {
            const size_t space = line.find(' ');
            if (!line.empty()) {
                g_fg_modes[line.substr(0, space)] = space == std::string::npos ? 1 : std::clamp(atoi(line.c_str() + space + 1), 1, 4);
            }
        }
    }
    const auto it = g_fg_modes.find(key);
    return it == g_fg_modes.end() ? 0 : it->second;
}

void FrameGenSetMode(const std::string &key, int mode)
{
    FrameGenMode(key);
    if (mode > 0) {
        g_fg_modes[key] = mode;
    } else {
        g_fg_modes.erase(key);
    }
    std::ofstream f(kFgFile);
    for (const auto &m : g_fg_modes) {
        f << m.first << " " << m.second << "\n";
    }
}

std::string FrameGenLine(const std::string &key)
{
    const int mode = FrameGenMode(key);
    return mode == 0 ? std::string() : mode == 1 ? std::string("framegen=auto\n") : "framegen=" + std::to_string(mode) + "\n";
}

static std::set<std::string> g_unlock_off;   // "-KEY": a game on by default, turned off
static std::set<std::string> g_unlock_on;    // "+KEY": a game off by default, turned on
static bool g_unlock_loaded;
static const char *const kUnlockFile = "/data/xpsemu/unlock60.txt";

// Games whose Unlock 60 is offered but OFF until the player turns it on: their timing was read from the
// code, not yet played through at 60.
static bool Unlock60DefaultOff(const std::string &key)
{
    // Black was offered here (patch 313) and removed (patch 314): at 60 its play ran fast (user test) -
    // its gameplay steps per frame; only its RenderWare timers follow the clock.
    (void)key;
    return false;
}

bool Unlock60Known(const std::string &key)
{
    return key == "41560020" || key == "45410091" ||    // Doom 3, Half-Life 2
           key == "56550039" ||                         // Hulk: Ultimate Destruction (patch 140)
           key == "4E4D0009" ||                         // Breakdown (patch 346): on, to test - removed if its play
                                                        // runs fast at 60
           key == "4D53002A" ||                         // Midtown Madness 3: measured, averaged frame time (main
                                                        // loop 0x1e6c87, timer on the trapped rdtsc 0x9f814); user
                                                        // played it at 60: perfect
           Unlock60DefaultOff(key);
}

bool Fps60Wanted(const std::string &key)
{
    if (!g_unlock_loaded) {
        g_unlock_loaded = true;
        std::ifstream f(kUnlockFile);
        std::string line;
        while (std::getline(f, line)) {
            if (line.size() > 1 && line[0] == '-') {
                g_unlock_off.insert(line.substr(1));
            } else if (line.size() > 1 && line[0] == '+') {
                g_unlock_on.insert(line.substr(1));
            }
        }
    }
    return Unlock60DefaultOff(key) ? g_unlock_on.count(key) != 0 : !g_unlock_off.count(key);
}

bool Unlock60On(const std::string &key)
{
    return Unlock60Known(key) && Fps60Wanted(key);
}

void Unlock60Toggle(const std::string &key)
{
    const bool on = Fps60Wanted(key);
    if (Unlock60DefaultOff(key)) {
        if (on) {
            g_unlock_on.erase(key);
        } else {
            g_unlock_on.insert(key);
        }
    } else if (on) {
        g_unlock_off.insert(key);
    } else {
        g_unlock_off.erase(key);
    }
    std::ofstream f(kUnlockFile);
    for (const std::string &k : g_unlock_off) {
        f << "-" << k << "\n";
    }
    for (const std::string &k : g_unlock_on) {
        f << "+" << k << "\n";
    }
}

GameProfile GameProfileLoad(const std::string &key)
{
    GameProfile profile;
    std::ifstream in(BasePath() + "game-settings/" + key + ".txt");
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, eq);
        if (name == "button_mapping") {     // patch 269
            XbPadMap m;
            if (xemu_ps5_padmap_parse(line.c_str() + eq + 1, &m)) {
                char text[64];
                xemu_ps5_padmap_format(&m, text, sizeof(text));
                profile.padmap = text;
            }
            continue;
        }
        int value = atoi(line.c_str() + eq + 1);
        for (int i = 0; i < GO__COUNT; i++) {
            std::vector<int> ok = Choices(i);
            if (name == kOptionKeys[i] &&
                std::find(ok.begin(), ok.end(), value) != ok.end()) {
                profile.value[i] = value;
            }
        }
    }
    return profile;
}

void GameProfileSave(const std::string &key, const GameProfile &profile)
{
    std::error_code ec;
    std::filesystem::create_directories(BasePath() + "game-settings", ec);
    std::string file = BasePath() + "game-settings/" + key + ".txt";
    bool any = false;
    std::ostringstream out;
    for (int i = 0; i < GO__COUNT; i++) {
        if (profile.value[i] != GAME_DEFAULT) {
            out << kOptionKeys[i] << "=" << profile.value[i] << "\n";
            any = true;
        }
    }
    if (!profile.padmap.empty()) {      // patch 269
        out << "button_mapping=" << profile.padmap << "\n";
        any = true;
    }
    if (any) {
        std::ofstream(file, std::ios::trunc) << out.str();
    } else {
        std::filesystem::remove(file, ec);
    }
    if (g_running && key == g_key) {
        g_profile = profile;
    }
}

void GameProfileReapply()
{
    if (g_running) {
        Apply();
    }
}

//
// Play time
//

static std::map<std::string, GamePlay> g_play;
static bool g_play_loaded;
static double g_unsaved; // Seconds played since the file was written

static void PlayLoad()
{
    g_play_loaded = true;
    std::ifstream in(BasePath() + "playtime.txt");
    std::string key;
    double seconds;
    long long last;
    while (in >> key >> seconds >> last) {
        g_play[key] = { seconds, (int64_t)last };
    }
}

static void PlaySave()
{
    std::ofstream out(BasePath() + "playtime.txt", std::ios::trunc);
    for (const auto &[key, play] : g_play) {
        out << key << "\t" << (long long)play.seconds << "\t"
            << (long long)play.last << "\n";
    }
    g_unsaved = 0;
}

GamePlay GamePlayGet(const std::string &key)
{
    if (!g_play_loaded) {
        PlayLoad();
    }
    auto it = g_play.find(key);
    return it == g_play.end() ? GamePlay() : it->second;
}

void GamePlayTick(float dt, bool playing)
{
    if (!g_running || !playing || dt <= 0 || dt > 1) {
        return;
    }
    g_play[g_key].seconds += dt;
    g_unsaved += dt;
    if (g_unsaved >= 30) { // Closing the app doesn't give a chance to save
        PlaySave();
    }
}

void GamePlayAdd(const std::string &key, double seconds, int64_t last)
{
    if (!g_play_loaded) {
        GamePlayGet(key);
    }
    g_play[key].seconds += seconds;
    g_play[key].last = last;
    PlaySave();
}

std::string GamePlayTimeText(double seconds)
{
    int minutes = (int)(seconds / 60);
    char buf[32];
    if (seconds <= 0) {
        return "Not played yet";
    } else if (minutes < 1) {
        return "Under a minute";
    } else if (minutes < 60) {
        snprintf(buf, sizeof(buf), "%dm", minutes);
    } else {
        snprintf(buf, sizeof(buf), "%dh %02dm", minutes / 60, minutes % 60);
    }
    return buf;
}

std::string GamePlayLastText(int64_t last)
{
    if (last <= 0) {
        return "Never";
    }
    time_t now = time(NULL), then = (time_t)last;
    struct tm a, b;
    localtime_r(&now, &a);
    localtime_r(&then, &b);
    // Days between the two dates (not 24 hour periods).
    a.tm_hour = b.tm_hour = 12;
    a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
    long days = lround(difftime(mktime(&a), mktime(&b)) / 86400);
    char buf[32];
    if (days <= 0) {
        return "Today";
    } else if (days == 1) {
        return "Yesterday";
    } else if (days < 7) {
        snprintf(buf, sizeof(buf), "%ld days ago", days);
    } else {
        strftime(buf, sizeof(buf), "%b %e", &b);
    }
    return buf;
}

//
// Starting and stopping
//

void GameStart(const std::string &key)
{
    if (g_running) {
        GameStop();
    }
    ReadConfig(g_globals);
    g_key = key;
    g_profile = GameProfileLoad(key);
    g_running = true;
    g_scale_now = true; // The Xbox restarts next
    Apply();
    g_scale_now = false;

    if (!g_play_loaded) {
        PlayLoad();
    }
    g_play[key].last = time(NULL);
    PlaySave();
}

void GameStop()
{
    if (!g_running) {
        return;
    }
    g_running = false;
    g_scale_now = true; // The Xbox restarts next
    WriteConfig(g_globals, true);
    g_scale_now = false;
    if (g_play_loaded) {
        PlaySave();
    }
}

bool GameAnyRunning()
{
    return g_running;
}

bool GameIsRunning(const std::string &key)
{
    return g_running && key == g_key;
}
