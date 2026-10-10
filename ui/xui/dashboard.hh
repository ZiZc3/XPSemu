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
#include "../xpsemu-dev.h"
#include <map>
#include <functional>
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
    bool m_startup_show = false;

    bool Visible() const { return m_visible; }
    void ParkTick(); // Each frame (DashboardTick)
    // The game running behind the dashboard, if any.
    const Game *NowPlayingGame();

protected:
    // The main menu: three pages, then the Xbox's own dashboard.
    enum Page { PAGE_GAMES, PAGE_SETTINGS, PAGE_SYSTEM, PAGE__COUNT };
    static const int MENU_XBOX_DASHBOARD = PAGE__COUNT;
    static const int MENU__COUNT = PAGE__COUNT + 1;

    bool m_visible = false;
    float m_alpha = 0;      // Fades in on Show, out on Hide
    bool m_startup_fade = false;
    float m_startup_fade_elapsed = 0;
    float m_startup_black = 0;     // The screen going black before the fade
    int m_startup_smooth = 0;      // Smooth frames in a row since Show
    bool m_startup_settled = false; // The fade has started
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
public: // (The rows' order is kGameRows, dashboard.cc)
    static const int GS_PATCHES = GO__COUNT;
    static const int GS_SHORTCUT = GO__COUNT + 1; // Home screen shortcut
    static const int GS_RESET = GO__COUNT + 2;
    static const int GS_NAME = GO__COUNT + 3;     // The game's own name
    static const int GS__COUNT = GO__COUNT + 4;
protected:
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
    float m_game_vel = 0;   // Its speed, springing towards m_game
    int m_glint_game = -1;  // Selection the glint is for
    float m_glint_start = 0; // When its glint starts (m_time)
    ImVec2 m_sel0, m_sel1;   // The selected case's box on screen
    float m_music_at = -1;   // When the music starts (after startup)
    int m_covers_new = 0;    // Covers downloaded, not yet told
    int m_settings_cat = -1; // Settings: the category open, or -1
    int m_cat_sel = 0;       // Settings: the category selected
    bool m_boot = false;     // The startup animation is playing
    // The orb: leaning towards the focus and the stick, spinning when
    // things move, rippling at each move, asleep when nothing happens.
    float m_orb_lean_x = 0, m_orb_lean_y = 0;
    float m_orb_phase[2] = { 0, 1.2f }; // Its rings' turn
    float m_orb_spin = 0;               // Extra spin (dies away)
    float m_orb_sleep = 0;              // 0 awake .. 1 asleep
    float m_orb_ripple[4] = { -9, -9, -9, -9 }; // When they started
    float m_orb_wake = -9;              // When it last woke
    void OrbPoke(float strength);
    // The Box Art Viewer (Square on a game).
    bool m_view = false, m_view_closing = false;
    float m_view_t = 0;                       // When it opened/closed
    float m_view_yaw = 0, m_view_pitch = 0;    // Its turn
    float m_view_vyaw = 0, m_view_vpitch = 0;  // Spinning
    float m_view_zoom = 1, m_view_zoom_to = 1;
    float m_view_flip_to = 0;                 // Turning to (yaw)
    float m_view_e = 0;            // How far out of the row (0..1)
    float m_view_shown_yaw = 0, m_view_shown_pitch = 0; // As drawn
    float m_view_close_yaw = 0, m_view_close_pitch = 0; // Closing from
    float m_view_land_yaw = 0;     // Closing to: straight
    bool m_view_flipping = false;
    ImVec2 m_view_from0, m_view_from1;        // The cover in the row
    float m_sys_ask = -1; // System: until when "again to confirm" holds
    void OpenViewer();
    void ViewerInput(bool accept, bool back, bool options, bool left,
                     bool right);
    void DrawViewer(float s, float a);
    bool ViewerLoading();
    void QuitApp(bool restart);
#if XPSEMU_DEV
    void DevStressLaunchNext();
    void DrawDev(float s, float a, float x, float y, float ay2);
