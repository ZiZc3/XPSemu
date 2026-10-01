//
// XPSemu: a log for each game played
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

// What's known about the game, for the log's header.
struct GameLogInfo {
    std::string name, path, key, xbe_title;
    uint32_t title_id;
    bool full_disc;
    std::string overrides; // Its own settings, "resolution=3 ..." or ""
    std::string patches;   // Patches applied (game-patches.cc), or ""
};

// A game starts: /data/xemu/xemu-game.log begins again (the last one is
// kept as xemu-game.log.old) with a header, then follows the game: a stats
// line every few seconds and everything the emulator prints (from
// xemu.log) until it stops.
void GameLogStart(const GameLogInfo &info);
void GameLogStop(const char *why);
// A line in the game's log, marked with the time into the game (and in
// xemu.log): what XPSemu noticed, e.g. a patch applied or a crash.
void GameLogNote(const std::string &text);
// Every frame.
void GameLogTick();

// Copies what the emulator printed since the last copy into the game's log.
// Only system calls: the crash handler calls it too.
extern "C" void xemu_ps5_game_log_copy(void);
