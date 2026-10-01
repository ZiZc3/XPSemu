//
// XPSemu: the dashboard (the PS5's front end for xemu)
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
#include <string>
#include <vector>
#include "scene.hh"
#include "game-patches.hh"
#include "game-profile.hh"

class DashboardScene : public Scene {
public:
    struct Game {
        std::string name; // The title in default.xbe, else the file's name
        std::string path;
        uint32_t title_id;
        bool full_disc; // A redump image, which needs converting to XISO
    };

    void Show() override;
    void Hide() override;
    bool IsAnimating() override;
    bool Draw() override;

    // True once shown by itself after the boot animation (see main.cc).
    bool m_auto_shown = false;

protected:
    // The main menu: three pages, then the Xbox's own dashboard.
    enum Page { PAGE_GAMES, PAGE_SETTINGS, PAGE_SYSTEM, PAGE__COUNT };
    static const int MENU_XBOX_DASHBOARD = PAGE__COUNT;
    static const int MENU__COUNT = PAGE__COUNT + 1;

    bool m_visible = false;
    float m_alpha = 0;      // Fades in on Show, out on Hide
    int m_page = PAGE_GAMES;
    bool m_in_page = false; // A page is open, or the main menu shows
    float m_page_anim = 0;  // 0 main menu .. 1 page, eased
    float m_menu_anim = 0;  // Highlight position in the main menu

    std::vector<Game> m_games;

    // Recently played, newest first (disc image paths, kept in
    // recent.txt), shown as a shelf of covers under the main menu.
    std::vector<std::string> m_recent;
    bool m_recent_loaded = false;
    bool m_shelf = false;     // The shelf has the focus, not the menu

    // A game's own settings (Triangle on a game): a page over the others.
    bool m_gs_open = false;
    bool m_gs_from_shelf = false;
    Game m_gs_game;
    std::string m_gs_key;
    GameProfile m_gs_profile;
    int m_gs_row = 0;
    std::string m_header = "Games"; // The page header's title (see there)
    // Its rows: the game options, then these.
    static const int GS_PATCHES = GO__COUNT;
    static const int GS_RESET = GO__COUNT + 1;
    static const int GS__COUNT = GO__COUNT + 2;
    // Its patches (every group of every .JMP file for it), and the list.
    struct PatchItem {
        const PatchFile *file;
        int group;
        std::string problem; // Why it can't apply to this game, or ""
        bool found = false;  // Fits only because its code was found
    };
    std::vector<PatchItem> m_gs_patch_items;
    PatchChoices m_gs_choices;
    bool m_gs_patches = false;
    int m_patch_row = 0;
    uint32_t m_gs_crc = 0; // This copy's default.xbe checksum
    bool m_gs_crc_ok = false;
    int m_shelf_sel = 0;
    float m_shelf_anim = 0;   // Focus position along the shelf
    float m_shelf_focus = 0;  // 0 menu .. 1 shelf, eased
    int m_game = 0;
    float m_game_anim = 0;  // List scroll position, following m_game

    int m_setting = 0;
    bool m_advanced = false; // Settings shows its Advanced list
    int m_adv_setting = 0;
    bool m_restart_needed = false; // An advanced setting applies next start
    int m_system_row = 0;

    // The emulated Xbox is paused while the dashboard shows.
    bool m_paused_vm = false;
    // A game or the Xbox dashboard was started from here, so Back can
    // return to it (at first there's only this dashboard).
    bool m_can_return = false;
    bool m_game_started = false; // Since the Xbox dashboard last started

    uint32_t m_prev_buttons = 0;
    float m_time = 0;

    void ScanGames();
    void HandleInput();
    void HandleInputInner();
    void ChangeSetting(int step);
    void ChangeAdvanced(int step);
    void Launch(const Game &game);
    void LoadRecent();
    void OpenGameSettings(const Game &game, bool from_shelf);
    void ChangeGameSetting(int step);
    void AddRecent(const std::string &path);
    std::vector<const Game *> RecentGames();
    void OpenXboxDashboard();
    void WatchLaunch(const Game &game, const std::vector<std::string> &applied,
                     const std::string &log);
    void WatchStop();

public:
    // Every frame (DashboardTick): the watch on a game just launched.
    void WatchTick();

protected:
    void Close();

    void DrawBackground(float s);
    void DrawOrb(float cx, float cy, float r);
    void DrawMainMenu(float s, float a);
    void DrawShelf(float s, float a, float x0, float y0);
    void DrawTitle(float s, float a, ImVec2 pos);
    void DrawGameSettings(float s, float a);
    void DrawGamePatches(float s, float a, float x, float w);
    void DrawCoverFloat(const Game &game, ImVec2 c, float h, float a);
    void DrawPageHeader(float s, float a);
    void DrawGames(float s, float a);
    void DrawSettings(float s, float a);
    void DrawAdvanced(float s, float a);
    void DrawGameArt(const Game &game, ImVec2 p0, ImVec2 p1, float a,
                     bool cover);
    void DrawSystem(float s, float a);
    void DrawHints(float s);
};

extern DashboardScene g_dashboard;

// The performance overlay over the game (Settings > Performance overlay),
// drawn every frame by the UI.
void DrawPerfOverlay();

// Every frame, dashboard shown or not: counts play time.
void DashboardTick();