#endif
    // Controller setup (Settings > Controller).
    bool m_pad_page = false, m_pad_capture = false, m_pad_released = false;
    int m_pad_sel = 0;
    float m_pad_capture_t = 0, m_pad_done_t = -9;
    void PadInput(bool accept, bool left, bool right, bool up, bool down,
                  bool options);
    void PadCapture();
    void DrawPadPage(float s, float a);
    // Home screen shortcuts: this game's (its folder, or ""), and the
    // disc to start at once when XPSemu runs as one.
    std::string m_gs_shortcut, m_autolaunch;
    void ToggleShortcut(const Game &game);
    // Renaming a game: the on-screen keyboard.
    bool m_kb_open = false, m_kb_shift = false;
    std::string m_kb_text;
    int m_kb_row = 0, m_kb_col = 0;
    void OpenNameEditor();
    void KeyboardInput(bool accept, bool back, bool square, bool options,
                       bool up, bool down, bool left, bool right);
    void DrawKeyboard(float s, float a);
    // The Patch Store (a game's patches) and Settings > Patches.
    struct StoreItem {
        int index;       // In kPatchCatalog
        int state;       // StoreState
        std::string why;
    };
    std::vector<StoreItem> m_store;
    struct AllPatchRow {
        std::string path, game_name, key;
        const PatchFile *file;
        int group;
    };
    std::vector<AllPatchRow> m_all_patches;
    int m_all_row = 0;
    bool m_all_page = false;
    std::map<std::string, bool> m_patch_active; // The green dot, by path
    int m_badge_next = -1;
    void BuildPatchItems();
    void StoreOpen();
    void StorePoll();
    void StoreGet(int row);
    void StoreRemove(const PatchFile &file);
    void RefreshPatchBadges();
    void PatchBadgeStep();
    void OpenAllPatches();
    void AllPatchesInput(bool accept, bool left, bool right, bool up,
                         bool down);
    void DrawAllPatches(float s, float a);
    float m_boot_t = 0;
    void DrawBoot(float s);
    bool m_covers_told_offline = false;
    void RequestMissingCovers();
    float m_eject_ask = -1;  // Until when "press again to eject" holds

    int m_setting = 0;
    bool m_advanced = false; // Settings shows its Advanced list
    int m_adv_setting = 0;
    bool m_restart_needed = false; // An advanced setting applies next start
    bool m_np_focus = false;   // Now Playing has the focus (Up from Games)
    float m_back_hold = 0;     // Circle held: quick resume at 0.6 s
    bool m_setup = false;      // First run: Xbox files missing
    // Attract mode: after two idle minutes, the games' covers, slowly.
    float m_idle = 0;
    bool m_attract = false;
    float m_attract_alpha = 0;
    float m_attract_t = 0;
    std::vector<int> m_attract_games;
    bool m_setup_ok = false;   // ...and now all there (restart to use them)
    float m_setup_next = 0;    // When to look for the files again
    std::string m_games_sig;   // The games folders as last scanned
    float m_games_next = 0;    // When to look at them again
    int m_system_row = 0;

    // The emulated Xbox is paused while the dashboard shows.
    bool m_paused_vm = false;
    // A game or the Xbox dashboard was started from here, so Back can
    // return to it (at first there's only this dashboard).
    bool m_can_return = false;
    bool m_game_started = false; // Since the Xbox dashboard last started
    std::string m_now_playing_path;

    uint32_t m_prev_buttons = 0;
    float m_time = 0;

    void ScanGames();
    void HandleInput();
    void HandleInputInner();
    void ChangeSetting(int step);
    void ChangeAdvanced(int step);
    void Launch(const Game &game, bool quiet = false);
    void LoadRecent();
    void OpenGameSettings(const Game &game, bool from_shelf);
    void ChangeGameSetting(int step);
    void AddRecent(const std::string &path);
    std::vector<const Game *> RecentGames();
    void OpenXboxDashboard();
    void StopGame(const char *why);
    void StopGameNow(const char *why);
    void LaunchNow(const Game &game, bool quiet);
    void QuitAppNow(bool restart);
    void ParkThen(std::function<void()> next);
    // Before a game is stopped, the Xbox runs on (muted, behind this
    // dashboard) until its hard disk is quiet, so a save just made gets
    // written instead of lost in the reset.
    struct {
        bool active = false;
        double start = 0, last_write = 0;
        uint64_t writes = 0;
        float gain = 1;
        std::function<void()> next; // The last one asked for
        bool done = false;   // The Xbox has finished writing
        double done_at = 0;  // ...then the burn ends, then next() runs
        float p_done = 0;    // How far it had burnt by then
        double ash_until = 0; // The last embers, after the bar is gone
        float gx = 0, gw = 0, gbottom = 0, gs = 1, ga = 1; // Where it was
    } m_park;
    float BurnAt(double when) const;
    void DrawAsh(double now);
    void WatchLaunch(const Game &game, const std::vector<std::string> &applied,
                     const std::string &log);
    void WatchStop();

public:
    // Every frame (DashboardTick): the watch on a game just launched.
    void WatchTick();
#if XPSEMU_DEV
    // Dev builds: the stress test (every frame, from DashboardTick)
    void DevStressStep(float dt);
#endif

protected:
    void Close();

    void DrawBackground(float s);
    void DrawOrb(float cx, float cy, float r);
    void DrawMainMenu(float s, float a);
    void DrawNowPlayingBar(float s, float a, float x, float w,
                           float bottom);
    void DrawNowPlaying(float s, float a, float x, float w,
                        float bottom);
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
    void DrawStatus(float s);
    void DrawSetup(float s, float a);
    void DrawAttract(float s, float a);
    bool UpdateAttract(float dt); // True while it has the screen
};

// True when the Xbox files XPSemu needs aren't all there (first run).
bool DashboardSetupNeeded();

extern DashboardScene g_dashboard;

// The performance overlay over the game (Settings > Performance overlay),
// drawn every frame by the UI.
void DrawPerfOverlay();
// Over the Xbox's boot after a game is launched, until the game runs.
void DrawLaunchCurtain();
// Side art beside a 4:3 game whose picture is g0..g1 (vk-helpers.cc).
void DrawBladeSides(ImDrawList *dl, ImVec2 g0, ImVec2 g1);

// Every frame, dashboard shown or not: counts play time.
void DashboardTick();
