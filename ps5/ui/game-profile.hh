//
// XPSemu: per-game settings and play time
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
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// The settings a game can have its own value for. All take effect while
// the game runs, without restarting XPSemu.
enum GameOption {
    GO_SCALE,   // Resolution, 1 to 4
    GO_ASPECT,  // CONFIG_DISPLAY_UI_ASPECT_RATIO_*
    GO_FIT,     // CONFIG_DISPLAY_UI_FIT_*
    GO_FILTER,  // CONFIG_DISPLAY_FILTERING_*
    GO_DSP,     // Audio DSP, 0 or 1
    GO_VOLUME,  // In tens of percent, 0 to 10
    GO_OVERLAY, // CONFIG_DISPLAY_UI_PERF_OVERLAY_*
    GO_AA,      // 0 off, 1 FXAA, 2 SMAA, 3 SSAA
    GO_FPS_CAP, // 0 off, GO_FPS_CAP_CUSTOM while it's being chosen, else frames a second
    GO_AF,      // Anisotropic filtering: 0 off, else 2, 4, 8, 16 (from the next start)
    GO_CAS,     // patch 247: CAS's sharpness, 0 to 4 (0.0 to 1.0)
    GO_TEXSHARP,// patch 247: texture sharpness, 0 off, 1 low, 2 medium, 3 high
    GO_CRT,     // patch 247: the CRT filter, 0 off, 1 scanlines, 2 CRT
    GO_TRIGGERS,// patch 266: adaptive triggers' kind, TRIGGERS_*; Default = Auto (the built-in list)
    GO_GYRO,    // patch 266: gyro aim, 1 low, 2 medium, 3 high; Default = Off
    GO_TURBO,   // patch 317: turbo buttons, a mask of TURBO_*; Default = Off
    GO_MSAA,    // patch 325: MSAA on the game's render targets, 2, 4 or 8 samples; Default = Off (from the next start)
    GO_A2C,     // patch 354: MSAA smooth cut-outs (alpha-to-coverage), 0 off; Default = On (with MSAA only)
    GO_MOVIEUP, // patch 355: the movie upscaler, 0 off; Default = On
    GO_SSAO,    // patch 363: ambient occlusion, 1 low, 2 high; Default = Off
    GO__COUNT
};

static const int GAME_DEFAULT = -1; // Use the user's own setting
// patch 317: turbo buttons (held = pressed again and again), the Xbox buttons in "turbo=" as a mask
enum { TURBO_A = 1, TURBO_B = 2, TURBO_X = 4, TURBO_Y = 8, TURBO_RT = 16, TURBO_LT = 32 };
static const int GO_FPS_CAP_CUSTOM = 1;

// patch 266: the DualSense's adaptive triggers, by kind of game (main_ps5.c's "triggers=" start line)
enum { TRIGGERS_OFF = 0, TRIGGERS_DRIVING = 1, TRIGGERS_SHOOTER = 2 };
// The built-in kind of an Xbox game (its key, else its name) or a Chihiro game (its 4-letter game ID).
int TriggersBuiltIn(const std::string &key, const std::string &name);
int TriggersChihiro(const std::string &game_id);
// What a game's start uses: its own choice, else the built-in kind.
int TriggersFor(const std::string &key, const std::string &name, int choice);

struct GameProfile {
    int value[GO__COUNT];
    // patch 269: the game's own button mapping ("button_mapping=" in its file, padmap_ps5.h's text), "" = Default
    // (your Settings > Controller layout). Xbox games only.
    std::string padmap;
    GameProfile() { for (int &v : value) v = GAME_DEFAULT; }
};

const char *GameOptionName(int option);
const char *GameOptionHelp(int option);
const char *GameOptionValueName(int option, int value);
// The same, longer where it helps: the resolution with its lines ("2x -
// 960p (~1080p)"). For the settings rows; the short one for chips.
const char *GameOptionValueLabel(int option, int value);
// value's neighbour step places along the option's values (with
// GAME_DEFAULT first when with_default), wrapping around.
int GameOptionStep(int option, int value, int step, bool with_default);

// A game's key for its files: its title ID, else its (reduced) file name.
std::string GameKey(uint32_t title_id, const std::string &fallback);

GameProfile GameProfileLoad(const std::string &key);
// A PAL game at 60 Hz (/data/xpsemu/pal60.txt: "key" on, "-key" off; else on for every PAL game).
bool Pal60On(const std::string &key);
int FrameGenMode(const std::string &key);
void FrameGenSetMode(const std::string &key, int mode);
std::string FrameGenLine(const std::string &key);
void Pal60Toggle(const std::string &key);
// Our own 60 FPS unlock: the games it's for, on unless turned off (/data/xpsemu/unlock60.txt: "-key").
bool Unlock60Known(const std::string &key);
bool Unlock60On(const std::string &key);
void Unlock60Toggle(const std::string &key);
// The one switch: the game's frame rate mod or unlock wanted (on unless turned off).
bool Fps60Wanted(const std::string &key);
void GameProfileSave(const std::string &key, const GameProfile &profile);

// The user's own value of an option (not a running game's), and changing
// it: applied now unless the running game has its own value, and saved.
// The resolution waits for the next game to start (see WriteConfig).
int GameGlobal(int option);
// What's in effect: the running game's own value, else the user's.
int GameEffective(int option);
void GameSetGlobal(int option, int value);

// A game starts: its settings take over from the user's until it stops
// (another game, or back to the Xbox dashboard). Also counts its play time.
void GameStart(const std::string &key);
void GameStop();
bool GameIsRunning(const std::string &key);
// A game is running (then the resolution can't change: see WriteConfig).
bool GameAnyRunning();
// The running game's settings were changed: apply them.
void GameProfileReapply();

// Play time, counted while the game runs (not paused, not in the
// dashboard). Call every frame.
void GamePlayTick(float dt, bool playing);
struct GamePlay {
    double seconds = 0;
    int64_t last = 0; // When it last started, seconds since 1970
};
GamePlay GamePlayGet(const std::string &key);
// A game's run, added when the dashboard comes back: games run without it.
void GamePlayAdd(const std::string &key, double seconds, int64_t last);
// "2h 14m", "12m", "Under a minute", "Never" and "Today", "Yesterday",
// "3 days ago", "Sep 2".
std::string GamePlayTimeText(double seconds);
std::string GamePlayLastText(int64_t last);
