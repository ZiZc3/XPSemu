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
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <math.h>
#include <sstream>
#include <time.h>

#include "common.hh"
#include "game-profile.hh"
#include "../xemu-settings.h"

static const char *const kOptionKeys[GO__COUNT] = {
    "resolution", "screen_shape", "picture_fit", "smoothing",
    "dsp",        "volume",       "overlay",
};

const char *GameOptionName(int option)
{
    static const char *const names[GO__COUNT] = {
        "Resolution",     "Screen shape",          "Picture fit",
        "Smoothing",      "Audio processor (DSP)", "Volume",
        "Performance overlay",
    };
    return names[option];
}

const char *GameOptionHelp(int option)
{
    static const char *const help[GO__COUNT] = {
        "Higher is sharper, and uses more of the GPU. Lower it if this game "
        "runs slow. Lines are for 480p games; 720p ones go higher.",
        "16:9 for this game if it runs in widescreen (the Xbox's "
        "Widescreen setting on): the Xbox never tells the TV.",
        "Stretch fills the whole screen.",
        "Off keeps the pixels sharp.",
        "Keep it Off: very slow on the PS5 and games stall. Only if this "
        "game has no sound without it.",
        "How loud this game is.",
        "Frame rate, CPU and memory in the corner while this game runs.",
    };
    return help[option];
}

// Each option's values, in the order they're offered.
static std::vector<int> Choices(int option)
{
    switch (option) {
    case GO_SCALE: return { 1, 2, 3, 4 };
    case GO_ASPECT:
        return { CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO,
                 CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3,
                 CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9 };
    case GO_FIT:
        return { CONFIG_DISPLAY_UI_FIT_SCALE, CONFIG_DISPLAY_UI_FIT_STRETCH };
    case GO_FILTER:
        return { CONFIG_DISPLAY_FILTERING_LINEAR,
                 CONFIG_DISPLAY_FILTERING_NEAREST };
    case GO_DSP: return { 1, 0 };
    case GO_VOLUME: return { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    case GO_OVERLAY:
        return { CONFIG_DISPLAY_UI_PERF_OVERLAY_OFF,
                 CONFIG_DISPLAY_UI_PERF_OVERLAY_FPS,
                 CONFIG_DISPLAY_UI_PERF_OVERLAY_FULL };
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
        snprintf(buf, sizeof(buf), "%dx", value);
        return buf;
    case GO_ASPECT:
        switch (value) {
        case CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE: return "Native";
        case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3: return "4:3";
        case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9: return "16:9";
        default: return "Auto";
        }
    case GO_FIT:
        return value == CONFIG_DISPLAY_UI_FIT_STRETCH ? "Stretch" :
               value == CONFIG_DISPLAY_UI_FIT_CENTER  ? "Center" :
                                                        "Fit";
    case GO_FILTER:
        return value == CONFIG_DISPLAY_FILTERING_LINEAR ? "On" : "Off";
    case GO_DSP: return value ? "On" : "Off";
    case GO_VOLUME:
        snprintf(buf, sizeof(buf), "%d%%", value * 10);
        return buf;
    case GO_OVERLAY:
        return value == CONFIG_DISPLAY_UI_PERF_OVERLAY_FULL ? "Full" :
               value == CONFIG_DISPLAY_UI_PERF_OVERLAY_FPS  ? "FPS" :
                                                              "Off";
    }
    return "";
}

const char *GameOptionValueLabel(int option, int value)
{
    if (option != GO_SCALE || value == GAME_DEFAULT) {
        return GameOptionValueName(option, value);
    }
    // Most Xbox games draw 480 lines; the scale multiplies them.
    switch (value) {
    case 1: return "1x - 480p (original)";
    case 2: return "2x - 960p (~1080p)";
    case 3: return "3x - 1440p (2K)";
    case 4: return "4x - 1920p (~4K)";
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

static void ReadConfig(int values[GO__COUNT])
{
    values[GO_SCALE] = std::clamp(g_config.display.quality.surface_scale, 1, 4);
    values[GO_ASPECT] = g_config.display.ui.aspect_ratio;
    values[GO_FIT] = g_config.display.ui.fit;
    values[GO_FILTER] = g_config.display.filtering;
    values[GO_DSP] = g_config.audio.use_dsp;
    values[GO_VOLUME] = (int)lroundf(g_config.audio.volume_limit * 10);
    values[GO_OVERLAY] = g_config.display.ui.perf_overlay;
}

// live: also change the renderer's resolution now (not while saving).
// Only as a game starts or stops, right before the Xbox restarts: changed
// under a running game, the surfaces it still uses are rebuilt, and the
// PS5's GPU faulted on the freed memory (GPU_FAULT_PAGE_FAULT_ASYNC).
static bool g_scale_now;

static void WriteConfig(const int values[GO__COUNT], bool live)
{
#ifdef __PROSPERO__
    // XPSemu: the resolution doesn't change while XPSemu runs. Switching it
    // between games (one game's surfaces rebuilt at the next one's size)
    // made the PS5's GPU fault (GPU_FAULT_PAGE_FAULT) every time a game
    // followed another, and on the PS5 it costs no speed anyway: every game
    // uses Settings > Video > Resolution, from XPSemu's next start (a game's
    // own resolution is left out: Apply).
    (void)live;
    g_config.display.quality.surface_scale = values[GO_SCALE];
#else
    if (live && g_scale_now &&
        (int)nv2a_get_surface_scale_factor() != values[GO_SCALE]) {
        nv2a_set_surface_scale_factor(values[GO_SCALE]);
    }
    g_config.display.quality.surface_scale = values[GO_SCALE];
#endif
    g_config.display.ui.aspect_ratio = values[GO_ASPECT];
    g_config.display.ui.fit = values[GO_FIT];
    g_config.display.filtering = values[GO_FILTER];
    g_config.audio.use_dsp = values[GO_DSP];
    g_config.audio.volume_limit = values[GO_VOLUME] / 10.0f;
    g_config.display.ui.perf_overlay = values[GO_OVERLAY];
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

static void SaveHook(bool before)
{
    static int game_values[GO__COUNT];
    if (!g_running) {
        return;
    }
    if (before) {
        ReadConfig(game_values);
        WriteConfig(g_globals, false);
    } else {
        WriteConfig(game_values, false);
    }
}

static struct HookInstaller {
    HookInstaller() { xemu_settings_save_hook = SaveHook; }
} g_hook_installer;

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
