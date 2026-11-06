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
#include <math.h>
#include <sstream>
#include <time.h>

#include "common.hh"
#include "game-profile.hh"
#include "xog_ui.h"

static const char *const kOptionKeys[GO__COUNT] = {
    "resolution", "screen_shape", "picture_fit", "smoothing",
    "dsp",        "volume",       "overlay",      "anti_aliasing", "fps_cap",
};

const char *GameOptionName(int option)
{
    static const char *const names[GO__COUNT] = {
        "Resolution",     "Screen shape",          "Picture fit",
        "Upscaling",      "Audio processor (DSP)", "Volume",
        "FPS counter",    "Anti-aliasing", "FPS cap",
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
        "How a picture smaller than the screen is enlarged. Sharp uses SGSR; Smooth is plain bilinear.",
        "Keep it Off: very slow on the PS5 and games stall. Only if this "
        "game has no sound without it.",
        "How loud this game is.",
        "In the corner while this game runs: the frame rate, or Full for frame time, CPU, memory and resolution too.",
        "Smooths jagged edges. FXAA is light, SMAA is sharper, SSAA draws at x6 and averages it down (the heaviest).",
        "Holds the game at this rate. Custom lets you type any number. A game whose speed is tied to its frames slows down with it.",
    };
    return help[option];
}

// Each option's values, in the order they're offered.
static std::vector<int> Choices(int option)
{
    switch (option) {
    case GO_SCALE: return { 1, 2, 3, 4, 5, 6 };
    case GO_ASPECT:
        return { CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO,
                 CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3,
                 CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9 };
    case GO_FIT:
        return { CONFIG_DISPLAY_UI_FIT_SCALE, CONFIG_DISPLAY_UI_FIT_STRETCH };
    case GO_FILTER:
        return { CONFIG_UPSCALE_SHARP, CONFIG_UPSCALE_SMOOTH };
    case GO_DSP: return { 1, 0 };
    case GO_VOLUME: return { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    case GO_OVERLAY:
        return { CONFIG_FPS_ON, CONFIG_FPS_FULL, CONFIG_FPS_OFF };
    case GO_AA: return { 0, 1, 2, 3 };
    case GO_FPS_CAP: return { 0, 30, 60, 90, GO_FPS_CAP_CUSTOM };
    }
    return {};
}

const char *GameOptionValueName(int option, int value)
{
    static char buf[16];
    if (value == GAME_DEFAULT) {
        return "Default";
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
        return value == CONFIG_UPSCALE_SHARP ? "Sharp" : "Smooth";
    case GO_DSP: return value ? "On" : "Off";
    case GO_VOLUME:
        snprintf(buf, sizeof(buf), "%d%%", value * 10);
        return buf;
    case GO_OVERLAY:
        return value == CONFIG_FPS_FULL ? "Full" : value == CONFIG_FPS_ON ? "FPS" : "Off";
    case GO_AA:
        return value == 1 ? "FXAA" : value == 2 ? "SMAA" : value == 3 ? "SSAA" : "Off";
    case GO_FPS_CAP:
        if (value == 0) {
            return "Off";
        }
        if (value == GO_FPS_CAP_CUSTOM) {
            return "Custom";
        }
        snprintf(buf, sizeof(buf), "%d FPS", value);
        return buf;
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
    values[GO_SCALE] = std::clamp(g_cfg.scale, 1, 6);
    values[GO_ASPECT] = g_cfg.aspect;
    values[GO_FIT] = g_cfg.fit;
    values[GO_FILTER] = g_cfg.upscale;
    values[GO_DSP] = 0;
    values[GO_VOLUME] = g_cfg.volume;
    values[GO_OVERLAY] = g_cfg.fps;
    values[GO_AA] = g_cfg.aa;
    values[GO_FPS_CAP] = g_cfg.fps_cap;
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
    if (g_pal60_off.count(key)) {
        return false;
    }
    return g_pal60_on.count(key) || key == "4D5300D1" ||    // Fable: 25 fps at 50 Hz, 30 at 60
           key == "54540086" || key == "545400A4" || key == "54540095" || key == "545400A5" ||  // GTA San Andreas, the same
           key == "5553001D";       // Prince of Persia: The Sands of Time, 30 fps at 60 Hz
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

static std::set<std::string> g_unlock_off;
static bool g_unlock_loaded;
static const char *const kUnlockFile = "/data/xpsemu/unlock60.txt";

bool Unlock60Known(const std::string &key)
{
    return key == "41560020" || key == "45410091" ||    // Doom 3, Half-Life 2
           key == "56550039";                           // Hulk: Ultimate Destruction (patch 140)
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
            }
        }
    }
    return !g_unlock_off.count(key);
}

bool Unlock60On(const std::string &key)
{
    return Unlock60Known(key) && Fps60Wanted(key);
}

void Unlock60Toggle(const std::string &key)
{
    if (Fps60Wanted(key)) {
        g_unlock_off.insert(key);
    } else {
        g_unlock_off.erase(key);
    }
    std::ofstream f(kUnlockFile);
    for (const std::string &k : g_unlock_off) {
        f << "-" << k << "\n";
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
