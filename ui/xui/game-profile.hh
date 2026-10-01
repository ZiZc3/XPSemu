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
    GO__COUNT
};

static const int GAME_DEFAULT = -1; // Use the user's own setting

struct GameProfile {
    int value[GO__COUNT];
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
// "2h 14m", "12m", "Under a minute", "Never" and "Today", "Yesterday",
// "3 days ago", "Sep 2".
std::string GamePlayTimeText(double seconds);
std::string GamePlayLastText(int64_t last);
